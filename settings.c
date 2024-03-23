#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "include/settings.h"

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
