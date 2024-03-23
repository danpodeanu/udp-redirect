#include <stdio.h>

#include "include/statistics.h"
#include "include/debug.h"

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
 * Convert a value to human readable (i.e., 1500 = 1.5K). Divide by 1000, not 1024.
 * @param[in] value The value to be converted
 * @param[in] host The host to resolve
 * @return The numeric portion of the human readable value.
 */
double int_to_human_value(double value) {
    double dvalue = value;
    int count = 0;

    while (dvalue > 1000 && count < (HUMAN_READABLE_SIZES_COUNT - 1)) {
        dvalue = dvalue / 1000;
        count = count + 1;
    }

    return dvalue;
}

/**
 * Convert a value to human readable (i.e., 1500 = 1.5K). Divide by 1000, not 1024.
 * @param[in] value The value to be converted
 * @param[in] host The host to resolve
 * @return The character (K, M, G, etc.) portion of the human readable value.
 */
char int_to_human_char(double value) {
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
 * Display the stored statistics
 * @param[in] debug_level The debug level to be used for the DEBUG() macro
 * @param[in] st The statistics structure
 * @param[in] now The current time
 */
void statistics_display(int debug_level, struct statistics *st, time_t now) {
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

    DEBUG(DEBUG_LEVEL_INFO, "---- STATS %ds ----", STATISTICS_DELAY_SECONDS);

    DEBUG(DEBUG_LEVEL_INFO, "listen:receive:packets: " HRF " (" HRF "/s), listen:receive:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_listen_packet_receive),
            HUMAN_READABLE((double)st->count_listen_packet_receive / time_delta),
            HUMAN_READABLE((double)st->count_listen_byte_receive),
            HUMAN_READABLE((double)st->count_listen_byte_receive / time_delta));
    DEBUG(DEBUG_LEVEL_INFO, "listen:send:packets: " HRF " (" HRF "/s), listen:send:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_listen_packet_send),
            HUMAN_READABLE((double)st->count_listen_packet_send / time_delta),
            HUMAN_READABLE((double)st->count_listen_byte_send),
            HUMAN_READABLE((double)st->count_listen_byte_send / time_delta));
    DEBUG(DEBUG_LEVEL_INFO, "connect:receive:packets: " HRF " (" HRF "/s), connect:receive:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_connect_packet_receive),
            HUMAN_READABLE((double)st->count_connect_packet_receive / time_delta),
            HUMAN_READABLE((double)st->count_connect_byte_receive),
            HUMAN_READABLE((double)st->count_connect_byte_receive / time_delta));
    DEBUG(DEBUG_LEVEL_INFO, "connect:send:packets: " HRF " (" HRF "/s), connect:send:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_connect_packet_send),
            HUMAN_READABLE((double)st->count_connect_packet_send / time_delta),
            HUMAN_READABLE((double)st->count_connect_byte_send),
            HUMAN_READABLE((double)st->count_connect_byte_send / time_delta));

    DEBUG(DEBUG_LEVEL_INFO, "---- STATS TOTAL ----");

    DEBUG(DEBUG_LEVEL_INFO, "listen:receive:packets: " HRF " (" HRF "/s), listen:receive:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_listen_packet_receive_total),
            HUMAN_READABLE((double)st->count_listen_packet_receive_total / time_delta_total),
            HUMAN_READABLE((double)st->count_listen_byte_receive_total),
            HUMAN_READABLE((double)st->count_listen_byte_receive_total / time_delta_total));
    DEBUG(DEBUG_LEVEL_INFO, "listen:send:packets: " HRF " (" HRF "/s), listen:send:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_listen_packet_send_total),
            HUMAN_READABLE((double)st->count_listen_packet_send_total / time_delta_total),
            HUMAN_READABLE((double)st->count_listen_byte_send_total),
            HUMAN_READABLE((double)st->count_listen_byte_send_total / time_delta_total));
    DEBUG(DEBUG_LEVEL_INFO, "connect:receive:packets: " HRF " (" HRF "/s), connect:receive:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_connect_packet_receive_total),
            HUMAN_READABLE((double)st->count_connect_packet_receive_total / time_delta_total),
            HUMAN_READABLE((double)st->count_connect_byte_receive_total),
            HUMAN_READABLE((double)st->count_connect_byte_receive_total / time_delta_total));
    DEBUG(DEBUG_LEVEL_INFO, "connect:send:packets: " HRF " (" HRF "/s), connect:send:bytes: " HRF " (" HRF "/s)",
            HUMAN_READABLE((double)st->count_connect_packet_send_total),
            HUMAN_READABLE((double)st->count_connect_packet_send_total / time_delta_total),
            HUMAN_READABLE((double)st->count_connect_byte_send_total),
            HUMAN_READABLE((double)st->count_connect_byte_send_total / time_delta_total));

    st->count_listen_packet_receive = st->count_listen_byte_receive = \
        st->count_listen_packet_send = st->count_listen_byte_send = \
        st->count_connect_packet_receive = st->count_connect_byte_receive = \
        st->count_connect_packet_send = st->count_connect_byte_send = 0;
}
