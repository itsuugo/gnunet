/*
     This file is part of GNUnet.
     (C) 2012 Christian Grothoff (and other contributing authors)

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
     Free Software Foundation, Inc., 59 Temple Place - Suite 330,
     Boston, MA 02111-1307, USA.
*/
/*
 * @file psycstore/test_plugin_psycstore.c
 * @brief Test for the psycstore plugins
 * @author Gabor X Toth
 * @author Christian Grothoff
 */
#include "platform.h"
#include "gnunet_util_lib.h"
#include "gnunet_testing_lib.h"
#include "gnunet_psycstore_plugin.h"
#include "gnunet_psycstore_service.h"
#include "gnunet_multicast_service.h"

#define DEBUG_PSYCSTORE GNUNET_EXTRA_LOGGING
#if DEBUG_PSYCSTORE
# define LOG_LEVEL "DEBUG"
#else
# define LOG_LEVEL "WARNING"
#endif

#define C2ARG(str) str, (sizeof (str) - 1)

#define LOG(kind,...) GNUNET_log_from (kind, "test-plugin-psycstore", __VA_ARGS__)

#define ASSERT(x) do { if (! (x)) { printf("Error at %s:%d\n", __FILE__, __LINE__); goto FAILURE;} } while (0)

static int ok;

/**
 * Name of plugin under test.
 */
static const char *plugin_name;

static struct GNUNET_CRYPTO_EccPrivateKey *channel_key;
static struct GNUNET_CRYPTO_EccPrivateKey *slave_key;

static struct GNUNET_CRYPTO_EccPublicSignKey channel_pub_key;
static struct GNUNET_CRYPTO_EccPublicSignKey slave_pub_key;

/**
 * Function called when the service shuts down.  Unloads our psycstore
 * plugin.
 *
 * @param api api to unload
 */
static void
unload_plugin (struct GNUNET_PSYCSTORE_PluginFunctions *api)
{
  char *libname;

  GNUNET_asprintf (&libname, "libgnunet_plugin_psycstore_%s", plugin_name);
  GNUNET_break (NULL == GNUNET_PLUGIN_unload (libname, api));
  GNUNET_free (libname);
}


/**
 * Load the psycstore plugin.
 *
 * @param cfg configuration to pass
 * @return NULL on error
 */
static struct GNUNET_PSYCSTORE_PluginFunctions *
load_plugin (const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  struct GNUNET_PSYCSTORE_PluginFunctions *ret;
  char *libname;

  GNUNET_log (GNUNET_ERROR_TYPE_INFO, _("Loading `%s' psycstore plugin\n"),
              plugin_name);
  GNUNET_asprintf (&libname, "libgnunet_plugin_psycstore_%s", plugin_name);
  if (NULL == (ret = GNUNET_PLUGIN_load (libname, (void*) cfg)))
  {
    FPRINTF (stderr, "Failed to load plugin `%s'!\n", plugin_name);
    return NULL;
  }
  GNUNET_free (libname);
  return ret;
}


struct FragmentClosure
{
  uint8_t n;
  uint64_t flags[16];
  struct GNUNET_MULTICAST_MessageHeader *msg[16];
};

static int
fragment_cb (void *cls, struct GNUNET_MULTICAST_MessageHeader *msg2,
             enum GNUNET_PSYCSTORE_MessageFlags flags)
{
  struct FragmentClosure *fcls = cls;
  struct GNUNET_MULTICAST_MessageHeader *msg1 = fcls->msg[fcls->n];
  uint64_t flags1 = fcls->flags[fcls->n++];
  int ret;

  if (flags1 == flags && msg1->header.size == msg2->header.size
      && 0 == memcmp (msg1, msg2, ntohs (msg1->header.size)))
  {
    LOG (GNUNET_ERROR_TYPE_DEBUG, "Fragment %llu matches\n",
         msg1->fragment_id);
    ret = GNUNET_YES;
  }
  else
  {
    LOG (GNUNET_ERROR_TYPE_ERROR, "Fragment %llu differs\n",
         msg1->fragment_id);
    ret = GNUNET_SYSERR;
  }

  GNUNET_free (msg2);
  return ret;
}


struct StateClosure {
  size_t n;
  void *value[16];
  size_t value_size[16];
};

static int
state_cb (void *cls, const char *name, const void *value, size_t value_size)
{
  struct StateClosure *scls = cls;
  const void *val = scls->value[scls->n];
  size_t val_size = scls->value_size[scls->n++];

  return value_size == val_size && 0 == memcmp (value, val, val_size)
    ? GNUNET_YES
    : GNUNET_SYSERR;
}


static void
run (void *cls, char *const *args, const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  struct GNUNET_PSYCSTORE_PluginFunctions *db;
  
  ok = 1;
  db = load_plugin (cfg);
  if (NULL == db)
  {
    FPRINTF (stderr,
             "%s", 
	     "Failed to initialize PSYCstore.  "
             "Database likely not setup, skipping test.\n");
    return;
  }

  /* Membership */

  channel_key = GNUNET_CRYPTO_ecc_key_create ();
  slave_key = GNUNET_CRYPTO_ecc_key_create ();

  GNUNET_CRYPTO_ecc_key_get_public_for_signature (channel_key, &channel_pub_key);
  GNUNET_CRYPTO_ecc_key_get_public_for_signature (slave_key, &slave_pub_key);

  ASSERT (GNUNET_OK == db->membership_store(db->cls, &channel_pub_key,
                                            &slave_pub_key, GNUNET_YES,
                                            4, 2, 1));

  ASSERT (GNUNET_YES == db->membership_test(db->cls, &channel_pub_key,
                                            &slave_pub_key, 4));

  ASSERT (GNUNET_YES == db->membership_test(db->cls, &channel_pub_key,
                                            &slave_pub_key, 2));

  ASSERT (GNUNET_NO == db->membership_test(db->cls, &channel_pub_key,
                                           &slave_pub_key, 1));


  /* Messages */

  struct GNUNET_MULTICAST_MessageHeader *msg
    = GNUNET_malloc (sizeof (*msg) + sizeof (channel_pub_key));
  ASSERT (msg != NULL);

  msg->header.type = htons (GNUNET_MESSAGE_TYPE_MULTICAST_MESSAGE);
  msg->header.size = htons (sizeof (*msg) + sizeof (channel_pub_key));

  msg->hop_counter = 9;
  msg->fragment_id = INT64_MAX - 1;
  msg->fragment_offset = 0;
  msg->message_id = INT64_MAX - 2;
  msg->group_generation = INT64_MAX - 3;
  msg->flags = GNUNET_MULTICAST_MESSAGE_LAST_FRAGMENT;

  memcpy (&msg[1], &channel_pub_key, sizeof (channel_pub_key));

  msg->purpose.size = htonl (ntohs (msg->header.size)
                             - sizeof (msg->header)
                             - sizeof (msg->hop_counter)
                             - sizeof (msg->signature));
  msg->purpose.purpose = htonl (234);
  GNUNET_CRYPTO_ecc_sign (slave_key, &msg->purpose, &msg->signature);

  struct FragmentClosure fcls = { 0 };
  fcls.n = 0;
  fcls.msg[0] = msg;
  fcls.flags[0] = GNUNET_PSYCSTORE_MESSAGE_STATE;

  ASSERT (GNUNET_OK == db->fragment_store (db->cls, &channel_pub_key, msg,
                                           fcls.flags[0]));

  ASSERT (GNUNET_OK == db->fragment_get (db->cls, &channel_pub_key,
                                         msg->fragment_id,
                                         fragment_cb, &fcls));
  ASSERT (fcls.n == 1);

  fcls.n = 0;

  ASSERT (GNUNET_OK == db->message_get_fragment (db->cls, &channel_pub_key,
                                                 msg->message_id,
                                                 msg->fragment_offset,
                                                 fragment_cb, &fcls));
  ASSERT (fcls.n == 1);

  ASSERT (GNUNET_OK == db->message_add_flags (
            db->cls, &channel_pub_key, msg->message_id,
            GNUNET_PSYCSTORE_MESSAGE_STATE_APPLIED));

  fcls.n = 0;
  fcls.flags[0] |= GNUNET_PSYCSTORE_MESSAGE_STATE_APPLIED;

  ASSERT (GNUNET_OK == db->fragment_get (db->cls, &channel_pub_key,
                                         msg->fragment_id,
                                         fragment_cb, &fcls));
  ASSERT (fcls.n == 1);

  struct GNUNET_MULTICAST_MessageHeader *msg1
    = GNUNET_malloc (sizeof (*msg1) + sizeof (channel_pub_key));

  memcpy (msg1, msg, sizeof (*msg1) + sizeof (channel_pub_key));

  msg1->fragment_id++;
  msg1->fragment_offset += 32768;

  fcls.n = 0;
  fcls.msg[1] = msg1;
  fcls.flags[1] = GNUNET_PSYCSTORE_MESSAGE_STATE_HASH;

  ASSERT (GNUNET_OK == db->fragment_store (db->cls, &channel_pub_key, msg1,
                                           fcls.flags[1]));

  ASSERT (GNUNET_OK == db->message_get (db->cls, &channel_pub_key,
                                        msg->message_id,
                                        fragment_cb, &fcls));
  ASSERT (fcls.n == 2);

  uint64_t max_state_msg_id = 0;
  ASSERT (GNUNET_OK == db->counters_get_slave (db->cls, &channel_pub_key,
                                               &max_state_msg_id)
          && max_state_msg_id == msg->message_id);

  uint64_t fragment_id = 0, message_id = 0, group_generation = 0;
  ASSERT (GNUNET_OK == db->counters_get_master (db->cls, &channel_pub_key,
                                                &fragment_id, &message_id,
                                                &group_generation)
          && fragment_id == msg1->fragment_id
          && message_id == msg1->message_id
          && group_generation == msg1->group_generation);


  /* State */

  ASSERT (GNUNET_OK == db->state_set (db->cls, &channel_pub_key, "_foo",
                                      C2ARG("one two three")));

  ASSERT (GNUNET_OK == db->state_set (db->cls, &channel_pub_key, "_foo_bar",
                                      slave_key,
                                      sizeof (*slave_key)));

  struct StateClosure scls = { 0 };
  scls.n = 0;
  scls.value[0] = "one two three";
  scls.value_size[0] = strlen ("one two three");

  ASSERT (GNUNET_OK == db->state_get (db->cls, &channel_pub_key, "_foo",
                                      state_cb, &scls));
  ASSERT (scls.n == 1);

  scls.n = 0;
  scls.value[1] = slave_key;
  scls.value_size[1] = sizeof (*slave_key);

  ASSERT (GNUNET_OK == db->state_get_all (db->cls, &channel_pub_key, "_foo",
                                          state_cb, &scls));
  ASSERT (scls.n == 2);

  scls.n = 0;
  ASSERT (GNUNET_NO == db->state_get_signed (db->cls, &channel_pub_key,
                                             state_cb, &scls));
  ASSERT (scls.n == 0);

  ASSERT (GNUNET_OK == db->state_update_signed (db->cls, &channel_pub_key));

  scls.n = 0;
  ASSERT (GNUNET_YES == db->state_get_signed (db->cls, &channel_pub_key,
                                              state_cb, &scls));
  ASSERT (scls.n == 2);

  ok = 0;
  
FAILURE:

  if (NULL != channel_key)
  {
    GNUNET_free (channel_key);
    channel_key = NULL;
  }
  if (NULL != slave_key)
  {
    GNUNET_free (slave_key);
    slave_key = NULL;
  }

  unload_plugin (db);
}


int
main (int argc, char *argv[])
{
  char cfg_name[128];
  char *const xargv[] = {
    "test-plugin-psycstore",
    "-c", cfg_name,
    "-L", LOG_LEVEL,
    NULL
  };
  struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_OPTION_END
  };

  GNUNET_DISK_directory_remove ("/tmp/gnunet-test-plugin-psycstore-sqlite");
  GNUNET_log_setup ("test-plugin-psycstore", LOG_LEVEL, NULL);
  plugin_name = GNUNET_TESTING_get_testname_from_underscore (argv[0]);
  GNUNET_snprintf (cfg_name, sizeof (cfg_name), "test_plugin_psycstore_%s.conf",
                   plugin_name);
  GNUNET_PROGRAM_run ((sizeof (xargv) / sizeof (char *)) - 1, xargv,
                      "test-plugin-psycstore", "nohelp", options, &run, NULL);

  if (ok != 0)
    FPRINTF (stderr, "Missed some testcases: %d\n", ok);

#if ! DEBUG_PSYCSTORE
  GNUNET_DISK_directory_remove ("/tmp/gnunet-test-plugin-psycstore-sqlite");
#endif

  return ok;
}

/* end of test_plugin_psycstore.c */
