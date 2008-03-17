/*
Copyright (c) 2007, 2008 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <arpa/inet.h>

#include "babel.h"
#include "util.h"
#include "net.h"
#include "network.h"
#include "source.h"
#include "neighbour.h"
#include "route.h"
#include "xroute.h"
#include "request.h"
#include "message.h"
#include "filter.h"

struct timeval update_flush_timeout = {0, 0};

const unsigned char packet_header[8] = {42, 1};

int parasitic = 0;
int silent_time = 30;
int split_horizon = 1;

unsigned short myseqno = 0;
struct timeval seqno_time = {0, 0};
int seqno_interval = -1;

struct buffered_update {
    unsigned char prefix[16];
    unsigned char plen;
};
struct buffered_update buffered_updates[MAX_BUFFERED_UPDATES];
struct network *update_net = NULL;
int updates = 0;

#define UNICAST_BUFSIZE 1024
int unicast_buffered = 0;
unsigned char *unicast_buffer = NULL;
struct neighbour *unicast_neighbour = NULL;
struct timeval unicast_flush_timeout = {0, 0};

unsigned short
hash_id(const unsigned char *id)
{
    int i;
    unsigned short hash = 0;
    for(i = 0; i < 8; i++)
        hash ^= (id[2 * i] << 8) | id[2 * i + 1];
    return hash;
}

void
parse_packet(const unsigned char *from, struct network *net,
             const unsigned char *packet, int len)
{
    int i, j;
    const unsigned char *message;
    unsigned char type, plen, hop_count;
    unsigned short seqno, metric;
    const unsigned char *address;
    struct neighbour *neigh;
    int have_current_source = 0;
    unsigned char current_source[16];

    if(from[0] != 0xFE || (from[1] & 0xC0) != 0x80) {
        fprintf(stderr, "Received packet from non-local address %s.\n",
                format_address(from));
        return;
    }

    if(packet[0] != 42) {
        fprintf(stderr, "Received malformed packet on %s from %s.\n",
                net->ifname, format_address(from));
        return;
    }

    if(packet[1] != 1) {
        fprintf(stderr,
                "Received packet with unknown version %d on %s from %s.\n",
                packet[1], net->ifname, format_address(from));
        return;
    }

    if(len % 24 != 8) {
        fprintf(stderr, "Received malformed packet on %s from %s.\n",
                net->ifname, format_address(from));
        return;
    }

    j = 0;
    for(i = 0; i < (len - 8) / 24; i++) {
        message = packet + 8 + 24 * i;
        type = message[0];
        plen = message[1];
        hop_count = message[3];
        seqno = ntohs(*(uint16_t*)(message + 4));
        metric = ntohs(*(uint16_t*)(message + 6));
        address = message + 8;
        if(type == 0) {
            int changed;
            if(memcmp(address, myid, 16) == 0)
                continue;
            debugf("Received hello (%d) on %s from %s (%s).\n",
                   metric, net->ifname,
                   format_address(address),
                   format_address(from));
            net->activity_time = now.tv_sec;
            update_hello_interval(net);
            neigh = add_neighbour(address, from, net);
            if(neigh == NULL)
                continue;
            changed = update_neighbour(neigh, seqno, metric);
            if(changed)
                update_neighbour_metric(neigh);
            if(metric > 0)
                schedule_neighbours_check(metric * 10, 0);
        } else {
            neigh = find_neighbour(from, net);
            if(neigh == NULL)
                continue;
            net->activity_time = now.tv_sec;
            if(type == 1) {
                debugf("Received ihu %d for %s from %s (%s) %d.\n",
                       metric,
                       format_address(address),
                       format_address(neigh->id),
                       format_address(from), seqno);
                if(memcmp(myid, address, 16) == 0) {
                    neigh->txcost = metric;
                    neigh->ihu_time = now;
                    neigh->ihu_interval = seqno;
                    update_neighbour_metric(neigh);
                    if(seqno > 0)
                        schedule_neighbours_check(seqno * 10 * 3, 0);
                }
            } else if(type == 2) {
                debugf("Received request on %s from %s (%s) for %s "
                       "(%d hops).\n",
                       net->ifname,
                       format_address(neigh->id),
                       format_address(from),
                       plen == 0xFF ?
                       "any" :
                       format_prefix(address, plen),
                       hop_count);
                if(plen == 0xFF) {
                    /* If a neighbour is requesting a full route dump from us,
                       we might as well send it an IHU. */
                    send_ihu(neigh, NULL);
                    send_update(neigh->network, 0, NULL, 0);
                } else {
                    handle_request(neigh, address, plen,
                                   hop_count, seqno, metric);
                }
            } else if(type == 3) {
                if(plen == 0xFF)
                    debugf("Received update for %s/none on %s from %s (%s).\n",
                           format_address(address),
                           net->ifname,
                           format_address(neigh->id),
                           format_address(from));
                else
                    debugf("Received update for %s on %s from %s (%s).\n",
                           format_prefix(address, plen),
                           net->ifname,
                           format_address(neigh->id),
                           format_address(from));
                memcpy(current_source, address, 16);
                have_current_source = 1;
                if(memcmp(address, myid, 16) == 0)
                    continue;
                if(plen <= 128) {
                    unsigned char prefix[16];
                    mask_prefix(prefix, address, plen);
                    update_route(address, prefix, plen, seqno, metric, neigh,
                                 neigh->address);
                }
            } else if(type == 4) {
                unsigned char prefix[16];
                debugf("Received prefix %s on %s from %s (%s).\n",
                       format_prefix(address, plen),
                       net->ifname,
                       format_address(neigh->id),
                       format_address(from));
                if(!have_current_source) {
                    fprintf(stderr, "Received prefix with no source "
                            "on %s from %s (%s).\n",
                            net->ifname,
                            format_address(neigh->id),
                            format_address(from));
                    continue;
                }
                if(memcmp(current_source, myid, 16) == 0)
                    continue;
                mask_prefix(prefix, address, plen);
                update_route(current_source, prefix, plen, seqno, metric,
                             neigh, neigh->address);
            } else if(type == 5) {
                unsigned char p4[16], prefix[16], nh[16];
                if(!net->ipv4)
                    continue;
                v4tov6(p4, message + 20);
                v4tov6(nh, message + 16);
                debugf("Received update for %s nh %s on %s from %s (%s).\n",
                       format_prefix(p4, plen + 96),
                       format_address(nh),
                       net->ifname,
                       format_address(neigh->id),
                       format_address(from));
                if(plen > 32)
                    continue;
                if(!have_current_source)
                    continue;
                if(memcmp(current_source, myid, 16) == 0)
                    continue;
                mask_prefix(prefix, p4, plen + 96);
                update_route(current_source, prefix, plen + 96, seqno, metric,
                             neigh, nh);
            } else {
                debugf("Received unknown packet type %d from %s (%s).\n",
                       type, format_address(neigh->id), format_address(from));
            }
        }
    }
    return;
}

void
handle_request(struct neighbour *neigh, const unsigned char *prefix,
               unsigned char plen, unsigned char hop_count,
               unsigned short seqno, unsigned short router_hash)
{
    struct xroute *xroute;
    struct route *route;
    struct neighbour *successor = NULL;

    xroute = find_xroute(prefix, plen);
    if(xroute) {
        if(hop_count > 0 && router_hash == hash_id(myid)) {
            if(seqno_compare(seqno, myseqno) > 0) {
                if(seqno_minus(seqno, myseqno) > 100) {
                    /* Hopelessly out-of-date request */
                    return;
                }
                update_myseqno(1);
            }
        }
        send_update(neigh->network, 1, prefix, plen);
        return;
    }

    route = find_installed_route(prefix, plen);
    if(route &&
       (hop_count == 0 ||
        (route->metric < INFINITY &&
         (router_hash != hash_id(route->src->id) ||
          seqno_compare(seqno, route->seqno) <= 0)))) {
        /* We can satisfy this request straight away.  Note that in the
           hop_count=0 case, we do send a recent retraction, in order to
           reply to nodes whose routes are about to expire. */
        send_update(neigh->network, 1, prefix, plen);
        return;
    }

    if(hop_count <= 1)
        return;

    if(route && router_hash == hash_id(route->src->id) &&
       seqno_minus(seqno, route->seqno) > 100) {
        /* Hopelessly out-of-date */
        return;
    }

    /* Let's forward this request. */
    if(route && route->metric < INFINITY)
        successor = route->neigh;

    if(!successor || successor == neigh) {
        struct route *other_route;
        /* We're about to forward a request to the requestor.  Try to
           find a different neighbour to forward the request to. */

        other_route = find_best_route(prefix, plen, 0, neigh);
        if(other_route && other_route->metric < INFINITY)
            successor = other_route->neigh;
    }

    if(!successor || successor == neigh)
        /* Give up */
        return;

    send_unicast_request(successor, prefix, plen, hop_count - 1,
                         seqno, router_hash);
    record_request(prefix, plen, seqno, router_hash, neigh->network, 0);
}


/* Under normal circumstances, there are enough moderation mechanisms
   elsewhere in the protocol to make sure that this last-ditch check
   should never trigger.  But I'm supersticious. */

static int
check_bucket(struct network *net)
{
    if(net->bucket > 0 && now.tv_sec > net->bucket_time) {
        net->bucket =
            MAX(0, (int)net->bucket - 40 * (now.tv_sec - net->bucket_time));
    }

    net->bucket_time = now.tv_sec;

    if(net->bucket < 400) {
        net->bucket++;
        return 1;
    } else {
        return 0;
    }
}

void
flushbuf(struct network *net)
{
    int rc;
    struct sockaddr_in6 sin6;

    assert(net->buffered <= net->bufsize);

    if(update_net == net)
        flushupdates();

    if(net->buffered > 0) {
        debugf("  (flushing %d buffered bytes on %s)\n",
               net->buffered, net->ifname);
        if(check_bucket(net)) {
            memset(&sin6, 0, sizeof(sin6));
            sin6.sin6_family = AF_INET6;
            memcpy(&sin6.sin6_addr, protocol_group, 16);
            sin6.sin6_port = htons(protocol_port);
            sin6.sin6_scope_id = net->ifindex;
            rc = babel_send(protocol_socket,
                            packet_header, sizeof(packet_header),
                            net->sendbuf, net->buffered,
                            (struct sockaddr*)&sin6, sizeof(sin6));
            if(rc < 0)
                perror("send");
        } else {
            fprintf(stderr, "Warning: bucket full, dropping packet to %s.\n",
                    net->ifname);
        }
    }
    VALGRIND_MAKE_MEM_UNDEFINED(net->sendbuf, net->bufsize);
    net->buffered = 0;
    net->flush_timeout.tv_sec = 0;
    net->flush_timeout.tv_usec = 0;
}

static void
schedule_flush(struct network *net)
{
    int msecs = jitter(net, 0);
    if(net->flush_timeout.tv_sec != 0 &&
       timeval_minus_msec(&net->flush_timeout, &now) < msecs)
        return;
    net->flush_timeout.tv_usec = (now.tv_usec + msecs * 1000) % 1000000;
    net->flush_timeout.tv_sec =
        now.tv_sec + (now.tv_usec / 1000 + msecs) / 1000;
}

void
schedule_flush_now(struct network *net)
{
    /* Almost now */
    int msecs = 5 + random() % 10;
    if(net->flush_timeout.tv_sec != 0 &&
       timeval_minus_msec(&net->flush_timeout, &now) < msecs)
        return;
    net->flush_timeout.tv_usec = (now.tv_usec + msecs * 1000) % 1000000;
    net->flush_timeout.tv_sec =
        now.tv_sec + (now.tv_usec / 1000 + msecs) / 1000;
}

static void
schedule_unicast_flush(void)
{
    int msecs;

    if(!unicast_neighbour)
        return;
    msecs = jitter(unicast_neighbour->network, 1);
    if(unicast_flush_timeout.tv_sec != 0 &&
       timeval_minus_msec(&unicast_flush_timeout, &now) < msecs)
        return;
    unicast_flush_timeout.tv_usec = (now.tv_usec + msecs * 1000) % 1000000;
    unicast_flush_timeout.tv_sec =
        now.tv_sec + (now.tv_usec / 1000 + msecs) / 1000;
}

static void
start_message(struct network *net, int bytes)
{
    assert(net->buffered % 24 == 0);
    if(net->bufsize - net->buffered < bytes)
        flushbuf(net);
}

static void
send_message(struct network *net,
             unsigned char type,  unsigned char plen, unsigned char hop_count,
             unsigned short seqno, unsigned short metric,
             const unsigned char *address)
{
    unsigned char *buf;
    int n;

    if(!net->up)
        return;

    start_message(net, 24);
    buf = net->sendbuf;
    n = net->buffered;

    buf[n++] = type;
    buf[n++] = plen;
    buf[n++] = 0;
    buf[n++] = hop_count;
    buf[n++] = (seqno >> 8) & 0xFF;
    buf[n++] = seqno & 0xFF;
    buf[n++] = (metric >> 8) & 0xFF;
    buf[n++] = metric & 0xFF;
    memcpy(buf + n, address, 16);
    n += 16;
    net->buffered = n;

    schedule_flush(net);
}

/* Flush buffers if they contain any hellos.  This avoids sending multiple
   hellos in a single packet, which breaks link quality estimation. */
int
flush_hellos(struct network *net)
{
    int i;
    assert(net->buffered % 24 == 0);

    for(i = 0; i < net->buffered / 24; i++) {
        const unsigned char *message;
        message = (const unsigned char*)(net->sendbuf + i * 24);
        if(message[0] == 0) {
            flushbuf(net);
            return 1;
        }
    }
    return 0;
}

void
send_hello_noupdate(struct network *net, unsigned interval)
{
    debugf("Sending hello (%d) to %s.\n", interval, net->ifname);
    net->hello_seqno = seqno_plus(net->hello_seqno, 1);
    flush_hellos(net);
    delay_jitter(&net->hello_time, &net->hello_timeout,
                 net->hello_interval);
    send_message(net, 0, 0, 0, net->hello_seqno,
                 interval > 0xFFFF ? 0 : interval,
                 myid);
}

void
send_hello(struct network *net)
{
    int changed;
    changed = update_hello_interval(net);
    send_hello_noupdate(net, (net->hello_interval + 9) / 10);
    /* Send full IHU every 3 hellos, and marginal IHU each time */
    if(changed || net->hello_seqno % 3 == 0)
        send_ihu(NULL, net);
    else
        send_marginal_ihu(net);
}

void
send_request(struct network *net,
             const unsigned char *prefix, unsigned char plen,
             unsigned char hop_count, unsigned short seqno,
             unsigned short router_hash)
{
    int i;

    if(net == NULL) {
        for(i = 0; i < numnets; i++) {
            if(!nets[i].up)
                continue;
            send_request(&nets[i], prefix, plen, hop_count, seqno, router_hash);
        }
        return;
    }

    /* Make sure any buffered updates go out before this request. */
    if(!net || update_net == net)
        flushupdates();

    debugf("Sending request to %s for %s (%d hops).\n",
           net->ifname, prefix ? format_prefix(prefix, plen) : "any",
           hop_count);
    if(prefix)
        send_message(net, 2, plen, hop_count, seqno, router_hash, prefix);
    else
        send_message(net, 2, 0xFF, 0, 0, 0, ones);
}

void
send_request_resend(struct neighbour *neigh,
                    const unsigned char *prefix, unsigned char plen,
                    unsigned short seqno, unsigned short router_hash)
{
    int time;

    if(neigh)
        send_unicast_request(neigh, prefix, plen, 127, seqno, router_hash);
    else
        send_request(NULL, prefix, plen, 127, seqno, router_hash);

    time = 2000;
    time = MIN(time, wireless_hello_interval / 2);
    time = MIN(time, wired_hello_interval / 2);
    time = MAX(time, 10);
    record_request(prefix, plen, seqno, router_hash,
                   neigh ? neigh->network : NULL, time);

}

void
flush_unicast(int dofree)
{
    struct sockaddr_in6 sin6;
    int rc;

    if(unicast_buffered == 0)
        goto done;

    if(!unicast_neighbour->network->up)
        goto done;

    /* Preserve ordering of messages */
    flushbuf(unicast_neighbour->network);

    if(check_bucket(unicast_neighbour->network)) {
        memset(&sin6, 0, sizeof(sin6));
        sin6.sin6_family = AF_INET6;
        memcpy(&sin6.sin6_addr, unicast_neighbour->address, 16);
        sin6.sin6_port = htons(protocol_port);
        sin6.sin6_scope_id = unicast_neighbour->network->ifindex;
        rc = babel_send(protocol_socket,
                        packet_header, sizeof(packet_header),
                        unicast_buffer, unicast_buffered,
                        (struct sockaddr*)&sin6, sizeof(sin6));
        if(rc < 0)
            perror("send(unicast)");
    } else {
        fprintf(stderr,
                "Warning: bucket full, dropping unicast packet"
                "to %s (%s) if %s.\n",
                format_address(unicast_neighbour->id),
                format_address(unicast_neighbour->address),
                unicast_neighbour->network->ifname);
    }

 done:
    VALGRIND_MAKE_MEM_UNDEFINED(unicast_buffer, UNICAST_BUFSIZE);
    unicast_buffered = 0;
    if(dofree && unicast_buffer) {
        free(unicast_buffer);
        unicast_buffer = NULL;
    }
    unicast_neighbour = NULL;
    unicast_flush_timeout.tv_sec = 0;
    unicast_flush_timeout.tv_usec = 0;
}

static void
send_unicast_message(struct neighbour *neigh,
                     unsigned char type,
                     unsigned char plen, unsigned char hop_count,
                     unsigned short seqno, unsigned short metric,
                     const unsigned char *address)
{
    if(unicast_neighbour) {
        if(neigh != unicast_neighbour ||
           unicast_buffered + 24 >=
           MIN(UNICAST_BUFSIZE, neigh->network->bufsize))
        flush_unicast(0);
    }

    if(!unicast_buffer)
        unicast_buffer = malloc(UNICAST_BUFSIZE);
    if(!unicast_buffer) {
        perror("malloc(unicast_buffer)");
        return;
    }

    unicast_neighbour = neigh;

    assert(unicast_buffered % 24 == 0);

    unicast_buffer[unicast_buffered++] = type;
    unicast_buffer[unicast_buffered++] = plen;
    unicast_buffer[unicast_buffered++] = 0;
    unicast_buffer[unicast_buffered++] = hop_count;
    unicast_buffer[unicast_buffered++] = (seqno >> 8) & 0xFF;
    unicast_buffer[unicast_buffered++] = seqno & 0xFF;
    unicast_buffer[unicast_buffered++] = (metric >> 8) & 0xFF;
    unicast_buffer[unicast_buffered++] = metric & 0xFF;
    memcpy(unicast_buffer + unicast_buffered, address, 16);
    unicast_buffered += 16;

    schedule_unicast_flush();
}

void
send_unicast_request(struct neighbour *neigh,
                     const unsigned char *prefix, unsigned char plen,
                     unsigned char hop_count, unsigned short seqno,
                     unsigned short router_hash)
{
    /* Make sure any buffered updates go out before this request. */
    if(update_net == neigh->network)
        flushupdates();

    debugf("Sending unicast request to %s (%s) for %s (%d hops).\n",
           format_address(neigh->id),
           format_address(neigh->address),
           prefix ? format_prefix(prefix, plen) : "any",
           hop_count);

    if(prefix)
        send_unicast_message(neigh,
                             2, plen, hop_count, seqno, router_hash, prefix);
    else
        send_unicast_message(neigh, 2, 0xFF, 0, 0, 0, ones);
}

/* Return the source-id of the last buffered update message. */
static const unsigned char *
message_source_id(struct network *net)
{
    int i;
    assert(net->buffered % 24 == 0);

    i = net->buffered / 24 - 1;
    while(i >= 0) {
        const unsigned char *message;
        message = (const unsigned char*)(net->sendbuf + i * 24);
        if(message[0] == 3)
            return message + 8;
        else if(message[0] == 4)
            i--;
        else
            break;
    }

    return NULL;
}

static void
really_send_update(struct network *net,
                   const unsigned char *id,
                   const unsigned char *prefix, unsigned char plen,
                   unsigned short seqno, unsigned short metric)
{
    int add_metric;

    if(!net->up)
        return;

    add_metric = output_filter(id, prefix, plen, net->ifindex);

    if(add_metric < INFINITY) {
        metric = MIN(metric + add_metric, INFINITY);
        if(plen >= 96 && v4mapped(prefix)) {
            const unsigned char *sid;
            unsigned char v4route[16];
            if(!net->ipv4)
                return;
            memset(v4route, 0, 8);
            memcpy(v4route + 8, net->ipv4, 4);
            memcpy(v4route + 12, prefix + 12, 4);
            start_message(net, 48);
            sid = message_source_id(net);
            if(sid == NULL || memcmp(id, sid, 16) != 0)
                send_message(net, 3, 0xFF, 0, 0, 0xFFFF, id);
            send_message(net, 5, plen - 96, 0, seqno, metric, v4route);
        } else {
            if(in_prefix(id, prefix, plen)) {
                send_message(net, 3, plen, 0, seqno, metric, id);
            } else {
                const unsigned char *sid;
                start_message(net, 48);
                sid = message_source_id(net);
                if(sid == NULL || memcmp(id, sid, 16) != 0)
                    send_message(net, 3, 0xFF, 0, 0, 0xFFFF, id);
                send_message(net, 4, plen, 0, seqno, metric, prefix);
            }
        }
    }
    satisfy_request(prefix, plen, seqno, hash_id(id), net);
}

void
flushupdates(void)
{
    int i;

    if(updates > 0) {
        int n = updates;
        struct network *net = update_net;
        updates = 0;
        update_net = NULL;

        debugf("  (flushing %d buffered updates)\n", n);

        for(i = 0; i < n; i++) {
            struct xroute *xroute;
            struct route *route;
            struct source *src;
            unsigned short seqno;
            unsigned short metric;
            xroute = find_xroute(buffered_updates[i].prefix,
                                 buffered_updates[i].plen);
            if(xroute) {
                really_send_update(net, myid,
                                   xroute->prefix, xroute->plen,
                                   myseqno, xroute->metric);
                continue;
            }
            route = find_installed_route(buffered_updates[i].prefix,
                                         buffered_updates[i].plen);
            if(route) {
                if(split_horizon &&
                   net->wired && route->neigh->network == net)
                    continue;
                seqno = route->seqno;
                metric = route->metric;
                really_send_update(net, route->src->id,
                                   route->src->prefix,
                                   route->src->plen,
                                   seqno, metric);
                update_source(route->src, seqno, metric);
                continue;
            }
            src = find_recent_source(buffered_updates[i].prefix,
                                     buffered_updates[i].plen);
            if(src) {
                really_send_update(net, src->id, src->prefix, src->plen,
                                   src->metric >= INFINITY ?
                                   src->seqno : seqno_plus(src->seqno, 1),
                                   INFINITY);
                continue;
            }
        }
        schedule_flush_now(net);
        VALGRIND_MAKE_MEM_UNDEFINED(&buffered_updates,
                                    sizeof(buffered_updates));
    }
    update_flush_timeout.tv_sec = 0;
    update_flush_timeout.tv_usec = 0;
}

static void
schedule_update_flush(struct network *net, int urgent)
{
    int msecs;
    msecs = update_jitter(net, urgent);
    if(update_flush_timeout.tv_sec != 0 &&
       timeval_minus_msec(&update_flush_timeout, &now) < msecs)
        return;
    update_flush_timeout.tv_usec = (now.tv_usec + msecs * 1000) % 1000000;
    update_flush_timeout.tv_sec =
        now.tv_sec + (now.tv_usec / 1000 + msecs) / 1000;
}

static void
buffer_update(struct network *net,
              const unsigned char *prefix, unsigned char plen)
{
    int i;

    if(update_net && update_net != net)
        flushupdates();

    update_net = net;

    for(i = 0; i < updates; i++) {
        if(buffered_updates[i].plen == plen &&
           memcmp(buffered_updates[i].prefix, prefix, 16) == 0)
            return;
    }

    if(updates >= MAX_BUFFERED_UPDATES)
        flushupdates();
    memcpy(buffered_updates[updates].prefix, prefix, 16);
    buffered_updates[updates].plen = plen;
    updates++;
}

void
send_update(struct network *net, int urgent,
            const unsigned char *prefix, unsigned char plen)
{
    int i, selfonly;
    struct request *request;

    if(prefix) {
        /* This is needed here, since really_send_update only handles the
           case where network is not null. */
        request = find_request(prefix, plen, NULL);
        if(request) {
            struct route *route;
            route = find_installed_route(prefix, plen);
            if(route) {
                urgent = 1;
                satisfy_request(prefix, plen, route->seqno,
                                hash_id(route->src->id), net);
            }
        }
    }

    if(net == NULL) {
        for(i = 0; i < numnets; i++) {
            send_update(&nets[i], urgent, prefix, plen);
        }
        return;
    }

    if(!net->up)
        return;

    selfonly =
        parasitic || (silent_time && now.tv_sec < reboot_time + silent_time);

    if(!selfonly)
        silent_time = 0;

    if(prefix) {
        if(updates > net->bufsize / 24 - 2) {
            /* Update won't fit in current packet */
            flushupdates();
        }
        if(!selfonly || find_xroute(prefix, plen)) {
            debugf("Sending update to %s for %s.\n",
                   net->ifname, format_prefix(prefix, plen));
            buffer_update(net, prefix, plen);
        }
    } else {
        send_self_update(net, 0);
        /* Don't send full route dumps more than ten times per second */
        if(net->update_time.tv_sec > 0 &&
           timeval_minus_msec(&now, &net->update_time) < 100)
            return;
        if(!selfonly) {
            debugf("Sending update to %s for any.\n", net->ifname);
            for(i = 0; i < numroutes; i++)
                if(routes[i].installed)
                    buffer_update(net,
                                  routes[i].src->prefix, routes[i].src->plen);
        }
        delay_jitter(&net->update_time, &net->update_timeout,
                     update_interval);
    }
    schedule_update_flush(net, urgent);
}

void
update_myseqno(int force)
{
    if(force || timeval_minus_msec(&now, &seqno_time) >= seqno_interval) {
        myseqno = seqno_plus(myseqno, 1);
        seqno_time = now;
    }
}

void
send_self_update(struct network *net, int force_seqno)
{
    int i;

    update_myseqno(force_seqno);

    if(net == NULL) {
        for(i = 0; i < numnets; i++) {
            if(!nets[i].up)
                continue;
            send_self_update(&nets[i], 0);
        }
        return;
    }

    debugf("Sending self update to %s.\n", net->ifname);

    delay_jitter(&net->self_update_time, &net->self_update_timeout,
                 net->self_update_interval);
    for(i = 0; i < numxroutes; i++) {
        send_update(net, 0, xroutes[i].prefix, xroutes[i].plen);
    }
}

void
send_ihu(struct neighbour *neigh, struct network *net)
{
    int i;

    if(neigh == NULL && net == NULL) {
        for(i = 0; i < numnets; i++) {
            if(!nets[i].up)
                continue;
            send_ihu(NULL, &nets[i]);
        }
        return;
    }

    if(neigh == NULL) {
        for(i = 0; i < numneighs; i++) {
            if(!neighbour_valid(&neighs[i]))
                continue;
            if(neighs[i].network == net)
                send_ihu(&neighs[i], net);
        }
    } else {
        int rxcost, interval;

        if(net && neigh->network != net)
            return;

        net = neigh->network;

        rxcost = neighbour_rxcost(neigh);

        interval = (net->hello_interval * 3 + 9) / 10;

        debugf("Sending ihu %d on %s to %s (%s).\n",
               rxcost,
               neigh->network->ifname,
               format_address(neigh->id),
               format_address(neigh->address));

        send_message(net, 1, 128, 0, interval < 0xFFFF ? interval : 0,
                     rxcost, neigh->id);
    }
}

/* Send IHUs to all marginal neighbours */
void
send_marginal_ihu(struct network *net)
{
    int i;
    for(i = 0; i < numneighs; i++) {
        if(!neighbour_valid(&neighs[i]) || (net && neighs[i].network != net))
            continue;
        if(neighs[i].txcost >= 384 || (neighs[i].reach & 0xF000) != 0xF000)
            send_ihu(&neighs[i], net);
    }
}
