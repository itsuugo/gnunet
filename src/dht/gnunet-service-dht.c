/*
     This file is part of GNUnet.
     (C) 2009, 2010 Christian Grothoff (and other contributing authors)

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
 * @file dht/gnunet-service-dht.c
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
#include "gnunet_datacache_lib.h"
#include "gnunet_transport_service.h"
#include "gnunet_hello_lib.h"
#include "gnunet_dht_service.h"
#include "gnunet_statistics_service.h"
#include "dhtlog.h"
#include "dht.h"

#define PRINT_TABLES GNUNET_NO

#define REAL_DISTANCE GNUNET_YES

#define EXTRA_CHECKS GNUNET_NO
/**
 * How many buckets will we allow total.
 */
#define MAX_BUCKETS sizeof (GNUNET_HashCode) * 8

/**
 * Should the DHT issue FIND_PEER requests to get better routing tables?
 */
#define DO_FIND_PEER GNUNET_YES

/**
 * What is the maximum number of peers in a given bucket.
 */
#define DEFAULT_BUCKET_SIZE 4

/**
 * Minimum number of peers we need for "good" routing,
 * any less than this and we will allow messages to
 * travel much further through the network!
 */
#define MINIMUM_PEER_THRESHOLD 20

#define DHT_MAX_RECENT 1000

#define FIND_PEER_CALC_INTERVAL GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 60)

/**
 * Default time to wait to send messages on behalf of other peers.
 */
#define DHT_DEFAULT_P2P_TIMEOUT GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 10)

/**
 * Default importance for handling messages on behalf of other peers.
 */
#define DHT_DEFAULT_P2P_IMPORTANCE 0

/**
 * How long to keep recent requests around by default.
 */
#define DEFAULT_RECENT_REMOVAL GNUNET_TIME_relative_multiply(GNUNET_TIME_UNIT_SECONDS, 30)

/**
 * Default time to wait to send find peer messages sent by the dht service.
 */
#define DHT_DEFAULT_FIND_PEER_TIMEOUT GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 30)

/**
 * Default importance for find peer messages sent by the dht service.
 */
#define DHT_DEFAULT_FIND_PEER_IMPORTANCE 8

/**
 * Default replication parameter for find peer messages sent by the dht service.
 */
#define DHT_DEFAULT_FIND_PEER_REPLICATION 4

/**
 * Default options for find peer requests sent by the dht service.
 */
#define DHT_DEFAULT_FIND_PEER_OPTIONS GNUNET_DHT_RO_DEMULTIPLEX_EVERYWHERE
/*#define DHT_DEFAULT_FIND_PEER_OPTIONS GNUNET_DHT_RO_NONE*/

/**
 * How long at least to wait before sending another find peer request.
 */
#define DHT_MINIMUM_FIND_PEER_INTERVAL GNUNET_TIME_relative_multiply(GNUNET_TIME_UNIT_MINUTES, 2)

/**
 * How long at most to wait before sending another find peer request.
 */
#define DHT_MAXIMUM_FIND_PEER_INTERVAL GNUNET_TIME_relative_multiply(GNUNET_TIME_UNIT_MINUTES, 8)

/**
 * How often to update our preference levels for peers in our routing tables.
 */
#define DHT_DEFAULT_PREFERENCE_INTERVAL GNUNET_TIME_relative_multiply(GNUNET_TIME_UNIT_MINUTES, 2)

/**
 * How long at most on average will we allow a reply forward to take
 * (before we quit sending out new requests)
 */
#define MAX_REQUEST_TIME GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 1)

/**
 * How many initial requests to send out (in true Kademlia fashion)
 */
#define DHT_KADEMLIA_REPLICATION 3

/*
 * Default frequency for sending malicious get messages
 */
#define DEFAULT_MALICIOUS_GET_FREQUENCY 1000 /* Number of milliseconds */

/*
 * Default frequency for sending malicious put messages
 */
#define DEFAULT_MALICIOUS_PUT_FREQUENCY 1000 /* Default is in milliseconds */

/**
 * Type for a malicious request, so we can ignore it during testing
 */
#define DHT_MALICIOUS_MESSAGE_TYPE 42

#define DHT_DEFAULT_PING_DELAY GNUNET_TIME_relative_multiply(GNUNET_TIME_UNIT_MINUTES, 1)

/**
 * Real maximum number of hops, at which point we refuse
 * to forward the message.
 */
#define MAX_HOPS 10

/**
 * How many time differences between requesting a core send and
 * the actual callback to remember.
 */
#define MAX_REPLY_TIMES 8

enum ConvergenceOptions
{
   /**
    * Use the linear method for convergence.
    */
   DHT_CONVERGE_LINEAR,

   /**
    * Converge using a fast converging square
    * function.
    */
   DHT_CONVERGE_SQUARE,

   /**
    * Converge using a slower exponential
    * function.
    */
   DHT_CONVERGE_EXPONENTIAL,

   /**
    * Don't do any special convergence, allow
    * the algorithm to hopefully route to closer
    * peers more often.
    */
   DHT_CONVERGE_RANDOM
};

/**
 * Linked list of messages to send to clients.
 */
struct P2PPendingMessage
{
  /**
   * Pointer to next item in the list
   */
  struct P2PPendingMessage *next;

  /**
   * Pointer to previous item in the list
   */
  struct P2PPendingMessage *prev;

  /**
   * Message importance level.
   */
  unsigned int importance;

  /**
   * Time when this request was scheduled to be sent.
   */
  struct GNUNET_TIME_Absolute scheduled;

  /**
   * How long to wait before sending message.
   */
  struct GNUNET_TIME_Relative timeout;

  /**
   * Actual message to be sent; // avoid allocation
   */
  const struct GNUNET_MessageHeader *msg; // msg = (cast) &pm[1]; // memcpy (&pm[1], data, len);

};

/**
 * Per-peer information.
 */
struct PeerInfo
{
  /**
   * Next peer entry (DLL)
   */
  struct PeerInfo *next;

  /**
   *  Prev peer entry (DLL)
   */
  struct PeerInfo *prev;

  /**
   * Head of pending messages to be sent to this peer.
   */
  struct P2PPendingMessage *head;

  /**
   * Tail of pending messages to be sent to this peer.
   */
  struct P2PPendingMessage *tail;

  /**
   * Core handle for sending messages to this peer.
   */
  struct GNUNET_CORE_TransmitHandle *th;

  /**
   * Task for scheduling message sends.
   */
  GNUNET_SCHEDULER_TaskIdentifier send_task;

  /**
   * Task for scheduling preference updates
   */
  GNUNET_SCHEDULER_TaskIdentifier preference_task;

  /**
   * Preference update context
   */
  struct GNUNET_CORE_InformationRequestContext *info_ctx;

  /**
   * What is the average latency for replies received?
   */
  struct GNUNET_TIME_Relative latency;

  /**
   * What is the identity of the peer?
   */
  struct GNUNET_PeerIdentity id;

  /**
   * Transport level distance to peer.
   */
  unsigned int distance;

  /**
   * Task for scheduling periodic ping messages for this peer.
   */
  GNUNET_SCHEDULER_TaskIdentifier ping_task;
};

/**
 * Peers are grouped into buckets.
 */
struct PeerBucket
{
  /**
   * Head of DLL
   */
  struct PeerInfo *head;

  /**
   * Tail of DLL
   */
  struct PeerInfo *tail;

  /**
   * Number of peers in the bucket.
   */
  unsigned int peers_size;
};

/**
 * Linked list of messages to send to clients.
 */
struct PendingMessage
{
  /**
   * Pointer to next item in the list
   */
  struct PendingMessage *next;

  /**
   * Pointer to previous item in the list
   */
  struct PendingMessage *prev;

  /**
   * Actual message to be sent; // avoid allocation
   */
  const struct GNUNET_MessageHeader *msg; // msg = (cast) &pm[1]; // memcpy (&pm[1], data, len);

};

/**
 * Struct containing information about a client,
 * handle to connect to it, and any pending messages
 * that need to be sent to it.
 */
struct ClientList
{
  /**
   * Linked list of active clients
   */
  struct ClientList *next;

  /**
   * The handle to this client
   */
  struct GNUNET_SERVER_Client *client_handle;

  /**
   * Handle to the current transmission request, NULL
   * if none pending.
   */
  struct GNUNET_CONNECTION_TransmitHandle *transmit_handle;

  /**
   * Linked list of pending messages for this client
   */
  struct PendingMessage *pending_head;

  /**
   * Tail of linked list of pending messages for this client
   */
  struct PendingMessage *pending_tail;
};


/**
 * Context containing information about a DHT message received.
 */
struct DHT_MessageContext
{
  /**
   * The client this request was received from.
   * (NULL if received from another peer)
   */
  struct ClientList *client;

  /**
   * The peer this request was received from.
   * (NULL if received from local client)
   */
  const struct GNUNET_PeerIdentity *peer;

  /**
   * The key this request was about
   */
  GNUNET_HashCode key;

  /**
   * The unique identifier of this request
   */
  uint64_t unique_id;

  /**
   * Desired replication level
   */
  uint32_t replication;

  /**
   * Network size estimate, either ours or the sum of
   * those routed to thus far. =~ Log of number of peers
   * chosen from for this request.
   */
  uint32_t network_size;

  /**
   * Any message options for this request
   */
  uint32_t msg_options;

  /**
   * How many hops has the message already traversed?
   */
  uint32_t hop_count;

  /**
   * How important is this message?
   */
  unsigned int importance;

  /**
   * How long should we wait to transmit this request?
   */
  struct GNUNET_TIME_Relative timeout;

  /**
   * Bloomfilter for this routing request.
   */
  struct GNUNET_CONTAINER_BloomFilter *bloom;

  /**
   * Did we forward this message? (may need to remember it!)
   */
  int forwarded;

  /**
   * Are we the closest known peer to this key (out of our neighbors?)
   */
  int closest;
};

/**
 * Record used for remembering what peers are waiting for what
 * responses (based on search key).
 */
struct DHTRouteSource
{
  /**
   * This is a DLL.
   */
  struct DHTRouteSource *next;

  /**
   * This is a DLL.
   */
  struct DHTRouteSource *prev;

  /**
   * Source of the request.  Replies should be forwarded to
   * this peer.
   */
  struct GNUNET_PeerIdentity source;

  /**
   * If this was a local request, remember the client; otherwise NULL.
   */
  struct ClientList *client;

  /**
   * Pointer to this nodes heap location (for removal)
   */
  struct GNUNET_CONTAINER_HeapNode *hnode;

  /**
   * Back pointer to the record storing this information.
   */
  struct DHTQueryRecord *record;

  /**
   * Task to remove this entry on timeout.
   */
  GNUNET_SCHEDULER_TaskIdentifier delete_task;

  /**
   * Bloomfilter of peers we have already sent back as
   * replies to the initial request.  Allows us to not
   * forward the same peer multiple times for a find peer
   * request.
   */
  struct GNUNET_CONTAINER_BloomFilter *find_peers_responded;

};

/**
 * Entry in the DHT routing table.
 */
struct DHTQueryRecord
{
  /**
   * Head of DLL for result forwarding.
   */
  struct DHTRouteSource *head;

  /**
   * Tail of DLL for result forwarding.
   */
  struct DHTRouteSource *tail;

  /**
   * Key that the record concerns.
   */
  GNUNET_HashCode key;

  /**
   * GET message of this record (what we already forwarded?).
   */
  //DV_DHT_MESSAGE get; Try to get away with not saving this.

  /**
   * Bloomfilter of the peers we've replied to so far
   */
  //struct GNUNET_BloomFilter *bloom_results; Don't think we need this, just remove from DLL on response.

};

/**
 * Context used to calculate the number of find peer messages
 * per X time units since our last scheduled find peer message
 * was sent.  If we have seen too many messages, delay or don't
 * send our own out.
 */
struct FindPeerMessageContext
{
  unsigned int count;

  struct GNUNET_TIME_Absolute start;

  struct GNUNET_TIME_Absolute end;
};

/**
 * DHT Routing results structure
 */
struct DHTResults
{
  /*
   * Min heap for removal upon reaching limit
   */
  struct GNUNET_CONTAINER_Heap *minHeap;

  /*
   * Hashmap for fast key based lookup
   */
  struct GNUNET_CONTAINER_MultiHashMap *hashmap;

};

/**
 * DHT structure for recent requests.
 */
struct RecentRequests
{
  /*
   * Min heap for removal upon reaching limit
   */
  struct GNUNET_CONTAINER_Heap *minHeap;

  /*
   * Hashmap for key based lookup
   */
  struct GNUNET_CONTAINER_MultiHashMap *hashmap;
};

struct RecentRequest
{
  /**
   * Position of this node in the min heap.
   */
  struct GNUNET_CONTAINER_HeapNode *heap_node;

  /**
   * Bloomfilter containing entries for peers
   * we forwarded this request to.
   */
  struct GNUNET_CONTAINER_BloomFilter *bloom;

  /**
   * Timestamp of this request, for ordering
   * the min heap.
   */
  struct GNUNET_TIME_Absolute timestamp;

  /**
   * Key of this request.
   */
  GNUNET_HashCode key;

  /**
   * Unique identifier for this request.
   */
  uint64_t uid;

  /**
   * Task to remove this entry on timeout.
   */
  GNUNET_SCHEDULER_TaskIdentifier remove_task;
};

/**
 * Which kind of convergence will we be using?
 */
enum ConvergenceOptions converge_option;

/**
 * Recent requests by hash/uid and by time inserted.
 */
static struct RecentRequests recent;

/**
 * Context to use to calculate find peer rates.
 */
static struct FindPeerMessageContext find_peer_context;

/**
 * Don't use our routing algorithm, always route
 * to closest peer; initially send requests to 3
 * peers.
 */
static int strict_kademlia;

/**
 * Routing option to end routing when closest peer found.
 */
static int stop_on_closest;

/**
 * Routing option to end routing when data is found.
 */
static int stop_on_found;

/**
 * Whether DHT needs to manage find peer requests, or
 * an external force will do it on behalf of the DHT.
 */
static int do_find_peer;

/**
 * How many peers have we added since we sent out our last
 * find peer request?
 */
static unsigned int newly_found_peers;

/**
 * Container of active queries we should remember
 */
static struct DHTResults forward_list;

/**
 * Handle to the datacache service (for inserting/retrieving data)
 */
static struct GNUNET_DATACACHE_Handle *datacache;

/**
 * Handle for the statistics service.
 */
struct GNUNET_STATISTICS_Handle *stats;

/**
 * The main scheduler to use for the DHT service
 */
static struct GNUNET_SCHEDULER_Handle *sched;

/**
 * The configuration the DHT service is running with
 */
static const struct GNUNET_CONFIGURATION_Handle *cfg;

/**
 * Handle to the core service
 */
static struct GNUNET_CORE_Handle *coreAPI;

/**
 * Handle to the transport service, for getting our hello
 */
static struct GNUNET_TRANSPORT_Handle *transport_handle;

/**
 * The identity of our peer.
 */
static struct GNUNET_PeerIdentity my_identity;

/**
 * Short id of the peer, for printing
 */
static char *my_short_id;

/**
 * Our HELLO
 */
static struct GNUNET_MessageHeader *my_hello;

/**
 * Task to run when we shut down, cleaning up all our trash
 */
static GNUNET_SCHEDULER_TaskIdentifier cleanup_task;

/**
 * The lowest currently used bucket.
 */
static unsigned int lowest_bucket; /* Initially equal to MAX_BUCKETS - 1 */

/**
 * The buckets (Kademlia routing table, complete with growth).
 * Array of size MAX_BUCKET_SIZE.
 */
static struct PeerBucket k_buckets[MAX_BUCKETS]; /* From 0 to MAX_BUCKETS - 1 */

/**
 * Hash map of all known peers, for easy removal from k_buckets on disconnect.
 */
static struct GNUNET_CONTAINER_MultiHashMap *all_known_peers;

/**
 * Recently seen find peer requests.
 */
static struct GNUNET_CONTAINER_MultiHashMap *recent_find_peer_requests;

/**
 * Maximum size for each bucket.
 */
static unsigned int bucket_size = DEFAULT_BUCKET_SIZE; /* Initially equal to DEFAULT_BUCKET_SIZE */

/**
 * List of active clients.
 */
static struct ClientList *client_list;

/**
 * Handle to the DHT logger.
 */
static struct GNUNET_DHTLOG_Handle *dhtlog_handle;

/*
 * Whether or not to send routing debugging information
 * to the dht logging server
 */
static unsigned int debug_routes;

/*
 * Whether or not to send FULL route information to
 * logging server
 */
static unsigned int debug_routes_extended;

/*
 * GNUNET_YES or GNUNET_NO, whether or not to act as
 * a malicious node which drops all messages
 */
static unsigned int malicious_dropper;

/*
 * GNUNET_YES or GNUNET_NO, whether or not to act as
 * a malicious node which sends out lots of GETS
 */
static unsigned int malicious_getter;

/**
 * GNUNET_YES or GNUNET_NO, whether or not to act as
 * a malicious node which sends out lots of PUTS
 */
static unsigned int malicious_putter;

/**
 * Frequency for malicious get requests.
 */
static unsigned long long malicious_get_frequency;

/**
 * Frequency for malicious put requests.
 */
static unsigned long long malicious_put_frequency;

/**
 * Reply times for requests, if we are busy, don't send any
 * more requests!
 */
static struct GNUNET_TIME_Relative reply_times[MAX_REPLY_TIMES];

/**
 * Current counter for replies.
 */
static unsigned int reply_counter;

/**
 * Forward declaration.
 */
static size_t send_generic_reply (void *cls, size_t size, void *buf);

/** Declare here so retry_core_send is aware of it */
size_t core_transmit_notify (void *cls,
                             size_t size, void *buf);

/**
 * Convert unique ID to hash code.
 *
 * @param uid unique ID to convert
 * @param hash set to uid (extended with zeros)
 */
static void
hash_from_uid (uint64_t uid,
               GNUNET_HashCode *hash)
{
  memset (hash, 0, sizeof(GNUNET_HashCode));
  *((uint64_t*)hash) = uid;
}

#if AVG
/**
 * Calculate the average send time between messages so that we can
 * ignore certain requests if we get too busy.
 *
 * @return the average time between asking core to send a message
 *         and when the buffer for copying it is passed
 */
static struct GNUNET_TIME_Relative get_average_send_delay()
{
  unsigned int i;
  unsigned int divisor;
  struct GNUNET_TIME_Relative average_time;
  average_time = GNUNET_TIME_relative_get_zero();
  divisor = 0;
  for (i = 0; i < MAX_REPLY_TIMES; i++)
  {
    average_time = GNUNET_TIME_relative_add(average_time, reply_times[i]);
    if (reply_times[i].value == (uint64_t)0)
      continue;
    else
      divisor++;
  }
  if (divisor == 0)
  {
    return average_time;
  }

  average_time = GNUNET_TIME_relative_divide(average_time, divisor);
  fprintf(stderr, "Avg send delay: %u sends is %llu\n", divisor, (long long unsigned int)average_time.value);
  return average_time;
}
#endif

/**
 * Given the largest send delay, artificially decrease it
 * so the next time around we may have a chance at sending
 * again.
 */
static void decrease_max_send_delay(struct GNUNET_TIME_Relative max_time)
{
  unsigned int i;
  for (i = 0; i < MAX_REPLY_TIMES; i++)
    {
      if (reply_times[i].value == max_time.value)
        {
          reply_times[i].value = reply_times[i].value / 2;
          return;
        }
    }
}

/**
 * Find the maximum send time of the recently sent values.
 *
 * @return the average time between asking core to send a message
 *         and when the buffer for copying it is passed
 */
static struct GNUNET_TIME_Relative get_max_send_delay()
{
  unsigned int i;
  struct GNUNET_TIME_Relative max_time;
  max_time = GNUNET_TIME_relative_get_zero();

  for (i = 0; i < MAX_REPLY_TIMES; i++)
  {
    if (reply_times[i].value > max_time.value)
      max_time.value = reply_times[i].value;
  }

  if (max_time.value > MAX_REQUEST_TIME.value)
    GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "Max send delay was %llu\n", (long long unsigned int)max_time.value);
  return max_time;
}

static void
increment_stats(const char *value)
{
  if (stats != NULL)
    {
      GNUNET_STATISTICS_update (stats, value, 1, GNUNET_NO);
    }
}

/**
 *  Try to send another message from our core send list
 */
static void
try_core_send (void *cls,
               const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct PeerInfo *peer = cls;
  struct P2PPendingMessage *pending;
  size_t ssize;

  peer->send_task = GNUNET_SCHEDULER_NO_TASK;

  if (tc->reason == GNUNET_SCHEDULER_REASON_SHUTDOWN)
    return;

  if (peer->th != NULL)
    return; /* Message send already in progress */

  pending = peer->head;
  if (pending != NULL)
    {
      ssize = ntohs(pending->msg->size);
#if DEBUG_DHT > 1
     GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "`%s:%s': Calling notify_transmit_ready with size %d for peer %s\n", my_short_id,
                "DHT", ssize, GNUNET_i2s(&peer->id));
#endif
      pending->scheduled = GNUNET_TIME_absolute_get();
      reply_counter++;
      if (reply_counter >= MAX_REPLY_TIMES)
	reply_counter = 0;
      peer->th = GNUNET_CORE_notify_transmit_ready(coreAPI, pending->importance,
                                                   pending->timeout, &peer->id,
                                                   ssize, &core_transmit_notify, peer);
    }
}

/**
 * Function called to send a request out to another peer.
 * Called both for locally initiated requests and those
 * received from other peers.
 *
 * @param cls DHT service closure argument
 * @param msg the encapsulated message
 * @param peer the peer to forward the message to
 * @param msg_ctx the context of the message (hop count, bloom, etc.)
 */
static void forward_result_message (void *cls,
                                    const struct GNUNET_MessageHeader *msg,
                                    struct PeerInfo *peer,
                                    struct DHT_MessageContext *msg_ctx)
{
  struct GNUNET_DHT_P2PRouteResultMessage *result_message;
  struct P2PPendingMessage *pending;
  size_t msize;
  size_t psize;

  increment_stats(STAT_RESULT_FORWARDS);
  msize = sizeof (struct GNUNET_DHT_P2PRouteResultMessage) + ntohs(msg->size);
  GNUNET_assert(msize <= GNUNET_SERVER_MAX_MESSAGE_SIZE);
  psize = sizeof(struct P2PPendingMessage) + msize;
  pending = GNUNET_malloc(psize);
  pending->msg = (struct GNUNET_MessageHeader *)&pending[1];
  pending->importance = DHT_SEND_PRIORITY;
  pending->timeout = GNUNET_TIME_relative_get_forever();
  result_message = (struct GNUNET_DHT_P2PRouteResultMessage *)pending->msg;
  result_message->header.size = htons(msize);
  result_message->header.type = htons(GNUNET_MESSAGE_TYPE_DHT_P2P_ROUTE_RESULT);
  result_message->options = htonl(msg_ctx->msg_options);
  result_message->hop_count = htonl(msg_ctx->hop_count + 1);
  GNUNET_assert(GNUNET_OK == GNUNET_CONTAINER_bloomfilter_get_raw_data(msg_ctx->bloom, result_message->bloomfilter, DHT_BLOOM_SIZE));
  result_message->unique_id = GNUNET_htonll(msg_ctx->unique_id);
  memcpy(&result_message->key, &msg_ctx->key, sizeof(GNUNET_HashCode));
  memcpy(&result_message[1], msg, ntohs(msg->size));
#if DEBUG_DHT > 1
  GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "%s:%s Adding pending message size %d for peer %s\n", my_short_id, "DHT", msize, GNUNET_i2s(&peer->id));
#endif
  GNUNET_CONTAINER_DLL_insert_after(peer->head, peer->tail, peer->tail, pending);
  if (peer->send_task == GNUNET_SCHEDULER_NO_TASK)
    peer->send_task = GNUNET_SCHEDULER_add_now(sched, &try_core_send, peer);
}
/**
 * Called when core is ready to send a message we asked for
 * out to the destination.
 *
 * @param cls closure (NULL)
 * @param size number of bytes available in buf
 * @param buf where the callee should write the message
 * @return number of bytes written to buf
 */
size_t core_transmit_notify (void *cls,
                             size_t size, void *buf)
{
  struct PeerInfo *peer = cls;
  char *cbuf = buf;
  struct P2PPendingMessage *pending;

  size_t off;
  size_t msize;

  if (buf == NULL)
    {
      /* client disconnected */
#if DEBUG_DHT
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "`%s:%s': buffer was NULL\n", my_short_id, "DHT");
#endif
      return 0;
    }

  if (peer->head == NULL)
    return 0;

  peer->th = NULL;
  off = 0;
  pending = peer->head;
  reply_times[reply_counter] = GNUNET_TIME_absolute_get_difference(pending->scheduled, GNUNET_TIME_absolute_get());
  msize = ntohs(pending->msg->size);
  if (msize <= size)
    {
      off = msize;
      memcpy (cbuf, pending->msg, msize);
      GNUNET_CONTAINER_DLL_remove (peer->head,
                                   peer->tail,
                                   pending);
#if DEBUG_DHT > 1
      GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "%s:%s Removing pending message size %d for peer %s\n", my_short_id, "DHT", msize, GNUNET_i2s(&peer->id));
#endif
      GNUNET_free (pending);
    }
#if SMART
  while (NULL != pending &&
          (size - off >= (msize = ntohs (pending->msg->size))))
    {
#if DEBUG_DHT_ROUTING
      GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "`%s:%s' : transmit_notify (core) called with size %d, available %d\n", my_short_id, "dht service", msize, size);
#endif
      memcpy (&cbuf[off], pending->msg, msize);
      off += msize;
      GNUNET_CONTAINER_DLL_remove (peer->head,
                                   peer->tail,
                                   pending);
      GNUNET_free (pending);
      pending = peer->head;
    }
#endif
  if ((peer->head != NULL) && (peer->send_task == GNUNET_SCHEDULER_NO_TASK))
    peer->send_task = GNUNET_SCHEDULER_add_now(sched, &try_core_send, peer);
#if DEBUG_DHT > 1
  GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "`%s:%s' : transmit_notify (core) called with size %d, available %d, returning %d\n", my_short_id, "dht service", msize, size, off);
#endif
  return off;
}

/**
 * Determine how many low order bits match in two
 * GNUNET_HashCodes.  i.e. - 010011 and 011111 share
 * the first two lowest order bits, and therefore the
 * return value is two (NOT XOR distance, nor how many
 * bits match absolutely!).
 *
 * @param first the first hashcode
 * @param second the hashcode to compare first to
 *
 * @return the number of bits that match
 */
static unsigned int matching_bits(const GNUNET_HashCode *first, const GNUNET_HashCode *second)
{
  unsigned int i;

  for (i = 0; i < sizeof (GNUNET_HashCode) * 8; i++)
    if (GNUNET_CRYPTO_hash_get_bit (first, i) != GNUNET_CRYPTO_hash_get_bit (second, i))
      return i;
  return sizeof (GNUNET_HashCode) * 8;
}

/**
 * Compute the distance between have and target as a 32-bit value.
 * Differences in the lower bits must count stronger than differences
 * in the higher bits.
 *
 * @return 0 if have==target, otherwise a number
 *           that is larger as the distance between
 *           the two hash codes increases
 */
static unsigned int
distance (const GNUNET_HashCode * target, const GNUNET_HashCode * have)
{
  unsigned int bucket;
  unsigned int msb;
  unsigned int lsb;
  unsigned int i;

  /* We have to represent the distance between two 2^9 (=512)-bit
     numbers as a 2^5 (=32)-bit number with "0" being used for the
     two numbers being identical; furthermore, we need to
     guarantee that a difference in the number of matching
     bits is always represented in the result.

     We use 2^32/2^9 numerical values to distinguish between
     hash codes that have the same LSB bit distance and
     use the highest 2^9 bits of the result to signify the
     number of (mis)matching LSB bits; if we have 0 matching
     and hence 512 mismatching LSB bits we return -1 (since
     512 itself cannot be represented with 9 bits) */

  /* first, calculate the most significant 9 bits of our
     result, aka the number of LSBs */
  bucket = matching_bits (target, have);
  /* bucket is now a value between 0 and 512 */
  if (bucket == 512)
    return 0;                   /* perfect match */
  if (bucket == 0)
    return (unsigned int) -1;   /* LSB differs; use max (if we did the bit-shifting
                                   below, we'd end up with max+1 (overflow)) */

  /* calculate the most significant bits of the final result */
  msb = (512 - bucket) << (32 - 9);
  /* calculate the 32-9 least significant bits of the final result by
     looking at the differences in the 32-9 bits following the
     mismatching bit at 'bucket' */
  lsb = 0;
  for (i = bucket + 1;
       (i < sizeof (GNUNET_HashCode) * 8) && (i < bucket + 1 + 32 - 9); i++)
    {
      if (GNUNET_CRYPTO_hash_get_bit (target, i) != GNUNET_CRYPTO_hash_get_bit (have, i))
        lsb |= (1 << (bucket + 32 - 9 - i));    /* first bit set will be 10,
                                                   last bit set will be 31 -- if
                                                   i does not reach 512 first... */
    }
  return msb | lsb;
}

/**
 * Return a number that is larger the closer the
 * "have" GNUNET_hash code is to the "target".
 *
 * @return inverse distance metric, non-zero.
 *         Must fudge the value if NO bits match.
 */
static unsigned int
inverse_distance (const GNUNET_HashCode * target,
                  const GNUNET_HashCode * have)
{
  if (matching_bits(target, have) == 0)
    return 1; /* Never return 0! */
  return ((unsigned int) -1) - distance (target, have);
}

/**
 * Find the optimal bucket for this key, regardless
 * of the current number of buckets in use.
 *
 * @param hc the hashcode to compare our identity to
 *
 * @return the proper bucket index, or GNUNET_SYSERR
 *         on error (same hashcode)
 */
static int find_bucket(const GNUNET_HashCode *hc)
{
  unsigned int bits;

  bits = matching_bits(&my_identity.hashPubKey, hc);
  if (bits == MAX_BUCKETS)
    return GNUNET_SYSERR;
  return MAX_BUCKETS - bits - 1;
}

/**
 * Find which k-bucket this peer should go into,
 * taking into account the size of the k-bucket
 * array.  This means that if more bits match than
 * there are currently buckets, lowest_bucket will
 * be returned.
 *
 * @param hc GNUNET_HashCode we are finding the bucket for.
 *
 * @return the proper bucket index for this key,
 *         or GNUNET_SYSERR on error (same hashcode)
 */
static int find_current_bucket(const GNUNET_HashCode *hc)
{
  int actual_bucket;
  actual_bucket = find_bucket(hc);

  if (actual_bucket == GNUNET_SYSERR) /* hc and our peer identity match! */
    return GNUNET_SYSERR;
  else if (actual_bucket < lowest_bucket) /* actual_bucket not yet used */
    return lowest_bucket;
  else
    return actual_bucket;
}

#if EXTRA_CHECKS
/**
 * Find a routing table entry from a peer identity
 *
 * @param peer the peer to look up
 *
 * @return the bucket number holding the peer, GNUNET_SYSERR if not found
 */
static int
find_bucket_by_peer(const struct PeerInfo *peer)
{
  int bucket;
  struct PeerInfo *pos;

  for (bucket = lowest_bucket; bucket < MAX_BUCKETS - 1; bucket++)
    {
      pos = k_buckets[bucket].head;
      while (pos != NULL)
        {
          if (peer == pos)
            return bucket;
          pos = pos->next;
        }
    }

  return GNUNET_SYSERR; /* No such peer. */
}
#endif

#if PRINT_TABLES
/**
 * Print the complete routing table for this peer.
 */
static void
print_routing_table ()
{
  int bucket;
  struct PeerInfo *pos;
  char char_buf[30000];
  int char_pos;
  memset(char_buf, 0, sizeof(char_buf));
  char_pos = 0;
  char_pos += sprintf(&char_buf[char_pos], "Printing routing table for peer %s\n", my_short_id);
  //fprintf(stderr, "Printing routing table for peer %s\n", my_short_id);
  for (bucket = lowest_bucket; bucket < MAX_BUCKETS; bucket++)
    {
      pos = k_buckets[bucket].head;
      char_pos += sprintf(&char_buf[char_pos], "Bucket %d:\n", bucket);
      //fprintf(stderr, "Bucket %d:\n", bucket);
      while (pos != NULL)
        {
          //fprintf(stderr, "\tPeer %s, best bucket %d, %d bits match\n", GNUNET_i2s(&pos->id), find_bucket(&pos->id.hashPubKey), matching_bits(&pos->id.hashPubKey, &my_identity.hashPubKey));
          char_pos += sprintf(&char_buf[char_pos], "\tPeer %s, best bucket %d, %d bits match\n", GNUNET_i2s(&pos->id), find_bucket(&pos->id.hashPubKey), matching_bits(&pos->id.hashPubKey, &my_identity.hashPubKey));
          pos = pos->next;
        }
    }
  fprintf(stderr, "%s", char_buf);
  fflush(stderr);
}
#endif

/**
 * Find a routing table entry from a peer identity
 *
 * @param peer the peer identity to look up
 *
 * @return the routing table entry, or NULL if not found
 */
static struct PeerInfo *
find_peer_by_id(const struct GNUNET_PeerIdentity *peer)
{
  int bucket;
  struct PeerInfo *pos;
  bucket = find_current_bucket(&peer->hashPubKey);

  if (bucket == GNUNET_SYSERR)
    return NULL;

  pos = k_buckets[bucket].head;
  while (pos != NULL)
    {
      if (0 == memcmp(&pos->id, peer, sizeof(struct GNUNET_PeerIdentity)))
        return pos;
      pos = pos->next;
    }
  return NULL; /* No such peer. */
}

/* Forward declaration */
static void
update_core_preference (void *cls,
                        const struct GNUNET_SCHEDULER_TaskContext *tc);
/**
 * Function called with statistics about the given peer.
 *
 * @param cls closure
 * @param peer identifies the peer
 * @param bpm_in set to the current bandwidth limit (receiving) for this peer
 * @param bpm_out set to the current bandwidth limit (sending) for this peer
 * @param latency current latency estimate, "FOREVER" if we have been
 *                disconnected
 * @param amount set to the amount that was actually reserved or unreserved;
 *               either the full requested amount or zero (no partial reservations)
 * @param preference current traffic preference for the given peer
 */
static void
update_core_preference_finish (void *cls,
                               const struct
                               GNUNET_PeerIdentity * peer,
                               struct GNUNET_BANDWIDTH_Value32NBO bpm_in,
                               struct GNUNET_BANDWIDTH_Value32NBO bpm_out,
                               int amount,
                               uint64_t preference)
{
  struct PeerInfo *peer_info = cls;
  peer_info->info_ctx = NULL;
  GNUNET_SCHEDULER_add_delayed(sched, DHT_DEFAULT_PREFERENCE_INTERVAL, &update_core_preference, peer_info);
}

static void
update_core_preference (void *cls,
                        const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct PeerInfo *peer = cls;
  uint64_t preference;

  if (tc->reason == GNUNET_SCHEDULER_REASON_SHUTDOWN)
    {
      return;
    }

  preference = 2 << matching_bits(&my_identity.hashPubKey, &peer->id.hashPubKey);
  peer->info_ctx = GNUNET_CORE_peer_change_preference (sched, cfg,
                                                       &peer->id,
                                                       GNUNET_TIME_relative_get_forever(),
                                                       GNUNET_BANDWIDTH_value_init (UINT32_MAX),
                                                       0,
                                                       preference,
                                                       &update_core_preference_finish,
                                                       peer);
}

/**
 * Really add a peer to a bucket (only do assertions
 * on size, etc.)
 *
 * @param peer GNUNET_PeerIdentity of the peer to add
 * @param bucket the already figured out bucket to add
 *        the peer to
 * @param latency the core reported latency of this peer
 * @param distance the transport level distance to this peer
 *
 * @return the newly added PeerInfo
 */
static struct PeerInfo *
add_peer(const struct GNUNET_PeerIdentity *peer,
         unsigned int bucket,
         struct GNUNET_TIME_Relative latency,
         unsigned int distance)
{
  struct PeerInfo *new_peer;
  GNUNET_assert(bucket < MAX_BUCKETS);
  GNUNET_assert(peer != NULL);
  new_peer = GNUNET_malloc(sizeof(struct PeerInfo));
  new_peer->latency = latency;
  new_peer->distance = distance;

  memcpy(&new_peer->id, peer, sizeof(struct GNUNET_PeerIdentity));

  GNUNET_CONTAINER_DLL_insert_after(k_buckets[bucket].head,
                                    k_buckets[bucket].tail,
                                    k_buckets[bucket].tail,
                                    new_peer);
  k_buckets[bucket].peers_size++;

  if ((matching_bits(&my_identity.hashPubKey, &peer->hashPubKey) > 0) && (k_buckets[bucket].peers_size <= bucket_size))
    {
      new_peer->preference_task = GNUNET_SCHEDULER_add_now(sched, &update_core_preference, new_peer);
    }

  return new_peer;
}

/**
 * Given a peer and its corresponding bucket,
 * remove it from that bucket.  Does not free
 * the PeerInfo struct, nor cancel messages
 * or free messages waiting to be sent to this
 * peer!
 *
 * @param peer the peer to remove
 * @param bucket the bucket the peer belongs to
 */
static void remove_peer (struct PeerInfo *peer,
                         unsigned int bucket)
{
  GNUNET_assert(k_buckets[bucket].peers_size > 0);
  GNUNET_CONTAINER_DLL_remove(k_buckets[bucket].head,
                              k_buckets[bucket].tail,
                              peer);
  k_buckets[bucket].peers_size--;
  if ((bucket == lowest_bucket) && (k_buckets[lowest_bucket].peers_size == 0) && (lowest_bucket < MAX_BUCKETS - 1))
    lowest_bucket++;
}

/**
 * Removes peer from a bucket, then frees associated
 * resources and frees peer.
 *
 * @param peer peer to be removed and freed
 * @param bucket which bucket this peer belongs to
 */
static void delete_peer (struct PeerInfo *peer,
                         unsigned int bucket)
{
  struct P2PPendingMessage *pos;
  struct P2PPendingMessage *next;
#if EXTRA_CHECKS
  struct PeerInfo *peer_pos;

  peer_pos = k_buckets[bucket].head;
  while ((peer_pos != NULL) && (peer_pos != peer))
    peer_pos = peer_pos->next;
  if (peer_pos == NULL)
    {
      GNUNET_log(GNUNET_ERROR_TYPE_WARNING, "%s:%s: Expected peer `%s' in bucket %d\n", my_short_id, "DHT", GNUNET_i2s(&peer->id), bucket);
      GNUNET_log(GNUNET_ERROR_TYPE_WARNING, "%s:%s: Lowest bucket: %d, find_current_bucket: %d, peer resides in bucket: %d\n", my_short_id, "DHT", lowest_bucket, find_current_bucket(&peer->id.hashPubKey), find_bucket_by_peer(peer));
    }
  GNUNET_assert(peer_pos != NULL);
#endif
  remove_peer(peer, bucket); /* First remove the peer from its bucket */

  if (peer->send_task != GNUNET_SCHEDULER_NO_TASK)
    GNUNET_SCHEDULER_cancel(sched, peer->send_task);
  if (peer->th != NULL)
    GNUNET_CORE_notify_transmit_ready_cancel(peer->th);

  pos = peer->head;
  while (pos != NULL) /* Remove any pending messages for this peer */
    {
      next = pos->next;
      GNUNET_free(pos);
      pos = next;
    }

  GNUNET_assert(GNUNET_CONTAINER_multihashmap_contains(all_known_peers, &peer->id.hashPubKey));
  GNUNET_CONTAINER_multihashmap_remove (all_known_peers, &peer->id.hashPubKey, peer);
  GNUNET_free(peer);
}


/**
 * Iterator over hash map entries.
 *
 * @param cls closure
 * @param key current key code
 * @param value PeerInfo of the peer to move to new lowest bucket
 * @return GNUNET_YES if we should continue to
 *         iterate,
 *         GNUNET_NO if not.
 */
static int move_lowest_bucket (void *cls,
                               const GNUNET_HashCode * key,
                               void *value)
{
  struct PeerInfo *peer = value;
  int new_bucket;

  GNUNET_assert(lowest_bucket > 0);
  new_bucket = lowest_bucket - 1;
  remove_peer(peer, lowest_bucket);
  GNUNET_CONTAINER_DLL_insert_after(k_buckets[new_bucket].head,
                                    k_buckets[new_bucket].tail,
                                    k_buckets[new_bucket].tail,
                                    peer);
  k_buckets[new_bucket].peers_size++;
  return GNUNET_YES;
}


/**
 * The current lowest bucket is full, so change the lowest
 * bucket to the next lower down, and move any appropriate
 * entries in the current lowest bucket to the new bucket.
 */
static void enable_next_bucket()
{
  struct GNUNET_CONTAINER_MultiHashMap *to_remove;
  struct PeerInfo *pos;
  GNUNET_assert(lowest_bucket > 0);
  to_remove = GNUNET_CONTAINER_multihashmap_create(bucket_size);
  pos = k_buckets[lowest_bucket].head;

#if PRINT_TABLES
  fprintf(stderr, "Printing RT before new bucket\n");
  print_routing_table();
#endif
  /* Populate the array of peers which should be in the next lowest bucket */
  while (pos != NULL)
    {
      if (find_bucket(&pos->id.hashPubKey) < lowest_bucket)
        GNUNET_CONTAINER_multihashmap_put(to_remove, &pos->id.hashPubKey, pos, GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY);
      pos = pos->next;
    }

  /* Remove peers from lowest bucket, insert into next lowest bucket */
  GNUNET_CONTAINER_multihashmap_iterate(to_remove, &move_lowest_bucket, NULL);
  GNUNET_CONTAINER_multihashmap_destroy(to_remove);
  lowest_bucket = lowest_bucket - 1;
#if PRINT_TABLES
  fprintf(stderr, "Printing RT after new bucket\n");
  print_routing_table();
#endif
}

/**
 * Find the closest peer in our routing table to the
 * given hashcode.
 *
 * @return The closest peer in our routing table to the
 *         key, or NULL on error.
 */
static struct PeerInfo *
find_closest_peer (const GNUNET_HashCode *hc)
{
  struct PeerInfo *pos;
  struct PeerInfo *current_closest;
  unsigned int lowest_distance;
  unsigned int temp_distance;
  int bucket;
  int count;

  lowest_distance = -1;

  if (k_buckets[lowest_bucket].peers_size == 0)
    return NULL;

  current_closest = NULL;
  for (bucket = lowest_bucket; bucket < MAX_BUCKETS; bucket++)
    {
      pos = k_buckets[bucket].head;
      count = 0;
      while ((pos != NULL) && (count < bucket_size))
        {
          temp_distance = distance(&pos->id.hashPubKey, hc);
          if (temp_distance <= lowest_distance)
            {
              lowest_distance = temp_distance;
              current_closest = pos;
            }
          pos = pos->next;
          count++;
        }
    }
  GNUNET_assert(current_closest != NULL);
  return current_closest;
}


/**
 * Function called to send a request out to another peer.
 * Called both for locally initiated requests and those
 * received from other peers.
 *
 * @param cls DHT service closure argument (unused)
 * @param msg the encapsulated message
 * @param peer the peer to forward the message to
 * @param msg_ctx the context of the message (hop count, bloom, etc.)
 */
static void forward_message (void *cls,
                             const struct GNUNET_MessageHeader *msg,
                             struct PeerInfo *peer,
                             struct DHT_MessageContext *msg_ctx)
{
  struct GNUNET_DHT_P2PRouteMessage *route_message;
  struct P2PPendingMessage *pending;
  size_t msize;
  size_t psize;

  increment_stats(STAT_ROUTE_FORWARDS);

  if ((msg_ctx->closest != GNUNET_YES) && (peer == find_closest_peer(&msg_ctx->key)))
    increment_stats(STAT_ROUTE_FORWARDS_CLOSEST);

  msize = sizeof (struct GNUNET_DHT_P2PRouteMessage) + ntohs(msg->size);
  GNUNET_assert(msize <= GNUNET_SERVER_MAX_MESSAGE_SIZE);
  psize = sizeof(struct P2PPendingMessage) + msize;
  pending = GNUNET_malloc(psize);
  pending->msg = (struct GNUNET_MessageHeader *)&pending[1];
  pending->importance = msg_ctx->importance;
  pending->timeout = msg_ctx->timeout;
  route_message = (struct GNUNET_DHT_P2PRouteMessage *)pending->msg;
  route_message->header.size = htons(msize);
  route_message->header.type = htons(GNUNET_MESSAGE_TYPE_DHT_P2P_ROUTE);
  route_message->options = htonl(msg_ctx->msg_options);
  route_message->hop_count = htonl(msg_ctx->hop_count + 1);
  route_message->network_size = htonl(msg_ctx->network_size);
  route_message->desired_replication_level = htonl(msg_ctx->replication);
  route_message->unique_id = GNUNET_htonll(msg_ctx->unique_id);
  if (msg_ctx->bloom != NULL)
    GNUNET_assert(GNUNET_OK == GNUNET_CONTAINER_bloomfilter_get_raw_data(msg_ctx->bloom, route_message->bloomfilter, DHT_BLOOM_SIZE));
  memcpy(&route_message->key, &msg_ctx->key, sizeof(GNUNET_HashCode));
  memcpy(&route_message[1], msg, ntohs(msg->size));
#if DEBUG_DHT > 1
  GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "%s:%s Adding pending message size %d for peer %s\n", my_short_id, "DHT", msize, GNUNET_i2s(&peer->id));
#endif
  GNUNET_CONTAINER_DLL_insert_after(peer->head, peer->tail, peer->tail, pending);
  if (peer->send_task == GNUNET_SCHEDULER_NO_TASK)
    peer->send_task = GNUNET_SCHEDULER_add_now(sched, &try_core_send, peer);
}

#if DO_PING
/**
 * Task used to send ping messages to peers so that
 * they don't get disconnected.
 *
 * @param cls the peer to send a ping message to
 * @param tc context, reason, etc.
 */
static void
periodic_ping_task (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct PeerInfo *peer = cls;
  struct GNUNET_MessageHeader ping_message;
  struct DHT_MessageContext message_context;

  if (tc->reason == GNUNET_SCHEDULER_REASON_SHUTDOWN)
    return;

  ping_message.size = htons(sizeof(struct GNUNET_MessageHeader));
  ping_message.type = htons(GNUNET_MESSAGE_TYPE_DHT_P2P_PING);

  memset(&message_context, 0, sizeof(struct DHT_MessageContext));
#if DEBUG_PING
  GNUNET_log(GNUNET_ERROR_TYPE_WARNING, "%s:%s Sending periodic ping to %s\n", my_short_id, "DHT", GNUNET_i2s(&peer->id));
#endif
  forward_message(NULL, &ping_message, peer, &message_context);
  peer->ping_task = GNUNET_SCHEDULER_add_delayed(sched, DHT_DEFAULT_PING_DELAY, &periodic_ping_task, peer);
}

/**
 * Schedule PING messages for the top X peers in each
 * bucket of the routing table (so core won't disconnect them!)
 */
void schedule_ping_messages()
{
  unsigned int bucket;
  unsigned int count;
  struct PeerInfo *pos;
  for (bucket = lowest_bucket; bucket < MAX_BUCKETS; bucket++)
    {
      pos = k_buckets[bucket].head;
      count = 0;
      while (pos != NULL)
        {
          if ((count < bucket_size) && (pos->ping_task == GNUNET_SCHEDULER_NO_TASK))
            GNUNET_SCHEDULER_add_now(sched, &periodic_ping_task, pos);
          else if ((count >= bucket_size) && (pos->ping_task != GNUNET_SCHEDULER_NO_TASK))
            {
              GNUNET_SCHEDULER_cancel(sched, pos->ping_task);
              pos->ping_task = GNUNET_SCHEDULER_NO_TASK;
            }
          pos = pos->next;
          count++;
        }
    }
}
#endif

/**
 * Attempt to add a peer to our k-buckets.
 *
 * @param peer, the peer identity of the peer being added
 *
 * @return NULL if the peer was not added,
 *         pointer to PeerInfo for new peer otherwise
 */
static struct PeerInfo *
try_add_peer(const struct GNUNET_PeerIdentity *peer,
             unsigned int bucket,
             struct GNUNET_TIME_Relative latency,
             unsigned int distance)
{
  int peer_bucket;
  struct PeerInfo *new_peer;
  peer_bucket = find_current_bucket(&peer->hashPubKey);
  if (peer_bucket == GNUNET_SYSERR)
    return NULL;

  GNUNET_assert(peer_bucket >= lowest_bucket);
  new_peer = add_peer(peer, peer_bucket, latency, distance);

  if ((k_buckets[lowest_bucket].peers_size) >= bucket_size)
    enable_next_bucket();
#if DO_PING
  schedule_ping_messages();
#endif
  return new_peer;
}


/**
 * Task run to check for messages that need to be sent to a client.
 *
 * @param client a ClientList, containing the client and any messages to be sent to it
 */
static void
process_pending_messages (struct ClientList *client)
{ 
  if (client->pending_head == NULL) 
    return;    
  if (client->transmit_handle != NULL) 
    return;
  client->transmit_handle =
    GNUNET_SERVER_notify_transmit_ready (client->client_handle,
					 ntohs (client->pending_head->msg->
						size),
					 GNUNET_TIME_UNIT_FOREVER_REL,
					 &send_generic_reply, client);
}

/**
 * Callback called as a result of issuing a GNUNET_SERVER_notify_transmit_ready
 * request.  A ClientList is passed as closure, take the head of the list
 * and copy it into buf, which has the result of sending the message to the
 * client.
 *
 * @param cls closure to this call
 * @param size maximum number of bytes available to send
 * @param buf where to copy the actual message to
 *
 * @return the number of bytes actually copied, 0 indicates failure
 */
static size_t
send_generic_reply (void *cls, size_t size, void *buf)
{
  struct ClientList *client = cls;
  char *cbuf = buf;
  struct PendingMessage *reply;
  size_t off;
  size_t msize;

  client->transmit_handle = NULL;
  if (buf == NULL)             
    {
      /* client disconnected */
      return 0;
    }
  off = 0;
  while ( (NULL != (reply = client->pending_head)) &&
	  (size >= off + (msize = ntohs (reply->msg->size))))
    {
      GNUNET_CONTAINER_DLL_remove (client->pending_head,
				   client->pending_tail,
				   reply);
      memcpy (&cbuf[off], reply->msg, msize);
      GNUNET_free (reply);
      off += msize;
    }
  process_pending_messages (client);
  return off;
}


/**
 * Add a PendingMessage to the clients list of messages to be sent
 *
 * @param client the active client to send the message to
 * @param pending_message the actual message to send
 */
static void
add_pending_message (struct ClientList *client,
                     struct PendingMessage *pending_message)
{
  GNUNET_CONTAINER_DLL_insert_after (client->pending_head,
				     client->pending_tail,
				     client->pending_tail,
				     pending_message);
  process_pending_messages (client);
}




/**
 * Called when a reply needs to be sent to a client, as
 * a result it found to a GET or FIND PEER request.
 *
 * @param client the client to send the reply to
 * @param message the encapsulated message to send
 * @param uid the unique identifier of this request
 */
static void
send_reply_to_client (struct ClientList *client,
                      const struct GNUNET_MessageHeader *message,
                      unsigned long long uid)
{
  struct GNUNET_DHT_RouteResultMessage *reply;
  struct PendingMessage *pending_message;
  uint16_t msize;
  size_t tsize;
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s:%s': Sending reply to client.\n", my_short_id, "DHT");
#endif
  msize = ntohs (message->size);
  tsize = sizeof (struct GNUNET_DHT_RouteResultMessage) + msize;
  if (tsize >= GNUNET_SERVER_MAX_MESSAGE_SIZE)
    {
      GNUNET_break_op (0);
      return;
    }

  pending_message = GNUNET_malloc (sizeof (struct PendingMessage) + tsize);
  pending_message->msg = (struct GNUNET_MessageHeader *)&pending_message[1];
  reply = (struct GNUNET_DHT_RouteResultMessage *)&pending_message[1];
  reply->header.type = htons (GNUNET_MESSAGE_TYPE_DHT_LOCAL_ROUTE_RESULT);
  reply->header.size = htons (tsize);
  reply->unique_id = GNUNET_htonll (uid);
  memcpy (&reply[1], message, msize);

  add_pending_message (client, pending_message);
}

/**
 * Consider whether or not we would like to have this peer added to
 * our routing table.  Check whether bucket for this peer is full,
 * if so return negative; if not return positive.  Since peers are
 * only added on CORE level connect, this doesn't actually add the
 * peer to the routing table.
 *
 * @param peer the peer we are considering adding
 *
 * @return GNUNET_YES if we want this peer, GNUNET_NO if not (bucket
 *         already full)
 *
 * FIXME: Think about making a context for this call so that we can
 *        ping the oldest peer in the current bucket and consider
 *        removing it in lieu of the new peer.
 */
static int consider_peer (struct GNUNET_PeerIdentity *peer)
{
  int bucket;

  if (GNUNET_YES == GNUNET_CONTAINER_multihashmap_contains(all_known_peers, &peer->hashPubKey))
    return GNUNET_NO; /* We already know this peer (are connected even!) */
  bucket = find_current_bucket(&peer->hashPubKey);
  if (bucket == GNUNET_SYSERR)
    return GNUNET_NO;
  if ((k_buckets[bucket].peers_size < bucket_size) || ((bucket == lowest_bucket) && (lowest_bucket > 0)))
    return GNUNET_YES;

  return GNUNET_NO;
}

/**
 * Main function that handles whether or not to route a result
 * message to other peers, or to send to our local client.
 *
 * @param msg the result message to be routed
 * @return the number of peers the message was routed to,
 *         GNUNET_SYSERR on failure
 */
static int route_result_message(void *cls,
                                struct GNUNET_MessageHeader *msg,
                                struct DHT_MessageContext *message_context)
{
  struct GNUNET_PeerIdentity new_peer;
  struct DHTQueryRecord *record;
  struct DHTRouteSource *pos;
  struct PeerInfo *peer_info;
  const struct GNUNET_MessageHeader *hello_msg;

  increment_stats(STAT_RESULTS);
  /**
   * If a find peer result message is received and contains a valid
   * HELLO for another peer, offer it to the transport service.
   */
  if (ntohs(msg->type) == GNUNET_MESSAGE_TYPE_DHT_FIND_PEER_RESULT)
    {
      if (ntohs(msg->size) <= sizeof(struct GNUNET_MessageHeader))
        GNUNET_break_op(0);

      hello_msg = &msg[1];
      if ((ntohs(hello_msg->type) != GNUNET_MESSAGE_TYPE_HELLO) || (GNUNET_SYSERR == GNUNET_HELLO_get_id((const struct GNUNET_HELLO_Message *)hello_msg, &new_peer)))
      {
        GNUNET_log(GNUNET_ERROR_TYPE_WARNING, "%s:%s Received non-HELLO message type in find peer result message!\n", my_short_id, "DHT");
        GNUNET_break_op(0);
        return GNUNET_NO;
      }
      else /* We have a valid hello, and peer id stored in new_peer */
      {
        find_peer_context.count++;
        increment_stats(STAT_FIND_PEER_REPLY);
        if (GNUNET_YES == consider_peer(&new_peer))
        {
          increment_stats(STAT_HELLOS_PROVIDED);
          GNUNET_TRANSPORT_offer_hello(transport_handle, hello_msg);
          GNUNET_CORE_peer_request_connect(sched, cfg, GNUNET_TIME_UNIT_FOREVER_REL, &new_peer, NULL, NULL);
        }
      }
    }

  if (malicious_dropper == GNUNET_YES)
    record = NULL;
  else
    record = GNUNET_CONTAINER_multihashmap_get(forward_list.hashmap, &message_context->key);

  if (record == NULL) /* No record of this message! */
    {
#if DEBUG_DHT
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "`%s:%s': Have no record of response key %s uid %llu\n", my_short_id,
                "DHT", GNUNET_h2s (message_context->key), message_context->unique_id);
#endif
#if DEBUG_DHT_ROUTING

      if ((debug_routes_extended) && (dhtlog_handle != NULL))
        {
          dhtlog_handle->insert_route (NULL,
                                       message_context->unique_id,
                                       DHTLOG_RESULT,
                                       message_context->hop_count,
                                       GNUNET_SYSERR,
                                       &my_identity,
                                       &message_context->key,
                                       message_context->peer, NULL);
        }
#endif
      if (message_context->bloom != NULL)
        {
          GNUNET_CONTAINER_bloomfilter_free(message_context->bloom);
          message_context->bloom = NULL;
        }
      return 0;
    }

  pos = record->head;
  while (pos != NULL)
    {
#if STRICT_FORWARDING
      if (ntohs(msg->type) == GNUNET_MESSAGE_TYPE_DHT_FIND_PEER_RESULT) /* If we have already forwarded this peer id, don't do it again! */
        {
          if (GNUNET_YES == GNUNET_CONTAINER_bloomfilter_test (pos->find_peers_responded, &new_peer.hashPubKey))
          {
            increment_stats("# find peer responses NOT forwarded (bloom match)");
            pos = pos->next;
            continue;
          }
          else
            GNUNET_CONTAINER_bloomfilter_add(pos->find_peers_responded, &new_peer.hashPubKey);
        }
#endif

      if (0 == memcmp(&pos->source, &my_identity, sizeof(struct GNUNET_PeerIdentity))) /* Local client (or DHT) initiated request! */
        {
#if DEBUG_DHT
          GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                      "`%s:%s': Sending response key %s uid %llu to client\n", my_short_id,
                      "DHT", GNUNET_h2s (message_context->key), message_context->unique_id);
#endif
#if DEBUG_DHT_ROUTING
          if ((debug_routes_extended) && (dhtlog_handle != NULL))
            {
              dhtlog_handle->insert_route (NULL, message_context->unique_id, DHTLOG_RESULT,
                                           message_context->hop_count,
                                           GNUNET_YES, &my_identity, &message_context->key,
                                           message_context->peer, NULL);
            }
#endif
          increment_stats(STAT_RESULTS_TO_CLIENT);
          if (ntohs(msg->type) == GNUNET_MESSAGE_TYPE_DHT_GET_RESULT)
            increment_stats(STAT_GET_REPLY);

          send_reply_to_client(pos->client, msg, message_context->unique_id);
        }
      else /* Send to peer */
        {
          peer_info = find_peer_by_id(&pos->source);
          if (peer_info == NULL) /* Didn't find the peer in our routing table, perhaps peer disconnected! */
            {
              pos = pos->next;
              continue;
            }

          if (message_context->bloom == NULL)
            message_context->bloom = GNUNET_CONTAINER_bloomfilter_init (NULL, DHT_BLOOM_SIZE, DHT_BLOOM_K);
          GNUNET_CONTAINER_bloomfilter_add (message_context->bloom, &my_identity.hashPubKey);
          if ((GNUNET_NO == GNUNET_CONTAINER_bloomfilter_test (message_context->bloom, &peer_info->id.hashPubKey)))
            {
#if DEBUG_DHT
              GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                          "`%s:%s': Forwarding response key %s uid %llu to peer %s\n", my_short_id,
                          "DHT", GNUNET_h2s (message_context->key), message_context->unique_id, GNUNET_i2s(&peer_info->id));
#endif
#if DEBUG_DHT_ROUTING
              if ((debug_routes_extended) && (dhtlog_handle != NULL))
                {
                  dhtlog_handle->insert_route (NULL, message_context->unique_id,
                                               DHTLOG_RESULT,
                                               message_context->hop_count,
                                               GNUNET_NO, &my_identity, &message_context->key,
                                               message_context->peer, &pos->source);
                }
#endif
              forward_result_message(cls, msg, peer_info, message_context);
            }
          else
            {
#if DEBUG_DHT
              GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                          "`%s:%s': NOT Forwarding response (bloom match) key %s uid %llu to peer %s\n", my_short_id,
                          "DHT", GNUNET_h2s (message_context->key), message_context->unique_id, GNUNET_i2s(&peer_info->id));
#endif
            }
        }
      pos = pos->next;
    }
  if (message_context->bloom != NULL)
    GNUNET_CONTAINER_bloomfilter_free(message_context->bloom);
  return 0;
}

/**
 * Iterator for local get request results,
 *
 * @param cls closure for iterator, a DatacacheGetContext
 * @param exp when does this value expire?
 * @param key the key this data is stored under
 * @param size the size of the data identified by key
 * @param data the actual data
 * @param type the type of the data
 *
 * @return GNUNET_OK to continue iteration, anything else
 * to stop iteration.
 */
static int
datacache_get_iterator (void *cls,
                        struct GNUNET_TIME_Absolute exp,
                        const GNUNET_HashCode * key,
                        uint32_t size, const char *data, uint32_t type)
{
  struct DHT_MessageContext *msg_ctx = cls;
  struct DHT_MessageContext *new_msg_ctx;
  struct GNUNET_DHT_GetResultMessage *get_result;
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s:%s': Received `%s' response from datacache\n", my_short_id, "DHT", "GET");
#endif
  new_msg_ctx = GNUNET_malloc(sizeof(struct DHT_MessageContext));
  memcpy(new_msg_ctx, msg_ctx, sizeof(struct DHT_MessageContext));
  get_result =
    GNUNET_malloc (sizeof (struct GNUNET_DHT_GetResultMessage) + size);
  get_result->header.type = htons (GNUNET_MESSAGE_TYPE_DHT_GET_RESULT);
  get_result->header.size =
    htons (sizeof (struct GNUNET_DHT_GetResultMessage) + size);
  get_result->expiration = GNUNET_TIME_absolute_hton(exp);
  get_result->type = htons (type);
  memcpy (&get_result[1], data, size);
  new_msg_ctx->peer = &my_identity;
  new_msg_ctx->bloom = GNUNET_CONTAINER_bloomfilter_init (NULL, DHT_BLOOM_SIZE, DHT_BLOOM_K);
  new_msg_ctx->hop_count = 0;
  new_msg_ctx->importance = DHT_DEFAULT_P2P_IMPORTANCE * 2; /* Make result routing a higher priority */
  new_msg_ctx->timeout = DHT_DEFAULT_P2P_TIMEOUT;
  increment_stats(STAT_GET_RESPONSE_START);
  route_result_message(cls, &get_result->header, new_msg_ctx);
  GNUNET_free(new_msg_ctx);
  //send_reply_to_client (datacache_get_ctx->client, &get_result->header,
  //                      datacache_get_ctx->unique_id);
  GNUNET_free (get_result);
  return GNUNET_OK;
}


/**
 * Server handler for all dht get requests, look for data,
 * if found, send response either to clients or other peers.
 *
 * @param cls closure for service
 * @param msg the actual get message
 * @param message_context struct containing pertinent information about the get request
 *
 * @return number of items found for GET request
 */
static unsigned int
handle_dht_get (void *cls, 
		const struct GNUNET_MessageHeader *msg,
                struct DHT_MessageContext *message_context)
{
  const struct GNUNET_DHT_GetMessage *get_msg;
  uint16_t get_type;
  unsigned int results;

  get_msg = (const struct GNUNET_DHT_GetMessage *) msg;
  if (ntohs (get_msg->header.size) != sizeof (struct GNUNET_DHT_GetMessage))
    {
      GNUNET_break (0);
      return 0;
    }

  get_type = ntohs (get_msg->type);
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s:%s': Received `%s' request, message type %u, key %s, uid %llu\n", my_short_id,
              "DHT", "GET", get_type, GNUNET_h2s (message_context->key),
              message_context->unique_id);
#endif
  increment_stats(STAT_GETS);
  results = 0;
  if (get_type == DHT_MALICIOUS_MESSAGE_TYPE)
    return results;

  if (datacache != NULL)
    results =
      GNUNET_DATACACHE_get (datacache, &message_context->key, get_type,
                            &datacache_get_iterator, message_context);

  if (results >= 1)
    {
#if DEBUG_DHT
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "`%s:%s': Found %d results for `%s' request uid %llu\n", my_short_id, "DHT",
                  results, "GET", message_context->unique_id);
#endif
#if DEBUG_DHT_ROUTING
      if ((debug_routes) && (dhtlog_handle != NULL))
        {
          dhtlog_handle->insert_query (NULL, message_context->unique_id, DHTLOG_GET,
                                message_context->hop_count, GNUNET_YES, &my_identity,
                                &message_context->key);
        }

      if ((debug_routes_extended) && (dhtlog_handle != NULL))
        {
          dhtlog_handle->insert_route (NULL, message_context->unique_id, DHTLOG_ROUTE,
                                       message_context->hop_count, GNUNET_YES,
                                       &my_identity, &message_context->key, message_context->peer,
                                       NULL);
        }
#endif
    }

  if (message_context->hop_count == 0) /* Locally initiated request */
    {
#if DEBUG_DHT_ROUTING
    if ((debug_routes) && (dhtlog_handle != NULL))
      {
        dhtlog_handle->insert_query (NULL, message_context->unique_id, DHTLOG_GET,
                                      message_context->hop_count, GNUNET_NO, &my_identity,
                                      &message_context->key);
      }
#endif
    }

  return results;
}

static void
remove_recent_find_peer(void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  GNUNET_HashCode *key = cls;
  if (GNUNET_YES == GNUNET_CONTAINER_multihashmap_remove(recent_find_peer_requests, key, key))
    {
      GNUNET_free(key);
    }
}

/**
 * Server handler for initiating local dht find peer requests
 *
 * @param cls closure for service
 * @param find_msg the actual find peer message
 * @param message_context struct containing pertinent information about the request
 *
 */
static void
handle_dht_find_peer (void *cls, 
                      const struct GNUNET_MessageHeader *find_msg,
                      struct DHT_MessageContext *message_context)
{
  struct GNUNET_MessageHeader *find_peer_result;
  struct GNUNET_DHT_FindPeerMessage *find_peer_message;
  struct DHT_MessageContext *new_msg_ctx;
  struct GNUNET_CONTAINER_BloomFilter *incoming_bloom;
  size_t hello_size;
  size_t tsize;
  GNUNET_HashCode *recent_hash;
#if RESTRICT_FIND_PEER
  struct GNUNET_PeerIdentity peer_id;
#endif

  find_peer_message = (struct GNUNET_DHT_FindPeerMessage *)find_msg;
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s:%s': Received `%s' request from client, key %s (msg size %d, we expected %d)\n",
              my_short_id, "DHT", "FIND PEER", GNUNET_h2s (message_context->key),
              ntohs (find_msg->size),
              sizeof (struct GNUNET_MessageHeader));
#endif
  if (my_hello == NULL)
  {
#if DEBUG_DHT
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "`%s': Our HELLO is null, can't return.\n",
                "DHT");
#endif
    return;
  }

  incoming_bloom = GNUNET_CONTAINER_bloomfilter_init(find_peer_message->bloomfilter, DHT_BLOOM_SIZE, DHT_BLOOM_K);
  if (GNUNET_YES == GNUNET_CONTAINER_bloomfilter_test(incoming_bloom, &my_identity.hashPubKey))
    {
      increment_stats(STAT_BLOOM_FIND_PEER);
      GNUNET_CONTAINER_bloomfilter_free(incoming_bloom);
      return; /* We match the bloomfilter, do not send a response to this peer (they likely already know us!)*/
    }
  GNUNET_CONTAINER_bloomfilter_free(incoming_bloom);

#if RESTRICT_FIND_PEER

  /**
   * Ignore any find peer requests from a peer we have seen very recently.
   */
  if (GNUNET_YES == GNUNET_CONTAINER_multihashmap_contains(recent_find_peer_requests, &message_context->key)) /* We have recently responded to a find peer request for this peer! */
  {
    increment_stats("# dht find peer requests ignored (recently seen!)");
    return;
  }

  /**
   * Use this check to only allow the peer to respond to find peer requests if
   * it would be beneficial to have the requesting peer in this peers routing
   * table.  Can be used to thwart peers flooding the network with find peer
   * requests that we don't care about.  However, if a new peer is joining
   * the network and has no other peers this is a problem (assume all buckets
   * full, no one will respond!).
   */
  memcpy(&peer_id.hashPubKey, &message_context->key, sizeof(GNUNET_HashCode));
  if (GNUNET_NO == consider_peer(&peer_id))
    {
      increment_stats("# dht find peer requests ignored (do not need!)");
      return;
    }
#endif

  recent_hash = GNUNET_malloc(sizeof(GNUNET_HashCode));
  memcpy(recent_hash, &message_context->key, sizeof(GNUNET_HashCode));
  GNUNET_CONTAINER_multihashmap_put (recent_find_peer_requests, &message_context->key, NULL, GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY);
  GNUNET_SCHEDULER_add_delayed (sched, GNUNET_TIME_relative_multiply(GNUNET_TIME_UNIT_SECONDS, 30), &remove_recent_find_peer, &recent_hash);

  /* Simplistic find_peer functionality, always return our hello */
  hello_size = ntohs(my_hello->size);
  tsize = hello_size + sizeof (struct GNUNET_MessageHeader);

  if (tsize >= GNUNET_SERVER_MAX_MESSAGE_SIZE)
    {
      GNUNET_break_op (0);
      return;
    }

  find_peer_result = GNUNET_malloc (tsize);
  find_peer_result->type = htons (GNUNET_MESSAGE_TYPE_DHT_FIND_PEER_RESULT);
  find_peer_result->size = htons (tsize);
  memcpy (&find_peer_result[1], my_hello, hello_size);

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s': Sending hello size %d to requesting peer.\n",
              "DHT", hello_size);

  new_msg_ctx = GNUNET_malloc(sizeof(struct DHT_MessageContext));
  memcpy(new_msg_ctx, message_context, sizeof(struct DHT_MessageContext));
  new_msg_ctx->peer = &my_identity;
  new_msg_ctx->bloom = GNUNET_CONTAINER_bloomfilter_init (NULL, DHT_BLOOM_SIZE, DHT_BLOOM_K);
  new_msg_ctx->hop_count = 0;
  new_msg_ctx->importance = DHT_DEFAULT_P2P_IMPORTANCE * 2; /* Make find peer requests a higher priority */
  new_msg_ctx->timeout = DHT_DEFAULT_P2P_TIMEOUT;
  increment_stats(STAT_FIND_PEER_ANSWER);
  route_result_message(cls, find_peer_result, new_msg_ctx);
  GNUNET_free(new_msg_ctx);
#if DEBUG_DHT_ROUTING
  if ((debug_routes) && (dhtlog_handle != NULL))
    {
      dhtlog_handle->insert_query (NULL, message_context->unique_id, DHTLOG_FIND_PEER,
                                   message_context->hop_count, GNUNET_YES, &my_identity,
                                   &message_context->key);
    }
#endif
  GNUNET_free(find_peer_result);
}


/**
 * Server handler for initiating local dht put requests
 *
 * @param cls closure for service
 * @param msg the actual put message
 * @param message_context struct containing pertinent information about the request
 */
static void
handle_dht_put (void *cls,
		const struct GNUNET_MessageHeader *msg,
                struct DHT_MessageContext *message_context)
{
  struct GNUNET_DHT_PutMessage *put_msg;
  size_t put_type;
  size_t data_size;

  GNUNET_assert (ntohs (msg->size) >=
                 sizeof (struct GNUNET_DHT_PutMessage));


  put_msg = (struct GNUNET_DHT_PutMessage *)msg;
  put_type = ntohs (put_msg->type);

  if (put_type == DHT_MALICIOUS_MESSAGE_TYPE)
    return;

  data_size = ntohs (put_msg->header.size) - sizeof (struct GNUNET_DHT_PutMessage);
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s:%s': Received `%s' request (inserting data!), message type %d, key %s, uid %llu\n",
              my_short_id, "DHT", "PUT", put_type, GNUNET_h2s (message_context->key), message_context->unique_id);
#endif
#if DEBUG_DHT_ROUTING
  if (message_context->hop_count == 0) /* Locally initiated request */
    {
      if ((debug_routes) && (dhtlog_handle != NULL))
        {
          dhtlog_handle->insert_query (NULL, message_context->unique_id, DHTLOG_PUT,
                                       message_context->hop_count, GNUNET_NO, &my_identity,
                                       &message_context->key);
        }
    }
#endif

  if (message_context->closest != GNUNET_YES)
    return;

#if DEBUG_DHT_ROUTING
  if ((debug_routes_extended) && (dhtlog_handle != NULL))
    {
      dhtlog_handle->insert_route (NULL, message_context->unique_id, DHTLOG_ROUTE,
                                   message_context->hop_count, GNUNET_YES,
                                   &my_identity, &message_context->key, message_context->peer,
                                   NULL);
    }

  if ((debug_routes) && (dhtlog_handle != NULL))
    {
      dhtlog_handle->insert_query (NULL, message_context->unique_id, DHTLOG_PUT,
                                   message_context->hop_count, GNUNET_YES, &my_identity,
                                   &message_context->key);
    }
#endif

  increment_stats(STAT_PUTS_INSERTED);
  if (datacache != NULL)
    GNUNET_DATACACHE_put (datacache, &message_context->key, data_size,
                          (char *) &put_msg[1], put_type,
                          GNUNET_TIME_absolute_ntoh(put_msg->expiration));
  else
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "`%s:%s': %s request received, but have no datacache!\n",
                my_short_id, "DHT", "PUT");
}

/**
 * Estimate the diameter of the network based
 * on how many buckets are currently in use.
 * Concept here is that the diameter of the network
 * is roughly the distance a message must travel in
 * order to reach its intended destination.  Since
 * at each hop we expect to get one bit closer, and
 * we have one bit per bucket, the number of buckets
 * in use should be the largest number of hops for
 * a sucessful message. (of course, this assumes we
 * know all peers in the network!)
 *
 * @return ballpark diameter figure
 */
static unsigned int estimate_diameter()
{
  return MAX_BUCKETS - lowest_bucket;
}

/**
 * To how many peers should we (on average)
 * forward the request to obtain the desired
 * target_replication count (on average).
 *
 * Always 0, 1 or 2 (don't send, send once, split)
 */
static unsigned int
get_forward_count (unsigned int hop_count, size_t target_replication)
{
#if DOUBLE
  double target_count;
  double random_probability;
#else
  uint32_t random_value;
#endif
  unsigned int target_value;
  unsigned int diameter;

  /**
   * If we are behaving in strict kademlia mode, send multiple initial requests,
   * but then only send to 1 or 0 peers based strictly on the number of hops.
   */
  if (strict_kademlia == GNUNET_YES)
    {
      if (hop_count == 0)
        return DHT_KADEMLIA_REPLICATION;
      else if (hop_count < MAX_HOPS)
        return 1;
      else
        return 0;
    }

  /* FIXME: the smaller we think the network is the more lenient we should be for
   * routing right?  The estimation below only works if we think we have reasonably
   * full routing tables, which for our RR topologies may not be the case!
   */
  diameter = estimate_diameter ();
  if ((hop_count > (diameter + 1) * 2) && (MINIMUM_PEER_THRESHOLD < estimate_diameter() * bucket_size))
    {
#if DEBUG_DHT
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "`%s:%s': Hop count too high (est %d, lowest %d), NOT Forwarding request\n", my_short_id,
                  "DHT", estimate_diameter(), lowest_bucket);
#endif
      return 0;
    }
  else if (hop_count > MAX_HOPS)
    {
#if DEBUG_DHT
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "`%s:%s': Hop count too high (greater than max)\n", my_short_id,
                  "DHT");
#endif
      return 0;
    }

#if DOUBLE
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING, "Replication %d, hop_count %u, diameter %u\n", target_replication, hop_count, diameter);
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING, "Numerator %f, denominator %f\n", (double)target_replication, ((double)target_replication * (hop_count + 1) + diameter));
  target_count = /* target_count is ALWAYS < 1 unless replication is < 1 */
    (double)target_replication / ((double)target_replication * (hop_count + 1) + diameter);
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING, "Target count is %f\n", target_count);
  random_probability = ((double)GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_WEAK,
      RAND_MAX)) / RAND_MAX;
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING, "Random is %f\n", random_probability);

  target_value = 0;
  //while (target_value < target_count)
  if (target_value < target_count)
    target_value++; /* target_value is ALWAYS 1 after this "loop", right?  Because target_count is always > 0, right?  Or does it become 0.00000... at some point because the hop count is so high? */


  //if ((target_count + 1 - (double)target_value) > random_probability)
  if ((target_count) > random_probability)
    target_value++;
#endif

  random_value = GNUNET_CRYPTO_random_u32(GNUNET_CRYPTO_QUALITY_STRONG, target_replication * (hop_count + 1) + diameter) + 1;
  GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "replication %u, at hop %d, will split with probability %f\n", target_replication, hop_count, target_replication / (double)((target_replication * (hop_count + 1) + diameter) + 1));
  target_value = 1;
  GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "random %u, target %u, max %u\n", random_value, target_replication, target_replication * (hop_count + 1) + diameter);
  if (random_value < target_replication)
    target_value++;

  return target_value;
}

/*
 * Check whether my identity is closer than any known peers.
 * If a non-null bloomfilter is given, check if this is the closest
 * peer that hasn't already been routed to.
 *
 * @param target hash code to check closeness to
 * @param bloom bloomfilter, exclude these entries from the decision
 *
 * Return GNUNET_YES if node location is closest, GNUNET_NO
 * otherwise.
 */
int
am_closest_peer (const GNUNET_HashCode * target, struct GNUNET_CONTAINER_BloomFilter *bloom)
{
  int bits;
  int other_bits;
  int bucket_num;
  int count;
  struct PeerInfo *pos;
  unsigned int my_distance;

  bucket_num = find_current_bucket(target);
  if (bucket_num == GNUNET_SYSERR) /* Same key! */
    return GNUNET_YES;

  bits = matching_bits(&my_identity.hashPubKey, target);
  my_distance = distance(&my_identity.hashPubKey, target);
  pos = k_buckets[bucket_num].head;
  count = 0;
  while ((pos != NULL) && (count < bucket_size))
    {
      if ((bloom != NULL) && (GNUNET_YES == GNUNET_CONTAINER_bloomfilter_test(bloom, &pos->id.hashPubKey)))
        {
          pos = pos->next;
          continue; /* Skip already checked entries */
        }

      other_bits = matching_bits(&pos->id.hashPubKey, target);
      if (other_bits > bits)
        return GNUNET_NO;
      else if (other_bits == bits) /* We match the same number of bits, do distance comparison */
        {
          if (strict_kademlia != GNUNET_YES) /* Return that we at as close as any other peer */
            return GNUNET_YES;
          else if (distance(&pos->id.hashPubKey, target) < my_distance) /* Check all known peers, only return if we are the true closest */
            return GNUNET_NO;
        }
      pos = pos->next;
    }

#if DEBUG_TABLE
  GNUNET_GE_LOG (coreAPI->ectx,
                 GNUNET_GE_WARNING | GNUNET_GE_ADMIN | GNUNET_GE_USER |
                 GNUNET_GE_BULK, "closest peer\n");
  printPeerBits (&closest);
  GNUNET_GE_LOG (coreAPI->ectx,
                 GNUNET_GE_WARNING | GNUNET_GE_ADMIN | GNUNET_GE_USER |
                 GNUNET_GE_BULK, "me\n");
  printPeerBits (coreAPI->my_identity);
  GNUNET_GE_LOG (coreAPI->ectx,
                 GNUNET_GE_WARNING | GNUNET_GE_ADMIN | GNUNET_GE_USER |
                 GNUNET_GE_BULK, "key\n");
  printKeyBits (target);
  GNUNET_GE_LOG (coreAPI->ectx,
                 GNUNET_GE_WARNING | GNUNET_GE_ADMIN | GNUNET_GE_USER |
                 GNUNET_GE_BULK,
                 "closest peer inverse distance is %u, mine is %u\n",
                 inverse_distance (target, &closest.hashPubKey),
                 inverse_distance (target,
                                   &coreAPI->my_identity->hashPubKey));
#endif

  /* No peers closer, we are the closest! */
  return GNUNET_YES;

}

/**
 * Decide whether to route this request exclusively
 * to a closer peer (if closer peers exist) or to choose
 * from the whole set of peers.
 *
 * @param hops number of hops this message has already traveled
 */
int
route_closer (const GNUNET_HashCode *target, struct GNUNET_CONTAINER_BloomFilter *bloom,
              unsigned int hops)
{
  unsigned int my_matching_bits;
  unsigned int bc;
  uint32_t random_value;
  struct PeerInfo *pos;
  int have_closer;
  int count;
  my_matching_bits = matching_bits(target, &my_identity.hashPubKey);

  /**
   * First check if we know any close (as close as us or closer) peers.
   */
  have_closer = GNUNET_NO;
  count = 0;
  for (bc = lowest_bucket; bc < MAX_BUCKETS; bc++)
    {
      pos = k_buckets[bc].head;
      count = 0;
      while ((pos != NULL) && (count < bucket_size))
        {
          if ((matching_bits(target, &pos->id.hashPubKey) > my_matching_bits) &&
              (GNUNET_NO == GNUNET_CONTAINER_bloomfilter_test (bloom, &pos->id.hashPubKey)))
            {
              have_closer = GNUNET_YES;
              break;
            }
          pos = pos->next;
          count++;
        }
      if (have_closer == GNUNET_YES)
        break;
    }

  if (have_closer == GNUNET_NO) /* We don't have a same distance or closer node, can't enforce closer only! */
    return GNUNET_NO;

  switch (converge_option)
    {
      case DHT_CONVERGE_LINEAR:
        /**
         * Simple linear curve for choosing whether or not to converge.
         * Choose to route only closer with probability hops/MAX_HOPS.
         */
        random_value = GNUNET_CRYPTO_random_u32(GNUNET_CRYPTO_QUALITY_WEAK, MAX_HOPS);
        if (random_value < hops)
          return GNUNET_YES;
        else
          return GNUNET_NO;
      case DHT_CONVERGE_SQUARE:
        /**
         * Simple square based curve.
         */
        if ((GNUNET_CRYPTO_random_u32(GNUNET_CRYPTO_QUALITY_WEAK, (uint32_t)-1) / (double)(uint32_t)-1) < (sqrt(hops) / sqrt(MAX_HOPS)))
          return GNUNET_YES;
        else
          return GNUNET_NO;
      default:
        return GNUNET_NO;
    }
}

/**
 * Select a peer from the routing table that would be a good routing
 * destination for sending a message for "target".  The resulting peer
 * must not be in the set of blocked peers.<p>
 *
 * Note that we should not ALWAYS select the closest peer to the
 * target, peers further away from the target should be chosen with
 * exponentially declining probability.
 *
 * @param target the key we are selecting a peer to route to
 * @param bloom a bloomfilter containing entries this request has seen already
 *
 * @return Peer to route to, or NULL on error
 */
static struct PeerInfo *
select_peer (const GNUNET_HashCode * target,
             struct GNUNET_CONTAINER_BloomFilter *bloom, unsigned int hops)
{
  unsigned int distance;
  unsigned int bc;
  unsigned int count;
  unsigned int my_matching_bits;
  unsigned long long largest_distance;
#if REAL_DISTANCE
  unsigned long long total_distance;
  unsigned long long selected;
#else
  unsigned int total_distance;
  unsigned int selected;
#endif

  int only_closer;
  struct PeerInfo *pos;
  struct PeerInfo *chosen;
  char *temp_stat;

  my_matching_bits = matching_bits(target, &my_identity.hashPubKey);
  only_closer = route_closer(target, bloom, hops);

  if (GNUNET_YES == only_closer)
    {
      GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "only routing to closer peers!\n");
      GNUNET_asprintf(&temp_stat, "# closer only routes at hop %u", hops);
      increment_stats(temp_stat);
    }
  else
    {
      GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "routing to all possible peers!\n");
      GNUNET_asprintf(&temp_stat, "# NOT closer only routes at hop %u", hops);
      increment_stats(temp_stat);
    }

  GNUNET_free(temp_stat);

  if (strict_kademlia == GNUNET_YES)
    {
      largest_distance = 0;
      chosen = NULL;
      for (bc = lowest_bucket; bc < MAX_BUCKETS; bc++)
        {
          pos = k_buckets[bc].head;
          count = 0;
          while ((pos != NULL) && (count < bucket_size))
            {
              /* If we are doing strict Kademlia routing, then checking the bloomfilter is basically cheating! */
              if (GNUNET_NO == GNUNET_CONTAINER_bloomfilter_test (bloom, &pos->id.hashPubKey))
                {
                  distance = inverse_distance (target, &pos->id.hashPubKey);
                  if (distance > largest_distance)
                    {
                      chosen = pos;
                      largest_distance = distance;
                    }
                }
              count++;
              pos = pos->next;
            }
        }

      if ((largest_distance > 0) && (chosen != NULL))
        {
          GNUNET_CONTAINER_bloomfilter_add(bloom, &chosen->id.hashPubKey);
          return chosen;
        }
      else
        {
          return NULL;
        }
    }
  else
    {
      /* GNUnet-style */
      total_distance = 0;
      for (bc = lowest_bucket; bc < MAX_BUCKETS; bc++)
        {
          pos = k_buckets[bc].head;
          count = 0;
          while ((pos != NULL) && (count < bucket_size))
            {
              if ((GNUNET_NO == GNUNET_CONTAINER_bloomfilter_test (bloom, &pos->id.hashPubKey)) &&
                  ((only_closer == GNUNET_NO) || (matching_bits(target, &pos->id.hashPubKey) >= my_matching_bits)))
                {
#if REAL_DISTANCE /* Use the "real" distance as computed by the inverse_distance function */
                  /** The "real" distance is best for routing to the closest peer, but in practice
                   * (with our routing algorithm) it is usually better to use the squared bit distance.
                   * This gives us a higher probability of routing towards close peers.
                   */
                  total_distance += (unsigned long long)inverse_distance (target, &pos->id.hashPubKey);
#else
                  total_distance += matching_bits(target, &pos->id.hashPubKey) * matching_bits(target ,&pos->id.hashPubKey);
#endif
                }
  #if DEBUG_DHT > 1
              GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                          "`%s:%s': Total distance is %llu, distance from %s to %s is %u\n",
                          my_short_id, "DHT", total_distance, GNUNET_i2s(&pos->id), GNUNET_h2s(target) , inverse_distance(target, &pos->id.hashPubKey));
  #endif
              pos = pos->next;
              count++;
            }
        }
      if (total_distance == 0)
        {
          increment_stats("# select_peer, total_distance == 0");
          return NULL;
        }

      selected = GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_WEAK, total_distance);
      for (bc = lowest_bucket; bc < MAX_BUCKETS; bc++)
        {
          pos = k_buckets[bc].head;
          count = 0;
          while ((pos != NULL) && (count < bucket_size))
            {
              if ((GNUNET_NO == GNUNET_CONTAINER_bloomfilter_test (bloom, &pos->id.hashPubKey)) &&
                  ((only_closer == GNUNET_NO) || (matching_bits(target, &pos->id.hashPubKey) >= my_matching_bits)))
                {
#if REAL_DISTANCE
                  distance = inverse_distance (target, &pos->id.hashPubKey);
#else
                  distance = matching_bits(target, &pos->id.hashPubKey) * matching_bits(target, &pos->id.hashPubKey);
#endif
                  if (distance > selected)
                    {
                      GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "Selected peer with %u matching bits to route to\n", distance);
                      return pos;
                    }
                  selected -= distance;
                }
              else
                {
  #if DEBUG_DHT
                  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                              "`%s:%s': peer %s matches bloomfilter.\n",
                              my_short_id, "DHT", GNUNET_i2s(&pos->id));
  #endif
                }
              pos = pos->next;
              count++;
            }
        }
  #if DEBUG_DHT
        GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                    "`%s:%s': peer %s matches bloomfilter.\n",
                    my_short_id, "DHT", GNUNET_i2s(&pos->id));
  #endif
      increment_stats("# failed to select peer");
      GNUNET_assert(only_closer == GNUNET_NO);
      return NULL;
    }
}

/**
 * Task used to remove recent entries, either
 * after timeout, when full, or on shutdown.
 *
 * @param cls the entry to remove
 * @param tc context, reason, etc.
 */
static void
remove_recent (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct RecentRequest *req = cls;
  static GNUNET_HashCode hash;

  GNUNET_assert(req != NULL);
  hash_from_uid(req->uid, &hash);
  GNUNET_assert (GNUNET_YES == GNUNET_CONTAINER_multihashmap_remove(recent.hashmap, &hash, req));
  GNUNET_CONTAINER_heap_remove_node(recent.minHeap, req->heap_node);
  GNUNET_CONTAINER_bloomfilter_free(req->bloom);
  GNUNET_free(req);

  if ((tc->reason == GNUNET_SCHEDULER_REASON_SHUTDOWN) && (0 == GNUNET_CONTAINER_multihashmap_size(recent.hashmap)) && (0 == GNUNET_CONTAINER_heap_get_size(recent.minHeap)))
  {
    GNUNET_CONTAINER_multihashmap_destroy(recent.hashmap);
    GNUNET_CONTAINER_heap_destroy(recent.minHeap);
  }
}


/**
 * Task used to remove forwarding entries, either
 * after timeout, when full, or on shutdown.
 *
 * @param cls the entry to remove
 * @param tc context, reason, etc.
 */
static void
remove_forward_entry (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct DHTRouteSource *source_info = cls;
  struct DHTQueryRecord *record;
  source_info = GNUNET_CONTAINER_heap_remove_node(forward_list.minHeap, source_info->hnode);
  record = source_info->record;
  GNUNET_CONTAINER_DLL_remove(record->head, record->tail, source_info);

  if (record->head == NULL) /* No more entries in DLL */
    {
      GNUNET_assert(GNUNET_YES == GNUNET_CONTAINER_multihashmap_remove(forward_list.hashmap, &record->key, record));
      GNUNET_free(record);
    }
  if (source_info->find_peers_responded != NULL)
    GNUNET_CONTAINER_bloomfilter_free(source_info->find_peers_responded);
  GNUNET_free(source_info);
}

/**
 * Remember this routing request so that if a reply is
 * received we can either forward it to the correct peer
 * or return the result locally.
 *
 * @param cls DHT service closure
 * @param msg_ctx Context of the route request
 *
 * @return GNUNET_YES if this response was cached, GNUNET_NO if not
 */
static int cache_response(void *cls, struct DHT_MessageContext *msg_ctx)
{
  struct DHTQueryRecord *record;
  struct DHTRouteSource *source_info;
  struct DHTRouteSource *pos;
  struct GNUNET_TIME_Absolute now;
  unsigned int current_size;

  current_size = GNUNET_CONTAINER_multihashmap_size(forward_list.hashmap);
  while (current_size >= MAX_OUTSTANDING_FORWARDS)
    {
      source_info = GNUNET_CONTAINER_heap_remove_root(forward_list.minHeap);
      GNUNET_assert(source_info != NULL);
      record = source_info->record;
      GNUNET_CONTAINER_DLL_remove(record->head, record->tail, source_info);
      if (record->head == NULL) /* No more entries in DLL */
        {
          GNUNET_assert(GNUNET_YES == GNUNET_CONTAINER_multihashmap_remove(forward_list.hashmap, &record->key, record));
          GNUNET_free(record);
        }
      GNUNET_SCHEDULER_cancel(sched, source_info->delete_task);
      if (source_info->find_peers_responded != NULL)
        GNUNET_CONTAINER_bloomfilter_free(source_info->find_peers_responded);
      GNUNET_free(source_info);
      current_size = GNUNET_CONTAINER_multihashmap_size(forward_list.hashmap);
    }
  now = GNUNET_TIME_absolute_get();
  record = GNUNET_CONTAINER_multihashmap_get(forward_list.hashmap, &msg_ctx->key);
  if (record != NULL) /* Already know this request! */
    {
      pos = record->head;
      while (pos != NULL)
        {
          if (0 == memcmp(msg_ctx->peer, &pos->source, sizeof(struct GNUNET_PeerIdentity)))
            break; /* Already have this peer in reply list! */
          pos = pos->next;
        }
      if ((pos != NULL) && (pos->client == msg_ctx->client)) /* Seen this already */
        {
          GNUNET_CONTAINER_heap_update_cost(forward_list.minHeap, pos->hnode, now.value);
          return GNUNET_NO;
        }
    }
  else
    {
      record = GNUNET_malloc(sizeof (struct DHTQueryRecord));
      GNUNET_assert(GNUNET_OK == GNUNET_CONTAINER_multihashmap_put(forward_list.hashmap, &msg_ctx->key, record, GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY));
      memcpy(&record->key, &msg_ctx->key, sizeof(GNUNET_HashCode));
    }

  source_info = GNUNET_malloc(sizeof(struct DHTRouteSource));
  source_info->record = record;
  source_info->delete_task = GNUNET_SCHEDULER_add_delayed(sched, DHT_FORWARD_TIMEOUT, &remove_forward_entry, source_info);
  source_info->find_peers_responded = GNUNET_CONTAINER_bloomfilter_init (NULL, DHT_BLOOM_SIZE, DHT_BLOOM_K);
  memcpy(&source_info->source, msg_ctx->peer, sizeof(struct GNUNET_PeerIdentity));
  GNUNET_CONTAINER_DLL_insert_after(record->head, record->tail, record->tail, source_info);
  if (msg_ctx->client != NULL) /* For local request, set timeout so high it effectively never gets pushed out */
    {
      source_info->client = msg_ctx->client;
      now = GNUNET_TIME_absolute_get_forever();
    }
  source_info->hnode = GNUNET_CONTAINER_heap_insert(forward_list.minHeap, source_info, now.value);
#if DEBUG_DHT > 1
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "`%s:%s': Created new forward source info for %s uid %llu\n", my_short_id,
                  "DHT", GNUNET_h2s (msg_ctx->key), msg_ctx->unique_id);
#endif
  return GNUNET_YES;
}


/**
 * Main function that handles whether or not to route a message to other
 * peers.
 *
 * @param cls closure for dht service (NULL)
 * @param msg the message to be routed
 * @param message_context the context containing all pertinent information about the message
 *
 * @return the number of peers the message was routed to,
 *         GNUNET_SYSERR on failure
 */
static int route_message(void *cls,
                         const struct GNUNET_MessageHeader *msg,
                         struct DHT_MessageContext *message_context)
{
  int i;
  int global_closest;
  struct PeerInfo *selected;
#if DEBUG_DHT_ROUTING > 1
  struct PeerInfo *nearest;
#endif
  unsigned int forward_count;
  struct RecentRequest *recent_req;
  GNUNET_HashCode unique_hash;
  char *stat_forward_count;
#if DEBUG_DHT_ROUTING
  int ret;
#endif

  if (malicious_dropper == GNUNET_YES)
    {
#if DEBUG_DHT_ROUTING
      if ((debug_routes_extended) && (dhtlog_handle != NULL))
        {
          dhtlog_handle->insert_route (NULL, message_context->unique_id, DHTLOG_ROUTE,
                                       message_context->hop_count, GNUNET_SYSERR,
                                       &my_identity, &message_context->key, message_context->peer,
                                       NULL);
        }
#endif
      if (message_context->bloom != NULL)
        GNUNET_CONTAINER_bloomfilter_free(message_context->bloom);
      return 0;
    }

  increment_stats(STAT_ROUTES);
  /* Semantics of this call means we find whether we are the closest peer out of those already
   * routed to on this messages path.
   */
  global_closest = am_closest_peer(&message_context->key, NULL);
  message_context->closest = am_closest_peer(&message_context->key, message_context->bloom);
  forward_count = get_forward_count(message_context->hop_count, message_context->replication);
  GNUNET_asprintf(&stat_forward_count, "# forward counts of %d", forward_count);
  increment_stats(stat_forward_count);
  GNUNET_free(stat_forward_count);
  if (message_context->bloom == NULL)
    message_context->bloom = GNUNET_CONTAINER_bloomfilter_init (NULL, DHT_BLOOM_SIZE, DHT_BLOOM_K);

  if ((stop_on_closest == GNUNET_YES) && (global_closest == GNUNET_YES) && (ntohs(msg->type) == GNUNET_MESSAGE_TYPE_DHT_PUT))
    forward_count = 0;

#if DEBUG_DHT_ROUTING
  if (forward_count == 0)
    ret = GNUNET_SYSERR;
  else
    ret = GNUNET_NO;

  if ((debug_routes_extended) && (dhtlog_handle != NULL))
    {
      dhtlog_handle->insert_route (NULL, message_context->unique_id, DHTLOG_ROUTE,
                                   message_context->hop_count, ret,
                                   &my_identity, &message_context->key, message_context->peer,
                                   NULL);
    }
#endif

  switch (ntohs(msg->type))
    {
    case GNUNET_MESSAGE_TYPE_DHT_GET: /* Add to hashmap of requests seen, search for data (always) */
      cache_response (cls, message_context);
      if ((handle_dht_get (cls, msg, message_context) > 0) && (stop_on_found == GNUNET_YES))
        forward_count = 0;
      break;
    case GNUNET_MESSAGE_TYPE_DHT_PUT: /* Check if closest, if so insert data. FIXME: thresholding to reduce complexity?*/
      increment_stats(STAT_PUTS);
      message_context->closest = global_closest;
      handle_dht_put (cls, msg, message_context);
      break;
    case GNUNET_MESSAGE_TYPE_DHT_FIND_PEER: /* Check if closest and not started by us, check options, add to requests seen */
      increment_stats(STAT_FIND_PEER);
      if (((message_context->hop_count > 0) && (0 != memcmp(message_context->peer, &my_identity, sizeof(struct GNUNET_PeerIdentity)))) || (message_context->client != NULL))
      {
        cache_response (cls, message_context);
        if ((message_context->closest == GNUNET_YES) || (message_context->msg_options == GNUNET_DHT_RO_DEMULTIPLEX_EVERYWHERE))
          handle_dht_find_peer (cls, msg, message_context);
      }
#if DEBUG_DHT_ROUTING
      if (message_context->hop_count == 0) /* Locally initiated request */
        {
          if ((debug_routes) && (dhtlog_handle != NULL))
            {
              dhtlog_handle->insert_dhtkey(NULL, &message_context->key);
              dhtlog_handle->insert_query (NULL, message_context->unique_id, DHTLOG_FIND_PEER,
                                           message_context->hop_count, GNUNET_NO, &my_identity,
                                           &message_context->key);
            }
        }
#endif
      break;
    default:
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "`%s': Message type (%d) not handled\n", "DHT", ntohs(msg->type));
    }

  GNUNET_CONTAINER_bloomfilter_add (message_context->bloom, &my_identity.hashPubKey);
  hash_from_uid(message_context->unique_id, &unique_hash);
  if (GNUNET_YES == GNUNET_CONTAINER_multihashmap_contains(recent.hashmap, &unique_hash))
  {
      recent_req = GNUNET_CONTAINER_multihashmap_get(recent.hashmap, &unique_hash);
      GNUNET_assert(recent_req != NULL);
      if (0 != memcmp(&recent_req->key, &message_context->key, sizeof(GNUNET_HashCode)))
        increment_stats(STAT_DUPLICATE_UID);
      else
      {
        increment_stats(STAT_RECENT_SEEN);
        GNUNET_CONTAINER_bloomfilter_or2(message_context->bloom, recent_req->bloom, DHT_BLOOM_SIZE);
      }
    }
  else
    {
      recent_req = GNUNET_malloc(sizeof(struct RecentRequest));
      recent_req->uid = message_context->unique_id;
      memcpy(&recent_req->key, &message_context->key, sizeof(GNUNET_HashCode));
      recent_req->remove_task = GNUNET_SCHEDULER_add_delayed(sched, DEFAULT_RECENT_REMOVAL, &remove_recent, recent_req);
      recent_req->heap_node = GNUNET_CONTAINER_heap_insert(recent.minHeap, recent_req, GNUNET_TIME_absolute_get().value);
      recent_req->bloom = GNUNET_CONTAINER_bloomfilter_init (NULL, DHT_BLOOM_SIZE, DHT_BLOOM_K);
      GNUNET_CONTAINER_multihashmap_put(recent.hashmap, &unique_hash, recent_req, GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY);
    }

  if (GNUNET_CONTAINER_multihashmap_size(recent.hashmap) > DHT_MAX_RECENT)
    {
      recent_req = GNUNET_CONTAINER_heap_peek(recent.minHeap);
      GNUNET_assert(recent_req != NULL);
      GNUNET_SCHEDULER_cancel(sched, recent_req->remove_task);
      GNUNET_SCHEDULER_add_now(sched, &remove_recent, recent_req);
    }

  for (i = 0; i < forward_count; i++)
    {
      selected = select_peer(&message_context->key, message_context->bloom, message_context->hop_count);

      if (selected != NULL)
        {
          GNUNET_CONTAINER_bloomfilter_add(message_context->bloom, &selected->id.hashPubKey);
#if DEBUG_DHT_ROUTING > 1
          nearest = find_closest_peer(&message_context->key);
          nearest_buf = GNUNET_strdup(GNUNET_i2s(&nearest->id));
          GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                      "`%s:%s': Forwarding request key %s uid %llu to peer %s (closest %s, bits %d, distance %u)\n", my_short_id,
                      "DHT", GNUNET_h2s (message_context->key), message_context->unique_id, GNUNET_i2s(&selected->id), nearest_buf, matching_bits(&nearest->id.hashPubKey, message_context->key), distance(&nearest->id.hashPubKey, message_context->key));
          GNUNET_free(nearest_buf);
#endif
          if ((debug_routes_extended) && (dhtlog_handle != NULL))
            {
              dhtlog_handle->insert_route (NULL, message_context->unique_id, DHTLOG_ROUTE,
                                           message_context->hop_count, GNUNET_NO,
                                           &my_identity, &message_context->key, message_context->peer,
                                           &selected->id);
            }
          forward_message(cls, msg, selected, message_context);
        }
      else
        {
          increment_stats("# NULL returned from select_peer");
          GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                      "`%s:%s': No peers selected for forwarding.\n", my_short_id,
                      "DHT");

        }
    }
#if DEBUG_DHT_ROUTING > 1
  if (forward_count == 0)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "`%s:%s': NOT Forwarding request key %s uid %llu to any peers\n", my_short_id,
                  "DHT", GNUNET_h2s (message_context->key), message_context->unique_id);
    }
#endif

  if (message_context->bloom != NULL)
    {
      GNUNET_CONTAINER_bloomfilter_or2(recent_req->bloom, message_context->bloom, DHT_BLOOM_SIZE);
      GNUNET_CONTAINER_bloomfilter_free(message_context->bloom);
    }

  return forward_count;
}

/**
 * Find a client if it exists, add it otherwise.
 *
 * @param client the server handle to the client
 *
 * @return the client if found, a new client otherwise
 */
static struct ClientList *
find_active_client (struct GNUNET_SERVER_Client *client)
{
  struct ClientList *pos = client_list;
  struct ClientList *ret;

  while (pos != NULL)
    {
      if (pos->client_handle == client)
        return pos;
      pos = pos->next;
    }

  ret = GNUNET_malloc (sizeof (struct ClientList));
  ret->client_handle = client;
  ret->next = client_list;
  client_list = ret;
  return ret;
}

/**
 * Task to send a malicious put message across the network.
 *
 * @param cls closure for this task
 * @param tc the context under which the task is running
 */
static void
malicious_put_task (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  static struct GNUNET_DHT_PutMessage put_message;
  static struct DHT_MessageContext message_context;
  static GNUNET_HashCode key;
  uint32_t random_key;

  if (tc->reason == GNUNET_SCHEDULER_REASON_SHUTDOWN)
    return;

  put_message.header.size = htons(sizeof(struct GNUNET_DHT_PutMessage));
  put_message.header.type = htons(GNUNET_MESSAGE_TYPE_DHT_PUT);
  put_message.type = htons(DHT_MALICIOUS_MESSAGE_TYPE);
  put_message.expiration = GNUNET_TIME_absolute_hton(GNUNET_TIME_absolute_get_forever());
  memset(&message_context, 0, sizeof(struct DHT_MessageContext));
  message_context.client = NULL;
  random_key = GNUNET_CRYPTO_random_u32(GNUNET_CRYPTO_QUALITY_WEAK, (uint32_t)-1);
  GNUNET_CRYPTO_hash(&random_key, sizeof(uint32_t), &key);
  memcpy(&message_context.key, &key, sizeof(GNUNET_HashCode));
  message_context.unique_id = GNUNET_ntohll (GNUNET_CRYPTO_random_u64(GNUNET_CRYPTO_QUALITY_WEAK, (uint64_t)-1));
  message_context.replication = ntohl (DHT_DEFAULT_FIND_PEER_REPLICATION);
  message_context.msg_options = ntohl (0);
  message_context.network_size = estimate_diameter();
  message_context.peer = &my_identity;
  message_context.importance = DHT_DEFAULT_P2P_IMPORTANCE; /* Make result routing a higher priority */
  message_context.timeout = DHT_DEFAULT_P2P_TIMEOUT;
  if (dhtlog_handle != NULL)
    dhtlog_handle->insert_dhtkey(NULL, &key);
  increment_stats(STAT_PUT_START);
  GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "%s:%s Sending malicious PUT message with hash %s", my_short_id, "DHT", GNUNET_h2s(&key));
  route_message(NULL, &put_message.header, &message_context);
  GNUNET_SCHEDULER_add_delayed(sched, GNUNET_TIME_relative_multiply(GNUNET_TIME_UNIT_MILLISECONDS, malicious_put_frequency), &malicious_put_task, NULL);

}

/**
 * Task to send a malicious put message across the network.
 *
 * @param cls closure for this task
 * @param tc the context under which the task is running
 */
static void
malicious_get_task (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  static struct GNUNET_DHT_GetMessage get_message;
  struct DHT_MessageContext message_context;
  static GNUNET_HashCode key;
  uint32_t random_key;

  if (tc->reason == GNUNET_SCHEDULER_REASON_SHUTDOWN)
    return;

  get_message.header.size = htons(sizeof(struct GNUNET_DHT_GetMessage));
  get_message.header.type = htons(GNUNET_MESSAGE_TYPE_DHT_GET);
  get_message.type = htons(DHT_MALICIOUS_MESSAGE_TYPE);
  memset(&message_context, 0, sizeof(struct DHT_MessageContext));
  message_context.client = NULL;
  random_key = GNUNET_CRYPTO_random_u32(GNUNET_CRYPTO_QUALITY_WEAK, (uint32_t)-1);
  GNUNET_CRYPTO_hash(&random_key, sizeof(uint32_t), &key);
  memcpy(&message_context.key, &key, sizeof(GNUNET_HashCode));
  message_context.unique_id = GNUNET_ntohll (GNUNET_CRYPTO_random_u64(GNUNET_CRYPTO_QUALITY_WEAK, (uint64_t)-1));
  message_context.replication = ntohl (DHT_DEFAULT_FIND_PEER_REPLICATION);
  message_context.msg_options = ntohl (0);
  message_context.network_size = estimate_diameter();
  message_context.peer = &my_identity;
  message_context.importance = DHT_DEFAULT_P2P_IMPORTANCE; /* Make result routing a higher priority */
  message_context.timeout = DHT_DEFAULT_P2P_TIMEOUT;
  if (dhtlog_handle != NULL)
    dhtlog_handle->insert_dhtkey(NULL, &key);
  increment_stats(STAT_GET_START);
  GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "%s:%s Sending malicious GET message with hash %s", my_short_id, "DHT", GNUNET_h2s(&key));
  route_message (NULL, &get_message.header, &message_context);
  GNUNET_SCHEDULER_add_delayed(sched, GNUNET_TIME_relative_multiply(GNUNET_TIME_UNIT_MILLISECONDS, malicious_get_frequency), &malicious_get_task, NULL);
}

/**
 * Iterator over hash map entries.
 *
 * @param cls closure
 * @param key current key code
 * @param value value in the hash map
 * @return GNUNET_YES if we should continue to
 *         iterate,
 *         GNUNET_NO if not.
 */
static int
add_known_to_bloom (void *cls,
                    const GNUNET_HashCode * key,
                    void *value)
{
  struct GNUNET_CONTAINER_BloomFilter *bloom = cls;
  GNUNET_CONTAINER_bloomfilter_add (bloom, key);
  return GNUNET_YES;
}

/**
 * Task to send a find peer message for our own peer identifier
 * so that we can find the closest peers in the network to ourselves
 * and attempt to connect to them.
 *
 * @param cls closure for this task
 * @param tc the context under which the task is running
 */
static void
send_find_peer_message (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct GNUNET_DHT_FindPeerMessage *find_peer_msg;
  struct DHT_MessageContext message_context;
  int ret;
  struct GNUNET_TIME_Relative next_send_time;
  struct GNUNET_CONTAINER_BloomFilter *temp_bloom;
#if COUNT_INTERVAL
  struct GNUNET_TIME_Relative time_diff;
  struct GNUNET_TIME_Absolute end;
  double multiplier;
  double count_per_interval;
#endif
  if (tc->reason == GNUNET_SCHEDULER_REASON_SHUTDOWN)
    return;

  if ((newly_found_peers > bucket_size) && (GNUNET_YES == do_find_peer)) /* If we are finding peers already, no need to send out our request right now! */
    {
      GNUNET_log(GNUNET_ERROR_TYPE_WARNING, "Have %d newly found peers since last find peer message sent!\n", newly_found_peers);
      GNUNET_SCHEDULER_add_delayed (sched,
                                    GNUNET_TIME_UNIT_MINUTES,
                                    &send_find_peer_message, NULL);
      newly_found_peers = 0;
      return;
    }
    
  increment_stats(STAT_FIND_PEER_START);
#if COUNT_INTERVAL
  end = GNUNET_TIME_absolute_get();
  time_diff = GNUNET_TIME_absolute_get_difference(find_peer_context.start, end);

  if (time_diff.value > FIND_PEER_CALC_INTERVAL.value)
    {
      multiplier = time_diff.value / FIND_PEER_CALC_INTERVAL.value;
      count_per_interval = find_peer_context.count / multiplier;
    }
  else
    {
      multiplier = FIND_PEER_CALC_INTERVAL.value / time_diff.value;
      count_per_interval = find_peer_context.count * multiplier;
    }
#endif

  find_peer_msg = GNUNET_malloc(sizeof(struct GNUNET_DHT_FindPeerMessage));
  find_peer_msg->header.size = htons(sizeof(struct GNUNET_DHT_FindPeerMessage));
  find_peer_msg->header.type = htons(GNUNET_MESSAGE_TYPE_DHT_FIND_PEER);
  temp_bloom = GNUNET_CONTAINER_bloomfilter_init (NULL, DHT_BLOOM_SIZE, DHT_BLOOM_K);
  GNUNET_CONTAINER_multihashmap_iterate(all_known_peers, &add_known_to_bloom, temp_bloom);
  GNUNET_assert(GNUNET_OK == GNUNET_CONTAINER_bloomfilter_get_raw_data(temp_bloom, find_peer_msg->bloomfilter, DHT_BLOOM_SIZE));
  memset(&message_context, 0, sizeof(struct DHT_MessageContext));
  memcpy(&message_context.key, &my_identity.hashPubKey, sizeof(GNUNET_HashCode));
  message_context.unique_id = GNUNET_ntohll (GNUNET_CRYPTO_random_u64(GNUNET_CRYPTO_QUALITY_STRONG, (uint64_t)-1));
  message_context.replication = DHT_DEFAULT_FIND_PEER_REPLICATION;
  message_context.msg_options = DHT_DEFAULT_FIND_PEER_OPTIONS;
  message_context.network_size = estimate_diameter();
  message_context.peer = &my_identity;
  message_context.importance = DHT_DEFAULT_FIND_PEER_IMPORTANCE;
  message_context.timeout = DHT_DEFAULT_FIND_PEER_TIMEOUT;

  ret = route_message(NULL, &find_peer_msg->header, &message_context);
  GNUNET_free(find_peer_msg);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s:%s': Sent `%s' request to %d peers\n", my_short_id, "DHT",
              "FIND PEER", ret);
  if (newly_found_peers < bucket_size)
    {
      next_send_time.value = (DHT_MAXIMUM_FIND_PEER_INTERVAL.value / 2) +
                              GNUNET_CRYPTO_random_u64(GNUNET_CRYPTO_QUALITY_STRONG,
                                                       DHT_MAXIMUM_FIND_PEER_INTERVAL.value / 2);
    }
  else
    {
      next_send_time.value = DHT_MINIMUM_FIND_PEER_INTERVAL.value +
                             GNUNET_CRYPTO_random_u64(GNUNET_CRYPTO_QUALITY_STRONG,
                                                      DHT_MAXIMUM_FIND_PEER_INTERVAL.value - DHT_MINIMUM_FIND_PEER_INTERVAL.value);
    }

  GNUNET_assert (next_send_time.value != 0);
  find_peer_context.count = 0;
  newly_found_peers = 0;
  find_peer_context.start = GNUNET_TIME_absolute_get();
  if (GNUNET_YES == do_find_peer)
  {
    GNUNET_SCHEDULER_add_delayed (sched,
                                  next_send_time,
       	                          &send_find_peer_message, NULL);
  }
}

/**
 * Handler for any generic DHT messages, calls the appropriate handler
 * depending on message type, sends confirmation if responses aren't otherwise
 * expected.
 *
 * @param cls closure for the service
 * @param client the client we received this message from
 * @param message the actual message received
 */
static void
handle_dht_local_route_request (void *cls, struct GNUNET_SERVER_Client *client,
                                const struct GNUNET_MessageHeader *message)
{
  const struct GNUNET_DHT_RouteMessage *dht_msg = (const struct GNUNET_DHT_RouteMessage *) message;
  const struct GNUNET_MessageHeader *enc_msg;
  struct DHT_MessageContext message_context;
  enc_msg = (const struct GNUNET_MessageHeader *) &dht_msg[1];
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s:%s': Received `%s' request from client, message type %d, key %s, uid %llu\n",
              my_short_id, "DHT", "GENERIC", enc_type, GNUNET_h2s (&dht_msg->key),
              GNUNET_ntohll (dht_msg->unique_id));
#endif
#if DEBUG_DHT_ROUTING
  if (dhtlog_handle != NULL)
    dhtlog_handle->insert_dhtkey (NULL, &dht_msg->key);
#endif
  memset(&message_context, 0, sizeof(struct DHT_MessageContext));
  message_context.client = find_active_client (client);
  memcpy(&message_context.key, &dht_msg->key, sizeof(GNUNET_HashCode));
  message_context.unique_id = GNUNET_ntohll (dht_msg->unique_id);
  message_context.replication = ntohl (dht_msg->desired_replication_level);
  message_context.msg_options = ntohl (dht_msg->options);
  message_context.network_size = estimate_diameter();
  message_context.peer = &my_identity;
  message_context.importance = DHT_DEFAULT_P2P_IMPORTANCE * 4; /* Make local routing a higher priority */
  message_context.timeout = DHT_DEFAULT_P2P_TIMEOUT;
  if (ntohs(enc_msg->type) == GNUNET_MESSAGE_TYPE_DHT_GET)
    increment_stats(STAT_GET_START);
  else if (ntohs(enc_msg->type) == GNUNET_MESSAGE_TYPE_DHT_PUT)
    increment_stats(STAT_PUT_START);
  else if (ntohs(enc_msg->type) == GNUNET_MESSAGE_TYPE_DHT_FIND_PEER)
    increment_stats(STAT_FIND_PEER_START);

  route_message(cls, enc_msg, &message_context);

  GNUNET_SERVER_receive_done (client, GNUNET_OK);

}

/**
 * Handler for any locally received DHT control messages,
 * sets malicious flags mostly for now.
 *
 * @param cls closure for the service
 * @param client the client we received this message from
 * @param message the actual message received
 *
 */
static void
handle_dht_control_message (void *cls, struct GNUNET_SERVER_Client *client,
                            const struct GNUNET_MessageHeader *message)
{
  const struct GNUNET_DHT_ControlMessage *dht_control_msg =
      (const struct GNUNET_DHT_ControlMessage *) message;
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s:%s': Received `%s' request from client, command %d\n", my_short_id, "DHT",
              "CONTROL", ntohs(dht_control_msg->command));
#endif

  switch (ntohs(dht_control_msg->command))
  {
  case GNUNET_MESSAGE_TYPE_DHT_FIND_PEER:
    GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "Sending self seeking find peer request!\n");
    GNUNET_SCHEDULER_add_now(sched, &send_find_peer_message, NULL);
    break;
  case GNUNET_MESSAGE_TYPE_DHT_MALICIOUS_GET:
    if (ntohs(dht_control_msg->variable) > 0)
      malicious_get_frequency = ntohs(dht_control_msg->variable);
    if (malicious_get_frequency == 0)
      malicious_get_frequency = DEFAULT_MALICIOUS_GET_FREQUENCY;
    if (malicious_getter != GNUNET_YES)
      GNUNET_SCHEDULER_add_now(sched, &malicious_get_task, NULL);
    malicious_getter = GNUNET_YES;
    GNUNET_log(GNUNET_ERROR_TYPE_WARNING, "%s:%s Initiating malicious GET behavior, frequency %d\n", my_short_id, "DHT", malicious_get_frequency);
    break;
  case GNUNET_MESSAGE_TYPE_DHT_MALICIOUS_PUT:
    if (ntohs(dht_control_msg->variable) > 0)
      malicious_put_frequency = ntohs(dht_control_msg->variable);
    if (malicious_put_frequency == 0)
      malicious_put_frequency = DEFAULT_MALICIOUS_PUT_FREQUENCY;
    if (malicious_putter != GNUNET_YES)
      GNUNET_SCHEDULER_add_now(sched, &malicious_put_task, NULL);
    malicious_putter = GNUNET_YES;
    GNUNET_log(GNUNET_ERROR_TYPE_WARNING, "%s:%s Initiating malicious PUT behavior, frequency %d\n", my_short_id, "DHT", malicious_put_frequency);
    break;
  case GNUNET_MESSAGE_TYPE_DHT_MALICIOUS_DROP:
    if ((malicious_dropper != GNUNET_YES) && (dhtlog_handle != NULL))
      dhtlog_handle->set_malicious(&my_identity);
    malicious_dropper = GNUNET_YES;
    GNUNET_log(GNUNET_ERROR_TYPE_WARNING, "%s:%s Initiating malicious DROP behavior\n", my_short_id, "DHT");
    break;
  default:
    GNUNET_log(GNUNET_ERROR_TYPE_WARNING, "%s:%s Unknown control command type `%d'!\n", ntohs(dht_control_msg->command));
  }

  GNUNET_SERVER_receive_done (client, GNUNET_OK);
}

/**
 * Handler for any generic DHT stop messages, calls the appropriate handler
 * depending on message type (if processed locally)
 *
 * @param cls closure for the service
 * @param client the client we received this message from
 * @param message the actual message received
 *
 */
static void
handle_dht_local_route_stop(void *cls, struct GNUNET_SERVER_Client *client,
                            const struct GNUNET_MessageHeader *message)
{

  const struct GNUNET_DHT_StopMessage *dht_stop_msg =
    (const struct GNUNET_DHT_StopMessage *) message;
  struct DHTQueryRecord *record;
  struct DHTRouteSource *pos;
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s:%s': Received `%s' request from client, uid %llu\n", my_short_id, "DHT",
              "GENERIC STOP", GNUNET_ntohll (dht_stop_msg->unique_id));
#endif
  record = GNUNET_CONTAINER_multihashmap_get(forward_list.hashmap, &dht_stop_msg->key);
  if (record != NULL)
    {
      pos = record->head;

      while (pos != NULL)
        {
          if ((pos->client != NULL) && (pos->client->client_handle == client))
            {
              GNUNET_SCHEDULER_cancel(sched, pos->delete_task);
              GNUNET_SCHEDULER_add_now(sched, &remove_forward_entry, pos);
            }
          pos = pos->next;
        }
    }

  GNUNET_SERVER_receive_done (client, GNUNET_OK);
}


/**
 * Core handler for p2p route requests.
 */
static int
handle_dht_p2p_route_request (void *cls,
			      const struct GNUNET_PeerIdentity *peer,
			      const struct GNUNET_MessageHeader *message,
			      struct GNUNET_TIME_Relative latency, uint32_t distance)
{
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s:%s': Received P2P request from peer %s\n", my_short_id, "DHT", GNUNET_i2s(peer));
#endif
  struct GNUNET_DHT_P2PRouteMessage *incoming = (struct GNUNET_DHT_P2PRouteMessage *)message;
  struct GNUNET_MessageHeader *enc_msg = (struct GNUNET_MessageHeader *)&incoming[1];
  struct DHT_MessageContext *message_context;

  if (get_max_send_delay().value > MAX_REQUEST_TIME.value)
  {
    fprintf(stderr, "Sending of previous replies took far too long, backing off!\n");
    decrease_max_send_delay(get_max_send_delay());
    return GNUNET_YES;
  }

  if (ntohs(enc_msg->type) == GNUNET_MESSAGE_TYPE_DHT_P2P_PING) /* Throw these away. FIXME: Don't throw these away? (reply)*/
    {
#if DEBUG_PING
      GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "%s:%s Received P2P Ping message.\n", my_short_id, "DHT");
#endif
      return GNUNET_YES;
    }

  if (ntohs(enc_msg->size) > GNUNET_SERVER_MAX_MESSAGE_SIZE)
    {
      GNUNET_break_op(0);
      return GNUNET_YES;
    }
  message_context = GNUNET_malloc(sizeof (struct DHT_MessageContext));
  message_context->bloom = GNUNET_CONTAINER_bloomfilter_init(incoming->bloomfilter, DHT_BLOOM_SIZE, DHT_BLOOM_K);
  GNUNET_assert(message_context->bloom != NULL);
  message_context->hop_count = ntohl(incoming->hop_count);
  memcpy(&message_context->key, &incoming->key, sizeof(GNUNET_HashCode));
  message_context->replication = ntohl(incoming->desired_replication_level);
  message_context->unique_id = GNUNET_ntohll(incoming->unique_id);
  message_context->msg_options = ntohl(incoming->options);
  message_context->network_size = ntohl(incoming->network_size);
  message_context->peer = peer;
  message_context->importance = DHT_DEFAULT_P2P_IMPORTANCE;
  message_context->timeout = DHT_DEFAULT_P2P_TIMEOUT;
  route_message(cls, enc_msg, message_context);
  GNUNET_free(message_context);
  return GNUNET_YES;
}


/**
 * Core handler for p2p route results.
 */
static int
handle_dht_p2p_route_result (void *cls,
			     const struct GNUNET_PeerIdentity *peer,
			     const struct GNUNET_MessageHeader *message,
			     struct GNUNET_TIME_Relative latency, uint32_t distance)
{
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "`%s:%s': Received request from peer %s\n", my_short_id, "DHT", GNUNET_i2s(peer));
#endif
  struct GNUNET_DHT_P2PRouteResultMessage *incoming = (struct GNUNET_DHT_P2PRouteResultMessage *)message;
  struct GNUNET_MessageHeader *enc_msg = (struct GNUNET_MessageHeader *)&incoming[1];
  struct DHT_MessageContext message_context;

  if (ntohs(enc_msg->size) > GNUNET_SERVER_MAX_MESSAGE_SIZE)
    {
      GNUNET_break_op(0);
      return GNUNET_YES;
    }

  memset(&message_context, 0, sizeof(struct DHT_MessageContext));
  message_context.bloom = GNUNET_CONTAINER_bloomfilter_init(incoming->bloomfilter, DHT_BLOOM_SIZE, DHT_BLOOM_K);
  GNUNET_assert(message_context.bloom != NULL);
  memcpy(&message_context.key, &incoming->key, sizeof(GNUNET_HashCode));
  message_context.unique_id = GNUNET_ntohll(incoming->unique_id);
  message_context.msg_options = ntohl(incoming->options);
  message_context.hop_count = ntohl(incoming->hop_count);
  message_context.peer = peer;
  message_context.importance = DHT_DEFAULT_P2P_IMPORTANCE * 2; /* Make result routing a higher priority */
  message_context.timeout = DHT_DEFAULT_P2P_TIMEOUT;
  route_result_message(cls, enc_msg, &message_context);
  return GNUNET_YES;
}


/**
 * Receive the HELLO from transport service,
 * free current and replace if necessary.
 *
 * @param cls NULL
 * @param message HELLO message of peer
 */
static void
process_hello (void *cls, const struct GNUNET_MessageHeader *message)
{
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Received our `%s' from transport service\n",
              "HELLO");
#endif

  GNUNET_assert (message != NULL);
  GNUNET_free_non_null(my_hello);
  my_hello = GNUNET_malloc(ntohs(message->size));
  memcpy(my_hello, message, ntohs(message->size));
}


/**
 * Task run during shutdown.
 *
 * @param cls unused
 * @param tc unused
 */
static void
shutdown_task (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  int bucket_count;
  struct PeerInfo *pos;
  if (transport_handle != NULL)
  {
    GNUNET_free_non_null(my_hello);
    GNUNET_TRANSPORT_get_hello_cancel(transport_handle, &process_hello, NULL);
    GNUNET_TRANSPORT_disconnect(transport_handle);
  }

  for (bucket_count = lowest_bucket; bucket_count < MAX_BUCKETS; bucket_count++)
    {
      while (k_buckets[bucket_count].head != NULL)
        {
          pos = k_buckets[bucket_count].head;
#if DEBUG_DHT
          GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                      "%s:%s Removing peer %s from bucket %d!\n", my_short_id, "DHT", GNUNET_i2s(&pos->id), bucket_count);
#endif
          delete_peer(pos, bucket_count);
        }
    }
  if (coreAPI != NULL)
    {
#if DEBUG_DHT
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "%s:%s Disconnecting core!\n", my_short_id, "DHT");
#endif
      GNUNET_CORE_disconnect (coreAPI);
    }
  if (datacache != NULL)
    {
#if DEBUG_DHT
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "%s:%s Destroying datacache!\n", my_short_id, "DHT");
#endif
      GNUNET_DATACACHE_destroy (datacache);
    }

  if (stats != NULL)
    {
      GNUNET_STATISTICS_destroy (stats, GNUNET_YES);
    }

  if (dhtlog_handle != NULL)
    GNUNET_DHTLOG_disconnect(dhtlog_handle);

  GNUNET_free_non_null(my_short_id);
}


/**
 * To be called on core init/fail.
 *
 * @param cls service closure
 * @param server handle to the server for this service
 * @param identity the public identity of this peer
 * @param publicKey the public key of this peer
 */
void
core_init (void *cls,
           struct GNUNET_CORE_Handle *server,
           const struct GNUNET_PeerIdentity *identity,
           const struct GNUNET_CRYPTO_RsaPublicKeyBinaryEncoded *publicKey)
{

  if (server == NULL)
    {
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "%s: Connection to core FAILED!\n", "dht",
              GNUNET_i2s (identity));
#endif
      GNUNET_SCHEDULER_cancel (sched, cleanup_task);
      GNUNET_SCHEDULER_add_now (sched, &shutdown_task, NULL);
      return;
    }
#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "%s: Core connection initialized, I am peer: %s\n", "dht",
              GNUNET_i2s (identity));
#endif

  /* Copy our identity so we can use it */
  memcpy (&my_identity, identity, sizeof (struct GNUNET_PeerIdentity));
  if (my_short_id != NULL)
    GNUNET_log(GNUNET_ERROR_TYPE_WARNING, "%s Receive CORE INIT message but have already been initialized! Did CORE fail?\n", "DHT SERVICE");
  my_short_id = GNUNET_strdup(GNUNET_i2s(&my_identity));
  /* Set the server to local variable */
  coreAPI = server;

  if (dhtlog_handle != NULL)
    dhtlog_handle->insert_node (NULL, &my_identity);
}


static struct GNUNET_SERVER_MessageHandler plugin_handlers[] = {
  {&handle_dht_local_route_request, NULL, GNUNET_MESSAGE_TYPE_DHT_LOCAL_ROUTE, 0},
  {&handle_dht_local_route_stop, NULL, GNUNET_MESSAGE_TYPE_DHT_LOCAL_ROUTE_STOP, 0},
  {&handle_dht_control_message, NULL, GNUNET_MESSAGE_TYPE_DHT_CONTROL, 0},
  {NULL, NULL, 0, 0}
};


static struct GNUNET_CORE_MessageHandler core_handlers[] = {
  {&handle_dht_p2p_route_request, GNUNET_MESSAGE_TYPE_DHT_P2P_ROUTE, 0},
  {&handle_dht_p2p_route_result, GNUNET_MESSAGE_TYPE_DHT_P2P_ROUTE_RESULT, 0},
  {NULL, 0, 0}
};

/**
 * Method called whenever a peer connects.
 *
 * @param cls closure
 * @param peer peer identity this notification is about
 * @param latency reported latency of the connection with peer
 * @param distance reported distance (DV) to peer
 */
void handle_core_connect (void *cls,
                          const struct GNUNET_PeerIdentity * peer,
                          struct GNUNET_TIME_Relative latency,
                          uint32_t distance)
{
  struct PeerInfo *ret;

#if DEBUG_DHT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "%s:%s Receives core connect message for peer %s distance %d!\n", my_short_id, "dht", GNUNET_i2s(peer), distance);
#endif

  if (GNUNET_YES == GNUNET_CONTAINER_multihashmap_contains(all_known_peers, &peer->hashPubKey))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "%s:%s Received %s message for peer %s, but already have peer in RT!", my_short_id, "DHT", "CORE CONNECT", GNUNET_i2s(peer));
      return;
    }

  if (datacache != NULL)
    GNUNET_DATACACHE_put(datacache, &peer->hashPubKey, sizeof(struct GNUNET_PeerIdentity), (const char *)peer, 0, GNUNET_TIME_absolute_get_forever());
  ret = try_add_peer(peer,
                     find_current_bucket(&peer->hashPubKey),
                     latency,
                     distance);
  if (ret != NULL)
    {
      newly_found_peers++;
      GNUNET_CONTAINER_multihashmap_put(all_known_peers, &peer->hashPubKey, ret, GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY);
    }
#if DEBUG_DHT
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "%s:%s Adding peer to routing list: %s\n", my_short_id, "DHT", ret == NULL ? "NOT ADDED" : "PEER ADDED");
#endif
}

/**
 * Method called whenever a peer disconnects.
 *
 * @param cls closure
 * @param peer peer identity this notification is about
 */
void handle_core_disconnect (void *cls,
                             const struct
                             GNUNET_PeerIdentity * peer)
{
  struct PeerInfo *to_remove;
  int current_bucket;

  GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "%s:%s: Received peer disconnect message for peer `%s' from %s\n", my_short_id, "DHT", GNUNET_i2s(peer), "CORE");

  if (GNUNET_YES != GNUNET_CONTAINER_multihashmap_contains(all_known_peers, &peer->hashPubKey))
    {
      GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "%s:%s: do not have peer `%s' in RT, can't disconnect!\n", my_short_id, "DHT", GNUNET_i2s(peer));
      return;
    }
  increment_stats(STAT_DISCONNECTS);
  GNUNET_assert(GNUNET_CONTAINER_multihashmap_contains(all_known_peers, &peer->hashPubKey));
  to_remove = GNUNET_CONTAINER_multihashmap_get(all_known_peers, &peer->hashPubKey);
  GNUNET_assert(0 == memcmp(peer, &to_remove->id, sizeof(struct GNUNET_PeerIdentity)));
  current_bucket = find_current_bucket(&to_remove->id.hashPubKey);
  delete_peer(to_remove, current_bucket);
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
#if DO_FIND_PEER
  struct GNUNET_TIME_Relative next_send_time;
#endif
  sched = scheduler;
  cfg = c;
  datacache = GNUNET_DATACACHE_create (sched, cfg, "dhtcache");
  GNUNET_SERVER_add_handlers (server, plugin_handlers);
  coreAPI = GNUNET_CORE_connect (sched, /* Main scheduler */
                                 cfg,   /* Main configuration */
                                 GNUNET_TIME_UNIT_FOREVER_REL,
                                 NULL,  /* Closure passed to DHT functions */
                                 &core_init,    /* Call core_init once connected */
                                 &handle_core_connect,  /* Handle connects */
                                 &handle_core_disconnect,  /* remove peers on disconnects */
                                 NULL,  /* Do we care about "status" updates? */
                                 NULL,  /* Don't want notified about all incoming messages */
                                 GNUNET_NO,     /* For header only inbound notification */
                                 NULL,  /* Don't want notified about all outbound messages */
                                 GNUNET_NO,     /* For header only outbound notification */
                                 core_handlers);        /* Register these handlers */

  if (coreAPI == NULL)
    return;
  transport_handle = GNUNET_TRANSPORT_connect(sched, cfg, 
					      NULL, NULL, NULL, NULL, NULL);
  if (transport_handle != NULL)
    GNUNET_TRANSPORT_get_hello (transport_handle, &process_hello, NULL);
  else
    GNUNET_log(GNUNET_ERROR_TYPE_WARNING, "Failed to connect to transport service!\n");

  lowest_bucket = MAX_BUCKETS - 1;
  forward_list.hashmap = GNUNET_CONTAINER_multihashmap_create(MAX_OUTSTANDING_FORWARDS / 10);
  forward_list.minHeap = GNUNET_CONTAINER_heap_create(GNUNET_CONTAINER_HEAP_ORDER_MIN);
  all_known_peers = GNUNET_CONTAINER_multihashmap_create(MAX_BUCKETS / 8);
  recent_find_peer_requests = GNUNET_CONTAINER_multihashmap_create(MAX_BUCKETS / 8);
  GNUNET_assert(all_known_peers != NULL);
  if (GNUNET_YES == GNUNET_CONFIGURATION_get_value_yesno(cfg, "dht_testing", "mysql_logging"))
    {
      debug_routes = GNUNET_YES;
    }

  if (GNUNET_YES ==
      GNUNET_CONFIGURATION_get_value_yesno(cfg, "dht",
                                           "strict_kademlia"))
    {
      strict_kademlia = GNUNET_YES;
    }

  if (GNUNET_YES ==
      GNUNET_CONFIGURATION_get_value_yesno(cfg, "dht",
                                           "stop_on_closest"))
    {
      stop_on_closest = GNUNET_YES;
    }

  if (GNUNET_YES ==
      GNUNET_CONFIGURATION_get_value_yesno(cfg, "dht",
                                           "stop_found"))
    {
      stop_on_found = GNUNET_YES;
    }

  if (GNUNET_YES ==
      GNUNET_CONFIGURATION_get_value_yesno(cfg, "dht",
                                           "malicious_getter"))
    {
      malicious_getter = GNUNET_YES;
      if (GNUNET_NO == GNUNET_CONFIGURATION_get_value_number (cfg, "DHT",
                                            "MALICIOUS_GET_FREQUENCY",
                                            &malicious_get_frequency))
        malicious_get_frequency = DEFAULT_MALICIOUS_GET_FREQUENCY;
    }

  if (GNUNET_YES ==
      GNUNET_CONFIGURATION_get_value_yesno(cfg, "dht",
                                           "malicious_putter"))
    {
      malicious_putter = GNUNET_YES;
      if (GNUNET_NO == GNUNET_CONFIGURATION_get_value_number (cfg, "DHT",
                                            "MALICIOUS_PUT_FREQUENCY",
                                            &malicious_put_frequency))
        malicious_put_frequency = DEFAULT_MALICIOUS_PUT_FREQUENCY;
    }

  if (GNUNET_YES ==
          GNUNET_CONFIGURATION_get_value_yesno(cfg, "dht",
                                               "malicious_dropper"))
    {
      malicious_dropper = GNUNET_YES;
    }

  if (GNUNET_NO ==
        GNUNET_CONFIGURATION_get_value_yesno(cfg, "dht",
                                             "do_find_peer"))
    {
      do_find_peer = GNUNET_NO;
    }
  else
    do_find_peer = GNUNET_YES;

  if (GNUNET_YES ==
      GNUNET_CONFIGURATION_get_value_yesno(cfg, "dht_testing",
                                           "mysql_logging_extended"))
    {
      debug_routes = GNUNET_YES;
      debug_routes_extended = GNUNET_YES;
    }

  if (GNUNET_YES == debug_routes)
    {
      dhtlog_handle = GNUNET_DHTLOG_connect(cfg);
      if (dhtlog_handle == NULL)
        {
          GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                      "Could not connect to mysql logging server, logging will not happen!");
        }
    }

  converge_option = DHT_CONVERGE_SQUARE;
  if (GNUNET_YES ==
      GNUNET_CONFIGURATION_get_value_yesno(cfg, "dht_testing",
                                           "converge_linear"))
    {
      converge_option = DHT_CONVERGE_LINEAR;
    }

  stats = GNUNET_STATISTICS_create(sched, "dht", cfg);

  if (stats != NULL)
    {
      GNUNET_STATISTICS_set(stats, STAT_ROUTES, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_ROUTE_FORWARDS, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_ROUTE_FORWARDS_CLOSEST, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_RESULTS, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_RESULTS_TO_CLIENT, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_RESULT_FORWARDS, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_GETS, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_PUTS, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_PUTS_INSERTED, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_FIND_PEER, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_FIND_PEER_START, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_GET_START, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_PUT_START, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_FIND_PEER_REPLY, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_FIND_PEER_ANSWER, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_BLOOM_FIND_PEER, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_GET_REPLY, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_GET_RESPONSE_START, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_HELLOS_PROVIDED, 0, GNUNET_NO);
      GNUNET_STATISTICS_set(stats, STAT_DISCONNECTS, 0, GNUNET_NO);
    }
  /* FIXME: if there are no recent requests then these never get freed, but alternative is _annoying_! */
  recent.hashmap = GNUNET_CONTAINER_multihashmap_create(DHT_MAX_RECENT / 2);
  recent.minHeap = GNUNET_CONTAINER_heap_create(GNUNET_CONTAINER_HEAP_ORDER_MIN);
  if (GNUNET_YES == do_find_peer)
  {
    next_send_time.value = DHT_MINIMUM_FIND_PEER_INTERVAL.value +
                           GNUNET_CRYPTO_random_u64(GNUNET_CRYPTO_QUALITY_STRONG,
                                                    (DHT_MAXIMUM_FIND_PEER_INTERVAL.value / 2) - DHT_MINIMUM_FIND_PEER_INTERVAL.value);
    find_peer_context.start = GNUNET_TIME_absolute_get();
    GNUNET_SCHEDULER_add_delayed (sched,
                                  next_send_time,
                                  &send_find_peer_message, &find_peer_context);
  }

  /* Scheduled the task to clean up when shutdown is called */
  cleanup_task = GNUNET_SCHEDULER_add_delayed (sched,
                                               GNUNET_TIME_UNIT_FOREVER_REL,
                                               &shutdown_task, NULL);
}

/**
 * The main function for the dht service.
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
