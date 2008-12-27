/***************************************************************************
 projekt              : olsrd-quagga
 file                 : quagga.c
 usage                : communication with the zebra-daemon
 copyright            : (C) 2006 by Immo 'FaUl' Wehrenberg
 e-mail               : immo@chaostreff-dortmund.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License version 2 as     *
 *   published by the Free Software Foundation or - at your option - under *
 *   the terms of the GNU General Public Licence version 2 but can be      *
 *   linked to any BSD-Licenced Software with public available sourcecode. *
 *                                                                         *
 ***************************************************************************/


#define HAVE_SOCKLEN_T

#include "quagga.h"
#include "olsr.h" /* olsr_exit
                     olsr_malloc */
#include "log.h" /* olsr_syslog */
#include "common/string.h" /* strscpy */

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef USE_UNIX_DOMAIN_SOCKET
#include <sys/un.h>
#endif


/* prototypes intern */
static struct {
  char status; // internal status
  char options; // internal options
  int sock; // Socket to zebra...
  char redistribute[ZEBRA_ROUTE_MAX];
  char distance;
  char flags;
  struct zebra_route *v4_rt; // routes currently exportet to zebra
} zebra;

static void *my_realloc (void *, size_t, const char *);
static void zebra_connect (void);
static unsigned char *try_read (ssize_t *);
static int zebra_send_command (unsigned char *);
static unsigned char* zebra_route_packet (uint16_t, struct zebra_route *);
static unsigned char *zebra_redistribute_packet (unsigned char, unsigned char);
static struct zebra_route *zebra_parse_route (unsigned char *);
#if 0
static void zebra_reconnect (void);
#endif
static void free_ipv4_route (struct zebra_route *);


static void *my_realloc (void *buf, size_t s, const char *c) {
  buf = realloc (buf, s);
  if (!buf) {
    OLSR_PRINTF (1, "(QUAGGA) OUT OF MEMORY: %s\n", strerror(errno));
    olsr_syslog(OLSR_LOG_ERR, "olsrd: out of memory!: %m\n");
    olsr_exit(c, EXIT_FAILURE);
  }
  return buf;
}


void init_zebra (void) {
  zebra_connect();
  if (!(zebra.status&STATUS_CONNECTED))
    olsr_exit ("(QUAGGA) AIIIII, could not connect to zebra! is zebra running?",
	       EXIT_FAILURE);
}


void zebra_cleanup (void) {
  int i;
  struct rt_entry *tmp;

  if (zebra.options & OPTION_EXPORT) {
    OLSR_FOR_ALL_RT_ENTRIES(tmp) {
      zebra_del_route(tmp);
    } OLSR_FOR_ALL_RT_ENTRIES_END(tmp);
  }

  for (i = 0; i < ZEBRA_ROUTE_MAX; i++)
    if (zebra.redistribute[i]) zebra_disable_redistribute(i);
}


#if 0
static void zebra_reconnect (void) {
  struct rt_entry *tmp;
  int i;

  zebra_connect();
  if (!(zebra.status & STATUS_CONNECTED)) return; // try again next time

  if (zebra.options & OPTION_EXPORT) {
    OLSR_FOR_ALL_RT_ENTRIES(tmp) {
      zebra_add_route (tmp);
    } OLSR_FOR_ALL_RT_ENTRIES_END(tmp);
  }

  for (i = 0; i < ZEBRA_ROUTE_MAX; i++)
    if (zebra.redistribute[i]) zebra_redistribute(i + 1);
  /* Zebra sends us all routes of type it knows after
     zebra_redistribute(type) */
}
#endif


/* Connect to the zebra-daemon, returns a socket */
static void zebra_connect (void) {

  int ret;

#ifndef USE_UNIX_DOMAIN_SOCKET
  struct sockaddr_in i;
  if (close (zebra.sock) < 0) olsr_exit ("(QUAGGA) Could not close socket!", EXIT_FAILURE);


  zebra.sock = socket (AF_INET,SOCK_STREAM, 0);
#else
  struct sockaddr_un i;
  if (close (zebra.sock) < 0) olsr_exit ("(QUAGGA) Could not close socket!", EXIT_FAILURE);

  zebra.sock = socket (AF_UNIX,SOCK_STREAM, 0);
#endif

  if (zebra.sock <0 )
    olsr_exit("(QUAGGA) Could not create socket!", EXIT_FAILURE);

  memset (&i, 0, sizeof i);
#ifndef USE_UNIX_DOMAIN_SOCKET
  i.sin_family = AF_INET;
  i.sin_port = htons (ZEBRA_PORT);
  i.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
#else
  i.sun_family = AF_UNIX;
  strscpy (i.sun_path, ZEBRA_SOCKET, sizeof(i.sun_path));
#endif

  ret = connect (zebra.sock, (struct sockaddr *)&i, sizeof i);
  if  (ret < 0) zebra.status &= ~STATUS_CONNECTED;
  else zebra.status |= STATUS_CONNECTED;
}


/* Sends a command to zebra, command is
   the command defined in zebra.h, options is the packet-payload,
   optlen the length, of the payload */
static int zebra_send_command (unsigned char *options) {

  unsigned char *pnt;
  uint16_t len;
  int ret;

  if (!(zebra.status & STATUS_CONNECTED)) return 0;

  pnt = options;
  memcpy (&len, pnt, 2);

  len = ntohs(len);

  do {
    ret = write (zebra.sock, pnt, len);
    if (ret < 0) {
      if ((errno == EINTR) || (errno == EAGAIN)) {
	errno = 0;
        ret = 0;
	continue;
      }
      else {
	OLSR_PRINTF (1, "(QUAGGA) Disconnected from zebra\n");
	zebra.status &= ~STATUS_CONNECTED;
	free (options);
	return -1;
      }
    }
    pnt = pnt+ret;
  } while ((len -= ret));
  free (options);
  return 0;
}


/* Creates a Route-Packet-Payload, needs address, netmask, nexthop,
   distance, and a pointer of an size_t */
static unsigned char* zebra_route_packet (uint16_t cmd, struct zebra_route *r) {

  int count;
  uint8_t len;
  uint16_t size;
  uint32_t ind, metric;

  unsigned char *cmdopt, *t;

  cmdopt = olsr_malloc (ZEBRA_MAX_PACKET_SIZ , "zebra add_v4_route");

  t = &cmdopt[2];
  *t++ = cmd;
  *t++ = r->type;
  *t++ = r->flags;
  *t++ = r->message;
  *t++ = r->prefixlen;
  len = (r->prefixlen + 7) / 8;
  memcpy (t, &r->prefix.v4.s_addr, len);
  t = t + len;

  if (r->message & ZAPI_MESSAGE_NEXTHOP) {
    *t++ = r->nexthop_num + r->ifindex_num;
    
      for (count = 0; count < r->nexthop_num; count++)
	{
	  *t++ = ZEBRA_NEXTHOP_IPV4;
	  memcpy (t, &r->nexthop[count].v4.s_addr,
		  sizeof r->nexthop[count].v4.s_addr);
	  t += sizeof r->nexthop[count].v4.s_addr;
	}
      for (count = 0; count < r->ifindex_num; count++)
	{
	  *t++ = ZEBRA_NEXTHOP_IFINDEX;
          ind = htonl(r->ifindex[count]);
	  memcpy (t, &ind, sizeof ind);
	  t += sizeof ind;
	}
    }
  if ((r->message & ZAPI_MESSAGE_DISTANCE) > 0)
    *t++ = r->distance;
  if ((r->message & ZAPI_MESSAGE_METRIC) > 0)
    {
      metric = htonl (r->metric);
      memcpy (t, &metric, sizeof metric);
      t += sizeof metric;
    }
  size = htons (t - cmdopt);
  memcpy (cmdopt, &size, 2);

  return cmdopt;
}


/* Check wether there is data from zebra aviable */
void zebra_parse (void* foo __attribute__((unused))) {
  unsigned char *data, *f;
  unsigned char command;
  uint16_t length;
  ssize_t len;
  struct zebra_route *route;

  if (!(zebra.status & STATUS_CONNECTED)) {
//    zebra_reconnect();
    return;
  }
  data = try_read (&len);
  if (data) {
    f = data;
      OLSR_PRINTF(1,"length zebra_parse %u\n", (unsigned int) len);
    do {
      memcpy (&length, f, sizeof length);
      length = ntohs (length);
      if (!length) // something wired happened
	olsr_exit ("(QUAGGA) Zero message length??? ", EXIT_FAILURE);
      command = f[2];
      switch (command) {
        case ZEBRA_IPV4_ROUTE_ADD:
          route = zebra_parse_route(f);
          ip_prefix_list_add(&olsr_cnf->hna_entries, &route->prefix, route->prefixlen);
          free_ipv4_route (route);
          free (route);
          break;
        case ZEBRA_IPV4_ROUTE_DELETE:
          route = zebra_parse_route(f);
          ip_prefix_list_remove(&olsr_cnf->hna_entries, &route->prefix, route->prefixlen);
          free_ipv4_route (route);
          free (route);
          break;
        default:
          break;
      }
      f += length;
    } while ((f - data) < len);
    free (data);
  }
}


// tries to read a packet from zebra_socket
// if there is something to read - make sure to read whole packages
static unsigned char *try_read (ssize_t *len) {
  unsigned char *buf = NULL;
  ssize_t ret = 0, bsize = 0;
  uint16_t length = 0, l = 0;
  int sockstate;

  *len = 0;

  sockstate = fcntl (zebra.sock, F_GETFL, 0);
  fcntl (zebra.sock, F_SETFL, sockstate|O_NONBLOCK);

  do {
    if (*len == bsize) {
      bsize += BUFSIZE;
      buf = my_realloc (buf, bsize, "Zebra try_read");
    }
    ret = read (zebra.sock, buf + *len, bsize - *len);
    if (!ret) { // nothing more to read, packet is broken, discard!
      free (buf);
      return NULL;
    }

    if (ret < 0) {
      if (errno != EAGAIN) { // oops - we got disconnected
        OLSR_PRINTF (1, "(QUAGGA) Disconnected from zebra\n");
        zebra.status &= ~STATUS_CONNECTED;
      }
      free (buf);
      return NULL;
    }

    *len += ret;
    do {
      memcpy (&length, buf + l, 2);
      length = ntohs (length);
      l += length;
    } while (*len > l);
    if (*len < l)
      fcntl (zebra.sock, F_SETFL, sockstate);
  } while (*len != l); // GOT FULL PACKAGE!!

  fcntl (zebra.sock, F_SETFL, sockstate);
  return buf;
}


/* Parse an ipv4-route-packet recived from zebra
 */
static struct zebra_route *zebra_parse_route (unsigned char *opt) {
  
  struct zebra_route *r;
  int c;
  size_t size;
  uint16_t length;
  unsigned char *pnt;
      
  memcpy (&length, opt, sizeof length);
  length = ntohs (length);
  
  r = olsr_malloc (sizeof *r , "zebra_parse_route");
  pnt = &opt[3];
  r->type = *pnt++;
  r->flags = *pnt++;
  r->message = *pnt++;
  r->prefixlen = *pnt++;
  r->prefix.v4.s_addr = 0;

  size = (r->prefixlen + 7) / 8;
  memcpy (&r->prefix.v4.s_addr, pnt, size);
  pnt += size;

  if (r->message & ZAPI_MESSAGE_NEXTHOP) {
    r->nexthop_num = *pnt++;
    r->nexthop = olsr_malloc ((sizeof *r->nexthop) * r->nexthop_num,
        "quagga: zebra_parse_route");
    for (c = 0; c < r->nexthop_num; c++) {
      memcpy (&r->nexthop[c].v4.s_addr, pnt, sizeof r->nexthop[c].v4.s_addr);
      pnt += sizeof r->nexthop[c].v4.s_addr;
    }
  }

  if (r->message & ZAPI_MESSAGE_IFINDEX) {
    r->ifindex_num = *pnt++;
    r->ifindex = olsr_malloc (sizeof (uint32_t) * r->ifindex_num,
                            "quagga: zebra_parse_route");
    for (c = 0; c < r->ifindex_num; c++) {
      memcpy (&r->ifindex[c], pnt, sizeof r->ifindex[c]);
      r->ifindex[c] = ntohl (r->ifindex[c]);
      pnt += sizeof r->ifindex[c];
    }
  }

  if (r->message & ZAPI_MESSAGE_DISTANCE) {
    r->distance = *pnt++;
  }

// Quagga v0.98.6 BUG workaround: metric is always sent by zebra
// even without ZAPI_MESSAGE_METRIC message.
//  if (r.message & ZAPI_MESSAGE_METRIC) {
    memcpy (&r->metric, pnt, sizeof (uint32_t));
    r->metric = ntohl (r->metric);
      pnt += sizeof r->metric;
//  }
    
OLSR_PRINTF(1, "%u \n", (unsigned int) (pnt-opt));
    
    if (pnt - opt != length) { olsr_exit ("(QUAGGA) length does not match ??? ", EXIT_FAILURE);
     }

  return r;
}


static unsigned char *zebra_redistribute_packet (unsigned char cmd, unsigned char type) {
  unsigned char *data, *pnt;
  uint16_t size;

  data = olsr_malloc (ZEBRA_MAX_PACKET_SIZ , "zebra_redistribute_packet");

  pnt = &data[2];
  *pnt++ = cmd;
  *pnt++ = type;
  size = htons (pnt - data);
  memcpy (data, &size, 2);

  return data;
}


/* start redistribution FROM zebra */
int zebra_redistribute (unsigned char type) {

      if (zebra_send_command(zebra_redistribute_packet (ZEBRA_REDISTRIBUTE_ADD, type)) < 0)
        olsr_exit("(QUAGGA) could not send redistribute add command", EXIT_FAILURE);

  if (type > ZEBRA_ROUTE_MAX-1) return -1;
  zebra.redistribute[type] = 1;

  return 0;

}


/* end redistribution FROM zebra */
int zebra_disable_redistribute (unsigned char type) {

      if (zebra_send_command(zebra_redistribute_packet (ZEBRA_REDISTRIBUTE_DELETE, type)) < 0)
        olsr_exit("(QUAGGA) could not send redistribute delete command", EXIT_FAILURE);

  if (type > ZEBRA_ROUTE_MAX-1) return -1;
  zebra.redistribute[type] = 0;

  return 0;

}


static void free_ipv4_route (struct zebra_route *r) {

  if(r->ifindex_num) free(r->ifindex);
  if(r->nexthop_num) free(r->nexthop);

}


int zebra_add_route (const struct rt_entry *r) {

  struct zebra_route route;
  int retval;

  route.type = ZEBRA_ROUTE_OLSR;
  route.flags = zebra.flags;
  route.message = ZAPI_MESSAGE_NEXTHOP | ZAPI_MESSAGE_METRIC;
  route.prefixlen = r->rt_dst.prefix_len;
  route.prefix.v4.s_addr = r->rt_dst.prefix.v4.s_addr;
  route.ifindex_num = 0;
  route.ifindex = NULL;
  route.nexthop_num = 0;
  route.nexthop = NULL;

  if (r->rt_best->rtp_nexthop.gateway.v4.s_addr == r->rt_dst.prefix.v4.s_addr &&
       route.prefixlen == 32) {
    return 0;			/* Quagga BUG workaround: don't add routes with destination = gateway
				   see http://lists.olsr.org/pipermail/olsr-users/2006-June/001726.html */
    route.ifindex_num++;
    route.ifindex = olsr_malloc (sizeof *route.ifindex,
			       "zebra_add_route");
    *route.ifindex = r->rt_best->rtp_nexthop.interface->if_index;
  }
  else {
    route.nexthop_num++;
    route.nexthop = olsr_malloc (sizeof *route.nexthop, "zebra_add_route");
    route.nexthop->v4.s_addr = r->rt_best->rtp_nexthop.gateway.v4.s_addr;
  }

  route.metric = r->rt_best->rtp_metric.hops;

  if (zebra.distance) {
    route.message |= ZAPI_MESSAGE_DISTANCE;
    route.distance = zebra.distance;
  }

  retval = zebra_send_command (zebra_route_packet (ZEBRA_IPV4_ROUTE_ADD, &route));
  return retval;

}

int zebra_del_route (const struct rt_entry *r) {

  struct zebra_route route;
  int retval;

  route.type = ZEBRA_ROUTE_OLSR;
  route.flags = zebra.flags;
  route.message = ZAPI_MESSAGE_NEXTHOP | ZAPI_MESSAGE_METRIC;
  route.prefixlen = r->rt_dst.prefix_len;
  route.prefix.v4.s_addr = r->rt_dst.prefix.v4.s_addr;
  route.ifindex_num = 0;
  route.ifindex = NULL;
  route.nexthop_num = 0;
  route.nexthop = NULL;

  if (r->rt_nexthop.gateway.v4.s_addr == r->rt_dst.prefix.v4.s_addr &&
       route.prefixlen == 32){
    return 0;			/* Quagga BUG workaround: don't delete routes with destination = gateway
				   see http://lists.olsr.org/pipermail/olsr-users/2006-June/001726.html */
    route.ifindex_num++;
    route.ifindex = olsr_malloc (sizeof *route.ifindex,
			       "zebra_del_route");
    *route.ifindex = r->rt_nexthop.interface->if_index;
  }
  else {
    route.nexthop_num++;
    route.nexthop = olsr_malloc (sizeof *route.nexthop, "zebra_del_route");
    route.nexthop->v4.s_addr = r->rt_nexthop.gateway.v4.s_addr;
  }

  route.metric = 0;

  if (zebra.distance) {
    route.message |= ZAPI_MESSAGE_DISTANCE;
    route.distance = zebra.distance;
  }


  retval = zebra_send_command (zebra_route_packet (ZEBRA_IPV4_ROUTE_DELETE, &route));
  return retval;

}

void zebra_olsr_distance (unsigned char dist) {
  zebra.distance = dist;
}

void zebra_olsr_localpref (void) {
  zebra.flags &= ZEBRA_FLAG_SELECTED;
}

void zebra_export_routes (unsigned char t) {
  if (t)
    zebra.options |= OPTION_EXPORT;
  else
    zebra.options &= ~OPTION_EXPORT;
}

/*
 * Local Variables:
 * c-basic-offset: 2
 * indent-tabs-mode: nil
 * End:
 */
