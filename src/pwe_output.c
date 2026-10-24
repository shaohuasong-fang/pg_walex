/*
 * pg_walex - record description (text / JSON)
 */
#include "pwe_internal.h"

static const char *
fork_name(uint8 forknum)
{
	switch (forknum)
	{
		case 0:
			return "main";
		case 1:
			return "fsm";
		case 2:
			return "vm";
		case 3:
			return "init";
		default:
			return "unknown";
	}
}

void
pwe_record_describe(PweRecord *rec, int version, PweStr *out, bool with_sql)
{
	int			i;
	char		lsn[32];

	(void) with_sql;
	pwe_format_lsn(lsn, sizeof(lsn), rec->lsn);
	pwe_str_addf(out, "{\"lsn\":\"%s\",\"xid\":%u,\"rmgr\":\"%s\",\"info\":\"%s\","
				 "\"rmgr_id\":%u,\"info_id\":%u,\"total_len\":%u,\"prev_lsn\":\"%X/%X\"",
				 lsn, rec->xid, pwe_rmgr_name(rec->rmid),
				 pwe_rmgr_info_name(rec->rmid, rec->info, version),
				 rec->rmid, rec->info & 0xF0, rec->tot_len,
				 (uint32) (rec->prev_lsn >> 32), (uint32) rec->prev_lsn);
	if (rec->origin)
		pwe_str_addf(out, ",\"origin\":%u", rec->origin);
	if (rec->main_len)
		pwe_str_addf(out, ",\"main_len\":%u", rec->main_len);
	pwe_str_adds(out, ",\"blocks\":[");
	for (i = 0; i < rec->nblocks; i++)
	{
		PweBlock   *b = &rec->blocks[i];

		if (!b->in_use)
			continue;
		if (i > 0 && rec->blocks[0].in_use)
			pwe_str_addc(out, ',');
		pwe_str_addf(out, "{\"id\":%d,\"spc\":%u,\"db\":%u,\"rel\":%u,"
					 "\"fork\":\"%s\",\"blkno\":%u,\"data_len\":%u,"
					 "\"has_image\":%s,\"will_init\":%s}",
					 i, b->spc, b->db, b->rel, fork_name(b->forknum),
					 b->blkno, b->data_len,
					 b->has_image ? "true" : "false",
					 b->will_init ? "true" : "false");
	}
	pwe_str_adds(out, "]}");
}

void
pwe_record_describe_text(PweRecord *rec, int version, PweStr *out)
{
	char		lsn[32];
	int			i;

	pwe_format_lsn(lsn, sizeof(lsn), rec->lsn);
	pwe_str_addf(out, "%s: %s/%s: xid %u: tot_len %u",
				 lsn, pwe_rmgr_name(rec->rmid),
				 pwe_rmgr_info_name(rec->rmid, rec->info, version),
				 rec->xid, rec->tot_len);
	for (i = 0; i < rec->nblocks; i++)
	{
		PweBlock   *b = &rec->blocks[i];

		if (!b->in_use)
			continue;
		pwe_str_addf(out, "; blk %d: %u/%u/%u fork %s blkno %u len %u%s",
					 i, b->spc, b->db, b->rel, fork_name(b->forknum),
					 b->blkno, b->data_len, b->has_image ? " (FPI)" : "");
	}
	if (rec->main_len)
		pwe_str_addf(out, "; main_data %u", rec->main_len);
	pwe_str_addc(out, '\n');
}
