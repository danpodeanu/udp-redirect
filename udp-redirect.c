/**
 * @file udp-redirect.c
 * @author Dan Podeanu <pdan@esync.org>
 * @version 2.1.1
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
 * A single-file, high-performance UDP packet redirector supporting IPv4 and
 * IPv6.  Useful when layer-level redirection (e.g., firewall rules) is
 * impractical — common use cases include Wireguard VPN, DNS, and game servers.
 *
 * Design overview:
 * - Two non-blocking UDP sockets: a *listen* socket (faces the local client)
 *   and a *send* socket (faces the remote endpoint).
 * - A poll(2)-based main loop with a 1-second timeout drives all I/O; no
 *   threads or dynamic allocation occur inside the loop.
 * - Packets arriving on the listen socket are forwarded to the connect
 *   endpoint; replies arriving on the send socket are forwarded back to the
 *   most-recently-seen listen-side source.
 * - Optional strict-mode flags limit which sources are accepted on each side.
 * - Optional 60-second windowed statistics are printed to stderr.
 *
 * Targets: Linux x86-64 (SO_BINDTODEVICE) and macOS/Darwin arm64 (IP_BOUND_IF).
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
 * The udp-redirect version
 */
#define UDP_REDIRECT_VERSION    "2.1.1"

/**
 * The delay in seconds between displaying statistics
 */
#define STATISTICS_DELAY_SECONDS    60

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
 * @brief The available debug levels.
 */
enum DEBUG_LEVEL {
    DEBUG_LEVEL_ERROR = 0,      ///< Error messages
    DEBUG_LEVEL_INFO = 1,       ///< Informational messages
    DEBUG_LEVEL_VERBOSE = 2,    ///< Verbose messages
    DEBUG_LEVEL_DEBUG = 3       ///< Debug messages
};

/**
 * Parse a port number from a string (optarg).
 * Sets @p dest to the parsed value, or prints @p label and exits on error.
 * Validates that the string is a pure decimal integer in [0, 65535].
 */
#define PARSE_PORT(src, dest, label) \
    do { \
        char *_endptr; \
        long _val; \
        errno = EOK; \
        _val = strtol((src), &_endptr, 10); \
        if (errno != EOK || _endptr == (src) || *_endptr != '\0' || _val < 0 || _val > 65535) { \
            DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Invalid %s: %s", (label), (src)); \
            exit(EXIT_FAILURE); \
        } \
        (dest) = (int)_val; \
    } while (0)

/**
 * Standard debug macro requiring a locally defined debug level.
 * Adapted from the excellent https://github.com/jleffler/soq/blob/master/src/libsoq/debug.h
 *
 * Note that __VA_ARGS__ is a GCC extension; used for convenience to support DEBUG without format arguments.
 */
#define DEBUG(debug_level_local, debug_level_message, fmt, ...) \
        do { \
            if ((debug_level_local) >= (debug_level_message)) { \
                fprintf(stderr, "%s:%d:%lld:%s(): " fmt "\n", __FILE__, \
                    __LINE__, (long long)(time(NULL)), __func__, ##__VA_ARGS__); \
            } \
        } while (0)

/**
 * Command line options.
 */
static struct option longopts[] = {
    { "verbose",               no_argument,            NULL,           'v' }, ///< Verbose mode, can be specified multiple times (optional)
    { "debug",                 no_argument,            NULL,           'd' }, ///< Debug mode (optional)

    { "listen-address",        required_argument,      NULL,           'a' }, ///< Listen address (optional)
    { "listen-port",           required_argument,      NULL,           'b' }, ///< Listen port (required)
    { "listen-interface",      required_argument,      NULL,           'c' }, ///< Listen interface (optional)

    { "connect-address",       required_argument,      NULL,           'g' }, ///< Connect address (optional if --connect-host is specified)
    { "connect-host",          required_argument,      NULL,           'h' }, ///< Connect host (optional if --connect-address is specified)
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

    { "version",               no_argument,            NULL,           'z' }, ///< Display the version

    { NULL,                    0,                      NULL,            0 }
};

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

    int eignore;        ///< Ignore most recvfrom / sendto errors

    int stats;          ///< Display stats every 60 seconds
};

/**
 * Per-window and cumulative traffic counters.
 *
 * Window fields (no _total suffix) are reset to zero after each call to
 * statistics_display().  Total fields accumulate for the lifetime of the
 * process.  Both sets are updated inside statistics_display() before printing.
 */
struct statistics {
    time_t time_display_last;   ///< Wall-clock time of the last statistics_display() call; used to compute the window duration
    time_t time_display_first;  ///< Wall-clock time of the first statistics_display() call; used to compute the cumulative duration

    unsigned long count_listen_packet_receive;  ///< Packets received on the listen socket this window
    unsigned long count_listen_byte_receive;    ///< Bytes received on the listen socket this window

    unsigned long count_listen_packet_send;     ///< Packets sent back on the listen socket (connect→client) this window
    unsigned long count_listen_byte_send;       ///< Bytes sent back on the listen socket this window

    unsigned long count_connect_packet_receive; ///< Packets received on the send socket (from remote endpoint) this window
    unsigned long count_connect_byte_receive;   ///< Bytes received on the send socket this window

    unsigned long count_connect_packet_send;    ///< Packets forwarded to the remote endpoint this window
    unsigned long count_connect_byte_send;      ///< Bytes forwarded to the remote endpoint this window

    unsigned long count_listen_packet_receive_total;  ///< Cumulative packets received on the listen socket
    unsigned long count_listen_byte_receive_total;    ///< Cumulative bytes received on the listen socket

    unsigned long count_listen_packet_send_total;     ///< Cumulative packets sent back on the listen socket
    unsigned long count_listen_byte_send_total;       ///< Cumulative bytes sent back on the listen socket

    unsigned long count_connect_packet_receive_total; ///< Cumulative packets received on the send socket
    unsigned long count_connect_byte_receive_total;   ///< Cumulative bytes received on the send socket

    unsigned long count_connect_packet_send_total;    ///< Cumulative packets forwarded to the remote endpoint
    unsigned long count_connect_byte_send_total;      ///< Cumulative bytes forwarded to the remote endpoint
};

/* Function prototypes */

static int socket_setup(const int debug_level, const char *desc, const char *xaddr, const int xport, const char *xif, int xfamily, struct sockaddr_storage *xsock_name);
static char *resolve_host(int debug_level, const char *host);

static int parse_addr(const char *str, int port, struct sockaddr_storage *out);
static const char *addr_tostring(const struct sockaddr_storage *sa, char *buf, size_t len);
static int addr_port(const struct sockaddr_storage *sa);
static socklen_t addr_len(const struct sockaddr_storage *sa);
static int addr_is_unset(const struct sockaddr_storage *sa);
static int addr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b);

void settings_initialize(struct settings *s);
void usage(const char *argv0, const char *message);

void statistics_initialize(struct statistics *st);
double int_to_human_value(double value);
char int_to_human_char(double value);
void statistics_display(int debug_level, struct statistics *st, time_t now);

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

    struct sockaddr_storage lsock_name; /* Listen socket name */
    struct sockaddr_storage ssock_name; /* Send socket name */

    struct sockaddr_storage caddr; /* Connect address */

    /* Simplify addr_tostring() usage in DEBUG() by reserving buffers */
    char print_buffer1[INET6_ADDRSTRLEN];
    char print_buffer2[INET6_ADDRSTRLEN];

    static char network_buffer[NETWORK_BUFFER_SIZE]; /* The network buffer. All reads and writes happen here */

    struct pollfd ufds[2]; /* Poll file descriptors */

    struct sockaddr_storage endpoint; /* Address where the current packet was received from */
    struct sockaddr_storage previous_endpoint; /* Address where the previous packet was received from */

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
                PARSE_PORT(optarg, s.lport, "listen port");

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
                PARSE_PORT(optarg, s.cport, "connect port");

                break;
            case 'm': /* --send-address */
                s.saddr = optarg;

                break;
            case 'n': /* --send-port */
                PARSE_PORT(optarg, s.sport, "send port");

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
                PARSE_PORT(optarg, s.lsport, "listen sender port");

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
            case 'z': /* --version */
                fprintf(stderr, "udp-redirect v%s\n", UDP_REDIRECT_VERSION);
                exit(EXIT_SUCCESS);

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
        usage(argv0, "Options --listen-sender-port and --list-sender-address must either both be specified or none");
    }

    /* Set strict mode if using lsport and csport */
    if (s.lsaddr != NULL && s.lsport != 0) {
        s.lstrict = 1;
    }

    /* Resolve connect host if available */
    char *caddr_alloc = NULL;
    if (s.chost != NULL) {
        caddr_alloc = resolve_host(debug_level, s.chost);
        s.caddr = caddr_alloc;
    }

    DEBUG(debug_level, DEBUG_LEVEL_INFO, "---- INFO ----");

    DEBUG(debug_level, DEBUG_LEVEL_INFO, "Listen address: %s", (s.laddr != NULL)?s.laddr:"ANY");
    DEBUG(debug_level, DEBUG_LEVEL_INFO, "Listen port: %d", s.lport);
    DEBUG(debug_level, DEBUG_LEVEL_INFO, "Listen interface: %s", (s.lif != NULL)?s.lif:"ANY");

    if (s.chost != NULL) {
        DEBUG(debug_level, DEBUG_LEVEL_INFO, "Connect host: %s", s.chost);
    }

    if (s.caddr != NULL) {
        DEBUG(debug_level, DEBUG_LEVEL_INFO, "Connect address: %s", s.caddr);
    }

    DEBUG(debug_level, DEBUG_LEVEL_INFO, "Connect port: %d", s.cport);

    DEBUG(debug_level, DEBUG_LEVEL_INFO, "Send address: %s", (s.saddr != NULL)?s.saddr:"ANY");
    if (s.sport != 0) {
        DEBUG(debug_level, DEBUG_LEVEL_INFO, "Send port: %d", s.sport);
    } else {
        DEBUG(debug_level, DEBUG_LEVEL_INFO, "Send port: %s", "ANY");
    }
    DEBUG(debug_level, DEBUG_LEVEL_INFO, "Send interface: %s", (s.sif != NULL)?s.sif:"ANY");

    DEBUG(debug_level, DEBUG_LEVEL_INFO, "Listen strict: %s", s.lstrict?"ENABLED":"DISABLED");
    DEBUG(debug_level, DEBUG_LEVEL_INFO, "Connect strict: %s", s.cstrict?"ENABLED":"DISABLED");

    if (s.lsaddr != NULL) {
        DEBUG(debug_level, DEBUG_LEVEL_INFO, "Listen only accepts packets from address: %s", s.lsaddr);
    }
    if (s.lsport != 0) {
        DEBUG(debug_level, DEBUG_LEVEL_INFO, "Listen only accepts packets from port: %d", s.lsport);
    }

    DEBUG(debug_level, DEBUG_LEVEL_INFO, "Ignore errors: %s", s.eignore?"ENABLED":"DISABLED");
    DEBUG(debug_level, DEBUG_LEVEL_INFO, "Display stats: %s", s.stats?"ENABLED":"DISABLED");

    DEBUG(debug_level, DEBUG_LEVEL_INFO, "---- START ----");

    /* Set up connect address first so we know the family for the sockets below */
    if (parse_addr(s.caddr, s.cport, &caddr) == -1) {
        DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Invalid connect address: %s", s.caddr);

        if (caddr_alloc != NULL) {
            free(caddr_alloc);
        }
        exit(EXIT_FAILURE);
    }
    if (caddr_alloc != NULL) {
        free(caddr_alloc);
        s.caddr = NULL;
    }

    lsock = socket_setup(debug_level, "Listen", s.laddr, s.lport, s.lif, (int)caddr.ss_family, &lsock_name); /* Set up listening socket */
    ssock = socket_setup(debug_level, "Send", s.saddr, s.sport, s.sif, (int)caddr.ss_family, &ssock_name); /* Set up send socket */

    memset(&endpoint, 0, sizeof(endpoint)); /* No packet received, no endpoint */

    /* previous_endpoint: ss_family == 0 (zero-init) means "no endpoint seen yet" */
    memset(&previous_endpoint, 0, sizeof(previous_endpoint));
    if (s.lsaddr != NULL && s.lsport != 0) {
        if (parse_addr(s.lsaddr, s.lsport, &previous_endpoint) == -1) {
            DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Invalid listen sender address: %s", s.lsaddr);

            exit(EXIT_FAILURE);
        }
    }

    socklen_t endpoint_len = sizeof(endpoint);

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

    DEBUG(debug_level, DEBUG_LEVEL_VERBOSE, "entering infinite loop");

    st.time_display_first = st.time_display_last = time(NULL);

    /* Main loop */
    while (1) {
        int poll_retval;
        ssize_t recvfrom_retval;
        ssize_t sendto_retval;

        now = time(NULL);

        ufds[0].fd = lsock; ufds[0].events = POLLIN | POLLPRI; ufds[0].revents = 0;
        ufds[1].fd = ssock; ufds[1].events = POLLIN | POLLPRI; ufds[1].revents = 0;

        DEBUG(debug_level, DEBUG_LEVEL_DEBUG, "waiting for readable sockets");

        if (s.stats && (now - st.time_display_last) >= STATISTICS_DELAY_SECONDS) {
            statistics_display(debug_level, &st, now);
            st.time_display_last = now;
        }

        if ((poll_retval = poll(ufds, 2, 1000)) == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("poll");
            DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Could not check readable sockets (%d)", errno);

            exit(EXIT_FAILURE);
        }
        if (poll_retval == 0) {
            DEBUG(debug_level, DEBUG_LEVEL_DEBUG, "poll timeout");
            continue;
        }

        /* New data on the LISTEN socket */
        if (ufds[0].revents & POLLIN || ufds[0].revents & POLLPRI) {
            endpoint_len = sizeof(endpoint);
            if ((recvfrom_retval = recvfrom(lsock, network_buffer, sizeof(network_buffer), 0,
                            (struct sockaddr *)&endpoint, &endpoint_len)) == -1) {
                if (!ERRNO_IGNORE_CHECK(errno_ignore, errno)) {
                    perror("recvfrom");
                    DEBUG(debug_level, DEBUG_LEVEL_INFO, "Listen cannot receive (%d)", errno);

                    exit(EXIT_FAILURE);
                }
            }
            if (recvfrom_retval >= 0) {
                st.count_listen_packet_receive++;
                st.count_listen_byte_receive += recvfrom_retval;

                DEBUG(debug_level, DEBUG_LEVEL_DEBUG, "RECEIVE (%s, %d) -> (%s, %d) (LISTEN PORT): %zd bytes",
                        addr_tostring(&endpoint, print_buffer1, sizeof(print_buffer1)), addr_port(&endpoint),
                        addr_tostring(&lsock_name, print_buffer2, sizeof(print_buffer2)), addr_port(&lsock_name),
                        recvfrom_retval);

                /** Accept the packet IF:
                  * - There's no previous endpoint, OR
                  * - There is a previous endpoint, but we are not in strict mode, OR
                  * - The previous endpoint matches the current endpoint
                */
                if ((addr_is_unset(&previous_endpoint) || !s.lstrict) ||
                        addr_equal(&previous_endpoint, &endpoint)) {

                    if (addr_is_unset(&previous_endpoint) || !s.lstrict) {
                        if (!addr_equal(&previous_endpoint, &endpoint)) {
                            DEBUG(debug_level, DEBUG_LEVEL_DEBUG, "LISTEN remote endpoint set to (%s, %d)", addr_tostring(&endpoint, print_buffer1, sizeof(print_buffer1)), addr_port(&endpoint));
                        }

                        memcpy(&previous_endpoint, &endpoint, sizeof(endpoint));
                    }

                    if ((sendto_retval = sendto(ssock, network_buffer, recvfrom_retval, 0,
                                    (struct sockaddr *)&caddr, addr_len(&caddr))) == -1) {
                        if (!ERRNO_IGNORE_CHECK(errno_ignore, errno)) {
                            perror("sendto");
                            DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Cannot send packet to send port (%d)", errno);

                            exit(EXIT_FAILURE);
                        }
                    } else { // At least one byte was sent, record it
                        st.count_connect_packet_send++;
                        st.count_connect_byte_send += sendto_retval;
                    }

                    DEBUG(debug_level, (sendto_retval == recvfrom_retval || s.eignore == 1)?DEBUG_LEVEL_DEBUG:DEBUG_LEVEL_ERROR,
                            "SEND (%s, %d) -> (%s, %d) (SEND PORT): %zd bytes (%s WRITE %zd bytes)",
                            addr_tostring(&ssock_name, print_buffer1, sizeof(print_buffer1)), addr_port(&ssock_name),
                            addr_tostring(&caddr, print_buffer2, sizeof(print_buffer2)), addr_port(&caddr),
                            sendto_retval,
                            (sendto_retval == recvfrom_retval)?"FULL":"PARTIAL", recvfrom_retval);
                } else {
                    DEBUG(debug_level, DEBUG_LEVEL_ERROR, "LISTEN PORT invalid source (%s, %d), was expecting (%s, %d)",
                            addr_tostring(&endpoint, print_buffer1, sizeof(print_buffer1)), addr_port(&endpoint),
                            addr_tostring(&previous_endpoint, print_buffer2, sizeof(print_buffer2)), addr_port(&previous_endpoint));
                }
            }
        }

        /* New data on the SEND socket */
        if (ufds[1].revents & POLLIN || ufds[1].revents & POLLPRI) {
            endpoint_len = sizeof(endpoint);
            if ((recvfrom_retval = recvfrom(ssock, network_buffer, sizeof(network_buffer), 0,
                            (struct sockaddr *)&endpoint, &endpoint_len)) == -1) {
                if (!ERRNO_IGNORE_CHECK(errno_ignore, errno)) {
                    perror("recvfrom");
                    DEBUG(debug_level, DEBUG_LEVEL_INFO, "Send cannot receive packet (%d)", errno);

                    exit(EXIT_FAILURE);
                }
            }
            if (recvfrom_retval >= 0) {
                st.count_connect_packet_receive++;
                st.count_connect_byte_receive += recvfrom_retval;

                DEBUG(debug_level, DEBUG_LEVEL_DEBUG, "RECEIVE (%s, %d) -> (%s, %d) (SEND PORT): %zd bytes",
                        addr_tostring(&endpoint, print_buffer1, sizeof(print_buffer1)), addr_port(&endpoint),
                        addr_tostring(&ssock_name, print_buffer2, sizeof(print_buffer2)), addr_port(&ssock_name),
                        recvfrom_retval);

                /** Accept the packet IF:
                  * - The listen socket has received a packet, so we know the endpoint, AND
                  * - The packet was received from the connect endpoint, OR
                  * - We are not in strict mode
                  */
                if (!addr_is_unset(&previous_endpoint) &&
                        (!s.cstrict || addr_equal(&caddr, &endpoint))) {

                    if ((sendto_retval = sendto(lsock, network_buffer, recvfrom_retval, 0,
                                    (struct sockaddr *)&previous_endpoint, addr_len(&previous_endpoint))) == -1) {
                        if (!ERRNO_IGNORE_CHECK(errno_ignore, errno)) {
                            perror("sendto");
                            DEBUG(debug_level, DEBUG_LEVEL_INFO, "Cannot send packet to listen port (%d)", errno);

                            exit(EXIT_FAILURE);
                        }
                    } else { // At least one byte was sent, record it
                        st.count_listen_packet_send++;
                        st.count_listen_byte_send += sendto_retval;
                    }

                    DEBUG(debug_level, (sendto_retval == recvfrom_retval || s.eignore == 1)?DEBUG_LEVEL_DEBUG:DEBUG_LEVEL_ERROR,
                            "SEND (%s, %d) -> (%s, %d) (LISTEN PORT): %zd bytes (%s WRITE %zd bytes)",
                            addr_tostring(&lsock_name, print_buffer1, sizeof(print_buffer1)), addr_port(&lsock_name),
                            addr_tostring(&previous_endpoint, print_buffer2, sizeof(print_buffer2)), addr_port(&previous_endpoint),
                            sendto_retval,
                            (sendto_retval == recvfrom_retval)?"FULL":"PARTIAL", recvfrom_retval);
                } else {
                    DEBUG(debug_level, DEBUG_LEVEL_ERROR, "SEND PORT invalid source (%s, %d), was expecting (%s, %d)",
                            addr_tostring(&endpoint, print_buffer1, sizeof(print_buffer1)), addr_port(&endpoint),
                            addr_tostring(&caddr, print_buffer2, sizeof(print_buffer2)), addr_port(&caddr));
                }
            }
        }
    }

    /* Never reached. */
    return 0;
}

/* Network helper functions below */

/**
 * Parse an IPv4 or IPv6 address string into a sockaddr_storage.
 * Sets ss_family, the address field, and the port.
 * @param[in]  str  Dotted-quad IPv4 or colon-separated IPv6 address string.
 * @param[in]  port Port number in host byte order.
 * @param[out] out  Zeroed sockaddr_storage to fill.
 * @return AF_INET or AF_INET6 on success, -1 if the string is not a valid address.
 */
static int parse_addr(const char *str, int port, struct sockaddr_storage *out) {
    struct sockaddr_in  *a4 = (struct sockaddr_in  *)out;
    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)out;

    memset(out, 0, sizeof(*out));

    /* Reject IPv6 zone IDs (e.g. "fe80::1%en0"): inet_pton on macOS silently
     * accepts them but drops the scope, leading to inconsistent behaviour
     * across platforms.  Zone IDs are not supported. */
    if (strchr(str, '%') != NULL)
        return -1;

    if (inet_pton(AF_INET, str, &a4->sin_addr) == 1) {
        out->ss_family = AF_INET;
        a4->sin_port   = htons(port);
        return AF_INET;
    }
    if (inet_pton(AF_INET6, str, &a6->sin6_addr) == 1) {
        out->ss_family  = AF_INET6;
        a6->sin6_port   = htons(port);
        return AF_INET6;
    }
    return -1;
}

/**
 * Format the address portion of a sockaddr_storage into buf using inet_ntop.
 * @return buf on success, "?" on failure.
 */
static const char *addr_tostring(const struct sockaddr_storage *sa, char *buf, size_t len) {
    const void *ptr;
    if (sa->ss_family == AF_INET6)
        ptr = &((const struct sockaddr_in6 *)sa)->sin6_addr;
    else
        ptr = &((const struct sockaddr_in  *)sa)->sin_addr;
    return inet_ntop((int)sa->ss_family, ptr, buf, (socklen_t)len) ? buf : "?";
}

/**
 * Return the port from a sockaddr_storage in host byte order.
 */
static int addr_port(const struct sockaddr_storage *sa) {
    if (sa->ss_family == AF_INET6)
        return ntohs(((const struct sockaddr_in6 *)sa)->sin6_port);
    return ntohs(((const struct sockaddr_in *)sa)->sin_port);
}

/**
 * Return the wire size of a sockaddr_storage appropriate for its address family.
 */
static socklen_t addr_len(const struct sockaddr_storage *sa) {
    return (sa->ss_family == AF_INET6)
        ? (socklen_t)sizeof(struct sockaddr_in6)
        : (socklen_t)sizeof(struct sockaddr_in);
}

/**
 * Return 1 if the address is the zero-initialised "unset" sentinel (ss_family == 0).
 */
static int addr_is_unset(const struct sockaddr_storage *sa) {
    return sa->ss_family == 0;
}

/**
 * Return 1 if both sockaddr_storage values have the same family, address, and port.
 */
static int addr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
    if (a->ss_family != b->ss_family)
        return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *a4 = (const struct sockaddr_in *)a;
        const struct sockaddr_in *b4 = (const struct sockaddr_in *)b;
        return a4->sin_addr.s_addr == b4->sin_addr.s_addr &&
               a4->sin_port        == b4->sin_port;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)a;
        const struct sockaddr_in6 *b6 = (const struct sockaddr_in6 *)b;
        return memcmp(&a6->sin6_addr, &b6->sin6_addr, sizeof(struct in6_addr)) == 0 &&
               a6->sin6_port == b6->sin6_port;
    }
    return 0;
}

/**
 * Creates a UDP socket on the specified address, port and interface, returning the socket and
 * the socket name (if either arguments were NULL or 0).
 *
 * @param[in] debug_level The debug level to be used for the DEBUG() macro
 * @param[in] desc The caller description, added to debug messages
 * @param[in] xaddr The address for the socket to be created, or NULL for ANY
 * @param[in] xport The port for the socket to be created, or 0 for random (decided by bind())
 * @param[in] xif The OS interface name to bind to, or NULL for all interfaces.
 * @param[in] xfamily AF_INET or AF_INET6; used only when xaddr is NULL.
 * @param[out] xsock_name The name of the socket created.
 * @return The socket file descriptor as integer.
 *
 */
static int socket_setup(const int debug_level, const char *desc, const char *xaddr, const int xport, const char *xif, int xfamily, struct sockaddr_storage *xsock_name) {
    int xsock;
    const int enable = 1;
    int family;
    struct sockaddr_storage addr;

    memset(&addr, 0, sizeof(addr));

    /* Determine address family and fill bind address */
    if (xaddr != NULL) {
        if (parse_addr(xaddr, xport, &addr) == -1) {
            DEBUG(debug_level, DEBUG_LEVEL_ERROR, "%s address invalid: %s", desc, xaddr);

            exit(EXIT_FAILURE);
        }
        family = (int)addr.ss_family;
        DEBUG(debug_level, DEBUG_LEVEL_INFO, "%s socket: bind to address %s", desc, xaddr);
    } else {
        family = xfamily;
        addr.ss_family = (sa_family_t)family;
        if (family == AF_INET6) {
            ((struct sockaddr_in6 *)&addr)->sin6_addr = in6addr_any;
            ((struct sockaddr_in6 *)&addr)->sin6_port = htons(xport);
        } else {
            ((struct sockaddr_in *)&addr)->sin_addr.s_addr = INADDR_ANY;
            ((struct sockaddr_in *)&addr)->sin_port = htons(xport);
        }
        DEBUG(debug_level, DEBUG_LEVEL_INFO, "%s socket: bind to address %s", desc, "ANY");
    }

    /* Port specified or any */
    if (xport != 0) {
        DEBUG(debug_level, DEBUG_LEVEL_INFO, "%s socket: bind to port %d", desc, xport);
    } else {
        DEBUG(debug_level, DEBUG_LEVEL_INFO, "%s socket: bind to port %s", desc, "ANY");
    }

    /* Set up socket */
    DEBUG(debug_level, DEBUG_LEVEL_INFO, "%s socket: create", desc);
    if ((xsock = socket(family, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        perror("socket");
        DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Cannot create DGRAM socket (%d)", errno);

        exit(EXIT_FAILURE);
    }

    if (xif != NULL) {
#ifdef __MACH__
        unsigned int xif_idx;

        DEBUG(debug_level, DEBUG_LEVEL_INFO, "%s socket: bind to interface %s", desc, xif);

        if ((xif_idx = if_nametoindex(xif)) == 0) {
            perror("if_nametoindex");
            DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Cannot get the interface ID (%d)", errno);

            exit(EXIT_FAILURE);
        }

        {
            int if_level  = (family == AF_INET6) ? IPPROTO_IPV6 : IPPROTO_IP;
            int if_optname = (family == AF_INET6) ? IPV6_BOUND_IF : IP_BOUND_IF;
            if (setsockopt(xsock, if_level, if_optname, &xif_idx, sizeof(xif_idx)) == -1) {
                perror("setsockopt");
                DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Cannot set socket interface (%d)", errno);

                exit(EXIT_FAILURE);
            }
        }
#elif __unix__
        DEBUG(debug_level, DEBUG_LEVEL_INFO, "%s socket: bind to interface %s", desc, xif);

        if (strlen(xif) >= IFNAMSIZ) {
            DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Interface name too long (max %d): %s", IFNAMSIZ - 1, xif);
            exit(EXIT_FAILURE);
        }

        if (setsockopt(xsock, SOL_SOCKET, SO_BINDTODEVICE, xif, strlen(xif) + 1) == -1) {
            perror("setsockopt");
            DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Cannot set socket interface (%d)", errno);

            exit(EXIT_FAILURE);
        }
#endif
    } else {
        DEBUG(debug_level, DEBUG_LEVEL_INFO, "%s socket: bind to interface %s", desc, "ANY");
    }

    DEBUG(debug_level, DEBUG_LEVEL_INFO, "%s socket: reuse local address", desc);

    if (setsockopt(xsock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        perror("setsockopt");
        DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Cannot set socket SO_REUSEADDR (%d)", errno);

        exit(EXIT_FAILURE);
    }

    DEBUG(debug_level, DEBUG_LEVEL_INFO, "%s socket: set nonblocking", desc);
    if (fcntl(xsock, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Cannot set socket O_NONBLOCK (%d)", errno);

        exit(EXIT_FAILURE);
    }

    DEBUG(debug_level, DEBUG_LEVEL_INFO, "%s socket: bind", desc);
    if (bind(xsock, (struct sockaddr *)&addr, addr_len(&addr)) == -1) {
        perror("bind");
        DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Cannot bind socket (%d)", errno);

        exit(EXIT_FAILURE);
    }

    socklen_t xsock_name_len = sizeof(*xsock_name);
    if (getsockname(xsock, (struct sockaddr *)xsock_name, &xsock_name_len) == -1) {
        perror("getsockname");
        DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Cannot get socket name (%d)", errno);

        exit(EXIT_FAILURE);
    }

    return xsock;
}

/**
 * Resolve a host to an IP address.
 * @param[in] debug_level The debug level to be used for the DEBUG() macro
 * @param[in] host The host to resolve
 * @return A newly heap-allocated NUL-terminated string containing the resolved
 *         IP address (IPv4 dotted-quad or IPv6 colon-separated).  The caller
 *         owns the buffer and must free() it when done.  On failure the
 *         function prints a diagnostic and calls exit(EXIT_FAILURE).
 */
static char *resolve_host(int debug_level, const char *host) {
    struct addrinfo hints;
    struct addrinfo *res;
    char buf[INET6_ADDRSTRLEN];
    char *retval;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;   /* accept IPv4 or IPv6 */
    hints.ai_socktype = SOCK_DGRAM;

    if ((rc = getaddrinfo(host, NULL, &hints, &res)) != 0) {
        DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Could not resolve host %s: %s", host, gai_strerror(rc));

        exit(EXIT_FAILURE);
    }

    /* Use the first result; inet_ntop converts it back to a string for parse_addr() */
    {
        const void *addr_ptr = (res->ai_family == AF_INET6)
            ? (const void *)&((struct sockaddr_in6 *)res->ai_addr)->sin6_addr
            : (const void *)&((struct sockaddr_in  *)res->ai_addr)->sin_addr;

        if (inet_ntop(res->ai_family, addr_ptr, buf, sizeof(buf)) == NULL) {
            perror("inet_ntop");
            DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Could not format resolved address (%d)", errno);
            freeaddrinfo(res);

            exit(EXIT_FAILURE);
        }
    }

    freeaddrinfo(res);

    if ((retval = strdup(buf)) == NULL) {
        perror("strdup");
        DEBUG(debug_level, DEBUG_LEVEL_ERROR, "Could not duplicate resolved address string (%d)", errno);

        exit(EXIT_FAILURE);
    }

    DEBUG(debug_level, DEBUG_LEVEL_DEBUG, "Resolved %s to %s", host, retval);

    return retval;
}

/* Settings helper functions below */

/**
 * Initialize settings.
 * @param[out] s The settings structure to initialize.
 */
void settings_initialize(struct settings *s) {
    s->laddr = NULL;
    s->lport = 0;
    s->lif = NULL;

    s->caddr = NULL;
    s->chost = NULL;
    s->cport = 0;

    s->saddr = NULL;
    s->sport = 0;
    s->sif = NULL;

    s->lstrict = 0;
    s->cstrict = 0;

    s->lsaddr = NULL;
    s->lsport = 0;

    s->eignore = 1;
    s->stats = 0;
}

/**
 * Displays the program usage and exit with error.
 *
 * @param[in] argv0 The program name as started, e.g., /usr/local/bin/udp-redirect
 * @param[in] message The error message, or NULL
 *
 */
void usage(const char *argv0, const char *message) {
    if (message != NULL)
        fprintf(stderr, "%s\n", message);

    fprintf(stderr, "Usage: %s\n", argv0);
    fprintf(stderr, "          [--listen-address <address>] --listen-port <port> [--listen-interface <interface>]\n");
    fprintf(stderr, "          [--connect-address <address> | --connect-host <hostname> --connect-port <port>\n");
    fprintf(stderr, "          [--send-address <address>] [--send-port <port>] [--send-interface <interface>]\n");
    fprintf(stderr, "          [--listen-address-strict] [--connect-address-strict]\n");
    fprintf(stderr, "          [--listen-sender-address <address>] [--listen-sender-port <port>]\n");
    fprintf(stderr, "          [--ignore-errors] [--stop-errors]\n");
    fprintf(stderr, "          [--stats] [--verbose] [--debug] [--version]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "--stats                                 Display sent/received bytes statistics every 60 seconds (optional)\n");
    fprintf(stderr, "--verbose                               Verbose mode, can be specified multiple times (optional)\n");
    fprintf(stderr, "--debug                                 Debug mode (optional)\n");
    fprintf(stderr, "--version                               Display the version and exit\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "--listen-address <address>              Listen address, IPv4 or IPv6 (optional)\n");
    fprintf(stderr, "--listen-port <port>                    Listen port (required)\n");
    fprintf(stderr, "--listen-interface <interface>          Listen interface name (optional)\n");
    fprintf(stderr, "--listen-address-strict                 Only receive packets from the same source as the first packet (optional)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "--connect-address <address>             Connect address, IPv4 or IPv6 (optional if --connect-host is specified)\n");
    fprintf(stderr, "--connect-host <hostname>               Connect host, overwrites --connect-address if both are specified (optional if --connect-address is specified)\n");
    fprintf(stderr, "--connect-port <port>                   Connect port (required)\n");
    fprintf(stderr, "--connect-address-strict                Only receive packets from --connect-address / --connect-port (optional)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "--send-address <address>                Send packets from address, IPv4 or IPv6 (optional)\n");
    fprintf(stderr, "--send-port <port>                      Send packets from port (optional)\n");
    fprintf(stderr, "--send-interface <interface>            Send packets from interface (optional)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "--listen-sender-address <address>       Listen endpoint only accepts packets from this source address, IPv4 or IPv6 (optional)\n");
    fprintf(stderr, "--listen-sender-port <port>             Listen endpoint only accepts packets from this source port (optional)\n");
    fprintf(stderr, "                                        (must be set together, --listen-address-strict is implied)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "--ignore-errors                         Ignore most receive or send errors (unreachable, etc.) instead of exiting (optional) (default)\n");
    fprintf(stderr, "--stop-errors                           Exit on most receive or send errors (unreachable, etc.) (optional)\n");
    fprintf(stderr, "\n");

    exit(EXIT_FAILURE);
}

/* Statistics helper functions below */

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
 * Initialize statistics.
 * @param[out] st The statistics structure to initialize.
 */
void statistics_initialize(struct statistics *st) {
    st->time_display_last = 0;
    st->time_display_first = 0;

    st->count_listen_packet_receive = 0;
    st->count_listen_byte_receive = 0;

    st->count_listen_packet_send = 0;
    st->count_listen_byte_send = 0;

    st->count_connect_packet_receive = 0;
    st->count_connect_byte_receive = 0;

    st->count_connect_packet_send = 0;
    st->count_connect_byte_send = 0;

    st->count_listen_packet_receive_total = 0;
    st->count_listen_byte_receive_total = 0;

    st->count_listen_packet_send_total = 0;
    st->count_listen_byte_send_total = 0;

    st->count_connect_packet_receive_total = 0;
    st->count_connect_byte_receive_total = 0;

    st->count_connect_packet_send_total = 0;
    st->count_connect_byte_send_total = 0;
}

/**
 * Shared scaling helper: divides @p value by 1000 repeatedly until it is
 * <= 1000 or the maximum prefix is reached.
 * @param[in]  value     The raw value to scale.
 * @param[out] count_out The number of divisions performed (index into the
 *                       human-readable suffix array).
 * @return The scaled value.
 */
static double int_to_human_scale(double value, int *count_out) {
    int count = 0;

    while (value >= 1000 && count < (HUMAN_READABLE_SIZES_COUNT - 1)) {
        value = value / 1000;
        count = count + 1;
    }

    *count_out = count;
    return value;
}

/**
 * Convert a value to human readable (i.e., 1500 = 1.5K). Divide by 1000, not 1024.
 * @param[in] value The value to be converted
 * @return The numeric portion of the human readable value.
 */
double int_to_human_value(double value) {
    int count;
    return int_to_human_scale(value, &count);
}

/**
 * Convert a value to human readable (i.e., 1500 = 1.5K). Divide by 1000, not 1024.
 * @param[in] value The value to be converted
 * @return The character (K, M, G, etc.) portion of the human readable value.
 */
char int_to_human_char(double value) {
    static const char human_readable_sizes[] = HUMAN_READABLE_SIZES;
    int count;
    int_to_human_scale(value, &count);
    return human_readable_sizes[count];
}

/**
 * Display the stored statistics
 * @param[in] debug_level The debug level to be used for the DEBUG() macro
 * @param[in] st The statistics structure
 * @param[in] now The current time
 */
void statistics_display(int debug_level, struct statistics *st, time_t now) {
    /* Stats were explicitly requested: ensure they're visible at any verbosity level. */
    if (debug_level < DEBUG_LEVEL_INFO)
        debug_level = DEBUG_LEVEL_INFO;

    time_t time_delta = now - st->time_display_last;
    time_t time_delta_total = now - st->time_display_first;

    /* Clamp to 1 to avoid division by zero when called within the same second. */
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

    DEBUG(debug_level, DEBUG_LEVEL_INFO, "---- STATS %ds ----", STATISTICS_DELAY_SECONDS);

    DEBUG(debug_level, DEBUG_LEVEL_INFO, "listen:receive:packets: " HRF " (" HRF "/s), listen:receive:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_listen_packet_receive),
            HUMAN_READABLE((double)st->count_listen_packet_receive / time_delta),
            HUMAN_READABLE((double)st->count_listen_byte_receive),
            HUMAN_READABLE((double)st->count_listen_byte_receive / time_delta));
    DEBUG(debug_level, DEBUG_LEVEL_INFO, "listen:send:packets: " HRF " (" HRF "/s), listen:send:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_listen_packet_send),
            HUMAN_READABLE((double)st->count_listen_packet_send / time_delta),
            HUMAN_READABLE((double)st->count_listen_byte_send),
            HUMAN_READABLE((double)st->count_listen_byte_send / time_delta));
    DEBUG(debug_level, DEBUG_LEVEL_INFO, "connect:receive:packets: " HRF " (" HRF "/s), connect:receive:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_connect_packet_receive),
            HUMAN_READABLE((double)st->count_connect_packet_receive / time_delta),
            HUMAN_READABLE((double)st->count_connect_byte_receive),
            HUMAN_READABLE((double)st->count_connect_byte_receive / time_delta));
    DEBUG(debug_level, DEBUG_LEVEL_INFO, "connect:send:packets: " HRF " (" HRF "/s), connect:send:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_connect_packet_send),
            HUMAN_READABLE((double)st->count_connect_packet_send / time_delta),
            HUMAN_READABLE((double)st->count_connect_byte_send),
            HUMAN_READABLE((double)st->count_connect_byte_send / time_delta));

    DEBUG(debug_level, DEBUG_LEVEL_INFO, "---- STATS TOTAL ----");

    DEBUG(debug_level, DEBUG_LEVEL_INFO, "listen:receive:packets: " HRF " (" HRF "/s), listen:receive:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_listen_packet_receive_total),
            HUMAN_READABLE((double)st->count_listen_packet_receive_total / time_delta_total),
            HUMAN_READABLE((double)st->count_listen_byte_receive_total),
            HUMAN_READABLE((double)st->count_listen_byte_receive_total / time_delta_total));
    DEBUG(debug_level, DEBUG_LEVEL_INFO, "listen:send:packets: " HRF " (" HRF "/s), listen:send:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_listen_packet_send_total),
            HUMAN_READABLE((double)st->count_listen_packet_send_total / time_delta_total),
            HUMAN_READABLE((double)st->count_listen_byte_send_total),
            HUMAN_READABLE((double)st->count_listen_byte_send_total / time_delta_total));
    DEBUG(debug_level, DEBUG_LEVEL_INFO, "connect:receive:packets: " HRF " (" HRF "/s), connect:receive:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_connect_packet_receive_total),
            HUMAN_READABLE((double)st->count_connect_packet_receive_total / time_delta_total),
            HUMAN_READABLE((double)st->count_connect_byte_receive_total),
            HUMAN_READABLE((double)st->count_connect_byte_receive_total / time_delta_total));
    DEBUG(debug_level, DEBUG_LEVEL_INFO, "connect:send:packets: " HRF " (" HRF "/s), connect:send:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_connect_packet_send_total),
            HUMAN_READABLE((double)st->count_connect_packet_send_total / time_delta_total),
            HUMAN_READABLE((double)st->count_connect_byte_send_total),
            HUMAN_READABLE((double)st->count_connect_byte_send_total / time_delta_total));

    st->count_listen_packet_receive = st->count_listen_byte_receive = \
        st->count_listen_packet_send = st->count_listen_byte_send = \
        st->count_connect_packet_receive = st->count_connect_byte_receive = \
        st->count_connect_packet_send = st->count_connect_byte_send = 0;
}
