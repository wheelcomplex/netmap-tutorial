/*
 * This program forwards UDP packets between two netmap ports.
 * Only UDP packets with a destination port specified
 * by command-line option are forwarded, while all the other ones are
 * dropped. If port 0 is specified, all packets are forwarded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <net/if.h>
#include <stdint.h>
#include <net/netmap.h>
#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>

static int stop               = 0;
static unsigned long long fwd = 0;
static unsigned long long tot = 0;

static void
sigint_handler(int signum)
{
    stop = 1;
}

static int
rx_ready(struct nm_desc *nmd)
{
    unsigned int ri;

    for (ri = nmd->first_rx_ring; ri <= nmd->last_rx_ring; ri++) {
        struct netmap_ring *ring;

        ring = NETMAP_RXRING(nmd->nifp, ri);
        if (nm_ring_space(ring)) {
            return 1; /* there is something to read */
        }
    }

    return 0;
}

static inline int
pkt_select(const char *buf, int udp_port)
{
    struct ether_header *ethh;
    struct ip *iph;
    struct udphdr *udph;

    if (udp_port == 0) {
        return 1; /* no filter */
    }

    ethh = (struct ether_header *)buf;
    if (ethh->ether_type != htons(ETHERTYPE_IP)) {
        /* Filter out non-IP traffic. */
        return 0;
    }
    iph = (struct ip *)(ethh + 1);
    if (iph->ip_p != IPPROTO_UDP) {
        /* Filter out non-UDP traffic. */
        return 0;
    }
    udph = (struct udphdr *)(iph + 1);

    /* Match the destination port. */
    if (udph->uh_dport != htons(udp_port)) {
        return 0;
    }

    return 1;
}

#ifdef SOLUTION
static void
forward_pkts(struct nm_desc *src, struct nm_desc *dst, int udp_port,
             int zerocopy)
{
    unsigned int si = src->first_rx_ring;
    unsigned int di = dst->first_tx_ring;

    while (si <= src->last_rx_ring && di <= dst->last_tx_ring) {
        struct netmap_ring *txring;
        struct netmap_ring *rxring;
        unsigned int rxhead, txhead;
        int nrx, ntx;

        rxring = NETMAP_RXRING(src->nifp, si);
        txring = NETMAP_TXRING(dst->nifp, di);
        nrx    = nm_ring_space(rxring);
        ntx    = nm_ring_space(txring);
        if (nrx == 0) {
            si++;
            continue;
        }
        if (ntx == 0) {
            di++;
            continue;
        }

        rxhead = rxring->head;
        txhead = txring->head;
        for (; nrx > 0 && ntx > 0;
             nrx--, rxhead = nm_ring_next(rxring, rxhead), tot++) {
            struct netmap_slot *rs = &rxring->slot[rxhead];
            struct netmap_slot *ts = &txring->slot[txhead];
            char *rxbuf            = NETMAP_BUF(rxring, rs->buf_idx);

            if (!pkt_select(rxbuf, udp_port)) {
                continue; /* discard */
            }

            ts->len = rs->len;
            if (zerocopy) {
                uint32_t idx = ts->buf_idx;
                ts->buf_idx  = rs->buf_idx;
                rs->buf_idx  = idx;
                /* report the buffer change. */
                ts->flags |= NS_BUF_CHANGED;
                rs->flags |= NS_BUF_CHANGED;
            } else {
                char *txbuf = NETMAP_BUF(txring, ts->buf_idx);
                memcpy(txbuf, rxbuf, ts->len);
            }
            txhead = nm_ring_next(txring, txhead);
            ntx--;
            fwd++;
        }
        /* Update state of netmap ring. */
        rxring->head = rxring->cur = rxhead;
        txring->head = txring->cur = txhead;
    }
}
#endif /* SOLUTION */

static int
main_loop(const char *netmap_port_one, const char *netmap_port_two,
          int udp_port)
{
    struct nm_desc *nmd_one;
    struct nm_desc *nmd_two;
    int zerocopy;

    nmd_one = nm_open(netmap_port_one, NULL, 0, NULL);
    if (nmd_one == NULL) {
        if (!errno) {
            printf("Failed to nm_open(%s): not a netmap port\n",
                   netmap_port_one);
        } else {
            printf("Failed to nm_open(%s): %s\n", netmap_port_one,
                   strerror(errno));
        }
        return -1;
    }

    nmd_two = nm_open(netmap_port_two, NULL, NM_OPEN_NO_MMAP, nmd_one);
    if (nmd_two == NULL) {
        if (!errno) {
            printf("Failed to nm_open(%s): not a netmap port\n",
                   netmap_port_two);
        } else {
            printf("Failed to nm_open(%s): %s\n", netmap_port_two,
                   strerror(errno));
        }
        return -1;
    }

    /* Check if we can do zerocopy. */
    zerocopy = (nmd_one->mem == nmd_two->mem);
    printf("zerocopy %sabled\n", zerocopy ? "en" : "dis");

    while (!stop) {
#ifdef SOLUTION
        struct pollfd pfd[2];
        int ret;

        pfd[0].fd     = nmd_one->fd;
        pfd[1].fd     = nmd_two->fd;
        pfd[0].events = 0;
        pfd[1].events = 0;
        if (!rx_ready(nmd_one)) {
            /* Ran out of input packets on the first port, we need to
             * wait for them. */
            pfd[0].events |= POLLIN;
        } else {
            /* We have input packets on the first port, let's wait for
             * TX ring space in the other port. */
            pfd[1].events |= POLLOUT;
        }
        if (!rx_ready(nmd_two)) {
            /* Ran out of input packets on the second port, we need to
             * wait for them. */
            pfd[1].events |= POLLIN;
        } else {
            /* We have input packets on the second port, let's wait for
             * TX ring space in the other port. */
            pfd[0].events |= POLLOUT;
        }

        /* We poll with a timeout to have a chance to break the main loop if
         * no packets are coming. */
        ret = poll(pfd, 2, 1000);
        if (ret < 0) {
            perror("poll()");
        } else if (ret == 0) {
            /* Timeout */
            continue;
        }

        /* Forward in the two directions. */
        forward_pkts(nmd_one, nmd_two, udp_port, zerocopy);
        forward_pkts(nmd_two, nmd_one, udp_port, zerocopy);
#endif /* SOLUTION */
    }

    nm_close(nmd_one);
    nm_close(nmd_two);

    printf("Total processed packets: %llu\n", tot);
    printf("Forwarded packets      : %llu\n", fwd);

    return 0;
}

static void
usage(char **argv)
{
    printf("usage: %s [-h] [-p UDP_PORT] [-i NETMAP_PORT_ONE] "
           "[-i NETMAP_PORT_TWO]\n",
           argv[0]);
    exit(EXIT_SUCCESS);
}

int
main(int argc, char **argv)
{
    const char *netmap_port_one = NULL;
    const char *netmap_port_two = NULL;
    int udp_port                = 0; /* zero means select everything */
    struct sigaction sa;
    int opt;
    int ret;

    while ((opt = getopt(argc, argv, "hi:p:")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv);
            return 0;

        case 'i':
            if (netmap_port_one == NULL) {
                netmap_port_one = optarg;
            } else if (netmap_port_two == NULL) {
                netmap_port_two = optarg;
            }
            break;

        case 'p':
            udp_port = atoi(optarg);
            if (udp_port < 0 || udp_port >= 65535) {
                printf("    invalid UDP port %s\n", optarg);
                usage(argv);
            }
            break;

        default:
            printf("    unrecognized option '-%c'\n", opt);
            usage(argv);
            return -1;
        }
    }

    if (netmap_port_one == NULL) {
        printf("    missing netmap port #1\n");
        usage(argv);
    }

    if (netmap_port_two == NULL) {
        printf("    missing netmap port #2\n");
        usage(argv);
    }

    /* Register Ctrl-C handler. */
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ret         = sigaction(SIGINT, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGINT)");
        exit(EXIT_FAILURE);
    }
    (void)rx_ready;

    printf("Port one: %s\n", netmap_port_one);
    printf("Port two: %s\n", netmap_port_two);
    printf("UDP port: %d\n", udp_port);

    main_loop(netmap_port_one, netmap_port_two, udp_port);

    (void)pkt_select; /* silence the compiler */

    return 0;
}
