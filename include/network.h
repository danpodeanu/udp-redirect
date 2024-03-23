#ifndef __NETWORK_H

#define __NETWORK_H

#include <sys/socket.h>

int socket_setup(const int debug_level, const char *desc, const char *xaddr, const int xport, const char *xif, struct sockaddr_in *xsock_name);
char *resolve_host(int debug_level, const char *host);

#endif
