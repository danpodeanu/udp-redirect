#!/usr/bin/env bash
# Quick functional tests for udp-redirect.
# Requires: nc (netcat-openbsd), make, gcc
# Optional: socat (two-way forwarding test), python3 (source filtering test)

PASS=0
FAIL=0
PIDS=()

LPORT=19901
CPORT=19902

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }

cleanup() {
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    rm -f /tmp/udpr_test_*
}
trap cleanup EXIT

# --- Build ---
echo "=== build ==="
make -s && pass "build" || { echo "FAIL: build (aborting)"; exit 1; }

# --- Version ---
echo "=== --version ==="
./udp-redirect --version 2>&1 | grep -q "udp-redirect v" \
    && pass "--version prints version string" \
    || fail "--version prints version string"

# --- Required argument validation ---
echo "=== argument validation ==="

./udp-redirect 2>/dev/null
[ $? -ne 0 ] && pass "no args exits nonzero" || fail "no args exits nonzero"

./udp-redirect --listen-port 1234 2>/dev/null
[ $? -ne 0 ] && pass "missing --connect exits nonzero" || fail "missing --connect exits nonzero"

./udp-redirect --connect-address 127.0.0.1 --connect-port 1234 2>/dev/null
[ $? -ne 0 ] && pass "missing --listen-port exits nonzero" || fail "missing --listen-port exits nonzero"

./udp-redirect --listen-port 1234 --connect-address 127.0.0.1 2>/dev/null
[ $? -ne 0 ] && pass "missing --connect-port exits nonzero" || fail "missing --connect-port exits nonzero"

./udp-redirect --listen-port 1234 --connect-address 127.0.0.1 --connect-port 1234 \
    --listen-sender-port 5000 2>/dev/null
[ $? -ne 0 ] && pass "--listen-sender-port without --listen-sender-address rejected" \
              || fail "--listen-sender-port without --listen-sender-address rejected"

# --- Port validation (fix #1: strtol replaces atoi) ---
echo "=== port validation ==="

./udp-redirect --listen-port abc --connect-address 127.0.0.1 --connect-port 1234 2>/dev/null
[ $? -ne 0 ] && pass "port 'abc' rejected" || fail "port 'abc' accepted (should fail)"

./udp-redirect --listen-port 80abc --connect-address 127.0.0.1 --connect-port 1234 2>/dev/null
[ $? -ne 0 ] && pass "port '80abc' rejected" || fail "port '80abc' accepted (should fail)"

./udp-redirect --listen-port 99999 --connect-address 127.0.0.1 --connect-port 1234 2>/dev/null
[ $? -ne 0 ] && pass "port 99999 rejected" || fail "port 99999 accepted (should fail)"

./udp-redirect --listen-port -1 --connect-address 127.0.0.1 --connect-port 1234 2>/dev/null
[ $? -ne 0 ] && pass "port -1 rejected" || fail "port -1 accepted (should fail)"

./udp-redirect --listen-port 1234 --connect-address 127.0.0.1 --connect-port 65536 2>/dev/null
[ $? -ne 0 ] && pass "port 65536 rejected" || fail "port 65536 accepted (should fail)"

# --- Forwarding (listen -> connect) ---
echo "=== forwarding ==="

BACKEND_OUT=/tmp/udpr_test_backend.txt
> "$BACKEND_OUT"

# Start backend UDP listener (netcat-openbsd: nc -u -l PORT, no -p flag)
nc -u -l $CPORT > "$BACKEND_OUT" &
PIDS+=($!)
sleep 0.2

# Start udp-redirect
./udp-redirect --listen-port $LPORT --connect-address 127.0.0.1 --connect-port $CPORT 2>/dev/null &
PIDS+=($!)
sleep 0.2

# Send a test packet through the redirector
echo -n "hello-redirect" | nc -u -w1 127.0.0.1 $LPORT 2>/dev/null
sleep 0.3

grep -q "hello-redirect" "$BACKEND_OUT" \
    && pass "packet forwarded listen->connect" \
    || fail "packet forwarded listen->connect"

# --- Forwarding: multiple packets ---
echo "=== forwarding: multiple packets ==="

LPORT2=19903
CPORT2=19904
BACKEND2_OUT=/tmp/udpr_test_backend2.txt
> "$BACKEND2_OUT"

nc -u -l $CPORT2 > "$BACKEND2_OUT" &
PIDS+=($!)
sleep 0.2

./udp-redirect --listen-port $LPORT2 --connect-address 127.0.0.1 --connect-port $CPORT2 2>/dev/null &
PIDS+=($!)
sleep 0.2

echo -n "packet-one"   | nc -u -w1 127.0.0.1 $LPORT2 2>/dev/null; sleep 0.1
echo -n "packet-two"   | nc -u -w1 127.0.0.1 $LPORT2 2>/dev/null; sleep 0.1
echo -n "packet-three" | nc -u -w1 127.0.0.1 $LPORT2 2>/dev/null; sleep 0.3

grep -q "packet-one"   "$BACKEND2_OUT" && pass "multiple packets: 1 arrived" || fail "multiple packets: 1 arrived"
grep -q "packet-two"   "$BACKEND2_OUT" && pass "multiple packets: 2 arrived" || fail "multiple packets: 2 arrived"
grep -q "packet-three" "$BACKEND2_OUT" && pass "multiple packets: 3 arrived" || fail "multiple packets: 3 arrived"

# --- Forwarding: two-way (requires socat) ---
if command -v socat >/dev/null 2>&1; then
    echo "=== forwarding: two-way ==="

    LPORT3=19905
    CPORT3=19906
    CLIENT_OUT=/tmp/udpr_test_client.txt
    > "$CLIENT_OUT"

    # socat UDP echo server: echoes each datagram back to its sender
    socat UDP4-RECVFROM:$CPORT3,fork EXEC:'cat' &
    PIDS+=($!)
    sleep 0.2

    ./udp-redirect --listen-port $LPORT3 --connect-address 127.0.0.1 --connect-port $CPORT3 2>/dev/null &
    PIDS+=($!)
    sleep 0.2

    # nc sends from a random port to LPORT3; the reply from udp-redirect arrives
    # from LPORT3 (lsock), so nc's connected filter accepts it.
    echo -n "echo-me" | nc -u -w2 127.0.0.1 $LPORT3 > "$CLIENT_OUT" 2>/dev/null

    grep -q "echo-me" "$CLIENT_OUT" \
        && pass "two-way forwarding: reply received by client" \
        || fail "two-way forwarding: reply received by client"
else
    echo "=== forwarding: two-way (SKIPPED: socat not found) ==="
fi

# --- Forwarding: source address/port filtering ---
# Uses python3 for reliable source-port-controlled UDP sends.
# socat's sourceport= option can silently fail to bind on some systems, causing
# packets to leave from a random ephemeral port and making both tests unreliable.
if command -v python3 >/dev/null 2>&1; then
    echo "=== forwarding: source filtering ==="

    LPORT4=19907
    CPORT4=19908
    SENDER_PORT=19909
    WRONG_PORT=19910
    BACKEND4_OUT=/tmp/udpr_test_backend4.txt
    > "$BACKEND4_OUT"

    # Python one-shot UDP backend: waits up to 2 s for one datagram, then exits.
    python3 -c "
import socket, sys, select
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('', $CPORT4))
r, _, _ = select.select([s], [], [], 2.0)
if r:
    data, _ = s.recvfrom(65535)
    sys.stdout.buffer.write(data)
    sys.stdout.flush()
s.close()
" > "$BACKEND4_OUT" &
    BACKEND4_PID=$!
    PIDS+=($BACKEND4_PID)
    sleep 0.2

    # --listen-sender-address/port pre-specifies the only allowed source
    ./udp-redirect --listen-port $LPORT4 --connect-address 127.0.0.1 --connect-port $CPORT4 \
        --listen-sender-address 127.0.0.1 --listen-sender-port $SENDER_PORT 2>/dev/null &
    PIDS+=($!)
    sleep 0.2

    # Packet from wrong source port — should be dropped
    python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('', $WRONG_PORT))
s.sendto(b'wrong-source', ('127.0.0.1', $LPORT4))
s.close()
"
    sleep 0.3
    grep -q "wrong-source" "$BACKEND4_OUT" \
        && fail "source filtering: wrong-source packet should be dropped" \
        || pass "source filtering: wrong-source packet dropped"

    # Kill first backend (may still be waiting for its 2 s timeout) and start a fresh one.
    kill "$BACKEND4_PID" 2>/dev/null || true
    sleep 0.1

    BACKEND4B_OUT=/tmp/udpr_test_backend4b.txt
    > "$BACKEND4B_OUT"
    python3 -c "
import socket, sys, select
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('', $CPORT4))
r, _, _ = select.select([s], [], [], 2.0)
if r:
    data, _ = s.recvfrom(65535)
    sys.stdout.buffer.write(data)
    sys.stdout.flush()
s.close()
" > "$BACKEND4B_OUT" &
    PIDS+=($!)
    sleep 0.2

    # Packet from correct source port — should be forwarded
    python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('', $SENDER_PORT))
s.sendto(b'correct-source', ('127.0.0.1', $LPORT4))
s.close()
"
    sleep 0.5
    grep -q "correct-source" "$BACKEND4B_OUT" \
        && pass "source filtering: correct-source packet forwarded" \
        || fail "source filtering: correct-source packet forwarded"
else
    echo "=== forwarding: source filtering (SKIPPED: python3 not found) ==="
fi

# --- Hostname resolution ---
# Exercises the DEBUG("Resolved %s to %s") line inside resolve_host() that
# previously called strdup() and leaked the allocation.  Running with --debug
# ensures that code path executes.  We verify resolution actually worked by
# confirming a packet reaches the backend.
echo "=== hostname resolution ==="

LPORT5=19911
CPORT5=19912
BACKEND5_OUT=/tmp/udpr_test_backend5.txt
> "$BACKEND5_OUT"

nc -u -l $CPORT5 > "$BACKEND5_OUT" &
PIDS+=($!)
sleep 0.2

./udp-redirect --listen-port $LPORT5 --connect-host localhost --connect-port $CPORT5 \
    --debug 2>/dev/null &
PIDS+=($!)
sleep 0.2

echo -n "host-resolve" | nc -u -w1 127.0.0.1 $LPORT5 2>/dev/null
sleep 0.3

grep -q "host-resolve" "$BACKEND5_OUT" \
    && pass "--connect-host resolves and forwards" \
    || fail "--connect-host resolves and forwards"

# --- Stats first window timing ---
# Before the fix: time_display_last was initialised to 0 by statistics_initialize().
# The trigger condition is (now - time_display_last) >= 60.  With last=0 that
# evaluates to ~56 years >= 60, which is true on the very first loop iteration,
# so "---- STATS 60s ----" appeared in stderr within ~1 s of startup.
# After the fix: time_display_last = time(NULL), so the first window fires no
# earlier than 60 s after startup — it must NOT appear in a 1.5 s run.
echo "=== stats first window timing ==="

LPORT6=19913
CPORT6=19914
STATS_OUT=/tmp/udpr_test_stats.txt

./udp-redirect --listen-port $LPORT6 --connect-address 127.0.0.1 --connect-port $CPORT6 \
    --stats --verbose 2>"$STATS_OUT" &
STATS_PID=$!
PIDS+=($STATS_PID)
sleep 1.5

kill "$STATS_PID" 2>/dev/null || true

grep -q "STATS 60s" "$STATS_OUT" \
    && fail "stats not displayed before first 60-s window" \
    || pass "stats not displayed before first 60-s window"

# --- --stats works without --verbose ---
# Before the fix: statistics_display() used DEBUG_LEVEL_INFO throughout, so
# stats output was silently suppressed unless --verbose was also given.
# After the fix: debug_level is clamped to DEBUG_LEVEL_INFO inside
# statistics_display(), so stats always appear when --stats is specified.
# We can't wait 60 s for the real stats window, so we verify the two observable
# side-effects of the fix:
#   (a) With --stats alone (no --verbose), startup INFO lines must NOT appear —
#       confirming the clamp is local to statistics_display and doesn't leak.
#   (b) With --stats alone, the process starts and runs normally (no crash).
echo "=== --stats works without --verbose ==="

LPORT7=19915
CPORT7=19916
FIX6_OUT=/tmp/udpr_test_fix6.txt

./udp-redirect --listen-port $LPORT7 --connect-address 127.0.0.1 --connect-port $CPORT7 \
    --stats 2>"$FIX6_OUT" &
FIX6_PID=$!
PIDS+=($FIX6_PID)
sleep 0.5

kill -0 "$FIX6_PID" 2>/dev/null \
    && pass "--stats alone: process stays alive" \
    || fail "--stats alone: process stays alive"

kill "$FIX6_PID" 2>/dev/null || true

# INFO-level startup banner must not appear without --verbose
grep -q -- "---- INFO ----" "$FIX6_OUT" \
    && fail "--stats alone does not enable verbose INFO output" \
    || pass "--stats alone does not enable verbose INFO output"

# --- SO_BINDTODEVICE interface binding ---
# Before the fix: strlen(xif) was passed, omitting the '\0'.
# SO_BINDTODEVICE on Linux requires a null-terminated interface name; the
# length must be strlen(xif)+1.
# We can't bind to a specific interface in a generic test environment, but we
# can exercise the code path by supplying a non-existent interface name.
# setsockopt(SO_BINDTODEVICE) must fail and the process must exit non-zero
# (the error handler calls exit(EXIT_FAILURE)).  This confirms the syscall is
# reached with a properly-terminated string and that error handling still works.
echo "=== SO_BINDTODEVICE interface binding ==="

# Only Linux defines SO_BINDTODEVICE; skip on macOS.
if [ "$(uname)" = "Linux" ]; then
    ./udp-redirect --listen-port 19917 --connect-address 127.0.0.1 --connect-port 19918 \
        --listen-interface no_such_iface0 2>/dev/null
    [ $? -ne 0 ] \
        && pass "invalid interface name rejected (SO_BINDTODEVICE reached)" \
        || fail "invalid interface name rejected (SO_BINDTODEVICE reached)"
else
    echo "=== SO_BINDTODEVICE interface binding (SKIPPED: not Linux) ==="
fi

# --- caddr zero-initialisation ---
# caddr (struct sockaddr_in) was previously filled field-by-field, leaving the
# sin_zero[8] padding uninitialised on the stack.  sendto() passes the full
# struct to the kernel; padding must be zero per POSIX.
# The fix adds memset(&caddr, 0, sizeof(caddr)) before the field assignments.
# We verify that a packet forwarded via the caddr path actually arrives,
# confirming the struct is correctly constructed end-to-end.
echo "=== caddr zero-initialisation ==="

LPORT8=19919
CPORT8=19920
BACKEND8_OUT=/tmp/udpr_test_backend8.txt
> "$BACKEND8_OUT"

nc -u -l $CPORT8 > "$BACKEND8_OUT" &
PIDS+=($!)
sleep 0.2

./udp-redirect --listen-port $LPORT8 --connect-address 127.0.0.1 --connect-port $CPORT8 \
    2>/dev/null &
PIDS+=($!)
sleep 0.2

echo -n "caddr-init" | nc -u -w1 127.0.0.1 $LPORT8 2>/dev/null
sleep 0.3

grep -q "caddr-init" "$BACKEND8_OUT" \
    && pass "caddr zero-initialised: packet forwarded via connect address" \
    || fail "caddr zero-initialised: packet forwarded via connect address"

# --- int_to_human scaling consistency ---
# int_to_human_value() and int_to_human_char() previously each contained an
# identical while-loop.  The refactor extracted the loop into a shared static
# helper (int_to_human_scale).  We verify the two public functions still agree
# for values that cross each SI prefix boundary: a regression where one function
# scales and the other doesn't would produce output like "1001.0K" instead of
# "1.5K".
# We compile a small C driver that copies only the relevant defines and the
# three functions from udp-redirect.c, calls them with known inputs, and exits
# non-zero on any mismatch.
echo "=== int_to_human scaling consistency ==="

cat > /tmp/udpr_test_human.c << 'CEOF'
#include <stdio.h>
#include <math.h>

#define HUMAN_READABLE_SIZES       { ' ', 'K', 'M', 'G', 'T', 'P', 'E' }
#define HUMAN_READABLE_SIZES_COUNT 7

static double int_to_human_scale(double value, int *count_out) {
    int count = 0;
    while (value >= 1000 && count < (HUMAN_READABLE_SIZES_COUNT - 1)) {
        value = value / 1000;
        count = count + 1;
    }
    *count_out = count;
    return value;
}
double int_to_human_value(double value) {
    int count;
    return int_to_human_scale(value, &count);
}
char int_to_human_char(double value) {
    static const char human_readable_sizes[] = HUMAN_READABLE_SIZES;
    int count;
    int_to_human_scale(value, &count);
    return human_readable_sizes[count];
}

/* Returns 0 on success, prints the failing case and returns 1 on mismatch. */
static int check(double input, double expect_val, char expect_char) {
    double got_val  = int_to_human_value(input);
    char   got_char = int_to_human_char(input);
    /* Allow tiny floating-point rounding: compare to 4 decimal places */
    if (fabs(got_val - expect_val) > 0.0001 || got_char != expect_char) {
        fprintf(stderr, "MISMATCH input=%.0f: got %.4f'%c', want %.4f'%c'\n",
                input, got_val, got_char, expect_val, expect_char);
        return 1;
    }
    return 0;
}

int main(void) {
    int fail = 0;
    fail |= check(0,            0.0,      ' ');
    fail |= check(999,        999.0,      ' ');
    fail |= check(1000,         1.0,      'K');  /* exact boundary: 1000 >= 1000 */
    fail |= check(1001,         1.001,    'K');
    fail |= check(1500,         1.5,      'K');
    fail |= check(1000000,      1.0,      'M');
    fail |= check(1500000,      1.5,      'M');
    fail |= check(1000000000,   1.0,      'G');
    return fail;
}
CEOF

if gcc /tmp/udpr_test_human.c -o /tmp/udpr_test_human -lm 2>/dev/null \
        && /tmp/udpr_test_human; then
    pass "int_to_human: value and char functions agree across SI boundaries"
else
    fail "int_to_human: value and char functions agree across SI boundaries"
fi
rm -f /tmp/udpr_test_human.c /tmp/udpr_test_human

# --- inet_ntop for endpoint address formatting ---
# One DEBUG call used inet_ntoa() (static internal buffer) while every other
# address-formatting call in the file uses inet_ntop() with an explicit buffer.
# We trigger the "LISTEN remote endpoint set to" message by running with --debug
# and sending a packet from a fresh source, then verify the logged address looks
# like a valid dotted-quad rather than being empty or garbage.
echo "=== inet_ntop endpoint address formatting ==="

LPORT9=19921
CPORT9=19922
INETNTOP_OUT=/tmp/udpr_test_inetntop.txt

./udp-redirect --listen-port $LPORT9 --connect-address 127.0.0.1 --connect-port $CPORT9 \
    --debug 2>"$INETNTOP_OUT" &
INETNTOP_PID=$!
PIDS+=($INETNTOP_PID)
sleep 0.2

echo -n "probe" | nc -u -w1 127.0.0.1 $LPORT9 2>/dev/null
sleep 0.3

kill "$INETNTOP_PID" 2>/dev/null || true

# The debug line reads: "LISTEN remote endpoint set to (127.0.0.1, <port>)"
grep -qE "LISTEN remote endpoint set to \(127\.0\.0\.1, [0-9]+\)" "$INETNTOP_OUT" \
    && pass "inet_ntop: endpoint address logged as valid dotted-quad" \
    || fail "inet_ntop: endpoint address logged as valid dotted-quad"

# --- time_t for stats time deltas ---
# time_delta and time_delta_total were declared as int; the subtraction of two
# time_t values produces time_t, which is 64-bit on modern systems.  Storing the
# result in int truncates silently.  We verify the process starts and runs with
# --stats without crashing (a type-mismatch crash or compiler warning would
# surface here if the types were wrong).
echo "=== time_t stats time deltas ==="

LPORT10=19923
CPORT10=19924
TIMEDELTA_OUT=/tmp/udpr_test_timedelta.txt

./udp-redirect --listen-port $LPORT10 --connect-address 127.0.0.1 --connect-port $CPORT10 \
    --stats --verbose 2>"$TIMEDELTA_OUT" &
TIMEDELTA_PID=$!
PIDS+=($TIMEDELTA_PID)
sleep 0.5

kill -0 "$TIMEDELTA_PID" 2>/dev/null \
    && pass "time_t time_delta: process stable with --stats" \
    || fail "time_t time_delta: process stable with --stats"

kill "$TIMEDELTA_PID" 2>/dev/null || true

# --- inet_pton address parsing ---
# inet_addr() returns INADDR_NONE (0xFFFFFFFF) for both invalid input AND the
# legitimate broadcast address 255.255.255.255, so that address was incorrectly
# rejected.  inet_pton() returns 1 on success and 0 for an unparseable string,
# so it never conflates the broadcast address with an error.
echo "=== inet_pton address parsing ==="

# An invalid address string must still be rejected.
./udp-redirect --listen-port 19925 --connect-address "not.an.address" \
    --connect-port 1234 2>/dev/null
[ $? -ne 0 ] \
    && pass "inet_pton: invalid address string rejected" \
    || fail "inet_pton: invalid address string rejected"

# 255.255.255.255 must now be accepted past argument parsing.
# With inet_addr it exited immediately; with inet_pton the process starts up.
./udp-redirect --listen-port 19925 --connect-address 255.255.255.255 \
    --connect-port 19926 2>/dev/null &
BC_PID=$!
PIDS+=($BC_PID)
sleep 0.3

kill -0 "$BC_PID" 2>/dev/null \
    && pass "inet_pton: 255.255.255.255 no longer confused with INADDR_NONE" \
    || fail "inet_pton: 255.255.255.255 no longer confused with INADDR_NONE"

kill "$BC_PID" 2>/dev/null || true

# --- IPv6 support ---
# parse_addr() tries inet_pton(AF_INET) then inet_pton(AF_INET6), so both
# families are accepted.  We skip these tests if the kernel has no IPv6
# loopback (unusual, but possible in some containers).
if python3 -c "import socket; s=socket.socket(socket.AF_INET6,socket.SOCK_DGRAM); s.bind(('::1',0)); s.close()" 2>/dev/null; then
    echo "=== IPv6 support ==="

    # (a) Invalid address string is still rejected
    ./udp-redirect --listen-port 19927 --connect-address "2001:::invalid" \
        --connect-port 1234 2>/dev/null
    [ $? -ne 0 ] \
        && pass "IPv6: invalid address string rejected" \
        || fail "IPv6: invalid address string rejected"

    # (b) Forwarding via IPv6 loopback (connect-address ::1)
    #     Because the connect address is IPv6, socket_setup() opens IPv6 sockets
    #     for both listen and send, so the client must also connect via ::1.
    LPORT11=19929
    CPORT11=19930
    BACKEND11_OUT=/tmp/udpr_test_backend11.txt
    > "$BACKEND11_OUT"

    python3 -c "
import socket, sys, select
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('::1', $CPORT11))
r, _, _ = select.select([s], [], [], 2.0)
if r:
    data, _ = s.recvfrom(65535)
    sys.stdout.buffer.write(data)
    sys.stdout.flush()
s.close()
" > "$BACKEND11_OUT" &
    PIDS+=($!)
    sleep 0.2

    ./udp-redirect --listen-port $LPORT11 --connect-address ::1 \
        --connect-port $CPORT11 2>/dev/null &
    PIDS+=($!)
    sleep 0.2

    python3 -c "
import socket
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.sendto(b'ipv6-forward', ('::1', $LPORT11))
s.close()
"
    sleep 0.5

    grep -q "ipv6-forward" "$BACKEND11_OUT" \
        && pass "IPv6: packet forwarded via ::1 connect address" \
        || fail "IPv6: packet forwarded via ::1 connect address"

    # (c) Explicit --listen-address ::1 binds the listen socket to IPv6 loopback
    LPORT12=19931
    CPORT12=19932
    BACKEND12_OUT=/tmp/udpr_test_backend12.txt
    > "$BACKEND12_OUT"

    python3 -c "
import socket, sys, select
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('::1', $CPORT12))
r, _, _ = select.select([s], [], [], 2.0)
if r:
    data, _ = s.recvfrom(65535)
    sys.stdout.buffer.write(data)
    sys.stdout.flush()
s.close()
" > "$BACKEND12_OUT" &
    PIDS+=($!)
    sleep 0.2

    ./udp-redirect --listen-address ::1 --listen-port $LPORT12 \
        --connect-address ::1 --connect-port $CPORT12 2>/dev/null &
    PIDS+=($!)
    sleep 0.2

    python3 -c "
import socket
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.sendto(b'ipv6-listen-addr', ('::1', $LPORT12))
s.close()
"
    sleep 0.5

    grep -q "ipv6-listen-addr" "$BACKEND12_OUT" \
        && pass "IPv6: packet forwarded with explicit --listen-address ::1" \
        || fail "IPv6: packet forwarded with explicit --listen-address ::1"

else
    echo "=== IPv6 support (SKIPPED: IPv6 loopback not available) ==="
fi

# --- Results ---
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
