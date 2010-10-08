/*
     This file is part of GNUnet.
     (C) 2009 Christian Grothoff (and other contributing authors)

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
 * @file vpn/gnunet-service-dns.c
 * @author Philipp Tölke
 */
#include "platform.h"
#include "gnunet_getopt_lib.h"
#include "gnunet_service_lib.h"
#include "gnunet_network_lib.h"
#include "gnunet_os_lib.h"
#include "gnunet-service-dns-p.h"
#include "gnunet_protocols.h"
#include "gnunet-vpn-packet.h"
#include "gnunet-vpn-pretty-print.h"
#include "gnunet_container_lib.h"
#include "gnunet-dns-parser.h"
#include "gnunet_dht_service.h"
#include "gnunet_block_lib.h"
#include "gnunet_block_dns.h"

struct dns_cls {
	struct GNUNET_SCHEDULER_Handle *sched;

	struct GNUNET_NETWORK_Handle *dnsout;

	struct GNUNET_DHT_Handle *dht;

	unsigned short dnsoutport;

	struct answer_packet_list *head;
	struct answer_packet_list *tail;
};
static struct dns_cls mycls;

struct dns_query_id_state {
	unsigned valid:1;
	struct GNUNET_SERVER_Client* client;
	unsigned local_ip:32;
	unsigned local_port:16;
};
static struct dns_query_id_state query_states[65536]; /* This is < 1MiB */

void hijack(unsigned short port) {
	char port_s[6];

	GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "Hijacking, port is %d\n", port);
	snprintf(port_s, 6, "%d", port);
	GNUNET_OS_start_process(NULL, NULL, "gnunet-helper-hijack-dns", "gnunet-hijack-dns", port_s, NULL);
}

void unhijack(unsigned short port) {
	char port_s[6];

	GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "unHijacking, port is %d\n", port);
	snprintf(port_s, 6, "%d", port);
	GNUNET_OS_start_process(NULL, NULL, "gnunet-helper-hijack-dns", "gnunet-hijack-dns", "-d", port_s, NULL);
}

void receive_dht(void *cls,
		 struct GNUNET_TIME_Absolute exp,
		 const GNUNET_HashCode *key,
		 const struct GNUNET_PeerIdentity *const *get_path,
		 const struct GNUNET_PeerIdentity *const *put_path,
		 enum GNUNET_BLOCK_Type type,
		 size_t size,
		 const void *data)
{
  GNUNET_assert(type == GNUNET_BLOCK_TYPE_DNS);
  const struct GNUNET_DNS_Record* rec = data;
  GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "Got block of size %s, peer: %08x, desc: %08x\n", size, *((unsigned int*)&rec->peer), *((unsigned int*)&rec->service_descriptor));
}

/**
 * This receives the dns-payload from the daemon-vpn and sends it on over the udp-socket
 */
void receive_query(void *cls, struct GNUNET_SERVER_Client *client, const struct GNUNET_MessageHeader *message)
{
	struct query_packet* pkt = (struct query_packet*)message;
	struct dns_pkt* dns = (struct dns_pkt*)pkt->data;
	struct dns_pkt_parsed* pdns = parse_dns_packet(dns);

	if (pdns->queries[0]->namelen > 9 &&
	    0 == strncmp(pdns->queries[0]->name+(pdns->queries[0]->namelen - 9), ".gnunet.", 9)) {
	    GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "Query for .gnunet!\n");
	    GNUNET_HashCode key;
	    GNUNET_CRYPTO_hash(pdns->queries[0]->name, pdns->queries[0]->namelen, &key);
	    GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "Getting with key %08x, len is %d\n", *((unsigned int*)&key), pdns->queries[0]->namelen);
	    GNUNET_DHT_get_start(mycls.dht,
				 GNUNET_TIME_UNIT_MINUTES,
				 GNUNET_BLOCK_TYPE_DNS,
				 &key,
				 GNUNET_DHT_RO_NONE,
				 NULL,
				 0,
				 NULL,
				 0,
				 receive_dht,
				 NULL);
	    goto out;
	}

	GNUNET_free(pdns);

	struct sockaddr_in dest;
	memset(&dest, 0, sizeof dest);
	dest.sin_port = htons(53);
	dest.sin_addr.s_addr = pkt->orig_to;

	query_states[dns->s.id].valid = 1;
	query_states[dns->s.id].client = client;
	query_states[dns->s.id].local_ip = pkt->orig_from;
	query_states[dns->s.id].local_port = pkt->src_port;

	/* int r = */ GNUNET_NETWORK_socket_sendto(mycls.dnsout, dns, ntohs(pkt->hdr.size) - sizeof(struct query_packet) + 1, (struct sockaddr*) &dest, sizeof dest);

out:
	GNUNET_SERVER_receive_done(client, GNUNET_OK);
}

size_t send_answer(void* cls, size_t size, void* buf) {
	struct answer_packet_list* query = mycls.head;
	size_t len = ntohs(query->pkt.hdr.size);

	GNUNET_assert(len <= size);

	memcpy(buf, &query->pkt.hdr, len);

	GNUNET_CONTAINER_DLL_remove (mycls.head, mycls.tail, query);

	GNUNET_free(query);

	if (mycls.head != NULL) {
		GNUNET_SERVER_notify_transmit_ready(cls, ntohs(mycls.head->pkt.hdr.size), GNUNET_TIME_UNIT_FOREVER_REL, &send_answer, cls);
	}

	return len;
}

static void read_response (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc) {
	unsigned char buf[65536];
	struct dns_pkt* dns = (struct dns_pkt*)buf;

	if (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN)
		return;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	unsigned int addrlen = sizeof addr;

	int r;
	r = GNUNET_NETWORK_socket_recvfrom(mycls.dnsout, buf, 65536, (struct sockaddr*)&addr, &addrlen);

	/* if (r < 0) TODO */

	if (query_states[dns->s.id].valid == 1) {
		query_states[dns->s.id].valid = 0;

		size_t len = sizeof(struct answer_packet) + r - 1; /* 1 for the unsigned char data[1]; */
		struct answer_packet_list* answer = GNUNET_malloc(len + 2*sizeof(struct answer_packet_list*));
		answer->pkt.hdr.type = htons(GNUNET_MESSAGE_TYPE_LOCAL_RESPONSE_DNS);
		answer->pkt.hdr.size = htons(len);
		answer->pkt.from = addr.sin_addr.s_addr;
		answer->pkt.to = query_states[dns->s.id].local_ip;
		answer->pkt.dst_port = query_states[dns->s.id].local_port;
		memcpy(answer->pkt.data, buf, r);

		GNUNET_CONTAINER_DLL_insert_after(mycls.head, mycls.tail, mycls.tail, answer);

		/* struct GNUNET_CONNECTION_TransmitHandle* th = */ GNUNET_SERVER_notify_transmit_ready(query_states[dns->s.id].client, len, GNUNET_TIME_UNIT_FOREVER_REL, &send_answer, query_states[dns->s.id].client);
	}

	GNUNET_SCHEDULER_add_read_net(mycls.sched, GNUNET_TIME_UNIT_FOREVER_REL, mycls.dnsout, &read_response, NULL);
}


/**
 * Task run during shutdown.
 *
 * @param cls unused
 * @param tc unused
 */
static void
cleanup_task (void *cls,
	      const struct GNUNET_SCHEDULER_TaskContext *tc)
{
	unhijack(mycls.dnsoutport);
	GNUNET_DHT_disconnect(mycls.dht);
}

static void
publish_name (void *cls,
	     const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  if (0 != (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN))
    return;

  char* name = "philipptoelke.gnunet.";
  size_t size = sizeof(struct GNUNET_DNS_Record) + strlen(name) - 1;
  struct GNUNET_DNS_Record *data = alloca(size);
  memset(data, 0, size);
  memcpy(data->name, name, strlen(name));
  data->namelen = strlen(name);
  *((unsigned int*)&data->service_descriptor) = 0x11223344;
  *((unsigned int*)&data->peer) = 0x55667788;

  GNUNET_HashCode key;
  GNUNET_CRYPTO_hash(name, strlen(name)+1, &key);
  GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "Putting with key %08x, len is %d\n", *((unsigned int*)&key), strlen(name));

  GNUNET_DHT_put(mycls.dht,
		      &key,
		      GNUNET_DHT_RO_NONE,
		      GNUNET_BLOCK_TYPE_DNS,
		      size,
		      (char*)data,
		      GNUNET_TIME_relative_to_absolute(GNUNET_TIME_UNIT_HOURS),
		      GNUNET_TIME_UNIT_MINUTES,
		      NULL,
		      NULL);

  GNUNET_SCHEDULER_add_delayed (mycls.sched, GNUNET_TIME_UNIT_MINUTES, publish_name, NULL);
}

/**
 * @param cls closure
 * @param sched scheduler to use
 * @param server the initialized server
 * @param cfg configuration to use
 */
static void
run (void *cls,
     struct GNUNET_SCHEDULER_Handle *sched,
     struct GNUNET_SERVER_Handle *server,
     const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  static const struct GNUNET_SERVER_MessageHandler handlers[] = {
	  /* callback, cls, type, size */
    {&receive_query, NULL, GNUNET_MESSAGE_TYPE_LOCAL_QUERY_DNS, 0},
    {NULL, NULL, 0, 0}
  };

  {
  int i;
  for (i = 0; i < 65536; i++) {
    query_states[i].valid = 0;
  }
  }

  mycls.dht = GNUNET_DHT_connect(sched, cfg, 1024);

  struct sockaddr_in addr;

  mycls.sched = sched;
  mycls.dnsout = GNUNET_NETWORK_socket_create (AF_INET, SOCK_DGRAM, 0);
  if (mycls.dnsout == NULL) 
    return;
  memset(&addr, 0, sizeof(struct sockaddr_in));

  int err = GNUNET_NETWORK_socket_bind (mycls.dnsout,
					(struct sockaddr*)&addr, 
					sizeof(struct sockaddr_in));

  if (err != GNUNET_YES) {
	GNUNET_log(GNUNET_ERROR_TYPE_ERROR, "Could not bind a port, exiting\n");
	return;
  }
  socklen_t addrlen = sizeof(struct sockaddr_in);
  err = getsockname(GNUNET_NETWORK_get_fd(mycls.dnsout),
		    (struct sockaddr*) &addr, 
		    &addrlen);

  mycls.dnsoutport = htons(addr.sin_port);

  hijack(htons(addr.sin_port));

  GNUNET_SCHEDULER_add_now (mycls.sched, publish_name, NULL);

	GNUNET_SCHEDULER_add_read_net(sched, GNUNET_TIME_UNIT_FOREVER_REL, mycls.dnsout, &read_response, NULL);

  GNUNET_SERVER_add_handlers (server, handlers);
  GNUNET_SCHEDULER_add_delayed (sched,
		  GNUNET_TIME_UNIT_FOREVER_REL,
		  &cleanup_task,
		  cls);
}

/**
 * The main function for the dns service.
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
                              "dns",
			      GNUNET_SERVICE_OPTION_NONE,
			      &run, NULL)) ? 0 : 1;
}
