#ifndef __SETTINGS_H

#define __SETTINGS_H

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

void settings_initialize(struct settings *s);
void usage(const char *argv0, const char *message);

#endif
