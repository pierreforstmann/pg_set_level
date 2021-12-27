/*-------------------------------------------------------------------------
 *  
 * pg_set_level is a PostgreSQL extension which allows to customize 
 * SET statement for a specific parameter.
 *  
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *          
 * Copyright (c) 2020, Pierre Forstmann.
 *            
 *-------------------------------------------------------------------------
*/
#include "postgres.h"
#include "parser/analyze.h"
#include "nodes/nodes.h"
#include "storage/proc.h"
#include "access/xact.h"

#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "utils/memutils.h"
#if PG_VERSION_NUM <= 90600
#include "storage/lwlock.h"
#endif
#if PG_VERSION_NUM < 120000 
#include "access/transam.h"
#endif
#include "utils/varlena.h"
#include "utils/hsearch.h"

#include "utils/queryenvironment.h"
#include "tcop/cmdtag.h"

/*
 * parameters hash table:
 * shared by all back-end
 */
static const int pgsl_max = 100;

/*
 * select (max(length(name) from pg_settings = 38
 * add 1 for binary null ending character
 */
#define MAX_OPTION_NAME_LENGTH	39
typedef struct pgslHashKey {
   char	name[MAX_OPTION_NAME_LENGTH];
} pgslHashKey;

typedef struct pgslHashElem {
    pgslHashKey key;
} pgslHashElem;

static HTAB *pgsl_hashtable = NULL;

#include "nodes/nodes.h"

#include "storage/ipc.h"
#include "storage/spin.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "executor/executor.h"

PG_MODULE_MAGIC;

/*
 *
 * Global shared state
 * 
 */
typedef struct pgslSharedState
{
	LWLock	   	*lock;			/* self protection */
	bool		flag1;			/* flag1 : currently not used*/
} pgslSharedState;

/* Saved hook values in case of unload */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ProcessUtility_hook_type prev_process_utility_hook = NULL;

/* Links to shared memory state */
static pgslSharedState *pgsl= NULL;

static bool pgsl_enabled = true;

/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

static void pgsl_shmem_startup(void);
static void pgsl_shmem_shutdown(int code, Datum arg);
static void pgsl_exec(
#if PG_VERSION_NUM < 100000
		      Node *parsetree,
#else
		      PlannedStmt *pstmt,
#endif
		      const char *queryString,
#if PG_VERSION_NUM > 130000
                      bool readOnlyTree,
#endif
		      ProcessUtilityContext context,
		      ParamListInfo params,
#if PG_VERSION_NUM > 100000
	              QueryEnvironment *queryEnv,
#endif
		      DestReceiver *dest,
#if PG_VERSION_NUM < 130000
                      char *CompletionTag
#else
	              QueryCompletion *qc
#endif
);
static	char	*pg_set_level_names = NULL;	
static	char	*pg_set_level_action = NULL;	
static	char	*pg_set_level_default_action = "info";

typedef enum  {
        PGSL_INFO,
        PGSL_LOG,
        PGSL_NOTICE,
	PGSL_WARNING,
	PGSL_ERROR,
	PGSL_FATAL
} pgslAction;

static	pgslAction defaultAction;


/*
 ** Estimate shared memory space needed.
 *
 **/
static Size
pgsl_memsize(void)
{
	Size		size;

	size = MAXALIGN(sizeof(pgslSharedState));
	size = add_size(size, hash_estimate_size(pgsl_max, sizeof(pgslHashKey)));

	return size;
}


/*
 * shmem_startup hook: allocate or attach to shared memory.
 *
 */
static void
pgsl_shmem_startup(void)
{

	bool		found;
	HASHCTL 	hashctl;

	char 		*rawstring;
	List		*elemlist;
	ListCell	*l;
	bool		setting_list_is_ok = true;	
	const char	*return_string;

	elog(DEBUG5, "pg_set_level: pgsl_shmem_startup: entry");

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* reset in case this is a restart within the postmaster */
	pgsl = NULL;


	/*
 	** Create or attach to the shared memory state
 	**/
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pgsl = ShmemInitStruct("pg_set_level",
			        sizeof(pgslSharedState),
			        &found);

	if (!found)
	{
		/* First time through ... */
#if PG_VERSION_NUM <= 90600
		RequestAddinLWLocks(1);
		pgsl->lock = LWLockAssign();
#else
		pgsl->lock = &(GetNamedLWLockTranche("pg_set_level"))->lock;
#endif
	
		pgsl->flag1 = false;
	}

	LWLockRelease(AddinShmemInitLock);

	/*
         * If we're in the postmaster (or a standalone backend...), set up a shmem
         * exit hook (no current need ???) 
         */ 
        if (!IsUnderPostmaster)
		on_shmem_exit(pgsl_shmem_shutdown, (Datum) 0);

	/*
  	 * Done if some other process already completed our initialization.
  	 */
	if (found)
		return;

	/*
 	** create hash table for parameters
	*/

        memset(&hashctl, 0, sizeof(hashctl));
        hashctl.keysize = sizeof(pgslHashKey);
    	hashctl.entrysize = sizeof(pgslHashElem);
	pgsl_hashtable = ShmemInitHash("pg_set_level hash table", pgsl_max, pgsl_max, &hashctl,
				       HASH_ELEM );
        elog(DEBUG5, "pg_set_level: ShmemInitHash: OK");


	/*
 	 * check settings 
 	 */

	rawstring = pstrdup(pg_set_level_names);
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		elog(WARNING, "pg_set_level: list syntax is invalid");
		/* disable extension ... */
		setting_list_is_ok = false;
	}

	if (setting_list_is_ok == true) 
	{
		foreach (l, elemlist)
		{
			char	*tok = (char *)lfirst(l);

			return_string = GetConfigOption(tok, true, false);
			if (return_string == NULL)
			{
				elog(WARNING, "pg_set_level: %s is a unknown option", tok);
				setting_list_is_ok = false;
			}
			else
			{
				pgslHashKey key;
				bool found;
				pgslHashElem *elem;
				strcpy(key.name, tok);
				/*
 				** use HASH_ENTER to get valid memory pointer assigned to elem
				*/
				elem = hash_search(pgsl_hashtable, (void *)&key, HASH_ENTER, &found);	
				if (found)
		            		elog(DEBUG5, "pgsl_shmem_startup: Found entry %s before it was supposed to be added", key.name);
		        	else 
				{				
            				elog(DEBUG5, "pgsl_shmem_startup: %s entry added", key.name);
				}
				
			}
		}
	}


	pfree(rawstring);
	list_free(elemlist);

	/*
 	 * disable extension if some check has failed
 	 */

	if (setting_list_is_ok == true)
		elog(LOG, "pg_set_level: pg_set_level extension enabled");
	else
	{
		elog(LOG, "pg_set_level: pg_set_level extension disabled");
		pgsl_enabled = false;
	}

	elog(DEBUG5, "pg_set_level: pgsl_shmem_startup: exit");

}

/*
 *
 *  shmem_shutdown hook
 *   
 *  Note: we don't bother with acquiring lock, because there should be no
 *  other processes running when this is called.
 */
static void
pgsl_shmem_shutdown(int code, Datum arg)
{
	elog(DEBUG5, "pg_set_level: pgsl_shmem_shutdown: entry");

	/* Don't do anything during a crash. */
	if (code)
		return;

	/* Safety check ... shouldn't get here unless shmem is set up. */
	if (!pgsl)
		return;
	
	/* currently: no action */

	elog(DEBUG5, "pg_set_level: pgsl_shmem_shutdown: exit");
}


/*
 * Module load callback
 */
void
_PG_init(void)
{
	elog(DEBUG5, "pg_set_level: _PG_init(): entry");
	

	/* get the configuration */
	DefineCustomStringVariable("pg_set_level.names",
				"setting name list",
				NULL,
				&pg_set_level_names,
				NULL,
				PGC_POSTMASTER,
				0,
				NULL,
				NULL,
				NULL);
	DefineCustomStringVariable("pg_set_level.action",
				"setting action",
				NULL,
				&pg_set_level_action,
				NULL,
				PGC_POSTMASTER,
				0,
				NULL,
				NULL,
				NULL);
	if (pg_set_level_action == NULL)
		pg_set_level_action = pg_set_level_default_action;	

	if (strcmp(pg_set_level_action,"fatal") != 0 && 
	    strcmp(pg_set_level_action,"error") != 0 && 
            strcmp(pg_set_level_action,"warning") != 0 &&
            strcmp(pg_set_level_action,"notice") != 0 &&
            strcmp(pg_set_level_action,"log") != 0 &&
            strcmp(pg_set_level_action,"info") != 0) 
	{
		elog(WARNING, "unrecognized pg_set_level_action: %s", pg_set_level_action);
		pg_set_level_action = pg_set_level_default_action;	
	}
	if (strcmp(pg_set_level_action,"fatal") == 0)
		defaultAction = PGSL_FATAL;
	else if (strcmp(pg_set_level_action,"error") == 0)
		defaultAction = PGSL_ERROR;
	else if (strcmp(pg_set_level_action,"warning") == 0)
		defaultAction = PGSL_WARNING;	
	else if (strcmp(pg_set_level_action,"notice") == 0)
		defaultAction = PGSL_NOTICE;	
	else if (strcmp(pg_set_level_action,"log") == 0)
		defaultAction = PGSL_LOG;	
	else if (strcmp(pg_set_level_action,"info") == 0)
		defaultAction = PGSL_INFO;	

	elog(LOG, "pg_set_level:_PG_init(): pg_set_level extension detected");

	/*
 	 * cannot call GetConfigOptionByName because can trigger
	 * TRAP: BadState("OidIsValid(CurrentUserId)", File: "miscinit.c", Line: 450)
	 * when GetConfigOptionByName calls GetUserId
	 */

	
	/*
 	 * Request additional shared resources.  (These are no-ops if we're not in
 	 * the postmaster process.)  We'll allocate or attach to the shared
 	 * resources in pgsl_shmem_startup().
	 */
	RequestAddinShmemSpace(pgsl_memsize());
#if PG_VERSION_NUM >= 90600
	RequestNamedLWLockTranche("pg_set_level", 1);
#endif


	/*
 	 * Install hooks
	 */

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgsl_shmem_startup;
	prev_process_utility_hook = ProcessUtility_hook;
 	ProcessUtility_hook = pgsl_exec;	

	elog(DEBUG5, "pg_set_level: _PG_init(): exit");
}


/*
 *  Module unload callback
 */
void
_PG_fini(void)
{
	elog(DEBUG5, "pg_set_level: _PG_fini(): entry");

	/* Uninstall hooks. */
	shmem_startup_hook = prev_shmem_startup_hook;
	ProcessUtility_hook = prev_process_utility_hook;

	elog(DEBUG5, "pg_set_level: _PG_fini(): exit");
}

static void
pgsl_exec(
#if PG_VERSION_NUM < 100000
	  Node *parsetree,
#else
	  PlannedStmt *pstmt,
#endif
	  const char *queryString,
#if PG_VERSION_NUM > 130000
	  bool readOnlyTree,
#endif
	  ProcessUtilityContext context,
	  ParamListInfo params,
#if PG_VERSION_NUM > 100000
	  QueryEnvironment *queryEnv,
#endif
	  DestReceiver *dest,
#if PG_VERSION_NUM < 130000
	  char *CompletionTag)
#else
	  QueryCompletion *qc)
#endif

{
#if PG_VERSION_NUM > 100000
	Node	   	*parsetree;
#endif
	VariableSetStmt	*setstmt;

	elog(DEBUG1, "pg_set_level: pgsl_exec: entry");
#if PG_VERSION_NUM > 100000
	parsetree = pstmt->utilityStmt;
#endif

	if (nodeTag(parsetree) == T_VariableSetStmt)
	{
		setstmt = (VariableSetStmt *)parsetree;
		if (setstmt->kind == VAR_SET_VALUE || setstmt->kind == VAR_SET_CURRENT)
		{
			pgslHashKey key;
			pgslHashElem *elem;
                        bool found;

			elog(DEBUG1, "pg_set_level: pgsl_exec: setstmt->name=%s", setstmt->name);
                        strcpy(key.name, setstmt->name);
                        elem = hash_search(pgsl_hashtable, (void *)&key, HASH_FIND, &found);
			if (found)
			{
				elog(DEBUG1, "pg_set_level: pgsl_exec: setstmt->name=%s found", setstmt->name);
				if (defaultAction == PGSL_FATAL)
					elog(FATAL, "pg_set_level: %s", queryString);
				else if (defaultAction == PGSL_ERROR)
					elog(ERROR, "pg_set_level: %s", queryString);
				else if (defaultAction == PGSL_WARNING)
					elog(WARNING, "pg_set_level: %s", queryString);
				else if (defaultAction == PGSL_LOG)
					elog(LOG, "pg_set_level: %s", queryString);
				else if (defaultAction == PGSL_NOTICE)
					elog(NOTICE, "pg_set_level: %s", queryString);
				else if (defaultAction == PGSL_INFO)
					elog(INFO, "pg_set_level: %s", queryString);
			}
			else 
			{
				elog(DEBUG1, "pg_set_level: pgsl_exec: setstmt->name=%s not found", setstmt->name);
			}
			
		}
	}


	/*
 	 * see src/backend/tcop/utility.c
 	 */

	if (prev_process_utility_hook)

                (*prev_process_utility_hook) (
#if PG_VERSION_NUM < 100000
						  parsetree,
#else
						  pstmt, 
#endif
						  queryString,
#if PG_VERSION_NUM > 130000
						  readOnlyTree,
#endif
						  context, 
						  params,
#if PG_VERSION_NUM > 100000
						  queryEnv,
#endif
					   	  dest, 
#if PG_VERSION_NUM < 130000
						  CompletionTag);
#else
                                                  qc);
#endif
	else	standard_ProcessUtility(
#if PG_VERSION_NUM < 100000
					parsetree,
#else
					pstmt, 
#endif
					queryString,
#if PG_VERSION_NUM > 130000
					readOnlyTree,
#endif
				       	context,
					params, 
#if PG_VERSION_NUM > 100000
					queryEnv,
#endif
					dest, 
#if PG_VERSION_NUM < 130000
					CompletionTag);
#else
                                        qc);
#endif

	elog(DEBUG1, "pg_set_level: pgsl_exec: exit");
}

