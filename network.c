#include <stdio.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "include/debug.h"
#include "include/network.h"

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
