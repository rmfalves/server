/* Copyright (C) 2000 MySQL AB & MySQL Finland AB & TCX DataKonsult AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */


/* Handler-calling-functions */

#ifdef __GNUC__
#pragma implementation				// gcc: Class implementation
#endif

#include "mysql_priv.h"
#include "ha_heap.h"
#include "ha_myisam.h"
#include "ha_myisammrg.h"
#ifdef HAVE_ISAM
#include "ha_isam.h"
#include "ha_isammrg.h"
#endif
#ifdef HAVE_BERKELEY_DB
#include "ha_berkeley.h"
#endif
#ifdef HAVE_EXAMPLE_DB
#include "examples/ha_example.h"
#endif
#ifdef HAVE_ARCHIVE_DB
#include "examples/ha_archive.h"
#endif
#ifdef HAVE_CSV_DB
#include "examples/ha_tina.h"
#endif
#ifdef HAVE_INNOBASE_DB
#include "ha_innodb.h"
#endif
#ifdef HAVE_NDBCLUSTER_DB
#include "ha_ndbcluster.h"
#endif
#ifdef HAVE_FEDERATED_DB
#include "ha_federated.h"
#endif
#include <myisampack.h>
#include <errno.h>

	/* static functions defined in this file */

static int NEAR_F delete_file(const char *name,const char *ext,int extflag);

static SHOW_COMP_OPTION have_yes= SHOW_OPTION_YES;

handlerton *handlertons[MAX_HA]={0};
ulong total_ha, total_ha_2pc, savepoint_alloc_size;

struct show_table_type_st sys_table_types[]=
{
  {"MyISAM",	&have_yes,
   "Default engine as of MySQL 3.23 with great performance", DB_TYPE_MYISAM},
  {"HEAP",	&have_yes,
   "Alias for MEMORY", DB_TYPE_HEAP},
  {"MEMORY",	&have_yes,
   "Hash based, stored in memory, useful for temporary tables", DB_TYPE_HEAP},
  {"MERGE",	&have_yes,
   "Collection of identical MyISAM tables", DB_TYPE_MRG_MYISAM},
  {"MRG_MYISAM",&have_yes,
   "Alias for MERGE", DB_TYPE_MRG_MYISAM},
  {"ISAM",	&have_isam,
   "Obsolete storage engine, now replaced by MyISAM", DB_TYPE_ISAM},
  {"MRG_ISAM",  &have_isam,
   "Obsolete storage engine, now replaced by MERGE", DB_TYPE_MRG_ISAM},
  {"InnoDB",	&have_innodb,
   "Supports transactions, row-level locking, and foreign keys", DB_TYPE_INNODB},
  {"INNOBASE",	&have_innodb,
   "Alias for INNODB", DB_TYPE_INNODB},
  {"BDB",	&have_berkeley_db,
   "Supports transactions and page-level locking", DB_TYPE_BERKELEY_DB},
  {"BERKELEYDB",&have_berkeley_db,
   "Alias for BDB", DB_TYPE_BERKELEY_DB},
  {"NDBCLUSTER", &have_ndbcluster,
   "Clustered, fault-tolerant, memory-based tables", DB_TYPE_NDBCLUSTER},
  {"NDB", &have_ndbcluster,
   "Alias for NDBCLUSTER", DB_TYPE_NDBCLUSTER},
  {"EXAMPLE",&have_example_db,
   "Example storage engine", DB_TYPE_EXAMPLE_DB},
  {"ARCHIVE",&have_archive_db,
   "Archive storage engine", DB_TYPE_ARCHIVE_DB},
  {"CSV",&have_csv_db,
   "CSV storage engine", DB_TYPE_CSV_DB},
  {"FEDERATED",&have_federated_db,
   "Federated MySQL storage engine", DB_TYPE_FEDERATED_DB},
  {NullS, NULL, NullS, DB_TYPE_UNKNOWN}
};

const char *ha_row_type[] = {
  "", "FIXED", "DYNAMIC", "COMPRESSED","?","?","?"
};

const char *tx_isolation_names[] =
{ "READ-UNCOMMITTED", "READ-COMMITTED", "REPEATABLE-READ", "SERIALIZABLE",
  NullS};
TYPELIB tx_isolation_typelib= {array_elements(tx_isolation_names)-1,"",
			       tx_isolation_names, NULL};

static TYPELIB known_extensions= {0,"known_exts", NULL, NULL};
uint known_extensions_id= 0;

enum db_type ha_resolve_by_name(const char *name, uint namelen)
{
  THD *thd= current_thd;
  if (thd && !my_strcasecmp(&my_charset_latin1, name, "DEFAULT")) {
    return (enum db_type) thd->variables.table_type;
  }
  
  show_table_type_st *types;
  for (types= sys_table_types; types->type; types++)
  {
    if (!my_strcasecmp(&my_charset_latin1, name, types->type))
      return (enum db_type) types->db_type;
  }
  return DB_TYPE_UNKNOWN;
}

const char *ha_get_storage_engine(enum db_type db_type)
{
  show_table_type_st *types;
  for (types= sys_table_types; types->type; types++)
  {
    if (db_type == types->db_type)
      return types->type;
  }
  
  return "none";
}

	/* Use other database handler if databasehandler is not incompiled */

enum db_type ha_checktype(enum db_type database_type)
{
  show_table_type_st *types;
  THD *thd= current_thd;
  for (types= sys_table_types; types->type; types++)
  {
    if ((database_type == types->db_type) && 
	(*types->value == SHOW_OPTION_YES))
      return database_type;
  }

  switch (database_type) {
#ifndef NO_HASH
  case DB_TYPE_HASH:
    return (database_type);
#endif
  case DB_TYPE_MRG_ISAM:
    return (DB_TYPE_MRG_MYISAM);
  default:
    break;
  }
  
  return ((enum db_type) thd->variables.table_type != DB_TYPE_UNKNOWN ?
          (enum db_type) thd->variables.table_type :
          (enum db_type) global_system_variables.table_type !=
          DB_TYPE_UNKNOWN ?
          (enum db_type) global_system_variables.table_type : DB_TYPE_MYISAM);
} /* ha_checktype */


handler *get_new_handler(TABLE *table, enum db_type db_type)
{
  switch (db_type) {
#ifndef NO_HASH
  case DB_TYPE_HASH:
    return new ha_hash(table);
#endif
#ifdef HAVE_ISAM
  case DB_TYPE_MRG_ISAM:
    return new ha_isammrg(table);
  case DB_TYPE_ISAM:
    return new ha_isam(table);
#else
  case DB_TYPE_MRG_ISAM:
    return new ha_myisammrg(table);
#endif
#ifdef HAVE_BERKELEY_DB
  case DB_TYPE_BERKELEY_DB:
    return new ha_berkeley(table);
#endif
#ifdef HAVE_INNOBASE_DB
  case DB_TYPE_INNODB:
    return new ha_innobase(table);
#endif
#ifdef HAVE_EXAMPLE_DB
  case DB_TYPE_EXAMPLE_DB:
    return new ha_example(table);
#endif
#ifdef HAVE_ARCHIVE_DB
  case DB_TYPE_ARCHIVE_DB:
    return new ha_archive(table);
#endif
#ifdef HAVE_FEDERATED_DB
  case DB_TYPE_FEDERATED_DB:
    return new ha_federated(table);
#endif
#ifdef HAVE_CSV_DB
  case DB_TYPE_CSV_DB:
    return new ha_tina(table);
#endif
#ifdef HAVE_NDBCLUSTER_DB
  case DB_TYPE_NDBCLUSTER:
    return new ha_ndbcluster(table);
#endif
  case DB_TYPE_HEAP:
    return new ha_heap(table);
  default:					// should never happen
  {
    enum db_type def=(enum db_type) current_thd->variables.table_type;
    /* Try first with 'default table type' */
    if (db_type != def)
      return get_new_handler(table, def);
  }
  /* Fall back to MyISAM */
  case DB_TYPE_MYISAM:
    return new ha_myisam(table);
  case DB_TYPE_MRG_MYISAM:
    return new ha_myisammrg(table);
  }
}

bool ha_caching_allowed(THD* thd, char* table_key,
                        uint key_length, uint8 cache_type)
{
#ifdef HAVE_INNOBASE_DB
  if (cache_type == HA_CACHE_TBL_ASKTRANSACT)
    return innobase_query_caching_of_table_permitted(thd, table_key, key_length);
#endif
  return 1;
}

static inline void ha_was_inited_ok(handlerton **ht)
{
  uint tmp= (*ht)->savepoint_offset;
  (*ht)->savepoint_offset= savepoint_alloc_size;
  savepoint_alloc_size+= tmp;
  (*ht)->slot= total_ha++;
  if ((*ht)->prepare)
    total_ha_2pc++;
}

int ha_init()
{
  int error= 0;
  handlerton **ht= handlertons;
  total_ha= savepoint_alloc_size= 0;

  if (opt_bin_log)
  {
    if (!(*ht= binlog_init()))
    {
      mysql_bin_log.close(LOG_CLOSE_INDEX);
      opt_bin_log= 0;
      error= 1;
    }
    else
      ha_was_inited_ok(ht++);
  }
#ifdef HAVE_BERKELEY_DB
  if (have_berkeley_db == SHOW_OPTION_YES)
  {
    if (!(*ht= berkeley_init()))
    {
      have_berkeley_db= SHOW_OPTION_DISABLED;	// If we couldn't use handler
      error= 1;
    }
    else
      ha_was_inited_ok(ht++);
  }
#endif
#ifdef HAVE_INNOBASE_DB
  if (have_innodb == SHOW_OPTION_YES)
  {
    if (!(*ht= innobase_init()))
    {
      have_innodb= SHOW_OPTION_DISABLED;	// If we couldn't use handler
      error= 1;
    }
    else
      ha_was_inited_ok(ht++);
  }
#endif
#ifdef HAVE_NDBCLUSTER_DB
  if (have_ndbcluster == SHOW_OPTION_YES)
  {
    if (!(*ht= ndbcluster_init()))
    {
      have_ndbcluster= SHOW_OPTION_DISABLED;
      error= 1;
    }
    else
      ha_was_inited_ok(ht++);
  }
#endif
  DBUG_ASSERT(total_ha < MAX_HA);
  opt_using_transactions= total_ha>opt_bin_log;
  savepoint_alloc_size+= sizeof(SAVEPOINT);
  return error;
}

	/* close, flush or restart databases */
	/* Ignore this for other databases than ours */

int ha_panic(enum ha_panic_function flag)
{
  int error=0;
#ifndef NO_HASH
  error|=h_panic(flag);			/* fix hash */
#endif
#ifdef HAVE_ISAM
  error|=mrg_panic(flag);
  error|=nisam_panic(flag);
#endif
  error|=heap_panic(flag);
  error|=mi_panic(flag);
  error|=myrg_panic(flag);
#ifdef HAVE_BERKELEY_DB
  if (have_berkeley_db == SHOW_OPTION_YES)
    error|=berkeley_end();
#endif
#ifdef HAVE_INNOBASE_DB
  if (have_innodb == SHOW_OPTION_YES)
    error|=innobase_end();
#endif
#ifdef HAVE_NDBCLUSTER_DB
  if (have_ndbcluster == SHOW_OPTION_YES)
    error|=ndbcluster_end();
#endif
  return error;
} /* ha_panic */

void ha_drop_database(char* path)
{
#ifdef HAVE_INNOBASE_DB
  if (have_innodb == SHOW_OPTION_YES)
    innobase_drop_database(path);
#endif
#ifdef HAVE_NDBCLUSTER_DB
  if (have_ndbcluster == SHOW_OPTION_YES)
    ndbcluster_drop_database(path);
#endif
}

/* don't bother to rollback here, it's done already */
void ha_close_connection(THD* thd)
{
  for (uint i=0; i < total_ha; i++)
    if (thd->ha_data[i])
      (*handlertons[i]->close_connection)(thd);
}

/* ========================================================================
 ======================= TRANSACTIONS ===================================*/

void trans_register_ha(THD *thd, bool all, handlerton *ht_arg)
{
  THD_TRANS *trans;
  if (all)
  {
    trans= &thd->transaction.all;
    thd->server_status|= SERVER_STATUS_IN_TRANS;
  }
  else
    trans= &thd->transaction.stmt;

#ifndef DBUG_OFF
  handlerton **ht=trans->ht;
  while (*ht)
  {
    DBUG_ASSERT(*ht != ht_arg);
    ht++;
  }
#endif
  trans->ht[trans->nht++]=ht_arg;
  trans->no_2pc|=(ht_arg->prepare==0);
  if (thd->transaction.xid.is_null())
    thd->transaction.xid.set(thd->query_id);
}

/*
  RETURN
     -1  - cannot prepare
      0  - ok
      1  - error, transaction was rolled back
*/
int ha_prepare(THD *thd)
{
  int error=0, all=1;
  THD_TRANS *trans=all ? &thd->transaction.all : &thd->transaction.stmt;
  handlerton **ht=trans->ht;
  DBUG_ENTER("ha_prepare");
#ifdef USING_TRANSACTIONS
  if (trans->nht)
  {
    if (trans->no_2pc)
      return -1;
    for (; *ht; ht++)
    {
      int err;
      statistic_increment(thd->status_var.ha_prepare_count,&LOCK_status);
      if ((err= (*(*ht)->prepare)(thd, all)))
      {
        my_error(ER_ERROR_DURING_COMMIT, MYF(0), err);
        ha_rollback_trans(thd, all);
        error=1;
        break;
      }
    }
  }
#endif /* USING_TRANSACTIONS */
  DBUG_RETURN(error);
}

/*
  RETURN
      0  - ok
      1  - transaction was rolled back
      2  - error during commit, data may be inconsistent
*/
int ha_commit_trans(THD *thd, bool all)
{
  int error= 0, cookie= 0;
  THD_TRANS *trans= all ? &thd->transaction.all : &thd->transaction.stmt;
  bool is_real_trans= all || thd->transaction.all.nht == 0;
  handlerton **ht= trans->ht;
  DBUG_ENTER("ha_commit_trans");
#ifdef USING_TRANSACTIONS
  if (trans->nht)
  {
    if (!trans->no_2pc && trans->nht > 1)
    {
      for (; *ht && !error; ht++)
      {
        int err;
        if ((err= (*(*ht)->prepare)(thd, all)))
        {
          my_error(ER_ERROR_DURING_COMMIT, MYF(0), err);
          error=1;
        }
        statistic_increment(thd->status_var.ha_prepare_count,&LOCK_status);
      }
      if (error || (is_real_trans &&
                    (error= !(cookie= tc_log->log(thd,
                                thd->transaction.xid.quick_get_my_xid())))))
      {
        ha_rollback_trans(thd, all);
        return 1;
      }
    }
    error=ha_commit_one_phase(thd, all) ? cookie ? 2 : 1 : 0;
    if (cookie)
      tc_log->unlog(cookie, thd->transaction.xid.quick_get_my_xid());
  }
#endif /* USING_TRANSACTIONS */
  DBUG_RETURN(error);
}

int ha_commit_one_phase(THD *thd, bool all)
{
  int error=0;
  THD_TRANS *trans=all ? &thd->transaction.all : &thd->transaction.stmt;
  bool is_real_trans=all || thd->transaction.all.nht == 0;
  handlerton **ht=trans->ht;
  DBUG_ENTER("ha_commit_one_phase");
#ifdef USING_TRANSACTIONS
  if (trans->nht)
  {
    bool need_start_waiters= 0;
    if (is_real_trans)
    {
      if ((error= wait_if_global_read_lock(thd, 0, 0)))
      {
        my_error(ER_SERVER_SHUTDOWN, MYF(0)); // we're killed
        error= 1;
      }
      else
        need_start_waiters= 1;
    }

    for (ht=trans->ht; *ht; ht++)
    {
      int err;
      if ((err= (*(*ht)->commit)(thd, all)))
      {
        my_error(ER_ERROR_DURING_COMMIT, MYF(0), err);
        error=1;
      }
      statistic_increment(thd->status_var.ha_commit_count,&LOCK_status);
      *ht= 0;
    }
    trans->nht=0;
    trans->no_2pc=0;
    if (is_real_trans)
      thd->transaction.xid.null();
    if (all)
    {
#ifdef HAVE_QUERY_CACHE
      if (thd->transaction.changed_tables)
        query_cache.invalidate(thd->transaction.changed_tables);
#endif
      thd->variables.tx_isolation=thd->session_tx_isolation;
      thd->transaction.cleanup();
    }
    if (need_start_waiters)
      start_waiting_global_read_lock(thd);
  }
#endif /* USING_TRANSACTIONS */
  DBUG_RETURN(error);
}


int ha_rollback_trans(THD *thd, bool all)
{
  int error=0;
  THD_TRANS *trans=all ? &thd->transaction.all : &thd->transaction.stmt;
  bool is_real_trans=all || thd->transaction.all.nht == 0;
  DBUG_ENTER("ha_rollback_trans");
#ifdef USING_TRANSACTIONS
  if (trans->nht)
  {
    for (handlerton **ht=trans->ht; *ht; ht++)
    {
      int err;
      if ((err= (*(*ht)->rollback)(thd, all)))
      { // cannot happen
        my_error(ER_ERROR_DURING_ROLLBACK, MYF(0), err);
        error=1;
      }
      statistic_increment(thd->status_var.ha_rollback_count,&LOCK_status);
      *ht= 0;
    }
    trans->nht=0;
    trans->no_2pc=0;
    if (is_real_trans)
      thd->transaction.xid.null();
    if (all)
    {
      thd->variables.tx_isolation=thd->session_tx_isolation;
      thd->transaction.cleanup();
    }
  }
#endif /* USING_TRANSACTIONS */
  /*
    If a non-transactional table was updated, warn; don't warn if this is a
    slave thread (because when a slave thread executes a ROLLBACK, it has
    been read from the binary log, so it's 100% sure and normal to produce
    error ER_WARNING_NOT_COMPLETE_ROLLBACK. If we sent the warning to the
    slave SQL thread, it would not stop the thread but just be printed in
    the error log; but we don't want users to wonder why they have this
    message in the error log, so we don't send it.
  */
  if (is_real_trans && (thd->options & OPTION_STATUS_NO_TRANS_UPDATE) &&
      !thd->slave_thread)
    push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                 ER_WARNING_NOT_COMPLETE_ROLLBACK,
                 ER(ER_WARNING_NOT_COMPLETE_ROLLBACK));
  DBUG_RETURN(error);
}

/*
  This is used to commit or rollback a single statement depending on the value
  of error. Note that if the autocommit is on, then the following call inside
  InnoDB will commit or rollback the whole transaction (= the statement). The
  autocommit mechanism built into InnoDB is based on counting locks, but if
  the user has used LOCK TABLES then that mechanism does not know to do the
  commit.
*/

int ha_autocommit_or_rollback(THD *thd, int error)
{
  DBUG_ENTER("ha_autocommit_or_rollback");
#ifdef USING_TRANSACTIONS
  if (thd->transaction.stmt.nht)
  {
    if (!error)
    {
      if (ha_commit_stmt(thd))
	error=1;
    }
    else
      (void) ha_rollback_stmt(thd);

    thd->variables.tx_isolation=thd->session_tx_isolation;
  }
#endif
  DBUG_RETURN(error);
}

int ha_commit_or_rollback_by_xid(LEX_STRING *ident, bool commit)
{
  XID xid;
  handlerton **ht= handlertons, **end_ht=ht+total_ha;
  int res= 1;

  xid.set(ident);
  for ( ; ht < end_ht ; ht++)
    if ((*ht)->recover)
      res= res &&
        (*(commit ? (*ht)->commit_by_xid : (*ht)->rollback_by_xid))(&xid);
  return res;
}

/*
  recover() step of xa
*/
int ha_recover(HASH *commit_list)
{
  int error= 0, len, got;
  handlerton **ht= handlertons, **end_ht=ht+total_ha;
  XID *list=0;
  DBUG_ENTER("ha_recover");

  DBUG_ASSERT(total_ha_2pc);
  DBUG_ASSERT(commit_list || tc_heuristic_recover);

  for (len=commit_list ? commit_list->records : MAX_XID_LIST_SIZE ;
       list==0 && len > MIN_XID_LIST_SIZE; len/=2)
  {
    list=(XID *)my_malloc(len*sizeof(XID), MYF(0));
  }
  if (!list)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), len);
    DBUG_RETURN(1);
  }

  for ( ; ht < end_ht ; ht++)
  {
    if (!(*ht)->recover)
      continue;
    while ((got=(*(*ht)->recover)(list, len)) > 0 )
    {
      for (int i=0; i < got; i ++)
      {
        my_xid x=list[i].get_my_xid();
        if (!x) // not "mine" - that is generated by external TM
          continue;
        if (commit_list ?
            hash_search(commit_list, (char *)&x, sizeof(x)) != 0 :
            tc_heuristic_recover == TC_HEURISTIC_RECOVER_COMMIT)
          (*(*ht)->commit_by_xid)(list+i);
        else
          (*(*ht)->rollback_by_xid)(list+i);
      }
      if (got < len)
        break;
    }
  }
  my_free((gptr)list, MYF(0));
  DBUG_RETURN(0);
}

/*
  return the list of XID's to a client, the same way SHOW commands do

  NOTE
    I didn't find in XA specs that an RM cannot return the same XID twice,
    so mysql_xa_recover does not filter XID's to ensure uniqueness.
    It can be easily fixed later, if necessary.
*/
bool mysql_xa_recover(THD *thd)
{
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  handlerton **ht= handlertons, **end_ht=ht+total_ha;
  bool error=TRUE;
  int len, got;
  XID *list=0;
  DBUG_ENTER("mysql_xa_recover");

  field_list.push_back(new Item_int("formatID",0,11));
  field_list.push_back(new Item_int("gtrid_length",0,11));
  field_list.push_back(new Item_int("bqual_length",0,11));
  field_list.push_back(new Item_empty_string("data",XIDDATASIZE));

  if (protocol->send_fields(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  for (len= MAX_XID_LIST_SIZE ; list==0 && len > MIN_XID_LIST_SIZE; len/=2)
  {
    list=(XID *)my_malloc(len*sizeof(XID), MYF(0));
  }
  if (!list)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), len);
    DBUG_RETURN(1);
  }

  for ( ; ht < end_ht ; ht++)
  {
    if (!(*ht)->recover)
      continue;
    while ((got=(*(*ht)->recover)(list, len)) > 0 )
    {
      XID *xid, *end;
      for (xid=list, end=list+got; xid < end; xid++)
      {
        if (xid->get_my_xid())
          continue; // skip "our" xids
        protocol->prepare_for_resend();
        protocol->store_long((longlong)xid->formatID);
        protocol->store_long((longlong)xid->gtrid_length);
        protocol->store_long((longlong)xid->bqual_length);
        protocol->store(xid->data, xid->gtrid_length+xid->bqual_length,
                        &my_charset_bin);
        if (protocol->write())
          goto err;
      }
      if (got < len)
        break;
    }
  }

  error=FALSE;
  send_eof(thd);
err:
  my_free((gptr)list, MYF(0));
  DBUG_RETURN(error);
}

/*
  This function should be called when MySQL sends rows of a SELECT result set
  or the EOF mark to the client. It releases a possible adaptive hash index
  S-latch held by thd in InnoDB and also releases a possible InnoDB query
  FIFO ticket to enter InnoDB. To save CPU time, InnoDB allows a thd to
  keep them over several calls of the InnoDB handler interface when a join
  is executed. But when we let the control to pass to the client they have
  to be released because if the application program uses mysql_use_result(),
  it may deadlock on the S-latch if the application on another connection
  performs another SQL query. In MySQL-4.1 this is even more important because
  there a connection can have several SELECT queries open at the same time.

  arguments:
  thd:           the thread handle of the current connection
  return value:  always 0
*/

int ha_release_temporary_latches(THD *thd)
{
#ifdef HAVE_INNOBASE_DB
  innobase_release_temporary_latches(thd);
#endif
  return 0;
}


/* 
  Export statistics for different engines. Currently we use it only for
  InnoDB.
*/

int ha_update_statistics()
{
#ifdef HAVE_INNOBASE_DB
  if (opt_innodb)
    innodb_export_status();
#endif
  return 0;
}

int ha_rollback_to_savepoint(THD *thd, SAVEPOINT *sv)
{
  int error=0;
  THD_TRANS *trans=&thd->transaction.all;
  handlerton **ht=trans->ht, **end_ht;
  DBUG_ENTER("ha_rollback_to_savepoint");
  DBUG_ASSERT(thd->transaction.stmt.ht[0] == 0);

  trans->nht=sv->nht;
  trans->no_2pc=0;
  end_ht=ht+sv->nht;
  /*
    rolling back to savepoint in all storage engines that were part of the
    transaction when the savepoint was set
  */
  for (; ht < end_ht; ht++)
  {
    int err;
    DBUG_ASSERT((*ht)->savepoint_set);
    if ((err= (*(*ht)->savepoint_rollback)(thd, (byte *)(sv+1)+(*ht)->savepoint_offset)))
    { // cannot happen
      my_error(ER_ERROR_DURING_ROLLBACK, MYF(0), err);
      error=1;
    }
    statistic_increment(thd->status_var.ha_savepoint_rollback_count,&LOCK_status);
    trans->no_2pc|=(*ht)->prepare == 0;
  }
  /*
    rolling back the transaction in all storage engines that were not part of
    the transaction when the savepoint was set
  */
  for (; *ht ; ht++)
  {
    int err;
    if ((err= (*(*ht)->rollback)(thd, 1)))
    { // cannot happen
      my_error(ER_ERROR_DURING_ROLLBACK, MYF(0), err);
      error=1;
    }
    statistic_increment(thd->status_var.ha_rollback_count,&LOCK_status);
    *ht=0; // keep it conveniently zero-filled
  }
  DBUG_RETURN(error);
}

/*
  note, that according to the sql standard (ISO/IEC 9075-2:2003)
  section "4.33.4 SQL-statements and transaction states",
  SAVEPOINT is *not* transaction-initiating SQL-statement
*/

int ha_savepoint(THD *thd, SAVEPOINT *sv)
{
  int error=0;
  THD_TRANS *trans=&thd->transaction.all;
  handlerton **ht=trans->ht;
  DBUG_ENTER("ha_savepoint");
  DBUG_ASSERT(thd->transaction.stmt.ht[0] == 0);
#ifdef USING_TRANSACTIONS
  for (; *ht; ht++)
  {
    int err;
    if (! (*ht)->savepoint_set)
    {
      my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), "SAVEPOINT");
      error=1;
      break;
    }
    if ((err= (*(*ht)->savepoint_set)(thd, (byte *)(sv+1)+(*ht)->savepoint_offset)))
    { // cannot happen
      my_error(ER_GET_ERRNO, MYF(0), err);
      error=1;
    }
    statistic_increment(thd->status_var.ha_savepoint_count,&LOCK_status);
  }
  sv->nht=trans->nht;
#endif /* USING_TRANSACTIONS */
  DBUG_RETURN(error);
}

int ha_release_savepoint(THD *thd, SAVEPOINT *sv)
{
  int error=0;
  handlerton **ht=thd->transaction.all.ht, **end_ht;
  DBUG_ENTER("ha_release_savepoint");
  DBUG_ASSERT(thd->transaction.stmt.ht[0] == 0);

  end_ht=ht+sv->nht;
  for (; ht < end_ht; ht++)
  {
    int err;
    if (!(*ht)->savepoint_release)
      continue;
    if ((err= (*(*ht)->savepoint_release)(thd, (byte *)(sv+1)+(*ht)->savepoint_offset)))
    { // cannot happen
      my_error(ER_GET_ERRNO, MYF(0), err);
      error=1;
    }
  }
  DBUG_RETURN(error);
}


int ha_start_consistent_snapshot(THD *thd)
{
#ifdef HAVE_INNOBASE_DB
  if ((have_innodb == SHOW_OPTION_YES) &&
      !innobase_start_trx_and_assign_read_view(thd))
    return 0;
#endif
  /*
    Same idea as when one wants to CREATE TABLE in one engine which does not
    exist:
  */
  push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
               "This MySQL server does not support any "
               "consistent-read capable storage engine");
  return 0;
}


bool ha_flush_logs()
{
  bool result=0;
#ifdef HAVE_BERKELEY_DB
  if ((have_berkeley_db == SHOW_OPTION_YES) &&
      berkeley_flush_logs())
    result=1;
#endif
#ifdef HAVE_INNOBASE_DB
  if ((have_innodb == SHOW_OPTION_YES) &&
      innobase_flush_logs())
    result=1;
#endif
  return result;
}

/*
  This should return ENOENT if the file doesn't exists.
  The .frm file will be deleted only if we return 0 or ENOENT
*/

int ha_delete_table(enum db_type table_type, const char *path)
{
  handler *file;
  char tmp_path[FN_REFLEN];

  /* DB_TYPE_UNKNOWN is used in ALTER TABLE when renaming only .frm files */
  if (table_type == DB_TYPE_UNKNOWN ||
      ! (file=get_new_handler((TABLE*) 0, table_type)))
    return ENOENT;

  if (lower_case_table_names == 2 && !(file->table_flags() & HA_FILE_BASED))
  {
    /* Ensure that table handler get path in lower case */
    strmov(tmp_path, path);
    my_casedn_str(files_charset_info, tmp_path);
    path= tmp_path;
  }
  int error=file->delete_table(path);
  delete file;
  return error;
}

/****************************************************************************
** General handler functions
****************************************************************************/

	/* Open database-handler. Try O_RDONLY if can't open as O_RDWR */
	/* Don't wait for locks if not HA_OPEN_WAIT_IF_LOCKED is set */

int handler::ha_open(const char *name, int mode, int test_if_locked)
{
  int error;
  DBUG_ENTER("handler::ha_open");
  DBUG_PRINT("enter",("name: %s  db_type: %d  db_stat: %d  mode: %d  lock_test: %d",
		      name, table->db_type, table->db_stat, mode,
		      test_if_locked));

  if ((error=open(name,mode,test_if_locked)))
  {
    if ((error == EACCES || error == EROFS) && mode == O_RDWR &&
	(table->db_stat & HA_TRY_READ_ONLY))
    {
      table->db_stat|=HA_READ_ONLY;
      error=open(name,O_RDONLY,test_if_locked);
    }
  }
  if (error)
  {
    my_errno=error;			/* Safeguard */
    DBUG_PRINT("error",("error: %d  errno: %d",error,errno));
  }
  else
  {
    if (table->db_options_in_use & HA_OPTION_READ_ONLY_DATA)
      table->db_stat|=HA_READ_ONLY;
    (void) extra(HA_EXTRA_NO_READCHECK);	// Not needed in SQL

    if (!alloc_root_inited(&table->mem_root))	// If temporary table
      ref=(byte*) sql_alloc(ALIGN_SIZE(ref_length)*2);
    else
      ref=(byte*) alloc_root(&table->mem_root, ALIGN_SIZE(ref_length)*2);
    if (!ref)
    {
      close();
      error=HA_ERR_OUT_OF_MEM;
    }
    else
      dupp_ref=ref+ALIGN_SIZE(ref_length);
  }
  DBUG_RETURN(error);
}

/*
  Read first row (only) from a table
  This is never called for InnoDB or BDB tables, as these table types
  has the HA_NOT_EXACT_COUNT set.
*/

int handler::read_first_row(byte * buf, uint primary_key)
{
  register int error;
  DBUG_ENTER("handler::read_first_row");

  statistic_increment(current_thd->status_var.ha_read_first_count,&LOCK_status);

  /*
    If there is very few deleted rows in the table, find the first row by
    scanning the table.
    TODO remove the test for HA_READ_ORDER
  */
  if (deleted < 10 || primary_key >= MAX_KEY ||
      !(index_flags(primary_key, 0, 0) & HA_READ_ORDER))
  {
    (void) ha_rnd_init(1);
    while ((error= rnd_next(buf)) == HA_ERR_RECORD_DELETED) ;
    (void) ha_rnd_end();
  }
  else
  {
    /* Find the first row through the primary key */
    (void) ha_index_init(primary_key);
    error=index_first(buf);
    (void) ha_index_end();
  }
  DBUG_RETURN(error);
}


/*
  Generate the next auto-increment number based on increment and offset
  
  In most cases increment= offset= 1, in which case we get:
  1,2,3,4,5,...
  If increment=10 and offset=5 and previous number is 1, we get:
  1,5,15,25,35,...
*/

inline ulonglong
next_insert_id(ulonglong nr,struct system_variables *variables)
{
  nr= (((nr+ variables->auto_increment_increment -
         variables->auto_increment_offset)) /
       (ulonglong) variables->auto_increment_increment);
  return (nr* (ulonglong) variables->auto_increment_increment +
          variables->auto_increment_offset);
}


/*
  Updates columns with type NEXT_NUMBER if:

  - If column value is set to NULL (in which case
    auto_increment_field_not_null is 0)
  - If column is set to 0 and (sql_mode & MODE_NO_AUTO_VALUE_ON_ZERO) is not
    set. In the future we will only set NEXT_NUMBER fields if one sets them
    to NULL (or they are not included in the insert list).


  There are two different cases when the above is true:

  - thd->next_insert_id == 0  (This is the normal case)
    In this case we set the set the column for the first row to the value
    next_insert_id(get_auto_increment(column))) which is normally
    max-used-column-value +1.

    We call get_auto_increment() only for the first row in a multi-row
    statement. For the following rows we generate new numbers based on the
    last used number.

  - thd->next_insert_id != 0.  This happens when we have read a statement
    from the binary log or when one has used SET LAST_INSERT_ID=#.

    In this case we will set the column to the value of next_insert_id.
    The next row will be given the id
    next_insert_id(next_insert_id)

    The idea is that generated auto_increment values are predictable and
    independent of the column values in the table.  This is needed to be
    able to replicate into a table that already has rows with a higher
    auto-increment value than the one that is inserted.

    After we have already generated an auto-increment number and the user
    inserts a column with a higher value than the last used one, we will
    start counting from the inserted value.

    thd->next_insert_id is cleared after it's been used for a statement.
*/

void handler::update_auto_increment()
{
  ulonglong nr;
  THD *thd= table->in_use;
  struct system_variables *variables= &thd->variables;
  DBUG_ENTER("handler::update_auto_increment");

  /*
    We must save the previous value to be able to restore it if the
    row was not inserted
  */
  thd->prev_insert_id= thd->next_insert_id;

  if ((nr= table->next_number_field->val_int()) != 0 ||
      table->auto_increment_field_not_null &&
      thd->variables.sql_mode & MODE_NO_AUTO_VALUE_ON_ZERO)
  {
    /* Clear flag for next row */
    table->auto_increment_field_not_null= FALSE;
    /* Mark that we didn't generate a new value **/
    auto_increment_column_changed=0;

    /* Update next_insert_id if we have already generated a value */
    if (thd->clear_next_insert_id && nr >= thd->next_insert_id)
    {
      if (variables->auto_increment_increment != 1)
        nr= next_insert_id(nr, variables);
      else
        nr++;
      thd->next_insert_id= nr;
      DBUG_PRINT("info",("next_insert_id: %lu", (ulong) nr));
    }
    DBUG_VOID_RETURN;
  }
  table->auto_increment_field_not_null= FALSE;
  if (!(nr= thd->next_insert_id))
  {
    nr= get_auto_increment();
    if (variables->auto_increment_increment != 1)
      nr= next_insert_id(nr-1, variables);
    /*
      Update next row based on the found value. This way we don't have to
      call the handler for every generated auto-increment value on a
      multi-row statement
    */
    thd->next_insert_id= nr;
  }

  DBUG_PRINT("info",("auto_increment: %lu", (ulong) nr));

  /* Mark that we should clear next_insert_id before next stmt */
  thd->clear_next_insert_id= 1;

  if (!table->next_number_field->store((longlong) nr))
    thd->insert_id((ulonglong) nr);
  else
    thd->insert_id(table->next_number_field->val_int());

  /*
    We can't set next_insert_id if the auto-increment key is not the
    first key part, as there is no guarantee that the first parts will be in
    sequence
  */
  if (!table->next_number_key_offset)
  {
    /*
      Set next insert id to point to next auto-increment value to be able to
      handle multi-row statements
      This works even if auto_increment_increment > 1
    */
    thd->next_insert_id= next_insert_id(nr, variables);
  }
  else
    thd->next_insert_id= 0;

  /* Mark that we generated a new value */
  auto_increment_column_changed=1;
  DBUG_VOID_RETURN;
}

/*
  restore_auto_increment

  In case of error on write, we restore the last used next_insert_id value
  because the previous value was not used.
*/

void handler::restore_auto_increment()
{
  THD *thd= table->in_use;
  if (thd->next_insert_id)
    thd->next_insert_id= thd->prev_insert_id;
}


ulonglong handler::get_auto_increment()
{
  ulonglong nr;
  int error;

  (void) extra(HA_EXTRA_KEYREAD);
  index_init(table->next_number_index);
  if (!table->next_number_key_offset)
  {						// Autoincrement at key-start
    error=index_last(table->record[1]);
  }
  else
  {
    byte key[MAX_KEY_LENGTH];
    key_copy(key, table->record[0],
             table->key_info + table->next_number_index,
             table->next_number_key_offset);
    error=index_read(table->record[1], key, table->next_number_key_offset,
                     HA_READ_PREFIX_LAST);
  }

  if (error)
    nr=1;
  else
    nr=((ulonglong) table->next_number_field->
        val_int_offset(table->rec_buff_length)+1);
  index_end();
  (void) extra(HA_EXTRA_NO_KEYREAD);
  return nr;
}

	/* Print error that we got from handler function */

void handler::print_error(int error, myf errflag)
{
  DBUG_ENTER("handler::print_error");
  DBUG_PRINT("enter",("error: %d",error));

  int textno=ER_GET_ERRNO;
  switch (error) {
  case EACCES:
    textno=ER_OPEN_AS_READONLY;
    break;
  case EAGAIN:
    textno=ER_FILE_USED;
    break;
  case ENOENT:
    textno=ER_FILE_NOT_FOUND;
    break;
  case HA_ERR_KEY_NOT_FOUND:
  case HA_ERR_NO_ACTIVE_RECORD:
  case HA_ERR_END_OF_FILE:
    textno=ER_KEY_NOT_FOUND;
    break;
  case HA_ERR_WRONG_MRG_TABLE_DEF:
    textno=ER_WRONG_MRG_TABLE;
    break;
  case HA_ERR_FOUND_DUPP_KEY:
  {
    uint key_nr=get_dup_key(error);
    if ((int) key_nr >= 0)
    {
      /* Write the dupplicated key in the error message */
      char key[MAX_KEY_LENGTH];
      String str(key,sizeof(key),system_charset_info);
      key_unpack(&str,table,(uint) key_nr);
      uint max_length=MYSQL_ERRMSG_SIZE-(uint) strlen(ER(ER_DUP_ENTRY));
      if (str.length() >= max_length)
      {
	str.length(max_length-4);
	str.append("...");
      }
      my_error(ER_DUP_ENTRY, MYF(0), str.c_ptr(), key_nr+1);
      DBUG_VOID_RETURN;
    }
    textno=ER_DUP_KEY;
    break;
  }
  case HA_ERR_FOUND_DUPP_UNIQUE:
    textno=ER_DUP_UNIQUE;
    break;
  case HA_ERR_RECORD_CHANGED:
    textno=ER_CHECKREAD;
    break;
  case HA_ERR_CRASHED:
    textno=ER_NOT_KEYFILE;
    break;
  case HA_ERR_CRASHED_ON_USAGE:
    textno=ER_CRASHED_ON_USAGE;
    break;
  case HA_ERR_CRASHED_ON_REPAIR:
    textno=ER_CRASHED_ON_REPAIR;
    break;
  case HA_ERR_OUT_OF_MEM:
    my_message(ER_OUT_OF_RESOURCES, ER(ER_OUT_OF_RESOURCES), errflag);
    DBUG_VOID_RETURN;
  case HA_ERR_WRONG_COMMAND:
    textno=ER_ILLEGAL_HA;
    break;
  case HA_ERR_OLD_FILE:
    textno=ER_OLD_KEYFILE;
    break;
  case HA_ERR_UNSUPPORTED:
    textno=ER_UNSUPPORTED_EXTENSION;
    break;
  case HA_ERR_RECORD_FILE_FULL:
    textno=ER_RECORD_FILE_FULL;
    break;
  case HA_ERR_LOCK_WAIT_TIMEOUT:
    textno=ER_LOCK_WAIT_TIMEOUT;
    break;
  case HA_ERR_LOCK_TABLE_FULL:
    textno=ER_LOCK_TABLE_FULL;
    break;
  case HA_ERR_LOCK_DEADLOCK:
    textno=ER_LOCK_DEADLOCK;
    break;
  case HA_ERR_READ_ONLY_TRANSACTION:
    textno=ER_READ_ONLY_TRANSACTION;
    break;
  case HA_ERR_CANNOT_ADD_FOREIGN:
    textno=ER_CANNOT_ADD_FOREIGN;
    break;
  case HA_ERR_ROW_IS_REFERENCED:
    textno=ER_ROW_IS_REFERENCED;
    break;
  case HA_ERR_NO_REFERENCED_ROW:
    textno=ER_NO_REFERENCED_ROW;
    break;
  case HA_ERR_NO_SUCH_TABLE:
  {
    /*
      We have to use path to find database name instead of using
      table->table_cache_key because if the table didn't exist, then
      table_cache_key was not set up
    */
    char *db;
    char buff[FN_REFLEN];
    uint length=dirname_part(buff,table->path);
    buff[length-1]=0;
    db=buff+dirname_length(buff);
    my_error(ER_NO_SUCH_TABLE, MYF(0), db, table->table_name);
    break;
  }
  default:
    {
      /* The error was "unknown" to this function.
	 Ask handler if it has got a message for this error */
      bool temporary= FALSE;
      String str;
      temporary= get_error_message(error, &str);
      if (!str.is_empty())
      {
	const char* engine= table_type();
	if (temporary)
	  my_error(ER_GET_TEMPORARY_ERRMSG, MYF(0), error, str.ptr(), engine);
	else
	  my_error(ER_GET_ERRMSG, MYF(0), error, str.ptr(), engine);
      }
      else
	my_error(ER_GET_ERRNO,errflag,error);
      DBUG_VOID_RETURN;
    }
  }
  my_error(textno, errflag, table->table_name, error);
  DBUG_VOID_RETURN;
}


/*
   Return an error message specific to this handler

   SYNOPSIS
   error        error code previously returned by handler
   buf          Pointer to String where to add error message
   
   Returns true if this is a temporary error
 */

bool handler::get_error_message(int error, String* buf)
{
  return FALSE;
}


/* Return key if error because of duplicated keys */

uint handler::get_dup_key(int error)
{
  DBUG_ENTER("handler::get_dup_key");
  table->file->errkey  = (uint) -1;
  if (error == HA_ERR_FOUND_DUPP_KEY || error == HA_ERR_FOUND_DUPP_UNIQUE)
    info(HA_STATUS_ERRKEY | HA_STATUS_NO_LOCK);
  DBUG_RETURN(table->file->errkey);
}


int handler::delete_table(const char *name)
{
  int error=0;
  for (const char **ext=bas_ext(); *ext ; ext++)
  {
    if (delete_file(name,*ext,2))
    {
      if ((error=errno) != ENOENT)
	break;
    }
  }
  return error;
}


int handler::rename_table(const char * from, const char * to)
{
  DBUG_ENTER("handler::rename_table");
  for (const char **ext=bas_ext(); *ext ; ext++)
  {
    if (rename_file_ext(from,to,*ext))
      DBUG_RETURN(my_errno);
  }
  DBUG_RETURN(0);
}

/*
  Tell the handler to turn on or off transaction in the handler
*/

int ha_enable_transaction(THD *thd, bool on)
{
  int error=0;

  DBUG_ENTER("ha_enable_transaction");
  thd->transaction.on= on;
  DBUG_RETURN(error);
}

int handler::index_next_same(byte *buf, const byte *key, uint keylen)
{
  int error;
  if (!(error=index_next(buf)))
  {
    if (key_cmp_if_same(table, key, active_index, keylen))
    {
      table->status=STATUS_NOT_FOUND;
      error=HA_ERR_END_OF_FILE;
    }
  }
  return error;
}


/****************************************************************************
** Some general functions that isn't in the handler class
****************************************************************************/

/*
  Initiates table-file and calls apropriate database-creator
  Returns 1 if something got wrong
*/

int ha_create_table(const char *name, HA_CREATE_INFO *create_info,
		    bool update_create_info)
{
  int error;
  TABLE table;
  char name_buff[FN_REFLEN];
  DBUG_ENTER("ha_create_table");

  if (openfrm(current_thd, name,"",0,(uint) READ_ALL, 0, &table))
    DBUG_RETURN(1);
  if (update_create_info)
  {
    update_create_info_from_table(create_info, &table);
  }
  if (lower_case_table_names == 2 &&
      !(table.file->table_flags() & HA_FILE_BASED))
  {
    /* Ensure that handler gets name in lower case */
    strmov(name_buff, name);
    my_casedn_str(files_charset_info, name_buff);
    name= name_buff;
  }

  error=table.file->create(name,&table,create_info);
  VOID(closefrm(&table));
  if (error)
    my_error(ER_CANT_CREATE_TABLE, MYF(ME_BELL+ME_WAITTANG), name,error);
  DBUG_RETURN(error != 0);
}

/*
  Try to discover table from engine and 
  if found, write the frm file to disk.
  
  RETURN VALUES:
   0 : Table existed in engine and created 
       on disk if so requested
   1 : Table does not exist
  >1 : error

*/

int ha_create_table_from_engine(THD* thd, 
				const char *db, 
				const char *name,
				bool create_if_found)
{
  int error;
  const void *frmblob;
  uint frmlen;
  char path[FN_REFLEN];
  HA_CREATE_INFO create_info;
  TABLE table;
  DBUG_ENTER("ha_create_table_from_engine");
  DBUG_PRINT("enter", ("name '%s'.'%s'  create_if_found: %d",
                       db, name, create_if_found));

  bzero((char*) &create_info,sizeof(create_info));

  if ((error= ha_discover(thd, db, name, &frmblob, &frmlen)))
    DBUG_RETURN(error); 
  /*
    Table exists in handler
    frmblob and frmlen are set
  */

  if (create_if_found)
  {
    (void)strxnmov(path,FN_REFLEN,mysql_data_home,"/",db,"/",name,NullS);
    // Save the frm file    
    if ((error = writefrm(path, frmblob, frmlen)))
      goto err_end;

    if (openfrm(thd, path,"",0,(uint) READ_ALL, 0, &table))
      DBUG_RETURN(1);

    update_create_info_from_table(&create_info, &table);
    create_info.table_options|= HA_CREATE_FROM_ENGINE;

    if (lower_case_table_names == 2 &&
	!(table.file->table_flags() & HA_FILE_BASED))
    {
      /* Ensure that handler gets name in lower case */
      my_casedn_str(files_charset_info, path);
    }
    
    error=table.file->create(path,&table,&create_info);
    VOID(closefrm(&table));
  }

err_end:
  my_free((char*) frmblob, MYF(MY_ALLOW_ZERO_PTR));
  DBUG_RETURN(error);  
}

static int NEAR_F delete_file(const char *name,const char *ext,int extflag)
{
  char buff[FN_REFLEN];
  VOID(fn_format(buff,name,"",ext,extflag | 4));
  return(my_delete_with_symlink(buff,MYF(MY_WME)));
}

void st_ha_check_opt::init()
{
  flags= sql_flags= 0;
  sort_buffer_size = current_thd->variables.myisam_sort_buff_size;
}


/*****************************************************************************
  Key cache handling.

  This code is only relevant for ISAM/MyISAM tables

  key_cache->cache may be 0 only in the case where a key cache is not
  initialized or when we where not able to init the key cache in a previous
  call to ha_init_key_cache() (probably out of memory)
*****************************************************************************/

/* Init a key cache if it has not been initied before */


int ha_init_key_cache(const char *name, KEY_CACHE *key_cache)
{
  DBUG_ENTER("ha_init_key_cache");

  if (!key_cache->key_cache_inited)
  {
    pthread_mutex_lock(&LOCK_global_system_variables);
    long tmp_buff_size= (long) key_cache->param_buff_size;
    long tmp_block_size= (long) key_cache->param_block_size;
    uint division_limit= key_cache->param_division_limit;
    uint age_threshold=  key_cache->param_age_threshold;
    pthread_mutex_unlock(&LOCK_global_system_variables);
    DBUG_RETURN(!init_key_cache(key_cache,
				tmp_block_size,
				tmp_buff_size,
				division_limit, age_threshold));
  }
  DBUG_RETURN(0);
}


/* Resize key cache */

int ha_resize_key_cache(KEY_CACHE *key_cache)
{
  DBUG_ENTER("ha_resize_key_cache");

  if (key_cache->key_cache_inited)
  {
    pthread_mutex_lock(&LOCK_global_system_variables);
    long tmp_buff_size= (long) key_cache->param_buff_size;
    long tmp_block_size= (long) key_cache->param_block_size;
    uint division_limit= key_cache->param_division_limit;
    uint age_threshold=  key_cache->param_age_threshold;
    pthread_mutex_unlock(&LOCK_global_system_variables);
    DBUG_RETURN(!resize_key_cache(key_cache, tmp_block_size,
				  tmp_buff_size,
				  division_limit, age_threshold));
  }
  DBUG_RETURN(0);
}


/* Change parameters for key cache (like size) */

int ha_change_key_cache_param(KEY_CACHE *key_cache)
{
  if (key_cache->key_cache_inited)
  {
    pthread_mutex_lock(&LOCK_global_system_variables);
    uint division_limit= key_cache->param_division_limit;
    uint age_threshold=  key_cache->param_age_threshold;
    pthread_mutex_unlock(&LOCK_global_system_variables);
    change_key_cache_param(key_cache, division_limit, age_threshold);
  }
  return 0;
}

/* Free memory allocated by a key cache */

int ha_end_key_cache(KEY_CACHE *key_cache)
{
  end_key_cache(key_cache, 1);		// Can never fail
  return 0;
}

/* Move all tables from one key cache to another one */

int ha_change_key_cache(KEY_CACHE *old_key_cache,
			KEY_CACHE *new_key_cache)
{
  mi_change_key_cache(old_key_cache, new_key_cache);
  return 0;
}


/*
  Try to discover one table from handler(s)

  RETURN
    0  ok. In this case *frmblob and *frmlen are set
    1  error.  frmblob and frmlen may not be set
*/

int ha_discover(THD *thd, const char *db, const char *name,
		const void **frmblob, uint *frmlen)
{
  int error= 1; // Table does not exist in any handler
  DBUG_ENTER("ha_discover");
  DBUG_PRINT("enter", ("db: %s, name: %s", db, name));
#ifdef HAVE_NDBCLUSTER_DB
  if (have_ndbcluster == SHOW_OPTION_YES)
    error= ndbcluster_discover(thd, db, name, frmblob, frmlen);
#endif
  if (!error)
    statistic_increment(thd->status_var.ha_discover_count,&LOCK_status);
  DBUG_RETURN(error);
}


/*
  Call this function in order to give the handler the possiblity 
  to ask engine if there are any new tables that should be written to disk 
  or any dropped tables that need to be removed from disk
*/

int
ha_find_files(THD *thd,const char *db,const char *path,
	      const char *wild, bool dir, List<char> *files)
{
  int error= 0;
  DBUG_ENTER("ha_find_files");
  DBUG_PRINT("enter", ("db: %s, path: %s, wild: %s, dir: %d", 
		       db, path, wild, dir));
#ifdef HAVE_NDBCLUSTER_DB
  if (have_ndbcluster == SHOW_OPTION_YES)
    error= ndbcluster_find_files(thd, db, path, wild, dir, files);
#endif
  DBUG_RETURN(error);
  
  
}

#ifdef NOT_YET_USED

/*
  Ask handler if the table exists in engine

  RETURN
    0                   Table does not exist
    1                   Table exists
    #                   Error code

 */
int ha_table_exists(THD* thd, const char* db, const char* name)
{
  int error= 2;
  DBUG_ENTER("ha_table_exists");
  DBUG_PRINT("enter", ("db: %s, name: %s", db, name));
#ifdef HAVE_NDBCLUSTER_DB
  if (have_ndbcluster == SHOW_OPTION_YES)
    error= ndbcluster_table_exists(thd, db, name);
#endif
  DBUG_RETURN(error);
}

#endif


/*
  Read first row between two ranges.
  Store ranges for future calls to read_range_next

  SYNOPSIS
    read_range_first()
    start_key		Start key. Is 0 if no min range
    end_key		End key.  Is 0 if no max range
    eq_range_arg	Set to 1 if start_key == end_key		
    sorted		Set to 1 if result should be sorted per key

  NOTES
    Record is read into table->record[0]

  RETURN
    0			Found row
    HA_ERR_END_OF_FILE	No rows in range
    #			Error code
*/

int handler::read_range_first(const key_range *start_key,
			      const key_range *end_key,
			      bool eq_range_arg, bool sorted)
{
  int result;
  DBUG_ENTER("handler::read_range_first");

  eq_range= eq_range_arg;
  end_range= 0;
  if (end_key)
  {
    end_range= &save_end_range;
    save_end_range= *end_key;
    key_compare_result_on_equal= ((end_key->flag == HA_READ_BEFORE_KEY) ? 1 :
				  (end_key->flag == HA_READ_AFTER_KEY) ? -1 : 0);
  }
  range_key_part= table->key_info[active_index].key_part;

  if (!start_key)			// Read first record
    result= index_first(table->record[0]);
  else
    result= index_read(table->record[0],
		       start_key->key,
		       start_key->length,
		       start_key->flag);
  if (result)
    DBUG_RETURN((result == HA_ERR_KEY_NOT_FOUND) 
		? HA_ERR_END_OF_FILE
		: result);

  DBUG_RETURN (compare_key(end_range) <= 0 ? 0 : HA_ERR_END_OF_FILE);
}


/*
  Read next row between two ranges.

  SYNOPSIS
    read_range_next()

  NOTES
    Record is read into table->record[0]

  RETURN
    0			Found row
    HA_ERR_END_OF_FILE	No rows in range
    #			Error code
*/

int handler::read_range_next()
{
  int result;
  DBUG_ENTER("handler::read_range_next");

  if (eq_range)
  {
    /* We trust that index_next_same always gives a row in range */
    DBUG_RETURN(index_next_same(table->record[0],
                                end_range->key,
                                end_range->length));
  }
  result= index_next(table->record[0]);
  if (result)
    DBUG_RETURN(result);
  DBUG_RETURN(compare_key(end_range) <= 0 ? 0 : HA_ERR_END_OF_FILE);
}


/*
  Compare if found key (in row) is over max-value

  SYNOPSIS
    compare_key
    range		range to compare to row. May be 0 for no range
 
  NOTES
    See key.cc::key_cmp() for details

  RETURN
    The return value is SIGN(key_in_row - range_key):

    0			Key is equal to range or 'range' == 0 (no range)
   -1			Key is less than range
    1			Key is larger than range
*/

int handler::compare_key(key_range *range)
{
  int cmp;
  if (!range)
    return 0;					// No max range
  cmp= key_cmp(range_key_part, range->key, range->length);
  if (!cmp)
    cmp= key_compare_result_on_equal;
  return cmp;
}

int handler::index_read_idx(byte * buf, uint index, const byte * key,
			     uint key_len, enum ha_rkey_function find_flag)
{
  int error= ha_index_init(index);
  if (!error)
    error= index_read(buf, key, key_len, find_flag);
  if (!error)
    error= ha_index_end();
  return error;
}


/*
  Returns a list of all known extensions.

  SYNOPSIS
    ha_known_exts()
 
  NOTES
    No mutexes, worst case race is a minor surplus memory allocation
    We have to recreate the extension map if mysqld is restarted (for example
    within libmysqld)

  RETURN VALUE
    pointer		pointer to TYPELIB structure
*/

TYPELIB *ha_known_exts(void)
{
  if (!known_extensions.type_names || mysys_usage_id != known_extensions_id)
  {
    show_table_type_st *types;
    List<char> found_exts;
    List_iterator_fast<char> it(found_exts);
    const char **ext, *old_ext;

    known_extensions_id= mysys_usage_id;
    found_exts.push_back((char*) ".db");
    for (types= sys_table_types; types->type; types++)
    {      
      if (*types->value == SHOW_OPTION_YES)
      {
	handler *file= get_new_handler(0,(enum db_type) types->db_type);
	for (ext= file->bas_ext(); *ext; ext++)
	{
	  while ((old_ext= it++))
          {
	    if (!strcmp(old_ext, *ext))
	      break;
          }
	  if (!old_ext)
	    found_exts.push_back((char *) *ext);

	  it.rewind();
	}
	delete file;
      }
    }
    ext= (const char **) my_once_alloc(sizeof(char *)*
                                       (found_exts.elements+1),
                                       MYF(MY_WME | MY_FAE));
    
    DBUG_ASSERT(ext);
    known_extensions.count= found_exts.elements;
    known_extensions.type_names= ext;

    while ((old_ext= it++))
      *ext++= old_ext;
    *ext= 0;
  }
  return &known_extensions;
}
