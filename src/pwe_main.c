/*
 * pg_walex - command line front end
 */
#include "pwe_internal.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static void
usage(FILE *out)
{
	fprintf(out,
			"pg_walex " PWE_VERSION " - PostgreSQL WAL Extractor\n"
			"Read PostgreSQL 11..18 WAL segments directly (no server, no libpq),\n"
			"reconstruct the changes as portable SQL text and expose a RESTful API.\n\n"
			"Usage: pg_walex [options] <wal-directory>\n\n"
			"Options:\n"
			"  -d, --dir PATH          WAL directory (pg_wal)\n"
			"  -f, --file PATH         read a single WAL segment file\n"
			"      --pg-version N      force PostgreSQL major version (11..18)\n"
			"  -s, --start-lsn LSN     start at LSN (X/Y, inclusive)\n"
			"  -e, --end-lsn LSN       stop before LSN (X/Y)\n"
			"  -t, --timeline N        timeline to read (default: highest)\n"
			"  -b, --blocksize N       WAL block size (default: auto/8192)\n"
			"  -S, --segsize N         WAL segment size (default: auto/16MB)\n"
			"  -F, --format FMT        sql | json | text | stats (default: sql)\n"
			"  -o, --output PATH       write output to FILE (with --tocopy: output directory)\n"
			"      --tocopy            write COPY-format output: manifest.txt plus\n"
			"                          <schema>.<table>.meta.sql and .data.sql files\n"
			"                          (output directory from -o, default ./pg_walex_copy)\n"
			"      --batch-inserts[=N] merge consecutive INSERTs of one table into a\n"
			"                          single multi-VALUES statement (default N=100)\n"
			"      --rollback          also emit the inverse statement for every\n"
			"                          UPDATE/DELETE (commented as rollback)\n"
			"  -l, --limit N           stop after N WAL records\n"
			"      --xid N             only emit the given transaction\n"
			"      --table NAME        only emit DML for NAME ([schema.]table)\n"
			"      --include-non-sql   add comments for non-SQL WAL records\n"
			"      --include-aborted   report aborted transactions\n"
			"      --logical-messages  show pg_logical_emit_message payloads\n"
			"      --no-comments       emit bare SQL without comments/BEGIN/COMMIT\n"
			"      --no-crc            skip WAL record CRC verification\n"
			"      --db-oid N          restrict output to one database OID\n"
			"      --no-warmup         start scanning exactly at --start-lsn\n"
			"      --pgdata PATH       seed catalog from an offline data directory\n"
			"      --no-pgdata         do not auto-detect/read the data directory\n"
			"      --no-connect        do not emit psql \\connect between databases\n"
			"      --max-rows N        row cache size for ctid->row tracking (default 200000)\n"
			"      --serve             run the RESTful API server\n"
			"      --monitor           with --serve: follow the WAL directory in the\n"
			"                          background and expose the latest reconstructed\n"
			"                          SQL statement at /api/v1/item\n"
			"      --listen ADDR       listen address (default 127.0.0.1)\n"
			"      --port N            listen port (default 8900)\n"
			"      --token TOKEN       require Bearer TOKEN for API requests\n"
			"  -v, --verbose           verbose messages on stderr\n"
			"      --version           print version and exit\n"
			"  -h, --help              show this help\n\n"
			"Examples:\n"
			"  pg_walex --pg-version version_num waldirectory > changes.sql\n"
			"  pg_walex -F json --start-lsn 0/16B2C60 waldirectory\n"
			"  pg_walex --serve --port 8900 --dir waldirectory\n"
                        "Comments:\n"
                        " valid version_num is : 11,12,13,14,15,16,17,18\n"
                        " waldirectory: <pgdata directory>/pg_wal\n");
}

static int
format_from_name(const char *name)
{
	if (!name)
		return 0;
	if (strcmp(name, "sql") == 0)
		return 0;
	if (strcmp(name, "json") == 0)
		return 1;
	if (strcmp(name, "text") == 0)
		return 2;
	if (strcmp(name, "stats") == 0)
		return 3;
	return -1;
}

int
main(int argc, char **argv)
{
	static struct option longopts[] = {
		{"dir", required_argument, NULL, 'd'},
		{"file", required_argument, NULL, 'f'},
		{"pg-version", required_argument, NULL, 1000},
		{"start-lsn", required_argument, NULL, 's'},
		{"end-lsn", required_argument, NULL, 'e'},
		{"timeline", required_argument, NULL, 't'},
		{"blocksize", required_argument, NULL, 'b'},
		{"segsize", required_argument, NULL, 'S'},
		{"format", required_argument, NULL, 'F'},
		{"output", required_argument, NULL, 'o'},
		{"limit", required_argument, NULL, 'l'},
		{"xid", required_argument, NULL, 1001},
		{"table", required_argument, NULL, 1002},
		{"include-non-sql", no_argument, NULL, 1003},
		{"include-aborted", no_argument, NULL, 1004},
		{"logical-messages", no_argument, NULL, 1005},
		{"no-comments", no_argument, NULL, 1006},
		{"no-crc", no_argument, NULL, 1007},
		{"max-rows", required_argument, NULL, 1008},
		{"serve", no_argument, NULL, 1009},
		{"listen", required_argument, NULL, 1010},
		{"port", required_argument, NULL, 1011},
		{"token", required_argument, NULL, 1012},
		{"db-oid", required_argument, NULL, 1014},
		{"no-connect", no_argument, NULL, 1015},
		{"no-warmup", no_argument, NULL, 1016},
		{"pgdata", required_argument, NULL, 1017},
		{"no-pgdata", no_argument, NULL, 1018},
		{"tocopy", no_argument, NULL, 1020},
		{"batch-inserts", optional_argument, NULL, 1021},
		{"monitor", no_argument, NULL, 1022},
		{"rollback", no_argument, NULL, 1023},
		{"verbose", no_argument, NULL, 'v'},
		{"version", no_argument, NULL, 1013},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};
	PweReaderOpts ropts;
	PweSqlOptions sopts;
	PweReader  *reader = NULL;
	PweReconstructor *recon = NULL;
	PweStats	stats;
	FILE	   *out = stdout;
	FILE	   *recon_out = NULL;
	char		err[2048];
	int			opt;
	int			format = 0;
	int			exit_code = 0;
	bool		verbose = false;
	bool		serve = false;
	bool		warmup = true;
	bool		no_pgdata = false;
	const char *pgdata_opt = NULL;
	char		pgdata_guess[4096];
	bool		no_comments = false;
	uint64		limit = 0;
	uint64		xid_filter = 0;
	uint64		max_rows = 200000;
	uint32		db_filter = 0;
	const char *table_filter = NULL;
	const char *outfile = NULL;
	PweHttpOptions hopts;

	memset(&ropts, 0, sizeof(ropts));
	memset(&sopts, 0, sizeof(sopts));
	memset(&stats, 0, sizeof(stats));
	memset(&hopts, 0, sizeof(hopts));
	ropts.verify_crc = true;
	sopts.include_comments = true;
	hopts.listen = "127.0.0.1";
	hopts.port = 8900;

	while ((opt = getopt_long(argc, argv, "d:f:s:e:t:b:S:F:o:l:vh",
							  longopts, NULL)) != -1)
	{
		switch (opt)
		{
			case 'd':
				ropts.wal_dir = optarg;
				hopts.default_wal_dir = optarg;
				break;
			case 'f':
				ropts.wal_file = optarg;
				break;
			case 1000:
				{
					int64		v;

					if (!pwe_parse_int(optarg, &v) || v < 11 || v > 18)
					{
						fprintf(stderr, "pg_walex: invalid PostgreSQL version \"%s\"\n", optarg);
						return 2;
					}
					ropts.version = (int) v;
					hopts.default_version = (int) v;
				}
				break;
			case 's':
				if (!pwe_parse_lsn(optarg, &ropts.start_lsn))
				{
					fprintf(stderr, "pg_walex: invalid start LSN \"%s\"\n", optarg);
					return 2;
				}
				break;
			case 'e':
				if (!pwe_parse_lsn(optarg, &ropts.end_lsn))
				{
					fprintf(stderr, "pg_walex: invalid end LSN \"%s\"\n", optarg);
					return 2;
				}
				break;
			case 't':
				{
					int64		v;

					if (!pwe_parse_int(optarg, &v) || v <= 0)
					{
						fprintf(stderr, "pg_walex: invalid timeline \"%s\"\n", optarg);
						return 2;
					}
					ropts.timeline = (int) v;
				}
				break;
			case 'b':
				{
					int64		v;

					if (!pwe_parse_int(optarg, &v) || v < 1024)
					{
						fprintf(stderr, "pg_walex: invalid block size \"%s\"\n", optarg);
						return 2;
					}
					ropts.blocksize = (uint32) v;
				}
				break;
			case 'S':
				{
					int64		v;

					if (!pwe_parse_int(optarg, &v) || v < 1024 * 1024)
					{
						fprintf(stderr, "pg_walex: invalid segment size \"%s\"\n", optarg);
						return 2;
					}
					ropts.segsize = (uint32) v;
				}
				break;
			case 'F':
				format = format_from_name(optarg);
				if (format < 0)
				{
					fprintf(stderr, "pg_walex: unknown format \"%s\"\n", optarg);
					return 2;
				}
				break;
			case 'o':
				outfile = optarg;
				break;
			case 'l':
				if (!pwe_parse_u64(optarg, &limit))
				{
					fprintf(stderr, "pg_walex: invalid limit \"%s\"\n", optarg);
					return 2;
				}
				break;
			case 1001:
				if (!pwe_parse_u64(optarg, &xid_filter))
				{
					fprintf(stderr, "pg_walex: invalid xid \"%s\"\n", optarg);
					return 2;
				}
				break;
			case 1002:
				table_filter = optarg;
				break;
			case 1003:
				sopts.include_non_sql = true;
				break;
			case 1004:
				sopts.include_aborted = true;
				break;
			case 1005:
				sopts.logical_messages = true;
				break;
			case 1006:
				no_comments = true;
				sopts.include_comments = false;
				break;
			case 1007:
				ropts.verify_crc = false;
				break;
			case 1008:
				if (!pwe_parse_u64(optarg, &max_rows))
				{
					fprintf(stderr, "pg_walex: invalid --max-rows \"%s\"\n", optarg);
					return 2;
				}
				break;
			case 1009:
				serve = true;
				break;
			case 1010:
				hopts.listen = optarg;
				break;
			case 1011:
				{
					int64		v;

					if (!pwe_parse_int(optarg, &v) || v <= 0 || v > 65535)
					{
						fprintf(stderr, "pg_walex: invalid port \"%s\"\n", optarg);
						return 2;
					}
					hopts.port = (int) v;
				}
				break;
			case 1012:
				hopts.token = optarg;
				break;
			case 1014:
				{
					uint64		v;

					if (!pwe_parse_u64(optarg, &v) || v == 0 || v > 0xFFFFFFFFULL)
					{
						fprintf(stderr, "pg_walex: invalid database OID \"%s\"\n", optarg);
						return 2;
					}
					db_filter = (uint32) v;
					hopts.default_db_oid = (uint32) v;
				}
				break;
			case 1015:
				sopts.emit_connect = false;
				break;
			case 1016:
				warmup = false;
				break;
			case 1017:
				pgdata_opt = optarg;
				break;
			case 1018:
				no_pgdata = true;
				break;
			case 1020:
				sopts.tocopy = true;
				break;
			case 1021:
				if (optarg)
				{
					uint64		v;

					if (!pwe_parse_u64(optarg, &v) || v == 0 || v > 1000000)
					{
						fprintf(stderr, "pg_walex: invalid --batch-inserts \"%s\"\n", optarg);
						return 2;
					}
					sopts.batch_rows = (uint32) v;
				}
				else
					sopts.batch_rows = 100;
				break;
			case 1022:
				hopts.monitor = true;
				break;
			case 1023:
				sopts.rollback = true;
				break;
			case 'v':
				verbose = true;
				break;
			case 1013:
				printf("pg_walex %s\n", PWE_VERSION);
				return 0;
			case 'h':
				usage(stdout);
				return 0;
			default:
				usage(stderr);
				return 2;
		}
	}
	if (!ropts.wal_dir && !ropts.wal_file && optind < argc)
	{
		ropts.wal_dir = argv[optind];
		hopts.default_wal_dir = argv[optind];
	}
	if (serve)
	{
		if (hopts.default_version == 0)
			hopts.default_version = ropts.version;
		return pwe_http_serve(&hopts);
	}
	if (!ropts.wal_dir && !ropts.wal_file)
	{
		fprintf(stderr, "pg_walex: no WAL directory given (see --help)\n");
		return 2;
	}
	if (sopts.tocopy)
	{
		/* -o is the output directory in COPY mode (default ./pg_walex_copy) */
		const char *dir = outfile ? outfile : "./pg_walex_copy";
		char		mpath[4096];

		if (mkdir(dir, 0755) != 0 && errno != EEXIST)
		{
			fprintf(stderr, "pg_walex: cannot create directory \"%s\": %s\n",
					dir, strerror(errno));
			return 2;
		}
		snprintf(mpath, sizeof(mpath), "%s/manifest.txt", dir);
		out = fopen(mpath, "w");
		if (!out)
		{
			fprintf(stderr, "pg_walex: cannot open \"%s\": %s\n", mpath, strerror(errno));
			return 2;
		}
		sopts.output_dir = dir;
		fprintf(out, "-- pg_walex manifest (COPY mode)\n"
				"-- replay with: cd %s && psql -d <dbname> -f manifest.txt\n\n",
				dir);
	}
	else if (outfile)
	{
		out = fopen(outfile, "w");
		if (!out)
		{
			fprintf(stderr, "pg_walex: cannot open \"%s\": %s\n", outfile, strerror(errno));
			return 2;
		}
	}
	sopts.limit = (uint32) limit;
	sopts.only_xid = (uint32) xid_filter;
	sopts.only_table = table_filter;
	sopts.max_row_cache = (uint32) max_rows;
	sopts.include_comments = !no_comments;
	sopts.only_database = db_filter;
	sopts.emit_connect = true;
	sopts.start_lsn = ropts.start_lsn;
	if (warmup)
		ropts.start_lsn = 0;	/* build catalog/row state from the beginning */
	if (pgdata_opt)
		sopts.pgdata = pgdata_opt;
	else if (!no_pgdata)
	{
		const char *p = ropts.wal_dir ? ropts.wal_dir : ropts.wal_file;

		if (p && pwe_guess_pgdata(p, pgdata_guess, sizeof(pgdata_guess)))
			sopts.pgdata = pgdata_guess;
	}
	if (verbose && sopts.pgdata)
		fprintf(stderr, "pg_walex: seeding catalog from %s\n", sopts.pgdata);

	memset(err, 0, sizeof(err));
	reader = pwe_reader_open(&ropts, err, sizeof(err));
	if (!reader)
	{
		fprintf(stderr, "pg_walex: %s\n", err);
		if (out != stdout)
			fclose(out);
		return 1;
	}
	if (verbose)
		fprintf(stderr, "pg_walex: PostgreSQL %d, block size %u, segment size %u, timeline %u\n",
				pwe_reader_version(reader), pwe_reader_blocksize(reader),
				pwe_reader_segsize(reader), ropts.timeline);
	if (pwe_reader_version_mismatch(reader))
		fprintf(stderr, "pg_walex: WARNING: WAL page magic does not match PostgreSQL %d; parsing anyway\n",
				pwe_reader_version(reader));

	if (format == 0 || format == 3)
	{
		recon_out = (format == 3 && !sopts.tocopy) ? fopen("/dev/null", "w") : out;
		recon = pwe_recon_create(pwe_reader_version(reader), &sopts,
								 recon_out ? recon_out : out, &stats);
	}
	{
		PweRecord	rec;
		int			rc;
		uint64		count = 0;
		PweStr		line;

		pwe_str_init(&line);
		while ((rc = pwe_reader_next(reader, &rec, err, sizeof(err))) == 1)
		{
			count++;
			if (format == 0)
				pwe_recon_process(recon, &rec);
			else if (format == 1)
			{
				pwe_str_reset(&line);
				pwe_record_describe(&rec, pwe_reader_version(reader), &line, false);
				pwe_str_addc(&line, '\n');
				fwrite(line.data, 1, line.len, out);
			}
			else if (format == 2)
			{
				pwe_str_reset(&line);
				pwe_record_describe_text(&rec, pwe_reader_version(reader), &line);
				fwrite(line.data, 1, line.len, out);
			}
			else
				pwe_recon_process(recon, &rec);
			if (limit && count >= limit)
				break;
			memset(err, 0, sizeof(err));
		}
		if (rc < 0)
		{
			fprintf(stderr, "pg_walex: %s\n", err);
			exit_code = 1;
		}
		pwe_str_free(&line);
	}
	if (recon)
	{
		pwe_recon_finish(recon);
		if (format == 3 && recon_out && recon_out != out)
			fclose(recon_out);
		if (format == 3)
		{
			int			i;
			FILE	   *stat_out = sopts.tocopy ? stdout : out;

			fprintf(stat_out, "{\n  \"records\": %llu,\n  \"transactions\": %llu,\n"
					"  \"statements\": %llu,\n  \"insert\": %llu,\n  \"update\": %llu,\n"
					"  \"delete\": %llu,\n  \"truncate\": %llu,\n  \"ddl\": %llu,\n"
					"  \"first_lsn\": \"%X/%X\",\n  \"last_lsn\": \"%X/%X\",\n"
					"  \"rmgr\": {",
					(unsigned long long) stats.nrecords,
					(unsigned long long) stats.ntxns,
					(unsigned long long) stats.nsql,
					(unsigned long long) stats.ninsert,
					(unsigned long long) stats.nupdate,
					(unsigned long long) stats.ndelete,
					(unsigned long long) stats.ntruncate,
					(unsigned long long) stats.nddl,
					(uint32) (stats.first_lsn >> 32), (uint32) stats.first_lsn,
					(uint32) (stats.last_lsn >> 32), (uint32) stats.last_lsn);
			for (i = 0; i < 32; i++)
			{
				if (!stats.rmgr_count[i])
					continue;
				fprintf(stat_out, "%s\n    \"%s\": %llu", i ? "," : "",
						pwe_rmgr_name((uint8) i),
						(unsigned long long) stats.rmgr_count[i]);
			}
			fprintf(stat_out, "\n  }\n}\n");
		}
		pwe_recon_free(recon);
	}
	pwe_reader_close(reader);
	if (out != stdout)
		fclose(out);
	return exit_code;
}
