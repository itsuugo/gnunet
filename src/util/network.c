/*
     This file is part of GNUnet.
     (C) 2009 Christian Grothoff (and other contributing authors)

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
 * @file util/network/network.c
 * @brief basic, low-level TCP networking interface
 * @author Christian Grothoff
 *
 * This code is rather complex.  Only modify it if you
 * 1) Have a NEW testcase showing that the new code
 *    is needed and correct
 * 2) All EXISTING testcases pass with the new code
 * These rules should apply in general, but for this
 * module they are VERY, VERY important.
 *
 * TODO:
 * - can we merge receive_ready and receive_again?
 * - can we integrate the nth.timeout_task with the write_task's timeout?
 */

#include "platform.h"
#include "gnunet_common.h"
#include "gnunet_network_lib.h"
#include "gnunet_scheduler_lib.h"

#define DEBUG_NETWORK GNUNET_NO

struct GNUNET_NETWORK_TransmitHandle
{

  /**
   * Function to call if the send buffer has notify_size
   * bytes available.
   */
  GNUNET_NETWORK_TransmitReadyNotify notify_ready;

  /**
   * Closure for notify_ready.
   */
  void *notify_ready_cls;

  /**
   * Our socket handle.
   */
  struct GNUNET_NETWORK_SocketHandle *sh;

  /**
   * Timeout for receiving (in absolute time).
   */
  struct GNUNET_TIME_Absolute transmit_timeout;

  /**
   * Task called on timeout.
   */
  GNUNET_SCHEDULER_TaskIdentifier timeout_task;

  /**
   * At what number of bytes available in the
   * write buffer should the notify method be called?
   */
  size_t notify_size;

};

/**
 * @brief handle for a network socket
 */
struct GNUNET_NETWORK_SocketHandle
{

  /**
   * Scheduler that was used for the connect task.
   */
  struct GNUNET_SCHEDULER_Handle *sched;

  /**
   * Address information for connect (may be NULL).
   */
  struct addrinfo *ai;

  /**
   * Index for the next struct addrinfo for connect attempts (may be NULL)
   */
  struct addrinfo *ai_pos;

  /**
   * Network address of the other end-point, may be NULL.
   */
  struct sockaddr *addr;

  /**
   * Pointer to our write buffer.
   */
  char *write_buffer;

  /**
   * Size of our write buffer.
   */
  size_t write_buffer_size;

  /**
   * Current write-offset in write buffer (where
   * would we write next).
   */
  size_t write_buffer_off;

  /**
   * Current read-offset in write buffer (how many
   * bytes have already been send).
   */
  size_t write_buffer_pos;

  /**
   * Length of addr.
   */
  socklen_t addrlen;

  /**
   * Connect task that we may need to wait for.
   */
  GNUNET_SCHEDULER_TaskIdentifier connect_task;

  /**
   * Read task that we may need to wait for.
   */
  GNUNET_SCHEDULER_TaskIdentifier read_task;

  /**
   * Write task that we may need to wait for.
   */
  GNUNET_SCHEDULER_TaskIdentifier write_task;

  /**
   * The handle we return for GNUNET_NETWORK_notify_transmit_ready.
   */
  struct GNUNET_NETWORK_TransmitHandle nth;

  /**
   * Underlying OS's socket, set to -1 after fatal errors.
   */
  int sock;

  /**
   * Port to connect to.
   */
  uint16_t port;

  /**
   * Function to call on data received, NULL
   * if no receive is pending.
   */
  GNUNET_NETWORK_Receiver receiver;

  /**
   * Closure for receiver.
   */
  void *receiver_cls;

  /**
   * Timeout for receiving (in absolute time).
   */
  struct GNUNET_TIME_Absolute receive_timeout;

  /**
   * Maximum number of bytes to read
   * (for receiving).
   */
  size_t max;

};


/**
 * Create a socket handle by boxing an existing OS socket.  The OS
 * socket should henceforth be no longer used directly.
 * GNUNET_socket_destroy will close it.
 *
 * @param sched scheduler to use
 * @param osSocket existing socket to box
 * @param maxbuf maximum write buffer size for the socket (use
 *        0 for sockets that need no write buffers, such as listen sockets)
 * @return the boxed socket handle
 */
struct GNUNET_NETWORK_SocketHandle *
GNUNET_NETWORK_socket_create_from_existing (struct GNUNET_SCHEDULER_Handle
                                            *sched, int osSocket,
                                            size_t maxbuf)
{
  struct GNUNET_NETWORK_SocketHandle *ret;
  ret = GNUNET_malloc (sizeof (struct GNUNET_NETWORK_SocketHandle) + maxbuf);
  ret->write_buffer = (char *) &ret[1];
  ret->write_buffer_size = maxbuf;
  ret->sock = osSocket;
  ret->sched = sched;
  return ret;
}


/**
 * Create a socket handle by accepting on a listen socket.  This
 * function may block if the listen socket has no connection ready.
 *
 * @param sched scheduler to use
 * @param access function to use to check if access is allowed
 * @param access_cls closure for access
 * @param lsock listen socket
 * @param maxbuf maximum write buffer size for the socket (use
 *        0 for sockets that need no write buffers, such as listen sockets)
 * @return the socket handle, NULL on error
 */
struct GNUNET_NETWORK_SocketHandle *
GNUNET_NETWORK_socket_create_from_accept (struct GNUNET_SCHEDULER_Handle
                                          *sched,
                                          GNUNET_NETWORK_AccessCheck access,
                                          void *access_cls, int lsock,
                                          size_t maxbuf)
{
  struct GNUNET_NETWORK_SocketHandle *ret;
  char addr[32];
  socklen_t addrlen;
  int fd;
  int aret;
  struct sockaddr_in *v4;
  struct sockaddr_in6 *v6;
  struct sockaddr *sa;
  void *uaddr;

  addrlen = sizeof (addr);
  fd = accept (lsock, (struct sockaddr *) &addr, &addrlen);
  if (fd == -1)
    {
      GNUNET_log_strerror (GNUNET_ERROR_TYPE_WARNING, "accept");
      return NULL;
    }
#ifndef MINGW
  // FIXME NILS
  if (0 != fcntl (fd, F_SETFD, fcntl (fd, F_GETFD) | FD_CLOEXEC))
    GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR | GNUNET_ERROR_TYPE_BULK,
                         "fcntl");
#endif
  if (addrlen > sizeof (addr))
    {
      GNUNET_break (0);
      GNUNET_break (0 == CLOSE (fd));
      return NULL;
    }

  sa = (struct sockaddr *) addr;
  v6 = (struct sockaddr_in6 *) addr;
  if ((sa->sa_family == AF_INET6) && (IN6_IS_ADDR_V4MAPPED (&v6->sin6_addr)))
    {
      /* convert to V4 address */
      v4 = GNUNET_malloc (sizeof (struct sockaddr_in));
      memset (v4, 0, sizeof (struct sockaddr_in));
      v4->sin_family = AF_INET;
      memcpy (&v4->sin_addr,
              &((char *) &v6->sin6_addr)[sizeof (struct in6_addr) -
                                         sizeof (struct in_addr)],
              sizeof (struct in_addr));
      v4->sin_port = v6->sin6_port;
      uaddr = v4;
      addrlen = sizeof (struct sockaddr_in);
    }
  else
    {
      uaddr = GNUNET_malloc (addrlen);
      memcpy (uaddr, addr, addrlen);
    }

  if ((access != NULL) &&
      (GNUNET_YES != (aret = access (access_cls, uaddr, addrlen))))
    {
      if (aret == GNUNET_NO)
	GNUNET_log (GNUNET_ERROR_TYPE_INFO,
		    _("Access denied to `%s'\n"),
		    GNUNET_a2s(uaddr, addrlen));
      GNUNET_break (0 == SHUTDOWN (fd, SHUT_RDWR));
      GNUNET_break (0 == CLOSE (fd));
      GNUNET_free (uaddr);
      return NULL;
    }
#if DEBUG_NETWORK
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
	      _("Accepting connection from `%s'\n"),
	      GNUNET_a2s(uaddr, addrlen));
#endif
  ret = GNUNET_malloc (sizeof (struct GNUNET_NETWORK_SocketHandle) + maxbuf);
  ret->write_buffer = (char *) &ret[1];
  ret->write_buffer_size = maxbuf;
  ret->addr = uaddr;
  ret->addrlen = addrlen;
  ret->sock = fd;
  ret->sched = sched;
  return ret;
}

/**
 * Obtain the network address of the other party.
 *
 * @param sock the client to get the address for
 * @param addr where to store the address
 * @param addrlen where to store the length of the address
 * @return GNUNET_OK on success
 */
int
GNUNET_NETWORK_socket_get_address (struct GNUNET_NETWORK_SocketHandle *sock,
                                   void **addr, size_t * addrlen)
{
  if ((sock->addr == NULL) || (sock->addrlen == 0))
    return GNUNET_NO;
  *addr = GNUNET_malloc (sock->addrlen);
  memcpy (*addr, sock->addr, sock->addrlen);
  *addrlen = sock->addrlen;
  return GNUNET_OK;
}


/**
 * Set if a socket should use blocking or non-blocking IO.
 *
 * @return GNUNET_OK on success, GNUNET_SYSERR on error
 */
static int
socket_set_blocking (int handle, int doBlock)
{
#if MINGW
  u_long mode;
  mode = !doBlock;
#if HAVE_PLIBC_FD
  if (ioctlsocket (plibc_fd_get_handle (handle), FIONBIO, &mode) ==
      SOCKET_ERROR)
    {
      SetErrnoFromWinsockError (WSAGetLastError ());
      GNUNET_log_strerror (GNUNET_ERROR_TYPE_WARNING, "ioctlsocket");
      return GNUNET_SYSERR;
    }
#else
  if (ioctlsocket (handle, FIONBIO, &mode) == SOCKET_ERROR)
    {
      SetErrnoFromWinsockError (WSAGetLastError ());
      GNUNET_log_strerror (GNUNET_ERROR_TYPE_WARNING, "ioctlsocket");
      return GNUNET_SYSERR;
    }
#endif
  /* store the blocking mode */
#if HAVE_PLIBC_FD
  plibc_fd_set_blocking (handle, doBlock);
#else
  __win_SetHandleBlockingMode (handle, doBlock);
#endif
  return GNUNET_OK;

#else
  /* not MINGW */
  int flags = fcntl (handle, F_GETFL);
  if (flags == -1)
    {
      GNUNET_log_strerror (GNUNET_ERROR_TYPE_WARNING, "fcntl");
      return GNUNET_SYSERR;
    }
  if (doBlock)
    flags &= ~O_NONBLOCK;
  else
    flags |= O_NONBLOCK;
  if (0 != fcntl (handle, F_SETFL, flags))
    {
      GNUNET_log_strerror (GNUNET_ERROR_TYPE_WARNING, "fcntl");
      return GNUNET_SYSERR;
    }
  return GNUNET_OK;
#endif
}


/**
 * Initiate asynchronous TCP connect request.
 *
 * @param sock what socket to connect
 * @return GNUNET_SYSERR error (no more addresses to try)
 */
static int
try_connect (struct GNUNET_NETWORK_SocketHandle *sock)
{
  int s;

  if (sock->addr != NULL)
    {
      GNUNET_free (sock->addr);
      sock->addr = NULL;
      sock->addrlen = 0;
    }
  while (1)
    {
      if (sock->ai_pos == NULL)
        {
          /* no more addresses to try, fatal! */
          return GNUNET_SYSERR;
        }
      switch (sock->ai_pos->ai_family)
        {
        case AF_INET:
          ((struct sockaddr_in *) sock->ai_pos->ai_addr)->sin_port =
            htons (sock->port);
          break;
        case AF_INET6:
          ((struct sockaddr_in6 *) sock->ai_pos->ai_addr)->sin6_port =
            htons (sock->port);
          break;
        default:
          sock->ai_pos = sock->ai_pos->ai_next;
          continue;
        }
      s = SOCKET (sock->ai_pos->ai_family, SOCK_STREAM, 0);
      if (s == -1)
        {
          /* maybe unsupported address family, try next */
          GNUNET_log_strerror (GNUNET_ERROR_TYPE_INFO, "socket");
          sock->ai_pos = sock->ai_pos->ai_next;
          continue;
        }
#ifndef MINGW
      // FIXME NILS
      if (0 != fcntl (s, F_SETFD, fcntl (s, F_GETFD) | FD_CLOEXEC))
        GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR | GNUNET_ERROR_TYPE_BULK,
                             "fcntl");
#endif
      if (GNUNET_SYSERR == socket_set_blocking (s, GNUNET_NO))
        {
          /* we'll treat this one as fatal */
          GNUNET_break (0 == CLOSE (s));
          return GNUNET_SYSERR;
        }
#if DEBUG_NETWORK
      GNUNET_log (GNUNET_ERROR_TYPE_INFO,
		  _("Trying to connect to `%s'\n"),
		  GNUNET_a2s(sock->ai_pos->ai_addr,
			     sock->ai_pos->ai_addrlen));
#endif
      if ((0 != CONNECT (s,
                         sock->ai_pos->ai_addr,
                         sock->ai_pos->ai_addrlen)) && (errno != EINPROGRESS))
        {
          /* maybe refused / unsupported address, try next */
          GNUNET_log_strerror (GNUNET_ERROR_TYPE_INFO, "connect");
          GNUNET_break (0 == CLOSE (s));
          sock->ai_pos = sock->ai_pos->ai_next;
          continue;
        }
      break;
    }
  /* got one! copy address information! */
  sock->addrlen = sock->ai_pos->ai_addrlen;
  sock->addr = GNUNET_malloc (sock->addrlen);
  memcpy (sock->addr, sock->ai_pos->ai_addr, sock->addrlen);
  sock->ai_pos = sock->ai_pos->ai_next;
  sock->sock = s;
  return GNUNET_OK;
}


/**
 * Scheduler let us know that we're either ready to
 * write on the socket OR connect timed out.  Do the
 * right thing.
 */
static void
connect_continuation (void *cls,
                      const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct GNUNET_NETWORK_SocketHandle *sock = cls;
  unsigned int len;
  int error;

  /* nobody needs to wait for us anymore... */
  sock->connect_task = GNUNET_SCHEDULER_NO_PREREQUISITE_TASK;
  /* Note: write-ready does NOT mean connect succeeded,
     we need to use getsockopt to be sure */
  len = sizeof (error);
  errno = 0;
  error = 0;
  if ((0 == (tc->reason & GNUNET_SCHEDULER_REASON_WRITE_READY)) ||
      (0 != getsockopt (sock->sock, SOL_SOCKET, SO_ERROR, &error, &len)) ||
      (error != 0) || (errno != 0))
    {
#if DEBUG_NETWORK
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		  "Failed to establish TCP connection to `%s'\n",
		  GNUNET_a2s(sock->addr, sock->addrlen));
#endif
      /* connect failed / timed out */
      GNUNET_break (0 == CLOSE (sock->sock));
      sock->sock = -1;
      if (GNUNET_SYSERR == try_connect (sock))
        {
          /* failed for good */
#if DEBUG_NETWORK
	  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		      "Failed to establish TCP connection, no further addresses to try.\n");
#endif
	  /* connect failed / timed out */
          GNUNET_break (sock->ai_pos == NULL);
          freeaddrinfo (sock->ai);
          sock->ai = NULL;
          return;
        }
      sock->connect_task = GNUNET_SCHEDULER_add_write (tc->sched, GNUNET_NO,    /* abort on shutdown */
                                                       GNUNET_SCHEDULER_PRIORITY_KEEP,
                                                       GNUNET_SCHEDULER_NO_PREREQUISITE_TASK,
                                                       GNUNET_NETWORK_CONNECT_RETRY_TIMEOUT,
                                                       sock->sock,
                                                       &connect_continuation,
                                                       sock);
      return;
    }
  /* connect succeeded! clean up "ai" */
#if DEBUG_NETWORK
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
	      "Connection to `%s' succeeded!\n",
	      GNUNET_a2s(sock->addr, sock->addrlen));
#endif
  freeaddrinfo (sock->ai);
  sock->ai_pos = NULL;
  sock->ai = NULL;
}


/**
 * Create a socket handle by (asynchronously) connecting to a host.
 * This function returns immediately, even if the connection has not
 * yet been established.  This function only creates TCP connections.
 *
 * @param sched scheduler to use
 * @param hostname name of the host to connect to
 * @param port port to connect to
 * @param maxbuf maximum write buffer size for the socket (use
 *        0 for sockets that need no write buffers, such as listen sockets)
 * @return the socket handle
 */
struct GNUNET_NETWORK_SocketHandle *
GNUNET_NETWORK_socket_create_from_connect (struct GNUNET_SCHEDULER_Handle
                                           *sched, const char *hostname,
                                           uint16_t port, size_t maxbuf)
{
  struct GNUNET_NETWORK_SocketHandle *ret;
  struct addrinfo hints;
  int ec;

  ret = GNUNET_malloc (sizeof (struct GNUNET_NETWORK_SocketHandle) + maxbuf);
  ret->sock = -1;
  ret->sched = sched;
  ret->write_buffer = (char *) &ret[1];
  ret->write_buffer_size = maxbuf;
  ret->port = port;
  memset (&hints, 0, sizeof (hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (0 != (ec = getaddrinfo (hostname, NULL, &hints, &ret->ai)))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_INFO | GNUNET_ERROR_TYPE_BULK,
                  "`%s' failed for hostname `%s': %s\n",
                  "getaddrinfo", hostname, gai_strerror (ec));
      GNUNET_free (ret);
      return NULL;
    }
  ret->ai_pos = ret->ai;
  if (GNUNET_SYSERR == try_connect (ret))
    {
      freeaddrinfo (ret->ai);
      GNUNET_free (ret);
      return NULL;
    }
  ret->connect_task = GNUNET_SCHEDULER_add_write (sched, GNUNET_NO,     /* abort on shutdown */
                                                  GNUNET_SCHEDULER_PRIORITY_KEEP,
                                                  GNUNET_SCHEDULER_NO_PREREQUISITE_TASK,
                                                  GNUNET_NETWORK_CONNECT_RETRY_TIMEOUT,
                                                  ret->sock,
                                                  &connect_continuation, ret);
  return ret;

}


/**
 * Create a socket handle by (asynchronously) connecting to a host.
 * This function returns immediately, even if the connection has not
 * yet been established.  This function only creates TCP connections.
 *
 * @param sched scheduler to use
 * @param af_family address family to use
 * @param serv_addr server address
 * @param addrlen length of server address
 * @param maxbuf maximum write buffer size for the socket (use
 *        0 for sockets that need no write buffers, such as listen sockets)
 * @return the socket handle
 */
struct GNUNET_NETWORK_SocketHandle *
GNUNET_NETWORK_socket_create_from_sockaddr (struct GNUNET_SCHEDULER_Handle
                                            *sched, int af_family,
                                            const struct sockaddr *serv_addr,
                                            socklen_t addrlen, size_t maxbuf)
{
  int s;
  struct GNUNET_NETWORK_SocketHandle *ret;

  s = SOCKET (af_family, SOCK_STREAM, 0);
  if (s == -1)
    {
      GNUNET_log_strerror (GNUNET_ERROR_TYPE_WARNING |
                           GNUNET_ERROR_TYPE_BULK, "socket");
      return NULL;
    }
#ifndef MINGW
  if (0 != fcntl (s, F_SETFD, fcntl (s, F_GETFD) | FD_CLOEXEC))
    GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR | GNUNET_ERROR_TYPE_BULK,
                         "fcntl");
#endif
  if (GNUNET_SYSERR == socket_set_blocking (s, GNUNET_NO))
    {
      /* we'll treat this one as fatal */
      GNUNET_break (0 == CLOSE (s));
      return NULL;
    }
#if DEBUG_NETWORK
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
	      _("Trying to connect to `%s'\n"),
	      GNUNET_a2s(serv_addr, addrlen));
#endif
  if ((0 != CONNECT (s, serv_addr, addrlen)) && (errno != EINPROGRESS))
    {
      /* maybe refused / unsupported address, try next */
      GNUNET_log_strerror (GNUNET_ERROR_TYPE_INFO, "connect");
      GNUNET_break (0 == CLOSE (s));
      return NULL;
    }
  ret = GNUNET_NETWORK_socket_create_from_existing (sched, s, maxbuf);
  ret->addr = GNUNET_malloc (addrlen);
  memcpy (ret->addr, serv_addr, addrlen);
  ret->addrlen = addrlen;
  return ret;
}


/**
 * Check if socket is valid (no fatal errors have happened so far).
 * Note that a socket that is still trying to connect is considered
 * valid.
 *
 * @param sock socket to check
 * @return GNUNET_YES if valid, GNUNET_NO otherwise
 */
int
GNUNET_NETWORK_socket_check (struct GNUNET_NETWORK_SocketHandle *sock)
{
  if (sock->ai != NULL)
    return GNUNET_YES;          /* still trying to connect */
  return (sock->sock == -1) ? GNUNET_NO : GNUNET_YES;
}


/**
 * Scheduler let us know that the connect task is finished (or was
 * cancelled due to shutdown).  Now really clean up.
 */
static void
destroy_continuation (void *cls,
                      const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct GNUNET_NETWORK_SocketHandle *sock = cls;
  GNUNET_NETWORK_TransmitReadyNotify notify;

  if (sock->write_task != GNUNET_SCHEDULER_NO_PREREQUISITE_TASK)
    {
      GNUNET_SCHEDULER_add_after (sock->sched,
                                  GNUNET_YES,
                                  GNUNET_SCHEDULER_PRIORITY_KEEP,
                                  sock->write_task,
                                  &destroy_continuation, sock);
      return;
    }
  if (sock->sock != -1)
    {
#if DEBUG_NETWORK
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Shutting down socket.\n");
#endif
      SHUTDOWN (sock->sock, SHUT_RDWR);
    }
  if (sock->read_task != GNUNET_SCHEDULER_NO_PREREQUISITE_TASK)
    {
      GNUNET_SCHEDULER_add_after (sock->sched,
                                  GNUNET_YES,
                                  GNUNET_SCHEDULER_PRIORITY_KEEP,
                                  sock->read_task,
                                  &destroy_continuation, sock);
      return;
    }
  if (NULL != (notify = sock->nth.notify_ready))
    {
      sock->nth.notify_ready = NULL;
      notify (sock->nth.notify_ready_cls, 0, NULL);
      if (sock->nth.timeout_task != GNUNET_SCHEDULER_NO_PREREQUISITE_TASK)
        {
          GNUNET_SCHEDULER_cancel (sock->sched, sock->nth.timeout_task);
          sock->nth.timeout_task = GNUNET_SCHEDULER_NO_PREREQUISITE_TASK;
        }
    }
  if (sock->sock != -1)
    GNUNET_break (0 == CLOSE (sock->sock));
  GNUNET_free_non_null (sock->addr);
  if (sock->ai != NULL)
    freeaddrinfo (sock->ai);
  GNUNET_free (sock);
}


/**
 * Close the socket and free associated resources. Pending
 * transmissions are simply dropped.  A pending receive call will be
 * called with an error code of "EPIPE".
 *
 * @param sock socket to destroy
 */
void
GNUNET_NETWORK_socket_destroy (struct GNUNET_NETWORK_SocketHandle *sock)
{
  if (sock->write_buffer_off == 0)
    sock->ai_pos = NULL;        /* if we're still trying to connect and have
                                   no message pending, stop trying! */
  GNUNET_assert (sock->sched != NULL);
  GNUNET_SCHEDULER_add_after (sock->sched,
                              GNUNET_YES,
                              GNUNET_SCHEDULER_PRIORITY_KEEP,
                              sock->connect_task,
                              &destroy_continuation, sock);
}

/**
 * Tell the receiver callback that a timeout was reached.
 */
static void
signal_timeout (struct GNUNET_NETWORK_SocketHandle *sh)
{
  GNUNET_NETWORK_Receiver receiver;

#if DEBUG_NETWORK
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Network signals time out to receiver!\n");
#endif
  GNUNET_assert (NULL != (receiver = sh->receiver));
  sh->receiver = NULL;
  receiver (sh->receiver_cls, NULL, 0, NULL, 0, 0);
}


/**
 * Tell the receiver callback that we had an IO error.
 */
static void
signal_error (struct GNUNET_NETWORK_SocketHandle *sh, int errcode)
{
  GNUNET_NETWORK_Receiver receiver;
  GNUNET_assert (NULL != (receiver = sh->receiver));
  sh->receiver = NULL;
  receiver (sh->receiver_cls, NULL, 0, sh->addr, sh->addrlen, errcode);
}


/**
 * This function is called once we either timeout
 * or have data ready to read.
 */
static void
receive_ready (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct GNUNET_NETWORK_SocketHandle *sh = cls;
  struct GNUNET_TIME_Absolute now;
  char buffer[sh->max];
  ssize_t ret;
  GNUNET_NETWORK_Receiver receiver;

  sh->read_task = GNUNET_SCHEDULER_NO_PREREQUISITE_TASK;
  now = GNUNET_TIME_absolute_get ();
  if ((now.value > sh->receive_timeout.value) ||
      (0 != (tc->reason & GNUNET_SCHEDULER_REASON_TIMEOUT)) ||
      (0 != (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN)))
    {
#if DEBUG_NETWORK
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Receive encounters error: time out...\n");
#endif
      signal_timeout (sh);
      return;
    }
  if (sh->sock == -1)
    {
      /* connect failed for good */
#if DEBUG_NETWORK
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Receive encounters error, socket closed...\n");
#endif
      signal_error (sh, ECONNREFUSED);
      return;
    }
  GNUNET_assert (FD_ISSET (sh->sock, tc->read_ready));
RETRY:
  ret = RECV (sh->sock, buffer, sh->max,
#ifndef MINGW
      // FIXME MINGW
      MSG_DONTWAIT
#else
      0
#endif
      );
  if (ret == -1)
    {
      if (errno == EINTR)
        goto RETRY;
#if DEBUG_NETWORK
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Error receiving: %s\n", STRERROR (errno));
#endif
      signal_error (sh, errno);
      return;
    }
#if DEBUG_NETWORK
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "receive_ready read %u/%u bytes from `%s'!\n",
	      (unsigned int) ret,
	      sh->max,
	      GNUNET_a2s(sh->addr, sh->addrlen));
#endif
  GNUNET_assert (NULL != (receiver = sh->receiver));
  sh->receiver = NULL;
  receiver (sh->receiver_cls, buffer, ret, sh->addr, sh->addrlen, 0);
}


/**
 * This function is called after establishing a connection either has
 * succeeded or timed out.  Note that it is possible that the attempt
 * timed out and that we're immediately retrying.  If we are retrying,
 * we need to wait again (or timeout); if we succeeded, we need to
 * wait for data (or timeout).
 */
static void
receive_again (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct GNUNET_NETWORK_SocketHandle *sh = cls;
  struct GNUNET_TIME_Absolute now;

  sh->read_task = GNUNET_SCHEDULER_NO_PREREQUISITE_TASK;
  if ((sh->sock == -1) &&
      (sh->connect_task == GNUNET_SCHEDULER_NO_PREREQUISITE_TASK))
    {
      /* not connected and no longer trying */
#if DEBUG_NETWORK
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Receive encounters error, socket closed...\n");
#endif
      signal_error (sh, ECONNREFUSED);
      return;
    }
  now = GNUNET_TIME_absolute_get ();
  if ((now.value > sh->receive_timeout.value) ||
      (0 != (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN)))
    {
#if DEBUG_NETWORK
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Receive encounters error: time out...\n");
#endif
      signal_timeout (sh);
      return;
    }
  if (sh->connect_task != GNUNET_SCHEDULER_NO_PREREQUISITE_TASK)
    {
      /* connect was retried */
      sh->read_task = GNUNET_SCHEDULER_add_after (tc->sched,
                                                  GNUNET_YES,
                                                  GNUNET_SCHEDULER_PRIORITY_KEEP,
                                                  sh->connect_task,
                                                  &receive_again, sh);
      return;
    }
  /* connect succeeded, wait for data! */
  sh->read_task = GNUNET_SCHEDULER_add_read (tc->sched,
					     GNUNET_YES,
					     GNUNET_SCHEDULER_PRIORITY_KEEP,
					     sh->connect_task,
					     GNUNET_TIME_absolute_get_remaining
					     (sh->receive_timeout),
					     sh->sock, &receive_ready,
					     sh);
}


/**
 * Receive data from the given socket.  Note that this function will
 * call "receiver" asynchronously using the scheduler.  It will
 * "immediately" return.  Note that there MUST only be one active
 * receive call per socket at any given point in time (so do not
 * call receive again until the receiver callback has been invoked).
 *
 * @param sched scheduler to use
 * @param sock socket handle
 * @param max maximum number of bytes to read
 * @param timeout maximum amount of time to wait (use -1 for "forever")
 * @param receiver function to call with received data
 * @param receiver_cls closure for receiver
 * @return scheduler task ID used for receiving, GNUNET_SCHEDULER_NO_PREREQUISITE_TASK on error
 */
GNUNET_SCHEDULER_TaskIdentifier
GNUNET_NETWORK_receive (struct GNUNET_NETWORK_SocketHandle *sock,
                        size_t max,
                        struct GNUNET_TIME_Relative timeout,
                        GNUNET_NETWORK_Receiver receiver, void *receiver_cls)
{
  struct GNUNET_SCHEDULER_TaskContext tc;

  GNUNET_assert ((sock->read_task == GNUNET_SCHEDULER_NO_PREREQUISITE_TASK) &&
                 (sock->receiver == NULL));
  sock->receiver = receiver;
  sock->receiver_cls = receiver_cls;
  sock->receive_timeout = GNUNET_TIME_relative_to_absolute (timeout);
  sock->max = max;
  memset (&tc, 0, sizeof (tc));
  tc.sched = sock->sched;
  tc.reason = GNUNET_SCHEDULER_REASON_PREREQ_DONE;
  receive_again (sock, &tc);
  return sock->read_task;
}


/**
 * Cancel receive job on the given socket.  Note that the
 * receiver callback must not have been called yet in order
 * for the cancellation to be valid.
 *
 * @param sock socket handle
 * @param task task identifier returned from the receive call
 * @return closure of the original receiver callback
 */
void *
GNUNET_NETWORK_receive_cancel (struct GNUNET_NETWORK_SocketHandle *sock,
                               GNUNET_SCHEDULER_TaskIdentifier task)
{
  GNUNET_assert (sock->read_task == task);
  GNUNET_assert (sock == GNUNET_SCHEDULER_cancel (sock->sched, task));
  sock->read_task = GNUNET_SCHEDULER_NO_PREREQUISITE_TASK;
  sock->receiver = NULL;
  return sock->receiver_cls;
}


/**
 * Try to call the transmit notify method (check if we do
 * have enough space available first)!
 *
 * @param sock socket for which we should do this processing
 * @return GNUNET_YES if we were able to call notify
 */
static int
process_notify (struct GNUNET_NETWORK_SocketHandle *sock)
{
  size_t used;
  size_t avail;
  size_t size;
  GNUNET_NETWORK_TransmitReadyNotify notify;

  GNUNET_assert (sock->write_task == GNUNET_SCHEDULER_NO_PREREQUISITE_TASK);
  if (NULL == (notify = sock->nth.notify_ready))
    return GNUNET_NO;
  used = sock->write_buffer_off - sock->write_buffer_pos;
  avail = sock->write_buffer_size - used;
  size = sock->nth.notify_size;
  if (sock->nth.notify_size > avail)
    return GNUNET_NO;
  sock->nth.notify_ready = NULL;
  if (sock->nth.timeout_task != GNUNET_SCHEDULER_NO_PREREQUISITE_TASK)
    {
      GNUNET_SCHEDULER_cancel (sock->sched, sock->nth.timeout_task);
      sock->nth.timeout_task = GNUNET_SCHEDULER_NO_PREREQUISITE_TASK;
    }
  if (sock->write_buffer_size - sock->write_buffer_off < size)
    {
      /* need to compact */
      memmove (sock->write_buffer,
               &sock->write_buffer[sock->write_buffer_pos], used);
      sock->write_buffer_off -= sock->write_buffer_pos;
      sock->write_buffer_pos = 0;
    }
  GNUNET_assert (sock->write_buffer_size - sock->write_buffer_off >= size);
  size = notify (sock->nth.notify_ready_cls,
                 sock->write_buffer_size - sock->write_buffer_off,
                 &sock->write_buffer[sock->write_buffer_off]);
  sock->write_buffer_off += size;
  return GNUNET_YES;
}


/**
 * Task invoked by the scheduler when a call to transmit
 * is timing out (we never got enough buffer space to call
 * the callback function before the specified timeout
 * expired).
 *
 * This task notifies the client about the timeout.
 */
static void
transmit_timeout (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct GNUNET_NETWORK_SocketHandle *sock = cls;
  GNUNET_NETWORK_TransmitReadyNotify notify;

#if DEBUG_NETWORK
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Transmit fails, time out reached.\n");
#endif
  notify = sock->nth.notify_ready;
  sock->nth.notify_ready = NULL;
  notify (sock->nth.notify_ready_cls, 0, NULL);
}


static void
transmit_error (struct GNUNET_NETWORK_SocketHandle *sock)
{
  if (sock->nth.notify_ready == NULL)
    return;                     /* nobody to tell about it */
  if (sock->nth.timeout_task != GNUNET_SCHEDULER_NO_PREREQUISITE_TASK)
    {
      GNUNET_SCHEDULER_cancel (sock->sched, sock->nth.timeout_task);
      sock->nth.timeout_task = GNUNET_SCHEDULER_NO_PREREQUISITE_TASK;
    }
  transmit_timeout (sock, NULL);
}


/**
 * See if we are now connected.  If not, wait longer for
 * connect to succeed.  If connected, we should be able
 * to write now as well, unless we timed out.
 */
static void
transmit_ready (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct GNUNET_NETWORK_SocketHandle *sock = cls;
  ssize_t ret;
  size_t have;

  GNUNET_assert (sock->write_task != GNUNET_SCHEDULER_NO_PREREQUISITE_TASK);
  sock->write_task = GNUNET_SCHEDULER_NO_PREREQUISITE_TASK;
  if (sock->connect_task != GNUNET_SCHEDULER_NO_PREREQUISITE_TASK)
    {
      /* still waiting for connect */
      GNUNET_assert (sock->write_task ==
                     GNUNET_SCHEDULER_NO_PREREQUISITE_TASK);
      sock->write_task =
        GNUNET_SCHEDULER_add_delayed (tc->sched, GNUNET_NO,
                                      GNUNET_SCHEDULER_PRIORITY_KEEP,
                                      sock->connect_task,
                                      GNUNET_TIME_UNIT_ZERO, &transmit_ready,
                                      sock);
      return;
    }
  if (sock->sock == -1)
    {
#if DEBUG_NETWORK
      GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                  _("Could not satisfy pending transmission request, socket closed or connect failed.\n"));
#endif
      transmit_error (sock);
      return;                   /* connect failed for good, we're finished */
    }
  if ((tc->write_ready == NULL) || (!FD_ISSET (sock->sock, tc->write_ready)))
    {
      /* special circumstances: not yet ready to write */
      goto SCHEDULE_WRITE;
    }
  GNUNET_assert (sock->write_buffer_off >= sock->write_buffer_pos);
  process_notify (sock);
  have = sock->write_buffer_off - sock->write_buffer_pos;
  if (have == 0)
    {
      /* no data ready for writing, terminate write loop */
      return;
    }
RETRY:
  ret = SEND (sock->sock,
              &sock->write_buffer[sock->write_buffer_pos],
              have,
#ifndef MINGW
              // FIXME NILS
              MSG_DONTWAIT | MSG_NOSIGNAL
#else
              0
#endif
  );
  if (ret == -1)
    {
      if (errno == EINTR)
        goto RETRY;
#if DEBUG_NETWORK
      GNUNET_log_strerror (GNUNET_ERROR_TYPE_DEBUG, "send");
#endif
      SHUTDOWN (sock->sock, SHUT_RDWR);
      GNUNET_break (0 == CLOSE (sock->sock));
      sock->sock = -1;
      transmit_error (sock);
      return;
    }
#if DEBUG_NETWORK
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "transmit_ready transmitted %u/%u bytes to `%s'\n",
	      (unsigned int) ret,
	      have,
	      GNUNET_a2s(sock->addr, sock->addrlen));
#endif
  sock->write_buffer_pos += ret;
  if (sock->write_buffer_pos == sock->write_buffer_off)
    {
      /* transmitted all pending data */
      sock->write_buffer_pos = 0;
      sock->write_buffer_off = 0;
    }
  if ((sock->write_buffer_off == 0) && (NULL == sock->nth.notify_ready))
    return;                     /* all data sent! */
  /* not done writing, schedule more */
SCHEDULE_WRITE:
  if (sock->write_task == GNUNET_SCHEDULER_NO_PREREQUISITE_TASK)
    sock->write_task =
      GNUNET_SCHEDULER_add_write (tc->sched,
                                  GNUNET_NO,
                                  GNUNET_SCHEDULER_PRIORITY_KEEP,
                                  GNUNET_SCHEDULER_NO_PREREQUISITE_TASK,
                                  GNUNET_TIME_UNIT_FOREVER_REL,
                                  sock->sock, &transmit_ready, sock);
}


/**
 * Ask the socket to call us once the specified number of bytes
 * are free in the transmission buffer.  May call the notify
 * method immediately if enough space is available.
 *
 * @param sock socket
 * @param size number of bytes to send
 * @param timeout after how long should we give up (and call
 *        notify with buf NULL and size 0)?
 * @param notify function to call
 * @param notify_cls closure for notify
 * @return non-NULL if the notify callback was queued,
 *         NULL if we are already going to notify someone else (busy)
 */
struct GNUNET_NETWORK_TransmitHandle *
GNUNET_NETWORK_notify_transmit_ready (struct GNUNET_NETWORK_SocketHandle
                                      *sock, size_t size,
                                      struct GNUNET_TIME_Relative timeout,
                                      GNUNET_NETWORK_TransmitReadyNotify
                                      notify, void *notify_cls)
{
  if (sock->nth.notify_ready != NULL)
    return NULL;
  GNUNET_assert (notify != NULL);
  GNUNET_assert (sock->write_buffer_size >= size);

  if ((sock->sock == -1) &&
      (sock->connect_task == GNUNET_SCHEDULER_NO_PREREQUISITE_TASK))
    {
#if DEBUG_NETWORK
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Transmission request fails, connection failed.\n");
#endif
      notify (notify_cls, 0, NULL);
      return &sock->nth;
    }
  GNUNET_assert (sock->write_buffer_off <= sock->write_buffer_size);
  GNUNET_assert (sock->write_buffer_pos <= sock->write_buffer_size);
  GNUNET_assert (sock->write_buffer_pos <= sock->write_buffer_off);
  sock->nth.notify_ready = notify;
  sock->nth.notify_ready_cls = notify_cls;
  sock->nth.sh = sock;
  sock->nth.notify_size = size;
  sock->nth.transmit_timeout = GNUNET_TIME_relative_to_absolute (timeout);
  sock->nth.timeout_task = GNUNET_SCHEDULER_add_delayed (sock->sched,
                                                         GNUNET_NO,
                                                         GNUNET_SCHEDULER_PRIORITY_KEEP,
                                                         GNUNET_SCHEDULER_NO_PREREQUISITE_TASK,
                                                         timeout,
                                                         &transmit_timeout,
                                                         sock);
  if (sock->write_task == GNUNET_SCHEDULER_NO_PREREQUISITE_TASK)
    sock->write_task = GNUNET_SCHEDULER_add_delayed (sock->sched,
                                                     GNUNET_NO,
                                                     GNUNET_SCHEDULER_PRIORITY_KEEP,
                                                     sock->connect_task,
                                                     GNUNET_TIME_UNIT_ZERO,
                                                     &transmit_ready, sock);
  return &sock->nth;
}


/**
 * Cancel the specified transmission-ready
 * notification.
 */
void
GNUNET_NETWORK_notify_transmit_ready_cancel (struct
                                             GNUNET_NETWORK_TransmitHandle *h)
{
  GNUNET_assert (h->notify_ready != NULL);
  GNUNET_SCHEDULER_cancel (h->sh->sched, h->timeout_task);
  h->timeout_task = GNUNET_SCHEDULER_NO_PREREQUISITE_TASK;
  h->notify_ready = NULL;
}


#if 0                           /* keep Emacsens' auto-indent happy */
{
#endif
#ifdef __cplusplus
}
#endif
