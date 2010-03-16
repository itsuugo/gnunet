/*
     This file is part of GNUnet.
     (C) 2009, 2010 Christian Grothoff (and other contributing authors)

     GNUnet is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published
     by the Free Software Foundation; either version 2, or (at your
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

/**
 * @file dht/gnunet_dht_service.c
 * @brief main DHT service shell, building block for DHT implementations
 * @author Christian Grothoff
 * @author Nathan Evans
 */

#include "platform.h"
#include "gnunet_client_lib.h"
#include "gnunet_getopt_lib.h"
#include "gnunet_os_lib.h"
#include "gnunet_protocols.h"
#include "gnunet_service_lib.h"
#include "gnunet_core_service.h"
#include "gnunet_signal_lib.h"
#include "gnunet_util_lib.h"
#include "gnunet_datastore_service.h"
#include "dht.h"

/**
 * Handle to the datastore service (for inserting/retrieving data)
 */
static struct GNUNET_DATASTORE_Handle *datastore;

/**
 * The main scheduler to use for the DHT service
 */
static struct GNUNET_SCHEDULER_Handle *sched;

/**
 * The configuration the DHT service is running with
 */
static const struct GNUNET_CONFIGURATION_Handle *cfg;

/**
 * Timeout for transmissions to clients
 */
static struct GNUNET_TIME_Relative client_transmit_timeout;

/**
 * Handle to the core service
 */
static struct GNUNET_CORE_Handle *coreAPI;

/**
 * The identity of our peer.
 */
static struct GNUNET_PeerIdentity my_identity;

/**
 * Task to run when we shut down, cleaning up all our trash
 */
static GNUNET_SCHEDULER_TaskIdentifier cleanup_task;

struct ClientList
{
  /**
   * This is a linked list
   */
  struct ClientList *next;

  /**
   * The client in question
   */
  struct GNUNET_SERVER_Client *client;
};

/**
 * Server handler for initiating local dht get requests
 */
static void handle_dht_get (void *cls, struct GNUNET_SERVER_Client * client,
                     const struct GNUNET_MessageHeader *message);

/**
 * Server handler for stopping local dht get requests
 */
static void handle_dht_get_stop (void *cls, struct GNUNET_SERVER_Client * client,
                     const struct GNUNET_MessageHeader *message);

/**
 * Server handler for initiating local dht find peer requests
 */
static void handle_dht_find_peer (void *cls, struct GNUNET_SERVER_Client *
                           client, const struct GNUNET_MessageHeader *
                           message);

/**
 * Server handler for stopping local dht find peer requests
 */
static void handle_dht_find_peer_stop (void *cls, struct GNUNET_SERVER_Client *
                           client, const struct GNUNET_MessageHeader *
                           message);

/**
 * Server handler for initiating local dht put requests
 */
static void handle_dht_put (void *cls, struct GNUNET_SERVER_Client * client,
                     const struct GNUNET_MessageHeader *message);


static struct GNUNET_SERVER_MessageHandler plugin_handlers[] = {
  {&handle_dht_get, NULL, GNUNET_MESSAGE_TYPE_DHT_GET, 0},
  {&handle_dht_get_stop, NULL, GNUNET_MESSAGE_TYPE_DHT_GET_STOP, 0},
  {&handle_dht_put, NULL, GNUNET_MESSAGE_TYPE_DHT_PUT, 0},
  {&handle_dht_find_peer, NULL, GNUNET_MESSAGE_TYPE_DHT_FIND_PEER, 0},
  {&handle_dht_find_peer_stop, NULL, GNUNET_MESSAGE_TYPE_DHT_FIND_PEER_STOP, 0},
  {NULL, NULL, 0, 0}
};


/**
 * Core handler for p2p dht get requests.
 */
static int handle_dht_p2p_get (void *cls,
                             const struct GNUNET_PeerIdentity * peer,
                             const struct GNUNET_MessageHeader * message,
                             struct GNUNET_TIME_Relative latency,
                             uint32_t distance);

/**
 * Core handler for p2p dht put requests.
 */
static int handle_dht_p2p_put (void *cls,
                             const struct GNUNET_PeerIdentity * peer,
                             const struct GNUNET_MessageHeader * message,
                             struct GNUNET_TIME_Relative latency,
                             uint32_t distance);

/**
 * Core handler for p2p dht find peer requests.
 */
static int handle_dht_p2p_find_peer (void *cls,
                             const struct GNUNET_PeerIdentity * peer,
                             const struct GNUNET_MessageHeader * message,
                             struct GNUNET_TIME_Relative latency,
                             uint32_t distance);

static struct GNUNET_CORE_MessageHandler core_handlers[] = {
  {&handle_dht_p2p_get, GNUNET_MESSAGE_TYPE_DHT_GET, 0},
  {&handle_dht_p2p_put, GNUNET_MESSAGE_TYPE_DHT_PUT, 0},
  {&handle_dht_p2p_find_peer, GNUNET_MESSAGE_TYPE_DHT_PUT, 0},
  {NULL, 0, 0}
};


/**
 * Server handler for initiating local dht get requests
 */
static void handle_dht_get (void *cls, struct GNUNET_SERVER_Client * client,
                     const struct GNUNET_MessageHeader *message)
{
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s': Received `%s' request from client\n", "DHT", "GET");
#endif

}

/**
 * Server handler for stopping local dht get requests
 */
static void handle_dht_get_stop (void *cls, struct GNUNET_SERVER_Client * client,
                          const struct GNUNET_MessageHeader *message)
{
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s': Received `%s' request from client\n", "DHT", "GET STOP");
#endif

}

/**
 * Server handler for initiating local dht find peer requests
 */
static void handle_dht_find_peer (void *cls, struct GNUNET_SERVER_Client *
                           client, const struct GNUNET_MessageHeader *
                           message)
{
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s': Received `%s' request from client\n", "DHT", "FIND PEER");
#endif

}

/**
 * Server handler for stopping local dht find peer requests
 */
static void handle_dht_find_peer_stop (void *cls, struct GNUNET_SERVER_Client *
                           client, const struct GNUNET_MessageHeader *
                           message)
{
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s': Received `%s' request from client\n", "DHT", "FIND PEER STOP");
#endif

}

/**
 * Server handler for initiating local dht put requests
 */
static void handle_dht_put (void *cls, struct GNUNET_SERVER_Client * client,
                     const struct GNUNET_MessageHeader *message)
{
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s': Received `%s' request from client\n", "DHT", "PUT");
#endif

}

/**
 * Core handler for p2p dht get requests.
 */
static int handle_dht_p2p_get (void *cls,
                             const struct GNUNET_PeerIdentity * peer,
                             const struct GNUNET_MessageHeader * message,
                             struct GNUNET_TIME_Relative latency,
                             uint32_t distance)
{
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s': Received `%s' request from another peer\n", "DHT", "GET");
#endif

  return GNUNET_YES;
}

/**
 * Core handler for p2p dht put requests.
 */
static int handle_dht_p2p_put (void *cls,
                             const struct GNUNET_PeerIdentity * peer,
                             const struct GNUNET_MessageHeader * message,
                             struct GNUNET_TIME_Relative latency,
                             uint32_t distance)
{
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s': Received `%s' request from another peer\n", "DHT", "PUT");
#endif

  return GNUNET_YES;
}

/**
 * Core handler for p2p dht find peer requests.
 */
static int handle_dht_p2p_find_peer (void *cls,
                             const struct GNUNET_PeerIdentity * peer,
                             const struct GNUNET_MessageHeader * message,
                             struct GNUNET_TIME_Relative latency,
                             uint32_t distance)
{
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s': Received `%s' request from another peer\n", "DHT", "FIND PEER");
#endif

  return GNUNET_YES;
}

/**
 * Task run during shutdown.
 *
 * @param cls unused
 * @param tc unused
 */
static void
shutdown_task (void *cls,
               const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  GNUNET_CORE_disconnect (coreAPI);
}

/**
 * To be called on core init/fail.
 */
void core_init (void *cls,
                struct GNUNET_CORE_Handle * server,
                const struct GNUNET_PeerIdentity *identity,
                const struct GNUNET_CRYPTO_RsaPublicKeyBinaryEncoded * publicKey)
{

  if (server == NULL)
    {
      GNUNET_SCHEDULER_cancel(sched, cleanup_task);
      GNUNET_SCHEDULER_add_now(sched, &shutdown_task, NULL);
      return;
    }
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "%s: Core connection initialized, I am peer: %s\n", "dht", GNUNET_i2s(identity));
#endif
  memcpy(&my_identity, identity, sizeof(struct GNUNET_PeerIdentity));
  coreAPI = server;
}

/**
 * Process dht requests.
 *
 * @param cls closure
 * @param scheduler scheduler to use
 * @param server the initialized server
 * @param c configuration to use
 */
static void
run (void *cls,
     struct GNUNET_SCHEDULER_Handle *scheduler,
     struct GNUNET_SERVER_Handle *server,
     const struct GNUNET_CONFIGURATION_Handle *c)
{
  sched = scheduler;
  cfg = c;

  datastore = GNUNET_DATASTORE_connect(c, scheduler);

  client_transmit_timeout = GNUNET_TIME_relative_multiply(GNUNET_TIME_UNIT_SECONDS, 5);
  GNUNET_SERVER_add_handlers (server, plugin_handlers);

  coreAPI =
  GNUNET_CORE_connect (sched,
                       cfg,
                       client_transmit_timeout,
                       NULL, /* FIXME: anything we want to pass around? */
                       &core_init,
                       NULL, /* Don't care about pre-connects */
                       NULL, /* Don't care about connects */
                       NULL, /* Don't care about disconnects */
                       NULL,
                       GNUNET_NO,
                       NULL,
                       GNUNET_NO,
                       core_handlers);

  if (coreAPI == NULL)
    return;
  /* load (server); Huh? */

  /* Scheduled the task to clean up when shutdown is called */
  cleanup_task = GNUNET_SCHEDULER_add_delayed (sched,
                                GNUNET_TIME_UNIT_FOREVER_REL,
                                &shutdown_task,
                                NULL);
}


/**
 * The main function for the dv service.
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc, char *const *argv)
{
  return (GNUNET_OK ==
          GNUNET_SERVICE_run (argc,
                              argv,
                              "dht",
                              GNUNET_SERVICE_OPTION_NONE,
                              &run, NULL)) ? 0 : 1;
}
