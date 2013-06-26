/*
     This file is part of GNUnet.
     (C) 2011 Christian Grothoff (and other contributing authors)

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

/**
 * @file mesh/test_mesh2_local.c
 * @brief test mesh2 local: test of mesh2 tunnels with just one peer
 * @author Bartlomiej Polot
 */

#include "platform.h"
#include "gnunet_util_lib.h"
#include "gnunet_dht_service.h"
#include "gnunet_testing_lib.h"
#include "gnunet_mesh2_service.h"

struct GNUNET_TESTING_Peer *me;

static struct GNUNET_MESH_Handle *mesh_peer_1;

static struct GNUNET_MESH_Handle *mesh_peer_2;

static struct GNUNET_MESH_Tunnel *t;

static int result = GNUNET_OK;

static GNUNET_SCHEDULER_TaskIdentifier abort_task;

static GNUNET_SCHEDULER_TaskIdentifier shutdown_task;


/**
 * Shutdown nicely
 */
static void
do_shutdown (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "shutdown\n");
  if (GNUNET_SCHEDULER_NO_TASK != abort_task)
  {
    GNUNET_SCHEDULER_cancel (abort_task);
  }
  if (NULL != t)
  {
    GNUNET_MESH_tunnel_destroy (t);
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Disconnect client 1\n");
  if (NULL != mesh_peer_1)
  {
    GNUNET_MESH_disconnect (mesh_peer_1);
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Disconnect client 2\n");
  if (NULL != mesh_peer_2)
  {
    GNUNET_MESH_disconnect (mesh_peer_2);
  }
}


/**
 * Something went wrong and timed out. Kill everything and set error flag
 */
static void
do_abort (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "ABORT\n");
  result = GNUNET_SYSERR;
  abort_task = GNUNET_SCHEDULER_NO_TASK;
  if (GNUNET_SCHEDULER_NO_TASK != shutdown_task)
  {
    GNUNET_SCHEDULER_cancel (shutdown_task);
    shutdown_task = GNUNET_SCHEDULER_NO_TASK;
  }
  do_shutdown (cls, tc);
}


/**
 * Function is called whenever a message is received.
 *
 * @param cls closure (set from GNUNET_MESH_connect)
 * @param tunnel connection to the other end
 * @param tunnel_ctx place to store local state associated with the tunnel
 * @param message the actual message
 * 
 * @return GNUNET_OK to keep the connection open,
 *         GNUNET_SYSERR to close it (signal serious error)
 */
static int
data_callback (void *cls, struct GNUNET_MESH_Tunnel *tunnel, void **tunnel_ctx,
               const struct GNUNET_MessageHeader *message)
{
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Data callback! Shutting down.\n");
  if (GNUNET_SCHEDULER_NO_TASK != shutdown_task)
    GNUNET_SCHEDULER_cancel (shutdown_task);
  shutdown_task =
    GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_UNIT_SECONDS, &do_shutdown,
                                  NULL);
  GNUNET_MESH_receive_done (tunnel);
  return GNUNET_OK;
}


/**
 * Method called whenever another peer has added us to a tunnel
 * the other peer initiated.
 *
 * @param cls closure
 * @param tunnel new handle to the tunnel
 * @param initiator peer that started the tunnel
 * @param port port number
 * @return initial tunnel context for the tunnel (can be NULL -- that's not an error)
 */
static void *
inbound_tunnel (void *cls, struct GNUNET_MESH_Tunnel *tunnel,
                const struct GNUNET_PeerIdentity *initiator,
                uint32_t port)
{
  long id = (long) cls;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "received incoming tunnel on peer %d, port %u\n",
              id, port);
  if (id != 2L)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "wrong peer\n");
    result = GNUNET_SYSERR;
  }
  return NULL;
}


/**
 * Function called whenever an inbound tunnel is destroyed.  Should clean up
 * any associated state.
 *
 * @param cls closure (set from GNUNET_MESH_connect)
 * @param tunnel connection to the other end (henceforth invalid)
 * @param tunnel_ctx place where local state associated
 *                   with the tunnel is stored
 */
static void
inbound_end (void *cls, const struct GNUNET_MESH_Tunnel *tunnel,
             void *tunnel_ctx)
{
  long id = (long) cls;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "incoming tunnel closed\n");
  if (id != 2)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "received closing tunnel on peer != 2\n");
    result = GNUNET_SYSERR;
  }
}


/**
 * Handler array for traffic received on peer1
 */
static struct GNUNET_MESH_MessageHandler handlers1[] = {
  {&data_callback, 1, 0},
  {NULL, 0, 0}
};


/**
 * Handler array for traffic received on peer2 (none expected)
 */
static struct GNUNET_MESH_MessageHandler handlers2[] = {
  {&data_callback, 1, 0},
  {NULL, 0, 0}
};


/**
 * Data send callback: fillbuffer with test packet.
 * 
 * @param cls Closure (unused).
 * @param size Buffer size.
 * @param buf Buffer to fill.
 * 
 * @return size of test packet.
 */
static size_t
do_send (void *cls, size_t size, void *buf)
{
  struct GNUNET_MessageHeader *m = buf;

  if (NULL == buf)
  {
    GNUNET_break (0);
    result = GNUNET_SYSERR;
    return 0;
  }
  m->size = htons (sizeof (struct GNUNET_MessageHeader));
  m->type = htons (1);
  GNUNET_assert (size >= sizeof (struct GNUNET_MessageHeader));
  return sizeof (struct GNUNET_MessageHeader);
}

/**
 * Connect to other client and send data
 * 
 * @param cls Closue (unused).
 * @param tc TaskContext.
 */
static void
do_connect (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct GNUNET_PeerIdentity id;

  GNUNET_TESTING_peer_get_identity (me, &id);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "CONNECT BY PORT\n");
  t = GNUNET_MESH_tunnel_create (mesh_peer_1, NULL, &id, 1);
  GNUNET_MESH_notify_transmit_ready (t, GNUNET_NO,
                                     GNUNET_TIME_UNIT_FOREVER_REL,
                                     sizeof (struct GNUNET_MessageHeader),
                                     &do_send, NULL);
}


/**
 * Initialize framework and start test
 * 
 * @param cls Closure (unused).
 * @param cfg Configuration handle.
 * @param peer Testing peer handle.
 */
static void
run (void *cls, 
     const struct GNUNET_CONFIGURATION_Handle *cfg,
     struct GNUNET_TESTING_Peer *peer)
{
  static uint32_t ports[] = {1, 0};

  me = peer;
  abort_task =
      GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_relative_multiply
                                    (GNUNET_TIME_UNIT_SECONDS, 15), &do_abort,
                                    NULL);
  mesh_peer_1 = GNUNET_MESH_connect (cfg,       /* configuration */
                                     (void *) 1L,     /* cls */
                                     &inbound_tunnel,   /* inbound new hndlr */
                                     &inbound_end,      /* inbound end hndlr */
                                     handlers1, /* traffic handlers */
                                     NULL);     /* ports offered */

  mesh_peer_2 = GNUNET_MESH_connect (cfg,       /* configuration */
                                     (void *) 2L,     /* cls */
                                     &inbound_tunnel,   /* inbound new hndlr */
                                     &inbound_end,      /* inbound end hndlr */
                                     handlers2, /* traffic handlers */
                                     ports);     /* ports offered */
  if (NULL == mesh_peer_1 || NULL == mesh_peer_2)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "Couldn't connect to mesh :(\n");
    result = GNUNET_SYSERR;
    return;
  }
  else
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "YAY! CONNECTED TO MESH :D\n");
  }
  GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_UNIT_SECONDS, &do_connect, NULL);
}


/**
 * Main
 */
int
main (int argc, char *argv[])
{
  if (0 != GNUNET_TESTING_peer_run ("test-mesh-local",
                                    "test_mesh2.conf",
                                &run, NULL))
    return 1;
  return (result == GNUNET_OK) ? 0 : 1;
}

/* end of test_mesh_local_1.c */
