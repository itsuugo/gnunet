/*
     This file is part of GNUnet
     Copyright (C) 2009, 2010, 2011 GNUnet e.V.

     GNUnet is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published
     by the Free Software Foundation; either version 3, or (at your
     option) any later version.

     GNUnet is distributed in the hope that it will be useful, but
     WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with GNUnet; see the file COPYING.  If not, write to the
     Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
     Boston, MA 02110-1301, USA.
*/

/**
 * @file datastore/plugin_datastore_mysql.c
 * @brief mysql-based datastore backend
 * @author Igor Wronsky
 * @author Christian Grothoff
 * @author Christophe Genevey
 *
 * NOTE: This db module does NOT work with mysql prior to 4.1 since
 * it uses prepared statements.  MySQL 5.0.46 promises to fix a bug
 * in MyISAM that is causing us grief.  At the time of this writing,
 * that version is yet to be released.  In anticipation, the code
 * will use MyISAM with 5.0.46 (and higher).  If you run such a
 * version, please run "make check" to verify that the MySQL bug
 * was actually fixed in your version (and if not, change the
 * code below to use MyISAM for gn071).
 *
 * HIGHLIGHTS
 *
 * Pros
 * + On up-to-date hardware where mysql can be used comfortably, this
 *   module will have better performance than the other db choices
 *   (according to our tests).
 * + Its often possible to recover the mysql database from internal
 *   inconsistencies. The other db choices do not support repair!
 * Cons
 * - Memory usage (Comment: "I have 1G and it never caused me trouble")
 * - Manual setup
 *
 * MANUAL SETUP INSTRUCTIONS
 *
 * 1) in gnunet.conf, set
 * @verbatim
       [datastore]
       DATABASE = "mysql"
   @endverbatim
 * 2) Then access mysql as root,
 * @verbatim
     $ mysql -u root -p
   @endverbatim
 *    and do the following. [You should replace $USER with the username
 *    that will be running the gnunetd process].
 * @verbatim
      CREATE DATABASE gnunet;
      GRANT select,insert,update,delete,create,alter,drop,create temporary tables
         ON gnunet.* TO $USER@localhost;
      SET PASSWORD FOR $USER@localhost=PASSWORD('$the_password_you_like');
      FLUSH PRIVILEGES;
   @endverbatim
 * 3) In the $HOME directory of $USER, create a ".my.cnf" file
 *    with the following lines
 * @verbatim
      [client]
      user=$USER
      password=$the_password_you_like
   @endverbatim
 *
 * Thats it. Note that .my.cnf file is a security risk unless its on
 * a safe partition etc. The $HOME/.my.cnf can of course be a symbolic
 * link. Even greater security risk can be achieved by setting no
 * password for $USER.  Luckily $USER has only priviledges to mess
 * up GNUnet's tables, nothing else (unless you give him more,
 * of course).<p>
 *
 * 4) Still, perhaps you should briefly try if the DB connection
 *    works. First, login as $USER. Then use,
 *
 * @verbatim
     $ mysql -u $USER -p $the_password_you_like
     mysql> use gnunet;
   @endverbatim
 *
 *    If you get the message &quot;Database changed&quot; it probably works.
 *
 *    [If you get &quot;ERROR 2002: Can't connect to local MySQL server
 *     through socket '/tmp/mysql.sock' (2)&quot; it may be resolvable by
 *     &quot;ln -s /var/run/mysqld/mysqld.sock /tmp/mysql.sock&quot;
 *     so there may be some additional trouble depending on your mysql setup.]
 *
 * REPAIRING TABLES
 *
 * - Its probably healthy to check your tables for inconsistencies
 *   every now and then.
 * - If you get odd SEGVs on gnunetd startup, it might be that the mysql
 *   databases have been corrupted.
 * - The tables can be verified/fixed in two ways;
 *   1) by running mysqlcheck -A, or
 *   2) by executing (inside of mysql using the GNUnet database):
 * @verbatim
     mysql> REPAIR TABLE gn090;
   @endverbatim
 *
 * PROBLEMS?
 *
 * If you have problems related to the mysql module, your best
 * friend is probably the mysql manual. The first thing to check
 * is that mysql is basically operational, that you can connect
 * to it, create tables, issue queries etc.
 */

#include "platform.h"
#include "gnunet_datastore_plugin.h"
#include "gnunet_util_lib.h"
#include "gnunet_mysql_lib.h"
#include "gnunet_my_lib.h"

#define MAX_DATUM_SIZE 65536


/**
 * Context for all functions in this plugin.
 */
struct Plugin
{
  /**
   * Our execution environment.
   */
  struct GNUNET_DATASTORE_PluginEnvironment *env;

  /**
   * Handle to talk to MySQL.
   */
  struct GNUNET_MYSQL_Context *mc;

  /**
   * Prepared statements.
   */
#define INSERT_ENTRY "INSERT INTO gn090 (repl,type,prio,anonLevel,expire,rvalue,hash,vhash,value) VALUES (?,?,?,?,?,?,?,?,?)"
  struct GNUNET_MYSQL_StatementHandle *insert_entry;

#define DELETE_ENTRY_BY_UID "DELETE FROM gn090 WHERE uid=?"
  struct GNUNET_MYSQL_StatementHandle *delete_entry_by_uid;

#define COUNT_ENTRY_BY_HASH "SELECT count(*) FROM gn090 FORCE INDEX (idx_hash) WHERE hash=?"
  struct GNUNET_MYSQL_StatementHandle *count_entry_by_hash;

#define SELECT_ENTRY_BY_HASH "SELECT type,prio,anonLevel,expire,hash,value,uid FROM gn090 FORCE INDEX (idx_hash) WHERE hash=? ORDER BY uid LIMIT 1 OFFSET ?"
  struct GNUNET_MYSQL_StatementHandle *select_entry_by_hash;

#define COUNT_ENTRY_BY_HASH_AND_VHASH "SELECT count(*) FROM gn090 FORCE INDEX (idx_hash_vhash) WHERE hash=? AND vhash=?"
  struct GNUNET_MYSQL_StatementHandle *count_entry_by_hash_and_vhash;

#define SELECT_ENTRY_BY_HASH_AND_VHASH "SELECT type,prio,anonLevel,expire,hash,value,uid FROM gn090 FORCE INDEX (idx_hash_vhash) WHERE hash=? AND vhash=? ORDER BY uid LIMIT 1 OFFSET ?"
  struct GNUNET_MYSQL_StatementHandle *select_entry_by_hash_and_vhash;

#define COUNT_ENTRY_BY_HASH_AND_TYPE "SELECT count(*) FROM gn090 FORCE INDEX (idx_hash_type_uid) WHERE hash=? AND type=?"
  struct GNUNET_MYSQL_StatementHandle *count_entry_by_hash_and_type;

#define SELECT_ENTRY_BY_HASH_AND_TYPE "SELECT type,prio,anonLevel,expire,hash,value,uid FROM gn090 FORCE INDEX (idx_hash_type_uid) WHERE hash=? AND type=? ORDER BY uid LIMIT 1 OFFSET ?"
  struct GNUNET_MYSQL_StatementHandle *select_entry_by_hash_and_type;

#define COUNT_ENTRY_BY_HASH_VHASH_AND_TYPE "SELECT count(*) FROM gn090 FORCE INDEX (idx_hash_vhash) WHERE hash=? AND vhash=? AND type=?"
  struct GNUNET_MYSQL_StatementHandle *count_entry_by_hash_vhash_and_type;

#define SELECT_ENTRY_BY_HASH_VHASH_AND_TYPE "SELECT type,prio,anonLevel,expire,hash,value,uid FROM gn090 FORCE INDEX (idx_hash_vhash) WHERE hash=? AND vhash=? AND type=? ORDER BY uid ASC LIMIT 1 OFFSET ?"
  struct GNUNET_MYSQL_StatementHandle *select_entry_by_hash_vhash_and_type;

#define UPDATE_ENTRY "UPDATE gn090 SET prio=prio+?,expire=IF(expire>=?,expire,?) WHERE uid=?"
  struct GNUNET_MYSQL_StatementHandle *update_entry;

#define DEC_REPL "UPDATE gn090 SET repl=GREATEST (1, repl) - 1 WHERE uid=?"
  struct GNUNET_MYSQL_StatementHandle *dec_repl;

#define SELECT_SIZE "SELECT SUM(LENGTH(value)+256) FROM gn090"
  struct GNUNET_MYSQL_StatementHandle *get_size;

#define SELECT_IT_NON_ANONYMOUS "SELECT type,prio,anonLevel,expire,hash,value,uid "\
   "FROM gn090 FORCE INDEX (idx_anonLevel_type_rvalue) "\
   "WHERE anonLevel=0 AND type=? AND "\
   "(rvalue >= ? OR"\
   "  NOT EXISTS (SELECT 1 FROM gn090 FORCE INDEX (idx_anonLevel_type_rvalue) WHERE anonLevel=0 AND type=? AND rvalue>=?)) "\
   "ORDER BY rvalue ASC LIMIT 1"
  struct GNUNET_MYSQL_StatementHandle *zero_iter;

#define SELECT_IT_EXPIRATION "SELECT type,prio,anonLevel,expire,hash,value,uid FROM gn090 FORCE INDEX (idx_expire) WHERE expire < ? ORDER BY expire ASC LIMIT 1"
  struct GNUNET_MYSQL_StatementHandle *select_expiration;

#define SELECT_IT_PRIORITY "SELECT type,prio,anonLevel,expire,hash,value,uid FROM gn090 FORCE INDEX (idx_prio) ORDER BY prio ASC LIMIT 1"
  struct GNUNET_MYSQL_StatementHandle *select_priority;

#define SELECT_IT_REPLICATION "SELECT type,prio,anonLevel,expire,hash,value,uid "\
  "FROM gn090 FORCE INDEX (idx_repl_rvalue) "\
  "WHERE repl=? AND "\
  " (rvalue>=? OR"\
  "  NOT EXISTS (SELECT 1 FROM gn090 FORCE INDEX (idx_repl_rvalue) WHERE repl=? AND rvalue>=?)) "\
  "ORDER BY rvalue ASC "\
  "LIMIT 1"
  struct GNUNET_MYSQL_StatementHandle *select_replication;

#define SELECT_MAX_REPL "SELECT MAX(repl) FROM gn090"
  struct GNUNET_MYSQL_StatementHandle *max_repl;

#define GET_ALL_KEYS "SELECT hash from gn090"
  struct GNUNET_MYSQL_StatementHandle *get_all_keys;

};

#define MAX_PARAM 16

/**
 * Delete an entry from the gn090 table.
 *
 * @param plugin plugin context
 * @param uid unique ID of the entry to delete
 * @return #GNUNET_OK on success, #GNUNET_NO if no such value exists, #GNUNET_SYSERR on error
 */
static int
do_delete_entry (struct Plugin *plugin,
                 unsigned long long uid)
{
  int ret;
  uint64_t uid64 = (uint64_t) uid;
  struct GNUNET_MY_QueryParam params_delete[] = {
    GNUNET_MY_query_param_uint64 (&uid64),
    GNUNET_MY_query_param_end
  };

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Deleting value %llu from gn090 table\n",
              uid);
  ret = GNUNET_MY_exec_prepared (plugin->mc,
                                 plugin->delete_entry_by_uid,
                                 params_delete);
  if (ret >= 0)
  {
    return GNUNET_OK;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Deleting value %llu from gn090 table failed\n",
              (unsigned long long) uid);
  return ret;
}


/**
 * Get an estimate of how much space the database is
 * currently using.
 *
 * @param cls our `struct Plugin *`
 * @return number of bytes used on disk
 */
static void
mysql_plugin_estimate_size (void *cls,
                            unsigned long long *estimate)
{
  struct Plugin *plugin = cls;
  uint64_t total;
  int ret;
  struct GNUNET_MY_QueryParam params_get[] = {
    GNUNET_MY_query_param_end
  };
  struct GNUNET_MY_ResultSpec results_get[] = {
    GNUNET_MY_result_spec_uint64 (&total),
    GNUNET_MY_result_spec_end
  };

  ret = GNUNET_MY_exec_prepared (plugin->mc,
                                 plugin->get_size,
                                 params_get);
  *estimate = 0;
  total = UINT64_MAX;
  if ( (GNUNET_OK == ret) &&
       (GNUNET_OK ==
        GNUNET_MY_extract_result (plugin->get_size,
                                  results_get)) )
  {
    *estimate = (unsigned long long) total;
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Size estimate for MySQL payload is %lld\n",
                (long long) total);
    GNUNET_assert (UINT64_MAX != total);
    GNUNET_break (GNUNET_NO ==
                  GNUNET_MY_extract_result (plugin->get_size,
                                            NULL));
  }
}


/**
 * Store an item in the datastore.
 *
 * @param cls closure
 * @param key key for the item
 * @param size number of bytes in @a data
 * @param data content stored
 * @param type type of the content
 * @param priority priority of the content
 * @param anonymity anonymity-level for the content
 * @param replication replication-level for the content
 * @param expiration expiration time for the content
 * @param cont continuation called with success or failure status
 * @param cont_cls closure for @a cont
 */
static void
mysql_plugin_put (void *cls,
                  const struct GNUNET_HashCode *key,
                  uint32_t size,
                  const void *data,
                  enum GNUNET_BLOCK_Type type,
                  uint32_t priority,
                  uint32_t anonymity,
                  uint32_t replication,
                  struct GNUNET_TIME_Absolute expiration,
                  PluginPutCont cont,
                  void *cont_cls)
{
  struct Plugin *plugin = cls;
  uint64_t lexpiration = expiration.abs_value_us;
  uint64_t lrvalue = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK,
                                               UINT64_MAX);
  struct GNUNET_HashCode vhash;
  struct GNUNET_MY_QueryParam params_insert[] = {
    GNUNET_MY_query_param_uint32 (&replication),
    GNUNET_MY_query_param_uint32 (&type),
    GNUNET_MY_query_param_uint32 (&priority),
    GNUNET_MY_query_param_uint32 (&anonymity),
    GNUNET_MY_query_param_uint64 (&lexpiration),
    GNUNET_MY_query_param_uint64 (&lrvalue),
    GNUNET_MY_query_param_auto_from_type (key),
    GNUNET_MY_query_param_auto_from_type (&vhash),
    GNUNET_MY_query_param_fixed_size (data, size),
    GNUNET_MY_query_param_end
  };

  if (size > MAX_DATUM_SIZE)
  {
    GNUNET_break (0);
    cont (cont_cls, key, size, GNUNET_SYSERR, _("Data too large"));
    return;
  }
  GNUNET_CRYPTO_hash (data,
                      size,
                      &vhash);

  if (GNUNET_OK !=
      GNUNET_MY_exec_prepared (plugin->mc,
                               plugin->insert_entry,
                               params_insert))
  {
    cont (cont_cls,
          key,
          size,
          GNUNET_SYSERR,
          _("MySQL statement run failure"));
    return;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Inserted value `%s' with size %u into gn090 table\n",
              GNUNET_h2s (key),
              (unsigned int) size);
  if (size > 0)
    plugin->env->duc (plugin->env->cls,
                      size);
  GNUNET_break (GNUNET_NO ==
                GNUNET_MY_extract_result (plugin->insert_entry,
                                          NULL));
  cont (cont_cls,
        key,
        size,
        GNUNET_OK,
        NULL);
}


/**
 * Update the priority for a particular key in the datastore.  If
 * the expiration time in value is different than the time found in
 * the datastore, the higher value should be kept.  For the
 * anonymity level, the lower value is to be used.  The specified
 * priority should be added to the existing priority, ignoring the
 * priority in value.
 *
 * Note that it is possible for multiple values to match this put.
 * In that case, all of the respective values are updated.
 *
 * @param cls our "struct Plugin*"
 * @param uid unique identifier of the datum
 * @param delta by how much should the priority
 *     change?
 * @param expire new expiration time should be the
 *     MAX of any existing expiration time and
 *     this value
 * @param cont continuation called with success or failure status
 * @param cons_cls continuation closure
 */
static void
mysql_plugin_update (void *cls,
                     uint64_t uid,
                     uint32_t delta,
                     struct GNUNET_TIME_Absolute expire,
                     PluginUpdateCont cont,
                     void *cont_cls)
{
  struct Plugin *plugin = cls;
  uint64_t lexpire = expire.abs_value_us;
  int ret;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Updating value %llu adding %d to priority and maxing exp at %s\n",
              (unsigned long long) uid,
              delta,
	      GNUNET_STRINGS_absolute_time_to_string (expire));

  struct GNUNET_MY_QueryParam params_update[] = {
    GNUNET_MY_query_param_uint32 (&delta),
    GNUNET_MY_query_param_uint64 (&lexpire),
    GNUNET_MY_query_param_uint64 (&lexpire),
    GNUNET_MY_query_param_uint64 (&uid),
    GNUNET_MY_query_param_end
  };

  ret = GNUNET_MY_exec_prepared (plugin->mc,
                                 plugin->update_entry,
                                 params_update);

  if (GNUNET_OK != ret)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Failed to update value %llu\n",
                (unsigned long long) uid);
  }
  else
  {
    GNUNET_break (GNUNET_NO ==
                  GNUNET_MY_extract_result (plugin->update_entry,
                                            NULL));
  }
  cont (cont_cls,
        ret,
        NULL);
}


/**
 * Run the given select statement and call 'proc' on the resulting
 * values (which must be in particular positions).
 *
 * @param plugin the plugin handle
 * @param stmt select statement to run
 * @param proc function to call on result
 * @param proc_cls closure for @a proc
 * @param params_select arguments to initialize stmt
 */
static void
execute_select (struct Plugin *plugin,
                struct GNUNET_MYSQL_StatementHandle *stmt,
                PluginDatumProcessor proc,
                void *proc_cls,
                struct GNUNET_MY_QueryParam *params_select)
{
  int ret;
  uint32_t type;
  uint32_t priority;
  uint32_t anonymity;
  uint64_t uid;
  size_t value_size;
  void *value;
  struct GNUNET_HashCode key;
  struct GNUNET_TIME_Absolute expiration;
  struct GNUNET_MY_ResultSpec results_select[] = {
    GNUNET_MY_result_spec_uint32 (&type),
    GNUNET_MY_result_spec_uint32 (&priority),
    GNUNET_MY_result_spec_uint32 (&anonymity),
    GNUNET_MY_result_spec_absolute_time (&expiration),
    GNUNET_MY_result_spec_auto_from_type (&key),
    GNUNET_MY_result_spec_variable_size (&value, &value_size),
    GNUNET_MY_result_spec_uint64 (&uid),
    GNUNET_MY_result_spec_end
  };

  ret = GNUNET_MY_exec_prepared (plugin->mc,
                                 stmt,
                                 params_select);
  if (GNUNET_OK != ret)
  {
    proc (proc_cls,
          NULL, 0, NULL, 0, 0, 0, GNUNET_TIME_UNIT_ZERO_ABS, 0);
    return;
  }

  ret = GNUNET_MY_extract_result (stmt,
                                  results_select);
  if (GNUNET_OK != ret)
  {
    proc (proc_cls,
          NULL, 0, NULL, 0, 0, 0, GNUNET_TIME_UNIT_ZERO_ABS, 0);
    return;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Found %u-byte value under key `%s' with prio %u, anon %u, expire %s selecting from gn090 table\n",
              (unsigned int) value_size,
              GNUNET_h2s (&key),
	      (unsigned int) priority,
              (unsigned int) anonymity,
	      GNUNET_STRINGS_absolute_time_to_string (expiration));
  GNUNET_assert (value_size < MAX_DATUM_SIZE);
  GNUNET_break (GNUNET_NO ==
                GNUNET_MY_extract_result (stmt,
                                          NULL));
  ret = proc (proc_cls,
              &key,
              value_size,
              value,
              type,
              priority,
              anonymity,
              expiration,
              uid);
  GNUNET_MY_cleanup_result (results_select);
  if (GNUNET_NO == ret)
  {
    do_delete_entry (plugin, uid);
    if (0 != value_size)
      plugin->env->duc (plugin->env->cls,
                        - value_size);
  }
}


/**
 * Get one of the results for a particular key in the datastore.
 *
 * @param cls closure
 * @param offset offset of the result (modulo num-results);
 *               specific ordering does not matter for the offset
 * @param key key to match, never NULL
 * @param vhash hash of the value, maybe NULL (to
 *        match all values that have the right key).
 *        Note that for DBlocks there is no difference
 *        betwen key and vhash, but for other blocks
 *        there may be!
 * @param type entries of which type are relevant?
 *     Use 0 for any type.
 * @param proc function to call on the matching value,
 *        with NULL for if no value matches
 * @param proc_cls closure for @a proc
 */
static void
mysql_plugin_get_key (void *cls,
                      uint64_t offset,
                      const struct GNUNET_HashCode *key,
                      const struct GNUNET_HashCode *vhash,
                      enum GNUNET_BLOCK_Type type,
                      PluginDatumProcessor proc,
                      void *proc_cls)
{
  struct Plugin *plugin = cls;
  int ret;
  uint64_t total;
  struct GNUNET_MY_ResultSpec results_get[] = {
    GNUNET_MY_result_spec_uint64 (&total),
    GNUNET_MY_result_spec_end
  };

  total = UINT64_MAX;
  if (0 != type)
  {
    if (NULL != vhash)
    {
      struct GNUNET_MY_QueryParam params_get[] = {
        GNUNET_MY_query_param_auto_from_type (key),
        GNUNET_MY_query_param_auto_from_type (vhash),
        GNUNET_MY_query_param_uint32 (&type),
        GNUNET_MY_query_param_end
      };

      ret =
        GNUNET_MY_exec_prepared (plugin->mc,
                                 plugin->count_entry_by_hash_vhash_and_type,
                                 params_get);
      GNUNET_break (GNUNET_OK == ret);
      if (GNUNET_OK == ret)
        ret =
          GNUNET_MY_extract_result (plugin->count_entry_by_hash_vhash_and_type,
                                    results_get);
      if (GNUNET_OK == ret)
        GNUNET_break (GNUNET_NO ==
                      GNUNET_MY_extract_result (plugin->count_entry_by_hash_vhash_and_type,
                                                NULL));
    }
    else
    {
      struct GNUNET_MY_QueryParam params_get[] = {
        GNUNET_MY_query_param_auto_from_type (key),
        GNUNET_MY_query_param_uint32 (&type),
        GNUNET_MY_query_param_end
      };

      ret =
        GNUNET_MY_exec_prepared (plugin->mc,
                                 plugin->count_entry_by_hash_and_type,
                                 params_get);
      GNUNET_break (GNUNET_OK == ret);
      if (GNUNET_OK == ret)
        ret =
          GNUNET_MY_extract_result (plugin->count_entry_by_hash_and_type,
                                    results_get);
      if (GNUNET_OK == ret)
        GNUNET_break (GNUNET_NO ==
                      GNUNET_MY_extract_result (plugin->count_entry_by_hash_and_type,
                                                NULL));
    }
  }
  else
  {
    if (NULL != vhash)
    {
      struct GNUNET_MY_QueryParam params_get[] = {
        GNUNET_MY_query_param_auto_from_type (key),
        GNUNET_MY_query_param_auto_from_type (vhash),
        GNUNET_MY_query_param_end
      };

      ret =
        GNUNET_MY_exec_prepared (plugin->mc,
                                 plugin->count_entry_by_hash_and_vhash,
                                 params_get);
      GNUNET_break (GNUNET_OK == ret);
      if (GNUNET_OK == ret)
        ret =
          GNUNET_MY_extract_result (plugin->count_entry_by_hash_and_vhash,
                                    results_get);
      if (GNUNET_OK == ret)
        GNUNET_break (GNUNET_NO ==
                      GNUNET_MY_extract_result (plugin->count_entry_by_hash_and_vhash,
                                                NULL));
    }
    else
    {
      struct GNUNET_MY_QueryParam params_get[] = {
        GNUNET_MY_query_param_auto_from_type (key),
        GNUNET_MY_query_param_end
      };

      ret =
        GNUNET_MY_exec_prepared (plugin->mc,
                                 plugin->count_entry_by_hash,
                                 params_get);
      GNUNET_break (GNUNET_OK == ret);
      if (GNUNET_OK == ret)
        ret =
          GNUNET_MY_extract_result (plugin->count_entry_by_hash,
                                    results_get);
      if (GNUNET_OK == ret)
        GNUNET_break (GNUNET_NO ==
                      GNUNET_MY_extract_result (plugin->count_entry_by_hash,
                                                NULL));
    }
  }
  if ( (GNUNET_OK != ret) ||
       (0 >= total) )
  {
    proc (proc_cls, NULL, 0, NULL, 0, 0, 0, GNUNET_TIME_UNIT_ZERO_ABS, 0);
    return;
  }
  offset = offset % total;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Obtaining %llu/%lld result for GET `%s'\n",
              (unsigned long long) offset,
              (unsigned long long) total,
              GNUNET_h2s (key));
  if (type != GNUNET_BLOCK_TYPE_ANY)
  {
    if (NULL != vhash)
    {
      struct GNUNET_MY_QueryParam params_select[] = {
        GNUNET_MY_query_param_auto_from_type (key),
        GNUNET_MY_query_param_auto_from_type (vhash),
        GNUNET_MY_query_param_uint32 (&type),
        GNUNET_MY_query_param_uint64 (&offset),
        GNUNET_MY_query_param_end
      };

      execute_select (plugin,
                      plugin->select_entry_by_hash_vhash_and_type,
                      proc,
                      proc_cls,
                      params_select);
    }
    else
    {
      struct GNUNET_MY_QueryParam params_select[] = {
        GNUNET_MY_query_param_auto_from_type (key),
        GNUNET_MY_query_param_uint32 (&type),
        GNUNET_MY_query_param_uint64 (&offset),
        GNUNET_MY_query_param_end
      };

      execute_select (plugin,
                      plugin->select_entry_by_hash_and_type,
                      proc,
                      proc_cls,
                      params_select);
    }
  }
  else
  {
    if (NULL != vhash)
    {
      struct GNUNET_MY_QueryParam params_select[] = {
        GNUNET_MY_query_param_auto_from_type (key),
        GNUNET_MY_query_param_auto_from_type (vhash),
        GNUNET_MY_query_param_uint64 (&offset),
        GNUNET_MY_query_param_end
      };

      execute_select (plugin,
                      plugin->select_entry_by_hash_and_vhash,
                      proc,
                      proc_cls,
                      params_select);
    }
    else
    {
      struct GNUNET_MY_QueryParam params_select[] = {
        GNUNET_MY_query_param_auto_from_type (key),
        GNUNET_MY_query_param_uint64 (&offset),
        GNUNET_MY_query_param_end
      };

      execute_select (plugin,
                      plugin->select_entry_by_hash,
                      proc,
                      proc_cls,
                      params_select);
    }
  }

}


/**
 * Get a zero-anonymity datum from the datastore.
 *
 * @param cls our `struct Plugin *`
 * @param offset offset of the result
 * @param type entries of which type should be considered?
 *        Use 0 for any type.
 * @param proc function to call on a matching value or NULL
 * @param proc_cls closure for @a proc
 */
static void
mysql_plugin_get_zero_anonymity (void *cls,
                                 uint64_t offset,
                                 enum GNUNET_BLOCK_Type type,
                                 PluginDatumProcessor proc,
                                 void *proc_cls)
{
  struct Plugin *plugin = cls;
  uint32_t typei = (uint32_t) type;
  uint64_t rvalue = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK,
                                              UINT64_MAX);
  struct GNUNET_MY_QueryParam params_zero_iter[] = {
    GNUNET_MY_query_param_uint32 (&typei),
    GNUNET_MY_query_param_uint64 (&rvalue),
    GNUNET_MY_query_param_uint32 (&typei),
    GNUNET_MY_query_param_uint64 (&rvalue),
    GNUNET_MY_query_param_end
  };

  execute_select (plugin,
                  plugin->zero_iter,
                  proc,
                  proc_cls,
                  params_zero_iter);
}


/**
 * Context for #repl_proc() function.
 */
struct ReplCtx
{

  /**
   * Plugin handle.
   */
  struct Plugin *plugin;

  /**
   * Function to call for the result (or the NULL).
   */
  PluginDatumProcessor proc;

  /**
   * Closure for @e proc.
   */
  void *proc_cls;
};


/**
 * Wrapper for the processor for #mysql_plugin_get_replication().
 * Decrements the replication counter and calls the original
 * iterator.
 *
 * @param cls closure
 * @param key key for the content
 * @param size number of bytes in @a data
 * @param data content stored
 * @param type type of the content
 * @param priority priority of the content
 * @param anonymity anonymity-level for the content
 * @param expiration expiration time for the content
 * @param uid unique identifier for the datum;
 *        maybe 0 if no unique identifier is available
 * @return #GNUNET_SYSERR to abort the iteration, #GNUNET_OK to continue
 *         (continue on call to "next", of course),
 *         #GNUNET_NO to delete the item and continue (if supported)
 */
static int
repl_proc (void *cls,
           const struct GNUNET_HashCode *key,
           uint32_t size,
           const void *data,
           enum GNUNET_BLOCK_Type type,
           uint32_t priority,
           uint32_t anonymity,
           struct GNUNET_TIME_Absolute expiration,
           uint64_t uid)
{
  struct ReplCtx *rc = cls;
  struct Plugin *plugin = rc->plugin;
  int ret;
  int iret;

  ret = rc->proc (rc->proc_cls,
                  key,
                  size,
                  data,
                  type,
                  priority,
                  anonymity,
                  expiration,
                  uid);
  if (NULL != key)
  {
    struct GNUNET_MY_QueryParam params_proc[] = {
      GNUNET_MY_query_param_uint64 (&uid),
      GNUNET_MY_query_param_end
    };

    iret = GNUNET_MY_exec_prepared (plugin->mc,
                                    plugin->dec_repl,
                                    params_proc);
    if (GNUNET_SYSERR == iret)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "Failed to reduce replication counter\n");
      return GNUNET_SYSERR;
    }
  }
  return ret;
}


/**
 * Get a random item for replication.  Returns a single, not expired,
 * random item from those with the highest replication counters.  The
 * item's replication counter is decremented by one IF it was positive
 * before.  Call @a proc with all values ZERO or NULL if the datastore
 * is empty.
 *
 * @param cls closure
 * @param proc function to call the value (once only).
 * @param proc_cls closure for @a proc
 */
static void
mysql_plugin_get_replication (void *cls,
                              PluginDatumProcessor proc,
                              void *proc_cls)
{
  struct Plugin *plugin = cls;
  uint64_t rvalue;
  uint32_t repl;
  struct ReplCtx rc;
  struct GNUNET_MY_QueryParam params_get[] = {
    GNUNET_MY_query_param_end
  };
  struct GNUNET_MY_ResultSpec results_get[] = {
    GNUNET_MY_result_spec_uint32 (&repl),
    GNUNET_MY_result_spec_end
  };
  struct GNUNET_MY_QueryParam params_select[] = {
    GNUNET_MY_query_param_uint32 (&repl),
    GNUNET_MY_query_param_uint64 (&rvalue),
    GNUNET_MY_query_param_uint32 (&repl),
    GNUNET_MY_query_param_uint64 (&rvalue),
    GNUNET_MY_query_param_end
  };

  rc.plugin = plugin;
  rc.proc = proc;
  rc.proc_cls = proc_cls;

  if (1 !=
      GNUNET_MY_exec_prepared (plugin->mc,
                               plugin->max_repl,
                               params_get))
  {
    proc (proc_cls, NULL, 0, NULL, 0, 0, 0, GNUNET_TIME_UNIT_ZERO_ABS, 0);
    return;
  }

  if (GNUNET_OK !=
      GNUNET_MY_extract_result (plugin->max_repl,
                                results_get))
  {
    proc (proc_cls, NULL, 0, NULL, 0, 0, 0, GNUNET_TIME_UNIT_ZERO_ABS, 0);
    return;
  }
  GNUNET_break (GNUNET_NO ==
                GNUNET_MY_extract_result (plugin->max_repl,
                                          NULL));
  rvalue = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK,
                                     UINT64_MAX);

  execute_select (plugin,
                  plugin->select_replication,
                  &repl_proc,
                  &rc,
                  params_select);
}


/**
 * Get all of the keys in the datastore.
 *
 * @param cls closure
 * @param proc function to call on each key
 * @param proc_cls closure for @a proc
 */
static void
mysql_plugin_get_keys (void *cls,
                       PluginKeyProcessor proc,
                       void *proc_cls)
{
  struct Plugin *plugin = cls;
  int ret;
  MYSQL_STMT *statement;
  unsigned int cnt;
  struct GNUNET_HashCode key;
  struct GNUNET_HashCode last;
  struct GNUNET_MY_QueryParam params_select[] = {
    GNUNET_MY_query_param_end
  };
  struct GNUNET_MY_ResultSpec results_select[] = {
    GNUNET_MY_result_spec_auto_from_type (&key),
    GNUNET_MY_result_spec_end
  };

  GNUNET_assert (NULL != proc);
  statement = GNUNET_MYSQL_statement_get_stmt (plugin->get_all_keys);
  if (GNUNET_OK !=
      GNUNET_MY_exec_prepared (plugin->mc,
                               plugin->get_all_keys,
                               params_select))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                _("`%s' for `%s' failed at %s:%d with error: %s\n"),
                "mysql_stmt_execute",
                GET_ALL_KEYS,
                __FILE__,
                __LINE__,
                mysql_stmt_error (statement));
    GNUNET_MYSQL_statements_invalidate (plugin->mc);
    proc (proc_cls, NULL, 0);
    return;
  }
  memset (&last, 0, sizeof (last)); /* make static analysis happy */
  ret = GNUNET_YES;
  cnt = 0;
  while (ret == GNUNET_YES)
  {
    ret = GNUNET_MY_extract_result (plugin->get_all_keys,
                                    results_select);
    if (0 != memcmp (&last,
                     &key,
                     sizeof (key)))
    {
      if (0 != cnt)
        proc (proc_cls,
              &last,
              cnt);
      cnt = 1;
      last = key;
    }
    else
    {
      cnt++;
    }
  }
  if (0 != cnt)
    proc (proc_cls,
          &last,
          cnt);
  /* finally, let app know we are done */
  proc (proc_cls,
        NULL,
        0);
  if (GNUNET_SYSERR == ret)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                _("`%s' failed at %s:%d with error: %s\n"),
                "mysql_stmt_fetch",
                __FILE__,
                __LINE__,
                mysql_stmt_error (statement));
    GNUNET_MYSQL_statements_invalidate (plugin->mc);
    return;
  }
}


/**
 * Context for #expi_proc() function.
 */
struct ExpiCtx
{

  /**
   * Plugin handle.
   */
  struct Plugin *plugin;

  /**
   * Function to call for the result (or the NULL).
   */
  PluginDatumProcessor proc;

  /**
   * Closure for @e proc.
   */
  void *proc_cls;
};



/**
 * Wrapper for the processor for #mysql_plugin_get_expiration().
 * If no expired value was found, we do a second query for
 * low-priority content.
 *
 * @param cls closure
 * @param key key for the content
 * @param size number of bytes in data
 * @param data content stored
 * @param type type of the content
 * @param priority priority of the content
 * @param anonymity anonymity-level for the content
 * @param expiration expiration time for the content
 * @param uid unique identifier for the datum;
 *        maybe 0 if no unique identifier is available
 * @return #GNUNET_SYSERR to abort the iteration, #GNUNET_OK to continue
 *         (continue on call to "next", of course),
 *         #GNUNET_NO to delete the item and continue (if supported)
 */
static int
expi_proc (void *cls,
           const struct GNUNET_HashCode *key,
           uint32_t size,
           const void *data,
           enum GNUNET_BLOCK_Type type,
           uint32_t priority,
           uint32_t anonymity,
           struct GNUNET_TIME_Absolute expiration,
           uint64_t uid)
{
  struct ExpiCtx *rc = cls;
  struct Plugin *plugin = rc->plugin;
  struct GNUNET_MY_QueryParam params_select[] = {
    GNUNET_MY_query_param_end
  };

  if (NULL == key)
  {
    execute_select (plugin,
                    plugin->select_priority,
                    rc->proc,
                    rc->proc_cls,
                    params_select);
    return GNUNET_SYSERR;
  }
  return rc->proc (rc->proc_cls,
                   key,
                   size,
                   data,
                   type,
                   priority,
                   anonymity,
                   expiration,
                   uid);
}


/**
 * Get a random item for expiration.
 * Call @a proc with all values ZERO or NULL if the datastore is empty.
 *
 * @param cls closure
 * @param proc function to call the value (once only).
 * @param proc_cls closure for @a proc
 */
static void
mysql_plugin_get_expiration (void *cls,
                             PluginDatumProcessor proc,
                             void *proc_cls)
{
  struct Plugin *plugin = cls;
  struct GNUNET_TIME_Absolute now;
  struct GNUNET_MY_QueryParam params_select[] = {
    GNUNET_MY_query_param_absolute_time (&now),
    GNUNET_MY_query_param_end
  };
  struct ExpiCtx rc;

  rc.plugin = plugin;
  rc.proc = proc;
  rc.proc_cls = proc_cls;
  now = GNUNET_TIME_absolute_get ();
  execute_select (plugin,
                  plugin->select_expiration,
                  expi_proc,
                  &rc,
                  params_select);
}


/**
 * Drop database.
 *
 * @param cls the `struct Plugin *`
 */
static void
mysql_plugin_drop (void *cls)
{
  struct Plugin *plugin = cls;

  if (GNUNET_OK !=
      GNUNET_MYSQL_statement_run (plugin->mc,
                                  "DROP TABLE gn090"))
    return;                     /* error */
  plugin->env->duc (plugin->env->cls, 0);
}


/**
 * Entry point for the plugin.
 *
 * @param cls the `struct GNUNET_DATASTORE_PluginEnvironment *`
 * @return our `struct Plugin *`
 */
void *
libgnunet_plugin_datastore_mysql_init (void *cls)
{
  struct GNUNET_DATASTORE_PluginEnvironment *env = cls;
  struct GNUNET_DATASTORE_PluginFunctions *api;
  struct Plugin *plugin;

  plugin = GNUNET_new (struct Plugin);
  plugin->env = env;
  plugin->mc = GNUNET_MYSQL_context_create (env->cfg,
                                            "datastore-mysql");
  if (NULL == plugin->mc)
  {
    GNUNET_free (plugin);
    return NULL;
  }
#define MRUNS(a) (GNUNET_OK != GNUNET_MYSQL_statement_run (plugin->mc, a) )
#define PINIT(a,b) (NULL == (a = GNUNET_MYSQL_statement_prepare (plugin->mc, b)))
  if (MRUNS
      ("CREATE TABLE IF NOT EXISTS gn090 ("
       " repl INT(11) UNSIGNED NOT NULL DEFAULT 0,"
       " type INT(11) UNSIGNED NOT NULL DEFAULT 0,"
       " prio INT(11) UNSIGNED NOT NULL DEFAULT 0,"
       " anonLevel INT(11) UNSIGNED NOT NULL DEFAULT 0,"
       " expire BIGINT UNSIGNED NOT NULL DEFAULT 0,"
       " rvalue BIGINT UNSIGNED NOT NULL,"
       " hash BINARY(64) NOT NULL DEFAULT '',"
       " vhash BINARY(64) NOT NULL DEFAULT '',"
       " value BLOB NOT NULL DEFAULT ''," " uid BIGINT NOT NULL AUTO_INCREMENT,"
       " PRIMARY KEY (uid)," " INDEX idx_hash (hash(64)),"
       " INDEX idx_hash_uid (hash(64),uid),"
       " INDEX idx_hash_vhash (hash(64),vhash(64)),"
       " INDEX idx_hash_type_uid (hash(64),type,rvalue),"
       " INDEX idx_prio (prio)," " INDEX idx_repl_rvalue (repl,rvalue),"
       " INDEX idx_expire (expire),"
       " INDEX idx_anonLevel_type_rvalue (anonLevel,type,rvalue)"
       ") ENGINE=InnoDB") || MRUNS ("SET AUTOCOMMIT = 1") ||
      PINIT (plugin->insert_entry, INSERT_ENTRY) ||
      PINIT (plugin->delete_entry_by_uid, DELETE_ENTRY_BY_UID) ||
      PINIT (plugin->select_entry_by_hash, SELECT_ENTRY_BY_HASH) ||
      PINIT (plugin->select_entry_by_hash_and_vhash,
             SELECT_ENTRY_BY_HASH_AND_VHASH) ||
      PINIT (plugin->select_entry_by_hash_and_type,
             SELECT_ENTRY_BY_HASH_AND_TYPE) ||
      PINIT (plugin->select_entry_by_hash_vhash_and_type,
             SELECT_ENTRY_BY_HASH_VHASH_AND_TYPE) ||
      PINIT (plugin->count_entry_by_hash, COUNT_ENTRY_BY_HASH) ||
      PINIT (plugin->get_size, SELECT_SIZE) ||
      PINIT (plugin->count_entry_by_hash_and_vhash,
             COUNT_ENTRY_BY_HASH_AND_VHASH) ||
      PINIT (plugin->count_entry_by_hash_and_type, COUNT_ENTRY_BY_HASH_AND_TYPE)
      || PINIT (plugin->count_entry_by_hash_vhash_and_type,
                COUNT_ENTRY_BY_HASH_VHASH_AND_TYPE) ||
      PINIT (plugin->update_entry, UPDATE_ENTRY) ||
      PINIT (plugin->dec_repl, DEC_REPL) ||
      PINIT (plugin->zero_iter, SELECT_IT_NON_ANONYMOUS) ||
      PINIT (plugin->select_expiration, SELECT_IT_EXPIRATION) ||
      PINIT (plugin->select_priority, SELECT_IT_PRIORITY) ||
      PINIT (plugin->max_repl, SELECT_MAX_REPL) ||
      PINIT (plugin->get_all_keys, GET_ALL_KEYS) ||
      PINIT (plugin->select_replication, SELECT_IT_REPLICATION))
  {
    GNUNET_MYSQL_context_destroy (plugin->mc);
    GNUNET_free (plugin);
    return NULL;
  }
#undef PINIT
#undef MRUNS

  api = GNUNET_new (struct GNUNET_DATASTORE_PluginFunctions);
  api->cls = plugin;
  api->estimate_size = &mysql_plugin_estimate_size;
  api->put = &mysql_plugin_put;
  api->update = &mysql_plugin_update;
  api->get_key = &mysql_plugin_get_key;
  api->get_replication = &mysql_plugin_get_replication;
  api->get_expiration = &mysql_plugin_get_expiration;
  api->get_zero_anonymity = &mysql_plugin_get_zero_anonymity;
  api->get_keys = &mysql_plugin_get_keys;
  api->drop = &mysql_plugin_drop;
  GNUNET_log_from (GNUNET_ERROR_TYPE_INFO, "mysql",
                   _("Mysql database running\n"));
  return api;
}


/**
 * Exit point from the plugin.
 *
 * @param cls our `struct Plugin *`
 * @return always NULL
 */
void *
libgnunet_plugin_datastore_mysql_done (void *cls)
{
  struct GNUNET_DATASTORE_PluginFunctions *api = cls;
  struct Plugin *plugin = api->cls;

  GNUNET_MYSQL_context_destroy (plugin->mc);
  GNUNET_free (plugin);
  GNUNET_free (api);
  return NULL;
}

/* end of plugin_datastore_mysql.c */
