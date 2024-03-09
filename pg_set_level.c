/*-------------------------------------------------------------------------
 *  
 * pg_set_level is a PostgreSQL extension which allows to customize 
 * SET statement for a specific parameter.
 *  
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *          
 * Copyright (c) 2020, 2021, 2022, 2023, 2024 Pierre Forstmann.
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
#if PG_VERSION_NUM > 130000
#include "tcop/cmdtag.h"
#endif

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
#define MAX_ACTION_NAME_LENGTH	8
typedef struct pgslHashKey {
   char	name[MAX_OPTION_NAME_LENGTH];
} pgslHashKey;

typedef struct pgslHashElem {
    pgslHashKey key;
	int action;
} pgslHashElem;

static HTAB *pgsl_hashtable = NULL;
/* Saved hook values in case of unload */

#include "nodes/nodes.h"

#include "storage/ipc.h"
#include "storage/spin.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "executor/executor.h"
#include "catalog/objectaccess.h"

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
#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type prev_shmem_request_hook = NULL;
#endif

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ProcessUtility_hook_type prev_process_utility_hook = NULL;

static object_access_hook_type prev_object_access_hook = NULL;

/* Links to shared memory state */
static pgslSharedState *pgsl= NULL;

static bool pgsl_enabled = true;

/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

static void pgsl_shmem_startup(void);
static void pgsl_exec(
#if PG_VERSION_NUM < 100000
		      Node *parsetree,
#else
		      PlannedStmt *pstmt,
#endif
		      const char *queryString,
#if PG_VERSION_NUM >= 140000
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

static void pgsl_object_access_hook(ObjectAccessType access,
				    Oid classId,
				    Oid objectId,
				    int subId,
				    void *arg);


/* --- */

static	char	*pg_set_level_names = NULL;	
static	char	*pg_set_level_actions = NULL;	

typedef enum  {
    PGSL_INFO,
    PGSL_LOG,
    PGSL_NOTICE,
	PGSL_WARNING,
	PGSL_ERROR,
	PGSL_FATAL
} pgslAction;

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
 * pgsl_shmem_startup
 */
static void
pgsl_shmem_startup(void)
{

	HASHCTL 	hashctl;
	bool		shmem_found;

	char 		*rawstring_names;
	char		*rawstring_actions;
	List		*elemlist_names;
	List        *elemlist_actions;
	ListCell	*l_name;
	ListCell    *l_action;
	bool		setting_list_is_ok = true;	
	const char	*return_string;
	int			action;

	elog(LOG, "pg_set_level: pgsl_shmem_startup: entry");

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* reset in case this is a restart within the postmaster */
	pgsl = NULL;

	/*
 	 * Create or attach to the shared memory state
 	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pgsl = ShmemInitStruct("pg_set_level",
				pgsl_memsize(),
			        &shmem_found);

	if (!shmem_found)
	{

		/* First time through ... */
#if PG_VERSION_NUM <= 90600
		RequestAddinLWLocks(1);
		pgsl->lock = LWLockAssign();
#else
		pgsl->lock = &(GetNamedLWLockTranche("pg_set_level"))->lock;
#endif

		LWLockRelease(AddinShmemInitLock);

		/*
 		** create hash table for parameters
		*/

        memset(&hashctl, 0, sizeof(hashctl));
	    hashctl.keysize = sizeof(pgslHashKey);
	    hashctl.entrysize = sizeof(pgslHashElem);
		pgsl_hashtable = ShmemInitHash("pg_set_level hash table", pgsl_max, pgsl_max, &hashctl,
#if PG_VERSION_NUM < 140000 
					       HASH_ELEM);
#else
					       HASH_ELEM | HASH_STRINGS);
#endif


		/*
 		 * check settings 
	 	 */

		rawstring_names = pstrdup(pg_set_level_names);
		if (!SplitIdentifierString(rawstring_names, ',', &elemlist_names))
		{
			/* syntax error in list */
			elog(WARNING, "pg_set_level: pg_set_level.names list syntax is invalid");
			/* disable extension ... */
			setting_list_is_ok = false;
		}
		rawstring_actions = pstrdup(pg_set_level_actions);
		if (!SplitIdentifierString(rawstring_actions, ',', &elemlist_actions))
		{
			/* syntax error in list */
			elog(WARNING, "pg_set_level: pg_set_level.actions list syntax is invalid");
			/* disable extension ... */
			setting_list_is_ok = false;
		}

		if (list_length(elemlist_names) != list_length(elemlist_actions))
		{
			elog(LOG, "pg_set_level: pg_set_level.names and pg_set_level.actions mismatch");
			setting_list_is_ok = false;
		}

		if (setting_list_is_ok == true) 
		{
			forboth (l_name, elemlist_names, l_action, elemlist_actions)
			{
				char	*tok_name = (char *)lfirst(l_name);
				char	*tok_action = (char *)lfirst(l_action);
			

				return_string = GetConfigOption(tok_name, true, false);
				if (return_string == NULL)
				{
					elog(WARNING, "pg_set_level: %s is a unknown parameter", tok_name);
					setting_list_is_ok = false;
				};
		
				if (strcmp(tok_action,"fatal") != 0 && 
					strcmp(tok_action,"error") != 0 && 
					strcmp(tok_action,"warning") != 0 &&
					strcmp(tok_action,"notice") != 0 &&
					strcmp(tok_action,"log") != 0 &&
					strcmp(tok_action,"info") != 0) 
				{
					elog(WARNING, "unrecognized action : %s", tok_action);
					setting_list_is_ok = false;
				}
				
				action = INFO;
				if (strcmp(tok_action, "fatal") == 0)
					action = FATAL;
				else if (strcmp(tok_action, "error") == 0)
					action = ERROR;
				else if (strcmp(tok_action, "warning") == 0)
					action = WARNING;
				else if (strcmp(tok_action, "notice") == 0)
					action = NOTICE;
				else if (strcmp(tok_action, "log") == 0)
					action = LOG;
				else if (strcmp(tok_action, "info") == 0)
					action = INFO;

				if (setting_list_is_ok == true)
				{
					pgslHashKey key;
					bool found;
					pgslHashElem *elem;
					strcpy(key.name, tok_name);
					
					/*
 					** use HASH_ENTER to get valid memory pointer assigned to elem
					*/
					elem = hash_search(pgsl_hashtable, (void *)&key, HASH_ENTER, &found);	
					if (found)
					{
			            		elog(DEBUG1, "pgsl_shmem_startup: Found entry %s before it was supposed to be added", key.name);
					}
		        		else 
					{			
        	    				elog(LOG, "pgsl_shmem_startup: %s - %d entry added", key.name, action);
								elem->action = action;
					}
					/*
					 * make compiler happy
				 	*/
					elem = elem; 
				
				}	
			}
		}

		pfree(rawstring_names);
		pfree(rawstring_actions);
		list_free(elemlist_names);
		list_free(elemlist_actions);
	}



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

	elog(LOG, "pg_set_level: pgsl_shmem_startup: exit");

}

/*
 * shmen_request_hook
 *
 */
static void
pgsl_shmem_request(void)
{

	elog(LOG, "pg_set_level: pgsl_shmem_request(): entry");

	/*
 	 * Request additional shared resources.  (These are no-ops if we're not in
 	 * the postmaster process.)  We'll allocate or attach to the shared
 	 * resources in pgsl_shmem_startup().
	 */

#if PG_VERSION_NUM >= 150000
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
#endif

	RequestAddinShmemSpace(pgsl_memsize());
#if PG_VERSION_NUM >= 90600
	RequestNamedLWLockTranche("pg_set_level", 1);
#endif

	elog(LOG, "pg_set_level: pgsl_shmem_request(): exit");
}



/*
 * Module load callback
 */
void
_PG_init(void)
{
	elog(LOG, "pg_set_level:_PG_init(): entry");
	

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

	if (pg_set_level_names == NULL)
	{
		/*
		 * 	if pg_set_level.names is not set, pgsl_shmem_startup fails
		 */
		elog(LOG, "pg_set_level:_PG_init(): missing parameter pg_set_level.names");
		pgsl_enabled = false;
	}	

	DefineCustomStringVariable("pg_set_level.actions",
				"setting action",
				NULL,
				&pg_set_level_actions,
				NULL,
				PGC_POSTMASTER,
				0,
				NULL,
				NULL,
				NULL);
	if (pg_set_level_actions == NULL)
	{
		elog(LOG, "pg_set_level:_PG_init(): missing parameter pg_set_level.actions");
		pgsl_enabled = false;
	}

	/*
 	 * cannot call GetConfigOptionByName because can trigger
	 * TRAP: BadState("OidIsValid(CurrentUserId)", File: "miscinit.c", Line: 450)
	 * when GetConfigOptionByName calls GetUserId
	 */

	
	/*
 	 * Install hooks
	 */

	if (pgsl_enabled == true)
	{

#if PG_VERSION_NUM >= 150000
		prev_shmem_request_hook = shmem_request_hook;
		shmem_request_hook = pgsl_shmem_request;
#else
		pgsl_shmem_request();
#endif
		prev_shmem_startup_hook = shmem_startup_hook;
		shmem_startup_hook = pgsl_shmem_startup;

		prev_process_utility_hook = ProcessUtility_hook;
 		ProcessUtility_hook = pgsl_exec;	

		prev_object_access_hook = object_access_hook;
		object_access_hook = pgsl_object_access_hook;

		/* set_config oid = 2078 */
	}

	if (pgsl_enabled == false)
	{
		elog(LOG, "pg_set_level:_PG_init(): pg_set_level is not enabled");
	}
	elog(LOG, "pg_set_level:_PG_init(): exit");
}


/*
 *  Module unload callback
 */
void
_PG_fini(void)
{
	elog(LOG, "pg_set_level: _PG_fini(): entry");

	/* Uninstall hooks. */
	shmem_startup_hook = prev_shmem_startup_hook;
	ProcessUtility_hook = prev_process_utility_hook;

	elog(LOG, "pg_set_level: _PG_fini(): exit");
}

static void
pgsl_exec(
#if PG_VERSION_NUM < 100000
	  Node *parsetree,
#else
	  PlannedStmt *pstmt,
#endif
	  const char *queryString,
#if PG_VERSION_NUM >= 140000
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
				elog(DEBUG1, "pg_set_level: pgsl_exec: setstmt->name=%s action=%d found", 
								setstmt->name, elem->action);
				elog(elem->action, "pg_set_level: %s", queryString);
			}
			else 
			{
				elog(DEBUG1, "pg_set_level: pgsl_exec: setstmt->name=%s not found", setstmt->name);
			}
			/*
			 * make compiler happy
			 */
			elem = elem;
			
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
#if PG_VERSION_NUM >= 140000
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
#if PG_VERSION_NUM >= 140000
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

static void
pgsl_object_access_hook(ObjectAccessType access,
		        Oid classId,
			Oid objectId,
			int subId,
			void *arg)
{
	if (superuser() == false && access == OAT_FUNCTION_EXECUTE && objectId == 2078)
		elog(ERROR, "pgsl_object_access_hook: OAT_FUNCTION_EXECUTE set_config: access denied.");

}
