/*
 * pg_walex - PostgreSQL WAL parsing and recovery tool
 * Copyright (C) 2026 songshaohua <developerhonor@petalmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * pg_walex - utility functions
 */
#include "pg_walex.h"

#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef PWE_WITH_LZ4
#include <lz4.h>
#endif
#ifdef PWE_WITH_ZSTD
#include <zstd.h>
#endif

void *
pwe_malloc(size_t size)
{
	void	   *p = malloc(size ? size : 1);

	if (!p)
	{
		fprintf(stderr, "pg_walex: out of memory (%zu bytes)\n", size);
		exit(1);
	}
	return p;
}

void *
pwe_calloc(size_t n, size_t size)
{
	void	   *p = calloc(n ? n : 1, size ? size : 1);

	if (!p)
	{
		fprintf(stderr, "pg_walex: out of memory\n");
		exit(1);
	}
	return p;
}

void *
pwe_realloc(void *p, size_t size)
{
	void	   *q = realloc(p, size ? size : 1);

	if (!q)
	{
		fprintf(stderr, "pg_walex: out of memory (%zu bytes)\n", size);
		exit(1);
	}
	return q;
}

char *
pwe_strdup(const char *s)
{
	size_t		len = s ? strlen(s) : 0;
	char	   *p = pwe_malloc(len + 1);

	if (len)
		memcpy(p, s, len);
	p[len] = '\0';
	return p;
}

char *
pwe_strndup(const char *s, size_t n)
{
	size_t		len = 0;
	char	   *p;

	while (len < n && s[len])
		len++;
	p = pwe_malloc(len + 1);
	memcpy(p, s, len);
	p[len] = '\0';
	return p;
}

/* ------------------------------------------------------------------ */
/* PweStr                                                             */
/* ------------------------------------------------------------------ */
void
pwe_str_init(PweStr *s)
{
	s->data = NULL;
	s->len = 0;
	s->cap = 0;
}

void
pwe_str_free(PweStr *s)
{
	free(s->data);
	s->data = NULL;
	s->len = s->cap = 0;
}

void
pwe_str_reset(PweStr *s)
{
	s->len = 0;
	if (s->data)
		s->data[0] = '\0';
}

static void
pwe_str_grow(PweStr *s, size_t need)
{
	size_t		cap;

	if (s->len + need + 1 <= s->cap)
		return;
	cap = s->cap ? s->cap : 128;
	while (cap < s->len + need + 1)
		cap *= 2;
	s->data = pwe_realloc(s->data, cap);
	s->cap = cap;
}

void
pwe_str_add(PweStr *s, const char *data, size_t len)
{
	if (len == 0)
		return;
	pwe_str_grow(s, len);
	memcpy(s->data + s->len, data, len);
	s->len += len;
	s->data[s->len] = '\0';
}

void
pwe_str_addc(PweStr *s, char c)
{
	pwe_str_grow(s, 1);
	s->data[s->len++] = c;
	s->data[s->len] = '\0';
}

void
pwe_str_adds(PweStr *s, const char *str)
{
	pwe_str_add(s, str, strlen(str));
}

void
pwe_str_addf(PweStr *s, const char *fmt,...)
{
	va_list		ap;
	char		buf[512];
	int			n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t) n < sizeof(buf))
	{
		pwe_str_add(s, buf, n);
		return;
	}
	{
		char	   *big = pwe_malloc((size_t) n + 1);

		va_start(ap, fmt);
		vsnprintf(big, (size_t) n + 1, fmt, ap);
		va_end(ap);
		pwe_str_add(s, big, n);
		free(big);
	}
}

/* SQL string literal; uses E'' when control bytes or backslashes appear */
void
pwe_str_add_quoted(PweStr *s, const char *val, size_t len)
{
	size_t		i;
	bool		escape = false;

	for (i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char) val[i];

		if (c < 0x20 || c == 0x7f || c == '\\')
		{
			escape = true;
			break;
		}
	}
	if (escape)
		pwe_str_addc(s, 'E');
	pwe_str_addc(s, '\'');
	for (i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char) val[i];

		if (c == '\'')
			pwe_str_adds(s, "''");
		else if (escape && c == '\\')
			pwe_str_adds(s, "\\\\");
		else if (escape && c == '\n')
			pwe_str_adds(s, "\\n");
		else if (escape && c == '\r')
			pwe_str_adds(s, "\\r");
		else if (escape && c == '\t')
			pwe_str_adds(s, "\\t");
		else if (escape && (c < 0x20 || c == 0x7f))
			pwe_str_addf(s, "\\x%02x", c);
		else
			pwe_str_addc(s, (char) c);
	}
	pwe_str_addc(s, '\'');
}

static bool
ident_needs_quotes(const char *ident)
{
	size_t		i;

	if (!ident[0])
		return true;
	if (!(ident[0] >= 'a' && ident[0] <= 'z') && ident[0] != '_')
		return true;
	for (i = 1; ident[i]; i++)
	{
		char		c = ident[i];

		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			  c == '_' || c == '$'))
			return true;
	}
	return false;
}

void
pwe_str_add_ident(PweStr *s, const char *ident)
{
	size_t		i;

	if (!ident_needs_quotes(ident))
	{
		pwe_str_adds(s, ident);
		return;
	}
	pwe_str_addc(s, '"');
	for (i = 0; ident[i]; i++)
	{
		if (ident[i] == '"')
			pwe_str_addc(s, '"');
		pwe_str_addc(s, ident[i]);
	}
	pwe_str_addc(s, '"');
}

void
pwe_str_add_json(PweStr *s, const char *val, size_t len)
{
	size_t		i;

	pwe_str_addc(s, '"');
	for (i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char) val[i];

		switch (c)
		{
			case '"':
				pwe_str_adds(s, "\\\"");
				break;
			case '\\':
				pwe_str_adds(s, "\\\\");
				break;
			case '\n':
				pwe_str_adds(s, "\\n");
				break;
			case '\r':
				pwe_str_adds(s, "\\r");
				break;
			case '\t':
				pwe_str_adds(s, "\\t");
				break;
			default:
				if (c < 0x20)
					pwe_str_addf(s, "\\u%04x", c);
				else
					pwe_str_addc(s, (char) c);
		}
	}
	pwe_str_addc(s, '"');
}

const char *
pwe_str_cstr(PweStr *s)
{
	return s->data ? s->data : "";
}

/* ------------------------------------------------------------------ */
/* hash map                                                           */
/* ------------------------------------------------------------------ */
static uint64
hash_mix(uint64 k)
{
	k ^= k >> 33;
	k *= 0xff51afd7ed558ccdULL;
	k ^= k >> 33;
	k *= 0xc4ceb9fe1a85ec53ULL;
	k ^= k >> 33;
	return k;
}

void
pwe_hash_init(PweHash *h)
{
	h->nbuckets = 64;
	h->count = 0;
	h->buckets = pwe_calloc(h->nbuckets, sizeof(PweHashEntry *));
}

void
pwe_hash_free(PweHash *h, void (*freeval) (void *))
{
	size_t		i;

	if (!h->buckets)
		return;
	for (i = 0; i < h->nbuckets; i++)
	{
		PweHashEntry *e = h->buckets[i];

		while (e)
		{
			PweHashEntry *next = e->next;

			if (freeval)
				freeval(e->val);
			free(e);
			e = next;
		}
	}
	free(h->buckets);
	h->buckets = NULL;
	h->count = 0;
	h->nbuckets = 0;
}

static void
hash_resize(PweHash *h)
{
	size_t		newn;
	PweHashEntry **nb;
	size_t		i;

	newn = h->nbuckets * 4;
	nb = pwe_calloc(newn, sizeof(PweHashEntry *));
	for (i = 0; i < h->nbuckets; i++)
	{
		PweHashEntry *e = h->buckets[i];

		while (e)
		{
			PweHashEntry *next = e->next;
			size_t		idx = hash_mix(e->key) & (newn - 1);

			e->next = nb[idx];
			nb[idx] = e;
			e = next;
		}
	}
	free(h->buckets);
	h->buckets = nb;
	h->nbuckets = newn;
}

void *
pwe_hash_put(PweHash *h, uint64 key, void *val)
{
	size_t		idx;
	PweHashEntry *e;

	if (h->count + 1 > h->nbuckets * 3 / 4)
		hash_resize(h);
	idx = hash_mix(key) & (h->nbuckets - 1);
	for (e = h->buckets[idx]; e; e = e->next)
	{
		if (e->key == key)
		{
			void	   *old = e->val;

			e->val = val;
			return old;
		}
	}
	e = pwe_malloc(sizeof(PweHashEntry));
	e->key = key;
	e->val = val;
	e->next = h->buckets[idx];
	h->buckets[idx] = e;
	h->count++;
	return NULL;
}

void *
pwe_hash_get(PweHash *h, uint64 key)
{
	size_t		idx;
	PweHashEntry *e;

	if (!h->buckets)
		return NULL;
	idx = hash_mix(key) & (h->nbuckets - 1);
	for (e = h->buckets[idx]; e; e = e->next)
		if (e->key == key)
			return e->val;
	return NULL;
}

void *
pwe_hash_remove(PweHash *h, uint64 key)
{
	size_t		idx;
	PweHashEntry *e,
			   *prev = NULL;

	if (!h->buckets)
		return NULL;
	idx = hash_mix(key) & (h->nbuckets - 1);
	for (e = h->buckets[idx]; e; prev = e, e = e->next)
	{
		if (e->key == key)
		{
			void	   *val = e->val;

			if (prev)
				prev->next = e->next;
			else
				h->buckets[idx] = e->next;
			free(e);
			h->count--;
			return val;
		}
	}
	return NULL;
}

void
pwe_hash_iterate(PweHash *h, void (*cb) (uint64 key, void *val, void *arg), void *arg)
{
	size_t		i;

	if (!h->buckets)
		return;
	for (i = 0; i < h->nbuckets; i++)
	{
		PweHashEntry *e = h->buckets[i];

		while (e)
		{
			PweHashEntry *next = e->next;

			cb(e->key, e->val, arg);
			e = next;
		}
	}
}

/* ------------------------------------------------------------------ */
/* CRC32C (Castagnoli)                                                */
/* ------------------------------------------------------------------ */
static uint32 crc32c_table[256];
static bool crc32c_ready = false;

static void
crc32c_init(void)
{
	uint32		i;

	for (i = 0; i < 256; i++)
	{
		uint32		crc = i;
		int			j;

		for (j = 0; j < 8; j++)
			crc = (crc >> 1) ^ (0x82F63B78u & (uint32) (-(int32) (crc & 1)));
		crc32c_table[i] = crc;
	}
	crc32c_ready = true;
}

uint32
pwe_crc32c(uint32 crc, const void *data, size_t len)
{
	const uint8 *p = data;

	if (!crc32c_ready)
		crc32c_init();
	crc = ~crc;
	while (len--)
		crc = crc32c_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

/* ------------------------------------------------------------------ */
/* pglz decompression (ported from PostgreSQL)                        */
/* ------------------------------------------------------------------ */
bool
pwe_pglz_decompress(const uint8 *src, size_t srclen, uint8 *dst, size_t dstlen)
{
	const uint8 *sp = src;
	const uint8 *srcend = src + srclen;
	uint8	   *dp = dst;
	uint8	   *destend = dst + dstlen;

	while (sp < srcend && dp < destend)
	{
		uint8		ctrl = *sp++;
		int			ctrlc;

		for (ctrlc = 0; ctrlc < 8 && sp < srcend && dp < destend; ctrlc++)
		{
			if (ctrl & 1)
			{
				int32		len;
				int32		off;

				if (sp + 2 > srcend)
					return false;
				len = (sp[0] & 0x0f) + 3;
				off = ((sp[0] & 0xf0) << 4) | sp[1];
				sp += 2;
				if (len == 18)
				{
					if (sp >= srcend)
						return false;
					len += *sp++;
				}
				if (off == 0 || (size_t) off > (size_t) (dp - dst))
					return false;
				if ((size_t) len > (size_t) (destend - dp))
					len = (int32) (destend - dp);
				while (off < len)
				{
					memcpy(dp, dp - off, off);
					len -= off;
					dp += off;
					off <<= 1;
				}
				memcpy(dp, dp - off, len);
				dp += len;
			}
			else
			{
				if (sp >= srcend)
					return false;
				*dp++ = *sp++;
			}
			ctrl >>= 1;
		}
	}
	return dp == destend;
}

bool
pwe_lz4_available(void)
{
#ifdef PWE_WITH_LZ4
	return true;
#else
	return false;
#endif
}

bool
pwe_zstd_available(void)
{
#ifdef PWE_WITH_ZSTD
	return true;
#else
	return false;
#endif
}

bool
pwe_lz4_decompress(const uint8 *src, size_t srclen, uint8 *dst, size_t dstlen)
{
#ifdef PWE_WITH_LZ4
	int			n = LZ4_decompress_safe((const char *) src, (char *) dst,
										(int) srclen, (int) dstlen);

	return n >= 0 && (size_t) n == dstlen;
#else
	(void) src;
	(void) srclen;
	(void) dst;
	(void) dstlen;
	return false;
#endif
}

bool
pwe_zstd_decompress(const uint8 *src, size_t srclen, uint8 *dst, size_t dstlen)
{
#ifdef PWE_WITH_ZSTD
	size_t		n = ZSTD_decompress(dst, dstlen, src, srclen);

	return !ZSTD_isError(n) && n == dstlen;
#else
	(void) src;
	(void) srclen;
	(void) dst;
	(void) dstlen;
	return false;
#endif
}

/* ------------------------------------------------------------------ */
/* LSN helpers                                                        */
/* ------------------------------------------------------------------ */
void
pwe_format_lsn(char *buf, size_t buflen, uint64 lsn)
{
	snprintf(buf, buflen, "%X/%X", (uint32) (lsn >> 32), (uint32) lsn);
}

bool
pwe_parse_lsn(const char *str, uint64 *lsn)
{
	uint64		hi = 0,
				lo = 0;
	const char *p = str;
	char	   *end;

	if (!str || !*str)
		return false;
	errno = 0;
	hi = strtoull(p, &end, 16);
	if (end == p || errno)
		return false;
	if (*end == '/')
	{
		p = end + 1;
		errno = 0;
		lo = strtoull(p, &end, 16);
		if (end == p || errno)
			return false;
	}
	else
	{
		lo = hi;
		hi = 0;
	}
	if (*end != '\0')
		return false;
	*lsn = (hi << 32) | (lo & 0xFFFFFFFFULL);
	return true;
}

void
pwe_write_u16(uint8 *p, uint16 v)
{
	p[0] = (uint8) (v & 0xff);
	p[1] = (uint8) ((v >> 8) & 0xff);
}

uint32
pwe_align8(uint32 x)
{
	return (x + 7u) & ~7u;
}

uint64
pwe_align8_64(uint64 x)
{
	return (x + 7ull) & ~7ull;
}

int
pwe_hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

char *
pwe_hex_encode(const uint8 *data, size_t len)
{
	static const char hex[] = "0123456789abcdef";
	char	   *out = pwe_malloc(len * 2 + 1);
	size_t		i;

	for (i = 0; i < len; i++)
	{
		out[i * 2] = hex[data[i] >> 4];
		out[i * 2 + 1] = hex[data[i] & 0x0f];
	}
	out[len * 2] = '\0';
	return out;
}

bool
pwe_parse_u64(const char *s, uint64 *out)
{
	char	   *end;
	unsigned long long v;

	if (!s || !*s)
		return false;
	errno = 0;
	v = strtoull(s, &end, 0);
	if (end == s || *end != '\0' || errno)
		return false;
	*out = (uint64) v;
	return true;
}

bool
pwe_parse_int(const char *s, int64 *out)
{
	char	   *end;
	long long	v;

	if (!s || !*s)
		return false;
	errno = 0;
	v = strtoll(s, &end, 0);
	if (end == s || *end != '\0' || errno)
		return false;
	*out = (int64) v;
	return true;
}

void
pwe_trim(char *s)
{
	size_t		len;
	char	   *p = s;

	while (*p && isspace((unsigned char) *p))
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	len = strlen(s);
	while (len > 0 && isspace((unsigned char) s[len - 1]))
		s[--len] = '\0';
}

/* Guess the data directory that owns a pg_wal path. */
bool
pwe_guess_pgdata(const char *wal_path, char *out, size_t outlen)
{
	char		buf[4096];
	char	   *slash;
	struct stat st;
	size_t		len;

	if (!wal_path || !wal_path[0])
		return false;
	snprintf(buf, sizeof(buf), "%s", wal_path);
	len = strlen(buf);
	while (len > 1 && buf[len - 1] == '/')
		buf[--len] = '\0';

	/* if this is a single 24-hex segment file, use its directory */
	slash = strrchr(buf, '/');
	if (slash)
	{
		const char *base = slash + 1;

		if (strlen(base) == 24)
		{
			int			i;
			bool		hex = true;

			for (i = 0; i < 24; i++)
				if (pwe_hexval(base[i]) < 0)
					hex = false;
			if (hex)
				*slash = '\0';
		}
	}

	/* strip the trailing "pg_wal" component */
	slash = strrchr(buf, '/');
	if (slash && strcmp(slash + 1, "pg_wal") == 0)
		*slash = '\0';
	else if (strcmp(buf, "pg_wal") == 0)
		return false;

	{
		char		basepath[4200];

		snprintf(basepath, sizeof(basepath), "%s/base", buf);
		if (stat(basepath, &st) == 0 && S_ISDIR(st.st_mode))
		{
			snprintf(out, outlen, "%s", buf);
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* WAL segment file listing                                           */
/* ------------------------------------------------------------------ */
static int
wal_file_cmp(const void *a, const void *b)
{
	const PweWalFile *fa = a;
	const PweWalFile *fb = b;

	if (fa->tli != fb->tli)
		return fa->tli < fb->tli ? -1 : 1;
	if (fa->segno != fb->segno)
		return fa->segno < fb->segno ? -1 : 1;
	return 0;
}

int
pwe_list_wal_files(const char *dir, PweWalFile **files, int *nfiles,
				   char *err, size_t errlen)
{
	DIR		   *d;
	struct dirent *de;
	PweWalFile *arr = NULL;
	int			n = 0,
				cap = 0;

	*nfiles = 0;
	*files = NULL;
	d = opendir(dir);
	if (!d)
	{
		snprintf(err, errlen, "cannot open WAL directory \"%s\": %s", dir, strerror(errno));
		return -1;
	}
	while ((de = readdir(d)) != NULL)
	{
		const char *name = de->d_name;
		size_t		len = strlen(name);
		uint32		tli;
		uint32		logid;
		uint32		segindex;
		uint64		segno;
		char		full[4096];
		struct stat st;
		int			i;
		bool		hexok = true;

		if (len != 24)
			continue;
		for (i = 0; i < 24; i++)
		{
			if (pwe_hexval(name[i]) < 0)
			{
				hexok = false;
				break;
			}
		}
		if (!hexok)
			continue;
		{
			char		tmp[9];

			memcpy(tmp, name, 8);
			tmp[8] = '\0';
			tli = (uint32) strtoul(tmp, NULL, 16);
			memcpy(tmp, name + 8, 8);
			tmp[8] = '\0';
			logid = (uint32) strtoul(tmp, NULL, 16);
			memcpy(tmp, name + 16, 8);
			tmp[8] = '\0';
			segindex = (uint32) strtoul(tmp, NULL, 16);
			segno = ((uint64) logid << 32) | segindex;
		}
		snprintf(full, sizeof(full), "%s/%s", dir, name);
		if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (n == cap)
		{
			cap = cap ? cap * 2 : 32;
			arr = pwe_realloc(arr, sizeof(PweWalFile) * cap);
		}
		arr[n].path = pwe_strdup(full);
		arr[n].name = pwe_strdup(name);
		arr[n].tli = tli;
		arr[n].logid = logid;
		arr[n].segindex = segindex;
		arr[n].segno = segno;
		arr[n].size = (uint64) st.st_size;
		n++;
	}
	closedir(d);
	if (n == 0)
	{
		snprintf(err, errlen, "no WAL segment files found in \"%s\"", dir);
		free(arr);
		return -1;
	}
	qsort(arr, n, sizeof(PweWalFile), wal_file_cmp);
	*files = arr;
	*nfiles = n;
	return 0;
}

void
pwe_free_wal_files(PweWalFile *files, int nfiles)
{
	int			i;

	for (i = 0; i < nfiles; i++)
	{
		free(files[i].path);
		free(files[i].name);
	}
	free(files);
}
