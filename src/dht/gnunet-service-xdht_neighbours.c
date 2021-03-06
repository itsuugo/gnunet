/*
     This file is part of GNUnet.
     Copyright (C) 2009-2014, 2016 GNUnet e.V.

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
 * @file dht/gnunet-service-xdht_neighbours.c
 * @brief GNUnet DHT service's finger and friend table management code
 * @author Supriti Singh
 * @author Christian Grothoff
 */

#include "platform.h"
#include "gnunet_util_lib.h"
#include "gnunet_block_lib.h"
#include "gnunet_hello_lib.h"
#include "gnunet_constants.h"
#include "gnunet_protocols.h"
#include "gnunet_ats_service.h"
#include "gnunet_core_service.h"
#include "gnunet_datacache_lib.h"
#include "gnunet_transport_service.h"
#include "gnunet_dht_service.h"
#include "gnunet_statistics_service.h"
#include "gnunet-service-dht.h"
#include "gnunet-service-dht_datacache.h"
#include "gnunet-service-dht_neighbours.h"
#include "gnunet-service-xdht_routing.h"
#include "dht.h"

/**
 * TODO:
 * 1. In X-Vine paper, there is no policy defined for replicating the data to
 * recover in case of peer failure. We can do it in Chord way. In R5N, the key
 * is hashed and then data is stored according to the key value generated after
 * hashing.
 */

#define DEBUG(...)                                           \
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, __VA_ARGS__)

/**
 * Maximum possible fingers (including predecessor) of a peer
 */
#define MAX_FINGERS 65

/**
 * Maximum allowed number of pending messages per friend peer.
 */
#define MAXIMUM_PENDING_PER_FRIEND 64

/**
 * How long to wait before sending another find finger trail request
 */
#define DHT_FIND_FINGER_TRAIL_INTERVAL GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 2)

/**
 * How long to wait before sending another verify successor message.
 */
#define DHT_SEND_VERIFY_SUCCESSOR_INTERVAL GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 2)

/**
 * How long to wait before sending another verify successor message.
 */
#define DHT_SEND_VERIFY_SUCCESSOR_RETRY_INTERVAL GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 30)

/**
 * How long to wait before retrying notify successor.
 */
#define DHT_SEND_NOTIFY_SUCCESSOR_RETRY_INTERVAL GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 30)

/**
 * How long at most to wait for transmission of a request to a friend ?
 */
#define PENDING_MESSAGE_TIMEOUT GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_MINUTES, 2)

/**
 * Duration for which I may remain congested.
 * Note: Its a static value. In future, a peer may do some analysis and calculate
 * congestion_timeout based on 'some' parameters.
 */
#define CONGESTION_TIMEOUT GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_MINUTES, 2)

/**
 * In case we don't hear back from the current successor, then we can start
 * verify successor.
 */
#define WAIT_NOTIFY_CONFIRMATION GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_MILLISECONDS, 200)

/**
 * Maximum number of trails allowed to go through a friend.
 */
#define TRAILS_THROUGH_FRIEND_THRESHOLD 64

/**
 * Maximum number of trails stored per finger.
 */
#define MAXIMUM_TRAILS_PER_FINGER 4

/**
 * Finger map index for predecessor entry in finger table.
 */
#define PREDECESSOR_FINGER_ID 64

/**
 * FIXME: Its use only at 3 places check if you can remove it.
 * To check if a finger is predecessor or not.
 */
enum GDS_NEIGHBOURS_finger_type
{
  GDS_FINGER_TYPE_PREDECESSOR = 1,
  GDS_FINGER_TYPE_NON_PREDECESSOR = 0
};

GNUNET_NETWORK_STRUCT_BEGIN

/**
 * P2P PUT message
 */
struct PeerPutMessage
{
  /**
   * Type: #GNUNET_MESSAGE_TYPE_XDHT_P2P_PUT
   */
  struct GNUNET_MessageHeader header;

  /**
   * Processing options
   */
  uint32_t options GNUNET_PACKED;

  /**
   * Content type.
   */
  uint32_t block_type GNUNET_PACKED;

  /**
   * Hop count
   */
  uint32_t hop_count GNUNET_PACKED;

  /**
   * Replication level for this message
   * In the current implementation, this value is not used.
   */
  uint32_t desired_replication_level GNUNET_PACKED;

  /**
   * Length of the PUT path that follows (if tracked).
   */
  uint32_t put_path_length GNUNET_PACKED;

  /**
   * Best known destination (could be my friend or finger) which should
   * get this message next.
   */
  struct GNUNET_PeerIdentity best_known_destination;

  /**
   * In case best_known_destination is a finger, then trail to reach
   * to that finger. Else its default value is 0.
   */
  struct GNUNET_HashCode intermediate_trail_id;

  /**
   * When does the content expire?
   */
  struct GNUNET_TIME_AbsoluteNBO expiration_time;

  /**
   * The key to store the value under.
   */
  struct GNUNET_HashCode key GNUNET_PACKED;

  /* put path (if tracked) */

  /* Payload */

};

/**
 * P2P GET message
 */
struct PeerGetMessage
{
  /**
   * Type: #GNUNET_MESSAGE_TYPE_XDHT_P2P_GET
   */
  struct GNUNET_MessageHeader header;

  /**
   * Processing options
   */
  uint32_t options GNUNET_PACKED;

  /**
   * Desired content type.
   */
  uint32_t block_type GNUNET_PACKED;

  /**
   * Hop count
   */
  uint32_t hop_count GNUNET_PACKED;

  /**
   * Desired replication level for this request.
   * In the current implementation, this value is not used.
   */
  uint32_t desired_replication_level GNUNET_PACKED;

  /**
   * Total number of peers in get path.
   */
  unsigned int get_path_length;

  /**
   * Best known destination (could be my friend or finger) which should
   * get this message next.
   */
  struct GNUNET_PeerIdentity best_known_destination;

  /**
   * In case best_known_destination is a finger, then trail to reach
   * to that finger. Else its default value is 0.
   */
  struct GNUNET_HashCode intermediate_trail_id;

  /**
   * The key we are looking for.
   */
  struct GNUNET_HashCode key;

  /* Get path. */
  /* struct GNUNET_PeerIdentity[]*/
};

/**
 * P2P Result message
 */
struct PeerGetResultMessage
{
  /**
   * Type: #GNUNET_MESSAGE_TYPE_XDHT_P2P_GET_RESULT
   */
  struct GNUNET_MessageHeader header;

  /**
   * The type for the data.
   */
  uint32_t type GNUNET_PACKED;

  /**
   * Number of peers recorded in the outgoing path from source to the
   * stored location of this message.
   */
  uint32_t put_path_length GNUNET_PACKED;

  /**
   * Length of the GET path that follows (if tracked).
   */
  uint32_t get_path_length GNUNET_PACKED;

  /**
   * Peer which queried for get and should get the result.
   */
  struct GNUNET_PeerIdentity querying_peer;

  /**
   * When does the content expire?
   */
  struct GNUNET_TIME_AbsoluteNBO expiration_time;

  /**
   * The key of the corresponding GET request.
   */
  struct GNUNET_HashCode key;

  /* put path (if tracked) */

  /* get path (if tracked) */

  /* Payload */

};

/**
 * P2P Trail setup message
 */
struct PeerTrailSetupMessage
{
  /**
   * Type: #GNUNET_MESSAGE_TYPE_XDHT_P2P_TRAIL_SETUP
   */
  struct GNUNET_MessageHeader header;

  /**
   * Is source_peer trying to setup the trail to a predecessor or any finger.
   */
  uint32_t is_predecessor;

  /**
   * Peer closest to this value will be our finger.
   */
  uint64_t final_destination_finger_value;

  /**
   * Source peer which wants to setup the trail to one of its finger.
   */
  struct GNUNET_PeerIdentity source_peer;

  /**
   * Best known destination (could be my friend or finger) which should
   * get this message next.
   *
   * FIXME: this could be removed if we include trail_source / trail_dest
   * in the routing table. This way we save 32 bytes of bandwidth by using
   * extra 8 bytes of memory (2 * sizeof (GNUNET_PEER_ID))
   */
  struct GNUNET_PeerIdentity best_known_destination;

  /**
   * In case best_known_destination is a finger, then trail id of trail to
   * reach to this finger.
   */
  struct GNUNET_HashCode intermediate_trail_id;

  /**
   * Trail id for trail which we are trying to setup.
   */
  struct GNUNET_HashCode trail_id;

  /* List of peers which are part of trail setup so far.
   * Trail does NOT include source_peer and peer which will be closest to
   * ultimate_destination_finger_value.
   * struct GNUNET_PeerIdentity trail[]
   */
};

/**
  * P2P Trail Setup Result message
 */
struct PeerTrailSetupResultMessage
{

  /**
   * Type: #GNUNET_MESSAGE_TYPE_XDHT_P2P_TRAIL_SETUP_RESULT
   */
  struct GNUNET_MessageHeader header;

  /**
   * Finger to which we have found the path.
   */
  struct GNUNET_PeerIdentity finger_identity;

  /**
   * Peer which started trail_setup to find trail to finger_identity
   */
  struct GNUNET_PeerIdentity querying_peer;

  /**
   * Is the trail setup to querying_peer's predecessor or finger?
   */
  uint32_t is_predecessor;

  /**
   * Value to which finger_identity is the closest peer.
   */
  uint64_t ultimate_destination_finger_value;

  /**
   * Identifier of the trail from querying peer to finger_identity, NOT
   * including both endpoints.
   */
  struct GNUNET_HashCode trail_id;

  /* List of peers which are part of the trail from querying peer to
   * finger_identity, NOT including both endpoints.
   * struct GNUNET_PeerIdentity trail[]
   */
};

/**
 * P2P Verify Successor Message.
 */
struct PeerVerifySuccessorMessage
{
  /**
   * Type: #GNUNET_MESSAGE_TYPE_XDHT_P2P_VERIFY_SUCCESSOR
   */
  struct GNUNET_MessageHeader header;

  /**
   * Peer which wants to verify its successor.
   */
  struct GNUNET_PeerIdentity source_peer;

  /**
   * Source Peer's current successor.
   */
  struct GNUNET_PeerIdentity successor;

  /**
   * Identifier of trail to reach from source_peer to successor.
   */
  struct GNUNET_HashCode trail_id;

  /* List of the peers which are part of trail to reach  from source_peer
   * to successor, NOT including them
   * struct GNUNET_PeerIdentity trail[]
   */
};

/**
 * P2P Verify Successor Result Message
 */
struct PeerVerifySuccessorResultMessage
{
  /**
   * Type: #GNUNET_MESSAGE_TYPE_XDHT_P2P_VERIFY_SUCCESSOR_RESULT
   */
  struct GNUNET_MessageHeader header;

  /**
   * Peer which sent the request to verify its successor.
   */
  struct GNUNET_PeerIdentity querying_peer;

  /**
   * Successor to which PeerVerifySuccessorMessage was sent.
   */
  struct GNUNET_PeerIdentity current_successor;

  /**
   * Current Predecessor of source_successor. It can be same as querying peer
   * or different. In case it is different then it can be querying_peer's
   * probable successor.
   */
  struct GNUNET_PeerIdentity probable_successor;

  /**
   * Trail identifier of trail from querying_peer to current_successor.
   */
  struct GNUNET_HashCode trail_id;

  /**
   * Direction in which we are looking at the trail.
   */
  uint32_t trail_direction;

  /* In case probable_successor != querying_peer, then trail to reach from
   * querying_peer to probable_successor, NOT including end points.
   * struct GNUNET_PeerIdentity trail[]
   */
};

/**
 * P2P Notify New Successor Message.
 */
struct PeerNotifyNewSuccessorMessage
{
  /**
   * Type: #GNUNET_MESSAGE_TYPE_XDHT_P2P_NOTIFY_NEW_SUCCESSOR
   */
  struct GNUNET_MessageHeader header;

  /**
   * Peer which wants to notify its new successor.
   */
  struct GNUNET_PeerIdentity source_peer;

  /**
   * New successor of source_peer.
   */
  struct GNUNET_PeerIdentity new_successor;

  /**
   * Unique identifier of the trail from source_peer to new_successor,
   * NOT including the endpoints.
   */
  struct GNUNET_HashCode trail_id;

  /* List of peers in trail from source_peer to new_successor,
   * NOT including the endpoints.
   * struct GNUNET_PeerIdentity trail[]
   */
};

/**
 * P2P Notify Successor Confirmation message.
 */
struct PeerNotifyConfirmationMessage
{
   /**
   * Type: #GNUNET_MESSAGE_TYPE_XDHT_P2P_TRAIL_TEARDOWN
   */
  struct GNUNET_MessageHeader header;

  /**
   * Unique identifier of the trail.
   */
  struct GNUNET_HashCode trail_id;

  /**
   * Direction of trail.
   */
  uint32_t trail_direction;
};


/**
 * P2P Trail Tear Down message.
 */
struct PeerTrailTearDownMessage
{
  /**
   * Type: #GNUNET_MESSAGE_TYPE_XDHT_P2P_TRAIL_TEARDOWN
   */
  struct GNUNET_MessageHeader header;

  /**
   * Unique identifier of the trail.
   */
  struct GNUNET_HashCode trail_id;

  /**
   * Direction of trail.
   */
  uint32_t trail_direction;
};


/**
 * P2P Trail Rejection Message.
 */
struct PeerTrailRejectionMessage
{
  /**
   * Type: #GNUNET_MESSAGE_TYPE_XDHT_P2P_TRAIL_SETUP_REJECTION
   */
  struct GNUNET_MessageHeader header;

  /**
   * Peer which wants to set up the trail.
   */
  struct GNUNET_PeerIdentity source_peer;

  /**
   * Peer which sent trail rejection message as it it congested.
   */
  struct GNUNET_PeerIdentity congested_peer;

  /**
   * Peer identity closest to this value will be finger of
   * source_peer.
   */
  uint64_t ultimate_destination_finger_value;

  /**
   * Is source_peer trying to setup the trail to its predecessor or finger.
   */
  uint32_t is_predecessor;

  /**
   * Identifier for the trail that source peer is trying to setup.
   */
  struct GNUNET_HashCode trail_id;

  /**
   * Relative time for which congested_peer will remain congested.
   */
  struct GNUNET_TIME_Relative congestion_time;

  /* Trail_list from source_peer to peer which sent the message for trail setup
   * to congested peer. This trail does NOT include source_peer.
   struct GNUNET_PeerIdnetity trail[]*/
};

/**
 * P2P Add Trail Message.
 */
struct PeerAddTrailMessage
{
  /**
   * Type: #GNUNET_MESSAGE_TYPE_XDHT_P2P_ADD_TRAIL
   */
  struct GNUNET_MessageHeader header;

  /**
   * Source of the routing trail.
   */
  struct GNUNET_PeerIdentity source_peer;

  /**
   * Destination of the routing trail.
   */
  struct GNUNET_PeerIdentity destination_peer;

  /**
   * Unique identifier of the trail from source_peer to destination_peer,
   * NOT including the endpoints.
   */
  struct GNUNET_HashCode trail_id;

  /* Trail from source peer to destination peer, NOT including them.
   * struct GNUNET_PeerIdentity trail[]
   */
};


GNUNET_NETWORK_STRUCT_END


/**
 *  Entry in friend_peermap.
 */
struct FriendInfo
{
  /**
   * Friend Identity
   */
  const struct GNUNET_PeerIdentity *id;

  /**
   * Number of trails for which this friend is the first hop or if the friend
   * is finger.
   */
  unsigned int trails_count;

  /**
   * In case not 0, then amount of time for which this friend is congested.
   */
  struct GNUNET_TIME_Absolute congestion_timestamp;

  /**
   * Handle for sending messages to this friend.
   */
  struct GNUNET_MQ_Handle *mq;

};

/**
 * An individual element of the trail to reach to a finger.
 */
struct Trail_Element
{
  /**
    * Pointer to next item in the list
    */
  struct Trail_Element *next;

  /**
    * Pointer to prev item in the list
    */
  struct Trail_Element *prev;

  /**
   * An element in this trail.
   */
  struct GNUNET_PeerIdentity peer;
};

/**
 * Information about an individual trail.
 */
struct Trail
{
  /**
   * Head of trail.
   */
  struct Trail_Element *trail_head;

  /**
   * Tail of trail.
   */
  struct Trail_Element *trail_tail;

  /**
   * Unique identifier of this trail.
   */
  struct GNUNET_HashCode trail_id;

  /**
   * Length of trail pointed
   */
  unsigned int trail_length;

  /**
   * Is there a valid trail entry.
   */
  unsigned int is_present;
};

/**
 * An entry in finger_table
 */
struct FingerInfo
{
  /**
   * Finger identity.
   */
  struct GNUNET_PeerIdentity finger_identity;

  /**
   * In case not 0, this amount is time to wait for notify successor message.
   * Used ONLY for successor. NOT for any other finger.
   */
  struct GNUNET_TIME_Absolute wait_notify_confirmation;

  /**
   * Is any finger stored at this finger index.
   */
  unsigned int is_present;

  /**
   * Index in finger peer map
   */
  uint32_t finger_table_index;

  /**
   * Number of trails setup so far for this finger.
   * Should not cross MAXIMUM_TRAILS_PER_FINGER.
   */
  uint32_t trails_count;

  /**
   * Array of trails to reach to this finger.
   */
  struct Trail trail_list[MAXIMUM_TRAILS_PER_FINGER];
};


/**
 * Stores information about the peer which is closest to destination_finger_value.
 * 'closest' can be either successor or predecessor depending on is_predecessor
 * flag.
 */
struct Closest_Peer
{
  /**
   * Destination finger value.
   */
  uint64_t destination_finger_value;

  /**
   * Is finger_value a predecessor or any other finger.
   */
  unsigned int is_predecessor;

  /**
   * Trail id to reach to peer.
   * In case peer is my identity or friend, it is set to 0.
   */
  struct GNUNET_HashCode trail_id;

  /**
   * Next destination. In case of friend and my_identity , it is same as next_hop
   * In case of finger it is finger identity.
   */
  struct GNUNET_PeerIdentity best_known_destination;

  /**
   * In case best_known_destination is a finger, then first friend in the trail
   * to reach to it. In other case, same as best_known_destination.
   */
  struct GNUNET_PeerIdentity next_hop;

  /**
   * In case finger is the next hop, it contains a valid finger table index
   * at which the finger is stored. Else, It contains 65, which is out of range
   * of finger table index.
   */
  unsigned int finger_table_index;
};

/**
 * Context for send_verify_successor_task.
 */
struct VerifySuccessorContext
{
  /**
   * Number of times this has been scheduled.
   */
  unsigned int num_retries_scheduled;
};

/**
 * Task that sends FIND FINGER TRAIL requests. This task is started when we have
 * get our first friend.
 */
static struct GNUNET_SCHEDULER_Task *find_finger_trail_task;

/**
 * Task that sends verify successor message. This task is started when we get
 * our successor for the first time.
 */
static struct GNUNET_SCHEDULER_Task *send_verify_successor_task;

/**
 * Task that sends verify successor message. This task is started when we get
 * our successor for the first time.
 */
static struct GNUNET_SCHEDULER_Task *send_verify_successor_retry_task;

/**
 * Task that sends verify successor message. This task is started when we get
 * our successor for the first time.
 */
static struct GNUNET_SCHEDULER_Task *send_notify_new_successor_retry_task;

/**
 * Identity of this peer.
 */
static struct GNUNET_PeerIdentity my_identity;

/**
 * Peer map of all the friends of a peer
 */
static struct GNUNET_CONTAINER_MultiPeerMap *friend_peermap;

/**
 * Array of all the fingers.
 */
static struct FingerInfo finger_table [MAX_FINGERS];

/**
 * Handle to CORE.
 */
static struct GNUNET_CORE_Handle *core_api;

/**
 * The current finger index that we have want to find trail to. We start the
 * search with value = 0, i.e. successor  and then go to PREDCESSOR_FINGER_ID
 * and decrement it. For any index 63 <= index < 0, if finger is same as successor,
 * we reset this index to 0.
 */
static unsigned int current_search_finger_index;

/**
 * Time duration to schedule find finger trail task.
 */
static struct GNUNET_TIME_Relative find_finger_trail_task_next_send_time;

/**
 * Time duration to schedule verify successor task.
 */
static struct GNUNET_TIME_Relative verify_successor_next_send_time;

/**
 * Time duration to send verify successor again, if result was not received in time.
 */
static struct GNUNET_TIME_Relative verify_successor_retry_time;

/**
 * Time duration to retry send_notify_successor.
 */
static struct GNUNET_TIME_Relative notify_successor_retry_time;

/**
 * Are we waiting for confirmation from our new successor that it got the
 * message
 */
//static unsigned int waiting_for_notify_confirmation;

/* Below variables are used only for testing, and statistics collection. */
/**
 * Should we store our topology predecessor and successor IDs into statistics?
 */
unsigned int track_topology;

/**
 * Count of fingers found. Ideally we should have O(logn) fingers for a
 * stable network.
 */
static unsigned int total_fingers_found;

/**
 * Number of times we found the same successor.
 */
static unsigned int successor_times;

/**
 * Number of rounds for which we should search for finger.
 */
static unsigned int fingers_round_count;


/**
 * Construct a trail setup message and forward it to @a target_friend
 *
 * @param source_peer Peer which wants to setup the trail
 * @param ultimate_destination_finger_value Peer identity closest to this value
 *                                          will be finger to @a source_peer
 * @param best_known_destination Best known destination (could be finger or friend)
 *                               which should get this message. In case it is
 *                               friend, then it is same as target_friend
 * @param target_friend Friend to which message is forwarded now.
 * @param trail_length Total number of peers in trail setup so far.
 * @param trail_peer_list Trail setup so far
 * @param is_predecessor Is @a source_peer looking for trail to a predecessor or not.
 * @param trail_id Unique identifier for the trail we are trying to setup.
 * @param intermediate_trail_id Trail id of intermediate trail to reach to
 *                              best_known_destination when its a finger. If not
 *                              used then set to 0.
 */
static void
GDS_NEIGHBOURS_send_trail_setup (const struct GNUNET_PeerIdentity *source_peer,
                                 uint64_t ultimate_destination_finger_value,
                                 const struct GNUNET_PeerIdentity *best_known_destination,
                                 const struct FriendInfo *target_friend,
                                 unsigned int trail_length,
                                 const struct GNUNET_PeerIdentity *trail_peer_list,
                                 unsigned int is_predecessor,
                                 const struct GNUNET_HashCode *trail_id,
                                 const struct GNUNET_HashCode *intermediate_trail_id)
{
  struct GNUNET_MQ_Envelope *env;
  struct PeerTrailSetupMessage *tsm;
  size_t msize;

  msize = trail_length * sizeof (struct GNUNET_PeerIdentity);
  if (msize + sizeof (struct PeerTrailSetupMessage)
      >= GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE)
  {
    GNUNET_break (0);
    return;
  }
  if (GNUNET_MQ_get_length (target_friend->mq) >= MAXIMUM_PENDING_PER_FRIEND)
  {
    GNUNET_STATISTICS_update (GDS_stats,
			      gettext_noop ("# P2P messages dropped due to full queue"),
			      1,
			      GNUNET_NO);
    return;
  }
  env = GNUNET_MQ_msg_extra (tsm,
			     msize,
			     GNUNET_MESSAGE_TYPE_XDHT_P2P_TRAIL_SETUP);
  tsm->final_destination_finger_value = GNUNET_htonll (ultimate_destination_finger_value);
  tsm->source_peer = *source_peer;
  tsm->best_known_destination = *best_known_destination;
  tsm->is_predecessor = htonl (is_predecessor);
  tsm->trail_id = *trail_id;
  tsm->intermediate_trail_id = *intermediate_trail_id;
  GNUNET_memcpy (&tsm[1],
		 trail_peer_list,
		 msize);
  GNUNET_MQ_send (target_friend->mq,
		  env);
}


/**
 * Construct a trail setup result message and forward it to @a target_friend.
 *
 * @param querying_peer Peer which sent the trail setup request and should get
 *                      the result back.
 * @param Finger Peer to which the trail has been setup to.
 * @param target_friend Friend to which this message should be forwarded.
 * @param trail_length Numbers of peers in the trail.
 * @param trail_peer_list Peers which are part of the trail from
 *                        querying_peer to Finger, NOT including them.
 * @param is_predecessor Is @a Finger predecessor to @a querying_peer ?
 * @param ultimate_destination_finger_value Value to which @a finger is the closest
 *                                          peer.
 * @param trail_id Unique identifier of the trail.
 */
static void
GDS_NEIGHBOURS_send_trail_setup_result (const struct GNUNET_PeerIdentity *querying_peer,
                                        const struct GNUNET_PeerIdentity *finger,
                                        struct FriendInfo *target_friend,
                                        unsigned int trail_length,
                                        const struct GNUNET_PeerIdentity *trail_peer_list,
                                        unsigned int is_predecessor,
                                        uint64_t ultimate_destination_finger_value,
                                        const struct GNUNET_HashCode *trail_id)
{
  struct GNUNET_MQ_Envelope *env;
  struct PeerTrailSetupResultMessage *tsrm;
  size_t msize;

  msize = trail_length * sizeof (struct GNUNET_PeerIdentity);
  if (msize + sizeof (struct PeerTrailSetupResultMessage)
      >= GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE)
  {
    GNUNET_break (0);
    return;
  }
  if (GNUNET_MQ_get_length (target_friend->mq) >= MAXIMUM_PENDING_PER_FRIEND)
  {
    GNUNET_STATISTICS_update (GDS_stats,
                              gettext_noop ("# P2P messages dropped due to full queue"),
                              1,
			      GNUNET_NO);
    return;
  }
  env = GNUNET_MQ_msg_extra (tsrm,
			     msize,
			     GNUNET_MESSAGE_TYPE_XDHT_P2P_TRAIL_SETUP_RESULT);
  tsrm->querying_peer = *querying_peer;
  tsrm->finger_identity = *finger;
  tsrm->is_predecessor = htonl (is_predecessor);
  tsrm->trail_id = *trail_id;
  tsrm->ultimate_destination_finger_value
    = GNUNET_htonll (ultimate_destination_finger_value);
  GNUNET_memcpy (&tsrm[1],
		 trail_peer_list,
		 msize);
  GNUNET_MQ_send (target_friend->mq,
		  env);
}


/**
 * Send notify successor confirmation message.
 *
 * @param trail_id Unique Identifier of the trail.
 * @param trail_direction Destination to Source.
 * @param target_friend Friend to get this message next.
 */
static void
GDS_NEIGHBOURS_send_notify_succcessor_confirmation (const struct GNUNET_HashCode *trail_id,
                                                    unsigned int trail_direction,
						    struct FriendInfo *target_friend)
{
  struct PeerNotifyConfirmationMessage *ncm;
  struct GNUNET_MQ_Envelope *env;

  if (GNUNET_MQ_get_length (target_friend->mq) >= MAXIMUM_PENDING_PER_FRIEND)
  {
    GNUNET_STATISTICS_update (GDS_stats,
			      gettext_noop ("# P2P messages dropped due to full queue"),
			      1,
			      GNUNET_NO);
    return;
  }
  env = GNUNET_MQ_msg (ncm,
		       GNUNET_MESSAGE_TYPE_XDHT_P2P_NOTIFY_SUCCESSOR_CONFIRMATION);
  ncm->trail_id = *trail_id;
  ncm->trail_direction = htonl (trail_direction);
  GNUNET_MQ_send (target_friend->mq,
		  env);
}


/**
 * Send trail rejection message to @a target_friend
 *
 * @param source_peer Peer which is trying to setup the trail.
 * @param ultimate_destination_finger_value Peer closest to this value will be
 *                                          @a source_peer's finger
 * @param congested_peer Peer which sent this message as it is congested.
 * @param is_predecessor Is source_peer looking for trail to a predecessor or not.
 * @param trail_peer_list Trails seen so far in trail setup before getting rejected
 *                        by congested_peer. This does NOT include @a source_peer
 *                        and congested_peer.
 * @param trail_length Total number of peers in trail_peer_list, NOT including
 *                     @a source_peer and @a congested_peer
 * @param trail_id Unique identifier of this trail.
 * @param congestion_timeout Duration given by congested peer as an estimate of
 *                           how long it may remain congested.
 */
static void
GDS_NEIGHBOURS_send_trail_rejection (const struct GNUNET_PeerIdentity *source_peer,
                                     uint64_t ultimate_destination_finger_value,
                                     const struct GNUNET_PeerIdentity *congested_peer,
                                     unsigned int is_predecessor,
                                     const struct GNUNET_PeerIdentity *trail_peer_list,
                                     unsigned int trail_length,
                                     const struct GNUNET_HashCode *trail_id,
                                     struct FriendInfo *target_friend,
                                     const struct GNUNET_TIME_Relative congestion_timeout)
{
  struct PeerTrailRejectionMessage *trm;
  struct GNUNET_MQ_Envelope *env;
  size_t msize;

  msize = trail_length * sizeof (struct GNUNET_PeerIdentity);
  if (msize + sizeof (struct PeerTrailRejectionMessage)
      >= GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE)
  {
    GNUNET_break (0);
    return;
  }
  if (GNUNET_MQ_get_length (target_friend->mq) >= MAXIMUM_PENDING_PER_FRIEND)
  {
    GNUNET_STATISTICS_update (GDS_stats,
			      gettext_noop ("# P2P messages dropped due to full queue"),
			      1,
			      GNUNET_NO);
    return;
  }
  env = GNUNET_MQ_msg_extra (trm,
			     msize,
			     GNUNET_MESSAGE_TYPE_XDHT_P2P_TRAIL_SETUP_REJECTION);
  trm->source_peer = *source_peer;
  trm->congested_peer = *congested_peer;
  trm->congestion_time = congestion_timeout;
  trm->is_predecessor = htonl (is_predecessor);
  trm->trail_id = *trail_id;
  trm->ultimate_destination_finger_value
    = GNUNET_htonll (ultimate_destination_finger_value);
  GNUNET_memcpy (&trm[1],
		 trail_peer_list,
		 msize);
  GNUNET_MQ_send (target_friend->mq,
		  env);
}


/**
 * Construct a verify successor message and forward it to target_friend.
 * @param source_peer Peer which wants to verify its successor.
 * @param successor Peer which is @a source_peer's current successor.
 * @param trail_id Unique Identifier of trail from @a source_peer to @a successor,
 *                 NOT including them.
 * @param trail List of peers which are part of trail to reach from @a source_peer
 *              to @a successor, NOT including them.
 * @param trail_length Total number of peers in @a trail.
 * @param target_friend Next friend to get this message.
 */
static void
GDS_NEIGHBOURS_send_verify_successor_message (const struct GNUNET_PeerIdentity *source_peer,
                                              const struct GNUNET_PeerIdentity *successor,
                                              const struct GNUNET_HashCode *trail_id,
                                              struct GNUNET_PeerIdentity *trail,
                                              unsigned int trail_length,
                                              struct FriendInfo *target_friend)
{
  struct PeerVerifySuccessorMessage *vsm;
  struct GNUNET_MQ_Envelope *env;
  size_t msize;

  msize = trail_length * sizeof (struct GNUNET_PeerIdentity);
  if (msize + sizeof (*vsm) >= GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE)
  {
    GNUNET_break (0);
    return;
  }
  if (GNUNET_MQ_get_length (target_friend->mq) >= MAXIMUM_PENDING_PER_FRIEND)
  {
    GNUNET_STATISTICS_update (GDS_stats,
			      gettext_noop ("# P2P messages dropped due to full queue"),
			      1,
			      GNUNET_NO);
    return;
  }
  env = GNUNET_MQ_msg_extra (vsm,
			     msize,
			     GNUNET_MESSAGE_TYPE_XDHT_P2P_VERIFY_SUCCESSOR);
  vsm->source_peer = *source_peer;
  vsm->successor = *successor;
  vsm->trail_id = *trail_id;
  GNUNET_memcpy (&vsm[1],
		 trail,
		 msize);
  GNUNET_MQ_send (target_friend->mq,
		  env);
}


/**
 * FIXME: In every function we pass target friend except for this one.
 * so, either change everything or this one. also, should we just store
 * the pointer to friend in routing table rather than gnunet_peeridentity.
 * if yes then we should keep friend info in.h  andmake lot of changes.
 * Construct a trail teardown message and forward it to target friend.
 *
 * @param trail_id Unique identifier of the trail.
 * @param trail_direction Direction of trail.
 * @param target_friend Friend to get this message.
 */
void
GDS_NEIGHBOURS_send_trail_teardown (const struct GNUNET_HashCode *trail_id,
                                    unsigned int trail_direction,
                                    const struct GNUNET_PeerIdentity *peer)
{
  struct PeerTrailTearDownMessage *ttdm;
  struct GNUNET_MQ_Envelope *env;
  struct FriendInfo *target_friend;

  if (NULL == (target_friend =
               GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						  peer)))
  {
    /* FIXME: In what case friend can be null. ?*/
    GNUNET_break (0);
    return;
  }
  if (GNUNET_MQ_get_length (target_friend->mq) >= MAXIMUM_PENDING_PER_FRIEND)
  {
    GNUNET_STATISTICS_update (GDS_stats,
			      gettext_noop ("# P2P messages dropped due to full queue"),
			      1,
			      GNUNET_NO);
    return;
  }
  env = GNUNET_MQ_msg (ttdm,
		       GNUNET_MESSAGE_TYPE_XDHT_P2P_TRAIL_TEARDOWN);
  ttdm->trail_id = *trail_id;
  ttdm->trail_direction = htonl (trail_direction);
  GNUNET_MQ_send (target_friend->mq,
		  env);
}


/**
 * Construct a verify successor result message and send it to target_friend
 *
 * @param querying_peer Peer which sent the verify successor message.
 * @param source_successor Current_successor of @a querying_peer.
 * @param current_predecessor Current predecessor of @a successor. Could be same
 *                            or different from @a querying_peer.
 * @param trail_id Unique identifier of the trail from @a querying_peer to
 *                 @a successor, NOT including them.
 * @param trail List of peers which are part of trail from @a querying_peer to
 *                 @a successor, NOT including them.
 * @param trail_length Total number of peers in @a trail
 * @param trail_direction Direction in which we are sending the message. In this
 *                        case we are sending result from @a successor to @a querying_peer.
 * @param target_friend Next friend to get this message.
 */
static void
GDS_NEIGHBOURS_send_verify_successor_result (const struct GNUNET_PeerIdentity *querying_peer,
                                             const struct GNUNET_PeerIdentity *current_successor,
                                             const struct GNUNET_PeerIdentity *probable_successor,
                                             const struct GNUNET_HashCode *trail_id,
                                             const struct GNUNET_PeerIdentity *trail,
                                             unsigned int trail_length,
                                             enum GDS_ROUTING_trail_direction trail_direction,
                                             struct FriendInfo *target_friend)
{
  struct PeerVerifySuccessorResultMessage *vsmr;
  struct GNUNET_MQ_Envelope *env;
  size_t msize;

  msize = trail_length * sizeof(struct GNUNET_PeerIdentity);
  if (msize + sizeof (struct PeerVerifySuccessorResultMessage)
      >= GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE)
  {
    GNUNET_break (0);
    return;
  }
  if (GNUNET_MQ_get_length (target_friend->mq) >= MAXIMUM_PENDING_PER_FRIEND)
  {
    GNUNET_STATISTICS_update (GDS_stats,
			      gettext_noop ("# P2P messages dropped due to full queue"),
			      1,
			      GNUNET_NO);
    return;
  }
  env = GNUNET_MQ_msg_extra (vsmr,
			     msize,
			     GNUNET_MESSAGE_TYPE_XDHT_P2P_VERIFY_SUCCESSOR_RESULT);
  vsmr->querying_peer = *querying_peer;
  vsmr->current_successor = *current_successor;
  vsmr->probable_successor = *probable_successor;
  vsmr->trail_direction = htonl (trail_direction);
  vsmr->trail_id = *trail_id;
  GNUNET_memcpy (&vsmr[1],
		 trail,
		 msize);
  GNUNET_MQ_send (target_friend->mq,
		  env);
}


/**
 * Construct a notify new successor message and send it to target_friend
 * @param source_peer Peer which wants to notify to its new successor that it
 *                    could be its predecessor.
 * @param successor New successor of @a source_peer
 * @param successor_trail List of peers in Trail to reach from
 *                            @a source_peer to @a new_successor, NOT including
 *                            the endpoints.
 * @param successor_trail_length Total number of peers in @a new_successor_trail.
 * @param successor_trail_id Unique identifier of @a new_successor_trail.
 * @param target_friend Next friend to get this message.
 */
static void
GDS_NEIGHBOURS_send_notify_new_successor (const struct GNUNET_PeerIdentity *source_peer,
                                          const struct GNUNET_PeerIdentity *successor,
                                          const struct GNUNET_PeerIdentity *successor_trail,
                                          unsigned int successor_trail_length,
                                          const struct GNUNET_HashCode *succesor_trail_id,
                                          struct FriendInfo *target_friend)
{
  struct PeerNotifyNewSuccessorMessage *nsm;
  struct GNUNET_MQ_Envelope *env;
  size_t msize;

  msize = successor_trail_length * sizeof(struct GNUNET_PeerIdentity);
  if (msize + sizeof (struct PeerNotifyNewSuccessorMessage)
      >= GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE)
  {
    GNUNET_break (0);
    return;
  }
  if (GNUNET_MQ_get_length (target_friend->mq) >= MAXIMUM_PENDING_PER_FRIEND)
  {
    GNUNET_STATISTICS_update (GDS_stats,
			      gettext_noop ("# P2P messages dropped due to full queue"),
			      1,
			      GNUNET_NO);
    return;
  }
  env = GNUNET_MQ_msg_extra (nsm,
			     msize,
			     GNUNET_MESSAGE_TYPE_XDHT_P2P_NOTIFY_NEW_SUCCESSOR);
  nsm->new_successor = *successor;
  nsm->source_peer = *source_peer;
  nsm->trail_id = *succesor_trail_id;
  GNUNET_memcpy (&nsm[1],
		 successor_trail,
		 msize);
  GNUNET_MQ_send (target_friend->mq,
		  env);
}


/**
 * Construct an add_trail message and send it to target_friend
 *
 * @param source_peer Source of the trail.
 * @param destination_peer Destination of the trail.
 * @param trail_id Unique identifier of the trail from
 *                 @a source_peer to @a destination_peer, NOT including the endpoints.
 * @param trail List of peers in Trail from @a source_peer to @a destination_peer,
 *              NOT including the endpoints.
 * @param trail_length Total number of peers in @a trail.
 * @param target_friend Next friend to get this message.
 */
static void
GDS_NEIGHBOURS_send_add_trail (const struct GNUNET_PeerIdentity *source_peer,
                               const struct GNUNET_PeerIdentity *destination_peer,
                               const struct GNUNET_HashCode *trail_id,
                               const struct GNUNET_PeerIdentity *trail,
                               unsigned int trail_length,
                               struct FriendInfo *target_friend)
{
  struct PeerAddTrailMessage *adm;
  struct GNUNET_MQ_Envelope *env;
  size_t msize;

  msize = trail_length * sizeof(struct GNUNET_PeerIdentity);
  if (msize + sizeof (struct PeerAddTrailMessage)
      >= GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE)
  {
    GNUNET_break (0);
    return;
  }
  if (GNUNET_MQ_get_length (target_friend->mq) >= MAXIMUM_PENDING_PER_FRIEND)
  {
    GNUNET_STATISTICS_update (GDS_stats,
			      gettext_noop ("# P2P messages dropped due to full queue"),
			      1,
			      GNUNET_NO);
    return;
  }
  env = GNUNET_MQ_msg_extra (adm,
			     msize,
			     GNUNET_MESSAGE_TYPE_XDHT_P2P_ADD_TRAIL);
  adm->source_peer = *source_peer;
  adm->destination_peer = *destination_peer;
  adm->trail_id = *trail_id;
  GNUNET_memcpy (&adm[1],
		 trail,
		 msize);
  GNUNET_MQ_send (target_friend->mq,
		  env);
}


/**
 * Search my location in trail. In case I am present more than once in the
 * trail (can happen during trail setup), then return my lowest index.
 *
 * @param trail List of peers
 * @return my_index if found
 *         trail_length + 1 if an entry is present twice, It is an error.
 *         -1 if no entry found.
 */
static int
search_my_index (const struct GNUNET_PeerIdentity *trail,
                 int trail_length)
{
  int i;
  int index_seen = trail_length + 1;
  int flag = 0;

  for (i = 0; i < trail_length; i++)
  {
    if (0 == GNUNET_CRYPTO_cmp_peer_identity (&my_identity, &trail[i]))
    {
      flag = 1;
      if(index_seen == (trail_length + 1))
        index_seen = i;
      else
      {
        DEBUG("Entry is present twice in trail. Its not allowed\n");
      }
      break;
    }
  }

  if (1 == flag)
    return index_seen;
  return -1;
}


/**
 * Check if the friend is congested or have reached maximum number of trails
 * it can be part of of.
 * @param friend Friend to be checked.
 * @return #GNUNET_NO if friend is not congested or have not crossed threshold.
 *         #GNUNET_YES if friend is either congested or have crossed threshold
 */
static int
is_friend_congested (struct FriendInfo *friend)
{
  if (( friend->trails_count < TRAILS_THROUGH_FRIEND_THRESHOLD) &&
      ((0 == GNUNET_TIME_absolute_get_remaining
             (friend->congestion_timestamp).rel_value_us)))
    return GNUNET_NO;
  return GNUNET_YES;
}


/**
 * Select closest finger to value.
 *
 * @param peer1 First peer
 * @param peer2 Second peer
 * @param value Value to be compare
 * @return Closest peer
 */
static const struct GNUNET_PeerIdentity *
select_closest_finger (const struct GNUNET_PeerIdentity *peer1,
                       const struct GNUNET_PeerIdentity *peer2,
                       uint64_t value)
{
  uint64_t peer1_value;
  uint64_t peer2_value;

  GNUNET_memcpy (&peer1_value, peer1, sizeof (uint64_t));
  GNUNET_memcpy (&peer2_value, peer2, sizeof (uint64_t));
  peer1_value = GNUNET_ntohll (peer1_value);
  peer2_value = GNUNET_ntohll (peer2_value);

  if (peer1_value == value)
  {
    return peer1;
  }

  if (peer2_value == value)
  {
    return peer2;
  }

  if (value < peer1_value && peer1_value < peer2_value)
  {
    return peer1;
  }
  else if (value < peer2_value && peer2_value < peer1_value)
  {
    return peer2;
  }
  else if (peer1_value < value && value < peer2_value)
  {
    return peer2;
  }
  else if (peer2_value < value && value < peer1_value)
  {
    return peer1;
  }
  else if (peer1_value < peer2_value && peer2_value < value)
  {
    return peer1;
  }
  else  // if (peer2_value < peer1_value && peer1_value < value)
  {
    return peer2;
  }
}


/**
 * Select closest predecessor to value.
 *
 * @param peer1 First peer
 * @param peer2 Second peer
 * @param value Value to be compare
 * @return Peer which precedes value in the network.
 */
static const struct GNUNET_PeerIdentity *
select_closest_predecessor (const struct GNUNET_PeerIdentity *peer1,
                            const struct GNUNET_PeerIdentity *peer2,
                            uint64_t value)
{
  uint64_t peer1_value;
  uint64_t peer2_value;

  GNUNET_memcpy (&peer1_value, peer1, sizeof (uint64_t));
  GNUNET_memcpy (&peer2_value, peer2, sizeof (uint64_t));
  peer1_value = GNUNET_ntohll (peer1_value);
  peer2_value = GNUNET_ntohll (peer2_value);

   if (peer1_value == value)
  {
    return peer1;
  }

  if (peer2_value == value)
  {
    return peer2;
  }

  if (value < peer1_value && peer1_value < peer2_value)
  {
    return peer2;
  }
  else if (value < peer2_value && peer2_value < peer1_value)
  {
    return peer1;
  }
  else if (peer1_value < value && value < peer2_value)
  {
    return peer1;
  }
  else if (peer2_value < value && value < peer1_value)
  {
    return peer2;
  }
  else if (peer1_value < peer2_value && peer2_value < value)
  {
    return peer2;
  }
  else  // if (peer2_value < peer1_value && peer1_value < value)
  {
    return peer1;
  }
}

#if 0
/**
 *
 *
 */
void
test_print_trail (struct GNUNET_PeerIdentity *trail,
                  unsigned int trail_length)
{
  struct GNUNET_PeerIdentity print_peer;
  int i;

  FPRINTF (stderr,_("\nSUPU %s, %s, %d,trail_length = %d"),
  __FILE__, __func__,__LINE__,trail_length);
  for (i =0 ; i< trail_length; i++)
  {
    print_peer = trail[i];
    FPRINTF (stderr,_("\nSUPU %s, %s, %d,trail[%d]=%s"),
            __FILE__, __func__,__LINE__,i,GNUNET_i2s(&print_peer));
  }
}
#endif

#if 0
/**
 * This is a test function to print all the entries of friend table.
 */
static void
test_friend_peermap_print ()
{
  struct FriendInfo *friend;
  struct GNUNET_CONTAINER_MultiPeerMapIterator *friend_iter;
  struct GNUNET_PeerIdentity print_peer;
  struct GNUNET_PeerIdentity key_ret;
  int i;

  print_peer = my_identity;
  FPRINTF (stderr,_("\nSUPU************  FRIEND_PEERMAP of %s"),GNUNET_i2s(&print_peer));
  friend_iter = GNUNET_CONTAINER_multipeermap_iterator_create (friend_peermap);

  for (i = 0; i < GNUNET_CONTAINER_multipeermap_size (friend_peermap); i++)
  {
    if(GNUNET_YES == GNUNET_CONTAINER_multipeermap_iterator_next (friend_iter,
                                                                  &key_ret,
                                                                  (const void **)&friend))
    {
      GNUNET_memcpy (&print_peer, &key_ret, sizeof (struct GNUNET_PeerIdentity));
      FPRINTF (stderr,_("\nSUPU %s, %s, %d, friend = %s, friend->trails_count = %d"),
              __FILE__, __func__,__LINE__, GNUNET_i2s(&print_peer), friend->trails_count);
    }
  }
}
#endif

#if 0
/**
 * This is a test function, to print all the entries of finger table.
 */
static void
test_finger_table_print()
{
  struct FingerInfo *finger;
  struct GNUNET_PeerIdentity print_peer;
  //struct Trail *trail;
  int i;
  //int j;
  //int k;
  print_peer = my_identity;
  FPRINTF (stderr,_("\nSUPU************  FINGER_TABLE of %s"),GNUNET_i2s(&print_peer));
  for (i = 0; i < MAX_FINGERS; i++)
  {
    finger = &finger_table[i];

    if (GNUNET_NO == finger->is_present)
      continue;

    print_peer = finger->finger_identity;
    FPRINTF (stderr,_("\nSUPU %s, %s, %d, finger_table[%d] = %s, trails_count = %d"),
            __FILE__, __func__,__LINE__,i,GNUNET_i2s (&print_peer), finger->trails_count);

#if 0
    for (j = 0; j < finger->trails_count; j++)
    {
      trail = &finger->trail_list[j];
      FPRINTF (stderr,_("\nSUPU %s, %s, %d, trail_id[%d]=%s"),__FILE__, __func__,__LINE__,j, GNUNET_h2s(&trail->trail_id));
      struct Trail_Element *element;
      element = trail->trail_head;
      for (k = 0; k < trail->trail_length; k++)
      {
        print_peer = element->peer;
        FPRINTF (stderr,_("\nSUPU %s, %s, %d,trail[%d] = %s "),__FILE__, __func__,__LINE__,k, GNUNET_i2s(&print_peer));
        element = element->next;
      }
    }
    #endif
  }
}
#endif

/**
 * Select the closest peer among two peers (which should not be same)
 * with respect to value and finger_table_index
 * NOTE: peer1 != peer2
 * @param peer1 First peer
 * @param peer2 Second peer
 * @param value Value relative to which we find the closest
 * @param is_predecessor Is value a predecessor or any other finger.
 * @return Closest peer among two peers.
 */
static const struct GNUNET_PeerIdentity *
select_closest_peer (const struct GNUNET_PeerIdentity *peer1,
                     const struct GNUNET_PeerIdentity *peer2,
                     uint64_t value,
                     unsigned int is_predecessor)
{
  /* This check is here to ensure that calling function never sends
      same peer value in peer1 and peer2. Remove it later. */
  GNUNET_assert(0 != GNUNET_CRYPTO_cmp_peer_identity (peer1, peer2));
  if (1 == is_predecessor)
    return select_closest_predecessor (peer1, peer2, value);

  // TODO: Change name to something like select_closest_successor!!
  return select_closest_finger (peer1, peer2, value);
}


/**
 * Iterate over the list of all the trails of a finger. In case the first
 * friend to reach the finger has reached trail threshold or is congested,
 * then don't select it. In case there multiple available good trails to reach
 * to Finger, choose the one with shortest trail length.
 * Note: We use length as parameter. But we can use any other suitable parameter
 * also.
 * @param finger Finger Finger whose trail we have to select.
 * @return Trail Selected Trail.
 */
static struct Trail *
select_finger_trail (struct FingerInfo *finger)
{
  struct FriendInfo *friend;
  struct Trail *current_finger_trail;
  struct Trail *best_trail = NULL;
  unsigned int i;

  GNUNET_assert (finger->trails_count > 0);
  for (i = 0; i < finger->trails_count; i++)
  {
    current_finger_trail = &finger->trail_list[i];

    /* No trail stored at this index. */
    if (GNUNET_NO == current_finger_trail->is_present)
      continue;

    GNUNET_assert (NULL !=
                  (friend =
                   GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                      &current_finger_trail->trail_head->peer)));

    /* First friend to reach trail is not free. */
    if (GNUNET_YES == is_friend_congested (friend))
      continue;

    if (NULL == best_trail ||
        best_trail->trail_length > current_finger_trail->trail_length)
    {
      best_trail = current_finger_trail;
    }
  }

  return best_trail;
}


/**
 * Compare FINGER entry with current successor. If finger's first friend of all
 * its trail is not congested and  has not crossed trail threshold, then check
 * if finger peer identity is closer to final_destination_finger_value than
 * current_successor. If yes then update current_successor.
 * @param current_successor[in/out]
 * @return
 */
static void
compare_finger_and_current_closest_peer (struct Closest_Peer *current_closest_peer)
{
  struct FingerInfo *finger;
  const struct GNUNET_PeerIdentity *closest_peer;
  struct Trail *finger_trail;
  int i;

  /* Iterate over finger table. */
  for (i = 0; i < MAX_FINGERS; i++)
  {
    finger = &finger_table[i];

    if (GNUNET_NO == finger->is_present)
      continue;

    if (0 == GNUNET_CRYPTO_cmp_peer_identity (&finger->finger_identity,
                                              &current_closest_peer->best_known_destination))
      continue;

    /* If I am my own finger, then ignore this finger. */
    if (0 == GNUNET_CRYPTO_cmp_peer_identity (&finger->finger_identity,
                                              &my_identity))
      continue;

   /* If finger is a friend, we have already checked it in previous function. */
    if (NULL != (GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                    &finger->finger_identity)))
    {
      continue;
    }

    closest_peer = select_closest_peer (&finger->finger_identity,
                                        &current_closest_peer->best_known_destination,
                                        current_closest_peer->destination_finger_value,
                                        current_closest_peer->is_predecessor);

    if (0 == GNUNET_CRYPTO_cmp_peer_identity(&finger->finger_identity,
					     closest_peer))
    {
      /* Choose one of the trail to reach to finger. */
      finger_trail = select_finger_trail (finger);

      /* In case no trail found, ignore this finger. */
      if (NULL == finger_trail)
        continue;

      current_closest_peer->best_known_destination = *closest_peer;
      current_closest_peer->next_hop = finger_trail->trail_head->peer;
      current_closest_peer->trail_id = finger_trail->trail_id;
      current_closest_peer->finger_table_index = i;
    }
    continue;
  }
}


/**
 * Compare friend entry with current successor.
 * If friend identity and current_successor is same, then do nothing.
 * If friend is not congested and has not crossed trail threshold, then check
 * if friend peer identity is closer to final_destination_finger_value than
 * current_successor. If yes then update current_successor.
 *
 * @param cls closure
 * @param key current public key
 * @param value struct Closest_Peer
 * @return #GNUNET_YES if we should continue to iterate,
 *         #GNUNET_NO if not.
 */
static int
compare_friend_and_current_closest_peer (void *cls,
                                         const struct GNUNET_PeerIdentity *key,
                                         void *value)
{
  struct FriendInfo *friend = value;
  struct Closest_Peer *current_closest_peer = cls;
  const struct GNUNET_PeerIdentity *closest_peer;

  /* Friend is either congested or has crossed threshold. */
  if (GNUNET_YES == is_friend_congested (friend))
    return GNUNET_YES;

  /* If current_closest_peer and friend identity are same, then do nothing.*/
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (friend->id,
                                            &current_closest_peer->best_known_destination))
  {
    GNUNET_break (0);
    return GNUNET_YES;
  }

  closest_peer = select_closest_peer (friend->id,
                                      &current_closest_peer->best_known_destination,
                                      current_closest_peer->destination_finger_value,
                                      current_closest_peer->is_predecessor);

  /* Is friend the closest successor? */
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (friend->id,
					    closest_peer))
  {
    current_closest_peer->best_known_destination = *friend->id;
    current_closest_peer->next_hop = *friend->id;
  }
  return GNUNET_YES;
}


/**
 * Initialize current_successor to my_identity.
 * @param my_identity My peer identity
 * @return Updated closest_peer
 */
static struct Closest_Peer
init_closest_peer (struct GNUNET_PeerIdentity my_identity,
		   uint64_t destination_finger_value,
		   unsigned int is_predecessor)
{
  struct Closest_Peer current_closest_peer;

  memset (&current_closest_peer.trail_id,
	  0,
	  sizeof(struct GNUNET_HashCode));
  current_closest_peer.destination_finger_value = destination_finger_value;
  current_closest_peer.is_predecessor = is_predecessor;
  current_closest_peer.next_hop = my_identity;
  current_closest_peer.best_known_destination = my_identity;
  current_closest_peer.finger_table_index = 65; //65 is a for non valid finger table index.
  return current_closest_peer;
}


/**
 * Find locally best known peer, among your own identity, friend and finger list,
 * which is closest to given destination_finger_value.
 *
 * NOTE: In case a friend is also a finger, then it is always chosen as friend
 * not a finger.
 * @param destination_finger_value Peer closest to this value will be the next destination.
 * @param is_predecessor Are we looking for predecessor or finger?
 * @return Closest_Peer that contains all the relevant field to reach to
 *                      @a destination_finger_value
 */
static struct Closest_Peer
find_local_best_known_next_hop (uint64_t destination_finger_value,
                                unsigned int is_predecessor)
{
  struct Closest_Peer current_closest_peer;

   /* Initialize current_successor to my_identity. */
  current_closest_peer = init_closest_peer (my_identity,
                                            destination_finger_value,
                                            is_predecessor);

  /* Compare each friend entry with current_successor and update current_successor
   * with friend if its closest. */
  GNUNET_assert
          (GNUNET_SYSERR !=
           GNUNET_CONTAINER_multipeermap_iterate (friend_peermap,
                                                  &compare_friend_and_current_closest_peer,
                                                  &current_closest_peer));

  /* Compare each finger entry with current_successor and update current_successor
   * with finger if its closest. */
  compare_finger_and_current_closest_peer (&current_closest_peer);
  return current_closest_peer;
}


/**
 * Construct a Put message and send it to target_peer.
 * @param key Key for the content
 * @param block_type Type of the block
 * @param options Routing options
 * @param desired_replication_level Desired replication count
 * @param best_known_dest Peer to which this message should reach eventually,
 *                        as it is best known destination to me.
 * @param intermediate_trail_id Trail id in case
 * @param target_peer Peer to which this message will be forwarded.
 * @param hop_count Number of hops traversed so far.
 * @param put_path_length Total number of peers in @a put_path
 * @param put_path Number of peers traversed so far
 * @param expiration_time When does the content expire
 * @param data Content to store
 * @param data_size Size of content @a data in bytes
 */
void
GDS_NEIGHBOURS_send_put (const struct GNUNET_HashCode *key,
                         enum GNUNET_BLOCK_Type block_type,
                         enum GNUNET_DHT_RouteOption options,
                         uint32_t desired_replication_level,
                         struct GNUNET_PeerIdentity best_known_dest,
                         struct GNUNET_HashCode intermediate_trail_id,
                         struct GNUNET_PeerIdentity *target_peer,
                         uint32_t hop_count,
                         uint32_t put_path_length,
                         struct GNUNET_PeerIdentity *put_path,
                         struct GNUNET_TIME_Absolute expiration_time,
                         const void *data, size_t data_size)
{
  struct PeerPutMessage *ppm;
  struct GNUNET_MQ_Envelope *env;
  struct FriendInfo *target_friend;
  struct GNUNET_PeerIdentity *pp;
  size_t msize;

  msize = put_path_length * sizeof (struct GNUNET_PeerIdentity) + data_size;
  if (msize + sizeof (struct PeerPutMessage)
      >= GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE)
  {
    put_path_length = 0;
    msize = data_size;
  }
  if (msize + sizeof (struct PeerPutMessage)
      >= GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE)
  {
    GNUNET_break (0);
    return;
  }

  GNUNET_assert (NULL !=
                 (target_friend =
                  GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						     target_peer)));
  env = GNUNET_MQ_msg_extra (ppm,
			     msize,
			     GNUNET_MESSAGE_TYPE_XDHT_P2P_PUT);
  ppm->options = htonl (options);
  ppm->block_type = htonl (block_type);
  ppm->hop_count = htonl (hop_count + 1);
  ppm->desired_replication_level = htonl (desired_replication_level);
  ppm->expiration_time = GNUNET_TIME_absolute_hton (expiration_time);
  ppm->best_known_destination = best_known_dest;
  ppm->intermediate_trail_id = intermediate_trail_id;
  ppm->key = *key;
  ppm->put_path_length = htonl (put_path_length);
  pp = (struct GNUNET_PeerIdentity *) &ppm[1];
  GNUNET_memcpy (pp,
		 put_path,
		 put_path_length * sizeof (struct GNUNET_PeerIdentity));
  GNUNET_memcpy (&pp[put_path_length],
		 data,
		 data_size);
  GNUNET_MQ_send (target_friend->mq,
		  env);
}


/**
 * Handle the put request from the client.
 *
 * @param block_type Type of the block
 * @param options Routing options
 * @param desired_replication_level Desired replication count
 * @param expiration_time When does the content expire
 * @param hop_count how many hops has this message traversed so far
 * @param bf Bloom filter of peers this PUT has already traversed
 * @param key Key for the content
 * @param put_path_length number of entries in put_path
 * @param put_path peers this request has traversed so far (if tracked)
 * @param data Content to store
 * @param data_size Size of content @a data in bytes
 * @return #GNUNET_OK if the request was forwarded, #GNUNET_NO if not
 */
int
GDS_NEIGHBOURS_handle_put (enum GNUNET_BLOCK_Type block_type,
                           enum GNUNET_DHT_RouteOption options,
                           uint32_t desired_replication_level,
                           struct GNUNET_TIME_Absolute expiration_time,
                           uint32_t hop_count,
                           struct GNUNET_CONTAINER_BloomFilter *bf,
                           const struct GNUNET_HashCode *key,
                           unsigned int put_path_length,
                           struct GNUNET_PeerIdentity *put_path,
                           const void *data,
			   size_t data_size)
{
  struct GNUNET_PeerIdentity best_known_dest;
  struct GNUNET_HashCode intermediate_trail_id;
  struct GNUNET_PeerIdentity next_hop;
  uint64_t key_value;
  struct Closest_Peer successor;

  GNUNET_memcpy (&key_value,
		 key,
		 sizeof (uint64_t));
  key_value = GNUNET_ntohll (key_value);
  successor = find_local_best_known_next_hop (key_value,
                                              GDS_FINGER_TYPE_NON_PREDECESSOR);
  best_known_dest = successor.best_known_destination;
  next_hop = successor.next_hop;
  intermediate_trail_id = successor.trail_id;

  if (0 == GNUNET_CRYPTO_cmp_peer_identity (&best_known_dest,
					    &my_identity))
  {
    DEBUG("\n PUT_REQUEST_SUCCESSFUL for key = %s",
	  GNUNET_h2s(key));
    /* I am the destination. */
    GDS_DATACACHE_handle_put (expiration_time,
			      key,
			      0,
			      NULL,
                              block_type,
			      data_size,
			      data);
    GDS_CLIENTS_process_put (options,
			     block_type,
			     0,
                             ntohl (desired_replication_level),
                             1,
			     &my_identity,
			     expiration_time,
                             key,
			     data,
			     data_size);
    return GNUNET_NO;
  }
  /* In case we are sending the request to  a finger, then send across all of its
   trail.*/
 GDS_NEIGHBOURS_send_put (key,
			  block_type,
			  options,
			  desired_replication_level,
                          best_known_dest,
			  intermediate_trail_id,
			  &next_hop,
                          0,
			  1,
			  &my_identity,
			  expiration_time,
                          data,
			  data_size);
 return GNUNET_OK;
}


/**
 * Construct a Get message and send it to target_peer.
 *
 * @param key Key for the content
 * @param block_type Type of the block
 * @param options Routing options
 * @param desired_replication_level Desired replication count
 * @param best_known_dest Peer which should get this message. Same as target peer
 *                        if best_known_dest is a friend else its a finger.
 * @param intermediate_trail_id  Trail id to reach to @a best_known_dest
 *                              in case it is a finger else set to 0.
 * @param target_peer Peer to which this message will be forwarded.
 * @param hop_count Number of hops traversed so far.
 * @param data Content to store
 * @param data_size Size of content @a data in bytes
 * @param get_path_length Total number of peers in @a get_path
 * @param get_path Number of peers traversed so far
 */
void
GDS_NEIGHBOURS_send_get (const struct GNUNET_HashCode *key,
                         enum GNUNET_BLOCK_Type block_type,
                         enum GNUNET_DHT_RouteOption options,
                         uint32_t desired_replication_level,
                         const struct GNUNET_PeerIdentity *best_known_dest,
                         const struct GNUNET_HashCode *intermediate_trail_id,
			 const struct GNUNET_PeerIdentity *target_peer,
                         uint32_t hop_count,
                         uint32_t get_path_length,
                         const struct GNUNET_PeerIdentity *get_path)
{
  struct PeerGetMessage *pgm;
  struct GNUNET_MQ_Envelope *env;
  struct FriendInfo *target_friend;
  size_t msize;

  msize = get_path_length * sizeof (struct GNUNET_PeerIdentity);
  if (msize + sizeof (struct PeerGetMessage)
      >= GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE)
  {
    GNUNET_break (0);
    return;
  }
  GNUNET_assert (NULL !=
                 (target_friend =
                  GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						     target_peer)));
  env = GNUNET_MQ_msg_extra (pgm,
			     msize,
			     GNUNET_MESSAGE_TYPE_XDHT_P2P_GET);
  pgm->get_path_length = htonl (get_path_length);
  pgm->best_known_destination = *best_known_dest;
  pgm->key = *key;
  pgm->intermediate_trail_id = *intermediate_trail_id;
  pgm->hop_count = htonl (hop_count + 1);
  pgm->get_path_length = htonl (get_path_length);
  GNUNET_memcpy (&pgm[1],
		 get_path,
		 msize);
  GNUNET_MQ_send (target_friend->mq,
		  env);
}


/**
 * Send the get result to requesting client.
 *
 * @param key Key of the requested data.
 * @param block_type Block type
 * @param target_peer Next peer to forward the message to.
 * @param source_peer Peer which has the data for the key.
 * @param put_path_length Number of peers in @a put_path
 * @param put_path Path taken to put the data at its stored location.
 * @param get_path_length Number of peers in @a get_path
 * @param get_path Path taken to reach to the location of the key.
 * @param expiration When will this result expire?
 * @param data Payload to store
 * @param data_size Size of the @a data
 */
void
GDS_NEIGHBOURS_send_get_result (const struct GNUNET_HashCode *key,
                                enum GNUNET_BLOCK_Type block_type,
                                const struct GNUNET_PeerIdentity *target_peer,
                                const struct GNUNET_PeerIdentity *source_peer,
                                unsigned int put_path_length,
                                const struct GNUNET_PeerIdentity *put_path,
                                unsigned int get_path_length,
                                const struct GNUNET_PeerIdentity *get_path,
                                struct GNUNET_TIME_Absolute expiration,
                                const void *data, size_t data_size)
{
  struct PeerGetResultMessage *get_result;
  struct GNUNET_PeerIdentity *paths;
  struct GNUNET_MQ_Envelope *env;
  struct FriendInfo *target_friend;
  int current_path_index;
  size_t msize;

  msize = (put_path_length + get_path_length) * sizeof (struct GNUNET_PeerIdentity) +
    data_size;
  if (msize + sizeof (struct PeerGetResultMessage)
      >= GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE)
  {
    put_path_length = 0;
    msize = data_size;
  }
  if (msize + sizeof (struct PeerGetResultMessage)
      >= GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE)
  {
    GNUNET_break(0);
    return;
  }
  current_path_index = 0;
  if (get_path_length > 0)
  {
    current_path_index = search_my_index (get_path,
					  get_path_length);
    if (-1 == current_path_index)
    {
      GNUNET_break (0);
      return;
    }
    if ((get_path_length + 1) == current_path_index)
    {
      DEBUG ("Peer found twice in get path. Not allowed \n");
      GNUNET_break (0);
      return;
    }
  }
  if (0 == current_path_index)
  {
    DEBUG ("GET_RESULT TO CLIENT KEY = %s, Peer = %s",
	   GNUNET_h2s (key),
	   GNUNET_i2s (&my_identity));
    GDS_CLIENTS_handle_reply (expiration,
			      key,
			      get_path_length,
                              get_path,
			      put_path_length,
                              put_path,
			      block_type,
			      data_size,
			      data);
    return;
  }
  env = GNUNET_MQ_msg_extra (get_result,
			     msize,
			     GNUNET_MESSAGE_TYPE_XDHT_P2P_GET_RESULT);
  get_result->key = *key;
  get_result->querying_peer = *source_peer;
  get_result->expiration_time = GNUNET_TIME_absolute_hton (expiration);
  get_result->get_path_length = htonl (get_path_length);
  get_result->put_path_length = htonl (put_path_length);
  paths = (struct GNUNET_PeerIdentity *)&get_result[1];
  GNUNET_memcpy (paths,
		 put_path,
		 put_path_length * sizeof (struct GNUNET_PeerIdentity));
  GNUNET_memcpy (&paths[put_path_length],
		 get_path,
		 get_path_length * sizeof (struct GNUNET_PeerIdentity));
  GNUNET_memcpy (&paths[put_path_length + get_path_length],
		 data,
		 data_size);

  GNUNET_assert (NULL !=
                (target_friend =
                 GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                    &get_path[current_path_index - 1])));
  GNUNET_MQ_send (target_friend->mq,
		  env);
}


/**
 * Handle a result for a GET operation.
 *
 * @param cls closure
 * @param type type of the block
 * @param expiration_time when does the content expire
 * @param key key for the content
 * @param put_path_length number of entries in @a put_path
 * @param put_path peers the original PUT traversed (if tracked)
 * @param get_path_length number of entries in @a get_path
 * @param get_path peers this reply has traversed so far (if tracked)
 * @param data payload of the reply
 * @param data_size number of bytes in @a data
 */
static void
get_cb (void *cls,
        enum GNUNET_BLOCK_Type type,
        struct GNUNET_TIME_Absolute expiration_time,
        const struct GNUNET_HashCode *key,
        unsigned int put_path_length,
        const struct GNUNET_PeerIdentity *put_path,
        unsigned int get_path_length,
        const struct GNUNET_PeerIdentity *get_path,
        const void *data,
        size_t data_size)
{
  struct GNUNET_PeerIdentity *target_peer = cls;
  // FIXME: inline?
  GDS_NEIGHBOURS_send_get_result (key,
                                  type,
                                  target_peer,
                                  &my_identity,
                                  put_path_length,
                                  put_path,
                                  1,
                                  &my_identity,
                                  expiration_time,
                                  data,
                                  data_size);
}


/**
 * Perform a GET operation.  Forwards the given request to other
 * peers.  Does not lookup the key locally.  May do nothing if this is
 * the only peer in the network (or if we are the closest peer in the
 * network).
 *
 * @param block_type type of the block
 * @param options routing options
 * @param desired_replication_level desired replication count
 * @param hop_count how many hops did this request traverse so far?
 * @param key key for the content
 * @param xquery extended query
 * @param xquery_size number of bytes in @a xquery
 * @param bg group to filter duplicates
 * @param peer_bf filter for peers not to select (again, updated)
 * @return #GNUNET_OK if the request was forwarded, #GNUNET_NO if not
 */
int
GDS_NEIGHBOURS_handle_get (enum GNUNET_BLOCK_Type block_type,
                           enum GNUNET_DHT_RouteOption options,
                           uint32_t desired_replication_level,
                           uint32_t hop_count,
                           const struct GNUNET_HashCode *key,
                           const void *xquery,
                           size_t xquery_size,
                           struct GNUNET_BLOCK_Group *bg,
                           struct GNUNET_CONTAINER_BloomFilter *peer_bf)
{
  struct Closest_Peer successor;
  struct GNUNET_PeerIdentity best_known_dest;
  struct GNUNET_HashCode intermediate_trail_id;
  uint64_t key_value;

  GNUNET_memcpy (&key_value,
		 key,
		 sizeof (uint64_t));
  key_value = GNUNET_ntohll (key_value);
  successor = find_local_best_known_next_hop (key_value,
                                              GDS_FINGER_TYPE_NON_PREDECESSOR);
  best_known_dest = successor.best_known_destination;
  intermediate_trail_id = successor.trail_id;

  /* I am the destination. I have the data. */
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (&my_identity,
                                            &best_known_dest))
  {
    GDS_DATACACHE_handle_get (key,
			      block_type,
			      NULL,
			      0,
                              bg,
                              &get_cb,
                              NULL);
    return GNUNET_NO;
  }

  GDS_NEIGHBOURS_send_get (key,
			   block_type,
			   options,
			   desired_replication_level,
                           &best_known_dest,
			   &intermediate_trail_id,
			   &successor.next_hop,
                           0,
			   1,
			   &my_identity);
  return GNUNET_OK;
}


/**
 * Randomly choose one of your friends (which is not congested and have not crossed
 * trail threshold) from the friend_peermap
 * @return Friend Randomly chosen friend.
 *         NULL in case friend peermap is empty, or all the friends are either
 *              congested or have crossed trail threshold.
 */
static struct FriendInfo *
select_random_friend ()
{
  unsigned int current_size;
  uint32_t index;
  unsigned int j = 0;
  struct GNUNET_CONTAINER_MultiPeerMapIterator *iter;
  struct GNUNET_PeerIdentity key_ret;
  struct FriendInfo *friend;

  current_size = GNUNET_CONTAINER_multipeermap_size (friend_peermap);

  /* No friends.*/
  if (0 == current_size)
    return NULL;

  index = GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_WEAK, current_size);
  iter = GNUNET_CONTAINER_multipeermap_iterator_create (friend_peermap);

  /* Iterate till you don't reach to index. */
  for (j = 0; j < index ; j++)
    GNUNET_assert (GNUNET_YES ==
                   GNUNET_CONTAINER_multipeermap_iterator_next (iter, NULL, NULL));

  do
  {
    /* Reset the index in friend peermap to 0 as we reached to the end. */
    if (j == current_size)
    {
      j = 0;
      GNUNET_CONTAINER_multipeermap_iterator_destroy (iter);
      iter = GNUNET_CONTAINER_multipeermap_iterator_create (friend_peermap);

    }

    /* Get the friend stored at the index, j*/
    GNUNET_assert (GNUNET_YES ==
                   GNUNET_CONTAINER_multipeermap_iterator_next (iter,
                                                                &key_ret,
                                                                (const void **)&friend));

    /* This friend is not congested and has not crossed trail threshold. */
    if ((friend->trails_count < TRAILS_THROUGH_FRIEND_THRESHOLD) &&
        (0 == GNUNET_TIME_absolute_get_remaining (friend->congestion_timestamp).rel_value_us))
    {
      break;
    }
    friend = NULL;
    j++;
  } while (j != index);

  GNUNET_CONTAINER_multipeermap_iterator_destroy (iter);
  return friend;
}


/**
 * Compute 64 bit value of finger_identity corresponding to a finger index using
 * Chord formula.
 * For all fingers, n.finger[i] = n + pow (2,i),
 * For predecessor, n.finger[PREDECESSOR_FINGER_ID] = n - 1, where
 * n = my_identity, i = finger_index, n.finger[i] = 64 bit finger value
 *
 * @param finger_index Index corresponding to which we calculate 64 bit value.
 * @return 64 bit value.
 */
static uint64_t
compute_finger_identity_value (unsigned int finger_index)
{
  uint64_t my_id64;

  GNUNET_memcpy (&my_id64,
		 &my_identity,
		 sizeof (uint64_t));
  my_id64 = GNUNET_ntohll (my_id64);

  /* Are we looking for immediate predecessor? */
  if (PREDECESSOR_FINGER_ID == finger_index)
    return (my_id64 - 1);
  uint64_t add = (uint64_t)1 << finger_index;
  return (my_id64 + add);
}


/**
 * Choose a random friend. Calculate the next finger identity to search,from
 * current_search_finger_index. Start looking for the trail to reach to
 * finger identity through this random friend.
 *
 * @param cls closure for this task
 */
static void
send_find_finger_trail_message (void *cls)
{
  struct FriendInfo *target_friend;
  struct GNUNET_HashCode trail_id;
  struct GNUNET_HashCode intermediate_trail_id;
  unsigned int is_predecessor = 0;
  uint64_t finger_id_value;

  /* Schedule another send_find_finger_trail_message task. After one round of
   * finger search, this time is exponentially backoff. */
  find_finger_trail_task_next_send_time.rel_value_us =
      find_finger_trail_task_next_send_time.rel_value_us +
      GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK,
                                DHT_FIND_FINGER_TRAIL_INTERVAL.rel_value_us);
  find_finger_trail_task =
      GNUNET_SCHEDULER_add_delayed (find_finger_trail_task_next_send_time,
                                    &send_find_finger_trail_message,
                                    NULL);

  /* No space in my routing table. (Source and destination peers also store entries
   * in their routing table).  */
  if (GNUNET_YES == GDS_ROUTING_threshold_reached())
    return;

  target_friend = select_random_friend ();
  if (NULL == target_friend)
    return;

  finger_id_value = compute_finger_identity_value (current_search_finger_index);
  if (PREDECESSOR_FINGER_ID == current_search_finger_index)
    is_predecessor = 1;

  /* Generate a unique trail id for trail we are trying to setup. */
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_STRONG,
                              &trail_id,
			      sizeof (trail_id));
  memset (&intermediate_trail_id,
	  0,
	  sizeof (struct GNUNET_HashCode));
  GDS_NEIGHBOURS_send_trail_setup (&my_identity,
				   finger_id_value,
                                   target_friend->id,
				   target_friend,
				   0, NULL,
                                   is_predecessor,
				   &trail_id,
                                   &intermediate_trail_id);
}


/**
 * In case there are already maximum number of possible trails to reach to a
 * finger, then check if the new trail's length is lesser than any of the
 * existing trails.
 * If yes then replace that old trail by new trail.
 *
 * Note: Here we are taking length as a parameter to choose the best possible
 * trail, but there could be other parameters also like:
 * 1. duration of existence of a trail - older the better.
 * 2. if the new trail is completely disjoint than the
 *    other trails, then may be choosing it is better.
 *
 * @param finger Finger
 * @param new_finger_trail List of peers to reach from me to @a finger, NOT
 *                         including the endpoints.
 * @param new_finger_trail_length Total number of peers in @a new_finger_trail
 * @param new_finger_trail_id Unique identifier of @a new_finger_trail.
 */
static void
select_and_replace_trail (struct FingerInfo *finger,
                          const struct GNUNET_PeerIdentity *new_trail,
                          unsigned int new_trail_length,
                          const struct GNUNET_HashCode *new_trail_id)
{
  struct Trail *current_trail;
  unsigned int largest_trail_length;
  unsigned int largest_trail_index;
  struct Trail_Element *trail_element;
  const struct GNUNET_PeerIdentity *next_hop;
  unsigned int i;

  largest_trail_length = new_trail_length;
  largest_trail_index = MAXIMUM_TRAILS_PER_FINGER + 1;

  GNUNET_assert (MAXIMUM_TRAILS_PER_FINGER == finger->trails_count);

  for (i = 0; i < finger->trails_count; i++)
  {
    current_trail = &finger->trail_list[i];
    GNUNET_assert (GNUNET_YES == current_trail->is_present);
    if (current_trail->trail_length > largest_trail_length)
    {
      largest_trail_length = current_trail->trail_length;
      largest_trail_index = i;
    }
  }

  /* New trail is not better than existing ones. Send trail teardown. */
  if (largest_trail_index == (MAXIMUM_TRAILS_PER_FINGER + 1))
  {
    next_hop = GDS_ROUTING_get_next_hop (new_trail_id,
					 GDS_ROUTING_SRC_TO_DEST);
    GDS_ROUTING_remove_trail (new_trail_id);
    GDS_NEIGHBOURS_send_trail_teardown (new_trail_id,
                                        GDS_ROUTING_SRC_TO_DEST,
                                        next_hop);
    return;
  }

  /* Send trail teardown message across the replaced trail. */
  struct Trail *replace_trail = &finger->trail_list[largest_trail_index];
  next_hop = GDS_ROUTING_get_next_hop (&replace_trail->trail_id,
				       GDS_ROUTING_SRC_TO_DEST);
  GNUNET_assert (GNUNET_YES == GDS_ROUTING_remove_trail (&replace_trail->trail_id));
  GDS_NEIGHBOURS_send_trail_teardown (&replace_trail->trail_id,
                                      GDS_ROUTING_SRC_TO_DEST,
                                      next_hop);

  /* Free the trail. */
  while (NULL != (trail_element = replace_trail->trail_head))
  {
    GNUNET_CONTAINER_DLL_remove (replace_trail->trail_head,
                                 replace_trail->trail_tail,
				 trail_element);
    GNUNET_free_non_null (trail_element);
  }

  /* Add new trial at that location. */
  replace_trail->is_present = GNUNET_YES;
  replace_trail->trail_length = new_trail_length;
  replace_trail->trail_id = *new_trail_id;

  for (i = 0; i < new_trail_length; i++)
  {
    struct Trail_Element *element = GNUNET_new (struct Trail_Element);
    element->peer = new_trail[i];

    GNUNET_CONTAINER_DLL_insert_tail (replace_trail->trail_head,
                                      replace_trail->trail_tail,
                                      element);
  }
  /* FIXME: URGENT Are we adding the trail back to the list. */
}


/**
 * Check if the new trail to reach to finger is unique or do we already have
 * such a trail present for finger.
 * @param existing_finger Finger identity
 * @param new_trail New trail to reach @a existing_finger
 * @param trail_length Total number of peers in new_trail.
 * @return #GNUNET_YES if the new trail is unique
 *         #GNUNET_NO if same trail is already present.
 */
static int
is_new_trail_unique (struct FingerInfo *existing_finger,
                     const struct GNUNET_PeerIdentity *new_trail,
                     unsigned int trail_length)
{
  struct Trail *current_trail;
  struct Trail_Element *trail_element;
  int i;
  int j;

  GNUNET_assert (existing_finger->trails_count > 0);

  /* Iterate over list of trails. */
  for (i = 0; i < existing_finger->trails_count; i++)
  {
    current_trail = &(existing_finger->trail_list[i]);
    if(GNUNET_NO == current_trail->is_present)
      continue;

    /* New trail and existing trail length are not same. */
    if (current_trail->trail_length != trail_length)
    {
      return GNUNET_YES;
    }

    trail_element = current_trail->trail_head;
    for (j = 0; j < current_trail->trail_length; j++)
    {
      if (0 != GNUNET_CRYPTO_cmp_peer_identity (&new_trail[j],
                                                &trail_element->peer))
      {
        return GNUNET_YES;
      }
      trail_element = trail_element->next;
    }
  }
  return GNUNET_NO;
}


/**
 * FIXME; In case of multiple trails, we may have a case where a trail from in
 * between has been removed, then we should try to find a free slot , not simply
 * add a trail at then end of the list.
 * Add a new trail at a free slot in trail array of existing finger.
 * @param existing_finger Finger
 * @param new_finger_trail New trail from me to finger, NOT including endpoints
 * @param new_finger_trail_length Total number of peers in @a new_finger_trail
 * @param new_finger_trail_id Unique identifier of the trail.
 */
static void
add_new_trail (struct FingerInfo *existing_finger,
               const struct GNUNET_PeerIdentity *new_trail,
               unsigned int new_trail_length,
               const struct GNUNET_HashCode *new_trail_id)
{
  struct FriendInfo *friend;
  struct Trail *trail;
  unsigned int i;
  int free_slot = -1;

  if (GNUNET_NO == is_new_trail_unique (existing_finger,
					new_trail,
                                        new_trail_length))
    return;

  for (i = 0; i < existing_finger->trails_count; i++)
  {
    if (GNUNET_NO == existing_finger->trail_list[i].is_present)
    {
      free_slot = i;
      break;
    }
  }

  if (-1 == free_slot)
    free_slot = i;

  trail = &existing_finger->trail_list[free_slot];
  GNUNET_assert (GNUNET_NO == trail->is_present);
  trail->trail_id = *new_trail_id;
  trail->trail_length = new_trail_length;
  existing_finger->trails_count++;
  trail->is_present = GNUNET_YES;
  if (0 == new_trail_length)
  {
    friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                &existing_finger->finger_identity);
  }
  else
  {
    friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                &new_trail[0]);
  }
  GNUNET_assert (NULL != friend);
  friend->trails_count++;
  for (i = 0; i < new_trail_length; i++)
  {
    struct Trail_Element *element;

    element = GNUNET_new (struct Trail_Element);
    element->peer = new_trail[i];
    GNUNET_CONTAINER_DLL_insert_tail (trail->trail_head,
                                      trail->trail_tail,
                                      element);
  }

  existing_finger->trail_list[free_slot].trail_head = trail->trail_head;
  existing_finger->trail_list[free_slot].trail_tail = trail->trail_tail;
  existing_finger->trail_list[free_slot].trail_length = new_trail_length;
  existing_finger->trail_list[free_slot].trail_id = *new_trail_id;
  existing_finger->trail_list[free_slot].is_present = GNUNET_YES;
}


#if 0
/**
 * FIXME; In case of multiple trails, we may have a case where a trail from in
 * between has been removed, then we should try to find a free slot , not simply
 * add a trail at then end of the list.
 * Add a new trail at a free slot in trail array of existing finger.
 * @param existing_finger Finger
 * @param new_finger_trail New trail from me to finger, NOT including endpoints
 * @param new_finger_trail_length Total number of peers in @a new_finger_trail
 * @param new_finger_trail_id Unique identifier of the trail.
 */
static void
add_new_trail (struct FingerInfo *existing_finger,
               const struct GNUNET_PeerIdentity *new_trail,
               unsigned int new_trail_length,
               const struct GNUNET_HashCode *new_trail_id)
{
  struct Trail *trail;
  struct FriendInfo *first_friend;
  int i;
  int index;

  if (GNUNET_NO == is_new_trail_unique (existing_finger,
					new_trail,
                                        new_trail_length))
    return;

  index = existing_finger->trails_count;
  trail = &existing_finger->trail_list[index];
  GNUNET_assert (GNUNET_NO == trail->is_present);
  trail->trail_id = *new_trail_id;
  trail->trail_length = new_trail_length;
  existing_finger->trails_count++;
  trail->is_present = GNUNET_YES;

  GNUNET_assert (NULL == (GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                             &existing_finger->finger_identity)));
  /* If finger is a friend then we never call this function. */
  GNUNET_assert (new_trail_length > 0);

  first_friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                    &new_trail[0]);
  first_friend->trails_count++;

  for (i = 0; i < new_trail_length; i++)
  {
    struct Trail_Element *element;

    element = GNUNET_new (struct Trail_Element);
    element->peer = new_trail[i];
    GNUNET_CONTAINER_DLL_insert_tail (trail->trail_head,
                                      trail->trail_tail,
                                      element);
  }
  /* Do we need to add trail head and trail tail in the trail list itearator.*/
  existing_finger->trail_list[index].trail_head = trail->trail_head;
  existing_finger->trail_list[index].trail_tail = trail->trail_tail;
  existing_finger->trail_list[index].trail_length = new_trail_length;
  existing_finger->trail_list[index].trail_id = *new_trail_id;
  existing_finger->trail_list[index].is_present = GNUNET_YES;
}
#endif

/**
 * Get the next hop to send trail teardown message from routing table and
 * then delete the entry from routing table. Send trail teardown message for a
 * specific trail of a finger.
 * @param finger Finger whose trail is to be removed.
 * @param trail List of peers in trail from me to a finger, NOT including
 *              endpoints.
 */
static void
send_trail_teardown (struct FingerInfo *finger,
                     struct Trail *trail)
{
  struct FriendInfo *friend;
  const struct GNUNET_PeerIdentity *next_hop;

  next_hop = GDS_ROUTING_get_next_hop (&trail->trail_id,
                                       GDS_ROUTING_SRC_TO_DEST);
  if (NULL == next_hop)
  {
//    DEBUG(" NO ENTRY FOUND IN %s ROUTING TABLE for trail id %s, line=%d,traillength = %d",
//            GNUNET_i2s(&my_identity), GNUNET_h2s(&trail->trail_id), __LINE__,trail->trail_length);
    return;
  }
  GNUNET_assert (0 != GNUNET_CRYPTO_cmp_peer_identity (&finger->finger_identity,
                                                       &my_identity));

  GNUNET_assert(GNUNET_YES == trail->is_present);
  if (trail->trail_length > 0)
  {
    friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                &trail->trail_head->peer);
  }
  else
  {
    friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                &finger->finger_identity);
  }

  if(NULL == friend)
  {
    DEBUG ("\n LINE NO: = %d, Friend not found for trail id  %s of peer %s trail length = %d",
           __LINE__,
	   GNUNET_h2s (&trail->trail_id),
	   GNUNET_i2s(&my_identity),
	   trail->trail_length);
    return;
  }
  if ( (0 != GNUNET_CRYPTO_cmp_peer_identity (next_hop,
					      friend->id) ) &&
       (0 == trail->trail_length))
  {
     DEBUG ("\n LINE NO: = %d, Friend not found for trail id  %s of peer %s trail length = %d",
	    __LINE__,
	    GNUNET_h2s (&trail->trail_id),
	    GNUNET_i2s (&my_identity),
	    trail->trail_length);
    return;
  }
  GNUNET_assert (GNUNET_YES ==
		 GDS_ROUTING_remove_trail (&trail->trail_id));
  friend->trails_count--;
  GDS_NEIGHBOURS_send_trail_teardown (&trail->trail_id,
                                      GDS_ROUTING_SRC_TO_DEST,
                                      friend->id);
}


/**
 * Send trail teardown message across all the trails to reach to finger.
 * @param finger Finger whose all the trail should be freed.
 */
static void
send_all_finger_trails_teardown (struct FingerInfo *finger)
{
  for (unsigned int i = 0; i < finger->trails_count; i++)
  {
    struct Trail *trail;

    trail = &finger->trail_list[i];
    if (GNUNET_YES == trail->is_present)
    {
      send_trail_teardown (finger, trail);
      trail->is_present = GNUNET_NO;
    }
  }
}


/**
 * Free a specific trail
 * @param trail List of peers to be freed.
 */
static void
free_trail (struct Trail *trail)
{
  struct Trail_Element *trail_element;

  while (NULL != (trail_element = trail->trail_head))
  {
    GNUNET_CONTAINER_DLL_remove (trail->trail_head,
                                 trail->trail_tail,
                                 trail_element);
    GNUNET_free_non_null (trail_element);
  }
  trail->trail_head = NULL;
  trail->trail_tail = NULL;
}


/**
 * Free finger and its trail.
 *
 * @param finger Finger to be freed.
 * @param finger_table_index Index at which finger is stored.
 */
static void
free_finger (struct FingerInfo *finger,
	     unsigned int finger_table_index)
{
  struct Trail *trail;

  for (unsigned int i = 0; i < finger->trails_count; i++)
  {
    trail = &finger->trail_list[i];
    if (GNUNET_NO == trail->is_present)
      continue;

    if (trail->trail_length > 0)
      free_trail (trail);
    trail->is_present = GNUNET_NO;
  }

  finger->is_present = GNUNET_NO;
  memset (&finger_table[finger_table_index],
	  0,
	  sizeof (finger_table[finger_table_index]));
}


/**
 * Add a new entry in finger table at finger_table_index.
 * In case I am my own finger, then we don't have a trail. In case of a friend,
 * we have a trail with unique id and '0' trail length.
 * In case a finger is a friend, then increment the trails count of the friend.
 *
 * @param finger_identity Peer Identity of new finger
 * @param finger_trail Trail to reach from me to finger (excluding both end points).
 * @param finger_trail_length Total number of peers in @a finger_trail.
 * @param trail_id Unique identifier of the trail.
 * @param finger_table_index Index in finger table.
 */
static void
add_new_finger (const struct GNUNET_PeerIdentity *finger_identity,
                const struct GNUNET_PeerIdentity *finger_trail,
                unsigned int finger_trail_length,
                const struct GNUNET_HashCode *trail_id,
                unsigned int finger_table_index)
{
  struct FingerInfo *new_entry;
  struct FriendInfo *first_trail_hop;
  struct Trail *trail;
  unsigned int i;

  new_entry = GNUNET_new (struct FingerInfo);
  new_entry->finger_identity = *finger_identity;
  new_entry->finger_table_index = finger_table_index;
  new_entry->is_present = GNUNET_YES;

  /* If the new entry is my own identity. */
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (&my_identity,
                                            finger_identity))
  {
    new_entry->trails_count = 0;
    finger_table[finger_table_index] = *new_entry;
    GNUNET_free (new_entry);
    return;
  }

  /* Finger is a friend. */
  if (0 == finger_trail_length)
  {
    new_entry->trail_list[0].trail_id = *trail_id;
    new_entry->trails_count = 1;
    new_entry->trail_list[0].is_present = GNUNET_YES;
    new_entry->trail_list[0].trail_length = 0;
    new_entry->trail_list[0].trail_head = NULL;
    new_entry->trail_list[0].trail_tail = NULL;
    finger_table[finger_table_index] = *new_entry;
    GNUNET_assert (NULL !=
                  (first_trail_hop =
		   GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						      finger_identity)));

    first_trail_hop->trails_count++;
    GNUNET_free (new_entry);
    return;
  }

  GNUNET_assert (NULL !=
		 (first_trail_hop =
		  GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						     &finger_trail[0])));
  new_entry->trails_count = 1;
  first_trail_hop->trails_count++;
  /* Copy the finger trail into trail. */
  trail = &new_entry->trail_list[0];
  for(i = 0; i < finger_trail_length; i++)
  {
    struct Trail_Element *element = GNUNET_new (struct Trail_Element);

    element->next = NULL;
    element->prev = NULL;
    element->peer = finger_trail[i];
    GNUNET_CONTAINER_DLL_insert_tail (trail->trail_head,
                                      trail->trail_tail,
                                      element);
  }

  /* Add trail to trail list. */
  trail->trail_length = finger_trail_length;
  trail->trail_id = *trail_id;
  trail->is_present = GNUNET_YES;
  finger_table[finger_table_index] = *new_entry;
  GNUNET_free (new_entry);
}


/**
 * Periodic task to verify current successor. There can be multiple trails to reach
 * to successor, choose the shortest one and send verify successor message
 * across that trail.
 *
 * @param cls closure for this task
 */
static void
send_verify_successor_message (void *cls)
{
  struct FriendInfo *target_friend;
  struct Trail *trail;
  struct Trail_Element *element;
  unsigned int trail_length;
  unsigned int i = 0;
  struct FingerInfo *successor;

  successor = &finger_table[0];

  /* This task will be scheduled when the result for Verify Successor is received. */
  send_verify_successor_task = NULL;

  /* When verify successor is being called for first time *for current context*
   * cls will be NULL. If send_verify_successor_retry_task is not NO_TASK, we
   * must cancel the retry task scheduled for verify_successor of previous
   * context.
   */
  if (NULL == cls)
  {
    /* FIXME: Here we are scheduling a new verify successor task, as we
     got a new successor. But a send verify successor task may be in progress.
     1. We need to be sure that this is indeed a new successor. As this function
     is called even if we add a new trail to reach t old successor.
     2. Assuming the new successor is different, then verify successor message
     * to old successor may be following stages.
     * --> Waiting for verify successor result. Don't wait anymore. there is
     *     no trail to reach from old successor to me, hence, routing
     *     lookup will fail.
     * --> Waiting for notify confirmation. again don't wait for it. notify
     *    confirmation will not succeded.
     */
    if (send_verify_successor_retry_task != NULL)
    {
      /* FIXME: Are we scheduling retry task as soon as we send verify message.
       If yes then here before making this task, first check if the message
       is for the same peer again. */
      struct VerifySuccessorContext *old_ctx =
          GNUNET_SCHEDULER_cancel(send_verify_successor_retry_task);
      /* old_ctx must not be NULL, as the retry task had been scheduled */
      GNUNET_assert(NULL != old_ctx);
      GNUNET_free(old_ctx);
      /* FIXME: Why don't we reset the task to NO_TASK here? */
    }

    struct VerifySuccessorContext *ctx;
    ctx = GNUNET_new (struct VerifySuccessorContext);

    ctx->num_retries_scheduled++;
    send_verify_successor_retry_task =
        GNUNET_SCHEDULER_add_delayed (verify_successor_retry_time,
                                      &send_verify_successor_message,
                                      ctx);
  }
  else
  {
    /* This is a retry attempt for verify_successor for a previous context */
    struct VerifySuccessorContext *ctx;

    ctx = cls;
    ctx->num_retries_scheduled++;
    send_verify_successor_retry_task =
        GNUNET_SCHEDULER_add_delayed (verify_successor_retry_time,
                                      &send_verify_successor_message,
                                      ctx);
  }

  /* Among all the trails to reach to successor, select first one which is present.*/
  for (i = 0; i < successor->trails_count; i++)
  {
    trail = &successor->trail_list[i];
    if (GNUNET_YES == trail->is_present)
      break;
  }

  /* No valid trail found to reach to successor. */
  if (i == successor->trails_count)
    return;

  GNUNET_assert(0 != GNUNET_CRYPTO_cmp_peer_identity (&my_identity,
                                                      &successor->finger_identity));
  /* Trail stored at this index. */
  GNUNET_assert (GNUNET_YES == trail->is_present);
  if (NULL == GDS_ROUTING_get_next_hop (&trail->trail_id,
					GDS_ROUTING_SRC_TO_DEST))
  {
    DEBUG (" NO ENTRY FOUND IN %s ROUTING TABLE for trail id %s, line %u",
	   GNUNET_i2s (&my_identity),
	   GNUNET_h2s (&trail->trail_id),
	   __LINE__);
    GNUNET_break(0);
    return;
  }
  trail_length = trail->trail_length;
  if (trail_length > 0)
  {
     /* Copy the trail into peer list. */
    struct GNUNET_PeerIdentity peer_list[trail_length];

    element = trail->trail_head;
    for(i = 0; i < trail_length; i++)
    {
      peer_list[i] = element->peer;
      element = element->next;
    }
    GNUNET_assert (NULL != (target_friend =
                            GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                               &peer_list[0])));
    GDS_NEIGHBOURS_send_verify_successor_message (&my_identity,
                                                  &successor->finger_identity,
                                                  &trail->trail_id,
						  peer_list,
						  trail_length,
                                                  target_friend);
  }
  else
  {
    GNUNET_assert (NULL != (target_friend =
                            GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                               &successor->finger_identity)));
    GDS_NEIGHBOURS_send_verify_successor_message (&my_identity,
                                                  &successor->finger_identity,
                                                  &trail->trail_id,
						  NULL,
						  0,
                                                  target_friend);
  }
}


/**
 * FIXME: should this be a periodic task, incrementing the search finger index?
 * Update the current search finger index.
 * @a finger_identity
 * @a finger_table_index
 */
static void
update_current_search_finger_index (unsigned int finger_table_index)
{
  struct FingerInfo *successor;

  /* FIXME correct this: only move current index periodically */
  if (finger_table_index != current_search_finger_index)
    return;

  successor = &finger_table[0];
  GNUNET_assert (GNUNET_YES == successor->is_present);

  /* We were looking for immediate successor.  */
  if (0 == current_search_finger_index)
  {
    current_search_finger_index = PREDECESSOR_FINGER_ID;
    if (0 != GNUNET_CRYPTO_cmp_peer_identity (&my_identity,
					      &successor->finger_identity))
    {
      if (NULL == send_verify_successor_task)
      {
        send_verify_successor_task
	  = GNUNET_SCHEDULER_add_now (&send_verify_successor_message,
				      NULL);
      }
    }
    return;
  }
  current_search_finger_index--;
}


/**
 * Get the least significant bit set in val.
 *
 * @param val Value
 * @return Position of first bit set, 65 in case of error.
 */
static unsigned int
find_set_bit (uint64_t val)
{
  uint64_t i;
  unsigned int pos;

  i = 1;
  pos = 0;

  while (!(i & val))
  {
    i = i << 1;
    pos++;
    if (pos > 63)
    {
      GNUNET_break (0);
      return 65;
    }
  }

  if (val/i != 1)
    return 65; /* Some other bit was set to 1 as well. */

  return pos;
}


/**
 * Calculate finger_table_index from initial 64 bit finger identity value that
 * we send in trail setup message.
 * @param ultimate_destination_finger_value Value that we calculated from our
 *                                          identity and finger_table_index.
 * @param is_predecessor Is the entry for predecessor or not?
 * @return finger_table_index Value between 0 <= finger_table_index <= 64
 *         finger_table_index > PREDECESSOR_FINGER_ID, if error occurs.
 */
static unsigned int
get_finger_table_index (uint64_t ultimate_destination_finger_value,
                        unsigned int is_predecessor)
{
  uint64_t my_id64;
  uint64_t diff;
  unsigned int finger_table_index;

  GNUNET_memcpy (&my_id64, &my_identity, sizeof (uint64_t));
  my_id64 = GNUNET_ntohll (my_id64);

  /* Is this a predecessor finger? */
  if (1 == is_predecessor)
  {
    diff =  my_id64 - ultimate_destination_finger_value;
    if (1 == diff)
      finger_table_index = PREDECESSOR_FINGER_ID;
    else
      finger_table_index = PREDECESSOR_FINGER_ID + 1; //error value

  }
  else
  {
    diff = ultimate_destination_finger_value - my_id64;
    finger_table_index = find_set_bit (diff);
  }
  return finger_table_index;
}


/**
 * Remove finger and its associated data structures from finger table.
 * @param existing_finger Finger to be removed which is in finger table.
 * @param finger_table_index Index in finger table where @a existing_finger
 *                           is stored.
 */
static void
remove_existing_finger (struct FingerInfo *existing_finger,
                        unsigned int finger_table_index)
{
  GNUNET_assert (GNUNET_YES == existing_finger->is_present);

  /* If I am my own finger, then we have no trails. */
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (&existing_finger->finger_identity,
                                            &my_identity))
  {
    existing_finger->is_present = GNUNET_NO;
    memset ((void *)&finger_table[finger_table_index], 0,
            sizeof (finger_table[finger_table_index]));
    return;
  }

  /* For all other fingers, send trail teardown across all the trails to reach
   finger, and free the finger. */
  send_all_finger_trails_teardown (existing_finger);
  free_finger (existing_finger, finger_table_index);
}


/**
 * Check if there is already an entry in finger_table at finger_table_index.
 * We get the finger_table_index from 64bit finger value we got from the network.
 * -- If yes, then select the closest finger.
 *   -- If new and existing finger are same, then check if you can store more
 *      trails.
 *      -- If yes then add trail, else keep the best trails to reach to the
 *         finger.
 *   -- If the new finger is closest, remove the existing entry, send trail
 *      teardown message across all the trails to reach the existing entry.
 *      Add the new finger.
 *  -- If new and existing finger are different, and existing finger is closest
 *     then do nothing.
 * -- Update current_search_finger_index.
 * @param finger_identity Peer Identity of new finger
 * @param finger_trail Trail to reach the new finger
 * @param finger_trail_length Total number of peers in @a new_finger_trail.
 * @param is_predecessor Is this entry for predecessor in finger_table?
 * @param finger_value 64 bit value of finger identity that we got from network.
 * @param finger_trail_id Unique identifier of @finger_trail.
 */
static void
finger_table_add (const struct GNUNET_PeerIdentity *finger_identity,
                  const struct GNUNET_PeerIdentity *finger_trail,
                  unsigned int finger_trail_length,
                  unsigned int is_predecessor,
                  uint64_t finger_value,
                  const struct GNUNET_HashCode *finger_trail_id)
{
  struct FingerInfo *existing_finger;
  const struct GNUNET_PeerIdentity *closest_peer;
  struct FingerInfo *successor;
  unsigned int finger_table_index;

  /* Get the finger_table_index corresponding to finger_value we got from network.*/
  finger_table_index = get_finger_table_index (finger_value, is_predecessor);

  /* Invalid finger_table_index. */
  if ((finger_table_index > PREDECESSOR_FINGER_ID))
  {
    GNUNET_break_op (0);
    return;
  }

  /* Check if  new entry is same as successor. */
  if ((0 != finger_table_index) &&
      (PREDECESSOR_FINGER_ID != finger_table_index))
  {
    successor = &finger_table[0];
    if (GNUNET_NO == successor->is_present)
    {
      GNUNET_break (0); //ASSERTION FAILS HERE. FIXME
      return;
    }
    if (0 == GNUNET_CRYPTO_cmp_peer_identity (finger_identity,
                                              &successor->finger_identity))
    {
      if (0 == fingers_round_count)
      {
         find_finger_trail_task_next_send_time =
              GNUNET_TIME_STD_BACKOFF(find_finger_trail_task_next_send_time);
      }
      else
        fingers_round_count--;
      current_search_finger_index = 0;
      GNUNET_STATISTICS_update (GDS_stats,
                                gettext_noop
                                ("# FINGERS_COUNT"), (int64_t) total_fingers_found,
                                GNUNET_NO);
      total_fingers_found  = 0;
      return;
    }

    struct FingerInfo prev_finger;
    prev_finger = finger_table[finger_table_index - 1];
    if (0 == GNUNET_CRYPTO_cmp_peer_identity (finger_identity,
                                              &prev_finger.finger_identity))
    {
       current_search_finger_index--;
       return;
    }
  }

  total_fingers_found++;
  existing_finger = &finger_table[finger_table_index];

  /* No entry present in finger_table for given finger map index. */
  if (GNUNET_NO == existing_finger->is_present)
  {
    /* Shorten the trail if possible. */
    add_new_finger (finger_identity,
		    finger_trail,
                    finger_trail_length,
                    finger_trail_id,
		    finger_table_index);
    update_current_search_finger_index (finger_table_index);
    return;
  }

  /* If existing entry and finger identity are not same. */
  if (0 != GNUNET_CRYPTO_cmp_peer_identity (&existing_finger->finger_identity,
                                            finger_identity))
  {
    closest_peer = select_closest_peer (&existing_finger->finger_identity,
                                        finger_identity,
                                        finger_value,
                                        is_predecessor);

    /* If the new finger is the closest peer. */
    if (0 == GNUNET_CRYPTO_cmp_peer_identity (finger_identity,
					      closest_peer))
    {
      remove_existing_finger (existing_finger,
			      finger_table_index);
      add_new_finger (finger_identity,
		      finger_trail,
		      finger_trail_length,
                      finger_trail_id,
		      finger_table_index);
    }
    else
    {
      /* Existing finger is the closest one. We need to send trail teardown
         across the trail setup in routing table of all the peers. */
      if (0 != GNUNET_CRYPTO_cmp_peer_identity (finger_identity,
						&my_identity))
      {
        if (finger_trail_length > 0)
          GDS_NEIGHBOURS_send_trail_teardown (finger_trail_id,
                                              GDS_ROUTING_SRC_TO_DEST,
                                              &finger_trail[0]);
        else
          GDS_NEIGHBOURS_send_trail_teardown (finger_trail_id,
                                              GDS_ROUTING_SRC_TO_DEST,
                                              finger_identity);
      }
    }
  }
  else
  {
    /* If both new and existing entry are same as my_identity, then do nothing. */
    if (0 == GNUNET_CRYPTO_cmp_peer_identity (&existing_finger->finger_identity,
                                              &my_identity))
    {
      return;
    }

    /* If there is space to store more trails. */
    if (existing_finger->trails_count < MAXIMUM_TRAILS_PER_FINGER)
        add_new_trail (existing_finger,
		       finger_trail,
                       finger_trail_length,
		       finger_trail_id);
    else
        select_and_replace_trail (existing_finger,
				  finger_trail,
                                  finger_trail_length,
				  finger_trail_id);
  }
  update_current_search_finger_index (finger_table_index);
  return;
}


/**
 * Verify validity of P2P put messages.
 *
 * @param cls closure
 * @param put the message
 * @return #GNUNET_OK if the message is well-formed
 */
static int
check_dht_p2p_put (void *cls,
                    const struct PeerPutMessage *put)
{
  size_t msize;
  uint32_t putlen;

  msize = ntohs (put->header.size);
  putlen = ntohl (put->put_path_length);
  if ((msize <
       sizeof (struct PeerPutMessage) +
       putlen * sizeof (struct GNUNET_PeerIdentity)) ||
      (putlen >
       GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE / sizeof (struct GNUNET_PeerIdentity)))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Core handler for P2P put messages.
 *
 * @param cls closure
 * @param put the message
 */
static void
handle_dht_p2p_put (void *cls,
                    const struct PeerPutMessage *put)
{
  struct GNUNET_PeerIdentity *put_path;
  struct GNUNET_PeerIdentity current_best_known_dest;
  struct GNUNET_PeerIdentity best_known_dest;
  struct GNUNET_HashCode received_intermediate_trail_id;
  struct GNUNET_HashCode intermediate_trail_id;
  struct GNUNET_PeerIdentity next_hop;
  const struct GNUNET_PeerIdentity *next_routing_hop;
  enum GNUNET_DHT_RouteOption options;
  struct GNUNET_HashCode test_key;
  struct Closest_Peer successor;
  void *payload;
  size_t msize;
  uint32_t putlen = ntohl (put->put_path_length);
  struct GNUNET_PeerIdentity pp[putlen + 1];
  uint32_t hop_count;
  size_t payload_size;
  uint64_t key_value;

  msize = ntohs (put->header.size);
  GNUNET_STATISTICS_update (GDS_stats,
                            gettext_noop ("# Bytes received from other peers"),
			    (int64_t) msize,
                            GNUNET_NO);

  current_best_known_dest = put->best_known_destination;
  put_path = (struct GNUNET_PeerIdentity *) &put[1];
  payload = &put_path[putlen];
  options = ntohl (put->options);
  received_intermediate_trail_id = put->intermediate_trail_id;
  hop_count = ntohl(put->hop_count);
  payload_size = msize - (sizeof (struct PeerPutMessage) +
                          putlen * sizeof (struct GNUNET_PeerIdentity));
  hop_count++;
  switch (GNUNET_BLOCK_get_key (GDS_block_context,
				ntohl (put->block_type),
                                payload,
				payload_size,
				&test_key))
  {
    case GNUNET_YES:
      if (0 != memcmp (&test_key,
		       &put->key,
		       sizeof (struct GNUNET_HashCode)))
      {
        char *put_s = GNUNET_strdup (GNUNET_h2s_full (&put->key));
        GNUNET_break_op (0);
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "PUT with key `%s' for block with key %s\n",
		    put_s,
		    GNUNET_h2s_full (&test_key));
        GNUNET_free (put_s);
        return;
      }
    break;
    case GNUNET_NO:
      GNUNET_break_op (0);
      return;
    case GNUNET_SYSERR:
      /* cannot verify, good luck */
      break;
  }

  if (ntohl (put->block_type) == GNUNET_BLOCK_TYPE_REGEX) /* FIXME: do for all tpyes */
  {
    switch (GNUNET_BLOCK_evaluate (GDS_block_context,
                                   ntohl (put->block_type),
                                   NULL,
                                   GNUNET_BLOCK_EO_NONE,
                                   NULL,    /* query */
                                   NULL, 0, /* xquery */
                                   payload,
				   payload_size))
    {
    case GNUNET_BLOCK_EVALUATION_OK_MORE:
    case GNUNET_BLOCK_EVALUATION_OK_LAST:
      break;

    case GNUNET_BLOCK_EVALUATION_OK_DUPLICATE:
    case GNUNET_BLOCK_EVALUATION_RESULT_INVALID:
    case GNUNET_BLOCK_EVALUATION_RESULT_IRRELEVANT:
    case GNUNET_BLOCK_EVALUATION_REQUEST_VALID:
    case GNUNET_BLOCK_EVALUATION_REQUEST_INVALID:
    case GNUNET_BLOCK_EVALUATION_TYPE_NOT_SUPPORTED:
    default:
      GNUNET_break_op (0);
      return;
    }
  }

  /* Check if you are already a part of put path. */
  unsigned int i;
  for (i = 0; i < putlen; i++)
  {
    if (0 == GNUNET_CRYPTO_cmp_peer_identity (&my_identity,
					      &put_path[i]))
    {
      putlen = i;
      break;
    }
  }

  /* Add yourself to the list. */
  //if (0 != (options & GNUNET_DHT_RO_RECORD_ROUTE))
  if (1)
  {
    GNUNET_memcpy (pp,
		   put_path,
		   putlen * sizeof (struct GNUNET_PeerIdentity));
    pp[putlen] = my_identity;
    putlen++;
  }
  else
  {
    putlen = 0;
  }
  GNUNET_memcpy (&key_value,
		 &put->key,
		 sizeof (uint64_t));
  key_value = GNUNET_ntohll (key_value);
  successor = find_local_best_known_next_hop (key_value,
                                              GDS_FINGER_TYPE_NON_PREDECESSOR);
  next_hop = successor.next_hop;
  intermediate_trail_id = successor.trail_id;
  best_known_dest = successor.best_known_destination;

  if (0 != (GNUNET_CRYPTO_cmp_peer_identity (&current_best_known_dest,
					     &my_identity)))
  {
    next_routing_hop = GDS_ROUTING_get_next_hop (&received_intermediate_trail_id,
                                                 GDS_ROUTING_SRC_TO_DEST);
    if (NULL != next_routing_hop)
    {
      next_hop = *next_routing_hop;
      intermediate_trail_id = received_intermediate_trail_id;
      best_known_dest = current_best_known_dest;
    }
  }

  GDS_CLIENTS_process_put (options,
                           ntohl (put->block_type),
                           hop_count,
                           ntohl (put->desired_replication_level),
                           putlen,
			   pp,
                           GNUNET_TIME_absolute_ntoh (put->expiration_time),
                           &put->key,
                           payload,
                           payload_size);

  /* I am the final destination */
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (&my_identity,
					    &best_known_dest))
  {
    DEBUG ("\n PUT_REQUEST_SUCCESSFUL for key = %s",
	   GNUNET_h2s(&put->key));
    GDS_DATACACHE_handle_put (GNUNET_TIME_absolute_ntoh (put->expiration_time),
                              &put->key,
			      putlen,
			      pp,
			      ntohl (put->block_type),
                              payload_size,
			      payload);
  }
  GDS_NEIGHBOURS_send_put (&put->key,
                           ntohl (put->block_type),
			   ntohl (put->options),
                           ntohl (put->desired_replication_level),
                           best_known_dest,
			   intermediate_trail_id,
			   &next_hop,
                           hop_count,
			   putlen,
			   pp,
                           GNUNET_TIME_absolute_ntoh (put->expiration_time),
                           payload,
			   payload_size);
}


/**
 * Check integrity of @a get message.
 *
 * @param cls closure
 * @param get the message
 * @return #GNUNET_OK if @a get is well-formed
 */
static int
check_dht_p2p_get (void *cls,
		   const struct PeerGetMessage *get)
{
  uint32_t get_length;
  size_t msize;

  msize = ntohs (get->header.size);
  get_length = ntohl (get->get_path_length);
  if ((msize <
       sizeof (struct PeerGetMessage) +
       get_length * sizeof (struct GNUNET_PeerIdentity)) ||
       (get_length >
        GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE / sizeof (struct GNUNET_PeerIdentity)))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * FIXME: Check for loop in the request. If you already are part of get path,
 * then you need to reset the get path length.
 * Core handler for p2p get requests.
 *
 * @param cls closure
 * @param get the message
 */
static void
handle_dht_p2p_get (void *cls,
                    const struct PeerGetMessage *get)
{
  const struct GNUNET_PeerIdentity *get_path;
  struct GNUNET_PeerIdentity best_known_dest;
  struct GNUNET_PeerIdentity current_best_known_dest;
  struct GNUNET_HashCode intermediate_trail_id;
  struct GNUNET_HashCode received_intermediate_trail_id;
  struct Closest_Peer successor;
  struct GNUNET_PeerIdentity next_hop;
  const struct GNUNET_PeerIdentity *next_routing_hop;
  uint32_t get_length;
  uint64_t key_value;
  uint32_t hop_count;
  size_t msize;

  msize = ntohs (get->header.size);
  get_length = ntohl (get->get_path_length);
  current_best_known_dest = get->best_known_destination;
  received_intermediate_trail_id = get->intermediate_trail_id;
  get_path = (const struct GNUNET_PeerIdentity *) &get[1];
  hop_count = get->hop_count;
  hop_count++;
  GNUNET_STATISTICS_update (GDS_stats,
                            gettext_noop ("# Bytes received from other peers"),
			    msize,
                            GNUNET_NO);
  GNUNET_memcpy (&key_value,
		 &get->key,
		 sizeof (uint64_t));
  key_value = GNUNET_ntohll (key_value);

  /* Check if you are already a part of get path. */
  for (unsigned int i = 0; i < get_length; i++)
  {
    if (0 == GNUNET_CRYPTO_cmp_peer_identity (&my_identity,
					      &get_path[i]))
    {
      get_length = i;
      break;
    }
  }

  /* Add yourself in the get path. */
  struct GNUNET_PeerIdentity gp[get_length + 1];
  GNUNET_memcpy (gp,
		 get_path,
		 get_length * sizeof (struct GNUNET_PeerIdentity));
  gp[get_length] = my_identity;
  get_length = get_length + 1;
  GDS_CLIENTS_process_get (get->options,
			   get->block_type,
			   hop_count,
                           get->desired_replication_level,
			   get->get_path_length,
                           gp,
			   &get->key);


  successor = find_local_best_known_next_hop (key_value,
					      GDS_FINGER_TYPE_NON_PREDECESSOR);
  next_hop = successor.next_hop;
  best_known_dest = successor.best_known_destination;
  intermediate_trail_id = successor.trail_id;
  /* I am not the final destination. I am part of trail to reach final dest. */
  if (0 != (GNUNET_CRYPTO_cmp_peer_identity (&current_best_known_dest, &my_identity)))
  {
    next_routing_hop = GDS_ROUTING_get_next_hop (&received_intermediate_trail_id,
						 GDS_ROUTING_SRC_TO_DEST);
    if (NULL != next_routing_hop)
    {
      next_hop = *next_routing_hop;
      best_known_dest = current_best_known_dest;
      intermediate_trail_id = received_intermediate_trail_id;
    }
  }

  /* I am the final destination. */
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (&my_identity,
					    &best_known_dest))
  {
    if (1 == get_length)
    {
      DEBUG ("\n GET_REQUEST DONE for key = %s",
	     GNUNET_h2s(&get->key));
      GDS_DATACACHE_handle_get (&get->key,
				get->block_type, /* FIXME: endianess? */
				NULL,
				0,
                                NULL,
				&get_cb,
                                NULL);
    }
    else
    {
      GDS_DATACACHE_handle_get (&get->key,
				get->block_type, /* FIXME: endianess? */
				NULL,
				0,
				NULL,
                                &get_cb,
                                &gp[get_length - 2]);
    }
  }
  else
  {
    GDS_NEIGHBOURS_send_get (&get->key,
			     get->block_type, /* FIXME: endianess? */
			     get->options,
                             get->desired_replication_level,
			     &best_known_dest,
                             &intermediate_trail_id,
			     &next_hop,
			     hop_count,
                             get_length,
			     gp);
  }
}


/**
 * Check validity of @a get_result message.
 *
 * @param cls closure
 * @param get_result the message
 * @return #GNUNET_OK if @a get_result is well-formed
 */
static int
check_dht_p2p_get_result (void *cls,
			  const struct PeerGetResultMessage *get_result)
{
  size_t msize;
  unsigned int getlen;
  unsigned int putlen;

  msize = ntohs (get_result->header.size);
  getlen = ntohl (get_result->get_path_length);
  putlen = ntohl (get_result->put_path_length);
  if ((msize <
       sizeof (struct PeerGetResultMessage) +
       getlen * sizeof (struct GNUNET_PeerIdentity) +
       putlen * sizeof (struct GNUNET_PeerIdentity)) ||
      (getlen >
       GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE / sizeof (struct GNUNET_PeerIdentity) ||
      (putlen >
         GNUNET_CONSTANTS_MAX_ENCRYPTED_MESSAGE_SIZE / sizeof (struct GNUNET_PeerIdentity))))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Core handler for get result
 *
 * @param cls closure
 * @param get_result the message
 */
static void
handle_dht_p2p_get_result (void *cls,
                           const struct PeerGetResultMessage *get_result)
{
  const struct GNUNET_PeerIdentity *get_path;
  const struct GNUNET_PeerIdentity *put_path;
  const void *payload;
  size_t payload_size;
  size_t msize;
  unsigned int getlen;
  unsigned int putlen;
  int current_path_index;

  msize = ntohs (get_result->header.size);
  getlen = ntohl (get_result->get_path_length);
  putlen = ntohl (get_result->put_path_length);
  DEBUG ("GET_RESULT  FOR DATA_SIZE = %u\n",
	 (unsigned int) msize);
  GNUNET_STATISTICS_update (GDS_stats,
                            gettext_noop ("# Bytes received from other peers"),
			    msize,
                            GNUNET_NO);
  put_path = (const struct GNUNET_PeerIdentity *) &get_result[1];
  get_path = &put_path[putlen];
  payload = (const void *) &get_path[getlen];
  payload_size = msize - (sizeof (struct PeerGetResultMessage) +
                         (getlen + putlen) * sizeof (struct GNUNET_PeerIdentity));

  if (0 == (GNUNET_CRYPTO_cmp_peer_identity (&my_identity,
					     &get_path[0])))
  {
    GDS_CLIENTS_handle_reply (GNUNET_TIME_absolute_ntoh (get_result->expiration_time),
                              &get_result->key,
                              getlen,
			      get_path,
			      putlen,
                              put_path,
			      get_result->type,
			      payload_size,
			      payload);
    return;
  }
  current_path_index = search_my_index (get_path,
					getlen);
  if (-1 == current_path_index)
  {
    DEBUG ("No entry found in get path.\n");
    GNUNET_break (0);
    return;
  }
  if ((getlen + 1) == current_path_index)
  {
    DEBUG("Present twice in get path. Not allowed. \n");
    GNUNET_break (0);
    return;
  }
  GDS_NEIGHBOURS_send_get_result (&get_result->key,
				  get_result->type, /* FIXME: endianess? */
				  &get_path[current_path_index - 1],
				  &get_result->querying_peer,
				  putlen,
				  put_path,
				  getlen,
				  get_path,
				  GNUNET_TIME_absolute_ntoh (get_result->expiration_time),
				  payload,
				  payload_size);
}


/**
 * Find the next hop to pass trail setup message. First find the local best known
 * hop from your own identity, friends and finger. If you were part of trail,
 * then get the next hop from routing table. Compare next_hop from routing table
 * and local best known hop, and return the closest one to final_dest_finger_val
 * @param final_dest_finger_val 64 bit value of finger identity
 * @param intermediate_trail_id If you are part of trail to reach to some other
 *                              finger, then it is the trail id to reach to
 *                              that finger, else set to 0.
 * @param is_predecessor Are we looking for closest successor or predecessor.
 * @param source Source of trail setup message.
 * @param current_dest In case you are part of trail, then finger to which
 *                     we should forward the message. Else my own identity
 * @return Closest Peer for @a final_dest_finger_val
 */
static struct Closest_Peer
get_local_best_known_next_hop (uint64_t final_dest_finger_val,
                               const struct GNUNET_HashCode *intermediate_trail_id,
                               unsigned int is_predecessor,
                               const struct GNUNET_PeerIdentity *source,
                               const struct GNUNET_PeerIdentity *current_dest)
{
  struct Closest_Peer peer;

  peer = find_local_best_known_next_hop (final_dest_finger_val,
					 is_predecessor);

  /* Am I just a part of a trail towards a finger (current_destination)? */
  if (0 != GNUNET_CRYPTO_cmp_peer_identity (&my_identity,
					    current_dest) &&
      0 != GNUNET_CRYPTO_cmp_peer_identity (&peer.best_known_destination,
                                            current_dest))
  {
    const struct GNUNET_PeerIdentity *closest_peer;

    /* Select best successor among one found locally and current_destination
     * that we got from network.*/
    closest_peer = select_closest_peer (&peer.best_known_destination,
                                        current_dest,
                                        final_dest_finger_val,
                                        is_predecessor);

    /* Is current dest (end point of the trail of which I am a part) closest_peer? */
    if (0 == GNUNET_CRYPTO_cmp_peer_identity (current_dest,
					      closest_peer))
    {
      const struct GNUNET_PeerIdentity *next_hop;

      next_hop = GDS_ROUTING_get_next_hop (intermediate_trail_id,
                                           GDS_ROUTING_SRC_TO_DEST);
      /* next_hop NULL is a valid case. This intermediate trail id is set by
       some other finger, and while this trail setup is in progress, that other
       peer might have found a better trail ,and send trail teardown message
       across the network. In case we got the trail teardown message first,
       then next_hop will be NULL. A possible solution could be to keep track
       * of all removed trail id, and be sure that there is no other reason . */
      if(NULL != next_hop)
      {
         peer.next_hop = *next_hop;
         peer.best_known_destination =  *current_dest;
         peer.trail_id = *intermediate_trail_id;
      }
    }
  }
  return peer;
}


/**
 * Check format of a PeerTrailSetupMessage.
 *
 * @param cls closure
 * @param trail_setup the message
 * @return #GNUNET_OK if @a trail_setup is well-formed
 */
static int
check_dht_p2p_trail_setup (void *cls,
                            const struct PeerTrailSetupMessage *trail_setup)
{
  size_t msize;

  msize = ntohs (trail_setup->header.size);
  if ((msize - sizeof (struct PeerTrailSetupMessage)) %
      sizeof (struct GNUNET_PeerIdentity) != 0)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Core handle for PeerTrailSetupMessage.
 *
 * @param cls closure
 * @param trail_setup the message
 */
static void
handle_dht_p2p_trail_setup (void *cls,
                            const struct PeerTrailSetupMessage *trail_setup)
{
  struct FriendInfo *friend = cls;
  const struct GNUNET_PeerIdentity *trail_peer_list;
  struct GNUNET_PeerIdentity current_dest;
  struct FriendInfo *target_friend;
  struct GNUNET_PeerIdentity source;
  struct GNUNET_HashCode intermediate_trail_id;
  struct GNUNET_HashCode trail_id;
  unsigned int is_predecessor;
  uint32_t trail_length;
  uint64_t final_dest_finger_val;
  int i;
  size_t msize;

  msize = ntohs (trail_setup->header.size);
  trail_length = (msize - sizeof (struct PeerTrailSetupMessage))/
                  sizeof (struct GNUNET_PeerIdentity);
  GNUNET_STATISTICS_update (GDS_stats,
                            gettext_noop ("# Bytes received from other peers"),
			    msize,
                            GNUNET_NO);
  trail_peer_list = (const struct GNUNET_PeerIdentity *) &trail_setup[1];
  current_dest = trail_setup->best_known_destination;
  trail_id = trail_setup->trail_id;
  final_dest_finger_val
    = GNUNET_ntohll (trail_setup->final_destination_finger_value);
  source = trail_setup->source_peer;
  is_predecessor = ntohl (trail_setup->is_predecessor);
  intermediate_trail_id = trail_setup->intermediate_trail_id;

  /* Did the friend insert its ID in the trail list? */
  if ( (trail_length > 0) &&
       (0 != memcmp (&trail_peer_list[trail_length-1],
		     friend->id,
		     sizeof (struct GNUNET_PeerIdentity))) )
  {
    GNUNET_break_op (0);
    return;
  }

   /* If I was the source and got the message back, then set trail length to 0.*/
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (&my_identity,
					    &source))
  {
    trail_length = 0;
  }

  /* Check if you are present in the trail seen so far? */
  for (i = 0; i < trail_length; i++)
  {
    if (0 == GNUNET_CRYPTO_cmp_peer_identity (&trail_peer_list[i],
					      &my_identity))
    {
      /* We will add ourself later in code, if NOT destination. */
      trail_length = i;
      break;
    }
  }

  /* Is my routing table full?  */
  if (GNUNET_YES == GDS_ROUTING_threshold_reached ())
  {
    target_friend
      = (trail_length > 0)
      ? GNUNET_CONTAINER_multipeermap_get (friend_peermap,
					   &trail_peer_list[trail_length - 1])
      : GNUNET_CONTAINER_multipeermap_get (friend_peermap,
					   &source);
    if (NULL == target_friend)
    {
      DEBUG ("\n friend not found");
      GNUNET_break(0);
      return;
    }
    GDS_NEIGHBOURS_send_trail_rejection (&source,
					 final_dest_finger_val,
                                         &my_identity,
					 is_predecessor,
                                         trail_peer_list,
					 trail_length,
                                         &trail_id,
					 target_friend,
                                         CONGESTION_TIMEOUT);
    return;
  }

  /* Get the next hop to forward the trail setup request. */
  struct Closest_Peer next_peer
    = get_local_best_known_next_hop (final_dest_finger_val,
				     &intermediate_trail_id,
				     is_predecessor,
				     &source,
				     &current_dest);

  /* Am I the final destination? */
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (&next_peer.best_known_destination,
					    &my_identity))
  {
    if (0 == GNUNET_CRYPTO_cmp_peer_identity (&source,
					      &my_identity))
    {
      finger_table_add (&my_identity,
			NULL,
			0,
			is_predecessor,
                        final_dest_finger_val,
			&trail_id);
      return;
    }

    target_friend
      = (trail_length > 0)
      ? GNUNET_CONTAINER_multipeermap_get (friend_peermap,
					   &trail_peer_list[trail_length-1])
      : GNUNET_CONTAINER_multipeermap_get (friend_peermap,
					   &source);
    if (NULL == target_friend)
    {
      GNUNET_break_op (0);
      return;
    }
    GDS_ROUTING_add (&trail_id,
		     target_friend->id,
		     &my_identity);
    GDS_NEIGHBOURS_send_trail_setup_result (&source,
                                            &my_identity,
                                            target_friend,
					    trail_length,
                                            trail_peer_list,
                                            is_predecessor,
                                            final_dest_finger_val,
					    &trail_id);
    return;
  }
  /* I'm not the final destination. */
  target_friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						     &next_peer.next_hop);
  if (NULL == target_friend)
  {
    DEBUG ("\n target friend not found for peer = %s",
	   GNUNET_i2s(&next_peer.next_hop));
    GNUNET_break (0);
    return;
  }
  if (0 != GNUNET_CRYPTO_cmp_peer_identity (&my_identity,
					    &source))
  {
    /* Add yourself to list of peers. */
    struct GNUNET_PeerIdentity peer_list[trail_length + 1];

    GNUNET_memcpy (peer_list,
		   trail_peer_list,
		   trail_length * sizeof (struct GNUNET_PeerIdentity));
    peer_list[trail_length] = my_identity;
    GDS_NEIGHBOURS_send_trail_setup (&source,
				     final_dest_finger_val,
				     &next_peer.best_known_destination,
				     target_friend,
				     trail_length + 1,
				     peer_list,
				     is_predecessor,
				     &trail_id,
				     &next_peer.trail_id);
    return;
  }
  GDS_NEIGHBOURS_send_trail_setup (&source,
				   final_dest_finger_val,
				   &next_peer.best_known_destination,
				   target_friend,
				   0,
				   NULL,
				   is_predecessor,
				   &trail_id,
				   &next_peer.trail_id);
}


/**
 * Validate format of trail setup result messages.
 *
 * @param closure
 * @param trail_result the message
 * @return #GNUNET_OK if @a trail_result is well-formed
 */
static int
check_dht_p2p_trail_setup_result (void *cls,
				  const struct PeerTrailSetupResultMessage *trail_result)
{
  size_t msize;

  msize = ntohs (trail_result->header.size);
  if ((msize - sizeof (struct PeerTrailSetupResultMessage)) %
      sizeof (struct GNUNET_PeerIdentity) != 0)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Core handle for p2p trail setup result messages.
 *
 * @param closure
 * @param trail_result the message
 */
static void
handle_dht_p2p_trail_setup_result (void *cls,
				   const struct PeerTrailSetupResultMessage *trail_result)
{
  struct FriendInfo *friend = cls;
  const struct GNUNET_PeerIdentity *trail_peer_list;
  struct GNUNET_PeerIdentity next_hop;
  struct FriendInfo *target_friend;
  struct GNUNET_PeerIdentity querying_peer;
  struct GNUNET_PeerIdentity finger_identity;
  uint32_t trail_length;
  uint64_t ultimate_destination_finger_value;
  uint32_t is_predecessor;
  struct GNUNET_HashCode trail_id;
  int my_index;
  size_t msize;

  msize = ntohs (trail_result->header.size);
  trail_length = (msize - sizeof (struct PeerTrailSetupResultMessage))/
                  sizeof (struct GNUNET_PeerIdentity);

  GNUNET_STATISTICS_update (GDS_stats,
                            gettext_noop ("# Bytes received from other peers"),
			    msize,
                            GNUNET_NO);

  is_predecessor = ntohl (trail_result->is_predecessor);
  querying_peer = trail_result->querying_peer;
  finger_identity = trail_result->finger_identity;
  trail_id = trail_result->trail_id;
  trail_peer_list = (const struct GNUNET_PeerIdentity *) &trail_result[1];
  ultimate_destination_finger_value
    = GNUNET_ntohll (trail_result->ultimate_destination_finger_value);

  /* Am I the one who initiated the query? */
  if (0 == (GNUNET_CRYPTO_cmp_peer_identity (&querying_peer,
					     &my_identity)))
  {
    /* Check that you got the message from the correct peer. */
    if (trail_length > 0)
    {
      GNUNET_assert (0 == GNUNET_CRYPTO_cmp_peer_identity (&trail_peer_list[0],
							   friend->id));
    }
    else
    {
      GNUNET_assert (0 == GNUNET_CRYPTO_cmp_peer_identity (&finger_identity,
							   friend->id));
    }
    GDS_ROUTING_add (&trail_id,
		     &my_identity,
		     friend->id);
    finger_table_add (&finger_identity,
		      trail_peer_list,
		      trail_length,
                      is_predecessor,
		      ultimate_destination_finger_value,
		      &trail_id);
    return;
  }

  /* Get my location in the trail. */
  my_index = search_my_index (trail_peer_list,
			      trail_length);
  if (-1 == my_index)
  {
    DEBUG ("Not found in trail\n");
    GNUNET_break_op(0);
    return;
  }
  //TODO; return -2.
  if ((trail_length + 1) == my_index)
  {
    DEBUG ("Found twice in trail.\n");
    GNUNET_break_op(0);
    return;
  }

  //TODO; Refactor code here and above to check if sender peer is correct
  if (my_index == 0)
  {
    if (trail_length > 1)
      GNUNET_assert (0 == GNUNET_CRYPTO_cmp_peer_identity (&trail_peer_list[1],
							   friend->id));
    else
      GNUNET_assert (0 == GNUNET_CRYPTO_cmp_peer_identity (&finger_identity,
							   friend->id));
    next_hop = trail_result->querying_peer;
  }
  else
  {
    if (my_index == trail_length - 1)
    {
      GNUNET_assert (0 ==
		     GNUNET_CRYPTO_cmp_peer_identity (&finger_identity,
						      friend->id));
    }
    else
      GNUNET_assert (0 ==
		     GNUNET_CRYPTO_cmp_peer_identity (&trail_peer_list[my_index + 1],
                                                      friend->id));
    next_hop = trail_peer_list[my_index - 1];
  }

  target_friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						     &next_hop);
  if (NULL == target_friend)
  {
    GNUNET_break_op (0);
    return;
  }
  GDS_ROUTING_add (&trail_id,
		   &next_hop,
		   friend->id);
  GDS_NEIGHBOURS_send_trail_setup_result (&querying_peer,
					  &finger_identity,
                                          target_friend,
					  trail_length,
					  trail_peer_list,
                                          is_predecessor,
                                          ultimate_destination_finger_value,
                                          &trail_id);
}


/**
 * Invert the trail.
 *
 * @param trail Trail to be inverted
 * @param trail_length Total number of peers in the trail.
 * @return Updated trail
 */
static struct GNUNET_PeerIdentity *
invert_trail (const struct GNUNET_PeerIdentity *trail,
              unsigned int trail_length)
{
  int i;
  int j;
  struct GNUNET_PeerIdentity *inverted_trail;

  inverted_trail = GNUNET_new_array (trail_length,
				     struct GNUNET_PeerIdentity);
  i = 0;
  j = trail_length - 1;
  while (i < trail_length)
  {
    inverted_trail[i] = trail[j];
    i++;
    j--;
  }

  GNUNET_assert (NULL !=
		 GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						    &inverted_trail[0]));
  return inverted_trail;
}


/**
 * Return the shortest trail among all the trails to reach to finger from me.
 *
 * @param finger Finger
 * @param shortest_trail_length[out] Trail length of shortest trail from me
 *                                   to @a finger
 * @return Shortest trail.
 */
static struct GNUNET_PeerIdentity *
get_shortest_trail (struct FingerInfo *finger,
                    unsigned int *trail_length)
{
  struct Trail *trail;
  unsigned int flag = 0;
  unsigned int shortest_trail_index = 0;
  int shortest_trail_length = -1;
  struct Trail_Element *trail_element;
  struct GNUNET_PeerIdentity *trail_list;
  unsigned int i;

  /* Get the shortest trail to reach to current successor. */
  for (i = 0; i < finger->trails_count; i++)
  {
    trail = &finger->trail_list[i];

    if (0 == flag)
    {
      shortest_trail_index = i;
      shortest_trail_length = trail->trail_length;
      flag = 1;
      continue;
    }

    if (shortest_trail_length > trail->trail_length)
    {
      shortest_trail_index = i;
      shortest_trail_length = trail->trail_length;
    }
    continue;
  }

  /* Copy the shortest trail and return. */
  trail = &finger->trail_list[shortest_trail_index];
  trail_element = trail->trail_head;

  trail_list = GNUNET_new_array (shortest_trail_length,
				 struct GNUNET_PeerIdentity);

  for (i = 0; i < shortest_trail_length; i++,trail_element = trail_element->next)
  {
    trail_list[i] = trail_element->peer;
  }

  GNUNET_assert(shortest_trail_length != -1);

  *trail_length = shortest_trail_length;
  return trail_list;
}


/**
 * Check if @a trail_1 and @a trail_2 have any common element. If yes then join
 * them at common element. @a trail_1 always preceeds @a trail_2 in joined trail.
 *
 * @param trail_1 Trail from source to me, NOT including endpoints.
 * @param trail_1_len Total number of peers @a trail_1
 * @param trail_2 Trail from me to current predecessor, NOT including endpoints.
 * @param trail_2_len Total number of peers @a trail_2
 * @param joined_trail_len Total number of peers in combined trail of @a trail_1
 *                          @a trail_2.
 * @return Joined trail.
 */
static struct GNUNET_PeerIdentity *
check_for_duplicate_entries (const struct GNUNET_PeerIdentity *trail_1,
                             unsigned int trail_1_len,
                             const struct GNUNET_PeerIdentity *trail_2,
                             unsigned int trail_2_len,
                             unsigned int *joined_trail_len)
{
  struct GNUNET_PeerIdentity *joined_trail;
  unsigned int i;
  unsigned int j;
  unsigned int k;

  for (i = 0; i < trail_1_len; i++)
  {
    for (j = 0; j < trail_2_len; j++)
    {
      if (0 != GNUNET_CRYPTO_cmp_peer_identity (&trail_1[i],
						&trail_2[j]))
        continue;

      *joined_trail_len = i + (trail_2_len - j);
      joined_trail = GNUNET_new_array (*joined_trail_len,
				       struct GNUNET_PeerIdentity);


      /* Copy all the elements from 0 to i into joined_trail. */
      for(k = 0; k < ( i+1); k++)
      {
        joined_trail[k] = trail_1[k];
      }

      /* Increment j as entry stored is same as entry stored at i*/
      j = j+1;

      /* Copy all the elements from j to trail_2_len-1 to joined trail.*/
      while(k <= (*joined_trail_len - 1))
      {
        joined_trail[k] = trail_2[j];
        j++;
        k++;
      }

      return joined_trail;
    }
  }

  /* Here you should join the  trails. */
  *joined_trail_len = trail_1_len + trail_2_len + 1;
  joined_trail = GNUNET_new_array (*joined_trail_len,
				   struct GNUNET_PeerIdentity);


  for(i = 0; i < trail_1_len;i++)
  {
    joined_trail[i] = trail_1[i];
  }

  joined_trail[i] = my_identity;
  i++;

  for (j = 0; i < *joined_trail_len; i++,j++)
  {
    joined_trail[i] = trail_2[j];
  }

  return joined_trail;
}


/**
 * Return the trail from source to my current predecessor. Check if source
 * is already part of the this trail, if yes then return the shorten trail.
 *
 * @param current_trail Trail from source to me, NOT including the endpoints.
 * @param current_trail_length Number of peers in @a current_trail.
 * @param trail_src_to_curr_pred_length[out] Number of peers in trail from
 *                                           source to my predecessor, NOT including
 *                                           the endpoints.
 * @return Trail from source to my predecessor.
 */
static struct GNUNET_PeerIdentity *
get_trail_src_to_curr_pred (struct GNUNET_PeerIdentity source_peer,
                            const struct GNUNET_PeerIdentity *trail_src_to_me,
                            unsigned int trail_src_to_me_len,
                            unsigned int *trail_src_to_curr_pred_length)
{
  struct GNUNET_PeerIdentity *trail_me_to_curr_pred;
  struct GNUNET_PeerIdentity *trail_src_to_curr_pred;
  unsigned int trail_me_to_curr_pred_length;
  struct FingerInfo *current_predecessor;
  int i;
  unsigned int j;
  unsigned int len;

  current_predecessor = &finger_table[PREDECESSOR_FINGER_ID];

  /* Check if trail_src_to_me contains current_predecessor. */
  for (i = 0; i < trail_src_to_me_len; i++)
  {
    if (0 != GNUNET_CRYPTO_cmp_peer_identity(&trail_src_to_me[i],
                                             &current_predecessor->finger_identity))
      continue;


    *trail_src_to_curr_pred_length = i;

    if(0 == i)
      return NULL;

    trail_src_to_curr_pred = GNUNET_new_array (*trail_src_to_curr_pred_length,
					       struct GNUNET_PeerIdentity);
     for (j = 0; j < i; j++)
       trail_src_to_curr_pred[j] = trail_src_to_me[j];
     return trail_src_to_curr_pred;
  }


  trail_me_to_curr_pred = get_shortest_trail (current_predecessor,
                                              &trail_me_to_curr_pred_length);

  /* Check if trail contains the source_peer. */
  for (i = trail_me_to_curr_pred_length - 1; i >= 0; i--)
  {
    if (0 != GNUNET_CRYPTO_cmp_peer_identity (&source_peer,
                                              &trail_me_to_curr_pred[i]))
      continue;

    /* Source is NOT part of trail. */
    i++;

    /* Source is the last element in the trail to reach to my pred.
       Source is direct friend of the pred. */
    if (trail_me_to_curr_pred_length == i)
    {
      *trail_src_to_curr_pred_length = 0;
      GNUNET_free_non_null (trail_me_to_curr_pred);
      return NULL;
    }

    *trail_src_to_curr_pred_length = trail_me_to_curr_pred_length - i;
    trail_src_to_curr_pred = GNUNET_new_array (*trail_src_to_curr_pred_length,
					       struct GNUNET_PeerIdentity);


    for (j = 0; j < *trail_src_to_curr_pred_length; i++,j++)
      trail_src_to_curr_pred[j] = trail_me_to_curr_pred[i];
    GNUNET_free_non_null (trail_me_to_curr_pred);
    return trail_src_to_curr_pred;
  }

  trail_src_to_curr_pred = check_for_duplicate_entries (trail_src_to_me,
                                                        trail_src_to_me_len,
                                                        trail_me_to_curr_pred,
                                                        trail_me_to_curr_pred_length,
                                                        &len);
  *trail_src_to_curr_pred_length = len;
  GNUNET_free_non_null(trail_me_to_curr_pred);
  return trail_src_to_curr_pred;
}


/**
 * Add finger as your predecessor. To add, first generate a new trail id, invert
 * the trail to get the trail from me to finger, add an entry in your routing
 * table, send add trail message to peers which are part of trail from me to
 * finger and add finger in finger table.
 *
 * @param finger
 * @param trail
 * @param trail_length
 */
static void
update_predecessor (const struct GNUNET_PeerIdentity *finger,
                    const struct GNUNET_PeerIdentity *trail,
                    unsigned int trail_length)
{
  struct GNUNET_HashCode trail_to_new_predecessor_id;
  struct GNUNET_PeerIdentity *trail_to_new_predecessor;
  struct FriendInfo *target_friend;

  /* Generate trail id for trail from me to new predecessor = finger. */
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_STRONG,
                              &trail_to_new_predecessor_id,
                              sizeof (trail_to_new_predecessor_id));

  if (0 == trail_length)
  {
    trail_to_new_predecessor = NULL;
    GDS_ROUTING_add (&trail_to_new_predecessor_id,
		     &my_identity,
		     finger);
    target_friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						       finger);
    if (NULL == target_friend)
    {
      GNUNET_break (0);
      return;
    }
  }
  else
  {
    /* Invert the trail to get the trail from me to finger, NOT including the
       endpoints.*/
    GNUNET_assert(NULL != GNUNET_CONTAINER_multipeermap_get (friend_peermap,
							     &trail[trail_length-1]));
    trail_to_new_predecessor = invert_trail (trail,
					     trail_length);

    /* Add an entry in your routing table. */
    GDS_ROUTING_add (&trail_to_new_predecessor_id,
                     &my_identity,
                     &trail_to_new_predecessor[0]);

    GNUNET_assert (NULL != (target_friend =
                   GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                      &trail_to_new_predecessor[0])));
  }

  /* Add entry in routing table of all peers that are part of trail from me
     to finger, including finger. */
  GDS_NEIGHBOURS_send_add_trail (&my_identity,
                                 finger,
                                 &trail_to_new_predecessor_id,
                                 trail_to_new_predecessor,
                                 trail_length,
                                 target_friend);

  add_new_finger (finger,
		  trail_to_new_predecessor,
		  trail_length,
                  &trail_to_new_predecessor_id,
		  PREDECESSOR_FINGER_ID);
  GNUNET_free_non_null (trail_to_new_predecessor);
}


/**
 * Check if you already have a predecessor. If not then add finger as your
 * predecessor. If you have predecessor, then compare two peer identites.
 * If finger is correct predecessor, then remove the old entry, add finger in
 * finger table and send add_trail message to add the trail in the routing
 * table of all peers which are part of trail to reach from me to finger.
 * @param finger New peer which may be our predecessor.
 * @param trail List of peers to reach from @finger to me.
 * @param trail_length Total number of peer in @a trail.
 */
static void
compare_and_update_predecessor (const struct GNUNET_PeerIdentity *finger,
                                const struct GNUNET_PeerIdentity *trail,
                                unsigned int trail_length)
{
  struct FingerInfo *current_predecessor;
  const struct GNUNET_PeerIdentity *closest_peer;
  uint64_t predecessor_value;
  unsigned int is_predecessor = 1;

  current_predecessor = &finger_table[PREDECESSOR_FINGER_ID];
  GNUNET_assert (0 != GNUNET_CRYPTO_cmp_peer_identity (finger,
						       &my_identity));

  /* No predecessor. Add finger as your predecessor. */
  if (GNUNET_NO == current_predecessor->is_present)
  {
    update_predecessor (finger,
			trail,
			trail_length);
    return;
  }

  if (0 == GNUNET_CRYPTO_cmp_peer_identity (&current_predecessor->finger_identity,
                                            finger))
  {
    return;
  }

  predecessor_value = compute_finger_identity_value (PREDECESSOR_FINGER_ID);
  closest_peer = select_closest_peer (finger,
                                      &current_predecessor->finger_identity,
                                      predecessor_value,
				      is_predecessor);

  /* Finger is the closest predecessor. Remove the existing one and add the new
     one. */
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (closest_peer,
					    finger))
  {
    remove_existing_finger (current_predecessor,
			    PREDECESSOR_FINGER_ID);
    update_predecessor (finger,
			trail,
			trail_length);
    return;
  }
}


/**
 * Check format of a p2p verify successor messages.
 *
 * @param cls closure
 * @param vsm the message
 * @return #GNUNET_OK if @a vsm is well-formed
 */
static int
check_dht_p2p_verify_successor (void *cls,
				const struct PeerVerifySuccessorMessage *vsm)
{
  size_t msize;

  msize = ntohs (vsm->header.size);
  if ((msize - sizeof (struct PeerVerifySuccessorMessage)) %
      sizeof (struct GNUNET_PeerIdentity) != 0)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Core handle for p2p verify successor messages.
 *
 * @param cls closure
 * @param vsm the message
 */
static void
handle_dht_p2p_verify_successor (void *cls,
				 const struct PeerVerifySuccessorMessage *vsm)
{
  struct FriendInfo *friend = cls;
  struct GNUNET_HashCode trail_id;
  struct GNUNET_PeerIdentity successor;
  struct GNUNET_PeerIdentity source_peer;
  struct GNUNET_PeerIdentity *trail;
  const struct GNUNET_PeerIdentity *next_hop;
  struct FingerInfo current_predecessor;
  struct FriendInfo *target_friend;
  unsigned int trail_src_to_curr_pred_len = 0;
  struct GNUNET_PeerIdentity *trail_src_to_curr_pred;
  unsigned int trail_length;
  size_t msize;

  msize = ntohs (vsm->header.size);
  trail_length = (msize - sizeof (struct PeerVerifySuccessorMessage))/
                  sizeof (struct GNUNET_PeerIdentity);
  GNUNET_STATISTICS_update (GDS_stats,
                            gettext_noop ("# Bytes received from other peers"),
			    msize,
                            GNUNET_NO);

  trail_id = vsm->trail_id;
  source_peer = vsm->source_peer;
  successor = vsm->successor;
  trail = (struct GNUNET_PeerIdentity *)&vsm[1];

  /* I am NOT the successor of source_peer. Pass the message to next_hop on
   * the trail. */
  if (0 != (GNUNET_CRYPTO_cmp_peer_identity (&successor,
					     &my_identity)))
  {
    next_hop = GDS_ROUTING_get_next_hop (&trail_id,
					 GDS_ROUTING_SRC_TO_DEST);
    if (NULL == next_hop)
      return;

    target_friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						       next_hop);
    if (NULL == target_friend)
    {
      GNUNET_break_op(0);
      return;
    }
    GDS_NEIGHBOURS_send_verify_successor_message (&source_peer,
						  &successor,
                                                  &trail_id,
						  trail,
						  trail_length,
                                                  target_friend);
    return;
  }

  /* I am the destination of this message. */
  /* Check if the source_peer could be our predecessor and if yes then update
   * it.  */
  compare_and_update_predecessor (&source_peer,
				  trail,
				  trail_length);
  current_predecessor = finger_table[PREDECESSOR_FINGER_ID];

  /* Is source of this message NOT my predecessor. */
  if (0 != (GNUNET_CRYPTO_cmp_peer_identity (&current_predecessor.finger_identity,
                                             &source_peer)))
  {
    trail_src_to_curr_pred
      = get_trail_src_to_curr_pred (source_peer,
				    trail,
				    trail_length,
				    &trail_src_to_curr_pred_len);
  }
  else
  {
    trail_src_to_curr_pred_len = trail_length;
    trail_src_to_curr_pred = GNUNET_new_array (trail_src_to_curr_pred_len,
					       struct GNUNET_PeerIdentity);

    for (unsigned int i = 0; i < trail_src_to_curr_pred_len; i++)
    {
      trail_src_to_curr_pred[i] = trail[i];
    }
  }

  GNUNET_assert (NULL !=
                (target_friend =
                 GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						    friend->id)));
  GDS_NEIGHBOURS_send_verify_successor_result (&source_peer,
					       &my_identity,
                                               &current_predecessor.finger_identity,
                                               &trail_id,
					       trail_src_to_curr_pred,
                                               trail_src_to_curr_pred_len,
                                               GDS_ROUTING_DEST_TO_SRC,
                                               target_friend);
  GNUNET_free_non_null (trail_src_to_curr_pred);
}


/**
 * If the trail from me to my probable successor contains a friend not
 * at index 0, then we can shorten the trail.
 *
 * @param probable_successor Peer which is our probable successor
 * @param trail_me_to_probable_successor Peers in path from me to my probable
 *                                       successor, NOT including the endpoints.
 * @param trail_me_to_probable_successor_len Total number of peers in
 *                                           @a trail_me_to_probable_succesor.
 * @return Updated trail, if any friend found.
 *         Else the trail_me_to_probable_successor.
 */
const struct GNUNET_PeerIdentity *
check_trail_me_to_probable_succ (const struct GNUNET_PeerIdentity *probable_successor,
                                 const struct GNUNET_PeerIdentity *trail_me_to_probable_successor,
                                 unsigned int trail_me_to_probable_successor_len,
                                 unsigned int *trail_to_new_successor_length)
{
  unsigned int i;
  unsigned int j;
  struct GNUNET_PeerIdentity *trail_to_new_successor;

  /* Probable successor is  a friend */
  if (NULL != GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                 probable_successor))
  {
    trail_to_new_successor = NULL;
    *trail_to_new_successor_length = 0;
    return trail_to_new_successor;
  }

  /* Is there any friend of yours in this trail. */
  if (trail_me_to_probable_successor_len > 1)
  {
    for (i = trail_me_to_probable_successor_len - 1; i > 0; i--)
    {
      if (NULL == GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                     &trail_me_to_probable_successor[i]))
        continue;

      *trail_to_new_successor_length = (trail_me_to_probable_successor_len - i);
      trail_to_new_successor = GNUNET_new_array (*trail_to_new_successor_length,
						 struct GNUNET_PeerIdentity);
      for (j = 0; j < *trail_to_new_successor_length; i++,j++)
      {
        trail_to_new_successor[j] = trail_me_to_probable_successor[i];
      }

      return trail_to_new_successor;
    }
  }

  *trail_to_new_successor_length = trail_me_to_probable_successor_len;
  return trail_me_to_probable_successor;
}


// TODO: Move up
struct SendNotifyContext
{
  struct GNUNET_PeerIdentity source_peer;
  struct GNUNET_PeerIdentity successor;
  struct GNUNET_PeerIdentity *successor_trail;
  unsigned int successor_trail_length;
  struct GNUNET_HashCode succesor_trail_id;
  struct FriendInfo *target_friend;
  unsigned int num_retries_scheduled;
};


void
send_notify_new_successor (void *cls);


/**
 * Check if the peer which sent us verify successor result message is still ours
 * successor or not. If not, then compare existing successor and probable successor.
 * In case probable successor is the correct successor, remove the existing
 * successor. Add probable successor as new successor. Send notify new successor
 * message to new successor.
 * @param curr_succ Peer to which we sent the verify successor message. It may
 * or may not be our real current successor, as we may have few iterations of
 * find finger trail task.
 * @param probable_successor Peer which should be our successor accroding to @a
 *                           curr_succ
 * @param trail List of peers to reach from me to @a probable successor, NOT including
 *              endpoints.
 * @param trail_length Total number of peers in @a trail.
 */
static void
compare_and_update_successor (const struct GNUNET_PeerIdentity *curr_succ,
                              const struct GNUNET_PeerIdentity *probable_successor,
                              const struct GNUNET_PeerIdentity *trail,
                              unsigned int trail_length)
{
  struct FingerInfo *current_successor;
  const struct GNUNET_PeerIdentity *closest_peer;
  struct GNUNET_HashCode trail_id;
  const struct GNUNET_PeerIdentity *trail_me_to_probable_succ;
  struct FriendInfo *target_friend;
  unsigned int trail_me_to_probable_succ_len;
  unsigned int is_predecessor = 0;
  uint64_t successor_value;
  struct SendNotifyContext *notify_ctx;

  current_successor = &finger_table[0];
  successor_value = compute_finger_identity_value(0);

  /* If probable successor is same as current_successor, do nothing. */
  if(0 == GNUNET_CRYPTO_cmp_peer_identity (probable_successor,
                                           &current_successor->finger_identity))
  {
    if ((NULL != GDS_stats))
    {
      char *my_id_str;
      uint64_t succ;
      char *key;
      uint64_t my_id;
      GNUNET_memcpy (&my_id, &my_identity, sizeof(uint64_t));
      my_id_str = GNUNET_strdup (GNUNET_i2s_full (&my_identity));
      GNUNET_memcpy (&succ,
		     &current_successor->finger_identity,
		     sizeof(uint64_t));
      succ = GNUNET_ntohll(succ);
      GNUNET_asprintf (&key,
		       "XDHT:%s:",
		       my_id_str);
      GNUNET_free (my_id_str);

      GNUNET_STATISTICS_set (GDS_stats, key, succ, 0);
      GNUNET_free (key);
    }
    if (send_verify_successor_task == NULL)
      send_verify_successor_task =
              GNUNET_SCHEDULER_add_delayed(verify_successor_next_send_time,
                                           &send_verify_successor_message,
                                           NULL);
    return;
  }
  closest_peer = select_closest_peer (probable_successor,
                                      &current_successor->finger_identity,
                                      successor_value,
				      is_predecessor);

  /* If the current_successor in the finger table is closest, then do nothing. */
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (closest_peer,
                                            &current_successor->finger_identity))
  {
    //FIXME: Is this a good place to return the stats.
    if ((NULL != GDS_stats))
    {
      char *my_id_str;
      uint64_t succ;
      char *key;

      my_id_str = GNUNET_strdup (GNUNET_i2s_full (&my_identity));
      GNUNET_memcpy(&succ, &current_successor->finger_identity, sizeof(uint64_t));
      GNUNET_asprintf (&key, "XDHT:%s:", my_id_str);
      GNUNET_free (my_id_str);
      GNUNET_STATISTICS_set (GDS_stats, key, succ, 0);
      GNUNET_free (key);
    }

    if(0 == successor_times)
    {
//      successor_times = 3;
      verify_successor_next_send_time =
              GNUNET_TIME_STD_BACKOFF (verify_successor_next_send_time);
    }
    else
      successor_times--;


    if (send_verify_successor_task == NULL)
      send_verify_successor_task =
              GNUNET_SCHEDULER_add_delayed (verify_successor_next_send_time,
					    &send_verify_successor_message,
					    NULL);
    return;
  }

  /* Probable successor is the closest peer.*/
  if(trail_length > 0)
  {
    GNUNET_assert(NULL != GNUNET_CONTAINER_multipeermap_get  (friend_peermap,
							      &trail[0]));
  }
  else
  {
    GNUNET_assert(NULL != GNUNET_CONTAINER_multipeermap_get (friend_peermap,
							     probable_successor));
  }

  trail_me_to_probable_succ_len = 0;
  trail_me_to_probable_succ = check_trail_me_to_probable_succ (probable_successor,
							       trail,
							       trail_length,
							       &trail_me_to_probable_succ_len);

  /* Remove the existing successor. */
  remove_existing_finger (current_successor, 0);
   /* Generate a new trail id to reach to your new successor. */
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_STRONG,
                              &trail_id,
			      sizeof (trail_id));

  if (trail_me_to_probable_succ_len > 0)
  {
    GDS_ROUTING_add (&trail_id,
		     &my_identity,
		     &trail_me_to_probable_succ[0]);
    GNUNET_assert (NULL !=
                  (target_friend =
		   GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						      &trail_me_to_probable_succ[0])));
  }
  else
  {
    GDS_ROUTING_add (&trail_id,
		     &my_identity,
		     probable_successor);
    GNUNET_assert (NULL !=
                  (target_friend =
                   GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                      probable_successor)));
  }

  add_new_finger (probable_successor,
		  trail_me_to_probable_succ,
                  trail_me_to_probable_succ_len,
		  &trail_id,
		  0);

  notify_ctx = GNUNET_new (struct SendNotifyContext);

  notify_ctx->source_peer = my_identity;
  notify_ctx->successor = *probable_successor;
  notify_ctx->successor_trail = GNUNET_new_array (trail_me_to_probable_succ_len,
						  struct GNUNET_PeerIdentity);
  GNUNET_memcpy (notify_ctx->successor_trail,
		 trail_me_to_probable_succ,
		 sizeof(struct GNUNET_PeerIdentity) * trail_me_to_probable_succ_len);
  notify_ctx->successor_trail_length = trail_me_to_probable_succ_len;
  notify_ctx->succesor_trail_id = trail_id;
  notify_ctx->target_friend = target_friend;
  notify_ctx->num_retries_scheduled = 0;

  // TODO: Check if we should verify before schedule if already scheduled.
  GNUNET_SCHEDULER_add_now (&send_notify_new_successor,
			    notify_ctx);
}


void
send_notify_new_successor (void *cls)
{
  struct SendNotifyContext *ctx = cls;

  GDS_NEIGHBOURS_send_notify_new_successor (&ctx->source_peer,
                                            &ctx->successor,
                                            ctx->successor_trail,
                                            ctx->successor_trail_length,
                                            &ctx->succesor_trail_id,
                                            ctx->target_friend);

  if ( (0 == ctx->num_retries_scheduled) &&
       (send_notify_new_successor_retry_task != NULL) )
  {
    // Result from previous notify successos hasn't arrived, so the retry task
    // hasn't been cancelled! Already a new notify successor must be called.
    // We will cancel the retry request.
    struct SendNotifyContext *old_notify_ctx;

    old_notify_ctx = GNUNET_SCHEDULER_cancel(send_notify_new_successor_retry_task);
    GNUNET_free (old_notify_ctx->successor_trail);
    GNUNET_free (old_notify_ctx);
    send_notify_new_successor_retry_task = NULL;
  }

  ctx->num_retries_scheduled++;
  send_notify_new_successor_retry_task
    = GNUNET_SCHEDULER_add_delayed (notify_successor_retry_time,
				    &send_notify_new_successor,
				    cls);
}


/**
 * Check integrity of verify successor result messages.
 *
 * @param cls closure
 * @param vsrm the message
 * @return #GNUNET_OK if @a vrsm is well-formed
 */
static int
check_dht_p2p_verify_successor_result (void *cls,
				       const struct PeerVerifySuccessorResultMessage *vsrm)
{
  size_t msize;

  msize = ntohs (vsrm->header.size);
  if ((msize - sizeof (struct PeerVerifySuccessorResultMessage)) %
      sizeof (struct GNUNET_PeerIdentity) != 0)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Core handle for p2p verify successor result messages.
 *
 * @param cls closure
 * @param vsrm the message
 */
static void
handle_dht_p2p_verify_successor_result (void *cls,
					const struct PeerVerifySuccessorResultMessage *vsrm)
{
  enum GDS_ROUTING_trail_direction trail_direction;
  struct GNUNET_PeerIdentity querying_peer;
  struct GNUNET_HashCode trail_id;
  const struct GNUNET_PeerIdentity *next_hop;
  struct FriendInfo *target_friend;
  struct GNUNET_PeerIdentity probable_successor;
  struct GNUNET_PeerIdentity current_successor;
  const struct GNUNET_PeerIdentity *trail;
  unsigned int trail_length;
  size_t msize;

  msize = ntohs (vsrm->header.size);
  trail_length = (msize - sizeof (struct PeerVerifySuccessorResultMessage))
    / sizeof (struct GNUNET_PeerIdentity);

  GNUNET_STATISTICS_update (GDS_stats,
                            gettext_noop ("# Bytes received from other peers"),
			    msize,
                            GNUNET_NO);

  trail = (const struct GNUNET_PeerIdentity *) &vsrm[1];
  querying_peer = vsrm->querying_peer;
  trail_direction = ntohl (vsrm->trail_direction);
  trail_id = vsrm->trail_id;
  probable_successor = vsrm->probable_successor;
  current_successor = vsrm->current_successor;

  /* Am I the querying_peer? */
  if (0 == (GNUNET_CRYPTO_cmp_peer_identity (&querying_peer,
					     &my_identity)))
  {
    /* Cancel Retry Task */
    if (NULL != send_verify_successor_retry_task)
    {
      struct VerifySuccessorContext *ctx;

      ctx = GNUNET_SCHEDULER_cancel (send_verify_successor_retry_task);
      GNUNET_free (ctx);
      send_verify_successor_retry_task = NULL;
    }
    compare_and_update_successor (&current_successor,
                                  &probable_successor,
				  trail,
				  trail_length);
    return;
  }

  /*If you are not the querying peer then pass on the message */
  if(NULL == (next_hop =
              GDS_ROUTING_get_next_hop (&trail_id,
					trail_direction)))
  {
    /* Here it may happen that source peer has found a new successor, and removed
     the trail, Hence no entry found in the routing table. Fail silently.*/
    DEBUG (" NO ENTRY FOUND IN %s ROUTING TABLE for trail id %s, line %u",
           GNUNET_i2s (&my_identity),
           GNUNET_h2s (&trail_id),
           __LINE__);
    GNUNET_break_op(0);
    return;
  }
  if (NULL == (target_friend =
	       GNUNET_CONTAINER_multipeermap_get (friend_peermap, next_hop)))
  {
    GNUNET_break_op(0);
    return;
  }
  GDS_NEIGHBOURS_send_verify_successor_result (&querying_peer,
                                               &vsrm->current_successor,
                                               &probable_successor,
					       &trail_id,
                                               trail,
                                               trail_length,
                                               trail_direction,
					       target_friend);
}


/**
 * Check integrity of p2p notify new successor messages.
 *
 * @param cls closure
 * @param nsm the message
 * @return #GNUNET_OK if @a nsm is well-formed
 */
static int
check_dht_p2p_notify_new_successor (void *cls,
				    const struct PeerNotifyNewSuccessorMessage *nsm)
{
  size_t msize;

  msize = ntohs (nsm->header.size);
  if ((msize - sizeof (struct PeerNotifyNewSuccessorMessage)) %
      sizeof (struct GNUNET_PeerIdentity) != 0)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Core handle for p2p notify new successor messages.
 *
 * @param cls closure
 * @param nsm the message
 */
static void
handle_dht_p2p_notify_new_successor (void *cls,
				     const struct PeerNotifyNewSuccessorMessage *nsm)
{
  struct FriendInfo *friend = cls;
  const struct GNUNET_PeerIdentity *trail;
  struct GNUNET_PeerIdentity source;
  struct GNUNET_PeerIdentity new_successor;
  struct GNUNET_HashCode trail_id;
  struct GNUNET_PeerIdentity next_hop;
  struct FriendInfo *target_friend;
  int my_index;
  size_t msize;
  uint32_t trail_length;

  msize = ntohs (nsm->header.size);
  trail_length = (msize - sizeof (struct PeerNotifyNewSuccessorMessage))/
                  sizeof (struct GNUNET_PeerIdentity);
  GNUNET_STATISTICS_update (GDS_stats,
                            gettext_noop ("# Bytes received from other peers"),
			    msize,
                            GNUNET_NO);
  trail = (const struct GNUNET_PeerIdentity *) &nsm[1];
  source = nsm->source_peer;
  new_successor = nsm->new_successor;
  trail_id = nsm->trail_id;

  /* I am the new_successor to source_peer. */
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (&my_identity,
					    &new_successor))
  {
    if (trail_length > 0)
      GNUNET_assert(0 == GNUNET_CRYPTO_cmp_peer_identity (&trail[trail_length - 1],
							  friend->id));
    else
      GNUNET_assert(0 == GNUNET_CRYPTO_cmp_peer_identity (&source,
							  friend->id));

    compare_and_update_predecessor (&source,
				    trail,
				    trail_length);
    target_friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						       friend->id);
    GNUNET_assert (NULL != target_friend);
    GDS_NEIGHBOURS_send_notify_succcessor_confirmation (&trail_id,
                                                        GDS_ROUTING_DEST_TO_SRC,
                                                        target_friend);
    return;
  }

  GNUNET_assert(trail_length > 0);
  /* I am part of trail to reach to successor. */
  my_index = search_my_index (trail, trail_length);
  if (-1 == my_index)
  {
    DEBUG ("No entry found in trail\n");
    GNUNET_break_op (0);
    return;
  }
  if((trail_length + 1) == my_index)
  {
    DEBUG ("Found twice in trail.\n");
    GNUNET_break_op (0);
    return;
  }
  if ((trail_length-1) == my_index)
    next_hop = new_successor;
  else
    next_hop = trail[my_index + 1];

  GDS_ROUTING_add (&trail_id,
		   friend->id,
		   &next_hop);
  target_friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						     &next_hop);
  if (NULL == target_friend)
  {
    GNUNET_break(0);
    return;
  }
  GDS_NEIGHBOURS_send_notify_new_successor (&source,
					    &new_successor,
					    trail,
                                            trail_length,
                                            &trail_id,
					    target_friend);
}


/**
 * Core handler for P2P notify successor message
 *
 * @param cls closure
 * @param notify_confirmation the message
 */
static void
handle_dht_p2p_notify_succ_confirmation (void *cls,
                                         const struct PeerNotifyConfirmationMessage *notify_confirmation)
{
  enum GDS_ROUTING_trail_direction trail_direction;
  struct GNUNET_HashCode trail_id;
  struct FriendInfo *target_friend;
  const struct GNUNET_PeerIdentity *next_hop;

  GNUNET_STATISTICS_update (GDS_stats,
                            gettext_noop ("# Bytes received from other peers"),
			    ntohs (notify_confirmation->header.size),
                            GNUNET_NO);
  trail_direction = ntohl (notify_confirmation->trail_direction);
  trail_id = notify_confirmation->trail_id;

  next_hop = GDS_ROUTING_get_next_hop (&trail_id,
				       trail_direction);
  if (NULL == next_hop)
  {
    /* The source of notify new successor, might have found even a better
     successor. In that case it send a trail teardown message, and hence,
     the next hop is NULL. */
    //Fixme: Add some print to confirm the above theory.
    return;
  }

  /* I peer which sent the notify successor message to the successor. */
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (next_hop,
					    &my_identity))
  {
   /*
    * Schedule another round of verify sucessor with your current successor
    * which may or may not be source of this message. This message is used
    * only to ensure that we have a path setup to reach to our successor.
    */

    // TODO: cancel schedule of notify_successor_retry_task
    if (send_notify_new_successor_retry_task != NULL)
    {
      struct SendNotifyContext *notify_ctx;
      notify_ctx = GNUNET_SCHEDULER_cancel(send_notify_new_successor_retry_task);
      GNUNET_free (notify_ctx->successor_trail);
      GNUNET_free (notify_ctx);
      send_notify_new_successor_retry_task = NULL;
    }
    if (send_verify_successor_task == NULL)
    {
      verify_successor_next_send_time.rel_value_us =
      DHT_SEND_VERIFY_SUCCESSOR_INTERVAL.rel_value_us +
      GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK,
                                DHT_SEND_VERIFY_SUCCESSOR_INTERVAL.rel_value_us);
      send_verify_successor_task
	= GNUNET_SCHEDULER_add_delayed(verify_successor_next_send_time,
				       &send_verify_successor_message,
				       NULL);
    }
  }
  else
  {
    target_friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						       next_hop);
    if (NULL == target_friend)
      {
      DEBUG ("\n friend not found, line number = %d",
	     __LINE__);
      return;
    }
    GDS_NEIGHBOURS_send_notify_succcessor_confirmation  (&trail_id,
							 GDS_ROUTING_DEST_TO_SRC,
							 target_friend);
  }
}


/**
 * Check integrity of P2P trail rejection message
 *
 * @param cls closure
 * @param trail_rejection the message
 * @return #GNUNET_OK if @a trail_rejection is well-formed
 */
static int
check_dht_p2p_trail_setup_rejection (void *cls,
				     const struct PeerTrailRejectionMessage *trail_rejection)
{
  size_t msize;

  msize = ntohs (trail_rejection->header.size);
  if ((msize - sizeof (struct PeerTrailRejectionMessage)) %
      sizeof (struct GNUNET_PeerIdentity) != 0)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Core handler for P2P trail rejection message
 *
 * @param cls closure
 * @param trail_rejection the message
 */
static void
handle_dht_p2p_trail_setup_rejection (void *cls,
                                      const struct PeerTrailRejectionMessage *trail_rejection)
{
  struct FriendInfo *friend = cls;
  unsigned int trail_length;
  const struct GNUNET_PeerIdentity *trail_peer_list;
  struct FriendInfo *target_friend;
  struct GNUNET_TIME_Relative congestion_timeout;
  struct GNUNET_HashCode trail_id;
  struct GNUNET_PeerIdentity next_peer;
  struct GNUNET_PeerIdentity source;
  uint64_t ultimate_destination_finger_value;
  unsigned int is_predecessor;
  struct Closest_Peer successor;
  size_t msize;

  msize = ntohs (trail_rejection->header.size);
  trail_length = (msize - sizeof (struct PeerTrailRejectionMessage))/
                  sizeof (struct GNUNET_PeerIdentity);
  GNUNET_STATISTICS_update (GDS_stats,
                            gettext_noop ("# Bytes received from other peers"),
			    msize,
                            GNUNET_NO);

  trail_peer_list = (const struct GNUNET_PeerIdentity *) &trail_rejection[1];
  is_predecessor = ntohl (trail_rejection->is_predecessor);
  congestion_timeout = trail_rejection->congestion_time;
  source = trail_rejection->source_peer;
  trail_id = trail_rejection->trail_id;
  ultimate_destination_finger_value
    = GNUNET_ntohll (trail_rejection->ultimate_destination_finger_value);
  /* First set the congestion time of the friend that sent you this message. */
  target_friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						     friend->id);
  if (NULL == target_friend)
  {
    DEBUG ("\nLINE = %d ,No friend found.",__LINE__);
    GNUNET_break(0);
    return;
  }
  target_friend->congestion_timestamp
    = GNUNET_TIME_absolute_add (GNUNET_TIME_absolute_get(),
				congestion_timeout);

  /* I am the source peer which wants to setup the trail. Do nothing.
   * send_find_finger_trail_task is scheduled periodically.*/
  if(0 == (GNUNET_CRYPTO_cmp_peer_identity (&my_identity, &source)))
    return;

  /* If I am congested then pass this message to peer before me in trail. */
  if (GNUNET_YES == GDS_ROUTING_threshold_reached())
  {
    /* First remove yourself from the trail. */
    unsigned int new_trail_length = trail_length - 1;
    struct GNUNET_PeerIdentity trail[new_trail_length];

    GNUNET_memcpy (trail,
		   trail_peer_list,
		   new_trail_length * sizeof(struct GNUNET_PeerIdentity));
    if (0 == trail_length)
      next_peer = source;
    else
      next_peer = trail[new_trail_length-1];

    target_friend
      = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
					   &next_peer);
    if (NULL == target_friend)
    {
      DEBUG ("\nLINE = %d ,No friend found.",
	     __LINE__);
      GNUNET_break(0);
      return;
    }
    GDS_NEIGHBOURS_send_trail_rejection (&source,
                                         ultimate_destination_finger_value,
                                         &my_identity,
					 is_predecessor,
                                         trail,
					 new_trail_length,
					 &trail_id,
                                         target_friend,
					 CONGESTION_TIMEOUT);
    return;
  }

  successor = find_local_best_known_next_hop (ultimate_destination_finger_value,
					      is_predecessor);

  /* Am I the final destination? */
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (&successor.best_known_destination,
					    &my_identity))
  {
     /*Here you are already part of trail. Copy the trail removing yourself. */
    unsigned int new_trail_length = trail_length - 1;
    struct GNUNET_PeerIdentity trail[new_trail_length];

    GNUNET_memcpy (trail,
		   trail_peer_list,
		   new_trail_length * sizeof(struct GNUNET_PeerIdentity));

    if (0 == new_trail_length)
      next_peer = source;
    else
    {
      next_peer = trail[new_trail_length-1];
    }
    target_friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						       &next_peer);

    if (NULL == target_friend)
    {
      DEBUG ("\nLINE = %d ,No friend found.",
	     __LINE__);
      GNUNET_break(0);
      return;
    }
    GDS_NEIGHBOURS_send_trail_setup_result (&source,
                                            &my_identity,
                                            target_friend,
					    new_trail_length,
                                            trail,
                                            is_predecessor,
                                            ultimate_destination_finger_value,
                                            &trail_id);
    return;
  }
  /* Here I was already part of trail. So no need to add. */
  target_friend = GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						     &successor.next_hop);
  if (NULL == target_friend)
  {
    DEBUG ("\nLINE = %d ,No friend found.",__LINE__);
    GNUNET_break (0);
    return;
  }
  GDS_NEIGHBOURS_send_trail_setup (&source,
				   ultimate_destination_finger_value,
				   &successor.best_known_destination,
				   target_friend,
				   trail_length,
				   trail_peer_list,
				   is_predecessor,
				   &trail_id,
				   &successor.trail_id);
}


/**
 * Core handler for trail teardown message.
 *
 * @param cls closure
 * @param trail_teardown the message
 */
static void
handle_dht_p2p_trail_teardown (void *cls,
                               const struct PeerTrailTearDownMessage *trail_teardown)
{
  enum GDS_ROUTING_trail_direction trail_direction;
  struct GNUNET_HashCode trail_id;
  const struct GNUNET_PeerIdentity *next_hop;
  size_t msize;

  msize = ntohs (trail_teardown->header.size);
  GNUNET_STATISTICS_update (GDS_stats,
                            gettext_noop ("# Bytes received from other peers"),
			    msize,
                            GNUNET_NO);
  trail_direction = ntohl (trail_teardown->trail_direction);
  trail_id = trail_teardown->trail_id;

  /* Check if peer is the real peer from which we should get this message.*/
  /* Get the prev_hop for this trail by getting the next hop in opposite direction. */
#if 0
  GNUNET_assert (NULL != (prev_hop =
                 GDS_ROUTING_get_next_hop (trail_id, ! trail_direction)));
  if (0 != GNUNET_CRYPTO_cmp_peer_identity (prev_hop,
					    friend->id))
  {
    GNUNET_break (0);
    return;
  }
#endif

  next_hop = GDS_ROUTING_get_next_hop (&trail_id,
				       trail_direction);
  if (NULL == next_hop)
  {
    DEBUG(" NO ENTRY FOUND IN %s ROUTING TABLE for trail id %s, line %u",
          GNUNET_i2s (&my_identity),
          GNUNET_h2s (&trail_id),
          __LINE__);
    GNUNET_break (0);
    return;
  }

  /* I am the next hop, which means I am the final destination. */
  if (0 == GNUNET_CRYPTO_cmp_peer_identity (next_hop, &my_identity))
  {
    GNUNET_assert (GNUNET_YES ==
		   GDS_ROUTING_remove_trail (&trail_id));
    return;
  }
  /* If not final destination, then send a trail teardown message to next hop.*/
  GNUNET_assert (NULL != GNUNET_CONTAINER_multipeermap_get (friend_peermap,
							    next_hop));
  GNUNET_assert (GNUNET_YES ==
		 GDS_ROUTING_remove_trail (&trail_id));
  GDS_NEIGHBOURS_send_trail_teardown (&trail_id,
				      trail_direction,
				      next_hop);
}


/**
 * Check validity of p2p add trail message.
 *
 * @param cls closure
 * @param add_trail the message
 * @return #GNUNET_OK if @a add_trail is well-formed
 */
static int
check_dht_p2p_add_trail (void *cls,
			 const struct PeerAddTrailMessage *add_trail)
{
  size_t msize;

  msize = ntohs (add_trail->header.size);
  if ((msize - sizeof (struct PeerAddTrailMessage)) %
      sizeof (struct GNUNET_PeerIdentity) != 0)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Core handle for p2p add trail message.
 *
 * @param cls closure
 * @param add_trail the message
 */
static void
handle_dht_p2p_add_trail (void *cls,
                          const struct PeerAddTrailMessage *add_trail)
{
  struct FriendInfo *friend = cls;
  const struct GNUNET_PeerIdentity *trail;
  struct GNUNET_HashCode trail_id;
  struct GNUNET_PeerIdentity destination_peer;
  struct GNUNET_PeerIdentity source_peer;
  struct GNUNET_PeerIdentity next_hop;
  unsigned int trail_length;
  unsigned int my_index;
  size_t msize;

  msize = ntohs (add_trail->header.size);
  /* In this message we pass the whole trail from source to destination as we
   * are adding that trail.*/
  //FIXME: failed when run with 1000 pears. check why.
  trail_length = (msize - sizeof (struct PeerAddTrailMessage))/
                  sizeof (struct GNUNET_PeerIdentity);
  GNUNET_STATISTICS_update (GDS_stats,
                            gettext_noop ("# Bytes received from other peers"),
			    msize,
                            GNUNET_NO);

  trail = (const struct GNUNET_PeerIdentity *) &add_trail[1];
  destination_peer = add_trail->destination_peer;
  source_peer = add_trail->source_peer;
  trail_id = add_trail->trail_id;

  /* I am not the destination of the trail. */
  if (0 != GNUNET_CRYPTO_cmp_peer_identity (&my_identity,
					    &destination_peer))
  {
    struct FriendInfo *target_friend;

    /* Get my location in the trail. */
    my_index = search_my_index (trail, trail_length);
    if (-1 == my_index)
    {
      GNUNET_break_op (0);
      return;
    }
    if((trail_length + 1) == my_index)
    {
      DEBUG ("Found twice in trail.\n");
      GNUNET_break_op (0);
      return;
    }
    if ((trail_length - 1) == my_index)
    {
      next_hop = destination_peer;
    }
    else
    {
      next_hop = trail[my_index + 1];
    }
    /* Add in your routing table. */
    GNUNET_assert (GNUNET_OK == GDS_ROUTING_add (&trail_id,
						 friend->id,
						 &next_hop));
    //GNUNET_assert (GNUNET_OK == GDS_ROUTING_add (trail_id, next_hop, *peer));
    GNUNET_assert (NULL !=
                  (target_friend =
                   GNUNET_CONTAINER_multipeermap_get (friend_peermap,
						      &next_hop)));
    GDS_NEIGHBOURS_send_add_trail (&source_peer,
				   &destination_peer,
				   &trail_id,
                                   trail,
				   trail_length,
				   target_friend);
    return;
  }
  /* I am the destination. Add an entry in routing table. */
  GNUNET_assert (GNUNET_OK == GDS_ROUTING_add (&trail_id,
					       friend->id,
					       &my_identity));
}


/**
 * Free the finger trail in which the first friend to reach to a finger is
 * disconnected_friend. Also remove entry from routing table for that particular
 * trail id.
 * @param disconnected_friend PeerIdentity of friend which got disconnected
 * @param remove_finger Finger whose trail we need to check if it has
 *                      disconnected_friend as the first hop.
 * @return Total number of trails in which disconnected_friend was the first
 *         hop.
 */
static int
remove_matching_trails (const struct GNUNET_PeerIdentity *disconnected_friend,
                        struct FingerInfo *finger)
{
  const struct GNUNET_PeerIdentity *next_hop;
  struct FriendInfo *remove_friend;
  struct Trail *current_trail;
  unsigned int matching_trails_count = 0;
  int i;

  /* Iterate over all the trails of finger. */
  for (i = 0; i < finger->trails_count; i++)
  {
    current_trail = &finger->trail_list[i];
    if (GNUNET_NO == current_trail->is_present)
      continue;

    /* First friend to reach to finger is disconnected_peer. */
    if (0 == GNUNET_CRYPTO_cmp_peer_identity (&current_trail->trail_head->peer,
                                              disconnected_friend))
    {
      remove_friend =
                     GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                        disconnected_friend);
      GNUNET_assert (NULL != remove_friend);
      next_hop = GDS_ROUTING_get_next_hop (&current_trail->trail_id,
                                           GDS_ROUTING_SRC_TO_DEST);

      /* Here it may happen that as all the peers got disconnected, the entry in
       routing table for that particular trail has been removed, because the
       previously disconnected peer was either a next hop or prev hop of that
       peer. */
      if (NULL != next_hop)
      {
        GNUNET_assert (0 == (GNUNET_CRYPTO_cmp_peer_identity (disconnected_friend,
                                                              next_hop)));
        GNUNET_assert (GNUNET_YES ==
		       GDS_ROUTING_remove_trail (&current_trail->trail_id));
      }
      matching_trails_count++;
      free_trail (current_trail);
      current_trail->is_present = GNUNET_NO;
    }
  }
  return matching_trails_count;
}


/**
 * Iterate over finger_table entries.
 * 0. Ignore finger which is my_identity or if no valid entry present at
 *    that finger index.
 * 1. If disconnected_friend is a finger, then remove the routing entry from
      your own table. Free the trail.
 * 2. Check if disconnected_friend is the first friend in the trail to reach to a finger.
 *   2.1 Remove all the trails and entry from routing table in which disconnected
 *       friend is the first friend in the trail. If disconnected_friend is the
 *       first friend in all the trails to reach finger, then remove the finger.
 * @param disconnected_friend Peer identity of friend which got disconnected.
 */
static void
remove_matching_fingers (const struct GNUNET_PeerIdentity *disconnected_peer)
{
  struct FingerInfo *current_finger;
  int removed_trails_count;
  int i;

  /* Iterate over finger table entries. */
  for (i = 0; i < MAX_FINGERS; i++)
  {
    current_finger = &finger_table[i];

    /* No finger stored at this trail index or I am the finger. */
    if ((GNUNET_NO == current_finger->is_present) ||
        (0 == GNUNET_CRYPTO_cmp_peer_identity (&current_finger->finger_identity,
                                               &my_identity)))
      continue;

    /* Is disconnected_peer a finger? */
    if (0 == GNUNET_CRYPTO_cmp_peer_identity (disconnected_peer,
                                              &current_finger->finger_identity))
    {
      remove_existing_finger (current_finger, i);
    }

    /* If finger is a friend but not disconnected_friend, then continue. */
    if (NULL != GNUNET_CONTAINER_multipeermap_get (friend_peermap,
                                                   &current_finger->finger_identity))
      continue;

    /* Iterate over the list of trails to reach remove_finger. Check if
     * disconnected_friend is the first friend in any of the trail. */
    removed_trails_count = remove_matching_trails (disconnected_peer,
                                                   current_finger);
    current_finger->trails_count =
            current_finger->trails_count - removed_trails_count;
    if (0 == current_finger->trails_count)
    {
      current_finger->is_present = GNUNET_NO;
      memset (&finger_table[i],
	      0,
	      sizeof (finger_table[i]));
    }
  }
}


/**
 * Method called whenever a peer disconnects.
 *
 * @param cls closure
 * @param peer peer identity this notification is about
 * @param internal_cls our `struct FriendInfo` for @a peer
 */
static void
handle_core_disconnect (void *cls,
			const struct GNUNET_PeerIdentity *peer,
			void *internal_cls)
{
  struct FriendInfo *remove_friend = internal_cls;

  /* If disconnected to own identity, then return. */
  if (NULL == remove_friend)
    return;
  remove_matching_fingers (peer);
  GNUNET_assert (GNUNET_SYSERR !=
		 GDS_ROUTING_remove_trail_by_peer (peer));
  GNUNET_assert (GNUNET_YES ==
                 GNUNET_CONTAINER_multipeermap_remove (friend_peermap,
                                                       peer,
                                                       remove_friend));
  if (0 != GNUNET_CONTAINER_multipeermap_size (friend_peermap))
    return;

  if (NULL != find_finger_trail_task)
  {
    GNUNET_SCHEDULER_cancel (find_finger_trail_task);
    find_finger_trail_task = NULL;
  }
  else
    GNUNET_break (0);
}


/**
 * Method called whenever a peer connects.
 *
 * @param cls closure
 * @param peer_identity peer identity this notification is about
 * @param mq message queue for sending data to @a peer
 * @return our `struct FriendInfo` for this peer
 */
static void *
handle_core_connect (void *cls,
		     const struct GNUNET_PeerIdentity *peer_identity,
		     struct GNUNET_MQ_Handle *mq)
{
  struct FriendInfo *friend;

  /* Check for connect to self message */
  if (0 == memcmp (&my_identity,
		   peer_identity,
		   sizeof (struct GNUNET_PeerIdentity)))
    return NULL;
  friend = GNUNET_new (struct FriendInfo);
  friend->id = peer_identity;
  friend->mq = mq;
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CONTAINER_multipeermap_put (friend_peermap,
                                                    friend->id,
						    friend,
                                                    GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY));

  /* FIXME: now we are not making a distinction between fingers which are friends
   * also.But later, we should add a congestion timestamp on the friend, so that it is
   * selected after some time out. This is to ensure that both peers have added
   * each other as their friend. */
  /* Got a first connection, good time to start with FIND FINGER TRAIL requests...*/
  if (NULL == find_finger_trail_task)
  {
    find_finger_trail_task
      = GNUNET_SCHEDULER_add_now (&send_find_finger_trail_message,
				  NULL);
  }
  return friend;
}


/**
 * To be called on core init/fail.
 *
 * @param cls service closure
 * @param identity the public identity of this peer
 */
static void
core_init (void *cls,
           const struct GNUNET_PeerIdentity *identity)
{
  my_identity = *identity;
}


/**
 * Initialize finger table entries.
 */
static void
finger_table_init ()
{
  memset (&finger_table, 0, sizeof (finger_table));
}


/**
 * Initialize neighbours subsystem.
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on error
 */
int
GDS_NEIGHBOURS_init (void)
{
  struct GNUNET_MQ_MessageHandler core_handlers[] = {
    GNUNET_MQ_hd_var_size (dht_p2p_put,
                           GNUNET_MESSAGE_TYPE_XDHT_P2P_PUT,
                           struct PeerPutMessage,
                           NULL),
    GNUNET_MQ_hd_var_size (dht_p2p_get,
                           GNUNET_MESSAGE_TYPE_XDHT_P2P_GET,
                           struct PeerGetMessage,
                           NULL),
    GNUNET_MQ_hd_var_size (dht_p2p_get_result,
                           GNUNET_MESSAGE_TYPE_XDHT_P2P_GET_RESULT,
                           struct PeerGetResultMessage,
                           NULL),
    GNUNET_MQ_hd_var_size (dht_p2p_trail_setup,
                           GNUNET_MESSAGE_TYPE_XDHT_P2P_TRAIL_SETUP,
                           struct PeerTrailSetupMessage,
                           NULL),
    GNUNET_MQ_hd_var_size (dht_p2p_trail_setup_result,
                           GNUNET_MESSAGE_TYPE_XDHT_P2P_TRAIL_SETUP_RESULT,
                           struct PeerTrailSetupResultMessage,
                           NULL),
    GNUNET_MQ_hd_var_size (dht_p2p_verify_successor,
                           GNUNET_MESSAGE_TYPE_XDHT_P2P_VERIFY_SUCCESSOR,
                           struct PeerVerifySuccessorMessage,
                           NULL),
    GNUNET_MQ_hd_var_size (dht_p2p_verify_successor_result,
                           GNUNET_MESSAGE_TYPE_XDHT_P2P_VERIFY_SUCCESSOR_RESULT,
                           struct PeerVerifySuccessorResultMessage,
                           NULL),
    GNUNET_MQ_hd_var_size (dht_p2p_notify_new_successor,
                           GNUNET_MESSAGE_TYPE_XDHT_P2P_NOTIFY_NEW_SUCCESSOR,
                           struct PeerNotifyNewSuccessorMessage,
                           NULL),
    GNUNET_MQ_hd_var_size (dht_p2p_trail_setup_rejection,
                           GNUNET_MESSAGE_TYPE_XDHT_P2P_TRAIL_SETUP_REJECTION,
                           struct PeerTrailRejectionMessage,
                           NULL),
    GNUNET_MQ_hd_fixed_size (dht_p2p_trail_teardown,
                             GNUNET_MESSAGE_TYPE_XDHT_P2P_TRAIL_TEARDOWN,
                             struct PeerTrailTearDownMessage,
                             NULL),
    GNUNET_MQ_hd_var_size (dht_p2p_add_trail,
                           GNUNET_MESSAGE_TYPE_XDHT_P2P_ADD_TRAIL,
                           struct PeerAddTrailMessage,
                           NULL),
    GNUNET_MQ_hd_fixed_size (dht_p2p_notify_succ_confirmation,
                             GNUNET_MESSAGE_TYPE_XDHT_P2P_NOTIFY_SUCCESSOR_CONFIRMATION,
                             struct PeerNotifyConfirmationMessage,
                             NULL),
    GNUNET_MQ_handler_end ()
  };

  core_api = GNUNET_CORE_connect (GDS_cfg,
				  NULL,
				  &core_init,
				  &handle_core_connect,
				  &handle_core_disconnect,
				  core_handlers);
  if (NULL == core_api)
    return GNUNET_SYSERR;
  friend_peermap = GNUNET_CONTAINER_multipeermap_create (256,
							 GNUNET_YES);
  finger_table_init ();
  successor_times = 10;
  fingers_round_count = 5;
  find_finger_trail_task_next_send_time.rel_value_us =
      DHT_FIND_FINGER_TRAIL_INTERVAL.rel_value_us +
      GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK,
                                DHT_FIND_FINGER_TRAIL_INTERVAL.rel_value_us);

  verify_successor_next_send_time.rel_value_us =
      DHT_SEND_VERIFY_SUCCESSOR_INTERVAL.rel_value_us +
      GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK,
                                DHT_SEND_VERIFY_SUCCESSOR_INTERVAL.rel_value_us);

  verify_successor_retry_time.rel_value_us =
      DHT_SEND_VERIFY_SUCCESSOR_RETRY_INTERVAL.rel_value_us +
      GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK,
                                DHT_SEND_VERIFY_SUCCESSOR_RETRY_INTERVAL.rel_value_us);

  notify_successor_retry_time.rel_value_us =
      DHT_SEND_NOTIFY_SUCCESSOR_RETRY_INTERVAL.rel_value_us +
      GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK,
                                DHT_SEND_NOTIFY_SUCCESSOR_RETRY_INTERVAL.rel_value_us);

  return GNUNET_OK;
}


/**
 * Free the memory held up by trails of a finger.
 */
static void
delete_finger_table_entries()
{
  for (unsigned int i = 0; i < MAX_FINGERS; i++)
  {
    if (GNUNET_YES != finger_table[i].is_present)
      continue;
    for (unsigned int j = 0; j < finger_table[i].trails_count; j++)
      free_trail(&finger_table[i].trail_list[j]);
  }
}


/**
 * Shutdown neighbours subsystem.
 */
void
GDS_NEIGHBOURS_done (void)
{
  if (NULL == core_api)
    return;

  GNUNET_CORE_disconnect (core_api);
  core_api = NULL;

  delete_finger_table_entries();
  GNUNET_assert (0 == GNUNET_CONTAINER_multipeermap_size (friend_peermap));
  GNUNET_CONTAINER_multipeermap_destroy (friend_peermap);
  friend_peermap = NULL;

  if (NULL != find_finger_trail_task)
  {
    GNUNET_SCHEDULER_cancel (find_finger_trail_task);
    find_finger_trail_task = NULL;
  }

  if (NULL != send_verify_successor_task)
  {
    GNUNET_SCHEDULER_cancel (send_verify_successor_task);
    send_verify_successor_task = NULL;
  }
  if (NULL != send_verify_successor_retry_task)
  {
    struct VerifySuccessorContext *ctx;

    ctx = GNUNET_SCHEDULER_cancel (send_verify_successor_retry_task);
    GNUNET_free (ctx);
    send_verify_successor_retry_task = NULL;
  }
  if (NULL != send_notify_new_successor_retry_task)
  {
    struct SendNotifyContext *notify_ctx;

    notify_ctx = GNUNET_SCHEDULER_cancel (send_notify_new_successor_retry_task);
    GNUNET_free (notify_ctx->successor_trail);
    GNUNET_free (notify_ctx);
    send_notify_new_successor_retry_task = NULL;
  }
}


/**
 * Get my identity
 *
 * @return my identity
 */
struct GNUNET_PeerIdentity *
GDS_NEIGHBOURS_get_id (void)
{
  return &my_identity;
}

/* end of gnunet-service-xdht_neighbours.c */
