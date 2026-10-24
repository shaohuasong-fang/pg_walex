/*
 * pg_walex - embedded RESTful API server
 */
#include "pwe_internal.h"

#include <ctype.h>
#include <errno.h>
#include <arpa/inet.h>
#include <strings.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define PWE_HTTP_MAX_REQ (16u * 1024u * 1024u)

typedef struct PweApiParams
{
	const char *dir;
	const char *file;
	int			version;
	uint64		start_lsn;
	uint64		end_lsn;
	uint64		limit;
	uint64		xid;
	uint32		db_oid;
	char		table[256];
	bool		has_table;
	bool		comments;
	bool		non_sql;
	bool		logical;
	bool		aborted;
	bool		no_crc;
	char		pgdata[1024];
	const char *format;			/* sql | json | stats */
} PweApiParams;

static void
params_init(PweApiParams *p, const PweHttpOptions *opts)
{
	memset(p, 0, sizeof(*p));
	p->dir = opts->default_wal_dir;
	p->version = opts->default_version;
	p->comments = true;
	p->format = "json";
}

static void
url_decode(char *s)
{
	char	   *r = s;
	char	   *w = s;

	while (*r)
	{
		if (*r == '%' && pwe_hexval(r[1]) >= 0 && pwe_hexval(r[2]) >= 0)
		{
			*w++ = (char) ((pwe_hexval(r[1]) << 4) | pwe_hexval(r[2]));
			r += 3;
		}
		else if (*r == '+')
		{
			*w++ = ' ';
			r++;
		}
		else
			*w++ = *r++;
	}
	*w = '\0';
}

static void
query_param(const char *query, const char *key, char *out, size_t outlen)
{
	const char *p = query;
	size_t		klen = strlen(key);

	out[0] = '\0';
	if (!query)
		return;
	while (*p)
	{
		const char *amp = strchr(p, '&');
		const char *eq = strchr(p, '=');

		if (eq && (size_t) (eq - p) == klen && strncmp(p, key, klen) == 0)
		{
			size_t		n = amp ? (size_t) (amp - eq - 1) : strlen(eq + 1);

			if (n >= outlen)
				n = outlen - 1;
			memcpy(out, eq + 1, n);
			out[n] = '\0';
			url_decode(out);
			return;
		}
		if (!amp)
			break;
		p = amp + 1;
	}
}

static void
params_from_query(PweApiParams *p, const char *query)
{
	char		buf[1024];

	query_param(query, "dir", buf, sizeof(buf));
	if (buf[0])
		p->dir = pwe_strdup(buf);
	query_param(query, "file", buf, sizeof(buf));
	if (buf[0])
		p->file = pwe_strdup(buf);
	query_param(query, "version", buf, sizeof(buf));
	if (buf[0])
	{
		int64		v;

		if (pwe_parse_int(buf, &v) && v >= 11 && v <= 18)
			p->version = (int) v;
	}
	query_param(query, "start_lsn", buf, sizeof(buf));
	if (buf[0])
		pwe_parse_lsn(buf, &p->start_lsn);
	query_param(query, "end_lsn", buf, sizeof(buf));
	if (buf[0])
		pwe_parse_lsn(buf, &p->end_lsn);
	query_param(query, "limit", buf, sizeof(buf));
	if (buf[0])
		pwe_parse_u64(buf, &p->limit);
	query_param(query, "xid", buf, sizeof(buf));
	if (buf[0])
		pwe_parse_u64(buf, &p->xid);
	query_param(query, "pgdata", p->pgdata, sizeof(p->pgdata));
	query_param(query, "db_oid", buf, sizeof(buf));
	if (buf[0])
	{
		uint64		v;

		if (pwe_parse_u64(buf, &v))
			p->db_oid = (uint32) v;
	}
	query_param(query, "table", p->table, sizeof(p->table));
	if (p->table[0])
		p->has_table = true;
	query_param(query, "format", buf, sizeof(buf));
	if (buf[0])
	{
		if (strcmp(buf, "sql") == 0)
			p->format = "sql";
		else if (strcmp(buf, "stats") == 0)
			p->format = "stats";
		else
			p->format = "json";
	}
	query_param(query, "comments", buf, sizeof(buf));
	if (buf[0])
		p->comments = (strcmp(buf, "0") != 0 && strcmp(buf, "false") != 0);
	query_param(query, "non_sql", buf, sizeof(buf));
	if (buf[0])
		p->non_sql = (strcmp(buf, "0") != 0 && strcmp(buf, "false") != 0);
	query_param(query, "logical_messages", buf, sizeof(buf));
	if (buf[0])
		p->logical = (strcmp(buf, "0") != 0 && strcmp(buf, "false") != 0);
}

/* --- minimal JSON body reader (flat object) ----------------------- */
static bool
json_find(const char *body, const char *key, const char **val)
{
	char		pattern[128];
	char	   *p;

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr((char *) body, pattern);
	if (!p)
		return false;
	p = strchr(p + strlen(pattern), ':');
	if (!p)
		return false;
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	*val = p;
	return true;
}

static bool
json_get_str(const char *body, const char *key, char *out, size_t outlen)
{
	const char *v;
	size_t		n = 0;

	out[0] = '\0';
	if (!json_find(body, key, &v))
		return false;
	if (*v == '"')
	{
		v++;
		while (*v && *v != '"' && n + 1 < outlen)
		{
			if (*v == '\\' && v[1])
			{
				v++;
				switch (*v)
				{
					case 'n':
						out[n++] = '\n';
						break;
					case 't':
						out[n++] = '\t';
						break;
					case 'r':
						out[n++] = '\r';
						break;
					default:
						out[n++] = *v;
						break;
				}
				v++;
			}
			else
				out[n++] = *v++;
		}
	}
	else
	{
		while (*v && *v != ',' && *v != '}' && *v != '\n' && n + 1 < outlen)
			out[n++] = *v++;
		while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\r'))
			n--;
	}
	out[n] = '\0';
	return true;
}

static bool
json_get_uint(const char *body, const char *key, uint64 *out)
{
	const char *v;
	char		buf[64];
	size_t		n = 0;

	if (!json_find(body, key, &v))
		return false;
	if (*v == '"')
		v++;
	while (*v && (isalnum((unsigned char) *v) || *v == '/' || *v == '.') &&
		   n + 1 < sizeof(buf))
		buf[n++] = *v++;
	buf[n] = '\0';
	if (strcmp(key, "start_lsn") == 0 || strcmp(key, "end_lsn") == 0)
		return pwe_parse_lsn(buf, out);
	return pwe_parse_u64(buf, out);
}

static bool
json_get_bool(const char *body, const char *key, bool *out)
{
	const char *v;

	if (!json_find(body, key, &v))
		return false;
	*out = (strncmp(v, "true", 4) == 0 || *v == '1');
	return true;
}

static void
params_from_json(PweApiParams *p, const char *body)
{
	char		buf[1024];
	uint64		u;
	bool		b;

	if (json_get_str(body, "dir", buf, sizeof(buf)) && buf[0])
		p->dir = pwe_strdup(buf);
	if (json_get_str(body, "file", buf, sizeof(buf)) && buf[0])
		p->file = pwe_strdup(buf);
	if ((json_get_str(body, "version", buf, sizeof(buf)) ||
		 json_get_str(body, "pg_version", buf, sizeof(buf))) && buf[0])
	{
		int64		v;

		if (pwe_parse_int(buf, &v) && v >= 11 && v <= 18)
			p->version = (int) v;
	}
	else if (json_get_uint(body, "version", &u) && u >= 11 && u <= 18)
		p->version = (int) u;
	if (json_get_uint(body, "start_lsn", &u))
		p->start_lsn = u;
	if (json_get_uint(body, "end_lsn", &u))
		p->end_lsn = u;
	if (json_get_uint(body, "limit", &u))
		p->limit = u;
	if (json_get_uint(body, "xid", &u))
		p->xid = u;
	if (json_get_uint(body, "db_oid", &u) ||
		json_get_uint(body, "database_oid", &u))
		p->db_oid = (uint32) u;
	if (json_get_str(body, "pgdata", buf, sizeof(buf)) && buf[0])
		snprintf(p->pgdata, sizeof(p->pgdata), "%s", buf);
	if (json_get_str(body, "table", buf, sizeof(buf)) && buf[0])
	{
		snprintf(p->table, sizeof(p->table), "%s", buf);
		p->has_table = true;
	}
	if (json_get_str(body, "format", buf, sizeof(buf)) && buf[0])
	{
		if (strcmp(buf, "sql") == 0)
			p->format = "sql";
		else if (strcmp(buf, "stats") == 0)
			p->format = "stats";
		else
			p->format = "json";
	}
	if (json_get_bool(body, "comments", &b))
		p->comments = b;
	if (json_get_bool(body, "non_sql", &b))
		p->non_sql = b;
	if (json_get_bool(body, "logical_messages", &b))
		p->logical = b;
}

/* --- reconstruction ------------------------------------------------- */
static int
run_reconstruction(PweApiParams *p, FILE *out, PweStats *stats, char *err, size_t errlen)
{
	PweReaderOpts ropts;
	PweSqlOptions sopts;
	PweReader  *reader;
	PweReconstructor *recon;
	PweRecord	rec;
	int			rc;
	uint64		count = 0;

	memset(&ropts, 0, sizeof(ropts));
	memset(&sopts, 0, sizeof(sopts));
	ropts.wal_dir = p->dir;
	ropts.wal_file = p->file;
	ropts.version = p->version;
	ropts.start_lsn = 0;	/* warm up the catalog, emit from p->start_lsn */
	ropts.end_lsn = p->end_lsn;
	ropts.verify_crc = !p->no_crc;
	sopts.include_comments = p->comments;
	sopts.include_non_sql = p->non_sql;
	sopts.include_aborted = p->aborted;
	sopts.logical_messages = p->logical;
	sopts.only_xid = (uint32) p->xid;
	sopts.only_database = p->db_oid;
	sopts.emit_connect = (p->db_oid == 0);
	{
		char		guess[4096];

		if (p->pgdata[0])
			sopts.pgdata = p->pgdata;
		else if (p->dir && pwe_guess_pgdata(p->dir, guess, sizeof(guess)))
			sopts.pgdata = pwe_strdup(guess);
	}
	sopts.max_row_cache = 200000;
	sopts.limit = (uint32) p->limit;
	sopts.start_lsn = p->start_lsn;
	if (p->has_table)
		sopts.only_table = p->table;

	reader = pwe_reader_open(&ropts, err, errlen);
	if (!reader)
		return -1;
	recon = pwe_recon_create(pwe_reader_version(reader), &sopts, out, stats);
	while ((rc = pwe_reader_next(reader, &rec, err, errlen)) == 1)
	{
		count++;
		pwe_recon_process(recon, &rec);
		if (p->limit && count >= p->limit)
			break;
		memset(err, 0, errlen);
	}
	pwe_recon_finish(recon);
	pwe_recon_free(recon);
	pwe_reader_close(reader);
	if (rc < 0)
		return -1;
	return 0;
}

/* --- HTTP plumbing -------------------------------------------------- */
static void
send_all(int fd, const char *data, size_t len)
{
	while (len > 0)
	{
		ssize_t		n = send(fd, data, len, 0);

		if (n <= 0)
			return;
		data += n;
		len -= (size_t) n;
	}
}

static void
send_response(int fd, int status, const char *content_type, const char *body, size_t len)
{
	char		header[512];
	int			n;

	n = snprintf(header, sizeof(header),
				 "HTTP/1.1 %d %s\r\n"
				 "Content-Type: %s\r\n"
				 "Content-Length: %zu\r\n"
				 "Connection: close\r\n"
				 "Access-Control-Allow-Origin: *\r\n"
				 "Access-Control-Allow-Headers: Authorization, Content-Type\r\n"
				 "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n\r\n",
				 status,
				 status == 200 ? "OK" : (status == 400 ? "Bad Request" :
										 (status == 401 ? "Unauthorized" :
										  (status == 404 ? "Not Found" : "Internal Server Error"))),
				 content_type, len);
	send_all(fd, header, (size_t) n);
	send_all(fd, body, len);
}

static void
send_json(int fd, int status, PweStr *data)
{
	PweStr		body;

	pwe_str_init(&body);
	pwe_str_adds(&body, "{\"code\":");
	pwe_str_addf(&body, "%d", status == 200 ? 0 : 1);
	pwe_str_adds(&body, ",\"error_code\":");
	if (status == 200)
		pwe_str_adds(&body, "\"\"");
	else
		pwe_str_adds(&body, "\"ERROR\"");
	pwe_str_adds(&body, ",\"error_message\":\"\",\"data\":");
	if (data && data->len)
		pwe_str_add(&body, data->data, data->len);
	else
		pwe_str_adds(&body, "null");
	pwe_str_adds(&body, "}");
	send_response(fd, status, "application/json; charset=utf-8", body.data, body.len);
	pwe_str_free(&body);
}

static void
send_error(int fd, int status, const char *msg)
{
	PweStr		body;

	pwe_str_init(&body);
	pwe_str_adds(&body, "{\"code\":1,\"error_code\":\"ERROR\",\"error_message\":");
	pwe_str_add_json(&body, msg, strlen(msg));
	pwe_str_adds(&body, ",\"data\":null}");
	send_response(fd, status, "application/json; charset=utf-8", body.data, body.len);
	pwe_str_free(&body);
}

static void
stats_json(PweStats *st, PweStr *out)
{
	int			i;

	pwe_str_addf(out, "{\"records\":%llu,\"transactions\":%llu,\"statements\":%llu,"
				 "\"insert\":%llu,\"update\":%llu,\"delete\":%llu,\"truncate\":%llu,"
				 "\"ddl\":%llu,\"first_lsn\":\"%X/%X\",\"last_lsn\":\"%X/%X\",\"rmgr\":{",
				 (unsigned long long) st->nrecords,
				 (unsigned long long) st->ntxns,
				 (unsigned long long) st->nsql,
				 (unsigned long long) st->ninsert,
				 (unsigned long long) st->nupdate,
				 (unsigned long long) st->ndelete,
				 (unsigned long long) st->ntruncate,
				 (unsigned long long) st->nddl,
				 (uint32) (st->first_lsn >> 32), (uint32) st->first_lsn,
				 (uint32) (st->last_lsn >> 32), (uint32) st->last_lsn);
	for (i = 0; i < 32; i++)
	{
		if (!st->rmgr_count[i])
			continue;
		pwe_str_addf(out, "%s\"%s\":%llu", i ? "," : "",
					 pwe_rmgr_name((uint8) i),
					 (unsigned long long) st->rmgr_count[i]);
	}
	pwe_str_adds(out, "}}");
}

/* --- endpoints ------------------------------------------------------ */
static void
handle_info(int fd, PweApiParams *p)
{
	PweWalFile *files = NULL;
	int			nfiles = 0;
	char		err[512];
	PweStr		json;
	int			i;
	uint32		segsize = PWE_SEGSIZE_DEFAULT;
	int			version = 0;
	uint64		first_lsn = 0,
				last_lsn = 0;
	const char *dir = p->dir ? p->dir : p->file;

	pwe_str_init(&json);
	if (!dir)
	{
		send_error(fd, 400, "dir parameter is required");
		return;
	}
	if (p->file)
	{
		/* single file info */
		struct stat st;
		uint8		page[8192];
		int			f = open(p->file, O_RDONLY);
		ssize_t		n = f >= 0 ? read(f, page, sizeof(page)) : -1;
		const PweVersionInfo *vi = NULL;

		if (f >= 0)
			close(f);
		if (n >= 2)
			vi = pwe_version_by_magic((uint16) (page[0] | (page[1] << 8)));
		if (stat(p->file, &st) == 0)
		{
			pwe_str_adds(&json, "{\"file\":");
			pwe_str_add_json(&json, p->file, strlen(p->file));
			pwe_str_addf(&json, ",\"size\":%llu,\"version\":%d,\"timeline\":null,\"segments\":[",
						 (unsigned long long) st.st_size, vi ? vi->major : 0);
			pwe_str_add_json(&json, p->file, strlen(p->file));
			pwe_str_adds(&json, "]}");
		}
		else
		{
			send_error(fd, 404, "cannot stat file");
			return;
		}
		send_json(fd, 200, &json);
		pwe_str_free(&json);
		return;
	}
	if (pwe_list_wal_files(p->dir, &files, &nfiles, err, sizeof(err)) < 0)
	{
		send_error(fd, 400, err);
		return;
	}
	{
		uint64		maxsize = 0;
		uint8		page[8192];
		int			f = open(files[0].path, O_RDONLY);

		if (f >= 0)
		{
			ssize_t		n = read(f, page, sizeof(page));

			close(f);
			if (n >= 40)
			{
				const PweVersionInfo *vi = pwe_version_by_magic((uint16) (page[0] | (page[1] << 8)));

				if (vi)
					version = vi->major;
				if ((page[2] & 0x02) && n >= 40)
				{
					uint32		seg = (uint32) (page[32] | (page[33] << 8) |
											 (page[34] << 16) | ((uint32) page[35] << 24));

					if (seg >= 1024 * 1024)
						segsize = seg;
				}
			}
		}
		for (i = 0; i < nfiles; i++)
			if (files[i].size > maxsize)
				maxsize = files[i].size;
		(void) maxsize;
		if (p->version)
			version = p->version;
		{
			uint64		per = (uint64) 0x100000000ULL / segsize;

			if (per == 0)
				per = 1;
			first_lsn = (uint64) ((files[0].logid * per + files[0].segindex) * segsize);
			last_lsn = (uint64) (((files[nfiles - 1].logid * per +
								   files[nfiles - 1].segindex) * segsize) +
								 files[nfiles - 1].size);
		}
		pwe_str_adds(&json, "{\"dir\":");
		pwe_str_add_json(&json, p->dir, strlen(p->dir));
		pwe_str_addf(&json, ",\"version\":%d,\"timeline\":%u,\"segment_size\":%u,"
					 "\"first_lsn\":\"%X/%X\",\"last_lsn\":\"%X/%X\",\"segments\":[",
					 version, files[nfiles - 1].tli, segsize,
					 (uint32) (first_lsn >> 32), (uint32) first_lsn,
					 (uint32) (last_lsn >> 32), (uint32) last_lsn);
		for (i = 0; i < nfiles; i++)
		{
			if (i)
				pwe_str_addc(&json, ',');
			pwe_str_add_json(&json, files[i].name, strlen(files[i].name));
		}
		pwe_str_adds(&json, "]}");
	}
	pwe_free_wal_files(files, nfiles);
	send_json(fd, 200, &json);
	pwe_str_free(&json);
}

static void
handle_records(int fd, PweApiParams *p)
{
	PweReaderOpts ropts;
	PweReader  *reader;
	PweRecord	rec;
	char		err[1024];
	uint64		count = 0;
	PweStr		json;
	FILE	   *mem;
	char	   *buf = NULL;
	size_t		buflen = 0;
	bool		first = true;

	if (!p->dir && !p->file)
	{
		send_error(fd, 400, "dir parameter is required");
		return;
	}
	memset(&ropts, 0, sizeof(ropts));
	ropts.wal_dir = p->dir;
	ropts.wal_file = p->file;
	ropts.version = p->version;
	ropts.start_lsn = 0;	/* warm up the catalog, emit from p->start_lsn */
	ropts.end_lsn = p->end_lsn;
	ropts.verify_crc = !p->no_crc;
	reader = pwe_reader_open(&ropts, err, sizeof(err));
	if (!reader)
	{
		send_error(fd, 400, err);
		return;
	}
	pwe_str_init(&json);
	pwe_str_adds(&json, "[");
	{
		int			rc;

		while ((rc = pwe_reader_next(reader, &rec, err, sizeof(err))) == 1)
		{
			PweStr		line;

			if (!first)
				pwe_str_addc(&json, ',');
			first = false;
			pwe_str_init(&line);
			pwe_record_describe(&rec, pwe_reader_version(reader), &line, false);
			pwe_str_add(&json, line.data, line.len);
			pwe_str_free(&line);
			count++;
			if (p->limit && count >= p->limit)
				break;
			if (json.len > 8 * 1024 * 1024)
				break;
			memset(err, 0, sizeof(err));
		}
		if (rc < 0)
		{
			pwe_reader_close(reader);
			pwe_str_free(&json);
			send_error(fd, 400, err);
			return;
		}
	}
	pwe_reader_close(reader);
	pwe_str_adds(&json, "]");
	send_json(fd, 200, &json);
	pwe_str_free(&json);
	(void) mem;
	(void) buf;
	(void) buflen;
}

static void
handle_sql(int fd, PweApiParams *p, bool json_envelope)
{
	char		err[1024];
	PweStats	stats;
	char	   *buf = NULL;
	size_t		buflen = 0;
	FILE	   *mem;

	if (!p->dir && !p->file)
	{
		send_error(fd, 400, "dir parameter is required");
		return;
	}
	memset(&stats, 0, sizeof(stats));
	mem = open_memstream(&buf, &buflen);
	if (!mem)
	{
		send_error(fd, 500, "out of memory");
		return;
	}
	if (run_reconstruction(p, mem, &stats, err, sizeof(err)) < 0)
	{
		fclose(mem);
		free(buf);
		send_error(fd, 400, err);
		return;
	}
	fclose(mem);
	if (json_envelope)
	{
		PweStr		json;

		pwe_str_init(&json);
		pwe_str_adds(&json, "{\"format\":\"sql\",\"sql\":");
		pwe_str_add_json(&json, buf ? buf : "", buflen);
		pwe_str_adds(&json, ",\"stats\":");
		stats_json(&stats, &json);
		pwe_str_addc(&json, '}');
		send_json(fd, 200, &json);
		pwe_str_free(&json);
	}
	else
		send_response(fd, 200, "text/plain; charset=utf-8", buf ? buf : "", buflen);
	free(buf);
}

static void
handle_stats(int fd, PweApiParams *p)
{
	char		err[1024];
	PweStats	stats;
	FILE	   *devnull;
	PweStr		json;

	if (!p->dir && !p->file)
	{
		send_error(fd, 400, "dir parameter is required");
		return;
	}
	memset(&stats, 0, sizeof(stats));
	devnull = fopen("/dev/null", "w");
	if (!devnull)
	{
		send_error(fd, 500, "cannot open /dev/null");
		return;
	}
	if (run_reconstruction(p, devnull, &stats, err, sizeof(err)) < 0)
	{
		fclose(devnull);
		send_error(fd, 400, err);
		return;
	}
	fclose(devnull);
	pwe_str_init(&json);
	stats_json(&stats, &json);
	send_json(fd, 200, &json);
	pwe_str_free(&json);
}

static void
handle_catalog(int fd, PweApiParams *p)
{
	char		err[1024];
	PweStats	stats;
	FILE	   *devnull;
	PweReconstructor *recon;
	PweReaderOpts ropts;
	PweSqlOptions sopts;
	PweReader  *reader;
	PweRecord	rec;
	PweStr		json;
	int			rc;
	size_t		i;
	bool		first_table = true;

	if (!p->dir && !p->file)
	{
		send_error(fd, 400, "dir parameter is required");
		return;
	}
	memset(&stats, 0, sizeof(stats));
	memset(&ropts, 0, sizeof(ropts));
	memset(&sopts, 0, sizeof(sopts));
	ropts.wal_dir = p->dir;
	ropts.wal_file = p->file;
	ropts.version = p->version;
	ropts.end_lsn = p->end_lsn;
	sopts.include_comments = false;
	devnull = fopen("/dev/null", "w");
	reader = pwe_reader_open(&ropts, err, sizeof(err));
	if (!reader)
	{
		if (devnull)
			fclose(devnull);
		send_error(fd, 400, err);
		return;
	}
	recon = pwe_recon_create(pwe_reader_version(reader), &sopts, devnull, &stats);
	while ((rc = pwe_reader_next(reader, &rec, err, sizeof(err))) == 1)
	{
		pwe_recon_process(recon, &rec);
		memset(err, 0, sizeof(err));
		if (p->limit && stats.nrecords >= p->limit)
			break;
	}
	pwe_str_init(&json);
	pwe_str_adds(&json, "{\"tables\":[");
	{
		PweCatalog *cat = pwe_recon_catalog(recon);

		for (i = 0; i < cat->rels_by_oid.nbuckets; i++)
		{
			PweHashEntry *e;

			for (e = cat->rels_by_oid.buckets[i]; e; e = e->next)
			{
				PweRel	   *rel = e->val;
				int			a;

				if (rel->oid < PWE_FIRST_NORMAL_OID)
					continue;
				if (!(rel->relkind == 'r' || rel->relkind == 'p' ||
					  rel->relkind == 'm' || rel->relkind == 'f'))
					continue;
				if (!first_table)
					pwe_str_addc(&json, ',');
				first_table = false;
				pwe_str_adds(&json, "{\"oid\":");
				pwe_str_addf(&json, "%u,\"schema\":", rel->oid);
				pwe_str_add_json(&json, rel->nspname, strlen(rel->nspname));
				pwe_str_adds(&json, ",\"name\":");
				pwe_str_add_json(&json, rel->name, strlen(rel->name));
				pwe_str_adds(&json, ",\"columns\":[");
				{
					bool		firstcol = true;

					for (a = 1; a <= rel->natts; a++)
					{
						PweAttr    *at = rel->attrs ? rel->attrs[a] : NULL;

						if (!at || at->dropped)
							continue;
						if (!firstcol)
							pwe_str_addc(&json, ',');
						firstcol = false;
						pwe_str_adds(&json, "{\"name\":");
						pwe_str_add_json(&json, at->name, strlen(at->name));
						pwe_str_adds(&json, ",\"type_oid\":");
						pwe_str_addf(&json, "%u,\"not_null\":%s}", at->typid,
									 at->notnull ? "true" : "false");
					}
				}
				pwe_str_adds(&json, "]}");
			}
		}
	}
	pwe_str_adds(&json, "]}");
	pwe_recon_finish(recon);
	pwe_recon_free(recon);
	pwe_reader_close(reader);
	if (devnull)
		fclose(devnull);
	send_json(fd, 200, &json);
	pwe_str_free(&json);
}

static void
handle_health(int fd)
{
	PweStr		json;

	pwe_str_init(&json);
	pwe_str_adds(&json, "{\"status\":\"ok\",\"tool\":\"pg_walex\",\"version\":\"" PWE_VERSION "\","
				 "\"postgres_versions\":[11,12,13,14,15,16,17,18],\"pglz\":true,\"lz4\":");
	pwe_str_adds(&json, pwe_lz4_available() ? "true" : "false");
	pwe_str_adds(&json, ",\"zstd\":");
	pwe_str_adds(&json, pwe_zstd_available() ? "true" : "false");
	pwe_str_adds(&json, "}");
	send_json(fd, 200, &json);
	pwe_str_free(&json);
}

/* find header value in raw request head */
static bool
header_value(const char *req, const char *name, char *out, size_t outlen)
{
	const char *p = req;

	out[0] = '\0';
	while (p && *p)
	{
		const char *eol = strstr(p, "\r\n");

		if (!eol)
			break;
		if (strncasecmp(p, name, strlen(name)) == 0 && p[strlen(name)] == ':')
		{
			const char *v = p + strlen(name) + 1;
			size_t		n;

			while (*v == ' ' || *v == '\t')
				v++;
			n = (size_t) (eol - v);
			if (n >= outlen)
				n = outlen - 1;
			memcpy(out, v, n);
			out[n] = '\0';
			return true;
		}
		p = eol + 2;
	}
	return false;
}

static void
handle_client(int fd, const PweHttpOptions *opts)
{
	char	   *req = pwe_malloc(PWE_HTTP_MAX_REQ);
	size_t		used = 0;
	size_t		header_end = 0;
	ssize_t		n;
	char		method[16] = "";
	char		target[2048] = "";
	char	   *query;
	char	   *body = NULL;
	char		auth[512];
	PweApiParams params;

	while (used < PWE_HTTP_MAX_REQ - 1)
	{
		char	   *end;

		n = recv(fd, req + used, PWE_HTTP_MAX_REQ - 1 - used, 0);
		if (n <= 0)
			break;
		used += (size_t) n;
		req[used] = '\0';
		end = strstr(req, "\r\n\r\n");
		if (end)
		{
			header_end = (size_t) (end - req) + 4;
			break;
		}
	}
	if (!used)
	{
		free(req);
		close(fd);
		return;
	}
	req[used] = '\0';
	{
		char	   *sp1 = strchr(req, ' ');
		char	   *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;

		if (sp1 && sp2)
		{
			size_t		mlen = (size_t) (sp1 - req);

			if (mlen >= sizeof(method))
				mlen = sizeof(method) - 1;
			memcpy(method, req, mlen);
			method[mlen] = '\0';
			{
				size_t		tlen = (size_t) (sp2 - sp1 - 1);

				if (tlen >= sizeof(target))
					tlen = sizeof(target) - 1;
				memcpy(target, sp1 + 1, tlen);
				target[tlen] = '\0';
			}
		}
	}
	if (opts->token && opts->token[0] &&
		strcmp(target, "/api/v1/health") != 0 && strcmp(target, "/health") != 0)
	{
		char		expected[512];

		snprintf(expected, sizeof(expected), "Bearer %s", opts->token);
		if (!header_value(req, "Authorization", auth, sizeof(auth)) ||
			strcmp(auth, expected) != 0)
		{
			send_error(fd, 401, "missing or invalid Bearer token");
			free(req);
			close(fd);
			return;
		}
	}
	if (strcmp(method, "OPTIONS") == 0)
	{
		send_response(fd, 200, "text/plain", "", 0);
		free(req);
		close(fd);
		return;
	}

	/* read body if Content-Length present */
	{
		char		clen[32];

		if (header_value(req, "Content-Length", clen, sizeof(clen)))
		{
			uint64		want = strtoull(clen, NULL, 10);
			uint64		have = used - header_end;

			if (want > PWE_HTTP_MAX_REQ - 1)
				want = PWE_HTTP_MAX_REQ - 1;
			while (have < want)
			{
				n = recv(fd, req + used, (size_t) (want - have), 0);
				if (n <= 0)
					break;
				used += (size_t) n;
				have += (uint64) n;
			}
			req[used] = '\0';
			body = req + header_end;
		}
	}

	query = strchr(target, '?');
	if (query)
	{
		*query = '\0';
		query++;
	}
	params_init(&params, opts);
	if (query)
		params_from_query(&params, query);
	if (body && body[0])
		params_from_json(&params, body);

	if (strcmp(target, "/api/v1/health") == 0 || strcmp(target, "/health") == 0)
		handle_health(fd);
	else if (strcmp(target, "/api/v1/info") == 0)
		handle_info(fd, &params);
	else if (strcmp(target, "/api/v1/wal-files") == 0)
		handle_info(fd, &params);
	else if (strcmp(target, "/api/v1/records") == 0)
		handle_records(fd, &params);
	else if (strcmp(target, "/api/v1/catalog") == 0)
		handle_catalog(fd, &params);
	else if (strcmp(target, "/api/v1/stats") == 0)
		handle_stats(fd, &params);
	else if (strcmp(target, "/api/v1/sql") == 0)
		handle_sql(fd, &params, false);
	else if (strcmp(target, "/api/v1/parse") == 0)
	{
		if (strcmp(method, "POST") == 0 && body && body[0] &&
			params.format && strcmp(params.format, "sql") != 0)
			handle_sql(fd, &params, true);
		else if (strcmp(method, "POST") == 0 && body && body[0])
			handle_sql(fd, &params, true);
		else if (strcmp(method, "GET") == 0)
			handle_records(fd, &params);
		else
			send_error(fd, 400, "POST with JSON body required");
	}
	else
		send_error(fd, 404, "unknown endpoint");

	free(req);
	close(fd);
}

int
pwe_http_serve(const PweHttpOptions *opts)
{
	int			sfd;
	int			on = 1;
	struct sockaddr_in addr;

	signal(SIGPIPE, SIG_IGN);
	sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sfd < 0)
	{
		fprintf(stderr, "pg_walex: socket: %s\n", strerror(errno));
		return 1;
	}
	setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t) opts->port);
	if (!opts->listen || strcmp(opts->listen, "0.0.0.0") == 0)
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	else if (inet_pton(AF_INET, opts->listen, &addr.sin_addr) != 1)
	{
		fprintf(stderr, "pg_walex: invalid listen address \"%s\"\n", opts->listen);
		close(sfd);
		return 1;
	}
	if (bind(sfd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
	{
		fprintf(stderr, "pg_walex: bind %s:%d: %s\n",
				opts->listen ? opts->listen : "0.0.0.0", opts->port, strerror(errno));
		close(sfd);
		return 1;
	}
	if (listen(sfd, 64) < 0)
	{
		fprintf(stderr, "pg_walex: listen: %s\n", strerror(errno));
		close(sfd);
		return 1;
	}
	fprintf(stderr, "pg_walex %s REST API listening on http://%s:%d/api/v1\n",
			PWE_VERSION, opts->listen ? opts->listen : "127.0.0.1", opts->port);
	if (opts->default_wal_dir)
		fprintf(stderr, "pg_walex: default WAL directory: %s\n", opts->default_wal_dir);
	for (;;)
	{
		int			cfd = accept(sfd, NULL, NULL);

		if (cfd < 0)
		{
			if (errno == EINTR)
				continue;
			fprintf(stderr, "pg_walex: accept: %s\n", strerror(errno));
			break;
		}
		handle_client(cfd, opts);
	}
	close(sfd);
	return 0;
}
