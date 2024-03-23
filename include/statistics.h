#ifndef __STATISTICS_H

#define __STATISTICS_H

#include <time.h>

/**
 * The delay in seconds between displaying statistics
 */
#define STATISTICS_DELAY_SECONDS    60

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
 * Store and display stats
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

void statistics_initialize(struct statistics *st);
double int_to_human_value(double value);
char int_to_human_char(double value);
void statistics_display(int debug_level, struct statistics *st, time_t now);

#endif
