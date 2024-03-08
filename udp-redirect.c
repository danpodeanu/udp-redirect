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

/**
 * Standard debug macro requiring a locally defined debug level.
 * Adapted from the excellent https://github.com/jleffler/soq/blob/master/src/libsoq/debug.h
 *
 * Note that __VA_ARGS__ is a GCC extension; used for convenience to support DEBUG without format arguments.
 */
#define DEBUG(lvl, fmt, ...) \
        do { \
            if ((debug_level) >= (lvl)) { \
                fprintf(stderr, "%s:%d:%d:%s(): " fmt "\n", __FILE__, \
                    __LINE__, (int)(time(NULL)), __func__, ##__VA_ARGS__); \
            } \
        } while (0)

/**
 * The size of the network buffer used for receiving / sending packets
 */
#define NETWORK_BUFFER_SIZE    65535

/**
 * The delay in seconds between displaying statistics
 */
#define STATS_DELAY_SECONDS    60

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
  * Hardcoded printf format for a human readable value made of a float and a char
  */
#define HUMAN_READABLE_FORMAT "%.1lf%c"

/**
  * Shortcut for the above, for shorter printf
  */
#define HRF HUMAN_READABLE_FORMAT

/**
  * Hardcoded invocations of int to human readable, compatible with HUMAN_READABLE_FORMAT in printf
  */
#define HUMAN_READABLE(X) int_to_human_value((X)), int_to_human_char((X))

/**
  * The human readable size suffixes
  */
#define HUMAN_READABLE_SIZES { ' ', 'K', 'M', 'G', 'T', 'P', 'E' }

/**
  * The number of human readable size suffixes
  */
#define HUMAN_READABLE_SIZES_COUNT 7

/**
 * @brief The available debug levels.
 */
enum DEBUG_LEVEL {
    DEBUG_LEVEL_ERROR = 0,      ///< Error messages
    DEBUG_LEVEL_INFO = 1,       ///< Informational messages
    DEBUG_LEVEL_VERBOSE = 2,    ///< Verbose messages
    DEBUG_LEVEL_DEBUG = 3       ///< Debug messages
};

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
 * Displays the program usage and exit with error.
 *
 * @param[in] argv0 The program name as started, e.g., /usr/local/bin/udp_redirector
 * @param[in] message The error message, or NULL
 *
 */
void usage(const char *argv0, const char *message) {
    if (message != NULL)
        fprintf(stderr, "%s\n", message);

    fprintf(stderr, "Usage: %s\n", argv0);
    fprintf(stderr, "          [--listen-address <address>] --listen-port <port> [--listen-interface <interface>]\n");
    fprintf(stderr, "          --connect-address <address> | --connect-host <hostname> --connect-port <port>\n");
    fprintf(stderr, "          [--send-address <address>] [--send-port <port>] [--send-interface <interface>]\n");
    fprintf(stderr, "          [--list-address-strict] [--connect-address-strict]\n");
    fprintf(stderr, "          [--lsten-sender-addr <address>] [--listen-sender-port <port>]\n");
    fprintf(stderr, "          [--ignore-errors] [--stop-errors]\n");
    fprintf(stderr, "          [--stats] [--verbose] [--debug]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "--verbose                            Verbose mode, can be specified multiple times (optional)\n");
    fprintf(stderr, "--debug                              Debug mode (optional)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "--listen-address <address>           Listen address (optional)\n");
    fprintf(stderr, "--listen-port <port>                 Listen port (required)\n");
    fprintf(stderr, "--listen-interface <interface>       Listen interface name (optional)\n");
    fprintf(stderr, "--listen-address-strict              Only receive packets from the same source as the first packet (optional)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "--connect-address <address>          Connect address (required)\n");
    fprintf(stderr, "--connect-host <hostname>            Connect host, overwrites caddr if both are specified (required)\n");
    fprintf(stderr, "--connect-port <port>                Connect port (required)\n");
    fprintf(stderr, "--connect-address-strict             Only receive packets from the connect caddr / cport (optional)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "--send-address <address>             Send packets from address (optional)\n");
    fprintf(stderr, "--send-port <port>                   Send packets from port (optional)\n");
    fprintf(stderr, "--send-interface <interface>         Send packets from interface (optional)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "--listen-sender-address <address>    Listen endpoint only accepts packets from this source address (optional)\n");
    fprintf(stderr, "--listen-sender-port <port>          Listen endpoint only accepts packets from this source port (optional)\n");
    fprintf(stderr, "                                     (must be set together, --listen-address-strict is implied)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "--ignore-errors                      Ignore most receive or send errors (unreachable, etc.) instead of exiting (optional) (default)\n");
    fprintf(stderr, "--stop-errors                        Exit on most receive or send errors (unreachable, etc.) (optional)\n");
    fprintf(stderr, "\n");

    exit(EXIT_FAILURE);
}

/**
 * Store command line option values in one place.
 */
struct settings {
    char *laddr;        ///< Listen address
    int lport;          ///< Listen port
    char *lif;          ///< Listen interface

    char *caddr;        ///< Connect address
    char *chost;        ///< Connect host
    int cport;          ///< Connect port

    char *saddr;        ///< Send packets from address
    int sport;          ///< Send packets from port
    char *sif;          ///< Send packets from interface

    int lstrict;        ///< Strict mode for listener (set endpoint on first packet arrival)
    int cstrict;        ///< Strict mode for sender (only accept from caddr / cport)

    char *lsaddr;       ///< Listen port expects packets from this address
    int lsport;         ///< Listen port only expects packets from this port

    int eignore;        //< Ignore most recvfrom / sendto errors

    int stats;          //< Display stats every 60 seconds
};

/**
 * Record and display stats
 */
struct statistics {
    time_t time_display_last;
    time_t time_display_first;

    unsigned long count_listen_packet_receive;
    unsigned long count_listen_byte_receive;

    unsigned long count_listen_packet_send;
    unsigned long count_listen_byte_send;

    unsigned long count_connect_packet_receive;
    unsigned long count_connect_byte_receive;

    unsigned long count_connect_packet_send;
    unsigned long count_connect_byte_send;


    unsigned long count_listen_packet_receive_total;
    unsigned long count_listen_byte_receive_total;

    unsigned long count_listen_packet_send_total;
    unsigned long count_listen_byte_send_total;

    unsigned long count_connect_packet_receive_total;
    unsigned long count_connect_byte_receive_total;

    unsigned long count_connect_packet_send_total;
    unsigned long count_connect_byte_send_total;
};

/**
 * Creates a UDP socket on the specified address, port and interface, returning the socket and
 * the socket name (if either arguments were NULL or 0).
 *
 * @param[in] debug_level The debug level to be used for the DEBUG() macro
 * @param[in] desc The caller description, added to debug messages
 * @param[in] xaddr The IPV4 address for the socket to be created, or NULL for INADDR_ANY
 * @param[in] xport The IPV4 port for the socket to be created, or 0 for random (decided by bind())
 * @param[in] xif The OS interface name to bind to, or NULL for all interfaces.
 * @param[out] xsock_name The name of the socket created.
 * @return The socket file descriptor as integer.
 *
 */
int socket_setup(const int debug_level, const char *desc, const char *xaddr, const int xport, const char *xif, struct sockaddr_in *xsock_name) {
    int xsock;
    const int enable = 1;
    struct sockaddr_in addr;

    /* Set up listening socket */
    DEBUG(DEBUG_LEVEL_INFO, "%s socket: create", desc);
    if ((xsock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP)) == -1) {
        perror("socket");
        DEBUG(DEBUG_LEVEL_ERROR, "Cannot create DGRAM socket (%d)", errno);

        exit(EXIT_FAILURE);
    }

    addr.sin_family = AF_INET;

    /* Address specified or any */
    if (xaddr != NULL) {
        if ((addr.sin_addr.s_addr = inet_addr(xaddr)) == INADDR_NONE) {
            perror("inet_addr");
            DEBUG(DEBUG_LEVEL_ERROR, "%s address invalid %s (%d)", desc, xaddr, errno);

            exit(EXIT_FAILURE);
        }
        DEBUG(DEBUG_LEVEL_INFO, "%s socket: bind to address %s", desc, xaddr);
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
        DEBUG(DEBUG_LEVEL_INFO, "%s socket: bind to address %s", desc, "ANY");
    }

    /* Port specified or any */
    if (xport != 0) {
        addr.sin_port = htons(xport);
        DEBUG(DEBUG_LEVEL_INFO, "%s socket: bind to port %d", desc, xport);
    } else {
        addr.sin_port = 0;
        DEBUG(DEBUG_LEVEL_INFO, "%s socket: bind to port %s", desc, "ANY");
    }

    if (xif != NULL) {
#ifdef __MACH__
        unsigned int xif_idx;

        DEBUG(DEBUG_LEVEL_INFO, "%s socket: bind to interface %s", desc, xif);

        if ((xif_idx = if_nametoindex(xif)) == 0) {
            perror("if_nametoindex");
            DEBUG(DEBUG_LEVEL_ERROR, "Cannot get the interface ID (%d)", errno);

            exit(EXIT_FAILURE);
        }

        if (setsockopt(xsock, IPPROTO_IP, IP_BOUND_IF, &xif_idx, sizeof(xif_idx)) == -1) {
            perror("setsockopt");
            DEBUG(DEBUG_LEVEL_ERROR, "Cannot set socket interface (%d)", errno);

            exit(EXIT_FAILURE);
        }
#elif __unix__
        DEBUG(DEBUG_LEVEL_INFO, "%s socket: bind to interface %s", desc, xif);

        if (setsockopt(xsock, SOL_SOCKET, SO_BINDTODEVICE, xif, strlen(xif)) == -1) {
            perror("setsockopt");
            DEBUG(DEBUG_LEVEL_ERROR, "Cannot set socket interface (%d)", errno);

            exit(EXIT_FAILURE);
        }
#endif
    } else {
        DEBUG(DEBUG_LEVEL_INFO, "%s socket: bind to interface %s", desc, "ANY");
    }

    DEBUG(DEBUG_LEVEL_INFO, "%s socket: reuse local address", desc);

    if (setsockopt(xsock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        perror("setsockopt");
        DEBUG(DEBUG_LEVEL_ERROR, "Cannot set socket SO_REUSEADDR (%d)", errno);

        exit(EXIT_FAILURE);
    }

    DEBUG(DEBUG_LEVEL_INFO, "%s socket: set nonblocking", desc);
    if (fcntl(xsock, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        DEBUG(DEBUG_LEVEL_ERROR, "Cannot set socket O_NONBLOCK (%d)", errno);

        exit(EXIT_FAILURE);
    }

    DEBUG(DEBUG_LEVEL_INFO, "%s socket: bind", desc);
    if (bind(xsock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        DEBUG(DEBUG_LEVEL_ERROR, "Cannot bind socket (%d)", errno);

        exit(EXIT_FAILURE);
    }

    socklen_t xsock_name_len = sizeof(*xsock_name);
    if (getsockname(xsock, (struct sockaddr *)xsock_name, &xsock_name_len) == -1) {
        perror("getsockname");
        DEBUG(DEBUG_LEVEL_ERROR, "Cannot get socket name (%d)", errno);

        exit(EXIT_FAILURE);
    }

    return xsock;
}

/**
 * Resolve a host to an IP address.
 * @param[in] debug_level The debug level to be used for the DEBUG() macro
 * @param[in] host The host to resolve
 * @return A newly allocated buffer containing the IP.
 */
char *resolve_host(int debug_level, const char *host) {
    struct hostent *host_info;
    struct in_addr *address;
    char *retval;

    if ((host_info = gethostbyname(host)) == NULL) {
        perror("gethostbyname");
        DEBUG(DEBUG_LEVEL_INFO, "Could not resolve host %s (%d)", host, errno);

        exit(EXIT_FAILURE);
    }

    address = (struct in_addr *)(host_info->h_addr);
    if ((retval = strdup(inet_ntoa(*address))) == NULL) {
        perror("strdup");
        DEBUG(DEBUG_LEVEL_INFO, "Could not duplicate string (%d)", errno);

        exit(EXIT_FAILURE);
    }

    DEBUG(DEBUG_LEVEL_DEBUG, "Resolved %s to %s", host, strdup(inet_ntoa(*address)));

    return retval;
}

/**
 * Convert an unsigned long value to human readable (i.e., 1500 = 1.5K). Divide by 1000, not 1024.
 * @param[in] value The value to be converted
 * @param[in] host The host to resolve
 * @return The numeric portion of the human readable value.
 */
double int_to_human_value(unsigned long value) {
    double dvalue = value;
    int count = 0;

    while (dvalue > 1000 && count < (HUMAN_READABLE_SIZES_COUNT - 1)) {
        dvalue = dvalue / 1000;
        count = count + 1;
    }

    return dvalue;
}

/**
 * Convert an unsigned long value to human readable (i.e., 1500 = 1.5K). Divide by 1000, not 1024.
 * @param[in] value The value to be converted
 * @param[in] host The host to resolve
 * @return The character (K, M, G, etc.) portion of the human readable value.
 */
char int_to_human_char(unsigned long value) {
    double dvalue = value;
    int count = 0;
    static const char human_readable_sizes[] = HUMAN_READABLE_SIZES;

    while (dvalue > 1000 && count < (HUMAN_READABLE_SIZES_COUNT - 1)) {
        dvalue = dvalue / 1000;
        count = count + 1;
    }

    return human_readable_sizes[count];
}

/**
 * Display the stored stats
 * @param[in] debug_level The debug level to be used for the DEBUG() macro
 * @param[in] st The statistics structure
 * @param[in] now The current time
 */
void stats_display(int debug_level, struct statistics *st, time_t now) {
    int time_delta = now - st->time_display_last;
    int time_delta_total = now - st->time_display_first;

    if (time_delta < 1)
        time_delta = 1;
    if (time_delta_total < 1)
        time_delta_total = 1;

    st->count_listen_packet_receive_total += st->count_listen_packet_receive;
    st->count_listen_byte_receive_total += st->count_listen_byte_receive;
    st->count_listen_packet_send_total += st->count_listen_packet_send;
    st->count_listen_byte_send_total += st->count_listen_byte_send;

    st->count_connect_packet_receive_total += st->count_connect_packet_receive;
    st->count_connect_byte_receive_total += st->count_connect_byte_receive;
    st->count_connect_packet_send_total += st->count_connect_packet_send;
    st->count_connect_byte_send_total += st->count_connect_byte_send;

    DEBUG(DEBUG_LEVEL_INFO, "---- STATS %ds ----", STATS_DELAY_SECONDS);

    DEBUG(DEBUG_LEVEL_INFO, "listen:receive:packets: " HRF " (" HRF "/s), listen:receive:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE(st->count_listen_packet_receive),
            HUMAN_READABLE((float)st->count_listen_packet_receive / time_delta),
            HUMAN_READABLE(st->count_listen_byte_receive),
            HUMAN_READABLE((float)st->count_listen_byte_receive / time_delta));
    DEBUG(DEBUG_LEVEL_INFO, "listen:send:packets: " HRF " (" HRF "/s), listen:send:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE(st->count_listen_packet_send),
            HUMAN_READABLE((float)st->count_listen_packet_send / time_delta),
            HUMAN_READABLE(st->count_listen_byte_send),
            HUMAN_READABLE((float)st->count_listen_byte_send / time_delta));
    DEBUG(DEBUG_LEVEL_INFO, "connect:receive:packets: " HRF " (" HRF "/s), connect:receive:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE(st->count_connect_packet_receive),
            HUMAN_READABLE((float)st->count_connect_packet_receive / time_delta),
            HUMAN_READABLE(st->count_connect_byte_receive),
            HUMAN_READABLE((float)st->count_connect_byte_receive / time_delta));
    DEBUG(DEBUG_LEVEL_INFO, "connect:send:packets: " HRF " (" HRF "/s), connect:send:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE(st->count_connect_packet_send),
            HUMAN_READABLE((float)st->count_connect_packet_send / time_delta),
            HUMAN_READABLE(st->count_connect_byte_send),
            HUMAN_READABLE((float)st->count_connect_byte_send / time_delta));

    DEBUG(DEBUG_LEVEL_INFO, "---- STATS TOTAL ----");

    DEBUG(DEBUG_LEVEL_INFO, "listen:receive:packets: " HRF " (" HRF "/s), listen:receive:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE(st->count_listen_packet_receive_total),
            HUMAN_READABLE((float)st->count_listen_packet_receive_total / time_delta_total),
            HUMAN_READABLE(st->count_listen_byte_receive_total),
            HUMAN_READABLE((float)st->count_listen_byte_receive_total / time_delta_total));
    DEBUG(DEBUG_LEVEL_INFO, "listen:send:packets: " HRF " (" HRF "/s), listen:send:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE(st->count_listen_packet_send_total),
            HUMAN_READABLE((float)st->count_listen_packet_send_total / time_delta_total),
            HUMAN_READABLE(st->count_listen_byte_send_total),
            HUMAN_READABLE((float)st->count_listen_byte_send_total / time_delta_total));
    DEBUG(DEBUG_LEVEL_INFO, "connect:receive:packets: " HRF " (" HRF "/s), connect:receive:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE(st->count_connect_packet_receive_total),
            HUMAN_READABLE((float)st->count_connect_packet_receive_total / time_delta_total),
            HUMAN_READABLE(st->count_connect_byte_receive_total),
            HUMAN_READABLE((float)st->count_connect_byte_receive_total / time_delta_total));
    DEBUG(DEBUG_LEVEL_INFO, "connect:send:packets: " HRF " (" HRF "/s), connect:send:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE(st->count_connect_packet_send_total),
            HUMAN_READABLE((float)st->count_connect_packet_send_total / time_delta_total),
            HUMAN_READABLE(st->count_connect_byte_send_total),
            HUMAN_READABLE((float)st->count_connect_byte_send_total / time_delta_total));

    st->count_listen_packet_receive = st->count_listen_byte_receive = \
        st->count_listen_packet_send = st->count_listen_byte_send = \
        st->count_connect_packet_receive = st->count_connect_byte_receive = \
        st->count_connect_packet_send = st->count_connect_byte_send = 0;
}

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

    /* Command line arguments and default values */
    struct settings s = {
        NULL, 0, NULL,          // *laddr, lport, *lif
        NULL, NULL, 0,          // *caddr, *chost, cport
        NULL, 0, NULL,          // *saddr, sport, *sif
        0, 0,                   // lstrict, cstrict
        NULL, 0,                // *lsaddr, lsport
        1,                      // eignore
        0,                      // stats
    };

    struct statistics statistics_initializer = {
        0,                      // time_display_last
        0,                      // time_display_first
        0, 0,                   // count_listen_packet_receive, count_listen_byte_receive
        0, 0,                   // count_listen_packet_send, count_listen_byte_send
        0, 0,                   // count_connect_packet_receive, count_connect_byte_receive
        0, 0,                   // count_connect_packet_send, count_connect_byte_send
        0, 0,                   // count_listen_packet_receive_total, count_listen_byte_receive_total
        0, 0,                   // count_listen_packet_send_total, count_listen_byte_send_total
        0, 0,                   // count_connect_packet_receive_total, count_connect_byte_receive_total
        0, 0,                   // count_connect_packet_send_total, count_connect_byte_send_total
    };
    struct statistics st = statistics_initializer;

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

        if (s.stats && (now - st.time_display_last) > STATS_DELAY_SECONDS) {
            stats_display(debug_level, &st, now);
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
