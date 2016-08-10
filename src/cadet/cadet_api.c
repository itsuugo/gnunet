/*
     This file is part of GNUnet.
     Copyright (C) 2011 GNUnet e.V.

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
 * @file cadet/cadet_api.c
 * @brief cadet api: client implementation of new cadet service
 * @author Bartlomiej Polot
 */

#include "platform.h"
#include "gnunet_util_lib.h"
#include "gnunet_constants.h"
#include "gnunet_cadet_service.h"
#include "cadet.h"
#include "cadet_protocol.h"

#define LOG(kind,...) GNUNET_log_from (kind, "cadet-api",__VA_ARGS__)

/******************************************************************************/
/************************      DATA STRUCTURES     ****************************/
/******************************************************************************/

/**
 * Transmission queue to the service
 */
struct GNUNET_CADET_TransmitHandle
{
    /**
     * Double Linked list
     */
  struct GNUNET_CADET_TransmitHandle *next;

    /**
     * Double Linked list
     */
  struct GNUNET_CADET_TransmitHandle *prev;

    /**
     * Channel this message is sent on / for (may be NULL for control messages).
     */
  struct GNUNET_CADET_Channel *channel;

    /**
     * Request data task.
     */
  struct GNUNET_SCHEDULER_Task *request_data_task;

    /**
     * Callback to obtain the message to transmit, or NULL if we
     * got the message in 'data'.  Notice that messages built
     * by 'notify' need to be encapsulated with information about
     * the 'target'.
     */
  GNUNET_CONNECTION_TransmitReadyNotify notify;

    /**
     * Closure for 'notify'
     */
  void *notify_cls;

    /**
     * Size of the payload.
     */
  size_t size;
};

union CadetInfoCB {

  /**
   * Channel callback.
   */
  GNUNET_CADET_ChannelCB channel_cb;

  /**
   * Monitor callback
   */
  GNUNET_CADET_PeersCB peers_cb;

  /**
   * Monitor callback
   */
  GNUNET_CADET_PeerCB peer_cb;

  /**
   * Monitor callback
   */
  GNUNET_CADET_TunnelsCB tunnels_cb;

  /**
   * Tunnel callback.
   */
  GNUNET_CADET_TunnelCB tunnel_cb;
};


/**
 * Opaque handle to the service.
 */
struct GNUNET_CADET_Handle
{
    /**
     * Message queue (if available).
     */
  struct GNUNET_MQ_Handle *mq;

    /**
     * Set of handlers used for processing incoming messages in the channels
     */
  const struct GNUNET_CADET_MessageHandler *message_handlers;

  /**
   * Number of handlers in the handlers array.
   */
  unsigned int n_handlers;

  /**
   * Ports open.
   */
  struct GNUNET_CONTAINER_MultiHashMap *ports;

    /**
     * Double linked list of the channels this client is connected to, head.
     */
  struct GNUNET_CADET_Channel *channels_head;

    /**
     * Double linked list of the channels this client is connected to, tail.
     */
  struct GNUNET_CADET_Channel *channels_tail;

    /**
     * Callback for inbound channel disconnection
     */
  GNUNET_CADET_ChannelEndHandler *cleaner;

    /**
     * Closure for all the handlers given by the client
     */
  void *cls;

    /**
     * Messages to send to the service, head.
     */
  struct GNUNET_CADET_TransmitHandle *th_head;

    /**
     * Messages to send to the service, tail.
     */
  struct GNUNET_CADET_TransmitHandle *th_tail;

    /**
     * chid of the next channel to create (to avoid reusing IDs often)
     */
  CADET_ChannelNumber next_chid;

  /**
   * Configuration given by the client, in case of reconnection
   */
  const struct GNUNET_CONFIGURATION_Handle *cfg;

  /**
   * Time to the next reconnect in case one reconnect fails
   */
  struct GNUNET_TIME_Relative reconnect_time;

  /**
   * Task for trying to reconnect.
   */
  struct GNUNET_SCHEDULER_Task * reconnect_task;

  /**
   * Callback for an info task (only one active at a time).
   */
  union CadetInfoCB info_cb;

  /**
   * Info callback closure for @c info_cb.
   */
  void *info_cls;
};


/**
 * Description of a peer
 */
struct GNUNET_CADET_Peer
{
    /**
     * ID of the peer in short form
     */
  GNUNET_PEER_Id id;

  /**
   * Channel this peer belongs to
   */
  struct GNUNET_CADET_Channel *t;
};


/**
 * Opaque handle to a channel.
 */
struct GNUNET_CADET_Channel
{
    /**
     * DLL next
     */
  struct GNUNET_CADET_Channel *next;

    /**
     * DLL prev
     */
  struct GNUNET_CADET_Channel *prev;

    /**
     * Handle to the cadet this channel belongs to
     */
  struct GNUNET_CADET_Handle *cadet;

    /**
     * Local ID of the channel
     */
  CADET_ChannelNumber chid;

    /**
     * Channel's port, if any.
     */
  struct GNUNET_CADET_Port *port;

    /**
     * Other end of the channel.
     */
  GNUNET_PEER_Id peer;

  /**
   * Any data the caller wants to put in here
   */
  void *ctx;

    /**
     * Size of packet queued in this channel
     */
  unsigned int packet_size;

    /**
     * Channel options: reliability, etc.
     */
  enum GNUNET_CADET_ChannelOption options;

    /**
     * Are we allowed to send to the service?
     */
  int allow_send;

};

/**
 * Opaque handle to a port.
 */
struct GNUNET_CADET_Port
{
    /**
     * Handle to the CADET session this port belongs to.
     */
  struct GNUNET_CADET_Handle *cadet;

    /**
     * Port ID.
     */
  struct GNUNET_HashCode *hash;

    /**
     * Callback handler for incoming channels on this port.
     */
  GNUNET_CADET_InboundChannelNotificationHandler *handler;

    /**
     * Closure for @a handler.
     */
  void *cls;
};


/**
 * Implementation state for cadet's message queue.
 */
struct CadetMQState
{
  /**
   * The current transmit handle, or NULL
   * if no transmit is active.
   */
  struct GNUNET_CADET_TransmitHandle *th;

  /**
   * Channel to send the data over.
   */
  struct GNUNET_CADET_Channel *channel;
};


/******************************************************************************/
/***********************     AUXILIARY FUNCTIONS      *************************/
/******************************************************************************/

/**
 * Check if transmission is a payload packet.
 *
 * @param th Transmission handle.
 *
 * @return #GNUNET_YES if it is a payload packet,
 *         #GNUNET_NO if it is a cadet management packet.
 */
static int
th_is_payload (struct GNUNET_CADET_TransmitHandle *th)
{
  return (th->notify != NULL) ? GNUNET_YES : GNUNET_NO;
}


/**
 * Find the Port struct for a hash.
 *
 * @param h CADET handle.
 * @param hash HashCode for the port number.
 *
 * @return The port handle if known, NULL otherwise.
 */
static struct GNUNET_CADET_Port *
find_port (const struct GNUNET_CADET_Handle *h,
	   const struct GNUNET_HashCode *hash)
{
  struct GNUNET_CADET_Port *p;

  p = GNUNET_CONTAINER_multihashmap_get (h->ports, hash);

  return p;
}


/**
 * Get the channel handler for the channel specified by id from the given handle
 * @param h Cadet handle
 * @param chid ID of the wanted channel
 * @return handle to the required channel or NULL if not found
 */
static struct GNUNET_CADET_Channel *
retrieve_channel (struct GNUNET_CADET_Handle *h, CADET_ChannelNumber chid)
{
  struct GNUNET_CADET_Channel *ch;

  ch = h->channels_head;
  while (ch != NULL)
  {
    if (ch->chid == chid)
      return ch;
    ch = ch->next;
  }
  return NULL;
}


/**
 * Create a new channel and insert it in the channel list of the cadet handle
 *
 * @param h Cadet handle
 * @param chid Desired chid of the channel, 0 to assign one automatically.
 *
 * @return Handle to the created channel.
 */
static struct GNUNET_CADET_Channel *
create_channel (struct GNUNET_CADET_Handle *h, CADET_ChannelNumber chid)
{
  struct GNUNET_CADET_Channel *ch;

  ch = GNUNET_new (struct GNUNET_CADET_Channel);
  GNUNET_CONTAINER_DLL_insert (h->channels_head, h->channels_tail, ch);
  ch->cadet = h;
  if (0 == chid)
  {
    ch->chid = h->next_chid;
    while (NULL != retrieve_channel (h, h->next_chid))
    {
      h->next_chid++;
      h->next_chid &= ~GNUNET_CADET_LOCAL_CHANNEL_ID_SERV;
      h->next_chid |= GNUNET_CADET_LOCAL_CHANNEL_ID_CLI;
    }
  }
  else
  {
    ch->chid = chid;
  }
  ch->allow_send = GNUNET_NO;
  return ch;
}


/**
 * Destroy the specified channel.
 * - Destroys all peers, calling the disconnect callback on each if needed
 * - Cancels all outgoing traffic for that channel, calling respective notifys
 * - Calls cleaner if channel was inbound
 * - Frees all memory used
 *
 * @param ch Pointer to the channel.
 * @param call_cleaner Whether to call the cleaner handler.
 *
 * @return Handle to the required channel or NULL if not found.
 */
static void
destroy_channel (struct GNUNET_CADET_Channel *ch, int call_cleaner)
{
  struct GNUNET_CADET_Handle *h;
  struct GNUNET_CADET_TransmitHandle *th;
  struct GNUNET_CADET_TransmitHandle *next;

  LOG (GNUNET_ERROR_TYPE_DEBUG, " destroy_channel %X\n", ch->chid);

  if (NULL == ch)
  {
    GNUNET_break (0);
    return;
  }
  h = ch->cadet;

  GNUNET_CONTAINER_DLL_remove (h->channels_head, h->channels_tail, ch);

  /* signal channel destruction */
  if ( (NULL != h->cleaner) && (0 != ch->peer) && (GNUNET_YES == call_cleaner) )
  {
    LOG (GNUNET_ERROR_TYPE_DEBUG, " calling cleaner\n");
    h->cleaner (h->cls, ch, ch->ctx);
  }

  /* check that clients did not leave messages behind in the queue */
  for (th = h->th_head; NULL != th; th = next)
  {
    next = th->next;
    if (th->channel != ch)
      continue;
    /* Clients should have aborted their requests already.
     * Management traffic should be ok, as clients can't cancel that.
     * If the service crashed and we are reconnecting, it's ok.
     */
    GNUNET_break (GNUNET_NO == th_is_payload (th));
    GNUNET_CADET_notify_transmit_ready_cancel (th);
  }

  if (0 != ch->peer)
    GNUNET_PEER_change_rc (ch->peer, -1);
  GNUNET_free (ch);
  return;
}


/**
 * Add a transmit handle to the transmission queue and set the
 * timeout if needed.
 *
 * @param h cadet handle with the queue head and tail
 * @param th handle to the packet to be transmitted
 */
static void
add_to_queue (struct GNUNET_CADET_Handle *h,
              struct GNUNET_CADET_TransmitHandle *th)
{
  GNUNET_CONTAINER_DLL_insert_tail (h->th_head, h->th_tail, th);
}


/**
 * Send an ack on the channel to confirm the processing of a message.
 *
 * @param ch Channel on which to send the ACK.
 */
static void
send_ack (struct GNUNET_CADET_Channel *ch)
{
  struct GNUNET_CADET_LocalAck *msg;
  struct GNUNET_MQ_Envelope *env;

  env = GNUNET_MQ_msg (msg, GNUNET_MESSAGE_TYPE_CADET_LOCAL_ACK);

  LOG (GNUNET_ERROR_TYPE_DEBUG, "Sending ACK on channel %X\n", ch->chid);
  msg->channel_id = htonl (ch->chid);
  GNUNET_MQ_send (ch->cadet->mq, env);

  return;
}



/******************************************************************************/
/***********************      RECEIVE HANDLERS     ****************************/
/******************************************************************************/


/**
 * Call the @a notify callback given to #GNUNET_CADET_notify_transmit_ready to
 * request the data to send over MQ. Since MQ manages the queue, this function
 * is scheduled immediatly after a transmit ready notification.
 *
 * @param cls Closure (transmit handle).
 */
static void
request_data (void *cls)
{
  struct GNUNET_CADET_TransmitHandle *th = cls;
  struct GNUNET_CADET_LocalData *msg;
  struct GNUNET_MQ_Envelope *env;
  size_t osize;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "Requesting Data: %u bytes\n", th->size);
  th->request_data_task = NULL;
  th->channel->packet_size = 0;
  env = GNUNET_MQ_msg_extra (msg, th->size,
                             GNUNET_MESSAGE_TYPE_CADET_LOCAL_DATA);
  msg->id = htonl (th->channel->chid);
  osize = th->notify (th->notify_cls, th->size, &msg[1]);
  GNUNET_assert (osize == th->size);
  th->channel->allow_send = GNUNET_NO;
  GNUNET_MQ_send (th->channel->cadet->mq, env);
  GNUNET_CADET_notify_transmit_ready_cancel (th);
}


/**
 * Process the new channel notification and add it to the channels in the handle
 *
 * @param h     The cadet handle
 * @param msg   A message with the details of the new incoming channel
 */
static void
handle_channel_created (void *cls,
                        const struct GNUNET_CADET_ChannelCreateMessage *msg)
{
  struct GNUNET_CADET_Handle *h = cls;
  struct GNUNET_CADET_Channel *ch;
  struct GNUNET_CADET_Port *port;
  const struct GNUNET_HashCode *port_number;
  CADET_ChannelNumber chid;

  chid = ntohl (msg->channel_id);
  port_number = &msg->port;
  LOG (GNUNET_ERROR_TYPE_DEBUG, "Creating incoming channel %X [%s]\n",
       chid, GNUNET_h2s (port_number));
  if (chid < GNUNET_CADET_LOCAL_CHANNEL_ID_SERV)
  {
    GNUNET_break (0);
    return;
  }
  port = find_port (h, port_number);
  if (NULL != port)
  {
    void *ctx;

    ch = create_channel (h, chid);
    ch->allow_send = GNUNET_NO;
    ch->peer = GNUNET_PEER_intern (&msg->peer);
    ch->cadet = h;
    ch->chid = chid;
    ch->port = port;
    ch->options = ntohl (msg->opt);

    LOG (GNUNET_ERROR_TYPE_DEBUG, "  created channel %p\n", ch);
    ctx = port->handler (port->cls, ch, &msg->peer, port->hash, ch->options);
    if (NULL != ctx)
      ch->ctx = ctx;
    LOG (GNUNET_ERROR_TYPE_DEBUG, "User notified\n");
  }
  else
  {
    struct GNUNET_CADET_ChannelDestroyMessage *d_msg;
    struct GNUNET_MQ_Envelope *env;

    LOG (GNUNET_ERROR_TYPE_DEBUG, "No handler for incoming channels\n");
    env = GNUNET_MQ_msg (d_msg, GNUNET_MESSAGE_TYPE_CADET_CHANNEL_DESTROY);
    d_msg->channel_id = msg->channel_id;
    GNUNET_MQ_send (h->mq, env);
  }
  return;
}


/**
 * Process the channel destroy notification and free associated resources
 *
 * @param h     The cadet handle
 * @param msg   A message with the details of the channel being destroyed
 */
static void
handle_channel_destroy (void *cls,
                        const struct GNUNET_CADET_ChannelDestroyMessage *msg)
{
  struct GNUNET_CADET_Handle *h = cls;
  struct GNUNET_CADET_Channel *ch;
  CADET_ChannelNumber chid;

  chid = ntohl (msg->channel_id);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "Channel %X Destroy from service\n", chid);
  ch = retrieve_channel (h, chid);

  if (NULL == ch)
  {
    LOG (GNUNET_ERROR_TYPE_DEBUG, "channel %X unknown\n", chid);
    return;
  }
  destroy_channel (ch, GNUNET_YES);
}


/**
 * Check that message received from CADET service is well-formed.
 *
 * @param cls the `struct GNUNET_CADET_Handle`
 * @param message the message we got
 * @return #GNUNET_OK if the message is well-formed,
 *         #GNUNET_SYSERR otherwise
 */
static int
check_local_data (void *cls,
                  const struct GNUNET_CADET_LocalData *message)
{
  struct GNUNET_CADET_Handle *h = cls;
  struct GNUNET_CADET_Channel *ch;
  uint16_t size;

  size = ntohs (message->header.size);
  if (sizeof (*message) + sizeof (struct GNUNET_MessageHeader) > size)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }

  ch = retrieve_channel (h, ntohl (message->id));
  if (NULL == ch)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }

  return GNUNET_OK;
}


/**
 * Process the incoming data packets, call appropriate handlers.
 *
 * @param h       The cadet handle
 * @param message A message encapsulating the data
 */
static void
handle_local_data (void *cls,
                   const struct GNUNET_CADET_LocalData *message)
{
  struct GNUNET_CADET_Handle *h = cls;
  const struct GNUNET_MessageHeader *payload;
  const struct GNUNET_CADET_MessageHandler *handler;
  struct GNUNET_CADET_Channel *ch;
  unsigned int i;
  uint16_t type;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "Got a data message!\n");
  ch = retrieve_channel (h, ntohl (message->id));
  GNUNET_assert (NULL != ch);

  payload = (struct GNUNET_MessageHeader *) &message[1];
  LOG (GNUNET_ERROR_TYPE_DEBUG, "  %s data on channel %s [%X]\n",
       GC_f2s (ch->chid >= GNUNET_CADET_LOCAL_CHANNEL_ID_SERV),
       GNUNET_i2s (GNUNET_PEER_resolve2 (ch->peer)), ntohl (message->id));

  type = ntohs (payload->type);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "  payload type %s\n", GC_m2s (type));
  for (i = 0; i < h->n_handlers; i++)
  {
    handler = &h->message_handlers[i];
    LOG (GNUNET_ERROR_TYPE_DEBUG, "    checking handler for type %u\n",
         handler->type);
    if (handler->type == type)
    {
      if (GNUNET_OK !=
          handler->callback (h->cls, ch, &ch->ctx, payload))
      {
        LOG (GNUNET_ERROR_TYPE_DEBUG, "callback caused disconnection\n");
        GNUNET_CADET_channel_destroy (ch);
        break;
      }
      else
      {
        LOG (GNUNET_ERROR_TYPE_DEBUG,
             "callback completed successfully\n");
        break;
      }
    }
  }
}


/**
 * Process a local ACK message, enabling the client to send
 * more data to the service.
 *
 * @param h Cadet handle.
 * @param message Message itself.
 */
static void
handle_local_ack (void *cls,
                  const struct GNUNET_CADET_LocalAck *message)
{
  struct GNUNET_CADET_Handle *h = cls;
  struct GNUNET_CADET_Channel *ch;
  CADET_ChannelNumber chid;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "Got an ACK!\n");
  chid = ntohl (message->channel_id);
  ch = retrieve_channel (h, chid);
  if (NULL == ch)
  {
    LOG (GNUNET_ERROR_TYPE_DEBUG, "ACK on unknown channel %X\n", chid);
    return;
  }
  LOG (GNUNET_ERROR_TYPE_DEBUG, "  on channel %X!\n", ch->chid);
  ch->allow_send = GNUNET_YES;
  if (0 < ch->packet_size)
  {
    struct GNUNET_CADET_TransmitHandle *th;
    struct GNUNET_CADET_TransmitHandle *next;
    LOG (GNUNET_ERROR_TYPE_DEBUG,
         "  pending data, sending %U bytes!\n",
         ch->packet_size);
    for (th = h->th_head; NULL != th; th = next)
    {
      next = th->next;
      if (th->channel == ch)
      {
        th->request_data_task = GNUNET_SCHEDULER_add_now (&request_data, th);
        GNUNET_CONTAINER_DLL_remove (h->th_head, h->th_tail, th);
        break;
      }
    }
    /* Complain if we got thru all th without sending anything, ch was wrong */
    GNUNET_break (NULL != th);
  }
}

/**
 * Reconnect to the service, retransmit all infomation to try to restore the
 * original state.
 *
 * @param h handle to the cadet
 *
 * @return #GNUNET_YES in case of sucess, #GNUNET_NO otherwise (service down...)
 */
static void
reconnect (struct GNUNET_CADET_Handle *h);


/**
 * Reconnect callback: tries to reconnect again after a failer previous
 * reconnection.
 *
 * @param cls closure (cadet handle)
 */
static void
reconnect_cbk (void *cls);


/**
 * Generic error handler, called with the appropriate error code and
 * the same closure specified at the creation of the message queue.
 * Not every message queue implementation supports an error handler.
 *
 * @param cls closure, a `struct GNUNET_CORE_Handle *`
 * @param error error code
 */
static void
handle_mq_error (void *cls,
                 enum GNUNET_MQ_Error error)
{
  struct GNUNET_CADET_Handle *h = cls;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "MQ ERROR: %u\n", error);
  GNUNET_MQ_destroy (h->mq);
  h->mq = NULL;
  reconnect (h);
}


/**
 * Reconnect to the service, retransmit all infomation to try to restore the
 * original state.
 *
 * @param h handle to the cadet
 *
 * @return GNUNET_YES in case of sucess, GNUNET_NO otherwise (service down...)
 */
static int
do_reconnect (struct GNUNET_CADET_Handle *h)
{
  GNUNET_MQ_hd_fixed_size (channel_created,
                           GNUNET_MESSAGE_TYPE_CADET_CHANNEL_CREATE,
                           struct GNUNET_CADET_ChannelCreateMessage);
  GNUNET_MQ_hd_fixed_size (channel_destroy,
                           GNUNET_MESSAGE_TYPE_CADET_CHANNEL_DESTROY,
                           struct GNUNET_CADET_ChannelDestroyMessage);
  GNUNET_MQ_hd_var_size (local_data,
                         GNUNET_MESSAGE_TYPE_CADET_LOCAL_DATA,
                         struct GNUNET_CADET_LocalData);
  GNUNET_MQ_hd_fixed_size (local_ack,
                           GNUNET_MESSAGE_TYPE_CADET_LOCAL_ACK,
                           struct GNUNET_CADET_LocalAck);
  // FIXME
//   GNUNET_MQ_hd_fixed_Y       size (channel_destroyed,
//                            GNUNET_MESSAGE_TYPE_CADET_CHANNEL_NACK,
//                            struct GNUNET_CADET_ChannelDestroyMessage);
  struct GNUNET_MQ_MessageHandler handlers[] = {
    make_channel_created_handler (h),
    make_channel_destroy_handler (h),
    make_local_data_handler (h),
    make_local_ack_handler (h),
    GNUNET_MQ_handler_end ()
  };

  LOG (GNUNET_ERROR_TYPE_DEBUG, "Connecting to CADET\n");

  GNUNET_assert (NULL == h->mq);
  h->mq = GNUNET_CLIENT_connecT (h->cfg,
                                 "cadet",
                                 handlers,
                                 &handle_mq_error,
                                 h);
  if (NULL == h->mq)
  {
    reconnect (h);
    return GNUNET_NO;
  }
  else
  {
    h->reconnect_time = GNUNET_TIME_UNIT_MILLISECONDS;
  }
  return GNUNET_YES;
}

/**
 * Reconnect callback: tries to reconnect again after a failer previous
 * reconnecttion
 *
 * @param cls closure (cadet handle)
 */
static void
reconnect_cbk (void *cls)
{
  struct GNUNET_CADET_Handle *h = cls;

  h->reconnect_task = NULL;
  do_reconnect (h);
}


/**
 * Reconnect to the service, retransmit all infomation to try to restore the
 * original state.
 *
 * @param h handle to the cadet
 *
 * @return #GNUNET_YES in case of sucess, #GNUNET_NO otherwise (service down...)
 */
static void
reconnect (struct GNUNET_CADET_Handle *h)
{
  struct GNUNET_CADET_Channel *ch;

  LOG (GNUNET_ERROR_TYPE_DEBUG,
       "Requested RECONNECT, destroying all channels\n");
  for (ch = h->channels_head; NULL != ch; ch = h->channels_head)
    destroy_channel (ch, GNUNET_YES);
  if (NULL == h->reconnect_task)
    h->reconnect_task = GNUNET_SCHEDULER_add_delayed (h->reconnect_time,
                                                      &reconnect_cbk, h);
}


/*
 * Process a local reply about info on all channels, pass info to the user.
 *
 * @param h Cadet handle.
 * @param message Message itself.
 */
// static void
// process_get_channels (struct GNUNET_CADET_Handle *h,
//                      const struct GNUNET_MessageHeader *message)
// {
//   struct GNUNET_CADET_LocalInfo *msg;
//
//   GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "Get Channels messasge received\n");
//
//   if (NULL == h->channels_cb)
//   {
//     GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "  ignored\n");
//     return;
//   }
//
//   msg = (struct GNUNET_CADET_LocalInfo *) message;
//   if (ntohs (message->size) !=
//       (sizeof (struct GNUNET_CADET_LocalInfo) +
//        sizeof (struct GNUNET_PeerIdentity)))
//   {
//     GNUNET_break_op (0);
//     GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
//                 "Get channels message: size %hu - expected %u\n",
//                 ntohs (message->size),
//                 sizeof (struct GNUNET_CADET_LocalInfo));
//     return;
//   }
//   h->channels_cb (h->channels_cls,
//                   ntohl (msg->channel_id),
//                   &msg->owner,
//                   &msg->destination);
// }



/*
 * Process a local monitor_channel reply, pass info to the user.
 *
 * @param h Cadet handle.
 * @param message Message itself.
 */
// static void
// process_show_channel (struct GNUNET_CADET_Handle *h,
//                      const struct GNUNET_MessageHeader *message)
// {
//   struct GNUNET_CADET_LocalInfo *msg;
//   size_t esize;
//
//   GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "Show Channel messasge received\n");
//
//   if (NULL == h->channel_cb)
//   {
//     GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "  ignored\n");
//     return;
//   }
//
//   /* Verify message sanity */
//   msg = (struct GNUNET_CADET_LocalInfo *) message;
//   esize = sizeof (struct GNUNET_CADET_LocalInfo);
//   if (ntohs (message->size) != esize)
//   {
//     GNUNET_break_op (0);
//     GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
//                 "Show channel message: size %hu - expected %u\n",
//                 ntohs (message->size),
//                 esize);
//
//     h->channel_cb (h->channel_cls, NULL, NULL);
//     h->channel_cb = NULL;
//     h->channel_cls = NULL;
//
//     return;
//   }
//
//   h->channel_cb (h->channel_cls,
//                  &msg->destination,
//                  &msg->owner);
// }



/**
 * Process a local reply about info on all tunnels, pass info to the user.
 *
 * @param h Cadet handle.
 * @param message Message itself.
 */
static void
process_get_peers (struct GNUNET_CADET_Handle *h,
                     const struct GNUNET_MessageHeader *message)
{
  struct GNUNET_CADET_LocalInfoPeer *msg;
  uint16_t size;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Get Peer messasge received\n");

  if (NULL == h->info_cb.peers_cb)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "  ignored\n");
    return;
  }

  size = ntohs (message->size);
  if (sizeof (struct GNUNET_CADET_LocalInfoPeer) > size)
  {
    h->info_cb.peers_cb (h->info_cls, NULL, -1, 0, 0);
    h->info_cb.peers_cb = NULL;
    h->info_cls = NULL;
    return;
  }

  msg = (struct GNUNET_CADET_LocalInfoPeer *) message;
  h->info_cb.peers_cb (h->info_cls, &msg->destination,
                       (int) ntohs (msg->tunnel),
                       (unsigned int ) ntohs (msg->paths),
                       0);
}


/**
 * Process a local peer info reply, pass info to the user.
 *
 * @param h Cadet handle.
 * @param message Message itself.
 */
static void
process_get_peer (struct GNUNET_CADET_Handle *h,
                  const struct GNUNET_MessageHeader *message)
{
  struct GNUNET_CADET_LocalInfoPeer *msg;
  struct GNUNET_PeerIdentity *id;
  unsigned int epaths;
  unsigned int paths;
  unsigned int path_length;
  unsigned int i;
  int neighbor;
  size_t esize;
  size_t msize;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Info Peer messasge received\n");
  if (NULL == h->info_cb.peer_cb)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "  ignored\n");
    return;
  }

  /* Verify message sanity */
  msg = (struct GNUNET_CADET_LocalInfoPeer *) message;
  esize = ntohs (message->size);
  msize = sizeof (struct GNUNET_CADET_LocalInfoPeer);
  if (esize < msize)
  {
    GNUNET_break_op (0);
    h->info_cb.peer_cb (h->info_cls, NULL, 0, 0, 0, NULL);
    goto clean_cls;
  }
  epaths = (unsigned int) ntohs (msg->paths);
  paths = 0;
  path_length = 0;
  neighbor = GNUNET_NO;
  id = (struct GNUNET_PeerIdentity *) &msg[1];
  for (i = 0; msize < esize; i++)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, " %s\n", GNUNET_i2s (&id[i]));
    msize += sizeof (struct GNUNET_PeerIdentity);
    path_length++;
    if (0 == memcmp (&id[i], &msg->destination,
                     sizeof (struct GNUNET_PeerIdentity)))
    {
      if (1 == path_length)
        neighbor = GNUNET_YES;
      path_length = 0;
      paths++;
    }
  }
  if (msize != esize)
  {
    GNUNET_break_op (0);
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "m:%u, e: %u\n",
                (unsigned int) msize,
                (unsigned int) esize);
    h->info_cb.peer_cb (h->info_cls, NULL, 0, 0, 0, NULL);
    goto clean_cls;
  }
  if (paths != epaths)
  {
    GNUNET_break_op (0);
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING, "p:%u, e: %u\n", paths, epaths);
    h->info_cb.peer_cb (h->info_cls, NULL, 0, 0, 0, NULL);
    goto clean_cls;
  }

  /* Call Callback with tunnel info. */
  id = (struct GNUNET_PeerIdentity *) &msg[1];
  h->info_cb.peer_cb (h->info_cls,
                      &msg->destination,
                      (int) ntohs (msg->tunnel),
                      neighbor,
                      paths,
                      id);

  clean_cls:
  h->info_cb.peer_cb = NULL;
  h->info_cls = NULL;
}


/**
 * Process a local reply about info on all tunnels, pass info to the user.
 *
 * @param h Cadet handle.
 * @param message Message itself.
 */
static void
process_get_tunnels (struct GNUNET_CADET_Handle *h,
                     const struct GNUNET_MessageHeader *message)
{
  struct GNUNET_CADET_LocalInfoTunnel *msg;
  uint16_t size;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Get Tunnels messasge received\n");

  if (NULL == h->info_cb.tunnels_cb)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "  ignored\n");
    return;
  }

  size = ntohs (message->size);
  if (sizeof (struct GNUNET_CADET_LocalInfoTunnel) > size)
  {
    h->info_cb.tunnels_cb (h->info_cls, NULL, 0, 0, 0, 0);
    h->info_cb.tunnels_cb = NULL;
    h->info_cls = NULL;
    return;
  }

  msg = (struct GNUNET_CADET_LocalInfoTunnel *) message;
  h->info_cb.tunnels_cb (h->info_cls, &msg->destination,
                         ntohl (msg->channels), ntohl (msg->connections),
                         ntohs (msg->estate), ntohs (msg->cstate));

}


/**
 * Process a local tunnel info reply, pass info to the user.
 *
 * @param h Cadet handle.
 * @param message Message itself.
 */
static void
process_get_tunnel (struct GNUNET_CADET_Handle *h,
                    const struct GNUNET_MessageHeader *message)
{
  struct GNUNET_CADET_LocalInfoTunnel *msg;
  size_t esize;
  size_t msize;
  unsigned int ch_n;
  unsigned int c_n;
  struct GNUNET_CADET_Hash *conns;
  CADET_ChannelNumber *chns;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Get Tunnel messasge received\n");
  if (NULL == h->info_cb.tunnel_cb)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "  ignored\n");
    return;
  }

  /* Verify message sanity */
  msg = (struct GNUNET_CADET_LocalInfoTunnel *) message;
  msize = ntohs (message->size);
  esize = sizeof (struct GNUNET_CADET_LocalInfoTunnel);
  if (esize > msize)
  {
    GNUNET_break_op (0);
    h->info_cb.tunnel_cb (h->info_cls, NULL, 0, 0, NULL, NULL, 0, 0);
    goto clean_cls;
  }
  ch_n = ntohl (msg->channels);
  c_n = ntohl (msg->connections);
  esize += ch_n * sizeof (CADET_ChannelNumber);
  esize += c_n * sizeof (struct GNUNET_CADET_Hash);
  if (msize != esize)
  {
    GNUNET_break_op (0);
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "m:%u, e: %u (%u ch, %u conn)\n",
                (unsigned int) msize,
                (unsigned int) esize,
                ch_n,
                c_n);
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "%u (%u ch, %u conn)\n",
                (unsigned int) sizeof (struct GNUNET_CADET_LocalInfoTunnel),
                (unsigned int) sizeof (CADET_ChannelNumber),
                (unsigned int) sizeof (struct GNUNET_HashCode));
    h->info_cb.tunnel_cb (h->info_cls, NULL, 0, 0, NULL, NULL, 0, 0);
    goto clean_cls;
  }

  /* Call Callback with tunnel info. */
  conns = (struct GNUNET_CADET_Hash *) &msg[1];
  chns = (CADET_ChannelNumber *) &conns[c_n];
  h->info_cb.tunnel_cb (h->info_cls, &msg->destination,
                ch_n, c_n, chns, conns,
                ntohs (msg->estate), ntohs (msg->cstate));

clean_cls:
  h->info_cb.tunnel_cb = NULL;
  h->info_cls = NULL;
}


// FIXME: add monitor messages to mq
// static void
// msg_received (void *cls, const struct GNUNET_MessageHeader *msg)
// {
//   struct GNUNET_CADET_Handle *h = cls;
//   uint16_t type;
//
//   if (msg == NULL)
//   {
//     LOG (GNUNET_ERROR_TYPE_DEBUG,
// 	 "Cadet service disconnected, reconnecting\n", h);
//     reconnect (h);
//     return;
//   }
//   type = ntohs (msg->type);
//   LOG (GNUNET_ERROR_TYPE_DEBUG, "\n");
//   LOG (GNUNET_ERROR_TYPE_DEBUG, "Received a message: %s\n",
//        GC_m2s (type));
//   switch (type)
//   {
//     /* Notify of a new incoming channel */
//   case GNUNET_MESSAGE_TYPE_CADET_CHANNEL_CREATE:
// //     process_channel_created (h,
// // 			     (struct GNUNET_CADET_ChannelCreateMessage *) msg);
//     break;
//     /* Notify of a channel disconnection */
//   case GNUNET_MESSAGE_TYPE_CADET_CHANNEL_DESTROY: /* TODO separate(gid problem)*/
//   case GNUNET_MESSAGE_TYPE_CADET_CHANNEL_NACK:
// //     process_channel_destroy (h,
// // 			     (struct GNUNET_CADET_ChannelDestroyMessage *) msg);
//     break;
//   case GNUNET_MESSAGE_TYPE_CADET_LOCAL_DATA:
// //     process_incoming_data (h, msg);
//     break;
//   case GNUNET_MESSAGE_TYPE_CADET_LOCAL_ACK:
// //     process_ack (h, msg);
//     break;
// //   case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_CHANNELS:
// //     process_get_channels (h, msg);
// //     break;
// //   case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_CHANNEL:
// //     process_show_channel (h, msg);
// //     break;
//   case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_PEERS:
//     process_get_peers (h, msg);
//     break;
//   case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_PEER:
//     process_get_peer (h, msg);
//     break;
//   case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_TUNNELS:
//     process_get_tunnels (h, msg);
//     break;
//   case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_TUNNEL:
//     process_get_tunnel (h, msg);
//     break;
// //   case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_CHANNEL:
// //     process_show_channel (h, msg);
// //     break;
//   default:
//     /* We shouldn't get any other packages, log and ignore */
//     LOG (GNUNET_ERROR_TYPE_WARNING,
//          "unsolicited message form service (type %s)\n",
//          GC_m2s (ntohs (msg->type)));
//   }
//   LOG (GNUNET_ERROR_TYPE_DEBUG, "message processed\n");
//   GNUNET_CLIENT_receive (h->client, &msg_received, h,
//                          GNUNET_TIME_UNIT_FOREVER_REL);
// }


/******************************************************************************/
/**********************      API CALL DEFINITIONS     *************************/
/******************************************************************************/

struct GNUNET_CADET_Handle *
GNUNET_CADET_connect (const struct GNUNET_CONFIGURATION_Handle *cfg, void *cls,
                     GNUNET_CADET_ChannelEndHandler cleaner,
                     const struct GNUNET_CADET_MessageHandler *handlers)
{
  struct GNUNET_CADET_Handle *h;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "GNUNET_CADET_connect()\n");
  h = GNUNET_new (struct GNUNET_CADET_Handle);
  LOG (GNUNET_ERROR_TYPE_DEBUG, " addr %p\n", h);
  h->cfg = cfg;
  h->cleaner = cleaner;
  h->ports = GNUNET_CONTAINER_multihashmap_create (4, GNUNET_YES);
  do_reconnect (h);
  if (h->mq == NULL)
  {
    GNUNET_break (0);
    GNUNET_CADET_disconnect (h);
    return NULL;
  }
  h->cls = cls;
  h->message_handlers = handlers;
  h->next_chid = GNUNET_CADET_LOCAL_CHANNEL_ID_CLI;
  h->reconnect_time = GNUNET_TIME_UNIT_MILLISECONDS;
  h->reconnect_task = NULL;

  /* count handlers */
  for (h->n_handlers = 0;
       handlers && handlers[h->n_handlers].type;
       h->n_handlers++) ;
  LOG (GNUNET_ERROR_TYPE_DEBUG, "GNUNET_CADET_connect() END\n");
  return h;
}


void
GNUNET_CADET_disconnect (struct GNUNET_CADET_Handle *handle)
{
  struct GNUNET_CADET_Channel *ch;
  struct GNUNET_CADET_Channel *aux;
  struct GNUNET_CADET_TransmitHandle *th;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "CADET DISCONNECT\n");

  ch = handle->channels_head;
  while (NULL != ch)
  {
    aux = ch->next;
    if (ch->chid < GNUNET_CADET_LOCAL_CHANNEL_ID_SERV)
    {
      GNUNET_break (0);
      LOG (GNUNET_ERROR_TYPE_DEBUG, "channel %X not destroyed\n", ch->chid);
    }
    destroy_channel (ch, GNUNET_YES);
    ch = aux;
  }
  while ( (th = handle->th_head) != NULL)
  {
    struct GNUNET_MessageHeader *msg;

    /* Make sure it is an allowed packet (everything else should have been
     * already canceled).
     */
    GNUNET_break (GNUNET_NO == th_is_payload (th));
    msg = (struct GNUNET_MessageHeader *) &th[1];
    switch (ntohs(msg->type))
    {
      case GNUNET_MESSAGE_TYPE_CADET_CHANNEL_CREATE:
      case GNUNET_MESSAGE_TYPE_CADET_CHANNEL_DESTROY:
      case GNUNET_MESSAGE_TYPE_CADET_LOCAL_PORT_OPEN:
      case GNUNET_MESSAGE_TYPE_CADET_LOCAL_PORT_CLOSE:
      case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_CHANNELS:
      case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_CHANNEL:
      case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_PEER:
      case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_PEERS:
      case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_TUNNEL:
      case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_TUNNELS:
        break;
      default:
        GNUNET_break (0);
        LOG (GNUNET_ERROR_TYPE_ERROR, "unexpected unsent msg %s\n",
             GC_m2s (ntohs(msg->type)));
    }

    GNUNET_CADET_notify_transmit_ready_cancel (th);
  }

  if (NULL != handle->mq)
  {
    GNUNET_MQ_destroy (handle->mq);
    handle->mq = NULL;
  }
  if (NULL != handle->reconnect_task)
  {
    GNUNET_SCHEDULER_cancel(handle->reconnect_task);
    handle->reconnect_task = NULL;
  }

  GNUNET_CONTAINER_multihashmap_destroy (handle->ports);
  handle->ports = NULL;
  GNUNET_free (handle);
}


/**
 * Open a port to receive incomming channels.
 *
 * @param h CADET handle.
 * @param port Hash representing the port number.
 * @param new_channel Function called when an channel is received.
 * @param new_channel_cls Closure for @a new_channel.
 *
 * @return Port handle.
 */
struct GNUNET_CADET_Port *
GNUNET_CADET_open_port (struct GNUNET_CADET_Handle *h,
			const struct GNUNET_HashCode *port,
			GNUNET_CADET_InboundChannelNotificationHandler
			    new_channel,
			void *new_channel_cls)
{
  struct GNUNET_CADET_PortMessage *msg;
  struct GNUNET_MQ_Envelope *env;
  struct GNUNET_CADET_Port *p;

  GNUNET_assert (NULL != new_channel);
  p = GNUNET_new (struct GNUNET_CADET_Port);
  p->cadet = h;
  p->hash = GNUNET_new (struct GNUNET_HashCode);
  *p->hash = *port;
  p->handler = new_channel;
  p->cls = new_channel_cls;
  GNUNET_assert (GNUNET_OK ==
		 GNUNET_CONTAINER_multihashmap_put (h->ports,
						    p->hash,
						    p,
						    GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY));

  env = GNUNET_MQ_msg (msg, GNUNET_MESSAGE_TYPE_CADET_LOCAL_PORT_OPEN);
  msg->port = *p->hash;
  GNUNET_MQ_send (h->mq, env);

  return p;
}

/**
 * Close a port opened with @a GNUNET_CADET_open_port.
 * The @a new_channel callback will no longer be called.
 *
 * @param p Port handle.
 */
void
GNUNET_CADET_close_port (struct GNUNET_CADET_Port *p)
{
  struct GNUNET_CADET_PortMessage *msg;
  struct GNUNET_MQ_Envelope *env;

  env = GNUNET_MQ_msg (msg, GNUNET_MESSAGE_TYPE_CADET_LOCAL_PORT_CLOSE);

  msg->port = *p->hash;
  GNUNET_MQ_send (p->cadet->mq, env);
  GNUNET_CONTAINER_multihashmap_remove (p->cadet->ports, p->hash, p);
  GNUNET_free (p->hash);
  GNUNET_free (p);
}


/**
 * Create a new channel towards a remote peer.
 *
 * If the destination port is not open by any peer or the destination peer
 * does not accept the channel, #GNUNET_CADET_ChannelEndHandler will be called
 * for this channel.
 *
 * @param h cadet handle
 * @param channel_ctx client's channel context to associate with the channel
 * @param peer peer identity the channel should go to
 * @param port Port hash (port number).
 * @param options CadetOption flag field, with all desired option bits set to 1.
 *
 * @return handle to the channel
 */
struct GNUNET_CADET_Channel *
GNUNET_CADET_channel_create (struct GNUNET_CADET_Handle *h,
                            void *channel_ctx,
                            const struct GNUNET_PeerIdentity *peer,
                            const struct GNUNET_HashCode *port,
                            enum GNUNET_CADET_ChannelOption options)
{
  struct GNUNET_CADET_ChannelCreateMessage *msg;
  struct GNUNET_MQ_Envelope *env;
  struct GNUNET_CADET_Channel *ch;

  LOG (GNUNET_ERROR_TYPE_DEBUG,
       "Creating new channel to %s:%u\n",
       GNUNET_i2s (peer), port);
  ch = create_channel (h, 0);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "  at %p\n", ch);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "  number %X\n", ch->chid);
  ch->ctx = channel_ctx;
  ch->peer = GNUNET_PEER_intern (peer);

  env = GNUNET_MQ_msg (msg, GNUNET_MESSAGE_TYPE_CADET_CHANNEL_CREATE);
  msg->channel_id = htonl (ch->chid);
  msg->port = *port;
  msg->peer = *peer;
  msg->opt = htonl (options);
  ch->allow_send = GNUNET_NO;
  GNUNET_MQ_send (h->mq, env);

  return ch;
}


void
GNUNET_CADET_channel_destroy (struct GNUNET_CADET_Channel *channel)
{
  struct GNUNET_CADET_Handle *h;
  struct GNUNET_CADET_ChannelDestroyMessage *msg;
  struct GNUNET_MQ_Envelope *env;
  struct GNUNET_CADET_TransmitHandle *th;
  struct GNUNET_CADET_TransmitHandle *next;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "Destroying channel\n");
  h = channel->cadet;
  for  (th = h->th_head; th != NULL; th = next)
  {
    next = th->next;
    if (th->channel == channel)
    {
      GNUNET_break (0);
      if (GNUNET_YES == th_is_payload (th))
      {
        /* applications should cancel before destroying channel */
        LOG (GNUNET_ERROR_TYPE_WARNING,
             "Channel destroyed without cancelling transmission requests\n");
        th->notify (th->notify_cls, 0, NULL);
      }
      else
      {
        LOG (GNUNET_ERROR_TYPE_WARNING, "no meta-traffic should be queued\n");
      }
      GNUNET_CONTAINER_DLL_remove (h->th_head, h->th_tail, th);
      GNUNET_CADET_notify_transmit_ready_cancel (th);
    }
  }

  env = GNUNET_MQ_msg (msg, GNUNET_MESSAGE_TYPE_CADET_CHANNEL_DESTROY);
  msg->channel_id = htonl (channel->chid);
  GNUNET_MQ_send (h->mq, env);

  destroy_channel (channel, GNUNET_YES);
}


/**
 * Get information about a channel.
 *
 * @param channel Channel handle.
 * @param option Query (GNUNET_CADET_OPTION_*).
 * @param ... dependant on option, currently not used
 *
 * @return Union with an answer to the query.
 */
const union GNUNET_CADET_ChannelInfo *
GNUNET_CADET_channel_get_info (struct GNUNET_CADET_Channel *channel,
                              enum GNUNET_CADET_ChannelOption option, ...)
{
  static int bool_flag;
  const union GNUNET_CADET_ChannelInfo *ret;

  switch (option)
  {
    case GNUNET_CADET_OPTION_NOBUFFER:
    case GNUNET_CADET_OPTION_RELIABLE:
    case GNUNET_CADET_OPTION_OOORDER:
      if (0 != (option & channel->options))
        bool_flag = GNUNET_YES;
      else
        bool_flag = GNUNET_NO;
      ret = (const union GNUNET_CADET_ChannelInfo *) &bool_flag;
      break;
    case GNUNET_CADET_OPTION_PEER:
      ret = (const union GNUNET_CADET_ChannelInfo *) GNUNET_PEER_resolve2 (channel->peer);
      break;
    default:
      GNUNET_break (0);
      return NULL;
  }

  return ret;
}


struct GNUNET_CADET_TransmitHandle *
GNUNET_CADET_notify_transmit_ready (struct GNUNET_CADET_Channel *channel,
                                    int cork,
                                    struct GNUNET_TIME_Relative maxdelay,
                                    size_t notify_size,
                                    GNUNET_CONNECTION_TransmitReadyNotify notify,
                                    void *notify_cls)
{
  struct GNUNET_CADET_TransmitHandle *th;

  GNUNET_assert (NULL != channel);
  GNUNET_assert (GNUNET_CONSTANTS_MAX_CADET_MESSAGE_SIZE >= notify_size);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "CADET NOTIFY TRANSMIT READY\n");
  LOG (GNUNET_ERROR_TYPE_DEBUG, "    on channel %X\n", channel->chid);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "    allow_send %d\n", channel->allow_send);
  if (channel->chid >= GNUNET_CADET_LOCAL_CHANNEL_ID_SERV)
    LOG (GNUNET_ERROR_TYPE_DEBUG, "    to origin\n");
  else
    LOG (GNUNET_ERROR_TYPE_DEBUG, "    to destination\n");
  LOG (GNUNET_ERROR_TYPE_DEBUG, "    payload size %u\n", notify_size);
  GNUNET_assert (NULL != notify);
  GNUNET_assert (0 == channel->packet_size); // Only one data packet allowed

  if (GNUNET_TIME_UNIT_FOREVER_REL.rel_value_us != maxdelay.rel_value_us)
  {
    LOG (GNUNET_ERROR_TYPE_WARNING,
         "CADET transmit ready timeout is deprected (has no effect)\n");
  }

  th = GNUNET_new (struct GNUNET_CADET_TransmitHandle);
  th->channel = channel;
  th->size = notify_size;
  channel->packet_size = th->size;
  LOG (GNUNET_ERROR_TYPE_DEBUG, "    total size %u\n", th->size);
  th->notify = notify;
  th->notify_cls = notify_cls;
  if (GNUNET_YES == channel->allow_send)
    th->request_data_task = GNUNET_SCHEDULER_add_now (&request_data, th);
  else
    add_to_queue (channel->cadet, th);

  LOG (GNUNET_ERROR_TYPE_DEBUG, "CADET NOTIFY TRANSMIT READY END\n");
  return th;
}


void
GNUNET_CADET_notify_transmit_ready_cancel (struct GNUNET_CADET_TransmitHandle *th)
{
  if (NULL != th->request_data_task)
  {
    GNUNET_SCHEDULER_cancel (th->request_data_task);
  }
  th->request_data_task = NULL;

  /* It might or might not have been queued (rarely not), but check anyway. */
  if (NULL != th->next)
  {
    struct GNUNET_CADET_Handle *h;
    h = th->channel->cadet;
    GNUNET_CONTAINER_DLL_remove (h->th_head, h->th_tail, th);
  }
  GNUNET_free (th);
}


void
GNUNET_CADET_receive_done (struct GNUNET_CADET_Channel *channel)
{
  send_ack (channel);
}


static void
send_info_request (struct GNUNET_CADET_Handle *h, uint16_t type)
{
  struct GNUNET_MessageHeader *msg;
  struct GNUNET_MQ_Envelope *env;

  env = GNUNET_MQ_msg (msg, type);
  GNUNET_MQ_send (h->mq, env);

  LOG (GNUNET_ERROR_TYPE_DEBUG,
       " Sending %s message to service\n",
       GC_m2s(type));
}


/**
 * Request a debug dump on the service's STDERR.
 *
 * WARNING: unstable API, likely to change in the future!
 *
 * @param h cadet handle
 */
void
GNUNET_CADET_request_dump (struct GNUNET_CADET_Handle *h)
{
  LOG (GNUNET_ERROR_TYPE_DEBUG, "requesting dump\n");
  send_info_request (h, GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_DUMP);
}


/**
 * Request information about peers known to the running cadet service.
 * The callback will be called for every peer known to the service.
 * Only one info request (of any kind) can be active at once.
 *
 *
 * WARNING: unstable API, likely to change in the future!
 *
 * @param h Handle to the cadet peer.
 * @param callback Function to call with the requested data.
 * @param callback_cls Closure for @c callback.
 *
 * @return #GNUNET_OK / #GNUNET_SYSERR
 */
int
GNUNET_CADET_get_peers (struct GNUNET_CADET_Handle *h,
                       GNUNET_CADET_PeersCB callback,
                       void *callback_cls)
{
  if (NULL != h->info_cb.peers_cb)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  send_info_request (h, GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_PEERS);
  h->info_cb.peers_cb = callback;
  h->info_cls = callback_cls;
  return GNUNET_OK;
}


/**
 * Cancel a peer info request. The callback will not be called (anymore).
 *
 * WARNING: unstable API, likely to change in the future!
 *
 * @param h Cadet handle.
 *
 * @return Closure given to GNUNET_CADET_get_peers.
 */
void *
GNUNET_CADET_get_peers_cancel (struct GNUNET_CADET_Handle *h)
{
  void *cls;

  cls = h->info_cls;
  h->info_cb.peers_cb = NULL;
  h->info_cls = NULL;
  return cls;
}


/**
 * Request information about a peer known to the running cadet peer.
 * The callback will be called for the tunnel once.
 * Only one info request (of any kind) can be active at once.
 *
 * WARNING: unstable API, likely to change in the future!
 *
 * @param h Handle to the cadet peer.
 * @param id Peer whose tunnel to examine.
 * @param callback Function to call with the requested data.
 * @param callback_cls Closure for @c callback.
 *
 * @return #GNUNET_OK / #GNUNET_SYSERR
 */
int
GNUNET_CADET_get_peer (struct GNUNET_CADET_Handle *h,
                       const struct GNUNET_PeerIdentity *id,
                       GNUNET_CADET_PeerCB callback,
                       void *callback_cls)
{
  struct GNUNET_CADET_LocalInfo *msg;
  struct GNUNET_MQ_Envelope *env;

  if (NULL != h->info_cb.peer_cb)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  env = GNUNET_MQ_msg (msg, GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_PEER);
  msg->peer = *id;
  GNUNET_MQ_send (h->mq, env);

  h->info_cb.peer_cb = callback;
  h->info_cls = callback_cls;
  return GNUNET_OK;
}


/**
 * Request information about tunnels of the running cadet peer.
 * The callback will be called for every tunnel of the service.
 * Only one info request (of any kind) can be active at once.
 *
 * WARNING: unstable API, likely to change in the future!
 *
 * @param h Handle to the cadet peer.
 * @param callback Function to call with the requested data.
 * @param callback_cls Closure for @c callback.
 *
 * @return #GNUNET_OK / #GNUNET_SYSERR
 */
int
GNUNET_CADET_get_tunnels (struct GNUNET_CADET_Handle *h,
                         GNUNET_CADET_TunnelsCB callback,
                         void *callback_cls)
{
  if (NULL != h->info_cb.tunnels_cb)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  send_info_request (h, GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_TUNNELS);
  h->info_cb.tunnels_cb = callback;
  h->info_cls = callback_cls;
  return GNUNET_OK;
}


/**
 * Cancel a monitor request. The monitor callback will not be called.
 *
 * @param h Cadet handle.
 *
 * @return Closure given to GNUNET_CADET_get_tunnels.
 */
void *
GNUNET_CADET_get_tunnels_cancel (struct GNUNET_CADET_Handle *h)
{
  void *cls;

  h->info_cb.tunnels_cb = NULL;
  cls = h->info_cls;
  h->info_cls = NULL;

  return cls;
}



/**
 * Request information about a tunnel of the running cadet peer.
 * The callback will be called for the tunnel once.
 * Only one info request (of any kind) can be active at once.
 *
 * WARNING: unstable API, likely to change in the future!
 *
 * @param h Handle to the cadet peer.
 * @param id Peer whose tunnel to examine.
 * @param callback Function to call with the requested data.
 * @param callback_cls Closure for @c callback.
 *
 * @return #GNUNET_OK / #GNUNET_SYSERR
 */
int
GNUNET_CADET_get_tunnel (struct GNUNET_CADET_Handle *h,
                        const struct GNUNET_PeerIdentity *id,
                        GNUNET_CADET_TunnelCB callback,
                        void *callback_cls)
{
  struct GNUNET_CADET_LocalInfo *msg;
  struct GNUNET_MQ_Envelope *env;

  if (NULL != h->info_cb.tunnel_cb)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  env = GNUNET_MQ_msg (msg, GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_TUNNEL);
  msg->peer = *id;
  GNUNET_MQ_send (h->mq, env);

  h->info_cb.tunnel_cb = callback;
  h->info_cls = callback_cls;
  return GNUNET_OK;
}


/**
 * Request information about a specific channel of the running cadet peer.
 *
 * WARNING: unstable API, likely to change in the future!
 * FIXME Add destination option.
 *
 * @param h Handle to the cadet peer.
 * @param initiator ID of the owner of the channel.
 * @param channel_number Channel number.
 * @param callback Function to call with the requested data.
 * @param callback_cls Closure for @c callback.
 *
 * @return #GNUNET_OK / #GNUNET_SYSERR
 */
int
GNUNET_CADET_show_channel (struct GNUNET_CADET_Handle *h,
                           struct GNUNET_PeerIdentity *initiator,
                           unsigned int channel_number,
                           GNUNET_CADET_ChannelCB callback,
                           void *callback_cls)
{
  struct GNUNET_CADET_LocalInfo *msg;
  struct GNUNET_MQ_Envelope *env;

  if (NULL != h->info_cb.channel_cb)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  env = GNUNET_MQ_msg (msg, GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_CHANNEL);
  msg->peer = *initiator;
  msg->channel_id = htonl (channel_number);
  GNUNET_MQ_send (h->mq, env);

  h->info_cb.channel_cb = callback;
  h->info_cls = callback_cls;
  return GNUNET_OK;
}


/**
 * Function called to notify a client about the connection
 * begin ready to queue more data.  "buf" will be
 * NULL and "size" zero if the connection was closed for
 * writing in the meantime.
 *
 * @param cls closure
 * @param size number of bytes available in buf
 * @param buf where the callee should write the message
 * @return number of bytes written to buf
 */
static size_t
cadet_mq_ntr (void *cls, size_t size,
             void *buf)
{
  struct GNUNET_MQ_Handle *mq = cls;
  struct CadetMQState *state = GNUNET_MQ_impl_state (mq);
  const struct GNUNET_MessageHeader *msg = GNUNET_MQ_impl_current (mq);
  uint16_t msize;

  state->th = NULL;
  if (NULL == buf)
  {
    GNUNET_MQ_inject_error (mq, GNUNET_MQ_ERROR_WRITE);
    return 0;
  }
  msize = ntohs (msg->size);
  GNUNET_assert (msize <= size);
  GNUNET_memcpy (buf, msg, msize);
  GNUNET_MQ_impl_send_continue (mq);
  return msize;
}


/**
 * Signature of functions implementing the
 * sending functionality of a message queue.
 *
 * @param mq the message queue
 * @param msg the message to send
 * @param impl_state state of the implementation
 */
static void
cadet_mq_send_impl (struct GNUNET_MQ_Handle *mq,
                    const struct GNUNET_MessageHeader *msg,
                    void *impl_state)
{
  struct CadetMQState *state = impl_state;

  GNUNET_assert (NULL == state->th);
  state->th =
      GNUNET_CADET_notify_transmit_ready (state->channel,
                                         /* FIXME: add option for corking */
                                         GNUNET_NO,
                                         GNUNET_TIME_UNIT_FOREVER_REL,
                                         ntohs (msg->size),
                                         &cadet_mq_ntr, mq);

}


/**
 * Signature of functions implementing the
 * destruction of a message queue.
 * Implementations must not free 'mq', but should
 * take care of 'impl_state'.
 *
 * @param mq the message queue to destroy
 * @param impl_state state of the implementation
 */
static void
cadet_mq_destroy_impl (struct GNUNET_MQ_Handle *mq,
                       void *impl_state)
{
  struct CadetMQState *state = impl_state;

  if (NULL != state->th)
    GNUNET_CADET_notify_transmit_ready_cancel (state->th);

  GNUNET_free (state);
}


/**
 * Create a message queue for a cadet channel.
 * The message queue can only be used to transmit messages,
 * not to receive them.
 *
 * @param channel the channel to create the message qeue for
 * @return a message queue to messages over the channel
 */
struct GNUNET_MQ_Handle *
GNUNET_CADET_mq_create (struct GNUNET_CADET_Channel *channel)
{
  struct GNUNET_MQ_Handle *mq;
  struct CadetMQState *state;

  state = GNUNET_new (struct CadetMQState);
  state->channel = channel;

  mq = GNUNET_MQ_queue_for_callbacks (&cadet_mq_send_impl,
                                      &cadet_mq_destroy_impl,
                                      NULL, /* FIXME: cancel impl. */
                                      state,
                                      NULL, /* no msg handlers */
                                      NULL, /* no err handlers */
                                      NULL); /* no handler cls */
  return mq;
}


/**
 * Transitional function to convert an unsigned int port to a hash value.
 * WARNING: local static value returned, NOT reentrant!
 * WARNING: do not use this function for new code!
 *
 * @param port Numerical port (unsigned int format).
 *
 * @return A GNUNET_HashCode usable for the new CADET API.
 */
const struct GNUNET_HashCode *
GC_u2h (uint32_t port)
{
  static struct GNUNET_HashCode hash;

  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "This is a transitional function, "
              "use proper crypto hashes as CADET ports\n");
  GNUNET_CRYPTO_hash (&port, sizeof (port), &hash);

  return &hash;
}
