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
 * pg_walex - per-major-version knowledge (page magic, catalog layouts,
 * record type names, built-in type table)
 */
#include "pg_walex.h"

#include "pwe_internal.h"
#include "pwe_catalog_layouts.h"

static const PweVersionInfo pwe_versions[] = {
	{9, 0xD093, false},
	{10, 0xD097, false},
	{11, 0xD098, false},
	{12, 0xD101, true},
	{13, 0xD106, true},
	{14, 0xD10D, true},
	{15, 0xD110, true},
	{16, 0xD113, true},
	{17, 0xD116, true},
	{18, 0xD118, true},
	{19, 0xD122, true},
};

const PweVersionInfo *
pwe_version_by_major(int major)
{
	size_t		i;

	for (i = 0; i < sizeof(pwe_versions) / sizeof(pwe_versions[0]); i++)
		if (pwe_versions[i].major == major)
			return &pwe_versions[i];
	return NULL;
}

const PweVersionInfo *
pwe_version_by_magic(uint16 magic)
{
	size_t		i;

	for (i = 0; i < sizeof(pwe_versions) / sizeof(pwe_versions[0]); i++)
		if (pwe_versions[i].page_magic == magic)
			return &pwe_versions[i];
	return NULL;
}

/* ------------------------------------------------------------------ */
/* resource manager names                                             */
/* ------------------------------------------------------------------ */
static const char *const pwe_rmgr_names[] = {
	"XLOG", "Transaction", "Storage", "CLOG", "Database", "Tablespace",
	"MultiXact", "RelMap", "Standby", "Heap2", "Heap", "Btree", "Hash",
	"Gin", "Gist", "Sequence", "SPGist", "BRIN", "CommitTs",
	"ReplicationOrigin", "Generic", "LogicalMessage"
};

const char *
pwe_rmgr_name(uint8 rmid)
{
	if (rmid < sizeof(pwe_rmgr_names) / sizeof(pwe_rmgr_names[0]))
		return pwe_rmgr_names[rmid];
	return "Unknown";
}

#define RM_XLOG_ID 0
#define RM_XACT_ID 1
#define RM_SMGR_ID 2
#define RM_CLOG_ID 3
#define RM_DBASE_ID 4
#define RM_TBLSPC_ID 5
#define RM_MULTIXACT_ID 6
#define RM_RELMAP_ID 7
#define RM_STANDBY_ID 8
#define RM_HEAP2_ID 9
#define RM_HEAP_ID 10
#define RM_BTREE_ID 11
#define RM_HASH_ID 12
#define RM_GIN_ID 13
#define RM_GIST_ID 14
#define RM_SEQ_ID 15
#define RM_SPGIST_ID 16
#define RM_BRIN_ID 17
#define RM_COMMIT_TS_ID 18
#define RM_REPLORIGIN_ID 19
#define RM_GENERIC_ID 20
#define RM_LOGICALMSG_ID 21

static const char *
heap_info_name(uint8 info)
{
	switch (info & 0x70)
	{
		case 0x00:
			return "INSERT";
		case 0x10:
			return "DELETE";
		case 0x20:
			return "UPDATE";
		case 0x30:
			return "TRUNCATE";
		case 0x40:
			return "HOT_UPDATE";
		case 0x50:
			return "CONFIRM";
		case 0x60:
			return "LOCK";
		case 0x70:
			return "INPLACE";
	}
	return "UNKNOWN";
}

static const char *
heap2_info_name(uint8 info, int major)
{
	switch (info & 0x70)
	{
		case 0x00:
			return "REWRITE";
		case 0x10:
			return major < 15 ? "CLEAN" : "PRUNE_ON_ACCESS";
		case 0x20:
			return major < 15 ? "FREEZE_PAGE" : "PRUNE_VACUUM_SCAN";
		case 0x30:
			return major < 15 ? "CLEANUP_INFO" : "PRUNE_VACUUM_CLEANUP";
		case 0x40:
			/* XLOG_HEAP2_VISIBLE was removed in 19 (folded into prune) */
			return major < 19 ? "VISIBLE" : "UNKNOWN";
		case 0x50:
			return "MULTI_INSERT";
		case 0x60:
			return "LOCK_UPDATED";
		case 0x70:
			return "NEW_CID";
	}
	return "UNKNOWN";
}

static const char *
xlog_info_name(uint8 info, int major)
{
	(void) major;
	switch (info & 0xF0)
	{
		case 0x00:
			return "CHECKPOINT_SHUTDOWN";
		case 0x10:
			return "CHECKPOINT_ONLINE";
		case 0x20:
			return "NOOP";
		case 0x30:
			return "NEXTOID";
		case 0x40:
			return "SWITCH";
		case 0x50:
			return "BACKUP_END";
		case 0x60:
			return "PARAMETER_CHANGE";
		case 0x70:
			return "RESTORE_POINT";
		case 0x80:
			return "FPW_CHANGE";
		case 0x90:
			return "END_OF_RECOVERY";
		case 0xA0:
			return "FPI_FOR_HINT";
		case 0xB0:
			return "FPI";
		case 0xC0:
			return "FPI_MULTI";
		case 0xD0:
			return "OVERWRITE_CONTRECORD";
		case 0xE0:
			return "CHECKPOINT_REDO";
	}
	return "UNKNOWN";
}

static const char *
xact_info_name(uint8 info)
{
	switch (info & 0x70)
	{
		case 0x00:
			return "COMMIT";
		case 0x10:
			return "PREPARE";
		case 0x20:
			return "ABORT";
		case 0x30:
			return "COMMIT_PREPARED";
		case 0x40:
			return "ABORT_PREPARED";
		case 0x50:
			return "ASSIGNMENT";
		case 0x60:
			return "INVALIDATIONS";
	}
	return "UNKNOWN";
}

static const char *
standby_info_name(uint8 info)
{
	switch (info & 0xF0)
	{
		case 0x00:
			return "RUNNING_XACTS";
		case 0x10:
			return "LOCK";
		case 0x20:
			return "INVALIDATIONS";
		case 0x30:
			return "SNAPSHOT";
	}
	return "UNKNOWN";
}

static const char *
smgr_info_name(uint8 info)
{
	switch (info & 0xF0)
	{
		case 0x00:
			return "CREATE";
		case 0x10:
			return "TRUNCATE";
		case 0x20:
			return "EXTEND";
	}
	return "UNKNOWN";
}

static const char *
dbase_info_name(uint8 info)
{
	switch (info & 0xF0)
	{
		case 0x00:
			return "CREATE";
		case 0x10:
			return "DROP";
	}
	return "UNKNOWN";
}

static const char *
tblspc_info_name(uint8 info)
{
	switch (info & 0xF0)
	{
		case 0x00:
			return "CREATE";
		case 0x10:
			return "DROP";
	}
	return "UNKNOWN";
}

static const char *
seq_info_name(uint8 info)
{
	switch (info & 0xF0)
	{
		case 0x00:
			return "LOG";
	}
	return "UNKNOWN";
}

static const char *
logicalmsg_info_name(uint8 info)
{
	switch (info & 0xF0)
	{
		case 0x00:
			return "MESSAGE";
	}
	return "UNKNOWN";
}

static const char *
generic_info_name(uint8 info)
{
	switch (info & 0xF0)
	{
		case 0x00:
			return "MESSAGE";
	}
	return "UNKNOWN";
}

const char *
pwe_rmgr_info_name(uint8 rmid, uint8 info, int major)
{
	uint8		op = info & 0x70;

	switch (rmid)
	{
		case RM_XLOG_ID:
			return xlog_info_name(info, major);
		case RM_XACT_ID:
			return xact_info_name(info);
		case RM_SMGR_ID:
			return smgr_info_name(info);
		case RM_DBASE_ID:
			return dbase_info_name(info);
		case RM_TBLSPC_ID:
			return tblspc_info_name(info);
		case RM_STANDBY_ID:
			return standby_info_name(info);
		case RM_HEAP2_ID:
			return heap2_info_name(info, major);
		case RM_HEAP_ID:
			return heap_info_name(info);
		case RM_SEQ_ID:
			return seq_info_name(info);
		case RM_LOGICALMSG_ID:
			return logicalmsg_info_name(info);
		case RM_GENERIC_ID:
			return generic_info_name(info);
		case RM_BTREE_ID:
			switch (op)
			{
				case 0x00:
					return "INSERT_LEAF";
				case 0x10:
					return "INSERT_UPPER";
				case 0x20:
					return "INSERT_META";
				case 0x30:
					return "SPLIT_L";
				case 0x40:
					return "SPLIT_R";
				case 0x50:
					return "VACUUM";
				case 0x60:
					return "DELETE";
				case 0x70:
					return "MARK_PAGE_HALFDEAD";
			}
			return "UNKNOWN";
		case RM_HASH_ID:
			switch (op)
			{
				case 0x00:
					return "ADD_OVFL_PAGE";
				case 0x10:
					return "SPLIT_ALLOCATE";
				case 0x20:
					return "SPLIT_COMPLETE";
				case 0x30:
					return "INSERT";
				case 0x40:
					return "MOVE_PAGE";
				case 0x50:
					return "DELETE";
				case 0x60:
					return "INIT_PAGE";
				case 0x70:
					return "SPLIT_CLEANUP";
			}
			return "UNKNOWN";
		case RM_CLOG_ID:
			return "TRUNCATE";
		case RM_MULTIXACT_ID:
			switch (op)
			{
				case 0x00:
					return "ZEROPAGE";
				case 0x10:
					return "CREATE";
				case 0x20:
					return "EXTEND";
				case 0x30:
					return "TRUNCATE";
			}
			return "UNKNOWN";
		case RM_RELMAP_ID:
			return "UPDATE";
		case RM_COMMIT_TS_ID:
			return "TRUNCATE";
		case RM_REPLORIGIN_ID:
			return "SET";
		default:
			return "RECORD";
	}
}

/* ------------------------------------------------------------------ */
/* catalog layouts                                                    */
/* ------------------------------------------------------------------ */
static const struct
{
	const char *name;
	uint32		oid;
	const PweCatCols *cols;
} pwe_cat_defs[PWE_CAT_COUNT] = {
	{"pg_class", 1259, pwe_cols_pg_class},
	{"pg_attribute", 1249, pwe_cols_pg_attribute},
	{"pg_namespace", 2615, pwe_cols_pg_namespace},
	{"pg_type", 1247, pwe_cols_pg_type},
	{"pg_index", 2610, pwe_cols_pg_index},
	{"pg_constraint", 2606, pwe_cols_pg_constraint},
	{"pg_enum", 3501, pwe_cols_pg_enum},
	{"pg_sequence", 2224, pwe_cols_pg_sequence},
	{"pg_database", 1262, pwe_cols_pg_database},
	{"pg_authid", 1260, pwe_cols_pg_authid},
	{"pg_tablespace", 1213, pwe_cols_pg_tablespace},
	{"pg_attrdef", 2604, pwe_cols_pg_attrdef},
};

int
pwe_cat_lookup_oid(uint32 reloid)
{
	int			i;

	for (i = 0; i < PWE_CAT_COUNT; i++)
		if (pwe_cat_defs[i].oid == reloid)
			return i;
	return PWE_CAT_NONE;
}

const char *
pwe_cat_name(int cat)
{
	if (cat < 0 || cat >= PWE_CAT_COUNT)
		return "unknown";
	return pwe_cat_defs[cat].name;
}

uint32
pwe_cat_oid(int cat)
{
	if (cat < 0 || cat >= PWE_CAT_COUNT)
		return 0;
	return pwe_cat_defs[cat].oid;
}

const PweCatCols *
pwe_cat_layout(int cat, int major)
{
	int			i;

	if (cat < 0 || cat >= PWE_CAT_COUNT)
		return NULL;
	/* versions 9, 10, 11..19 in that order; an entry with n == 0 means the
	 * catalog does not exist yet in that major version (e.g. pg_sequence
	 * before 10) and is reported as absent. */
	for (i = 0; i < 11; i++)
		if (pwe_cat_defs[cat].cols[i].v == major)
			return pwe_cat_defs[cat].cols[i].n > 0 ? &pwe_cat_defs[cat].cols[i] : NULL;
	return NULL;
}

/* ------------------------------------------------------------------ */
/* built-in type fallback table                                       */
/* ------------------------------------------------------------------ */
typedef struct PweBuiltinType
{
	uint32		oid;
	const char *name;
	int16		typlen;
	bool		byval;
	char		align;
	char		typtype;
	uint32		typelem;
	uint32		typarray;
} PweBuiltinType;

static const PweBuiltinType pwe_builtin_types[] = {
	{16, "bool", 1, true, 'c', 'b', 0, 1000},
	{17, "bytea", -1, false, 'i', 'b', 0, 1001},
	{18, "char", 1, true, 'c', 'b', 0, 1002},
	{19, "name", 64, false, 'c', 'b', 0, 1003},
	{20, "int8", 8, true, 'd', 'b', 0, 1016},
	{21, "int2", 2, true, 's', 'b', 0, 1005},
	{22, "int2vector", -1, false, 'i', 'b', 21, 1006},
	{23, "int4", 4, true, 'i', 'b', 0, 1007},
	{24, "regproc", 4, true, 'i', 'b', 0, 1008},
	{25, "text", -1, false, 'i', 'b', 0, 1009},
	{26, "oid", 4, true, 'i', 'b', 0, 1028},
	{27, "tid", 6, false, 's', 'b', 0, 1010},
	{28, "xid", 4, true, 'i', 'b', 0, 1011},
	{29, "cid", 4, true, 'i', 'b', 0, 1012},
	{30, "oidvector", -1, false, 'i', 'b', 26, 1013},
	{114, "json", -1, false, 'i', 'b', 0, 199},
	{142, "xml", -1, false, 'i', 'b', 0, 143},
	{194, "pg_node_tree", -1, false, 'i', 'b', 0, 0},
	{600, "point", 16, false, 'd', 'b', 0, 1017},
	{601, "lseg", 32, false, 'd', 'b', 0, 1018},
	{602, "path", -1, false, 'd', 'b', 0, 1019},
	{603, "box", 32, false, 'd', 'b', 0, 1020},
	{604, "polygon", -1, false, 'd', 'b', 0, 1027},
	{628, "line", 24, false, 'd', 'b', 0, 629},
	{650, "cidr", -1, false, 'i', 'b', 0, 651},
	{700, "float4", 4, true, 'i', 'b', 0, 1021},
	{701, "float8", 8, true, 'd', 'b', 0, 1022},
	{705, "unknown", -1, false, 'c', 'p', 0, 0},
	{718, "circle", 24, false, 'd', 'b', 0, 719},
	{790, "money", 8, true, 'd', 'b', 0, 791},
	{829, "macaddr", 6, false, 'i', 'b', 0, 1040},
	{869, "inet", -1, false, 'i', 'b', 0, 1041},
	{1033, "aclitem", 12, false, 'i', 'b', 0, 1034},
	{1042, "bpchar", -1, false, 'i', 'b', 0, 1014},
	{1043, "varchar", -1, false, 'i', 'b', 0, 1015},
	{1082, "date", 4, true, 'i', 'b', 0, 1182},
	{1083, "time", 8, true, 'd', 'b', 0, 1183},
	{1114, "timestamp", 8, true, 'd', 'b', 0, 1115},
	{1184, "timestamptz", 8, true, 'd', 'b', 0, 1185},
	{1186, "interval", 16, false, 'd', 'b', 0, 1187},
	{1266, "timetz", 12, false, 'd', 'b', 0, 1270},
	{1560, "bit", -1, false, 'i', 'b', 0, 1561},
	{1562, "varbit", -1, false, 'i', 'b', 0, 1563},
	{1700, "numeric", -1, false, 'i', 'b', 0, 1231},
	{1790, "refcursor", -1, false, 'i', 'b', 0, 2201},
	{2202, "regprocedure", 4, true, 'i', 'b', 0, 2207},
	{2203, "regoper", 4, true, 'i', 'b', 0, 2208},
	{2204, "regoperator", 4, true, 'i', 'b', 0, 2209},
	{2205, "regclass", 4, true, 'i', 'b', 0, 2210},
	{2206, "regtype", 4, true, 'i', 'b', 0, 2211},
	{2249, "record", -1, false, 'd', 'p', 0, 2287},
	{2275, "cstring", -1, false, 'c', 'p', 0, 1263},
	{2276, "any", -1, false, 'c', 'p', 0, 0},
	{2277, "anyarray", -1, false, 'd', 'p', 0, 0},
	{2283, "anyelement", -1, false, 'c', 'p', 0, 0},
	{2950, "uuid", 16, false, 'c', 'b', 0, 2951},
	{3115, "fdw_handler", 4, true, 'i', 'p', 0, 0},
	{3220, "pg_lsn", 8, true, 'd', 'b', 0, 3221},
	{3310, "pg_ndistinct", -1, false, 'i', 'b', 0, 0},
	{3402, "pg_dependencies", -1, false, 'i', 'b', 0, 0},
	{3614, "tsvector", -1, false, 'i', 'b', 0, 3643},
	{3615, "tsquery", -1, false, 'i', 'b', 0, 3645},
	{3734, "regconfig", 4, true, 'i', 'b', 0, 3735},
	{3769, "regdictionary", 4, true, 'i', 'b', 0, 3770},
	{3802, "jsonb", -1, false, 'i', 'b', 0, 3807},
	{4072, "jsonpath", -1, false, 'i', 'b', 0, 4073},
	{4089, "regnamespace", 4, true, 'i', 'b', 0, 4090},
	{4096, "regrole", 4, true, 'i', 'b', 0, 4097},
	{4191, "regcollation", 4, true, 'i', 'b', 0, 4192},
	{4451, "int4range", -1, false, 'i', 'r', 0, 4452},
	{4532, "numrange", -1, false, 'i', 'r', 0, 4533},
	{4533, "tsrange", -1, false, 'i', 'r', 0, 4534},
	{4534, "tstzrange", -1, false, 'i', 'r', 0, 4535},
	{4535, "daterange", -1, false, 'i', 'r', 0, 4536},
	{4536, "int8range", -1, false, 'i', 'r', 0, 4537},
	{5069, "xid8", 8, true, 'd', 'b', 0, 271},
};

const void *
pwe_builtin_type_lookup(uint32 oid, const char **name, int16 *typlen, bool *byval,
						char *align, char *typtype, uint32 *typelem,
						uint32 *typarray)
{
	size_t		i;

	for (i = 0; i < sizeof(pwe_builtin_types) / sizeof(pwe_builtin_types[0]); i++)
	{
		const PweBuiltinType *t = &pwe_builtin_types[i];

		if (t->oid == oid)
		{
			if (name)
				*name = t->name;
			if (typlen)
				*typlen = t->typlen;
			if (byval)
				*byval = t->byval;
			if (align)
				*align = t->align;
			if (typtype)
				*typtype = t->typtype;
			if (typelem)
				*typelem = t->typelem;
			if (typarray)
				*typarray = t->typarray;
			return t;
		}
	}
	return NULL;
}

uint32
pwe_builtin_array_element(uint32 oid)
{
	size_t		i;

	for (i = 0; i < sizeof(pwe_builtin_types) / sizeof(pwe_builtin_types[0]); i++)
		if (pwe_builtin_types[i].typarray == oid)
			return pwe_builtin_types[i].oid;
	return 0;
}

int
pwe_cat_col(int cat, int major, int field)
{
	const PweCatCols *c = pwe_cat_layout(cat, major);

	if (!c || field < 0 || field >= 64)
		return 0;
	return c->map[field];
}

int
pwe_cat_natts(int cat, int major)
{
	const PweCatCols *c = pwe_cat_layout(cat, major);

	return c ? c->n : 0;
}

int
pwe_cat_oid_col(int cat, int major)
{
	const PweCatCols *c = pwe_cat_layout(cat, major);

	return c ? c->oid_col : 0;
}

static const char *const *pwe_types_table(int cat)
{
	switch (cat)
	{
		case PWE_CAT_PG_CLASS:
			return pwe_types_pg_class;
		case PWE_CAT_PG_ATTRIBUTE:
			return pwe_types_pg_attribute;
		case PWE_CAT_PG_NAMESPACE:
			return pwe_types_pg_namespace;
		case PWE_CAT_PG_TYPE:
			return pwe_types_pg_type;
		case PWE_CAT_PG_INDEX:
			return pwe_types_pg_index;
		case PWE_CAT_PG_CONSTRAINT:
			return pwe_types_pg_constraint;
		case PWE_CAT_PG_ENUM:
			return pwe_types_pg_enum;
		case PWE_CAT_PG_SEQUENCE:
			return pwe_types_pg_sequence;
		case PWE_CAT_PG_DATABASE:
			return pwe_types_pg_database;
		case PWE_CAT_PG_AUTHID:
			return pwe_types_pg_authid;
		case PWE_CAT_PG_TABLESPACE:
			return pwe_types_pg_tablespace;
		case PWE_CAT_PG_ATTRDEF:
			return pwe_types_pg_attrdef;
		default:
			return NULL;
	}
}

const char *
pwe_cat_typecodes(int cat, int major)
{
	const char *const *tbl = pwe_types_table(cat);

	if (!tbl)
		return NULL;
	if (major < 9 || major > 19)
		return NULL;
	return tbl[major - 9];
}
