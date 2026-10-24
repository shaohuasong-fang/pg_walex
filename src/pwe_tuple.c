/*
 * pg_walex - heap tuple decoding and SQL value rendering
 */
#include "pwe_internal.h"

#include <math.h>
#include <inttypes.h>

#define PWE_SIZE_OF_HEAP_HEADER 5	/* xl_heap_header in WAL payload */
#define PWE_SIZEOF_TUPLE_HEADER 23	/* SizeofHeapTupleHeader == offsetof(t_bits) */

/* plain SQL keeps the historical ARRAY[...] rendering (see format_array()) */
static bool sql_legacy_arrays = false;

void
pwe_value_set_legacy_arrays(bool on)
{
	sql_legacy_arrays = on;
}

uint16
pwe_read_u16(const uint8 *p)
{
	return (uint16) p[0] | ((uint16) p[1] << 8);
}

uint32
pwe_read_u32(const uint8 *p)
{
	return (uint32) p[0] | ((uint32) p[1] << 8) |
		((uint32) p[2] << 16) | ((uint32) p[3] << 24);
}

uint64
pwe_read_u64(const uint8 *p)
{
	return (uint64) pwe_read_u32(p) | ((uint64) pwe_read_u32(p + 4) << 32);
}

int16
pwe_read_i16(const uint8 *p)
{
	return (int16) pwe_read_u16(p);
}

int32
pwe_read_i32(const uint8 *p)
{
	return (int32) pwe_read_u32(p);
}

int64
pwe_read_i64(const uint8 *p)
{
	return (int64) pwe_read_u64(p);
}

/* ------------------------------------------------------------------ */
/* tuple header parsing                                               */
/* ------------------------------------------------------------------ */
bool
pwe_tuple_parse(const uint8 *payload, uint32 payload_len, PweTuple *t,
				char *err, size_t errlen)
{
	uint16		infomask2;
	uint16		infomask;
	uint8		hoff;
	uint32		bitmap_len;
	uint32		data_off;

	memset(t, 0, sizeof(*t));
	if (payload_len < PWE_SIZE_OF_HEAP_HEADER)
	{
		snprintf(err, errlen, "tuple data too short (%u bytes)", payload_len);
		return false;
	}
	infomask2 = pwe_read_u16(payload);
	infomask = pwe_read_u16(payload + 2);
	hoff = payload[4];
	if (hoff < PWE_SIZEOF_TUPLE_HEADER)
	{
		snprintf(err, errlen, "invalid tuple t_hoff %u", hoff);
		return false;
	}
	if ((uint32) (hoff - PWE_SIZEOF_TUPLE_HEADER) > payload_len - PWE_SIZE_OF_HEAP_HEADER)
	{
		snprintf(err, errlen, "tuple t_hoff beyond record data");
		return false;
	}
	t->infomask2 = infomask2;
	t->infomask = infomask;
	t->natts = infomask2 & 0x07FF;
	t->has_null = (infomask & PWE_HEAP_HASNULL) != 0;
	t->has_oid = (infomask & PWE_HEAP_HASOID) != 0;
	data_off = (uint32) hoff - PWE_SIZEOF_TUPLE_HEADER;
	t->data = payload + PWE_SIZE_OF_HEAP_HEADER + data_off;
	if (data_off > payload_len - PWE_SIZE_OF_HEAP_HEADER)
	{
		snprintf(err, errlen, "tuple data offset out of range");
		return false;
	}
	t->datalen = payload_len - PWE_SIZE_OF_HEAP_HEADER - data_off;
	bitmap_len = ((uint32) t->natts + 7) / 8;
	if (t->has_null)
	{
		if (data_off < bitmap_len)
		{
			snprintf(err, errlen, "null bitmap out of range");
			return false;
		}
		/* t_bits starts right after the fixed 23-byte header */
		t->nullbits = payload + PWE_SIZE_OF_HEAP_HEADER;
	}
	if (t->has_oid)
	{
		if (data_off < 4)
		{
			snprintf(err, errlen, "tuple oid out of range");
			return false;
		}
		/* OID is stored at t_hoff - sizeof(Oid) */
		t->oid = pwe_read_u32(payload + PWE_SIZE_OF_HEAP_HEADER +
							  data_off - 4);
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* varlena                                                            */
/* ------------------------------------------------------------------ */
static bool
pwe_decompress_inline(PweValue *v, uint32 rawsize, uint8 method,
					  const uint8 *data, uint32 len)
{
	uint8	   *out;

	if (rawsize == 0 || rawsize > (uint32) (1024 * 1024 * 1024))
		return false;
	out = pwe_malloc(rawsize);
	if (method == 0)
	{
		if (!pwe_pglz_decompress(data, len, out, rawsize))
		{
			free(out);
			return false;
		}
	}
	else if (method == 1)
	{
		if (!pwe_lz4_decompress(data, len, out, rawsize))
		{
			free(out);
			return false;
		}
	}
	else if (method == 2)
	{
		if (!pwe_zstd_decompress(data, len, out, rawsize))
		{
			free(out);
			return false;
		}
	}
	else
	{
		free(out);
		return false;
	}
	free(v->owned);
	v->owned = out;
	v->data = out;
	v->len = (int) rawsize;
	return true;
}

/*
 * Parse a varlena datum at p.  Inline-compressed values are decompressed
 * immediately; external values are reported through v->toast.
 */
static bool
pwe_parse_varlena(PweValue *v, const uint8 *p, uint32 remain)
{
	uint8		b0;

	if (remain < 1)
		return false;
	b0 = p[0];
	if ((b0 & 0x01) == 0)
	{
		uint32		hdr;
		uint32		len;
		bool		compressed;

		if (remain < 4)
			return false;
		hdr = pwe_read_u32(p);
		len = (hdr >> 2) & 0x3FFFFFFF;
		compressed = (hdr & 0x03) == 0x02;
		if (len < 4 + (compressed ? 4 : 0) || len > remain)
			return false;
		if (compressed)
		{
			uint32		tcinfo = pwe_read_u32(p + 4);
			uint32		rawsize = tcinfo & 0x3FFFFFFF;
			uint8		method = (uint8) (tcinfo >> 30);

			if (!pwe_decompress_inline(v, rawsize, method, p + 8, len - 8))
				return false;
		}
		else
		{
			v->data = p + 4;
			v->len = (int) (len - 4);
		}
		v->typlen = -1;
		return true;
	}
	if (b0 == 0x01)
	{
		/* external TOAST pointer: 0x01, tag, varatt_external (16 bytes) */
		uint8		tag;

		if (remain < 2)
			return false;
		tag = p[1];
		if (tag != 18)			/* VARTAG_ONDISK */
			return false;
		if (remain < 2 + 16)
			return false;
		v->toast.valid = true;
		v->toast.rawsize = pwe_read_i32(p + 2);
		v->toast.extinfo = pwe_read_u32(p + 6);
		v->toast.valueid = pwe_read_u32(p + 10);
		v->toast.toastrelid = pwe_read_u32(p + 14);
		v->typlen = -1;
		return true;
	}
	{
		/* 1-byte header (unaligned short varlena) */
		uint32		len = (b0 >> 1) & 0x7F;

		if (len == 0 || len > remain)
			return false;
		v->data = p + 1;
		v->len = (int) (len - 1);
		v->typlen = -1;
		return true;
	}
}

void
pwe_value_resolve_inline(PweValue *v)
{
	/* decompression is handled while parsing */
	(void) v;
}

void
pwe_value_free(PweValue *v)
{
	if (!v)
		return;
	free(v->owned);
	v->owned = NULL;
}

void
pwe_values_free(PweValue *vals, int natts)
{
	int			i;

	if (!vals)
		return;
	for (i = 0; i < natts; i++)
		pwe_value_free(&vals[i]);
	free(vals);
}

/* ------------------------------------------------------------------ */
/* attribute decoding                                                 */
/* ------------------------------------------------------------------ */
typedef struct PweTypeDesc
{
	int16		len;
	bool		byval;
	char		align;
	char		typecode;
	uint32		typid;
} PweTypeDesc;

static void
code_to_desc(char code, PweTypeDesc *d)
{
	memset(d, 0, sizeof(*d));
	d->typecode = code;
	switch (code)
	{
		case 'i':
			d->len = 4;
			d->byval = true;
			d->align = 'i';
			break;
		case 's':
			d->len = 2;
			d->byval = true;
			d->align = 's';
			break;
		case 'b':
			d->len = 1;
			d->byval = true;
			d->align = 'c';
			break;
		case 'c':
			d->len = 1;
			d->byval = true;
			d->align = 'c';
			break;
		case 'n':
			d->len = 64;
			d->byval = false;
			d->align = 'c';
			break;
		case 'f':
			d->len = 4;
			d->byval = true;
			d->align = 'i';
			break;
		case 'd':
			d->len = 8;
			d->byval = true;
			d->align = 'd';
			break;
		case 'q':
			d->len = 8;
			d->byval = true;
			d->align = 'd';
			break;
		case 'a':
			d->len = 12;
			d->byval = false;
			d->align = 'i';
			break;
		case 't':
			d->len = 6;
			d->byval = false;
			d->align = 's';
			break;
		case 'v':
		default:
			d->len = -1;
			d->byval = false;
			d->align = 'i';
			break;
	}
}

static uint32
align_offset(uint32 off, char align)
{
	switch (align)
	{
		case 'd':
			return (off + 7u) & ~7u;
		case 'i':
			return (off + 3u) & ~3u;
		case 's':
			return (off + 1u) & ~1u;
		default:
			return off;
	}
}

static uint32
varlena_total_len(const uint8 *p, uint32 remain)
{
	if (remain < 1)
		return 0;
	if ((p[0] & 0x01) == 0)
	{
		uint32		len;

		if (remain < 4)
			return 0;
		len = (pwe_read_u32(p) >> 2) & 0x3FFFFFFF;
		return (len >= 4 && len <= remain) ? len : 0;
	}
	if (p[0] == 0x01)
		return remain >= 18 ? 18 : 0;
	{
		uint32		len = (p[0] >> 1) & 0x7F;

		return (len > 0 && len <= remain) ? len : 0;
	}
}

static bool
decode_one(PweValue *v, const uint8 *base, uint32 datalen, uint32 *off,
		   const PweTypeDesc *d, bool dropped, bool is_null)
{
	uint32		o;

	memset(v, 0, sizeof(*v));
	v->align = d->align;
	v->byval = d->byval;
	v->typlen = d->len;
	v->typecode = d->typecode;
	v->typid = d->typid;
	if (dropped || is_null)
	{
		v->isnull = true;
		return true;
	}
	o = *off;
	/*
	 * Varlena values with a 1-byte header ("short" values) and external
	 * TOAST pointers are stored without alignment; everything else follows
	 * the type's alignas.
	 */
	if (d->len > 0)
		o = align_offset(o, d->align);
	else if (o >= datalen || (base[o] & 0x01) == 0)
		o = align_offset(o, d->align);
	if (o > datalen)
		return false;
	if (d->len > 0)
	{
		if (o + (uint32) d->len > datalen)
			return false;
		if (d->byval && d->len <= 8)
		{
			uint64		u = 0;

			memcpy(&u, base + o, d->len);
			v->u = u;
		}
		else
		{
			v->data = base + o;
			v->len = d->len;
		}
		*off = o + (uint32) d->len;
		return true;
	}
	if (o >= datalen)
		return false;
	{
		uint32		consumed = varlena_total_len(base + o, datalen - o);

		if (consumed == 0)
			return false;
		if (!pwe_parse_varlena(v, base + o, datalen - o))
			return false;
		*off = o + consumed;
	}
	return true;
}

bool
pwe_tuple_decode_catalog(const PweTuple *t, const char *typecodes, int natts,
						 PweValue **vals_out, char *err, size_t errlen)
{
	PweValue   *vals;
	uint32		off = 0;
	int			i;

	if (!typecodes)
	{
		snprintf(err, errlen, "no catalog layout available");
		return false;
	}
	vals = pwe_calloc((size_t) natts, sizeof(PweValue));
	for (i = 0; i < natts; i++)
	{
		PweTypeDesc d;
		bool		nullbit = false;

		code_to_desc(typecodes[i], &d);
		if (i >= t->natts)
		{
			vals[i].isnull = true;
			vals[i].typecode = typecodes[i];
			continue;
		}
		if (t->has_null && t->nullbits)
			nullbit = (t->nullbits[i / 8] & (1 << (i % 8))) == 0;
		if (!decode_one(&vals[i], t->data, t->datalen, &off, &d, false, nullbit))
		{
			snprintf(err, errlen, "failed to decode catalog attribute %d", i + 1);
			pwe_values_free(vals, natts);
			return false;
		}
	}
	*vals_out = vals;
	return true;
}

bool
pwe_tuple_decode_row(const PweTuple *t, PweAttr **attrs, int nattrs,
					 PweValue **vals_out, char *err, size_t errlen)
{
	PweValue   *vals;
	uint32		off = 0;
	int			i;

	if (nattrs > t->natts)
	{
		snprintf(err, errlen, "tuple has %d attributes, relation expects %d",
				 t->natts, nattrs);
		return false;
	}
	vals = pwe_calloc((size_t) nattrs + 1, sizeof(PweValue));
	for (i = 1; i <= nattrs; i++)
	{
		PweAttr    *a = attrs ? attrs[i] : NULL;
		PweTypeDesc d;
		bool		nullbit = false;

		memset(&d, 0, sizeof(d));
		if (a)
		{
			d.len = a->len;
			d.byval = a->byval;
			d.align = a->align ? a->align : 'i';
			d.typid = a->typid;
		}
		else
			d.len = -1;
		if (t->has_null && t->nullbits)
			nullbit = (t->nullbits[(i - 1) / 8] & (1 << ((i - 1) % 8))) == 0;
		if (!decode_one(&vals[i], t->data, t->datalen, &off, &d,
						a ? a->dropped : true, nullbit))
		{
			snprintf(err, errlen, "failed to decode attribute %d", i);
			pwe_values_free(vals, nattrs + 1);
			return false;
		}
	}
	*vals_out = vals;
	return true;
}

/* ------------------------------------------------------------------ */
/* scalar formatting                                                  */
/* ------------------------------------------------------------------ */
static void
format_signed(PweStr *out, int64 v)
{
	pwe_str_addf(out, "%" PRId64, v);
}

static void
format_unsigned(PweStr *out, uint64 v)
{
	pwe_str_addf(out, "%" PRIu64, v);
}

/* Howard Hinnant's civil_from_days */
static void
civil_from_days(int64 z, int64 *y, int *m, int *d)
{
	z += 719468;
	{
		int64		era = (z >= 0 ? z : z - 146096) / 146097;
		uint64		doe = (uint64) (z - era * 146097);
		uint64		yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		int64		yy = (int64) yoe + era * 400;
		uint64		doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
		uint64		mp = (5 * doy + 2) / 153;

		*d = (int) (doy - (153 * mp + 2) / 5 + 1);
		*m = (int) (mp < 10 ? mp + 3 : mp - 9);
		*y = yy + (*m <= 2 ? 1 : 0);
	}
}

static void
format_date_value(PweStr *out, int32 days)
{
	int64		y;
	int			m,
				d;

	if (days == INT32_MAX)
	{
		pwe_str_adds(out, "infinity");
		return;
	}
	if (days == INT32_MIN)
	{
		pwe_str_adds(out, "-infinity");
		return;
	}
	civil_from_days((int64) days + 10957, &y, &m, &d);
	pwe_str_addf(out, "%04lld-%02d-%02d", (long long) y, m, d);
}

static void
format_time_us(PweStr *out, int64 us, int scale)
{
	int64		sec = us / 1000000;
	int64		frac = us % 1000000;
	int			hh,
				mm,
				ss;

	if (sec < 0)
		sec = 0;
	hh = (int) (sec / 3600);
	mm = (int) ((sec / 60) % 60);
	ss = (int) (sec % 60);
	pwe_str_addf(out, "%02d:%02d:%02d", hh, mm, ss);
	if (frac != 0)
	{
		char		buf[8];

		snprintf(buf, sizeof(buf), "%06lld", (long long) frac);
		if (scale >= 0 && scale < 6)
			buf[scale] = '\0';
		else
		{
			int			n = 5;

			while (n >= 0 && buf[n] == '0')
				buf[n--] = '\0';
		}
		pwe_str_addc(out, '.');
		pwe_str_adds(out, buf);
	}
}

static void
format_timestamp_value(PweStr *out, int64 us, bool with_tz)
{
	int64		days;
	int64		rem;
	int64		y;
	int			m,
				d;

	if (us == INT64_MAX)
	{
		pwe_str_adds(out, "infinity");
		return;
	}
	if (us == INT64_MIN)
	{
		pwe_str_adds(out, "-infinity");
		return;
	}
	days = us / INT64_C(86400000000);
	rem = us % INT64_C(86400000000);
	if (rem < 0)
	{
		rem += INT64_C(86400000000);
		days -= 1;
	}
	civil_from_days(days + 10957, &y, &m, &d);
	pwe_str_addf(out, "%04lld-%02d-%02d ", (long long) y, m, d);
	format_time_us(out, rem, -1);
	if (with_tz)
		pwe_str_adds(out, "+00");
}

/* numeric */
static void
format_numeric(PweStr *out, const uint8 *data, int len)
{
	uint16		hdr;
	bool		is_short;
	bool		neg;
	int			weight;
	int			dscale;
	int			ndigits;
	int			start = 0;
	int			i;
	PweStr		num;

	pwe_str_init(&num);
	if (len < 2)
	{
		pwe_str_adds(out, "NULL /* invalid numeric */");
		goto done;
	}
	hdr = pwe_read_u16(data);
	if ((hdr & 0xF000) == 0xC000)
	{
		if (hdr == 0xC000)
			pwe_str_adds(out, "'NaN'::numeric");
		else if (hdr == 0xD000)
			pwe_str_adds(out, "'Infinity'::numeric");
		else if (hdr == 0xF000)
			pwe_str_adds(out, "'-Infinity'::numeric");
		else
			pwe_str_adds(out, "'NaN'::numeric");
		goto done;
	}
	is_short = (hdr & 0x8000) != 0;
	neg = false;
	if (is_short)
	{
		neg = (hdr & 0x2000) != 0;
		dscale = (hdr & 0x1F80) >> 7;
		weight = hdr & 0x3F;
		if (hdr & 0x0040)
			weight -= 64;
		start = 2;
	}
	else
	{
		neg = (hdr & 0x4000) != 0;
		dscale = hdr & 0x3FFF;
		if (len < 4)
		{
			pwe_str_adds(out, "NULL /* invalid numeric */");
			goto done;
		}
		weight = pwe_read_i16(data + 2);
		start = 4;
	}
	ndigits = (len - start) / 2;
	if (ndigits <= 0 || weight < -1 || weight > 1000)
	{
		/* zero */
		pwe_str_adds(&num, "0");
		if (dscale > 0)
		{
			pwe_str_addc(&num, '.');
			for (i = 0; i < dscale; i++)
				pwe_str_addc(&num, '0');
		}
	}
	else
	{
		if (neg)
			pwe_str_addc(&num, '-');
		/* integer part */
		if (weight < 0)
			pwe_str_addc(&num, '0');
		else
		{
			for (i = 0; i <= weight; i++)
			{
				int			dig = (i < ndigits) ?
					pwe_read_u16(data + start + i * 2) : 0;

				if (i == 0)
					pwe_str_addf(&num, "%d", dig);
				else
					pwe_str_addf(&num, "%04d", dig);
			}
		}
		if (dscale > 0)
		{
			PweStr		frac;

			pwe_str_init(&frac);
			for (i = weight + 1; i < weight + 1 + (dscale + 3) / 4; i++)
			{
				int			dig = (i >= 0 && i < ndigits) ?
					pwe_read_u16(data + start + i * 2) : 0;

				pwe_str_addf(&frac, "%04d", dig);
			}
			while ((int) frac.len < dscale)
				pwe_str_addc(&frac, '0');
			frac.data[dscale] = '\0';
			pwe_str_addc(&num, '.');
			pwe_str_adds(&num, frac.data);
			pwe_str_free(&frac);
		}
	}
	pwe_str_addc(out, '\'');
	pwe_str_add(out, pwe_str_cstr(&num), num.len);
	pwe_str_adds(out, "'::numeric");
done:
	pwe_str_free(&num);
}

/*
 * COPY-format numeric: bare digits (or NaN/Infinity) with no SQL quoting
 * or ::numeric cast.  Returns false for malformed values so the caller
 * can emit \N.
 */
static bool
format_numeric_copy(PweStr *out, const uint8 *data, int len)
{
	uint16		hdr;
	bool		is_short;
	bool		neg;
	int			weight;
	int			dscale;
	int			ndigits;
	int			start = 0;
	int			i;

	if (len < 2)
		return false;
	hdr = pwe_read_u16(data);
	if ((hdr & 0xF000) == 0xC000)
	{
		if (hdr == 0xD000)
			pwe_str_adds(out, "Infinity");
		else if (hdr == 0xF000)
			pwe_str_adds(out, "-Infinity");
		else
			pwe_str_adds(out, "NaN");
		return true;
	}
	is_short = (hdr & 0x8000) != 0;
	neg = false;
	if (is_short)
	{
		neg = (hdr & 0x2000) != 0;
		dscale = (hdr & 0x1F80) >> 7;
		weight = hdr & 0x3F;
		if (hdr & 0x0040)
			weight -= 64;
		start = 2;
	}
	else
	{
		neg = (hdr & 0x4000) != 0;
		dscale = hdr & 0x3FFF;
		if (len < 4)
			return false;
		weight = pwe_read_i16(data + 2);
		start = 4;
	}
	ndigits = (len - start) / 2;
	if (ndigits <= 0 || weight < -1 || weight > 1000)
	{
		pwe_str_addc(out, '0');
		if (dscale > 0)
		{
			pwe_str_addc(out, '.');
			for (i = 0; i < dscale; i++)
				pwe_str_addc(out, '0');
		}
	}
	else
	{
		if (neg)
			pwe_str_addc(out, '-');
		if (weight < 0)
			pwe_str_addc(out, '0');
		else
		{
			for (i = 0; i <= weight; i++)
			{
				int			dig = (i < ndigits) ?
					pwe_read_u16(data + start + i * 2) : 0;

				if (i == 0)
					pwe_str_addf(out, "%d", dig);
				else
					pwe_str_addf(out, "%04d", dig);
			}
		}
		if (dscale > 0)
		{
			PweStr		frac;

			pwe_str_init(&frac);
			for (i = weight + 1; i < weight + 1 + (dscale + 3) / 4; i++)
			{
				int			dig = (i >= 0 && i < ndigits) ?
					pwe_read_u16(data + start + i * 2) : 0;

				pwe_str_addf(&frac, "%04d", dig);
			}
			while ((int) frac.len < dscale)
				pwe_str_addc(&frac, '0');
			frac.data[dscale] = '\0';
			pwe_str_addc(out, '.');
			pwe_str_adds(out, frac.data);
			pwe_str_free(&frac);
		}
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* jsonb binary -> JSON text                                          */
/* ------------------------------------------------------------------ */
static bool jsonb_render(const uint8 *data, int len, PweStr *out, int depth);

static bool
jsonb_render_node(const uint8 *base, uint32 reloff, int len, uint32 type,
				  PweStr *out, int depth)
{
	const uint8 *data = base + reloff;

	if ((type & 0x70000000) == 0x00000000)	/* string */
	{
		pwe_str_add_json(out, (const char *) data, (size_t) len);
		return true;
	}
	if ((type & 0x70000000) == 0x10000000)	/* numeric */
	{
		PweValue	v;
		uint32		vl;
		uint32		aoff = (reloff + 3u) & ~3u;
		int			alen = len - (int) (aoff - reloff);

		memset(&v, 0, sizeof(v));
		data = base + aoff;
		if (alen < 4)
			return false;
		vl = (pwe_read_u32(data) >> 2) & 0x3FFFFFFF;
		if (vl < 4 || vl > (uint32) alen)
			return false;
		if (!pwe_parse_varlena(&v, data, vl))
			return false;
		if (!v.data)
			return false;
		{
			PweStr		num;
			const char *np;
			size_t		nl;

			pwe_str_init(&num);
			format_numeric(&num, v.data, v.len);
			np = pwe_str_cstr(&num);
			nl = num.len;
			if (nl >= 2 && np[0] == '\'')
			{
				const char *end = strstr(np + 1, "'::numeric");

				if (end)
				{
					np++;
					nl = (size_t) (end - np);
				}
			}
			pwe_str_add(out, np, nl);
			pwe_str_free(&num);
		}
		pwe_value_free(&v);
		return true;
	}
	if ((type & 0x70000000) == 0x20000000)	/* false */
	{
		pwe_str_adds(out, "false");
		return true;
	}
	if ((type & 0x70000000) == 0x30000000)	/* true */
	{
		pwe_str_adds(out, "true");
		return true;
	}
	if ((type & 0x70000000) == 0x40000000)	/* null */
	{
		pwe_str_adds(out, "null");
		return true;
	}
	if ((type & 0x70000000) == 0x50000000)	/* nested container */
	{
		uint32		aoff = (reloff + 3u) & ~3u;

		return jsonb_render(base + aoff, len - (int) (aoff - reloff),
							out, depth + 1);
	}
	return false;
}

static bool
jsonb_render(const uint8 *data, int len, PweStr *out, int depth)
{
	uint32		header;
	uint32		count;
	bool		object;
	bool		scalar;
	uint32		nentries;
	const uint8 *children;
	const uint8 *payload;
	uint32		start = 0;
	uint32		ends[1024];
	uint32		starts[1024];
	uint32		i;
	PweStr		keys[512];
	PweStr		vals[512];
	uint32		nkids;

	if (depth > 32 || len < 4)
	{
		return false;
	}
	header = pwe_read_u32(data);
	count = header & 0x0FFFFFFF;
	object = (header & 0x20000000) != 0;
	scalar = (header & 0x10000000) != 0;
	nentries = object ? count * 2 : count;
	if (nentries > 1024 || (uint32) len < 4 + nentries * 4)
	{
		return false;
	}
	children = data + 4;
	payload = children + nentries * 4;
	for (i = 0; i < nentries; i++)
	{
		uint32		je = pwe_read_u32(children + i * 4);
		uint32		offlen = je & 0x0FFFFFFF;
		uint32		end;

		starts[i] = start;
		if (je & 0x80000000)
			end = offlen;
		else
			end = start + offlen;
		ends[i] = end;
		if (end < start || (uint32) (payload - data) + end > (uint32) len)
		{
			return false;
		}
		start = end;
	}
	if (scalar && nentries == 1)
		return jsonb_render_node(payload, starts[0], (int) (ends[0] - starts[0]),
								 pwe_read_u32(children), out, depth);
	if (!object)
	{
		pwe_str_addc(out, '[');
		for (i = 0; i < nentries; i++)
		{
			if (i)
				pwe_str_addc(out, ',');
			if (!jsonb_render_node(payload, starts[i], (int) (ends[i] - starts[i]),
								   pwe_read_u32(children + i * 4), out, depth))
			{
				return false;
			}
		}
		pwe_str_addc(out, ']');
		return true;
	}
	nkids = count < 512 ? count : 512;
	for (i = 0; i < nkids; i++)
	{
		pwe_str_init(&keys[i]);
		pwe_str_init(&vals[i]);
		if (!jsonb_render_node(payload, starts[i], (int) (ends[i] - starts[i]),
							   pwe_read_u32(children + i * 4), &keys[i], depth) ||
			!jsonb_render_node(payload, starts[count + i],
							   (int) (ends[count + i] - starts[count + i]),
							   pwe_read_u32(children + (count + i) * 4), &vals[i], depth))
		{
			uint32		j;

			for (j = 0; j <= i; j++)
			{
				pwe_str_free(&keys[j]);
				pwe_str_free(&vals[j]);
			}
			return false;
		}
	}
	pwe_str_addc(out, '{');
	for (i = 0; i < nkids; i++)
	{
		if (i)
			pwe_str_addc(out, ',');
		pwe_str_add(out, keys[i].data, keys[i].len);
		pwe_str_addc(out, ':');
		pwe_str_add(out, vals[i].data, vals[i].len);
	}
	pwe_str_addc(out, '}');
	for (i = 0; i < nkids; i++)
	{
		pwe_str_free(&keys[i]);
		pwe_str_free(&vals[i]);
	}
	return true;
}

static void
format_uuid(PweStr *out, const uint8 *data)
{
	char		buf[40];

	snprintf(buf, sizeof(buf),
			 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			 data[0], data[1], data[2], data[3], data[4], data[5],
			 data[6], data[7], data[8], data[9], data[10], data[11],
			 data[12], data[13], data[14], data[15]);
	pwe_str_addc(out, '\'');
	pwe_str_adds(out, buf);
	pwe_str_adds(out, "'::uuid");
}

static void
format_bytea(PweStr *out, const uint8 *data, int len)
{
	int			i;

	pwe_str_adds(out, "'\\x");
	for (i = 0; i < len; i++)
		pwe_str_addf(out, "%02x", data[i]);
	pwe_str_adds(out, "'::bytea");
}

/* render one dimension of an array as ARRAY[...]; leaves are SQL literals */
static bool
format_array_dim(PweCatalog *cat, uint32 db, const uint8 *p, uint32 len,
				 const PweTypeInfo *elemti, uint32 elemtype,
				 int ndim, const int32 *dims, int dimno,
				 const uint8 *nullbits, uint32 *off, int *itemidx, PweStr *out)
{
	int			i;

	pwe_str_adds(out, "ARRAY[");
	for (i = 0; i < dims[dimno]; i++)
	{
		if (i > 0)
			pwe_str_adds(out, ", ");
		if (dimno + 1 < ndim)
		{
			if (!format_array_dim(cat, db, p, len, elemti, elemtype,
								  ndim, dims, dimno + 1, nullbits, off,
								  itemidx, out))
				return false;
			continue;
		}
		{
			PweValue	ev;
			PweStr		lit;
			uint32		consumed;
			int			idx = (*itemidx)++;

			if (nullbits && !(nullbits[idx / 8] & (1 << (idx % 8))))
			{
				pwe_str_adds(out, "NULL");
				continue;
			}
			if (elemti->typlen > 0 ||
				*off >= len || (p[*off] & 0x01) == 0)
			{
				uint32		a = 1;

				if (elemti->align == 'd')
					a = 8;
				else if (elemti->align == 'i')
					a = 4;
				else if (elemti->align == 's')
					a = 2;
				*off = (*off + a - 1) & ~(a - 1);
			}
			if (*off > len)
				return false;
			consumed = (elemti->typlen > 0) ? (uint32) elemti->typlen :
				varlena_total_len(p + *off, len - *off);
			if (consumed == 0 || *off + consumed > len)
				return false;
			memset(&ev, 0, sizeof(ev));
			ev.typlen = elemti->typlen;
			ev.byval = elemti->byval;
			ev.align = elemti->align;
			ev.typid = elemtype;
			if (elemti->typlen > 0)
			{
				if (elemti->byval && elemti->typlen <= 8)
					memcpy(&ev.u, p + *off, elemti->typlen);
				else
				{
					ev.data = p + *off;
					ev.len = elemti->typlen;
				}
			}
			else if (!pwe_parse_varlena(&ev, p + *off, consumed))
				return false;
			pwe_str_init(&lit);
			pwe_value_literal(cat, db, &ev, elemti, &lit, 11);
			pwe_str_add(out, pwe_str_cstr(&lit), lit.len);
			pwe_str_free(&lit);
			pwe_value_free(&ev);
			*off += consumed;
		}
	}
	pwe_str_addc(out, ']');
	return true;
}

static bool
format_array(PweCatalog *cat, uint32 db, PweValue *v, const PweTypeInfo *ti,
			 PweStr *out)
{
	const uint8 *p = v->data;
	int			len = v->len;
	int32		ndim,
				dataoffset;
	uint32		elemtype;
	int32		dims[8];
	int			i;
	int			nitems;
	uint32		off;
	const uint8 *nullbits = NULL;
	PweTypeInfo elemti;
	int			itemidx = 0;

	if (len < 12)
	{
		return false;
	}
	ndim = pwe_read_i32(p);
	dataoffset = pwe_read_i32(p + 4);
	elemtype = pwe_read_u32(p + 8);
	/*
	 * Pre-bulk-output builds rendered only one-dimensional, non-empty arrays
	 * as ARRAY[...]; keep that exact text for the plain SQL stream.
	 */
	if (sql_legacy_arrays)
	{
		if (ndim < 1 || ndim > 8 || ndim != 1 || 12 + ndim * 8 > len)
			return false;
		dims[0] = pwe_read_i32(p + 12);
		if (dims[0] < 0)
			return false;
		nitems = dims[0];
		if (nitems == 0)
		{
			pwe_str_adds(out, "ARRAY[]");
			return true;
		}
	}
	else if (ndim < 0 || ndim > 8)
	{
		return false;
	}
	if (ndim == 0)
	{
		/* empty array: '[]' needs an explicit cast for the parser */
		PweStr		tn;

		pwe_str_init(&tn);
		pwe_type_display_name(ti, NULL, &tn, false);
		pwe_str_addf(out, "'{}'::%s", pwe_str_cstr(&tn));
		pwe_str_free(&tn);
		return true;
	}
	if (12 + ndim * 8 > len)
	{
		return false;
	}
	nitems = 1;
	for (i = 0; i < ndim; i++)
	{
		dims[i] = pwe_read_i32(p + 12 + i * 4);
		if (dims[i] < 0)
			return false;
		nitems *= dims[i];
	}
	if (nitems == 0)
	{
		PweStr		tn;

		pwe_str_init(&tn);
		pwe_type_display_name(ti, NULL, &tn, false);
		pwe_str_addf(out, "'{}'::%s", pwe_str_cstr(&tn));
		pwe_str_free(&tn);
		return true;
	}
	/* v->data excludes the 4-byte varlena header, while ArrayType's
	 * offsets are relative to the array start (including that header). */
	if (dataoffset != 0)
	{
		if ((uint32) dataoffset < 16 + (uint32) ndim * 8 || dataoffset > len + 4)
			return false;
		nullbits = p + 12 + ndim * 8;
		off = (uint32) dataoffset - 4;
	}
	else
		off = (uint32) (((16 + ndim * 8) + 7u) & ~7u) - 4;
	if (dataoffset != 0)
	{
		uint32		bitmaplen = ((uint32) nitems + 7) / 8;

		if (12 + ndim * 8 + bitmaplen > (uint32) len)
			return false;
	}
	elemti = pwe_catalog_type_info(cat, db, elemtype);
	if (!elemti.found)
	{
		memset(&elemti, 0, sizeof(elemti));
		elemti.oid = elemtype;
		elemti.typlen = -1;
		elemti.align = 'i';
		strcpy(elemti.name, "text");
		elemti.typtype = 'b';
		elemti.category = 'S';
	}
	return format_array_dim(cat, db, p, (uint32) len, &elemti, elemtype,
							ndim, dims, 0, nullbits, &off, &itemidx, out);
}

void
pwe_type_display_name(const PweTypeInfo *ti, const char *nspname,
					  PweStr *out, bool qualified)
{
	const char *name = ti->name[0] ? ti->name : "text";
	bool		is_array = (name[0] == '_' && ti->elem != 0 &&
							(ti->category == 'A' || ti->typlen < 0));

	if (is_array)
		name++;
	if (qualified && !is_array && nspname && nspname[0] &&
		strcmp(nspname, "pg_catalog") != 0)
	{
		pwe_str_add_ident(out, nspname);
		pwe_str_addc(out, '.');
	}
	if (strcmp(name, "char") == 0 && ti->oid == 18)
		pwe_str_adds(out, "\"char\"");
	else
		pwe_str_add_ident(out, name);
	if (is_array)
		pwe_str_adds(out, "[]");
}

void
pwe_format_typmod(const PweTypeInfo *ti, int32 typmod, PweStr *out)
{
	if (typmod < 0)
		return;
	if (ti->oid == 1043 || ti->oid == 1042 || ti->oid == 1560 || ti->oid == 1562)
		pwe_str_addf(out, "(%d)", typmod - 4);
	else if (ti->oid == 1700)
	{
		int32		m = typmod - 4;

		pwe_str_addf(out, "(%d,%d)", (m >> 16) & 0xFFFF, m & 0xFFFF);
	}
	else if (ti->oid == 1114 || ti->oid == 1184 || ti->oid == 1083 ||
			 ti->oid == 1266 || ti->oid == 1186)
		pwe_str_addf(out, "(%d)", typmod & 0xFFFF);
}

void
pwe_value_literal(PweCatalog *cat, uint32 db, PweValue *v, const PweTypeInfo *ti,
				  PweStr *out, int version)
{
	(void) version;
	if (v->isnull)
	{
		pwe_str_adds(out, "NULL");
		return;
	}
	if (v->toast.valid && v->data == NULL)
	{
		pwe_str_addf(out, "NULL /* pg_walex: unresolved toast value %u */",
					 v->toast.valueid);
		return;
	}
	if (v->data == NULL && v->typlen > 8)
	{
		pwe_str_adds(out, "NULL /* pg_walex: value unavailable */");
		return;
	}

	switch (ti->oid)
	{
		case 16:				/* bool */
			pwe_str_adds(out, v->u ? "true" : "false");
			return;
		case 18:				/* "char" */
			{
				char		c = (char) (v->u & 0xff);

				pwe_str_add_quoted(out, &c, 1);
				pwe_str_adds(out, "::\"char\"");
				return;
			}
		case 19:				/* name */
		case 25:				/* text */
		case 1043:				/* varchar */
		case 1042:				/* bpchar */
		case 705:				/* unknown */
			if (!v->data)
				break;
			pwe_str_add_quoted(out, (const char *) v->data, v->len);
			return;
		case 17:				/* bytea */
			if (!v->data)
				break;
			format_bytea(out, v->data, v->len);
			return;
		case 20:				/* int8 */
			if (v->typlen == 8)
			{
				format_signed(out, (int64) v->u);
				return;
			}
			break;
		case 21:				/* int2 */
			format_signed(out, (int16) (v->u & 0xffff));
			return;
		case 23:				/* int4 */
		case 24:				/* regproc */
			format_signed(out, (int32) (v->u & 0xffffffff));
			return;
		case 26:				/* oid */
		case 28:				/* xid */
		case 29:				/* cid */
		case 2205:				/* regclass */
		case 2206:				/* regtype */
		case 4089:				/* regnamespace */
		case 4096:				/* regrole */
			format_unsigned(out, (uint32) (v->u & 0xffffffff));
			return;
		case 700:				/* float4 */
			{
				uint32		bits = (uint32) (v->u & 0xffffffff);
				float		f;

				memcpy(&f, &bits, 4);
				if (isnan(f))
					pwe_str_adds(out, "'NaN'::real");
				else if (isinf(f))
					pwe_str_adds(out, f > 0 ? "'Infinity'::real" : "'-Infinity'::real");
				else
					pwe_str_addf(out, "%.9g", (double) f);
				return;
			}
		case 701:				/* float8 */
			{
				double		d;

				memcpy(&d, &v->u, 8);
				if (isnan(d))
					pwe_str_adds(out, "'NaN'::double precision");
				else if (isinf(d))
					pwe_str_adds(out, d > 0 ? "'Infinity'::double precision" : "'-Infinity'::double precision");
				else
					pwe_str_addf(out, "%.17g", d);
				return;
			}
		case 1700:				/* numeric */
			if (v->data)
			{
				format_numeric(out, v->data, v->len);
				return;
			}
			break;
		case 1082:				/* date */
			pwe_str_addc(out, '\'');
			format_date_value(out, (int32) v->u);
			pwe_str_adds(out, "'::date");
			return;
		case 1083:				/* time */
			pwe_str_addc(out, '\'');
			format_time_us(out, (int64) v->u, -1);
			pwe_str_adds(out, "'::time");
			return;
		case 1266:				/* timetz */
			if (v->data && v->typlen == 12)
			{
				int64		us = pwe_read_i64(v->data);
				int32		zone = pwe_read_i32(v->data + 8);
				int			east = -zone;
				int			hh = east / 3600;
				int			mm = (east / 60) % 60;

				pwe_str_addc(out, '\'');
				format_time_us(out, us, -1);
				pwe_str_addf(out, "%c%02d:%02d", east >= 0 ? '+' : '-',
							 abs(hh), abs(mm));
				pwe_str_adds(out, "'::timetz");
				return;
			}
			break;
		case 1114:				/* timestamp */
			pwe_str_addc(out, '\'');
			format_timestamp_value(out, (int64) v->u, false);
			pwe_str_adds(out, "'::timestamp");
			return;
		case 1184:				/* timestamptz */
			pwe_str_addc(out, '\'');
			format_timestamp_value(out, (int64) v->u, true);
			pwe_str_adds(out, "'::timestamptz");
			return;
		case 1186:				/* interval */
			if (v->data && v->typlen == 16)
			{
				int64		us = pwe_read_i64(v->data);
				int32		days = pwe_read_i32(v->data + 8);
				int32		months = pwe_read_i32(v->data + 12);
				int32		years = months / 12;
				int32		mons = months % 12;
				bool		first = true;

				pwe_str_addc(out, '\'');
				if (years)
				{
					pwe_str_addf(out, "%d year%s", years, years == 1 ? "" : "s");
					first = false;
				}
				if (mons)
				{
					if (!first)
						pwe_str_addc(out, ' ');
					pwe_str_addf(out, "%d mon%s", mons, mons == 1 ? "" : "s");
					first = false;
				}
				if (days)
				{
					if (!first)
						pwe_str_addc(out, ' ');
					pwe_str_addf(out, "%d day%s", days, days == 1 ? "" : "s");
					first = false;
				}
				if (us || first)
				{
					if (!first)
						pwe_str_addc(out, ' ');
					format_time_us(out, us, -1);
				}
				pwe_str_adds(out, "'::interval");
				return;
			}
			break;
		case 2950:				/* uuid */
			if (v->data && v->len == 16)
			{
				format_uuid(out, v->data);
				return;
			}
			break;
		case 114:				/* json */
			if (v->data)
			{
				pwe_str_add_quoted(out, (const char *) v->data, v->len);
				pwe_str_adds(out, "::json");
				return;
			}
			break;
		case 3802:				/* jsonb */
			if (v->data && v->len >= 4)
			{
				PweStr		js;

				pwe_str_init(&js);
				if (jsonb_render(v->data, v->len, &js, 0))
				{
					pwe_str_add_quoted(out, pwe_str_cstr(&js), js.len);
					pwe_str_adds(out, "::jsonb");
					pwe_str_free(&js);
					return;
				}
				pwe_str_free(&js);
			}
			break;
		case 142:				/* xml */
			if (v->data)
			{
				pwe_str_add_quoted(out, (const char *) v->data, v->len);
				pwe_str_adds(out, "::xml");
				return;
			}
			break;
		default:
			break;
	}

	/* enum types */
	if (ti->typtype == 'e' && v->byval)
	{
		const char *label = pwe_catalog_enum_label(cat, db, (uint32) (v->u & 0xffffffff));

		if (label)
		{
			pwe_str_add_quoted(out, label, strlen(label));
			return;
		}
		format_unsigned(out, (uint32) (v->u & 0xffffffff));
		return;
	}
	/* arrays */
	if (v->data && ti->elem != 0 && ti->typlen < 0 && ti->typtype != 'e' &&
		(ti->category == 'A' || ti->name[0] == '_' || pwe_builtin_array_element(ti->oid) != 0))
	{
		if (format_array(cat, db, v, ti, out))
			return;
	}
	/* domain over a known base type */
	if (ti->typtype == 'd' && ti->basetype != 0 && ti->basetype != ti->oid)
	{
		PweTypeInfo base = pwe_catalog_type_info(cat, db, ti->basetype);

		if (base.found)
		{
			pwe_value_literal(cat, db, v, &base, out, version);
			return;
		}
	}
	/* string-ish fallback */
	if (v->data && ti->typlen < 0 &&
		(ti->category == 'S' || ti->category == 'U'))
	{
		pwe_str_add_quoted(out, (const char *) v->data, v->len);
		return;
	}
	pwe_str_addf(out, "NULL /* pg_walex: cannot render type %s (oid %u) */",
				 ti->name[0] ? ti->name : "?", ti->oid);
}

/* ------------------------------------------------------------------ */
/* COPY text-format value rendering (raw text, no quoting/casts)      */
/* ------------------------------------------------------------------ */

/* append one array element, raw form, to out */
static bool
array_copy_scalar(PweCatalog *cat, uint32 db, const uint8 *p, uint32 len,
				  const PweTypeInfo *elemti, uint32 elemtype,
				  uint32 *off, PweStr *out)
{
	PweValue	ev;
	PweStr		raw;
	uint32		consumed;
	uint32		a = 1;
	bool		ok;
	size_t		i;
	bool		need_quote;

	if (elemti->typlen > 0 ||
		*off >= len || (p[*off] & 0x01) == 0)
	{
		if (elemti->align == 'd')
			a = 8;
		else if (elemti->align == 'i')
			a = 4;
		else if (elemti->align == 's')
			a = 2;
		*off = (*off + a - 1) & ~(a - 1);
	}
	if (*off > len)
		return false;
	consumed = (elemti->typlen > 0) ? (uint32) elemti->typlen :
		varlena_total_len(p + *off, len - *off);
	if (consumed == 0 || *off + consumed > len)
		return false;
	memset(&ev, 0, sizeof(ev));
	ev.typlen = elemti->typlen;
	ev.byval = elemti->byval;
	ev.align = elemti->align;
	ev.typid = elemtype;
	if (elemti->typlen > 0)
	{
		if (elemti->byval && elemti->typlen <= 8)
			memcpy(&ev.u, p + *off, elemti->typlen);
		else
		{
			ev.data = p + *off;
			ev.len = elemti->typlen;
		}
	}
	else if (!pwe_parse_varlena(&ev, p + *off, consumed))
		return false;
	pwe_str_init(&raw);
	ok = pwe_value_copy(cat, db, &ev, elemti, &raw);
	pwe_value_free(&ev);
	*off += consumed;
	if (!ok)
	{
		pwe_str_free(&raw);
		return false;
	}
	/* array member quoting: quote when empty, when it contains a special
	 * character or whitespace, or when it reads as NULL */
	need_quote = (raw.len == 0);
	if (!need_quote && raw.len == 4 && memcmp(raw.data, "NULL", 4) == 0)
		need_quote = true;
	for (i = 0; i < raw.len && !need_quote; i++)
	{
		char		c = raw.data[i];

		if (c == '{' || c == '}' || c == ',' || c == '"' || c == '\\' ||
			c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
			c == '\f')
			need_quote = true;
	}
	if (!need_quote)
		pwe_str_add(out, raw.data, raw.len);
	else
	{
		pwe_str_addc(out, '"');
		for (i = 0; i < raw.len; i++)
		{
			char		c = raw.data[i];

			if (c == '"' || c == '\\')
				pwe_str_addc(out, '\\');
			pwe_str_addc(out, c);
		}
		pwe_str_addc(out, '"');
	}
	pwe_str_free(&raw);
	return true;
}

static bool
format_array_copy_dim(PweCatalog *cat, uint32 db, const uint8 *p, uint32 len,
					  const PweTypeInfo *elemti, uint32 elemtype,
					  int ndim, const int32 *dims, int dimno,
					  const uint8 *nullbits, uint32 *off, int *itemidx,
					  PweStr *out)
{
	int			i;

	pwe_str_addc(out, '{');
	for (i = 0; i < dims[dimno]; i++)
	{
		if (i > 0)
			pwe_str_addc(out, ',');
		if (dimno == ndim - 1)
		{
			int			item = (*itemidx)++;

			if (nullbits && !(nullbits[item / 8] & (1 << (item % 8))))
			{
				pwe_str_adds(out, "NULL");
				continue;
			}
			if (!array_copy_scalar(cat, db, p, len, elemti, elemtype, off, out))
				return false;
		}
		else if (!format_array_copy_dim(cat, db, p, len, elemti, elemtype,
										ndim, dims, dimno + 1, nullbits,
										off, itemidx, out))
			return false;
	}
	pwe_str_addc(out, '}');
	return true;
}

/* render an array in COPY text form: {a,b,"c c",NULL} */
static bool
format_array_copy(PweCatalog *cat, uint32 db, PweValue *v, const PweTypeInfo *ti,
				  PweStr *out)
{
	const uint8 *p = v->data;
	int			len = v->len;
	int32		ndim,
				dataoffset;
	uint32		elemtype;
	int32		dims[8];
	int			i;
	int			nitems = 1;
	uint32		off;
	const uint8 *nullbits = NULL;
	PweTypeInfo elemti;
	int			itemidx = 0;

	if (len < 12)
		return false;
	ndim = pwe_read_i32(p);
	dataoffset = pwe_read_i32(p + 4);
	elemtype = pwe_read_u32(p + 8);
	if (ndim == 0)
	{
		pwe_str_adds(out, "{}");
		return true;
	}
	if (ndim < 0 || ndim > 8)
		return false;
	if (12 + ndim * 8 > len)
		return false;
	for (i = 0; i < ndim; i++)
	{
		dims[i] = pwe_read_i32(p + 12 + i * 4);
		if (dims[i] < 0)
			return false;
		nitems *= dims[i];
	}
	/* v->data excludes the 4-byte varlena header, while ArrayType's
	 * offsets are relative to the array start (including that header). */
	if (dataoffset != 0)
	{
		if ((uint32) dataoffset < 16 + (uint32) ndim * 8 || dataoffset > len + 4)
			return false;
		nullbits = p + 12 + ndim * 8;
		off = (uint32) dataoffset - 4;
	}
	else
		off = (uint32) (((16 + ndim * 8) + 7u) & ~7u) - 4;
	if (dataoffset != 0)
	{
		uint32		bitmaplen = ((uint32) nitems + 7) / 8;

		if (12 + ndim * 8 + bitmaplen > (uint32) len)
			return false;
	}
	elemti = pwe_catalog_type_info(cat, db, elemtype);
	if (!elemti.found)
	{
		memset(&elemti, 0, sizeof(elemti));
		elemti.oid = elemtype;
		elemti.typlen = -1;
		elemti.align = 'i';
		strcpy(elemti.name, "text");
		elemti.typtype = 'b';
		elemti.category = 'S';
	}
	return format_array_copy_dim(cat, db, p, (uint32) len, &elemti, elemtype,
								 ndim, dims, 0, nullbits, &off, &itemidx, out);
}

/*
 * Render a value in PostgreSQL COPY text format (raw text, no quoting and
 * no ::casts).  The caller escapes \\, tab, newline and carriage return and
 * translates SQL NULL to \N.  Returns false when the value cannot be
 * rendered; the caller then emits \N.
 */
bool
pwe_value_copy(PweCatalog *cat, uint32 db, PweValue *v, const PweTypeInfo *ti,
			   PweStr *out)
{
	if (v->isnull)
		return false;
	if (v->toast.valid && v->data == NULL)
		return false;			/* unresolved toast */
	if (v->data == NULL && v->typlen > 8)
		return false;			/* value unavailable */

	switch (ti->oid)
	{
		case 16:				/* bool */
			pwe_str_adds(out, v->u ? "t" : "f");
			return true;
		case 18:				/* "char" */
			{
				char		c = (char) (v->u & 0xff);

				pwe_str_add(out, &c, 1);
				return true;
			}
		case 19:				/* name */
		case 25:				/* text */
		case 1043:				/* varchar */
		case 1042:				/* bpchar */
		case 705:				/* unknown */
			if (!v->data)
				break;
			pwe_str_add(out, (const char *) v->data, v->len);
			return true;
		case 17:				/* bytea */
			if (!v->data)
				break;
			{
				int			i;

				pwe_str_adds(out, "\\x");
				for (i = 0; i < v->len; i++)
					pwe_str_addf(out, "%02x", v->data[i]);
			}
			return true;
		case 20:				/* int8 */
			if (v->typlen == 8)
			{
				format_signed(out, (int64) v->u);
				return true;
			}
			break;
		case 21:				/* int2 */
			format_signed(out, (int16) (v->u & 0xffff));
			return true;
		case 23:				/* int4 */
		case 24:				/* regproc */
			format_signed(out, (int32) (v->u & 0xffffffff));
			return true;
		case 26:				/* oid */
		case 28:				/* xid */
		case 29:				/* cid */
		case 2205:				/* regclass */
		case 2206:				/* regtype */
		case 4089:				/* regnamespace */
		case 4096:				/* regrole */
			format_unsigned(out, (uint32) (v->u & 0xffffffff));
			return true;
		case 700:				/* float4 */
			{
				uint32		bits = (uint32) (v->u & 0xffffffff);
				float		f;

				memcpy(&f, &bits, 4);
				if (isnan(f))
					pwe_str_adds(out, "NaN");
				else if (isinf(f))
					pwe_str_adds(out, f > 0 ? "Infinity" : "-Infinity");
				else
					pwe_str_addf(out, "%.9g", (double) f);
				return true;
			}
		case 701:				/* float8 */
			{
				double		d;

				memcpy(&d, &v->u, 8);
				if (isnan(d))
					pwe_str_adds(out, "NaN");
				else if (isinf(d))
					pwe_str_adds(out, d > 0 ? "Infinity" : "-Infinity");
				else
					pwe_str_addf(out, "%.17g", d);
				return true;
			}
		case 1700:				/* numeric */
			if (v->data && v->len >= 2)
			{
				uint16		hdr = pwe_read_u16(v->data);

				if ((hdr & 0xF000) == 0xC000)
				{
					if (hdr == 0xD000)
						pwe_str_adds(out, "Infinity");
					else if (hdr == 0xF000)
						pwe_str_adds(out, "-Infinity");
					else
						pwe_str_adds(out, "NaN");
					return true;
				}
				return format_numeric_copy(out, v->data, v->len);
			}
			break;
		case 1082:				/* date */
			format_date_value(out, (int32) v->u);
			return true;
		case 1083:				/* time */
			format_time_us(out, (int64) v->u, -1);
			return true;
		case 1266:				/* timetz */
			if (v->data && v->typlen == 12)
			{
				int64		us = pwe_read_i64(v->data);
				int32		zone = pwe_read_i32(v->data + 8);
				int			east = -zone;
				int			hh = east / 3600;
				int			mm = (east / 60) % 60;

				format_time_us(out, us, -1);
				pwe_str_addf(out, "%c%02d:%02d", east >= 0 ? '+' : '-',
							 abs(hh), abs(mm));
				return true;
			}
			break;
		case 1114:				/* timestamp */
			format_timestamp_value(out, (int64) v->u, false);
			return true;
		case 1184:				/* timestamptz */
			format_timestamp_value(out, (int64) v->u, true);
			return true;
		case 1186:				/* interval */
			if (v->data && v->typlen == 16)
			{
				int64		us = pwe_read_i64(v->data);
				int32		days = pwe_read_i32(v->data + 8);
				int32		months = pwe_read_i32(v->data + 12);
				int32		years = months / 12;
				int32		mons = months % 12;
				bool		first = true;

				if (years)
				{
					pwe_str_addf(out, "%d year%s", years, years == 1 ? "" : "s");
					first = false;
				}
				if (mons)
				{
					if (!first)
						pwe_str_addc(out, ' ');
					pwe_str_addf(out, "%d mon%s", mons, mons == 1 ? "" : "s");
					first = false;
				}
				if (days)
				{
					if (!first)
						pwe_str_addc(out, ' ');
					pwe_str_addf(out, "%d day%s", days, days == 1 ? "" : "s");
					first = false;
				}
				if (us || first)
				{
					if (!first)
						pwe_str_addc(out, ' ');
					format_time_us(out, us, -1);
				}
				return true;
			}
			break;
		case 2950:				/* uuid */
			if (v->data && v->len == 16)
			{
				pwe_str_addf(out,
							 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
							 v->data[0], v->data[1], v->data[2], v->data[3],
							 v->data[4], v->data[5], v->data[6], v->data[7],
							 v->data[8], v->data[9], v->data[10], v->data[11],
							 v->data[12], v->data[13], v->data[14], v->data[15]);
				return true;
			}
			break;
		case 114:				/* json */
			if (v->data)
			{
				pwe_str_add(out, (const char *) v->data, v->len);
				return true;
			}
			break;
		case 3802:				/* jsonb */
			if (v->data && v->len >= 4)
				return jsonb_render(v->data, v->len, out, 0);
			break;
		case 142:				/* xml */
			if (v->data)
			{
				pwe_str_add(out, (const char *) v->data, v->len);
				return true;
			}
			break;
		default:
			break;
	}

	/* enum types */
	if (ti->typtype == 'e' && v->byval)
	{
		const char *label = pwe_catalog_enum_label(cat, db, (uint32) (v->u & 0xffffffff));

		if (label)
		{
			pwe_str_adds(out, label);
			return true;
		}
		format_unsigned(out, (uint32) (v->u & 0xffffffff));
		return true;
	}
	/* arrays */
	if (v->data && ti->elem != 0 && ti->typlen < 0 && ti->typtype != 'e' &&
		(ti->category == 'A' || ti->name[0] == '_' || pwe_builtin_array_element(ti->oid) != 0))
	{
		if (format_array_copy(cat, db, v, ti, out))
			return true;
	}
	/* domain over a known base type */
	if (ti->typtype == 'd' && ti->basetype != 0 && ti->basetype != ti->oid)
	{
		PweTypeInfo base = pwe_catalog_type_info(cat, db, ti->basetype);

		if (base.found)
			return pwe_value_copy(cat, db, v, &base, out);
	}
	/* string-ish fallback */
	if (v->data && ti->typlen < 0 &&
		(ti->category == 'S' || ti->category == 'U'))
	{
		pwe_str_add(out, (const char *) v->data, v->len);
		return true;
	}
	return false;
}
