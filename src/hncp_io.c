/*
 * $Id: dncp_io.c $
 *
 * Author: Markus Stenberg <mstenber@cisco.com>
 *
 * Copyright (c) 2013 cisco Systems, Inc.
 *
 * Created:       Mon Nov 25 14:00:10 2013 mstenber
 * Last modified: Thu Feb 26 12:39:42 2015 mstenber
 * Edit time:     296 min
 *
 */

/* This module implements I/O needs of hncp. Notably, it has both
 * functionality that deals with sockets, and bit more abstract ones
 * that just deal with buffers for input and output (thereby
 * facilitating unit testing without using real sockets). */

#include "hncp_i.h"
#undef __unused
/* In linux, fcntl.h includes something with __unused. Argh. */
#include <fcntl.h>
#define __unused __attribute__((unused))
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <libubox/usock.h>
#include <ifaddrs.h>

#ifdef __linux__
#define AF_LINK AF_PACKET
#include <linux/if_packet.h>
#endif /* __linux__ */


int
dncp_io_get_hwaddrs(unsigned char *buf, int buf_left)
{
  struct ifaddrs *ia, *p;
  int r = getifaddrs(&ia);
  void *a1 = buf, *a2 = buf + ETHER_ADDR_LEN;
  int addrs = 0;
  unsigned char zeroed_addr[] = {0, 0, 0, 0, 0, 0};

  if (buf_left < ETHER_ADDR_LEN * 2)
    return 0;
  memset(buf, 0, ETHER_ADDR_LEN * 2);
  if (r)
    return 0;
  for (p = ia ; p ; p = p->ifa_next)
    if (p->ifa_addr && p->ifa_addr->sa_family == AF_LINK)
      {
        void *a;
#ifdef __linux__
        struct sockaddr_ll *sll = (struct sockaddr_ll *) p->ifa_addr;
        a = sll->sll_addr;
#else
        a = &p->ifa_addr->sa_data[0];
#endif /* __linux__ */
        if (memcmp(a, zeroed_addr, sizeof(zeroed_addr)) == 0)
          continue;
        if (!addrs || memcmp(a1, a, ETHER_ADDR_LEN) < 0)
          memcpy(a1, a, ETHER_ADDR_LEN);
        if (!addrs || memcmp(a2, a, ETHER_ADDR_LEN) > 0)
          memcpy(a2, a, ETHER_ADDR_LEN);
        addrs++;
      }
  L_INFO("dncp_io_get_hwaddrs => %s", HEX_REPR(buf, ETHER_ADDR_LEN * 2));
  freeifaddrs(ia);
  if (!addrs)
    {
      L_ERR("dncp_io_get_hwaddrs failed - no AF_LINK addresses");
      return 0;
    }
  return ETHER_ADDR_LEN * 2;
}

static void _timeout(struct uloop_timeout *t)
{
  dncp o = container_of(t, dncp_s, timeout);
  dncp_run(o);
}

static void _fd_callback(struct uloop_fd *u, unsigned int events __unused)
{
  dncp o = container_of(u, dncp_s, ufd);
  dncp_poll(o);
}

bool dncp_io_init(dncp o)
{
  int s;
  int on = 1;
  int off = 0;
  struct sockaddr_in6 addr;

  if (!o->udp_port)
    o->udp_port = HNCP_PORT;
  s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  if (s<0) {
    L_ERR("unable to create IPv6 UDP socket");
    return false;
  }
  fcntl(s, F_SETFL, O_NONBLOCK);
  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(o->udp_port);
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (bind(s, (struct sockaddr *)&addr, sizeof(addr))<0) {
    L_ERR("unable to bind to port %d", o->udp_port);
    return false;
  }
  if (setsockopt(s, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on)) < 0)
    {
      L_ERR("unable to setsockopt IPV6_RECVPKTINFO:%s", strerror(errno));
      return false;
    }
  if (setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &off, sizeof(off)) < 0)
    {
      L_ERR("unable to setsockopt IPV6_MULTICAST_LOOP:%s", strerror(errno));
      return false;
    }
  o->udp_socket = s;
  o->timeout.cb = _timeout;

  memset(&o->ufd, 0, sizeof(o->ufd));
  o->ufd.fd = o->udp_socket;
  o->ufd.cb = _fd_callback;
  uloop_fd_add(&o->ufd, ULOOP_READ);
  return true;
}

void dncp_io_uninit(dncp o)
{
  close(o->udp_socket);
  /* clear the timer from uloop. */
  uloop_timeout_cancel(&o->timeout);
  /* and the fd also. */
  (void)uloop_fd_delete(&o->ufd);
}

bool dncp_io_set_ifname_enabled(dncp o,
                                const char *ifname,
                                bool enabled)
{
  struct ipv6_mreq val;

  val.ipv6mr_multiaddr = o->profile_data.multicast_address;
  L_DEBUG("dncp_io_set_ifname_enabled %s %s",
          ifname, enabled ? "enabled" : "disabled");
  uint32_t ifindex = 0;
  dncp_link l = dncp_find_link_by_name(o, ifname, false);
  if (!(l && (ifindex = l->ifindex)))
    if (!(ifindex = if_nametoindex(ifname)))
      {
        L_DEBUG("unable to enable on %s - if_nametoindex: %s",
                ifname, strerror(errno));
        goto fail;
      }
  val.ipv6mr_interface = ifindex;
  if (setsockopt(o->udp_socket,
                 IPPROTO_IPV6,
                 enabled ? IPV6_ADD_MEMBERSHIP : IPV6_DROP_MEMBERSHIP,
                 (char *) &val, sizeof(val)) < 0)
    {
      L_ERR("unable to enable on %s - setsockopt:%s", ifname, strerror(errno));
      goto fail;
    }
  /* Yay. It succeeded(?). */
  return true;

 fail:
  return false;
}

void dncp_io_schedule(dncp o, int msecs)
{
  //1ms timeout was weird in VirtualBox env (causing less than 1ms to).
  uloop_timeout_set(&o->timeout, msecs?(msecs+1):0);
}

ssize_t dncp_io_recvfrom(dncp o, void *buf, size_t len,
                         char *ifname,
                         struct sockaddr_in6 *src,
                         struct in6_addr *dst)
{
  struct iovec iov = {buf, len};
  unsigned char cmsg_buf[256];
  struct msghdr msg = {src, sizeof(*src), &iov, 1,
                       cmsg_buf, sizeof(cmsg_buf), 0};
  ssize_t l;
  struct cmsghdr *h;
  struct in6_pktinfo *ipi6;

  while (1)
    {
#ifdef DTLS
      if (o->profile_data.d)
        {
          l = dtls_recvfrom(o->profile_data.d, buf, len, src);
          if (l > 0)
            {
              if (!IN6_IS_ADDR_LINKLOCAL(&src->sin6_addr))
                continue;
              /* In case of DTLS, we have just to trust that it has sane
               * scope id as we use that for interface determination. */
              if (!src->sin6_scope_id)
                {
                  L_DEBUG("linklocal w/o scope id..?");
                  continue;
                }
              if (!if_indextoname(src->sin6_scope_id, ifname))
                {
                  L_ERR("unable to receive (dtls) - if_indextoname:%s",
                        strerror(errno));
                  continue;
                }
              /* We do not _know_ destination address. However,
               * the code does not really care, so we fake something
               * here that looks like unicast linklocal address. */
              struct in6_addr dummy = { .s6_addr = { 0xfe,0x80 }};
              *dst = dummy;
              break;
            }
        }
#endif /* DTLS */
      l = recvmsg(o->udp_socket, &msg, MSG_DONTWAIT);
      if (l <= 0)
        {
          if (l < 0 && errno != EWOULDBLOCK)
            L_DEBUG("unable to receive - recvmsg:%s", strerror(errno));
          return l;
        }
      *ifname = 0;
      for (h = CMSG_FIRSTHDR(&msg); h ;
           h = CMSG_NXTHDR(&msg, h))
        if (h->cmsg_level == IPPROTO_IPV6
            && h->cmsg_type == IPV6_PKTINFO)
          {
            ipi6 = (struct in6_pktinfo *)CMSG_DATA(h);
            if (!if_indextoname(ipi6->ipi6_ifindex, ifname))
              {
                *ifname = 0;
                L_ERR("unable to receive - if_indextoname:%s",
                      strerror(errno));
                break;
              }
            *dst = ipi6->ipi6_addr;
          }
      if (!*ifname)
        {
          L_ERR("unable to receive - no ifname");
          continue;
        }
#ifdef DTLS
      if (o->profile_data.d && !IN6_IS_ADDR_MULTICAST(dst))
        {
          L_ERR("plaintext unicast received when in dtls mode - skip");
          continue;
        }
#endif /* DTLS */
      break;
    }
  /* not really reachable */
  return l;
}

ssize_t dncp_io_sendto(dncp o, void *buf, size_t len,
                       const struct sockaddr_in6 *dst)
{
  int flags = 0;
  ssize_t r;

#ifdef DTLS
  if (o->profile_data.d && !IN6_IS_ADDR_MULTICAST(&dst->sin6_addr))
    {
      /* Change destination port to DTLS server port too if it is the
       * default port. Otherwise answer on the different port (which
       * is presumably already DTLS protected due to protection in
       * input path).*/
      struct sockaddr_in6 rdst = *dst;
      if (rdst.sin6_port == htons(HNCP_PORT))
        rdst.sin6_port = htons(HNCP_DTLS_SERVER_PORT);
      r = dtls_sendto(o->profile_data.d, buf, len, &rdst);
    }
  else
#endif /* DTLS */
  r = sendto(o->udp_socket, buf, len, flags,
             (struct sockaddr *)dst, sizeof(*dst));
#if L_LEVEL >= 3
  if (r < 0)
    {
      char buf[128];
      const char *c = inet_ntop(AF_INET6, &dst->sin6_addr, buf, sizeof(buf));
      L_ERR("unable to send to %s - sendto:%s",
            c ? c : "?", strerror(errno));
    }
#endif /* L_LEVEL >= 3 */
  return r;
}

hnetd_time_t dncp_io_time(dncp o __unused)
{
  return hnetd_time();
}

#ifdef DTLS

void _dtls_readable_callback(dtls d __unused, void *context)
{
  dncp o = context;

  dncp_poll(o);
}


void hncp_set_dtls(dncp o, dtls d)
{
  o->profile_data.d = d;
  dtls_set_readable_callback(d, _dtls_readable_callback, o);
}

#endif /* DTLS */
