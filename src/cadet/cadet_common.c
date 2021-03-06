/*
     This file is part of GNUnet.
     Copyright (C) 2012 GNUnet e.V.

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
 * @file cadet/cadet_common.c
 * @brief CADET helper functions
 * @author Bartlomiej Polot
 */

#include "cadet.h"

/**
 * @brief Translate a fwd variable into a string representation, for logging.
 *
 * @param fwd Is FWD? (#GNUNET_YES or #GNUNET_NO)
 *
 * @return String representing FWD or BCK.
 */
char *
GC_f2s (int fwd)
{
  if (GNUNET_YES == fwd)
  {
    return "FWD";
  }
  else if (GNUNET_NO == fwd)
  {
    return "BCK";
  }
  else
  {
    /* Not an error, can happen with CONNECTION_BROKEN messages. */
    return "\???";
  }
}


/**
 * Test if @a bigger is larger than @a smaller.
 * Considers the case that @a bigger just overflowed
 * and is thus tiny while @a smaller is still below
 * `UINT32_MAX`.
 */
int
GC_is_pid_bigger (uint32_t bigger,
		  uint32_t smaller)
{
    return (PID_OVERFLOW (smaller, bigger) ||
            ( (bigger > smaller) &&
	      (! PID_OVERFLOW (bigger, smaller))) );
}


uint32_t
GC_max_pid (uint32_t a, uint32_t b)
{
  if (GC_is_pid_bigger(a, b))
    return a;
  return b;
}


uint32_t
GC_min_pid (uint32_t a, uint32_t b)
{
  if (GC_is_pid_bigger(a, b))
    return b;
  return a;
}


/**
 * Allocate a string with a hexdump of any binary data.
 *
 * @param bin Arbitrary binary data.
 * @param len Length of @a bin in bytes.
 * @param output Where to write the output (if *output be NULL it's allocated).
 *
 * @return The size of the output.
 */
size_t
GC_bin2s (void *bin, unsigned int len, char **output)
{
  char *data = bin;
  char *buf;
  unsigned int s_len;
  unsigned int i;

  s_len = 2 * len + 1;
  if (NULL == *output)
    *output = GNUNET_malloc (s_len);
  buf = *output;

  for (i = 0; i < len; i++)
  {
    SPRINTF (&buf[2 * i], "%2X", data[i]);
  }
  buf[s_len - 1] = '\0';

  return s_len;
}


#if !defined(GNUNET_CULL_LOGGING)
const char *
GC_m2s (uint16_t m)
{
  static char buf[2][16];
  static int idx;
  const char *s;

  idx = (idx + 1) % 2;
  switch (m)
  {
    /**
     * Used to mark the "payload" of a non-payload message.
     */
    case 0:
      s = "retransmit";
      break;

      /**
       * Request the creation of a path
       */
    case GNUNET_MESSAGE_TYPE_CADET_CONNECTION_CREATE:
      s = "CONN_CREAT";
      break;

      /**
       * Request the modification of an existing path
       */
    case GNUNET_MESSAGE_TYPE_CADET_CONNECTION_CREATE_ACK:
      s = "CONN_ACK";
      break;

      /**
       * Notify that a connection of a path is no longer valid
       */
    case GNUNET_MESSAGE_TYPE_CADET_CONNECTION_BROKEN:
      s = "CONN_BRKN";
      break;

      /**
       * At some point, the route will spontaneously change
       */
    case GNUNET_MESSAGE_TYPE_CADET_CONNECTION_PATH_CHANGED_UNIMPLEMENTED:
      s = "PATH_CHNGD";
      break;

      /**
       * Transport payload data.
       */
    case GNUNET_MESSAGE_TYPE_CADET_CHANNEL_APP_DATA:
      s = "DATA";
      break;

    /**
     * Confirm receipt of payload data.
     */
    case GNUNET_MESSAGE_TYPE_CADET_CHANNEL_APP_DATA_ACK:
      s = "DATA_ACK";
      break;

      /**
       * Key exchange message.
       */
    case GNUNET_MESSAGE_TYPE_CADET_TUNNEL_KX:
      s = "KX";
      break;

      /**
       * Encrypted.
       */
    case GNUNET_MESSAGE_TYPE_CADET_TUNNEL_ENCRYPTED:
      s = "ENCRYPTED";
      break;

      /**
       * Request the destuction of a path
       */
    case GNUNET_MESSAGE_TYPE_CADET_CONNECTION_DESTROY:
      s = "CONN_DSTRY";
      break;

      /**
       * ACK for a data packet.
       */
    case GNUNET_MESSAGE_TYPE_CADET_CONNECTION_HOP_BY_HOP_ENCRYPTED_ACK:
      s = "ACK";
      break;

      /**
       * POLL for ACK.
       */
    case GNUNET_MESSAGE_TYPE_CADET_TUNNEL_ENCRYPTED_POLL:
      s = "POLL";
      break;

      /**
       * Announce origin is still alive.
       */
    case GNUNET_MESSAGE_TYPE_CADET_CHANNEL_KEEPALIVE:
      s = "KEEPALIVE";
      break;

      /**
       * Open port
       */
    case GNUNET_MESSAGE_TYPE_CADET_LOCAL_PORT_OPEN:
      s = "OPEN_PORT";
      break;

      /**
       * Close port
       */
    case GNUNET_MESSAGE_TYPE_CADET_LOCAL_PORT_CLOSE:
      s = "CLOSE_PORT";
      break;

      /**
       * Ask the cadet service to create a new tunnel
       */
    case GNUNET_MESSAGE_TYPE_CADET_CHANNEL_OPEN:
      s = "CHAN_CREAT";
      break;

      /**
       * Ask the cadet service to destroy a tunnel
       */
    case GNUNET_MESSAGE_TYPE_CADET_CHANNEL_DESTROY:
      s = "CHAN_DSTRY";
      break;

      /**
       * Confirm the creation of a channel.
       */
    case GNUNET_MESSAGE_TYPE_CADET_CHANNEL_OPEN_ACK:
      s = "CHAN_ACK";
      break;

      /**
       * Confirm the creation of a channel.
       */
    case GNUNET_MESSAGE_TYPE_CADET_CHANNEL_OPEN_NACK_DEPRECATED:
      s = "CHAN_NACK";
      break;

      /**
       * Local payload traffic
       */
    case GNUNET_MESSAGE_TYPE_CADET_LOCAL_DATA:
      s = "LOC_DATA";
      break;

      /**
       * Local ACK for data.
       */
    case GNUNET_MESSAGE_TYPE_CADET_LOCAL_ACK:
      s = "LOC_ACK";
      break;

      /**
       * Local monitoring of channels.
       */
    case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_CHANNELS:
      s = "INFO_CHANS";
      break;

      /**
       * Local monitoring of a channel.
       */
    case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_CHANNEL:
      s = "INFO_CHAN";
      break;

      /**
       * Local monitoring of service.
       */
    case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_TUNNELS:
      s = "INFO_TUNS";
      break;

      /**
       * Local monitoring of service.
       */
    case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_TUNNEL:
      s = "INFO_TUN";
      break;

      /**
       * Local information about all connections of service.
       */
    case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_CONNECTIONS:
      s = "INFO_CONNS";
      break;

      /**
       * Local information of service about a specific connection.
       */
    case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_CONNECTION:
      s = "INFO_CONN";
      break;

      /**
       * Local information about all peers known to the service.
       */
    case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_PEERS:
      s = "INFO_PEERS";
      break;

      /**
       * Local information of service about a specific peer.
       */
    case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_PEER:
      s = "INFO_PEER";
      break;

      /**
       * Traffic (net-cat style) used by the Command Line Interface.
       */
    case GNUNET_MESSAGE_TYPE_CADET_CLI:
      s = "CLI";
      break;

      /**
       * Debug request.
       */
    case GNUNET_MESSAGE_TYPE_CADET_LOCAL_INFO_DUMP:
      s = "INFO_DUMP";
      break;

      /**
       * Used to mark the "payload" of a non-payload message.
       */
    case UINT16_MAX:
      s = "      N/A";
      break;


    default:
      SPRINTF (buf[idx], "{UNK: %5u}", m);
      return buf[idx];
  }
  SPRINTF (buf[idx], "{%10s}", s);
  return buf[idx];
}
#else
const char *
GC_m2s (uint16_t m)
{
  return "";
}
#endif
