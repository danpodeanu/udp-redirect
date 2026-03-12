/**
 * @file udp-redirect-test.c
 * @brief Unit tests for udp-redirect.c helper functions.
 *
 * Technique: rename main() in the source file via a preprocessor define before
 * including it.  This gives this translation unit full access to every static
 * helper without requiring any changes to udp-redirect.c.
 *
 * Build: gcc udp-redirect-test.c -o udp-redirect-test -Wall -O0 -lm
 * Run:   ./udp-redirect-test
 */

#define main udp_redirect_main
#include "udp-redirect.c"
#undef main

#include <math.h>   /* fabs */
#include <stdio.h>
#include <string.h>

/* ---- Minimal test framework -------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

static void check(int cond, const char *name) {
    if (cond) {
        printf("PASS: %s\n", name);
        g_pass++;
    } else {
        printf("FAIL: %s\n", name);
        g_fail++;
    }
}

#define CHECK_DOUBLE(a, b, name) \
    check(fabs((double)(a) - (double)(b)) < 0.0001, (name))

/* ---- Tests --------------------------------------------------------------- */

static void test_parse_addr(void) {
    struct sockaddr_storage ss;
    const struct sockaddr_in  *a4;
    const struct sockaddr_in6 *a6;

    printf("\n=== parse_addr ===\n");

    /* Valid IPv4 */
    check(parse_addr("127.0.0.1", 1234, &ss) == AF_INET,
          "parse_addr: IPv4 loopback returns AF_INET");
    check(ss.ss_family == AF_INET,
          "parse_addr: IPv4 sets ss_family");
    a4 = (const struct sockaddr_in *)&ss;
    check(ntohs(a4->sin_port) == 1234,
          "parse_addr: IPv4 port stored correctly");

    /* Valid IPv6 */
    check(parse_addr("::1", 5678, &ss) == AF_INET6,
          "parse_addr: IPv6 loopback returns AF_INET6");
    check(ss.ss_family == AF_INET6,
          "parse_addr: IPv6 sets ss_family");
    a6 = (const struct sockaddr_in6 *)&ss;
    check(ntohs(a6->sin6_port) == 5678,
          "parse_addr: IPv6 port stored correctly");

    /* 255.255.255.255 — was incorrectly rejected by the old inet_addr() path */
    check(parse_addr("255.255.255.255", 80, &ss) == AF_INET,
          "parse_addr: 255.255.255.255 accepted (not confused with INADDR_NONE)");

    /* Full IPv6 address */
    check(parse_addr("2001:db8::1", 443, &ss) == AF_INET6,
          "parse_addr: full IPv6 address accepted");

    /* All-zeros compressed form */
    check(parse_addr("::", 80, &ss) == AF_INET6,
          "parse_addr: :: (all-zeros) accepted");
    a6 = (const struct sockaddr_in6 *)&ss;
    check(ntohs(a6->sin6_port) == 80,
          "parse_addr: :: port stored correctly");

    /* Link-local unicast */
    check(parse_addr("fe80::1", 1234, &ss) == AF_INET6,
          "parse_addr: fe80::1 link-local accepted");

    /* IPv4-mapped IPv6 address */
    check(parse_addr("::ffff:192.0.2.1", 443, &ss) == AF_INET6,
          "parse_addr: ::ffff:192.0.2.1 IPv4-mapped accepted");

    /* Fully-expanded loopback */
    check(parse_addr("0:0:0:0:0:0:0:1", 8080, &ss) == AF_INET6,
          "parse_addr: fully-expanded 0:0:0:0:0:0:0:1 accepted");
    a6 = (const struct sockaddr_in6 *)&ss;
    check(ntohs(a6->sin6_port) == 8080,
          "parse_addr: fully-expanded loopback port stored correctly");

    /* Invalid IPv6: bad hex digit */
    check(parse_addr("gggg::1", 80, &ss) == -1,
          "parse_addr: gggg::1 invalid hex digit rejected");

    /* Invalid IPv6: too many groups */
    check(parse_addr("1:2:3:4:5:6:7:8:9", 80, &ss) == -1,
          "parse_addr: 1:2:3:4:5:6:7:8:9 too many groups rejected");

    /* Invalid inputs */
    check(parse_addr("not.an.address", 80, &ss) == -1,
          "parse_addr: hostname string rejected");
    check(parse_addr("999.0.0.1", 80, &ss) == -1,
          "parse_addr: out-of-range octet rejected");
    check(parse_addr("1.2.3.4.5", 80, &ss) == -1,
          "parse_addr: five-octet string rejected");
    check(parse_addr("", 80, &ss) == -1,
          "parse_addr: empty string rejected");
    /* IPv6 with zone ID — inet_pton does not accept the '%' scope syntax */
    check(parse_addr("::1%eth0", 80, &ss) == -1,
          "parse_addr: IPv6 zone ID rejected");

    /* Port 0 */
    check(parse_addr("10.0.0.1", 0, &ss) == AF_INET,
          "parse_addr: port 0 accepted");
    a4 = (const struct sockaddr_in *)&ss;
    check(ntohs(a4->sin_port) == 0,
          "parse_addr: port 0 stored correctly");
}

static void test_addr_tostring(void) {
    struct sockaddr_storage ss;
    char buf[INET6_ADDRSTRLEN];

    printf("\n=== addr_tostring ===\n");

    parse_addr("192.168.1.1", 0, &ss);
    check(strcmp(addr_tostring(&ss, buf, sizeof(buf)), "192.168.1.1") == 0,
          "addr_tostring: IPv4 round-trips correctly");

    parse_addr("::1", 0, &ss);
    check(strcmp(addr_tostring(&ss, buf, sizeof(buf)), "::1") == 0,
          "addr_tostring: IPv6 loopback round-trips correctly");

    parse_addr("255.255.255.255", 0, &ss);
    check(strcmp(addr_tostring(&ss, buf, sizeof(buf)), "255.255.255.255") == 0,
          "addr_tostring: broadcast round-trips correctly");

    /* Unset (ss_family == 0) — inet_ntop rejects AF_UNSPEC, returns "?" */
    memset(&ss, 0, sizeof(ss));
    check(strcmp(addr_tostring(&ss, buf, sizeof(buf)), "?") == 0,
          "addr_tostring: unset address returns '?'");

    /* Buffer too small — inet_ntop returns NULL, function returns "?" */
    parse_addr("192.168.1.1", 0, &ss);
    {
        char small[3]; /* far too small for a dotted-quad */
        check(strcmp(addr_tostring(&ss, small, sizeof(small)), "?") == 0,
              "addr_tostring: buffer too small returns '?'");
    }
}

static void test_addr_port(void) {
    struct sockaddr_storage ss;

    printf("\n=== addr_port ===\n");

    parse_addr("10.0.0.1", 443, &ss);
    check(addr_port(&ss) == 443, "addr_port: IPv4 port 443");

    parse_addr("::1", 8080, &ss);
    check(addr_port(&ss) == 8080, "addr_port: IPv6 port 8080");

    parse_addr("1.2.3.4", 0, &ss);
    check(addr_port(&ss) == 0, "addr_port: port 0");

    parse_addr("1.2.3.4", 65535, &ss);
    check(addr_port(&ss) == 65535, "addr_port: max port 65535");
}

static void test_addr_len(void) {
    struct sockaddr_storage ss;

    printf("\n=== addr_len ===\n");

    parse_addr("1.2.3.4", 0, &ss);
    check(addr_len(&ss) == sizeof(struct sockaddr_in),
          "addr_len: IPv4 returns sizeof(sockaddr_in)");

    parse_addr("::1", 0, &ss);
    check(addr_len(&ss) == sizeof(struct sockaddr_in6),
          "addr_len: IPv6 returns sizeof(sockaddr_in6)");
}

static void test_addr_is_unset(void) {
    struct sockaddr_storage ss;

    printf("\n=== addr_is_unset ===\n");

    memset(&ss, 0, sizeof(ss));
    check(addr_is_unset(&ss) == 1,
          "addr_is_unset: zero-initialised storage is unset");

    parse_addr("127.0.0.1", 1234, &ss);
    check(addr_is_unset(&ss) == 0,
          "addr_is_unset: IPv4 address is not unset");

    parse_addr("::1", 1234, &ss);
    check(addr_is_unset(&ss) == 0,
          "addr_is_unset: IPv6 address is not unset");
}

static void test_addr_equal(void) {
    struct sockaddr_storage a, b;

    printf("\n=== addr_equal ===\n");

    /* Same IPv4 address and port */
    parse_addr("10.0.0.1", 1234, &a);
    parse_addr("10.0.0.1", 1234, &b);
    check(addr_equal(&a, &b) == 1,
          "addr_equal: identical IPv4 addr+port");

    /* Different port */
    parse_addr("10.0.0.1", 9999, &b);
    check(addr_equal(&a, &b) == 0,
          "addr_equal: same IPv4 addr, different port");

    /* Different address */
    parse_addr("10.0.0.2", 1234, &b);
    check(addr_equal(&a, &b) == 0,
          "addr_equal: different IPv4 address");

    /* Same IPv6 */
    parse_addr("::1", 999, &a);
    parse_addr("::1", 999, &b);
    check(addr_equal(&a, &b) == 1,
          "addr_equal: identical IPv6 addr+port");

    /* IPv6 different port */
    parse_addr("::1", 1000, &b);
    check(addr_equal(&a, &b) == 0,
          "addr_equal: same IPv6 addr, different port");

    /* IPv6 different address */
    parse_addr("::2", 999, &b);
    check(addr_equal(&a, &b) == 0,
          "addr_equal: different IPv6 address");

    /* Different families — IPv4 vs IPv6 */
    parse_addr("127.0.0.1", 1234, &a);
    parse_addr("::1",       1234, &b);
    check(addr_equal(&a, &b) == 0,
          "addr_equal: IPv4 vs IPv6 not equal");

    /* Reflexivity */
    parse_addr("192.168.0.1", 80, &a);
    check(addr_equal(&a, &a) == 1,
          "addr_equal: reflexive");
}

static void test_int_to_human(void) {
    printf("\n=== int_to_human_value / int_to_human_char ===\n");

    CHECK_DOUBLE(int_to_human_value(0),       0.0,   "int_to_human_value: 0");
    CHECK_DOUBLE(int_to_human_value(999),    999.0,   "int_to_human_value: 999");
    CHECK_DOUBLE(int_to_human_value(1000),     1.0,   "int_to_human_value: 1000 -> 1.0");
    CHECK_DOUBLE(int_to_human_value(1500),     1.5,   "int_to_human_value: 1500 -> 1.5");
    CHECK_DOUBLE(int_to_human_value(1000000),  1.0,   "int_to_human_value: 1e6 -> 1.0");
    CHECK_DOUBLE(int_to_human_value(1500000),  1.5,   "int_to_human_value: 1.5e6 -> 1.5");
    CHECK_DOUBLE(int_to_human_value(1e9),      1.0,   "int_to_human_value: 1e9 -> 1.0");

    check(int_to_human_char(0)       == ' ', "int_to_human_char: 0 -> ' '");
    check(int_to_human_char(999)     == ' ', "int_to_human_char: 999 -> ' '");
    check(int_to_human_char(1000)    == 'K', "int_to_human_char: 1000 -> 'K'");
    check(int_to_human_char(1500)    == 'K', "int_to_human_char: 1500 -> 'K'");
    check(int_to_human_char(1000000) == 'M', "int_to_human_char: 1e6 -> 'M'");
    check(int_to_human_char(1e9)     == 'G', "int_to_human_char: 1e9 -> 'G'");
    check(int_to_human_char(1e12)    == 'T', "int_to_human_char: 1e12 -> 'T'");
    check(int_to_human_char(1e15)    == 'P', "int_to_human_char: 1e15 -> 'P'");
    check(int_to_human_char(1e18)    == 'E', "int_to_human_char: 1e18 -> 'E' (max prefix)");

    /* Values beyond the max prefix (E) must not overflow the suffix array;
     * they clamp at 'E' with a value > 1000. */
    check(int_to_human_char(1e21)    == 'E', "int_to_human_char: 1e21 clamps at 'E'");
    check(int_to_human_value(1e21)   >= 1000, "int_to_human_value: 1e21 stays >= 1000 at max prefix (not further divided)");

    /* Negative values: loop condition is >= 1000, so negatives pass straight
     * through without scaling. */
    check(int_to_human_char(-1)      == ' ', "int_to_human_char: negative -> ' '");
    CHECK_DOUBLE(int_to_human_value(-500), -500.0, "int_to_human_value: negative unscaled");

    /* value and char must agree for the same input */
    check(int_to_human_char(1000) == 'K' &&
          fabs(int_to_human_value(1000) - 1.0) < 0.0001,
          "int_to_human: value and char agree at 1000 boundary");
    check(int_to_human_char(1000000) == 'M' &&
          fabs(int_to_human_value(1000000) - 1.0) < 0.0001,
          "int_to_human: value and char agree at 1e6 boundary");
}

static void test_settings_initialize(void) {
    struct settings s;

    printf("\n=== settings_initialize ===\n");

    settings_initialize(&s);
    check(s.laddr  == NULL, "settings_initialize: laddr NULL");
    check(s.lport  == 0,    "settings_initialize: lport 0");
    check(s.lif    == NULL, "settings_initialize: lif NULL");
    check(s.caddr  == NULL, "settings_initialize: caddr NULL");
    check(s.chost  == NULL, "settings_initialize: chost NULL");
    check(s.cport  == 0,    "settings_initialize: cport 0");
    check(s.saddr  == NULL, "settings_initialize: saddr NULL");
    check(s.sport  == 0,    "settings_initialize: sport 0");
    check(s.sif    == NULL, "settings_initialize: sif NULL");
    check(s.lstrict == 0,   "settings_initialize: lstrict 0");
    check(s.cstrict == 0,   "settings_initialize: cstrict 0");
    check(s.lsaddr == NULL, "settings_initialize: lsaddr NULL");
    check(s.lsport == 0,    "settings_initialize: lsport 0");
    check(s.eignore == 1,   "settings_initialize: eignore defaults to 1 (ignore errors)");
    check(s.stats  == 0,    "settings_initialize: stats 0");
}

static void test_statistics_initialize(void) {
    struct statistics st;

    printf("\n=== statistics_initialize ===\n");

    statistics_initialize(&st);
    check(st.count_listen_packet_receive  == 0, "statistics_initialize: listen rx packets 0");
    check(st.count_listen_byte_receive    == 0, "statistics_initialize: listen rx bytes 0");
    check(st.count_listen_packet_send     == 0, "statistics_initialize: listen tx packets 0");
    check(st.count_listen_byte_send       == 0, "statistics_initialize: listen tx bytes 0");
    check(st.count_connect_packet_receive == 0, "statistics_initialize: connect rx packets 0");
    check(st.count_connect_byte_receive   == 0, "statistics_initialize: connect rx bytes 0");
    check(st.count_connect_packet_send    == 0, "statistics_initialize: connect tx packets 0");
    check(st.count_connect_byte_send      == 0, "statistics_initialize: connect tx bytes 0");

    check(st.count_listen_packet_receive_total  == 0, "statistics_initialize: listen rx packets total 0");
    check(st.count_connect_byte_send_total      == 0, "statistics_initialize: connect tx bytes total 0");
}

static void test_statistics_display(void) {
    struct statistics st;
    time_t t0;
    /* statistics_display() writes to stderr via DEBUG().  Redirect stderr to
     * /dev/null for this test so the output does not clutter test results. */
    FILE *old_stderr = stderr;
    stderr = fopen("/dev/null", "w");
    if (!stderr) stderr = old_stderr; /* fallback if /dev/null unavailable */

    printf("\n=== statistics_display ===\n");

    /* --- window counters accumulate into totals, then reset to 0 --- */
    statistics_initialize(&st);
    t0 = time(NULL);
    st.time_display_first = t0 - 120; /* started 2 minutes ago */
    st.time_display_last  = t0 - 60;  /* last display 60 s ago  */

    st.count_listen_packet_receive  = 100;
    st.count_listen_byte_receive    = 50000;
    st.count_listen_packet_send     = 90;
    st.count_listen_byte_send       = 45000;
    st.count_connect_packet_receive = 80;
    st.count_connect_byte_receive   = 40000;
    st.count_connect_packet_send    = 70;
    st.count_connect_byte_send      = 35000;

    statistics_display(DEBUG_LEVEL_ERROR, &st, t0); /* suppress stderr output */

    check(st.count_listen_packet_receive_total  == 100,  "statistics_display: listen rx pkts accumulated to total");
    check(st.count_listen_byte_receive_total    == 50000, "statistics_display: listen rx bytes accumulated to total");
    check(st.count_listen_packet_send_total     == 90,   "statistics_display: listen tx pkts accumulated to total");
    check(st.count_listen_byte_send_total       == 45000, "statistics_display: listen tx bytes accumulated to total");
    check(st.count_connect_packet_receive_total == 80,   "statistics_display: connect rx pkts accumulated to total");
    check(st.count_connect_byte_receive_total   == 40000, "statistics_display: connect rx bytes accumulated to total");
    check(st.count_connect_packet_send_total    == 70,   "statistics_display: connect tx pkts accumulated to total");
    check(st.count_connect_byte_send_total      == 35000, "statistics_display: connect tx bytes accumulated to total");

    /* window counters must be zeroed after display */
    check(st.count_listen_packet_receive  == 0, "statistics_display: listen rx pkts window reset");
    check(st.count_listen_byte_receive    == 0, "statistics_display: listen rx bytes window reset");
    check(st.count_listen_packet_send     == 0, "statistics_display: listen tx pkts window reset");
    check(st.count_listen_byte_send       == 0, "statistics_display: listen tx bytes window reset");
    check(st.count_connect_packet_receive == 0, "statistics_display: connect rx pkts window reset");
    check(st.count_connect_byte_receive   == 0, "statistics_display: connect rx bytes window reset");
    check(st.count_connect_packet_send    == 0, "statistics_display: connect tx pkts window reset");
    check(st.count_connect_byte_send      == 0, "statistics_display: connect tx bytes window reset");

    /* --- totals accumulate across multiple calls --- */
    st.count_listen_packet_receive = 50;
    st.count_listen_byte_receive   = 25000;
    statistics_display(DEBUG_LEVEL_ERROR, &st, t0 + 60);
    check(st.count_listen_packet_receive_total == 150,   "statistics_display: totals accumulate across calls");
    check(st.count_listen_byte_receive_total   == 75000, "statistics_display: byte totals accumulate across calls");

    /* --- time_delta < 1 is clamped to 1 (no division anomaly) ---
     * Pass now == time_display_last so raw delta is 0; must not crash or
     * produce an obviously wrong output (we just verify it doesn't abort). */
    statistics_initialize(&st);
    t0 = time(NULL);
    st.time_display_first = t0;
    st.time_display_last  = t0;
    st.count_listen_packet_receive = 10;
    statistics_display(DEBUG_LEVEL_ERROR, &st, t0); /* delta == 0 -> clamped to 1 */
    check(st.count_listen_packet_receive_total == 10,
          "statistics_display: zero time_delta clamped to 1 (no crash)");

    /* --- debug_level clamped locally, caller's copy must be unchanged ---
     * Pass ERROR level; function must bump it internally to show stats but
     * must not write back to the caller's variable. */
    {
        int dl = DEBUG_LEVEL_ERROR;
        statistics_initialize(&st);
        t0 = time(NULL);
        st.time_display_first = t0 - 60;
        st.time_display_last  = t0 - 60;
        statistics_display(dl, &st, t0);
        check(dl == DEBUG_LEVEL_ERROR,
              "statistics_display: caller debug_level not mutated");
    }

    if (stderr != old_stderr) { fclose(stderr); }
    stderr = old_stderr;
}

/* ---- Entry point --------------------------------------------------------- */

int main(void) {
    test_parse_addr();
    test_addr_tostring();
    test_addr_port();
    test_addr_len();
    test_addr_is_unset();
    test_addr_equal();
    test_int_to_human();
    test_settings_initialize();
    test_statistics_initialize();
    test_statistics_display();

    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
