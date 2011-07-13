#include <zebra.h>

#include <thread.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include "stream.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <sockopt.h>
#include <sys/time.h>
#include <privs.h>
#include <memory.h>
#include "log.h"
#include "linklist.h"
#include "sv.h"

#include "sisis_structs.h"
#include "sisis_api.h"
#include "sisis_process_types.h"

extern struct zebra_privs_t shimd_privs;

#include "shim/shimd.h"
#include "shim/shim_sisis.h"
#include "shim/shim_network.h"
#include "shim/shim_interface.h"
#include "rospf6d/ospf6_message.h"
#include "shim/shim_packet.h"

#define BACKLOG 10

int
shim_sisis_init (uint64_t host_num)
{
  int sockfd;
  uint64_t ptype, ptype_version, pid, timestamp;
  char sisis_addr[INET6_ADDRSTRLEN];

  // Store process type
  ptype = (uint64_t)SISIS_PTYPE_RIBCOMP_SHIM;
  ptype_version = (uint64_t)VERSION;

  // Get pid
  pid = getpid();        // Get start time
  struct timeval tv;
  gettimeofday(&tv, NULL);
  timestamp = (tv.tv_sec * 100 + (tv.tv_usec / 10000)) & 0x00000000ffffffffLLU;   // In 100ths of seconds

  // Register SIS-IS address
  if (sisis_register(sisis_addr, ptype, ptype_version, host_num, pid, timestamp) != 0)
  {
    printf("Failed to register SIS-IS address.\n");
    exit(1);        
  }
  
  printf("Opening socket at %s on port %i.\n", sisis_addr, SHIM_SISIS_PORT);

  // Set up socket address info
  struct addrinfo hints, *addr;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET6;     // IPv6
  hints.ai_socktype = SOCK_STREAM;  // TCP
  char port_str[8];
  sprintf(port_str, "%u", SHIM_SISIS_PORT);
  getaddrinfo(sisis_addr, port_str, &hints, &addr);

  // Create socket
  if ((sockfd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol)) < 0)
  {
    printf("Failed to open socket.\n");
    exit(1);
  }

  if ((shim_sisis_listener(sockfd, addr->ai_addr, addr->ai_addrlen)) != 0)
    close(sockfd);

  // Bind to port
//  if (bind(sockfd, addr->ai_addr, addr->ai_addrlen) < 0)
//  {
//    printf("Failed to bind socket to port.\n");
//    exit(2);
//  }

//  if (listen(sockfd, BACKLOG) < 0)
//  {
//    printf("Failed to listen.\n");
//    exit(2);
//  }

  return sockfd;
}

unsigned int
number_of_sisis_addrs_for_process_type (unsigned int ptype) 
{
  char addr[INET6_ADDRSTRLEN+1];  
  unsigned int lsize;

  sisis_create_addr(addr, (uint64_t)ptype, (uint64_t)0, (uint64_t)0, (uint64_t)0, (uint64_t)0); 
  struct prefix_ipv6 prefix = sisis_make_ipv6_prefix(addr, 37);
  struct list_sis * addrs = get_sisis_addrs_for_prefix(&prefix);

  lsize = addrs->size;

  FREE_LINKED_LIST (addrs);
  return lsize;
}

unsigned int
are_checksums_same (void)
{
  int same = 0;
  struct listnode * node, * nnode;
  struct sisis_listener * listener;
  struct sisis_listener * listener_swp = (struct sisis_listener *)listgetdata(listhead(sm->listen_sockets));
  u_int16_t chsum_swp = listener_swp->chksum;

  for(ALL_LIST_ELEMENTS (sm->listen_sockets, node, nnode, listener))
  {
    zlog_debug("checksum: %d\n", listener->chksum);
    if(listener->chksum == chsum_swp)
    {
      same = 1;
      chsum_swp = listener->chksum;
    }
    else
    {
      return 0;
     } 
  }  

  return same;
}

void
reset_checksums (void)
{
  struct listnode * node, * nnode;
  struct sisis_listener * listener;

  for(ALL_LIST_ELEMENTS (sm->listen_sockets, node, nnode, listener))
  {
    listener->chksum = 0;
  }
}

unsigned int 
number_of_listeners (void)
{
  return listcount(sm->listen_sockets);
}

int
shim_sisis_listener(int sock, struct sockaddr * sa, socklen_t salen)
{
  int ret,en;

  sockopt_reuseaddr(sock);
  sockopt_reuseport(sock);

  if (sa->sa_family == AF_INET6) 
  {
    int on = 1;
    setsockopt (sock, IPPROTO_IPV6, IPV6_V6ONLY,
                (void *) &on, sizeof (on));
  }

  if (shimd_privs.change (ZPRIVS_RAISE) )
    zlog_err ("shim_sisis_listener: could not raise privs");

  ret = bind (sock, sa, salen);
  en = errno;
  if (shimd_privs.change (ZPRIVS_LOWER) )
    zlog_err ("shim_sisis_listener: could not lower privs");

  if (ret < 0)
  {
    zlog_err ("bind: %s", safe_strerror (en));
    return ret;
  }

  ret = listen (sock, 3);
  if (ret < 0)
  {
    zlog_err ("listen: %s", safe_strerror (errno));
    return ret;
  }

  thread_add_read (master, shim_sisis_accept, NULL, sock);

  return 0;
}

int
shim_sisis_accept(struct thread * thread)
{
  int accept_sock;
  int sisis_sock;
  struct sisis_listener *listener;
  union sockunion su;
  char buf[SU_ADDRSTRLEN];

  accept_sock = THREAD_FD (thread);
  if (accept_sock < 0)
    {   
      zlog_err ("accept_sock is negative value %d", accept_sock);
      return -1; 
    }   
  thread_add_read (master, shim_sisis_accept, NULL, accept_sock);
  
  sisis_sock = sockunion_accept(accept_sock, &su);

  if (sisis_sock < 0)
    {
      zlog_err ("[Error] SISIS socket accept failed (%s)", safe_strerror (errno));
      return -1;
    } 
  
  zlog_notice ("SISIS connection from host %s", inet_sutop (&su, buf));

  listener = XMALLOC (MTYPE_SHIM_SISIS_LISTENER, sizeof(*listener));
  listener->fd = accept_sock;
  listener->ibuf = stream_new (SV_HEADER_SIZE + 1500);
//  memcpy(&listener->su, sa, salen);
  listener->sisis_fd = sisis_sock;
  listener->thread = thread_add_read (master, shim_sisis_read, listener, sisis_sock);
  listnode_add (sm->listen_sockets, listener);

  return 0;
}

int 
shim_sisis_read(struct thread * thread)
{
  struct sisis_listener *listener;
  int sisis_sock;
  uint16_t length, command, checksum;
  int already;
  u_int ifindex;
  struct shim_interface * si;
  struct in6_addr src;
  struct in6_addr dst;

  zlog_notice("Reading packet from SISIS connection!\n");

  /* first of all get listener pointer. */
  listener = THREAD_ARG (thread);
  sisis_sock = THREAD_FD (thread);

  if ((already = stream_get_endp(listener->ibuf)) < SV_HEADER_SIZE)
  {
    ssize_t nbytes;
    if (((nbytes = stream_read_try (listener->ibuf, sisis_sock, SV_HEADER_SIZE-already)) == 0) || (nbytes == -1))
    {
      return -1;
    }
    
    if(nbytes != (SV_HEADER_SIZE - already))
    {
      listener->thread = thread_add_read (master, shim_sisis_read, listener, sisis_sock);
      return 0;
    }
    already = SV_HEADER_SIZE;
  }

  stream_set_getp(listener->ibuf, 0);
 
  /* read header packet. */
  length = stream_getw (listener->ibuf);
  command = stream_getw (listener->ibuf);

  // will be 0 so may be discarded
  stream_get (&src, listener->ibuf, sizeof (struct in6_addr));
  stream_get (&dst, listener->ibuf, sizeof (struct in6_addr));

  checksum = stream_getw (listener->ibuf);

  zlog_debug("SISIS: length: %d, command: %d, checksum: %d on sock %d\n", length, command, checksum, sisis_sock);

  if(length > STREAM_SIZE(listener->ibuf))
  {
    struct stream * ns;
    zlog_warn("message size exceeds buffer size");
    ns = stream_new(length);
    stream_copy(ns, listener->ibuf);
    stream_free(listener->ibuf);
    listener->ibuf = ns;
  }

  if(already < length)
  {
    ssize_t nbytes;
    if(((nbytes = stream_read_try(listener->ibuf, sisis_sock, length-already)) == 0) || nbytes == -1)
    {
      return -1;
    } 
    if(nbytes != (length-already))
    {
      listener->thread = thread_add_read (master, shim_sisis_read, listener, sisis_sock);
      return 0;
    }
  }

  length -= SV_HEADER_SIZE;

  switch(command)
  {
    case SV_JOIN_ALLSPF:
      zlog_debug("join allspf received");
      ifindex = stream_getl(listener->ibuf);
      shim_join_allspfrouters (ifindex);
      zlog_debug("index: %d", ifindex);
      break;
    case SV_LEAVE_ALLSPF:
      zlog_debug("leave allspf received");
      ifindex = stream_getl(listener->ibuf);
      shim_leave_allspfrouters (ifindex);
      zlog_debug("index: %d\n", ifindex);
      break;
    case SV_JOIN_ALLD:
      zlog_debug("join alld received");
      ifindex = stream_getl(listener->ibuf);
      shim_join_alldrouters (ifindex);
      zlog_debug("index: %d", ifindex);
      break;
    case SV_LEAVE_ALLD:
      zlog_debug("leave alld received");
      ifindex = stream_getl(listener->ibuf);
      shim_leave_alldrouters (ifindex);
      zlog_debug("index: %d", ifindex);
      break;
    case SV_MESSAGE:
      zlog_debug("SISIS hello message received");
      unsigned int num_of_addrs = number_of_sisis_addrs_for_process_type(SISIS_PTYPE_RIBCOMP_OSPF6);
      unsigned int num_of_listeners = number_of_listeners();
      zlog_debug("num of listeners: %d, num of addrs: %d", num_of_listeners, num_of_addrs);
      float received_ratio = num_of_listeners/num_of_addrs;;
      listener->chksum = checksum;
      if(received_ratio > (1/2))
      { 
        if(are_checksums_same())
        {
          ifindex = ntohl(stream_getl(listener->ibuf));
          zlog_debug("index: %d", ifindex);
          si = shim_interface_lookup_by_ifindex (ifindex);
          reset_checksums();
          shim_hello_send(listener->ibuf, si);
        }
        else
        { 
          zlog_notice("Checksums are not all the same");
        }
      } 
      else
      {
        zlog_notice("Not enough processes have sent their data: buffering ...");
      }
      break;
    default:
      break;
  }

  if (sisis_sock < 0) 
    /* Connection was closed during packet processing. */
    return -1;

  /* Register read thread. */
  stream_reset(listener->ibuf);
 
  /* prepare for next packet. */
  listener->thread = thread_add_read (master, shim_sisis_read, listener, sisis_sock);

  return 0;
}

int
shim_sisis_write (struct stream * obuf, struct buffer * wb)
{
  struct listnode * node, * nnode;
  struct sisis_listener * listener;

  printf("num of listeners %d\n", listcount(sm->listen_sockets));
  for(ALL_LIST_ELEMENTS (sm->listen_sockets, node,  nnode, listener))
  {
    printf("listener fd: %d\n", listener->sisis_fd);
    buffer_write(wb, listener->sisis_fd, STREAM_DATA(obuf), stream_get_endp(obuf));
  }

  return 0;
}
