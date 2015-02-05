/*
     This file is part of GNUnet.
     (C) 2009, 2012 Christian Grothoff (and other contributing authors)

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
 * @file rps/test_rps_multipeer.c
 * @brief Testcase for the random peer sampling service.  Starts
 *        a peergroup with a given number of peers, then waits to
 *        receive size pushes/pulls from each peer.  Expects to wait
 *        for one message from each peer.
 */
#include "platform.h"
#include "gnunet_testbed_service.h"
#include "gnunet_rps_service.h"
#include <time.h>


/**
 * How many peers do we start?
 */
#define NUM_PEERS 5

/**
 * How long do we run the test?
 */
#define TIMEOUT GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 60)


/**
 * Portion of malicious peers
 */
static double portion = .1;


/**
 * Information we track for each peer.
 */
struct RPSPeer
{
  /**
   * Handle for RPS connect operation.
   */
  struct GNUNET_TESTBED_Operation *op;

  /**
   * Handle to RPS service.
   */
  struct GNUNET_RPS_Handle *rps_handle;
};


/**
 * Information for all the peers.
 */
static struct RPSPeer rps_peers[NUM_PEERS];

/**
 * IDs of the peers.
 */
static struct GNUNET_PeerIdentity rps_peer_ids[NUM_PEERS];

/**
 * Return value from 'main'.
 */
static int ok;


/**
 * Task run on timeout to shut everything down.
 */
static void
shutdown_task (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  unsigned int i;

  for (i=0;i<NUM_PEERS;i++)
    GNUNET_TESTBED_operation_done (rps_peers[i].op);
  GNUNET_SCHEDULER_shutdown ();
}


/**
 * Callback to call when network size estimate is updated.
 *
 * @param cls closure
 * @param timestamp server timestamp
 * @param estimate the value of the current network size estimate
 * @param std_dev standard deviation (rounded down to nearest integer)
 *                of the size estimation values seen
 *
 */
static void
handle_reply (void *cls, uint64_t n, const struct GNUNET_PeerIdentity *peers)
{
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Got peer %s\n", GNUNET_i2s (peers));
  
  ok = 0;
}


/**
 * (Randomly) request random peers.
 */
  void
request_peers (void *cls,
               const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct RPSPeer *peer = (struct RPSPeer *) cls;
  struct GNUNET_RPS_Request_Handle *req_handle;

  req_handle = GNUNET_RPS_request_peers (peer->rps_handle, 1, handle_reply, NULL);
  GNUNET_free (req_handle);
}


/**
 * Seed peers.
 */
  void
seed_peers (void *cls,
               const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  unsigned int amount;
  struct RPSPeer *peer = (struct RPSPeer *) cls;

  GNUNET_assert (1 >= portion &&
                 0 <  portion);
                
  amount = portion * NUM_PEERS;

  // TODO log

  GNUNET_RPS_seed_ids (peer->rps_handle, amount, rps_peer_ids);
}


/**
 * Get the id of peer i.
 */
  void
info_cb (void *cb_cls,
         struct GNUNET_TESTBED_Operation *op,
         const struct GNUNET_TESTBED_PeerInformation *pinfo,
         const char *emsg)
{
  unsigned int *i = (unsigned int *) cb_cls;

  if (NULL == pinfo || NULL != emsg)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "Got Error: %s\n", emsg);
    return;
  }

  rps_peer_ids[*i] = *(pinfo->result.id);

  GNUNET_free (cb_cls);
}


/**
 * Callback to be called when RPS service connect operation is completed
 *
 * @param cls the callback closure from functions generating an operation
 * @param op the operation that has been finished
 * @param ca_result the RPS service handle returned from rps_connect_adapter
 * @param emsg error message in case the operation has failed; will be NULL if
 *          operation has executed successfully.
 */
static void
rps_connect_complete_cb (void *cls,
			 struct GNUNET_TESTBED_Operation *op,
			 void *ca_result,
			 const char *emsg)
{
  struct RPSPeer *peer = cls;
  struct GNUNET_RPS_Handle *rps = ca_result;
  struct GNUNET_RPS_Request_Handle *req_handle;

  peer->rps_handle = rps;

  GNUNET_assert (op == peer->op);
  if (NULL != emsg)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
		  "Failed to connect to RPS service: %s\n",
		  emsg);
      ok = 1;
      GNUNET_SCHEDULER_shutdown ();
      return;
    }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Started client successfully\n");

  req_handle = GNUNET_RPS_request_peers (rps, 1, handle_reply, NULL);
  GNUNET_free (req_handle);

  GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 10),
                                request_peers, peer);
  GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 10),
                                seed_peers, peer);
  // TODO test seeding > GNUNET_SERVER_MAX_MESSAGE_SIZE peers
}


/**
 * Adapter function called to establish a connection to
 * the RPS service.
 *
 * @param cls closure
 * @param cfg configuration of the peer to connect to; will be available until
 *          GNUNET_TESTBED_operation_done() is called on the operation returned
 *          from GNUNET_TESTBED_service_connect()
 * @return service handle to return in 'op_result', NULL on error
 */
static void *
rps_connect_adapter (void *cls,
		     const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  return GNUNET_RPS_connect (cfg);
}


/**
 * Adapter function called to destroy connection to
 * RPS service.
 *
 * @param cls closure
 * @param op_result service handle returned from the connect adapter
 */
static void
rps_disconnect_adapter (void *cls,
			void *op_result)
{
  struct GNUNET_RPS_Handle *h = op_result;
  GNUNET_RPS_disconnect (h);
}


/**
 * Actual "main" function for the testcase.
 *
 * @param cls closure
 * @param h the run handle
 * @param num_peers number of peers in 'peers'
 * @param peers handle to peers run in the testbed
 * @param links_succeeded the number of overlay link connection attempts that
 *          succeeded
 * @param links_failed the number of overlay link connection attempts that
 *          failed
 */
static void
run (void *cls,
     struct GNUNET_TESTBED_RunHandle *h,
     unsigned int num_peers,
     struct GNUNET_TESTBED_Peer **peers,
     unsigned int links_succeeded,
     unsigned int links_failed)
{
  unsigned int i;
  unsigned int *tmp_i;

  for ( i = 0 ; i < NUM_PEERS ; i++ )
  {
    tmp_i = GNUNET_new (unsigned int);
    *tmp_i = i;

    (void) GNUNET_TESTBED_peer_get_information (peers[i],
                                                GNUNET_TESTBED_PIT_IDENTITY,
                                                &info_cb, tmp_i);
  }

  GNUNET_assert (NUM_PEERS == num_peers);
  for (i=0;i<num_peers;i++)
    //rps_peers[i].peer_index = i;
    rps_peers[i].op = GNUNET_TESTBED_service_connect (&rps_peers[i],
						      peers[i],
						      "rps",
						      &rps_connect_complete_cb,
						      &rps_peers[i],
						      &rps_connect_adapter,
						      &rps_disconnect_adapter,
						      &rps_peers[i]);
  GNUNET_SCHEDULER_add_delayed (TIMEOUT, &shutdown_task, NULL);
}


/**
 * Entry point for the testcase, sets up the testbed.
 *
 * @param argc unused
 * @param argv unused
 * @return 0 on success
 */
int
main (int argc, char *argv[])
{
  ok = 1;
  (void) GNUNET_TESTBED_test_run ("test-rps-multipeer",
                                  "test_rps.conf",
                                  NUM_PEERS,
                                  0, NULL, NULL,
                                  &run, NULL);
  return ok;
}

/* end of test_rps_multipeer.c */
