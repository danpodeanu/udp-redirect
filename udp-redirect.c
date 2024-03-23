/**
 * @file udp-redirect.c
 * @author Dan Podeanu <pdan@esync.org>
 * @version 1.0
 *
 * @section LICENSE
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * @section DESCRIPTION
 *
 * A simple and high performance UDP redirector.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <math.h>
#include <netdb.h>
#include <time.h>

#include "include/debug.h"
#include "include/statistics.h"
#include "include/settings.h"
#include "include/network.h"

/**
 * The size of the network buffer used for receiving / sending packets
 */
#define NETWORK_BUFFER_SIZE    65535

/**
 * @brief Readability: errno value for OK.
 */
#define EOK 0

/**
  * Maximum known errno, used to ignore harmless sendto / recvfrom errors
  */
#define MAX_ERRNO 256

/**
  * Initialize errno ignore set
  */
#define ERRNO_IGNORE_INIT(X)   memset((X), 0, MAX_ERRNO * sizeof(unsigned char))

/**
  * Set errno ignore boolean for a specific errno
  */
#define ERRNO_IGNORE_SET(X, Y) if ((Y) >= 0 && (Y) < MAX_ERRNO) (X)[(Y)] = 1

/**
  * Check if errno is in declared set
  */
#define ERRNO_IGNORE_CHECK(X, Y) ((Y) >= 0 && (Y) < MAX_ERRNO && (X)[(Y)] == 1)

/**
 * Command line options.
 */
static struct option longopts[] = {
    { "verbose",               no_argument,            NULL,           'v' }, ///< Verbose mode, can be specified multiple times (optional)
    { "debug",                 no_argument,            NULL,           'd' }, ///< Debug mode (optional)

    { "listen-address",        required_argument,      NULL,           'a' }, ///< Listen address (optional)
    { "listen-port",           required_argument,      NULL,           'b' }, ///< Listen port (required)
    { "listen-interface",      required_argument,      NULL,           'c' }, ///< Listen interface (optional)

    { "connect-address",       required_argument,      NULL,           'g' }, ///< Connect address (required)
    { "connect-host",          required_argument,      NULL,           'h' }, ///< Connect host (required)
    { "connect-port",          required_argument,      NULL,           'i' }, ///< Connect port (required)

    { "send-address",          required_argument,      NULL,           'm' }, ///< Send packets address (optional)
    { "send-port",             required_argument,      NULL,           'n' }, ///< Send packets port (optional)
    { "send-interface",        required_argument,      NULL,           'o' }, ///< Send packets interface (optional)

    { "listen-address-strict", no_argument,            NULL,           'x' }, ///< Listener only receives packets from the same endpoint
    { "connect-address-strict",no_argument,            NULL,           'y' }, ///< Sender only receives packets from the connect address

    { "listen-sender-address", required_argument,      NULL,           'k' }, ///< Connect expects packets from this source address
    { "listen-sender-port",    required_argument,      NULL,           'l' }, ///< Connect expects packets from this source port

    { "ignore-errors",         no_argument,            NULL,           'r' }, ///< Ignore harmless recvfrom / sendto errors (default)
    { "stop-errors",           no_argument,            NULL,           's' }, ///< Do NOT ignore harmless recvfrom / sendto errors

    { "stats",                 no_argument,            NULL,           'q' }, ///< Display stats every 60 seconds

    { NULL,                    0,                      NULL,            0 }
};

/**
 * Main program function.
 *
 * @param[in] argc The count of arguments, including the program name.
 * @param[in] argv The program arguments
 * @return The program return code.
 *
 */
int main(int argc, char *argv[]) {
    /* Store debug level and program name */
    int debug_level = DEBUG_LEVEL_ERROR;
    char *argv0 = argv[0];

    int ch; /* Used by getopt_long */

    /* Command line arguments */
    struct settings s;

    struct statistics st;

    time_t now;

    int lsock; /* Listen socket */
    int ssock; /* Send socket */

    struct sockaddr_in lsock_name; /* Listen socket name */
    struct sockaddr_in ssock_name; /* Send socket name */

    struct sockaddr_in caddr; /* Connect address */

    /* Simplify inet_ntop usage in DEBUG() by reserving buffers to write output */
    char print_buffer1[INET_ADDRSTRLEN];
    char print_buffer2[INET_ADDRSTRLEN];

    char network_buffer[NETWORK_BUFFER_SIZE]; /* The network buffer. All reads and writes happen here */

    struct pollfd ufds[2]; /* Poll file descriptors */

    struct sockaddr_in endpoint; /* Address where the current packet was received from */
    struct sockaddr_in previous_endpoint; /* Address where the previous packet was received from */

    unsigned char errno_ignore[MAX_ERRNO];

    settings_initialize(&s);
    statistics_initialize(&st);

    while ((ch = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (ch) {
            case 'v': /* --verbose */
                if (debug_level < DEBUG_LEVEL_VERBOSE) {
                    debug_level = DEBUG_LEVEL_VERBOSE;
                } else {
                    debug_level = debug_level + 1;
                }

                break;
            case 'd': /* --debug */
                debug_level = DEBUG_LEVEL_DEBUG;

                break;
            case 'a': /* --listen-address */
                s.laddr = optarg;

                break;
            case 'b': /* --listen-port */
                s.lport = atoi(optarg);
                if (errno != EOK) {
                    perror("atoi");
                    DEBUG(DEBUG_LEVEL_ERROR, "Invalid listen port: %s (%d)", optarg, errno);

                    exit(EXIT_FAILURE);
                }

                break;
            case 'c': /* --listeninterface */
                s.lif = optarg;

                break;
            case 'g': /* --connect-address */
                s.caddr = optarg;

                break;
            case 'h': /* --connect-host */
                s.chost = optarg;

                break;
            case 'i': /* --connect-port */
                s.cport = atoi(optarg);
                if (errno != EOK) {
                    perror("atoi");
                    DEBUG(DEBUG_LEVEL_ERROR, "Invalid connect port: %s (%d)", optarg, errno);

                    exit(EXIT_FAILURE);
                }

                break;
            case 'm': /* --send-address */
                s.saddr = optarg;

                break;
            case 'n': /* --send-port */
                s.sport = atoi(optarg);
                if (errno != EOK) {
                    perror("atoi");
                    DEBUG(DEBUG_LEVEL_ERROR, "Invalid send port: %s (%d)", optarg, errno);

                    exit(EXIT_FAILURE);
                }

                break;
            case 'o': /* --send-interface */
                s.sif = optarg;

                break;
            case 'x': /* --listen-address-strict */
                s.lstrict = 1;

                break;
            case 'y': /* --connect-address-strict */
                s.cstrict = 1;

                break;
            case 'k': /* --listen-sender-address */
                s.lsaddr = optarg;

                break;
            case 'l': /* --listen-sender-port */
                s.lsport = atoi(optarg);
                if (errno != EOK) {
                    perror("atoi");
                    DEBUG(DEBUG_LEVEL_ERROR, "Invalid send port: %s (%d)", optarg, errno);

                    exit(EXIT_FAILURE);
                }

                break;
            case 'r': /* --ignore-errors */
                s.eignore = 1;

                break;
            case 's': /* --stop-errors */
                s.eignore = 0;

                break;
            case 'q': /* --stats */
                s.stats = 1;

                break;
            default:
                usage(argv0, NULL);
                break;
        }
    }
    argc -= optind;
    argv += optind;

    if (argc != 0) {
        usage(argv0, "Unknown argument");
    }

    if (s.lport == 0) {
        usage(argv0, "Listen port not specified");
    }

    if (s.caddr == NULL && s.chost == NULL) {
        usage(argv0, "Connect host or address not specified");
    }

    if (s.cport == 0) {
        usage(argv0, "Connect port not specified");
    }

    if ((s.lsaddr != NULL && s.lsport == 0) ||
            (s.lsaddr == NULL && s.lsport != 0)) {
        usage(argv0, "Options lsport and csport must either both be specified or none");
    }

    /* Set strict mode if using lsport and csport */
    if (s.lsaddr != NULL && s.lsport != 0) {
        s.lstrict = 1;
    }

    /* Resolve connect host if available */
    if (s.chost != NULL) {
        s.caddr = resolve_host(debug_level, s.chost);
    }

    DEBUG(DEBUG_LEVEL_INFO, "---- INFO ----");

//    DEBUG(DEBUG_LEVEL_ERROR, "Debug level: %d", debug_level);

    DEBUG(DEBUG_LEVEL_INFO, "Listen address: %s", (s.laddr != NULL)?s.laddr:"ANY");
    DEBUG(DEBUG_LEVEL_INFO, "Listen port: %d", s.lport);
    DEBUG(DEBUG_LEVEL_INFO, "Listen interface: %s", (s.lif != NULL)?s.lif:"ANY");

    if (s.chost != NULL) {
        DEBUG(DEBUG_LEVEL_INFO, "Connect host: %s", s.chost);
    }

    if (s.caddr != NULL) {
        DEBUG(DEBUG_LEVEL_INFO, "Connect address: %s", s.caddr);
    }

    DEBUG(DEBUG_LEVEL_INFO, "Connect port: %d", s.cport);

    DEBUG(DEBUG_LEVEL_INFO, "Send address: %s", (s.saddr != NULL)?s.saddr:"ANY");
    if (s.sport != 0) {
        DEBUG(DEBUG_LEVEL_INFO, "Send port: %d", s.sport);
    } else {
        DEBUG(DEBUG_LEVEL_INFO, "Send port: %s", "ANY");
    }
    DEBUG(DEBUG_LEVEL_INFO, "Send interface: %s", (s.sif != NULL)?s.sif:"ANY");

    DEBUG(DEBUG_LEVEL_INFO, "Listen strict: %s", s.cstrict?"ENABLED":"DISABLED");
    DEBUG(DEBUG_LEVEL_INFO, "Connect strict: %s", s.lstrict?"ENABLED":"DISABLED");

    if (s.lsaddr != NULL) {
        DEBUG(DEBUG_LEVEL_INFO, "Listen only accepts packets from address: %s", s.lsaddr);
    }
    if (s.lsport != 0) {
        DEBUG(DEBUG_LEVEL_INFO, "Listen only accepts packets from port: %d", s.lsport);
    }

    DEBUG(DEBUG_LEVEL_INFO, "Ignore errors: %s", s.eignore?"ENABLED":"DISABLED");
    DEBUG(DEBUG_LEVEL_INFO, "Display stats: %s", s.stats?"ENABLED":"DISABLED");

    DEBUG(DEBUG_LEVEL_INFO, "---- START ----");

    lsock = socket_setup(debug_level, "Listen", s.laddr, s.lport, s.lif, &lsock_name); /* Set up listening socket */
    ssock = socket_setup(debug_level, "Send", s.saddr, s.sport, s.sif, &ssock_name); /* Set up send socket */

    /* Set up connect address */
    caddr.sin_family = AF_INET;
    if ((caddr.sin_addr.s_addr = inet_addr(s.caddr)) == INADDR_NONE) {
        perror("inet_addr");
        DEBUG(DEBUG_LEVEL_ERROR, "Invalid connect address %s (%d)", s.caddr, errno);

        exit(EXIT_FAILURE);
    }
    caddr.sin_port = htons(s.cport);

    endpoint.sin_addr.s_addr = 0; /* No packet received, no endpoint */

    previous_endpoint.sin_family = AF_INET;
    if (s.lsaddr == NULL && s.lsport == 0) {
        previous_endpoint.sin_addr.s_addr = 0; /* No packet received, no previous endpoint */
    } else {
        if ((previous_endpoint.sin_addr.s_addr = inet_addr(s.lsaddr)) == INADDR_NONE) {
            perror("inet_addr");
            DEBUG(DEBUG_LEVEL_ERROR, "Invalid listen packet address %s (%d)", s.lsaddr, errno);

            exit(EXIT_FAILURE);
        }
        previous_endpoint.sin_port = htons(s.lsport);
    }

    int endpoint_len = sizeof(endpoint);

    ERRNO_IGNORE_INIT(errno_ignore);
    ERRNO_IGNORE_SET(errno_ignore, EINTR); /* Always ignore EINTR */

    if (s.eignore == 1) { /* List of harmless recvfrom / sendto errors. Possibly incorrect. */
        ERRNO_IGNORE_SET(errno_ignore, EAGAIN);
        ERRNO_IGNORE_SET(errno_ignore, EHOSTUNREACH);
        ERRNO_IGNORE_SET(errno_ignore, ENETDOWN);
        ERRNO_IGNORE_SET(errno_ignore, ENETUNREACH);
        ERRNO_IGNORE_SET(errno_ignore, ENOBUFS);
        ERRNO_IGNORE_SET(errno_ignore, EPIPE);
        ERRNO_IGNORE_SET(errno_ignore, EADDRNOTAVAIL);
    }

    DEBUG(DEBUG_LEVEL_VERBOSE, "entering infinite loop");

    st.time_display_first = time(NULL);

    while (1) {
        int poll_retval;
        int recvfrom_retval;
        int sendto_retval;

        now = time(NULL);

        ufds[0].fd = lsock; ufds[0].events = POLLIN | POLLPRI; ufds[0].revents = 0;
        ufds[1].fd = ssock; ufds[1].events = POLLIN | POLLPRI; ufds[1].revents = 0;

        DEBUG(DEBUG_LEVEL_DEBUG, "waiting for readable sockets");

        if (s.stats && (now - st.time_display_last) > STATISTICS_DELAY_SECONDS) {
            statistics_display(debug_level, &st, now);
            st.time_display_last = now;
        }

        if ((poll_retval = poll(ufds, 2, 1000)) == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("poll");
            DEBUG(DEBUG_LEVEL_ERROR, "Could not check readable sockets (%d)", errno);

            exit(EXIT_FAILURE);
        }
        if (poll_retval == 0) {
            DEBUG(DEBUG_LEVEL_DEBUG, "poll timeout");
            continue;
        }

        /* New data on the LISTEN socket */
        if (ufds[0].revents & POLLIN || ufds[0].revents & POLLPRI) {
            if ((recvfrom_retval = recvfrom(lsock, network_buffer, sizeof(network_buffer), 0,
                            (struct sockaddr *)&endpoint, (socklen_t *)&endpoint_len)) == -1) {
                if (!ERRNO_IGNORE_CHECK(errno_ignore, errno)) {
                    perror("recvfrom");
                    DEBUG(DEBUG_LEVEL_INFO, "Listen cannot receive (%d)", errno);

                    exit(EXIT_FAILURE);
                }
            }
            if (recvfrom_retval > 0) {
                st.count_listen_packet_receive++;
                st.count_listen_byte_receive += recvfrom_retval;

                DEBUG(DEBUG_LEVEL_DEBUG, "RECEIVE (%s, %d) -> (%s, %d) (LISTEN PORT): %d bytes",
                        inet_ntop(AF_INET, &(endpoint.sin_addr), print_buffer1, INET_ADDRSTRLEN), ntohs(endpoint.sin_port),
                        inet_ntop(AF_INET, &(lsock_name.sin_addr), print_buffer2, INET_ADDRSTRLEN), ntohs(lsock_name.sin_port),
                        recvfrom_retval);

                /** Accept the packet IF:
                  * - There's no previous endpoint, OR
                  * - There is a previous endpoint, but we are not in strict mode, OR
                  * - The previous endpoint matches the current endpoint
                */
                if ((previous_endpoint.sin_addr.s_addr == 0 || !s.lstrict) ||
                        (previous_endpoint.sin_addr.s_addr == endpoint.sin_addr.s_addr &&
                         previous_endpoint.sin_port == endpoint.sin_port)) {

                    if (previous_endpoint.sin_addr.s_addr == 0 || !s.lstrict) {
                        if (previous_endpoint.sin_addr.s_addr != endpoint.sin_addr.s_addr ||
                                previous_endpoint.sin_port != endpoint.sin_port) {
                            DEBUG(DEBUG_LEVEL_DEBUG, "LISTEN remote endpoint set to (%s, %d)", inet_ntoa(endpoint.sin_addr), ntohs(endpoint.sin_port));
                        }

                        previous_endpoint.sin_addr.s_addr = endpoint.sin_addr.s_addr;
                        previous_endpoint.sin_port = endpoint.sin_port;
                    }

                    if ((sendto_retval = sendto(ssock, network_buffer, recvfrom_retval, 0,
                                    (struct sockaddr *)&caddr, sizeof(caddr))) == -1) {
                        if (!ERRNO_IGNORE_CHECK(errno_ignore, errno)) {
                            perror("sendto");
                            DEBUG(DEBUG_LEVEL_ERROR, "Cannot send packet to send port (%d)", errno);

                            exit(EXIT_FAILURE);
                        }
                    } else { // At least one byte was sent, record it
                        st.count_connect_packet_send++;
                        st.count_connect_byte_send += sendto_retval;
                    }

                    DEBUG((sendto_retval == recvfrom_retval || s.eignore == 1)?DEBUG_LEVEL_DEBUG:DEBUG_LEVEL_ERROR,
                            "SEND (%s, %d) -> (%s, %d) (SEND PORT): %d bytes (%s WRITE %d bytes)",
                            inet_ntop(AF_INET, &(ssock_name.sin_addr), print_buffer1, INET_ADDRSTRLEN), ntohs(ssock_name.sin_port),
                            inet_ntop(AF_INET, &(caddr.sin_addr), print_buffer2, INET_ADDRSTRLEN), ntohs(caddr.sin_port),
                            sendto_retval,
                            (sendto_retval == recvfrom_retval)?"FULL":"PARTIAL", recvfrom_retval);
                } else {
                    DEBUG(DEBUG_LEVEL_ERROR, "LISTEN PORT invalid source (%s, %d), was expecting (%s, %d)",
                            inet_ntop(AF_INET, &(endpoint.sin_addr), print_buffer1, INET_ADDRSTRLEN), ntohs(endpoint.sin_port),
                            inet_ntop(AF_INET, &(previous_endpoint.sin_addr), print_buffer2, INET_ADDRSTRLEN), ntohs(previous_endpoint.sin_port));
                }
            }
        }

        /* New data on the SEND socket */
        if (ufds[1].revents & POLLIN || ufds[1].revents & POLLPRI) {
            if ((recvfrom_retval = recvfrom(ssock, network_buffer, sizeof(network_buffer), 0,
                            (struct sockaddr *)&endpoint, (socklen_t *)&endpoint_len)) == -1) {
                if (!ERRNO_IGNORE_CHECK(errno_ignore, errno)) {
                    perror("recvfrom");
                    DEBUG(DEBUG_LEVEL_INFO, "Send cannot receive packet (%d)", errno);

                    exit(EXIT_FAILURE);
                }
            }
            if (recvfrom_retval > 0) {
                st.count_connect_packet_receive++;
                st.count_connect_byte_receive += recvfrom_retval;

                DEBUG(DEBUG_LEVEL_DEBUG, "RECEIVE (%s, %d) -> (%s, %d) (SEND PORT): %d bytes",
                        inet_ntop(AF_INET, &(endpoint.sin_addr), print_buffer1, INET_ADDRSTRLEN), ntohs(endpoint.sin_port),
                        inet_ntop(AF_INET, &(ssock_name.sin_addr), print_buffer2, INET_ADDRSTRLEN), ntohs(ssock_name.sin_port),
                        recvfrom_retval);

                /** Accept the packet IF:
                  * - The listen socket has received a packet, so we know the endpoint, AND
                  * - The packet was received from the connect endpoint, OR
                  * - We are not in strict mode
                  */
                if (previous_endpoint.sin_addr.s_addr != 0 &&
                        (!s.cstrict || (caddr.sin_addr.s_addr == endpoint.sin_addr.s_addr && caddr.sin_port == endpoint.sin_port))) {

                    if ((sendto_retval = sendto(lsock, network_buffer, recvfrom_retval, 0,
                                    (struct sockaddr *)&previous_endpoint, sizeof(previous_endpoint))) == -1) {
                        if (!ERRNO_IGNORE_CHECK(errno_ignore, errno)) {
                            perror("sendto");
                            DEBUG(DEBUG_LEVEL_INFO, "Cannot send packet to listen port (%d)", errno);

                            exit(EXIT_FAILURE);
                        }
                    } else { // At least one byte was sent, record it
                        st.count_listen_packet_send++;
                        st.count_listen_byte_send += sendto_retval;
                    }

                    DEBUG((sendto_retval == recvfrom_retval || s.eignore == 1)?DEBUG_LEVEL_DEBUG:DEBUG_LEVEL_ERROR,
                            "SEND (%s, %d) -> (%s, %d) (LISTEN PORT): %d bytes (%s WRITE %d bytes)",
                            inet_ntop(AF_INET, &(lsock_name.sin_addr), print_buffer2, INET_ADDRSTRLEN), ntohs(lsock_name.sin_port),
                            inet_ntop(AF_INET, &(previous_endpoint.sin_addr), print_buffer2, INET_ADDRSTRLEN), ntohs(previous_endpoint.sin_port),
                            sendto_retval,
                            (sendto_retval == recvfrom_retval)?"FULL":"PARTIAL", recvfrom_retval);
                } else {
                    DEBUG(DEBUG_LEVEL_ERROR, "SEND PORT invalid source (%s, %d), was expecting (%s, %d)",
                            inet_ntop(AF_INET, &(endpoint.sin_addr), print_buffer1, INET_ADDRSTRLEN), ntohs(endpoint.sin_port),
                            inet_ntop(AF_INET, &(caddr.sin_addr), print_buffer2, INET_ADDRSTRLEN), ntohs(caddr.sin_port));
                }
            }
        }
    }

    /* Never reached. */
    return 0;
}
