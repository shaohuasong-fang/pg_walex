/*
 * pg_walex - PostgreSQL WAL segment / record reader
 *
 * Independent re-implementation of the relevant parts of xlogreader.c,
 * supporting the on-disk formats of PostgreSQL 11 through 18.
 */
#include "pwe_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define PWE_SIZE_OF_XLOG_RECORD 24
#define PWE_XLP_FIRST_IS_CONTRECORD 0x0001
#define PWE_XLP_LONG_HEADER 0x0002
#define PWE_XLP_FIRST_IS_OVERWRITE_CONTRECORD 0x0008

struct PweReader
{
	PweReaderOpts opts;
	int			version;
	uint32		blocksize;
	uint32		segsize;
	PweWalFile *files;
	int			nfiles;
	int			tli;
	PweHash		segmap;			/* segno -> PweWalFile* */
	uint64		last_segno;
	bool		has_last_segno;
	uint64		pos;
	uint64		end_lsn;
	bool		started;
	bool		eof;
	bool		version_mismatch;
	char		cur_path[4096];
	int			cur_fd;
	uint8	   *page;
	uint8	   *recbuf;
	size_t		recbuf_cap;
	uint64		records_read;
};

static int
open_segment(PweReader *r, uint64 segno, char *err, size_t errlen)
{
	PweWalFile *f = pwe_hash_get(&r->segmap, segno);

	if (!f)
	{
		uint64		hi = segno;

		snprintf(err, errlen,
				 "WAL segment %08X%08X%08X not found (missing segment)",
				 r->tli, (uint32) (hi >> 32), (uint32) hi);
		return -1;
	}
	if (r->cur_fd >= 0 && strcmp(r->cur_path, f->path) == 0)
		return 0;
	if (r->cur_fd >= 0)
	{
		close(r->cur_fd);
		r->cur_fd = -1;
	}
	r->cur_fd = open(f->path, O_RDONLY);
	if (r->cur_fd < 0)
	{
		snprintf(err, errlen, "cannot open \"%s\": %s", f->path, strerror(errno));
		return -1;
	}
	strncpy(r->cur_path, f->path, sizeof(r->cur_path) - 1);
	r->cur_path[sizeof(r->cur_path) - 1] = '\0';
	return 0;
}

/* returns 0 ok, 1 short read (page partially zero), -1 error */
static int
read_page(PweReader *r, uint64 pageaddr, uint8 *buf, char *err, size_t errlen)
{
	uint64		segno = pageaddr / r->segsize;
	uint32		off = (uint32) (pageaddr % r->segsize);
	ssize_t		n;

	if (open_segment(r, segno, err, errlen) < 0)
		return -1;
	n = pread(r->cur_fd, buf, r->blocksize, (off_t) off);
	if (n < 0)
	{
		snprintf(err, errlen, "read error at %llu: %s",
				 (unsigned long long) pageaddr, strerror(errno));
		return -1;
	}
	if (n == 0)
		return 1;
	if ((uint32) n < r->blocksize)
	{
		memset(buf + n, 0, r->blocksize - n);
		return 1;
	}
	return 0;
}

static uint32
page_header_size(const uint8 *page)
{
	uint16		info = pwe_read_u16(page + 2);

	return (info & PWE_XLP_LONG_HEADER) ? 40 : 24;
}

static bool
page_valid(PweReader *r, const uint8 *page, uint64 pageaddr)
{
	const PweVersionInfo *vi = pwe_version_by_major(r->version);
	uint16		magic = pwe_read_u16(page);
	uint64		hdr_addr;

	if (!vi || magic != vi->page_magic)
		return false;
	hdr_addr = pwe_read_u64(page + 8);
	if (hdr_addr != pageaddr)
		return false;
	return true;
}

static void
ensure_recbuf(PweReader *r, size_t need)
{
	if (need <= r->recbuf_cap)
		return;
	r->recbuf_cap = need + 1024;
	r->recbuf = pwe_realloc(r->recbuf, r->recbuf_cap);
}

PweReader *
pwe_reader_open(const PweReaderOpts *opts, char *err, size_t errlen)
{
	PweReader  *r;
	PweWalFile *files = NULL;
	int			nfiles = 0;
	int			i;
	int			maxtli = 0;
	int			chosen;
	uint64		maxsize = 0;
	const PweVersionInfo *vi = NULL;

	memset(err, 0, errlen);
	r = pwe_calloc(1, sizeof(PweReader));
	r->opts = *opts;
	r->cur_fd = -1;
	r->blocksize = opts->blocksize ? opts->blocksize : PWE_BLCKSZ_DEFAULT;
	r->segsize = opts->segsize ? opts->segsize : PWE_SEGSIZE_DEFAULT;
	pwe_hash_init(&r->segmap);

	if (opts->wal_file)
	{
		struct stat st;
		const char *base = strrchr(opts->wal_file, '/');
		const char *name = base ? base + 1 : opts->wal_file;
		uint32		tli = 1;
		uint32		logid = 0;
		uint32		segindex = 0;
		uint64		segno = 0;
		char		tmp[9];

		if (stat(opts->wal_file, &st) != 0)
		{
			snprintf(err, errlen, "cannot stat \"%s\": %s", opts->wal_file, strerror(errno));
			goto fail;
		}
		if (strlen(name) == 24)
		{
			int			j;
			bool		hex = true;

			for (j = 0; j < 24; j++)
				if (pwe_hexval(name[j]) < 0)
					hex = false;
			if (hex)
			{
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
		}
		files = pwe_calloc(1, sizeof(PweWalFile));
		files[0].path = pwe_strdup(opts->wal_file);
		files[0].name = pwe_strdup(name);
		files[0].tli = tli;
		files[0].logid = logid;
		files[0].segindex = segindex;
		files[0].segno = segno;
		files[0].size = (uint64) st.st_size;
		nfiles = 1;
		r->tli = (int) tli;
	}
	else
	{
		if (!opts->wal_dir)
		{
			snprintf(err, errlen, "no WAL directory specified");
			goto fail;
		}
		if (pwe_list_wal_files(opts->wal_dir, &files, &nfiles, err, errlen) < 0)
			goto fail;
		for (i = 0; i < nfiles; i++)
		{
			if ((int) files[i].tli > maxtli)
				maxtli = (int) files[i].tli;
			if (files[i].size > maxsize)
				maxsize = files[i].size;
		}
		if (maxsize > 0 && maxsize % (1024 * 1024) == 0 &&
			opts->segsize == 0 && maxsize >= 1024 * 1024)
			r->segsize = (uint32) maxsize;
		chosen = opts->timeline ? opts->timeline : maxtli;
		r->tli = chosen;
	}
	r->files = files;
	r->nfiles = nfiles;

	/* build segment map for chosen timeline */
	{
		int			count = 0;

		for (i = 0; i < nfiles; i++)
		{
			if ((int) files[i].tli != r->tli)
				continue;
			pwe_hash_put(&r->segmap, files[i].segno, &files[i]);
			count++;
		}
		if (count == 0)
		{
			snprintf(err, errlen, "no WAL segments for timeline %d", r->tli);
			goto fail;
		}
	}

	/* remember highest segment */
	for (i = 0; i < nfiles; i++)
	{
		if ((int) files[i].tli != r->tli)
			continue;
		if (!r->has_last_segno || files[i].segno > r->last_segno)
		{
			r->last_segno = files[i].segno;
			r->has_last_segno = true;
		}
	}

	/* determine version / block size from the first page */
	{
		uint64		first_segno = 0;
		bool		have_first = false;
		uint64		first_page;
		uint16		magic;

		for (i = 0; i < nfiles; i++)
		{
			if ((int) files[i].tli != r->tli)
				continue;
			if (!have_first || files[i].segno < first_segno)
			{
				first_segno = files[i].segno;
				have_first = true;
			}
		}
		first_page = first_segno * r->segsize;
		r->page = pwe_malloc(r->blocksize);
		if (read_page(r, first_page, r->page, err, errlen) < 0)
			goto fail;
		magic = pwe_read_u16(r->page);
		vi = pwe_version_by_magic(magic);
		if (opts->version)
		{
			const PweVersionInfo *want = pwe_version_by_major(opts->version);

			if (!want)
			{
				snprintf(err, errlen, "unsupported PostgreSQL version %d (expected 11..18)",
						 opts->version);
				goto fail;
			}
			r->version = opts->version;
			if (vi && vi->major != opts->version)
				r->version_mismatch = true;
		}
		else if (vi)
			r->version = vi->major;
		else
		{
			snprintf(err, errlen,
					 "unknown WAL page magic 0x%04X; use --version to force a major version",
					 magic);
			goto fail;
		}
		if (page_header_size(r->page) == 40)
		{
			uint32		seg = pwe_read_u32(r->page + 32);
			uint32		blk = pwe_read_u32(r->page + 36);

			if (blk >= 1024 && blk <= 65536 && (blk & (blk - 1)) == 0)
				r->blocksize = blk;
			if (seg >= 1024 * 1024 && seg <= (1u << 30))
				r->segsize = seg;
		}
		if (r->blocksize != PWE_BLCKSZ_DEFAULT && !opts->blocksize)
			free(r->page), r->page = pwe_malloc(r->blocksize);
	}

	/* convert names (log/seg) to linear segment numbers for this segsize */
	{
		uint64		per = (uint64) 0x100000000ULL / r->segsize;

		if (per == 0)
			per = 1;
		pwe_hash_free(&r->segmap, NULL);
		pwe_hash_init(&r->segmap);
		r->last_segno = 0;
		r->has_last_segno = false;
		for (i = 0; i < nfiles; i++)
		{
			if ((int) files[i].tli != r->tli)
				continue;
			files[i].segno = (uint64) files[i].logid * per + files[i].segindex;
			pwe_hash_put(&r->segmap, files[i].segno, &files[i]);
			if (!r->has_last_segno || files[i].segno > r->last_segno)
			{
				r->last_segno = files[i].segno;
				r->has_last_segno = true;
			}
		}
	}

	if (opts->start_lsn)
	{
		uint64		pageaddr = opts->start_lsn - (opts->start_lsn % r->blocksize);

		/* start scanning at the page containing the requested LSN */
		r->pos = pageaddr;
	}
	else
	{
		uint64		first_segno = 0;
		bool		have_first = false;

		for (i = 0; i < nfiles; i++)
		{
			if ((int) files[i].tli != r->tli)
				continue;
			if (!have_first || files[i].segno < first_segno)
			{
				first_segno = files[i].segno;
				have_first = true;
			}
		}
		r->pos = first_segno * r->segsize;
	}
	r->end_lsn = opts->end_lsn;
	r->started = false;
	r->eof = false;
	return r;

fail:
	if (files)
		pwe_free_wal_files(files, nfiles);
	pwe_hash_free(&r->segmap, NULL);
	free(r->page);
	free(r);
	return NULL;
}

int
pwe_reader_version(PweReader *r)
{
	return r->version;
}

uint32
pwe_reader_blocksize(PweReader *r)
{
	return r->blocksize;
}

uint32
pwe_reader_segsize(PweReader *r)
{
	return r->segsize;
}

uint64
pwe_reader_position(PweReader *r)
{
	return r->pos;
}

bool
pwe_reader_version_mismatch(PweReader *r)
{
	return r->version_mismatch;
}

/* read a page into r->page; 0 = ok, 1 = empty page (end of WAL), -1 = error */
static int
fetch_page(PweReader *r, uint64 pageaddr, char *err, size_t errlen)
{
	int			rc = read_page(r, pageaddr, r->page, err, errlen);

	if (rc < 0)
		return -1;
	if (pwe_read_u16(r->page) == 0)
		return 1;				/* zero-filled page: end of WAL */
	if (!page_valid(r, r->page, pageaddr))
	{
		const PweVersionInfo *vi = pwe_version_by_major(r->version);
		uint16		magic = pwe_read_u16(r->page);

		if (vi && magic == vi->page_magic)
		{
			/*
			 * A recycled/stale page (valid magic but an address from a
			 * previous WAL incarnation) marks the end of the usable WAL.
			 */
			return 1;
		}
		if (vi && magic != vi->page_magic)
			snprintf(err, errlen,
					 "invalid WAL page magic 0x%04X at %X/%X (expected 0x%04X for PostgreSQL %d)",
					 magic, (uint32) (pageaddr >> 32), (uint32) pageaddr,
					 vi->page_magic, r->version);
		else
			snprintf(err, errlen, "invalid WAL page at %X/%X (page address mismatch)",
					 (uint32) (pageaddr >> 32), (uint32) pageaddr);
		return -1;
	}
	return 0;
}

static void
decode_block_headers(PweReader *r, PweRecord *rec, char *err, size_t errlen)
{
	uint8	   *ptr;
	uint32		datatotal = 0;
	uint8	   *cur;
	int			i;
	int			max_block_id = -1;

	memset(rec->blocks, 0, sizeof(rec->blocks));
	rec->nblocks = 0;
	rec->main_data = NULL;
	rec->main_len = 0;
	rec->origin = 0;
	rec->toplevel_xid = 0;

	ptr = rec->buf + PWE_SIZE_OF_XLOG_RECORD;
	while ((uint32) (ptr - rec->buf) + datatotal < rec->tot_len)
	{
		uint8		block_id;

		block_id = *ptr++;
		if (block_id == 255)	/* short main data */
		{
			uint8		main_len;

			if (ptr + 1 > rec->buf + rec->tot_len)
				goto shortdata;
			main_len = *ptr++;
			rec->main_len = main_len;
			datatotal += main_len;
			break;
		}
		else if (block_id == 254)	/* long main data */
		{
			uint32		main_len;

			if (ptr + 4 > rec->buf + rec->tot_len)
				goto shortdata;
			main_len = pwe_read_u32(ptr);
			ptr += 4;
			rec->main_len = main_len;
			datatotal += main_len;
			break;
		}
		else if (block_id == 253)	/* origin */
		{
			if (ptr + 4 > rec->buf + rec->tot_len)
				goto shortdata;
			rec->origin = pwe_read_u32(ptr);
			ptr += 4;
		}
		else if (block_id == 252)	/* top-level xid (PG16+) */
		{
			if (ptr + 4 > rec->buf + rec->tot_len)
				goto shortdata;
			rec->toplevel_xid = pwe_read_u32(ptr);
			ptr += 4;
		}
		else if (block_id <= PWE_MAX_BLOCK_ID)
		{
			PweBlock   *blk = &rec->blocks[block_id];
			uint8		fork_flags;

			if (ptr + 4 > rec->buf + rec->tot_len)
				goto shortdata;
			fork_flags = *ptr++;
			blk->in_use = true;
			blk->forknum = fork_flags & 0x0F;
			blk->has_image = (fork_flags & 0x10) != 0;
			blk->has_data = (fork_flags & 0x20) != 0;
			blk->will_init = (fork_flags & 0x40) != 0;
			blk->data_len = pwe_read_u16(ptr);
			ptr += 2;
			datatotal += blk->data_len;
			if (blk->has_image)
			{
				uint16		bimg_len;
				uint16		hole_offset;
				uint8		bimg_info;
				bool		compressed;

				if (ptr + 5 > rec->buf + rec->tot_len)
					goto shortdata;
				bimg_len = pwe_read_u16(ptr);
				hole_offset = pwe_read_u16(ptr + 2);
				bimg_info = ptr[4];
				ptr += 5;
				blk->bimg_info = bimg_info;
				blk->image_len = bimg_len;
				blk->hole_offset = hole_offset;
				if (r->version < 15)
				{
					compressed = (bimg_info & 0x02) != 0;
					blk->apply_image = (bimg_info & 0x04) != 0;
				}
				else
				{
					compressed = (bimg_info & (0x04 | 0x08 | 0x10)) != 0;
					blk->apply_image = (bimg_info & 0x02) != 0;
				}
				if (compressed)
				{
					if (bimg_info & 0x01)
					{
						if (ptr + 2 > rec->buf + rec->tot_len)
							goto shortdata;
						blk->hole_length = pwe_read_u16(ptr);
						ptr += 2;
					}
					else
						blk->hole_length = 0;
				}
				else
					blk->hole_length = (uint16) (r->blocksize - bimg_len);
				datatotal += bimg_len;
			}
			if (!(fork_flags & 0x80))
			{
				if (ptr + 12 > rec->buf + rec->tot_len)
					goto shortdata;
				blk->spc = pwe_read_u32(ptr);
				blk->db = pwe_read_u32(ptr + 4);
				blk->rel = pwe_read_u32(ptr + 8);
				ptr += 12;
			}
			else
			{
				int			j;
				bool		found = false;

				for (j = block_id - 1; j >= 0; j--)
				{
					if (rec->blocks[j].in_use)
					{
						blk->spc = rec->blocks[j].spc;
						blk->db = rec->blocks[j].db;
						blk->rel = rec->blocks[j].rel;
						found = true;
						break;
					}
				}
				if (!found)
				{
					snprintf(err, errlen, "BKPBLOCK_SAME_REL without previous relation");
					return;
				}
			}
			if (ptr + 4 > rec->buf + rec->tot_len)
				goto shortdata;
			blk->blkno = pwe_read_u32(ptr);
			ptr += 4;
			if (block_id > max_block_id)
				max_block_id = block_id;
		}
		else
		{
			snprintf(err, errlen, "invalid block id %u in WAL record", block_id);
			return;
		}
	}
	if ((uint64) (ptr - rec->buf) + datatotal != rec->tot_len)
	{
		snprintf(err, errlen,
				 "WAL record length mismatch at %X/%X (headers %u, data %u, total %u, rmid %u, info %02X)",
				 (uint32) (rec->lsn >> 32), (uint32) rec->lsn,
				 (unsigned) (ptr - rec->buf), datatotal, rec->tot_len,
				 rec->rmid, rec->info);
		return;
	}
	rec->nblocks = max_block_id + 1;

	/* lay out block data */
	cur = ptr;
	for (i = 0; i <= max_block_id; i++)
	{
		PweBlock   *blk = &rec->blocks[i];

		if (!blk->in_use)
			continue;
		if (blk->has_image)
		{
			blk->image = cur;
			cur += blk->image_len;
		}
		if (blk->has_data)
		{
			blk->data = cur;
			cur += blk->data_len;
		}
	}
	rec->main_data = cur;

	return;

shortdata:
	snprintf(err, errlen, "WAL record is truncated (short data)");
}

/*
 * Assemble a record starting at absolute position pos, spanning pages.
 * Returns 1 on success, 0 on EOF, -1 on error.
 */
static int
read_record_bytes(PweReader *r, uint64 pos, PweRecord *rec, char *err, size_t errlen)
{
	uint64		pageaddr;
	uint32		off;
	uint32		phsize;
	uint32		avail;
	uint32		tot_len;
	uint32		got;
	uint64		last_pageaddr;
	uint32		last_rem_len = 0;
	uint64		recstart;

	rec->lsn = pos;
	pageaddr = pos - (pos % r->blocksize);
	off = (uint32) (pos % r->blocksize);
	{
		int			prc = fetch_page(r, pageaddr, err, errlen);

		if (prc < 0)
			return -1;
		if (prc > 0)
			return 0;
	}
	phsize = page_header_size(r->page);
	if (off < phsize)
		off = phsize;
	recstart = pageaddr + off;
	rec->lsn = recstart;
	if ((pwe_read_u16(r->page + 2) & PWE_XLP_FIRST_IS_CONTRECORD) && off == phsize)
	{
		/* continuation of a record that started before our start point */
		uint32		rem = pwe_read_u32(r->page + 16);

		rec->lsn = pageaddr + phsize + pwe_align8(rem);
		r->pos = rec->lsn;
		return 2;				/* skip the partial record, resume after it */
	}
	if (off + 4 > r->blocksize)
	{
		rec->lsn = pageaddr + r->blocksize;
		r->pos = rec->lsn;
		return 2;
	}
	avail = r->blocksize - off;
	tot_len = pwe_read_u32(r->page + off);
	if (tot_len == 0 || (tot_len & 0xFF000000) != 0)
	{
		/* zero padding at end of page: move to next page */
		rec->lsn = pageaddr + r->blocksize;
		r->pos = rec->lsn;
		return 2;
	}
	if (tot_len < PWE_SIZE_OF_XLOG_RECORD)
	{
		snprintf(err, errlen,
				 "invalid record length %u at %X/%X (minimum %d)",
				 tot_len, (uint32) (recstart >> 32), (uint32) recstart,
				 PWE_SIZE_OF_XLOG_RECORD);
		return -1;
	}
	ensure_recbuf(r, tot_len);
	if (tot_len <= avail)
	{
		memcpy(r->recbuf, r->page + off, tot_len);
		r->pos = recstart + pwe_align8(tot_len);
		rec->next_lsn = r->pos;
	}
	else
	{
		uint32		remaining = tot_len;

		got = avail;
		memcpy(r->recbuf, r->page + off, avail);
		remaining -= avail;
		last_pageaddr = pageaddr;
		while (remaining > 0)
		{
			uint64		nextpage = pageaddr + r->blocksize;
			uint32		ph2;
			uint32		copy;

			{
				int			prc = fetch_page(r, nextpage, err, errlen);

				if (prc < 0)
					return -1;
				if (prc > 0)
				{
					snprintf(err, errlen, "WAL ends in the middle of a record");
					return -1;
				}
			}
			if (!(r->page[2] & PWE_XLP_FIRST_IS_CONTRECORD))
			{
				snprintf(err, errlen,
						 "missing contrecord flag at %X/%X while assembling record at %X/%X",
						 (uint32) (nextpage >> 32), (uint32) nextpage,
						 (uint32) (recstart >> 32), (uint32) recstart);
				return -1;
			}
			ph2 = page_header_size(r->page);
			last_rem_len = pwe_read_u32(r->page + 16);
			if (last_rem_len != remaining)
			{
				snprintf(err, errlen,
						 "invalid contrecord length %u (expected %u) at %X/%X",
						 last_rem_len, remaining,
						 (uint32) (nextpage >> 32), (uint32) nextpage);
				return -1;
			}
			copy = r->blocksize - ph2;
			if (copy > remaining)
				copy = remaining;
			memcpy(r->recbuf + got, r->page + ph2, copy);
			got += copy;
			remaining -= copy;
			pageaddr = nextpage;
			last_pageaddr = nextpage;
		}
		r->pos = last_pageaddr + page_header_size(r->page) + pwe_align8(last_rem_len);
		rec->next_lsn = r->pos;
	}

	rec->tot_len = tot_len;
	rec->buf = r->recbuf;
	rec->buf_len = tot_len;
	rec->xid = pwe_read_u32(r->recbuf + 4);
	rec->prev_lsn = pwe_read_u64(r->recbuf + 8);
	rec->info = r->recbuf[16];
	rec->rmid = r->recbuf[17];

	if (r->opts.verify_crc)
	{
		uint32		stored = pwe_read_u32(r->recbuf + 20);
		uint32		crc = 0;

		crc = pwe_crc32c(crc, r->recbuf + PWE_SIZE_OF_XLOG_RECORD,
						 tot_len - PWE_SIZE_OF_XLOG_RECORD);
		crc = pwe_crc32c(crc, r->recbuf, 20);
		if (crc != stored)
		{
			snprintf(err, errlen,
					 "WAL record CRC mismatch at %X/%X (stored %08X, computed %08X)",
					 (uint32) (recstart >> 32), (uint32) recstart, stored, crc);
			return -1;
		}
	}
	return 1;
}

int
pwe_reader_next(PweReader *r, PweRecord *rec, char *err, size_t errlen)
{
	memset(rec, 0, sizeof(*rec));
	while (!r->eof)
	{
		int			rc;
		uint64		nextpos;

		if (r->end_lsn && r->pos >= r->end_lsn)
			return 0;
		if (r->pos / r->segsize > r->last_segno)
			return 0;

		rc = read_record_bytes(r, r->pos, rec, err, errlen);
		if (rc == 0)
			return 0;
		if (rc == 2)
		{
			rec->lsn = r->pos;
			continue;
		}
		if (rc < 0)
			return -1;
		/* handle XLOG_SWITCH: continue at start of next segment */
		nextpos = r->pos;
		if (rec->rmid == 0 && (rec->info & 0xF0) == 0x40)
			nextpos = (rec->lsn / r->segsize + 1) * r->segsize;
		rec->next_lsn = nextpos;
		r->pos = nextpos;
		r->records_read++;

		/* skip records entirely before start_lsn */
		if (r->opts.start_lsn && rec->next_lsn <= r->opts.start_lsn &&
			rec->lsn < r->opts.start_lsn)
			continue;

		decode_block_headers(r, rec, err, errlen);
		if (err[0])
			return -1;
		return 1;
	}
	return 0;
}

void
pwe_reader_close(PweReader *r)
{
	if (!r)
		return;
	if (r->cur_fd >= 0)
		close(r->cur_fd);
	pwe_hash_free(&r->segmap, NULL);
	if (r->files)
		pwe_free_wal_files(r->files, r->nfiles);
	free(r->page);
	free(r->recbuf);
	free(r);
}
