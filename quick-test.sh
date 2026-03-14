#!/usr/bin/env bash
# Quick functional tests for udp-redirect.
# Requires: nc (netcat-openbsd), make, gcc
# Optional: socat (two-way forwarding test), python3 (source filtering / IPv6 tests)

PASS=0
FAIL=0
PIDS=()
TMPFILES=()
TMPDIR=""

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }

cleanup() {
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    for f in "${TMPFILES[@]}"; do
        rm -f "$f" 2>/dev/null || true
    done
    if [ -n "$TMPDIR" ]; then
        rm -rf "$TMPDIR" 2>/dev/null || true
    fi
}
trap cleanup EXIT

make_tmpfile() {
    local f
    f=$(mktemp "${TMPDIR}/udpr_test.XXXXXX") || exit 1
    TMPFILES+=("$f")
    echo "$f"
}

make_tmpdir() {
    local d
    d=$(mktemp -d "./udpr_test.d.XXXXXX") || exit 1
    TMPDIR="$d"
}

make_tmpdir

# --- Build C ---
echo "=== build C ==="
make -s && pass "build C" || { echo "FAIL: build C (aborting)"; exit 1; }

# --- int_to_human scaling consistency ---
echo "=== int_to_human scaling consistency ==="
HUMAN_C_FILE=$(mktemp "${TMPDIR}/udpr_test.XXXXXX.c") || exit 1
TMPFILES+=("$HUMAN_C_FILE")
HUMAN_BIN="${TMPDIR}/udpr_test_human"
HUMAN_GCC_ERR=$(make_tmpfile)
TMPFILES+=("$HUMAN_BIN")
cat > "$HUMAN_C_FILE" << 'CEOF'
#include <stdio.h>
#include <math.h>
#define HUMAN_READABLE_SIZES       { ' ', 'K', 'M', 'G', 'T', 'P', 'E' }
#define HUMAN_READABLE_SIZES_COUNT 7
static double int_to_human_scale(double value, int *count_out) {
    int count = 0;
    while (value >= 1000 && count < (HUMAN_READABLE_SIZES_COUNT - 1)) { value /= 1000; count++; }
    *count_out = count; return value;
}
double int_to_human_value(double value) { int c; return int_to_human_scale(value, &c); }
char int_to_human_char(double value) {
    static const char s[] = HUMAN_READABLE_SIZES;
    int c; int_to_human_scale(value, &c); return s[c];
}
static int check(double in, double ev, char ec) {
    double gv = int_to_human_value(in); char gc = int_to_human_char(in);
    if (fabs(gv - ev) > 0.0001 || gc != ec) {
        fprintf(stderr, "MISMATCH %.0f: got %.4f'%c', want %.4f'%c'\n", in, gv, gc, ev, ec);
        return 1;
    }
    return 0;
}
int main(void) {
    int f = 0;
    f |= check(0, 0.0, ' ');       f |= check(999, 999.0, ' ');
    f |= check(1000, 1.0, 'K');    f |= check(1001, 1.001, 'K');
    f |= check(1500, 1.5, 'K');    f |= check(1000000, 1.0, 'M');
    f |= check(1500000, 1.5, 'M'); f |= check(1000000000, 1.0, 'G');
    return f;
}
CEOF
if gcc "$HUMAN_C_FILE" -o "$HUMAN_BIN" -lm 2>"$HUMAN_GCC_ERR"; then
    chmod +x "$HUMAN_BIN" 2>/dev/null || true
    if "$HUMAN_BIN"; then
        pass "int_to_human: value and char functions agree across SI boundaries"
    else
        echo "int_to_human: executable failed with status $?"
        ls -l "$HUMAN_BIN" 2>/dev/null || true
        fail "int_to_human: value and char functions agree across SI boundaries"
    fi
else
    echo "int_to_human: gcc failed"
    if [ -s "$HUMAN_GCC_ERR" ]; then
        cat "$HUMAN_GCC_ERR"
    fi
    fail "int_to_human: value and char functions agree across SI boundaries"
fi

# ---------------------------------------------------------------------------
# run_tests BIN PORT_BASE
#
# Runs all functional tests against BIN.  All port numbers are offset by
# PORT_BASE so test runs with different binaries never collide.
# ---------------------------------------------------------------------------
run_tests() {
    local BIN="$1"
    local B="$2"
    local LABEL; LABEL=$(basename "$BIN")

    # Port allocations
    local P1=$((19901+B))  P2=$((19902+B))   # forwarding
    local P3=$((19903+B))  P4=$((19904+B))   # multiple packets
    local P5=$((19905+B))  P6=$((19906+B))   # two-way (socat)
    local P7=$((19907+B))  P8=$((19908+B))   # source filtering
    local P9=$((19909+B))  P10=$((19910+B))  # sender / wrong ports
    local P11=$((19911+B)) P12=$((19912+B))  # hostname resolution
    local P13=$((19913+B)) P14=$((19914+B))  # stats first window
    local P15=$((19915+B)) P16=$((19916+B))  # stats without --verbose
    local P17=$((19917+B)) P18=$((19918+B))  # SO_BINDTODEVICE
    local P19=$((19919+B)) P20=$((19920+B))  # caddr zero-init
    local P21=$((19921+B)) P22=$((19922+B))  # inet_ntop formatting
    local P23=$((19923+B)) P24=$((19924+B))  # time_t delta
    local P25=$((19925+B)) P26=$((19926+B))  # inet_pton / 255.255.255.255
    local P27=$((19927+B))                   # IPv6 invalid address
    local P29=$((19929+B)) P30=$((19930+B))  # IPv6 forwarding
    local P31=$((19931+B)) P32=$((19932+B))  # IPv6 --listen-address
    local P33=$((19933+B)) P34=$((19934+B))  # zero-length datagrams
    local P35=$((19935+B)) P36=$((19936+B))  # cross-family IPv4 listen -> IPv6 connect
    local P37=$((19937+B)) P38=$((19938+B))  # cross-family IPv6 listen -> IPv4 connect

    echo ""
    echo "===== $LABEL ====="

    # --- Version ---
    echo "=== $LABEL: --version ==="
    "$BIN" --version 2>&1 | grep -q "udp-redirect" \
        && pass "$LABEL: --version prints version string" \
        || fail "$LABEL: --version prints version string"

    # --- Required argument validation ---
    echo "=== $LABEL: argument validation ==="
    "$BIN" 2>/dev/null
    [ $? -ne 0 ] && pass "$LABEL: no args exits nonzero" || fail "$LABEL: no args exits nonzero"

    "$BIN" --listen-port 1234 2>/dev/null
    [ $? -ne 0 ] && pass "$LABEL: missing --connect exits nonzero" || fail "$LABEL: missing --connect exits nonzero"

    "$BIN" --connect-address 127.0.0.1 --connect-port 1234 2>/dev/null
    [ $? -ne 0 ] && pass "$LABEL: missing --listen-port exits nonzero" || fail "$LABEL: missing --listen-port exits nonzero"

    "$BIN" --listen-port 1234 --connect-address 127.0.0.1 2>/dev/null
    [ $? -ne 0 ] && pass "$LABEL: missing --connect-port exits nonzero" || fail "$LABEL: missing --connect-port exits nonzero"

    "$BIN" --listen-port 1234 --connect-address 127.0.0.1 --connect-port 1234 \
        --listen-sender-port 5000 2>/dev/null
    [ $? -ne 0 ] \
        && pass "$LABEL: --listen-sender-port without --listen-sender-address rejected" \
        || fail "$LABEL: --listen-sender-port without --listen-sender-address rejected"

    # --- Port validation ---
    echo "=== $LABEL: port validation ==="
    "$BIN" --listen-port abc --connect-address 127.0.0.1 --connect-port 1234 2>/dev/null
    [ $? -ne 0 ] && pass "$LABEL: port 'abc' rejected" || fail "$LABEL: port 'abc' rejected"

    "$BIN" --listen-port 80abc --connect-address 127.0.0.1 --connect-port 1234 2>/dev/null
    [ $? -ne 0 ] && pass "$LABEL: port '80abc' rejected" || fail "$LABEL: port '80abc' rejected"

    "$BIN" --listen-port 99999 --connect-address 127.0.0.1 --connect-port 1234 2>/dev/null
    [ $? -ne 0 ] && pass "$LABEL: port 99999 rejected" || fail "$LABEL: port 99999 rejected"

    "$BIN" --listen-port 1234 --connect-address 127.0.0.1 --connect-port 65536 2>/dev/null
    [ $? -ne 0 ] && pass "$LABEL: port 65536 rejected" || fail "$LABEL: port 65536 rejected"

    # --- Forwarding (listen -> connect) ---
    echo "=== $LABEL: forwarding ==="
    local BACKEND_OUT
    BACKEND_OUT=$(make_tmpfile)
    > "$BACKEND_OUT"
    nc -u -l $P2 > "$BACKEND_OUT" &
    PIDS+=($!)
    sleep 0.2
    "$BIN" --listen-port $P1 --connect-address 127.0.0.1 --connect-port $P2 2>/dev/null &
    PIDS+=($!)
    sleep 0.2
    echo -n "hello-redirect" | nc -u -w1 127.0.0.1 $P1 2>/dev/null
    sleep 0.3
    grep -q "hello-redirect" "$BACKEND_OUT" \
        && pass "$LABEL: packet forwarded listen->connect" \
        || fail "$LABEL: packet forwarded listen->connect"

    # --- Forwarding: multiple packets ---
    echo "=== $LABEL: forwarding: multiple packets ==="
    local BACKEND2_OUT
    BACKEND2_OUT=$(make_tmpfile)
    > "$BACKEND2_OUT"
    nc -u -l $P4 > "$BACKEND2_OUT" &
    PIDS+=($!)
    sleep 0.2
    "$BIN" --listen-port $P3 --connect-address 127.0.0.1 --connect-port $P4 2>/dev/null &
    PIDS+=($!)
    sleep 0.2
    echo -n "packet-one"   | nc -u -w1 127.0.0.1 $P3 2>/dev/null; sleep 0.1
    echo -n "packet-two"   | nc -u -w1 127.0.0.1 $P3 2>/dev/null; sleep 0.1
    echo -n "packet-three" | nc -u -w1 127.0.0.1 $P3 2>/dev/null; sleep 0.3
    grep -q "packet-one"   "$BACKEND2_OUT" && pass "$LABEL: multiple packets: 1 arrived" || fail "$LABEL: multiple packets: 1 arrived"
    grep -q "packet-two"   "$BACKEND2_OUT" && pass "$LABEL: multiple packets: 2 arrived" || fail "$LABEL: multiple packets: 2 arrived"
    grep -q "packet-three" "$BACKEND2_OUT" && pass "$LABEL: multiple packets: 3 arrived" || fail "$LABEL: multiple packets: 3 arrived"

    # --- Forwarding: zero-length datagram ---
    if command -v python3 >/dev/null 2>&1; then
        echo "=== $LABEL: forwarding: zero-length datagram ==="
        local ZERO_OUT
        ZERO_OUT=$(make_tmpfile)
        > "$ZERO_OUT"
        python3 - <<PY > "$ZERO_OUT" &
import socket, select, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('', $P34))
r, _, _ = select.select([s], [], [], 2.0)
if r:
    data, _ = s.recvfrom(65535)
    sys.stdout.write(str(len(data)))
    sys.stdout.flush()
s.close()
PY
        PIDS+=($!)
        sleep 0.2
        "$BIN" --listen-port $P33 --connect-address 127.0.0.1 --connect-port $P34 2>/dev/null &
        PIDS+=($!)
        sleep 0.2
        python3 - <<PY
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(b'', ('127.0.0.1', $P33))
s.close()
PY
        sleep 0.5
        grep -q "^0$" "$ZERO_OUT" \
            && pass "$LABEL: zero-length datagram forwarded" \
            || fail "$LABEL: zero-length datagram forwarded"
    else
        echo "=== $LABEL: forwarding: zero-length datagram (SKIPPED: python3 not found) ==="
    fi

    # --- Two-way forwarding (socat) ---
    if command -v socat >/dev/null 2>&1; then
        echo "=== $LABEL: forwarding: two-way ==="
        local CLIENT_OUT
        CLIENT_OUT=$(make_tmpfile)
        > "$CLIENT_OUT"
        socat UDP4-RECVFROM:$P6,fork EXEC:'cat' &
        PIDS+=($!)
        sleep 0.2
        "$BIN" --listen-port $P5 --connect-address 127.0.0.1 --connect-port $P6 2>/dev/null &
        PIDS+=($!)
        sleep 0.2
        echo -n "echo-me" | nc -u -w2 127.0.0.1 $P5 > "$CLIENT_OUT" 2>/dev/null
        grep -q "echo-me" "$CLIENT_OUT" \
            && pass "$LABEL: two-way forwarding: reply received by client" \
            || fail "$LABEL: two-way forwarding: reply received by client"
    else
        echo "=== $LABEL: forwarding: two-way (SKIPPED: socat not found) ==="
    fi

    # --- Source address/port filtering ---
    if command -v python3 >/dev/null 2>&1; then
        echo "=== $LABEL: forwarding: source filtering ==="
        local BACKEND4_OUT
        BACKEND4_OUT=$(make_tmpfile)
        > "$BACKEND4_OUT"
        python3 -c "
import socket, sys, select
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('', $P8))
r, _, _ = select.select([s], [], [], 2.0)
if r:
    data, _ = s.recvfrom(65535)
    sys.stdout.buffer.write(data); sys.stdout.flush()
s.close()
" > "$BACKEND4_OUT" &
        local BACKEND4_PID=$!
        PIDS+=($BACKEND4_PID)
        sleep 0.2
        "$BIN" --listen-port $P7 --connect-address 127.0.0.1 --connect-port $P8 \
            --listen-sender-address 127.0.0.1 --listen-sender-port $P9 2>/dev/null &
        PIDS+=($!)
        sleep 0.2
        python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('', $P10))
s.sendto(b'wrong-source', ('127.0.0.1', $P7))
s.close()
"
        sleep 0.3
        grep -q "wrong-source" "$BACKEND4_OUT" \
            && fail "$LABEL: source filtering: wrong-source packet should be dropped" \
            || pass "$LABEL: source filtering: wrong-source packet dropped"

        kill "$BACKEND4_PID" 2>/dev/null || true
        sleep 0.1

        local BACKEND4B_OUT
        BACKEND4B_OUT=$(make_tmpfile)
        > "$BACKEND4B_OUT"
        python3 -c "
import socket, sys, select
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('', $P8))
r, _, _ = select.select([s], [], [], 2.0)
if r:
    data, _ = s.recvfrom(65535)
    sys.stdout.buffer.write(data); sys.stdout.flush()
s.close()
" > "$BACKEND4B_OUT" &
        PIDS+=($!)
        sleep 0.2
        python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('', $P9))
s.sendto(b'correct-source', ('127.0.0.1', $P7))
s.close()
"
        sleep 0.5
        grep -q "correct-source" "$BACKEND4B_OUT" \
            && pass "$LABEL: source filtering: correct-source packet forwarded" \
            || fail "$LABEL: source filtering: correct-source packet forwarded"
    else
        echo "=== $LABEL: source filtering (SKIPPED: python3 not found) ==="
    fi

    # --- Hostname resolution ---
    # Use a dual-stack python3 listener (IPV6_V6ONLY=0) so the test passes
    # regardless of whether the OS resolves "localhost" to 127.0.0.1 or ::1
    # (macOS getaddrinfo returns ::1 first; nc -u -l binds IPv4 only → mismatch).
    echo "=== $LABEL: hostname resolution ==="
    local BACKEND5_OUT
    BACKEND5_OUT=$(make_tmpfile)
    > "$BACKEND5_OUT"
    if command -v python3 >/dev/null 2>&1; then
        python3 -c "
import socket, sys, select
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
except OSError:
    pass
s.bind(('::', $P12))
r, _, _ = select.select([s], [], [], 2.0)
if r:
    data, _ = s.recvfrom(4096)
    sys.stdout.buffer.write(data); sys.stdout.flush()
s.close()
" > "$BACKEND5_OUT" &
        PIDS+=($!)
    else
        nc -u -l $P12 > "$BACKEND5_OUT" &
        PIDS+=($!)
    fi
    sleep 0.2
    "$BIN" --listen-port $P11 --connect-host localhost --connect-port $P12 \
        --debug 2>/dev/null &
    PIDS+=($!)
    sleep 0.2
    echo -n "host-resolve" | nc -u -w1 127.0.0.1 $P11 2>/dev/null
    sleep 0.3
    grep -q "host-resolve" "$BACKEND5_OUT" \
        && pass "$LABEL: --connect-host resolves and forwards" \
        || fail "$LABEL: --connect-host resolves and forwards"

    # --- Stats first window timing ---
    echo "=== $LABEL: stats first window timing ==="
    local STATS_OUT
    STATS_OUT=$(make_tmpfile)
    "$BIN" --listen-port $P13 --connect-address 127.0.0.1 --connect-port $P14 \
        --stats --verbose 2>"$STATS_OUT" &
    local STATS_PID=$!
    PIDS+=($STATS_PID)
    sleep 1.5
    kill "$STATS_PID" 2>/dev/null || true
    grep -q "STATS 60s" "$STATS_OUT" \
        && fail "$LABEL: stats not displayed before first 60-s window" \
        || pass "$LABEL: stats not displayed before first 60-s window"

    # --- --stats without --verbose ---
    echo "=== $LABEL: --stats without --verbose ==="
    local FIX6_OUT
    FIX6_OUT=$(make_tmpfile)
    "$BIN" --listen-port $P15 --connect-address 127.0.0.1 --connect-port $P16 \
        --stats 2>"$FIX6_OUT" &
    local FIX6_PID=$!
    PIDS+=($FIX6_PID)
    sleep 0.5
    kill -0 "$FIX6_PID" 2>/dev/null \
        && pass "$LABEL: --stats alone: process stays alive" \
        || fail "$LABEL: --stats alone: process stays alive"
    kill "$FIX6_PID" 2>/dev/null || true
    grep -q -- "---- INFO ----" "$FIX6_OUT" \
        && fail "$LABEL: --stats alone does not enable verbose INFO output" \
        || pass "$LABEL: --stats alone does not enable verbose INFO output"

    # --- SO_BINDTODEVICE / interface binding ---
    echo "=== $LABEL: interface binding ==="
    if [ "$(uname)" = "Linux" ]; then
        "$BIN" --listen-port $P17 --connect-address 127.0.0.1 --connect-port $P18 \
            --listen-interface no_such_iface0 2>/dev/null
        [ $? -ne 0 ] \
            && pass "$LABEL: invalid interface name rejected" \
            || fail "$LABEL: invalid interface name rejected"
    else
        echo "=== $LABEL: interface binding (SKIPPED: not Linux) ==="
    fi

    # --- caddr zero-initialisation ---
    echo "=== $LABEL: caddr zero-initialisation ==="
    local BACKEND8_OUT
    BACKEND8_OUT=$(make_tmpfile)
    > "$BACKEND8_OUT"
    nc -u -l $P20 > "$BACKEND8_OUT" &
    PIDS+=($!)
    sleep 0.2
    "$BIN" --listen-port $P19 --connect-address 127.0.0.1 --connect-port $P20 2>/dev/null &
    PIDS+=($!)
    sleep 0.2
    echo -n "caddr-init" | nc -u -w1 127.0.0.1 $P19 2>/dev/null
    sleep 0.3
    grep -q "caddr-init" "$BACKEND8_OUT" \
        && pass "$LABEL: caddr zero-initialised: packet forwarded via connect address" \
        || fail "$LABEL: caddr zero-initialised: packet forwarded via connect address"

    # --- inet_ntop endpoint address formatting ---
    echo "=== $LABEL: inet_ntop endpoint address formatting ==="
    local INETNTOP_OUT
    INETNTOP_OUT=$(make_tmpfile)
    "$BIN" --listen-port $P21 --connect-address 127.0.0.1 --connect-port $P22 \
        --debug 2>"$INETNTOP_OUT" &
    local INETNTOP_PID=$!
    PIDS+=($INETNTOP_PID)
    sleep 0.2
    echo -n "probe" | nc -u -w1 127.0.0.1 $P21 2>/dev/null
    sleep 0.3
    kill "$INETNTOP_PID" 2>/dev/null || true
    grep -qE "LISTEN remote endpoint set to \(127\.0\.0\.1, [0-9]+\)" "$INETNTOP_OUT" \
        && pass "$LABEL: inet_ntop: endpoint address logged as valid dotted-quad" \
        || fail "$LABEL: inet_ntop: endpoint address logged as valid dotted-quad"

    # --- time_t stats time deltas ---
    echo "=== $LABEL: time_t stats time deltas ==="
    local TIMEDELTA_OUT
    TIMEDELTA_OUT=$(make_tmpfile)
    "$BIN" --listen-port $P23 --connect-address 127.0.0.1 --connect-port $P24 \
        --stats --verbose 2>"$TIMEDELTA_OUT" &
    local TIMEDELTA_PID=$!
    PIDS+=($TIMEDELTA_PID)
    sleep 0.5
    kill -0 "$TIMEDELTA_PID" 2>/dev/null \
        && pass "$LABEL: time_t time_delta: process stable with --stats" \
        || fail "$LABEL: time_t time_delta: process stable with --stats"
    kill "$TIMEDELTA_PID" 2>/dev/null || true

    # --- inet_pton address parsing ---
    echo "=== $LABEL: inet_pton address parsing ==="
    "$BIN" --listen-port $P25 --connect-address "not.an.address" \
        --connect-port 1234 2>/dev/null
    [ $? -ne 0 ] \
        && pass "$LABEL: inet_pton: invalid address string rejected" \
        || fail "$LABEL: inet_pton: invalid address string rejected"

    "$BIN" --listen-port $P25 --connect-address 255.255.255.255 \
        --connect-port $P26 2>/dev/null &
    local BC_PID=$!
    PIDS+=($BC_PID)
    sleep 0.3
    kill -0 "$BC_PID" 2>/dev/null \
        && pass "$LABEL: inet_pton: 255.255.255.255 no longer confused with INADDR_NONE" \
        || fail "$LABEL: inet_pton: 255.255.255.255 no longer confused with INADDR_NONE"
    kill "$BC_PID" 2>/dev/null || true

    # --- IPv6 support ---
    if python3 -c "import socket; s=socket.socket(socket.AF_INET6,socket.SOCK_DGRAM); s.bind(('::1',0)); s.close()" 2>/dev/null; then
        echo "=== $LABEL: IPv6 support ==="

        "$BIN" --listen-port $P27 --connect-address "2001:::invalid" \
            --connect-port 1234 2>/dev/null
        [ $? -ne 0 ] \
            && pass "$LABEL: IPv6: invalid address string rejected" \
            || fail "$LABEL: IPv6: invalid address string rejected"

        local BACKEND11_OUT
        BACKEND11_OUT=$(make_tmpfile)
        > "$BACKEND11_OUT"
        python3 -c "
import socket, sys, select
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('::1', $P30))
r, _, _ = select.select([s], [], [], 2.0)
if r:
    data, _ = s.recvfrom(65535)
    sys.stdout.buffer.write(data); sys.stdout.flush()
s.close()
" > "$BACKEND11_OUT" &
        PIDS+=($!)
        sleep 0.2
        "$BIN" --listen-port $P29 --connect-address ::1 --connect-port $P30 2>/dev/null &
        PIDS+=($!)
        sleep 0.2
        python3 -c "
import socket
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.sendto(b'ipv6-forward', ('::1', $P29))
s.close()
"
        sleep 0.5
        grep -q "ipv6-forward" "$BACKEND11_OUT" \
            && pass "$LABEL: IPv6: packet forwarded via ::1 connect address" \
            || fail "$LABEL: IPv6: packet forwarded via ::1 connect address"

        local BACKEND12_OUT
        BACKEND12_OUT=$(make_tmpfile)
        > "$BACKEND12_OUT"
        python3 -c "
import socket, sys, select
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('::1', $P32))
r, _, _ = select.select([s], [], [], 2.0)
if r:
    data, _ = s.recvfrom(65535)
    sys.stdout.buffer.write(data); sys.stdout.flush()
s.close()
" > "$BACKEND12_OUT" &
        PIDS+=($!)
        sleep 0.2
        "$BIN" --listen-address ::1 --listen-port $P31 \
            --connect-address ::1 --connect-port $P32 2>/dev/null &
        PIDS+=($!)
        sleep 0.2
        python3 -c "
import socket
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.sendto(b'ipv6-listen-addr', ('::1', $P31))
s.close()
"
        sleep 0.5
        grep -q "ipv6-listen-addr" "$BACKEND12_OUT" \
            && pass "$LABEL: IPv6: packet forwarded with explicit --listen-address ::1" \
            || fail "$LABEL: IPv6: packet forwarded with explicit --listen-address ::1"

        # --- Cross-family: IPv4 listen -> IPv6 connect ---
        echo "=== $LABEL: cross-family: IPv4 listen -> IPv6 connect ==="
        local BACKEND_CF1_OUT
        BACKEND_CF1_OUT=$(make_tmpfile)
        > "$BACKEND_CF1_OUT"
        python3 - <<PY > "$BACKEND_CF1_OUT" &
import socket, sys, select
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('::1', $P36))
r, _, _ = select.select([s], [], [], 3.0)
if r:
    data, _ = s.recvfrom(65535)
    sys.stdout.buffer.write(data); sys.stdout.flush()
s.close()
PY
        PIDS+=($!)
        sleep 0.2
        "$BIN" --listen-address 0.0.0.0 --listen-port $P35 \
            --connect-address ::1 --connect-port $P36 2>/dev/null &
        PIDS+=($!)
        sleep 0.2
        python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(b'cf-ipv4-to-ipv6', ('127.0.0.1', $P35))
s.close()
"
        sleep 0.5
        grep -q "cf-ipv4-to-ipv6" "$BACKEND_CF1_OUT" \
            && pass "$LABEL: cross-family: IPv4 client packet forwarded to IPv6 server" \
            || fail "$LABEL: cross-family: IPv4 client packet forwarded to IPv6 server"

        # --- Cross-family: IPv6 listen -> IPv4 connect ---
        echo "=== $LABEL: cross-family: IPv6 listen -> IPv4 connect ==="
        local BACKEND_CF2_OUT
        BACKEND_CF2_OUT=$(make_tmpfile)
        > "$BACKEND_CF2_OUT"
        python3 - <<PY > "$BACKEND_CF2_OUT" &
import socket, sys, select
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', $P38))
r, _, _ = select.select([s], [], [], 3.0)
if r:
    data, _ = s.recvfrom(65535)
    sys.stdout.buffer.write(data); sys.stdout.flush()
s.close()
PY
        PIDS+=($!)
        sleep 0.2
        "$BIN" --listen-address ::1 --listen-port $P37 \
            --connect-address 127.0.0.1 --connect-port $P38 2>/dev/null &
        PIDS+=($!)
        sleep 0.2
        python3 -c "
import socket
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.sendto(b'cf-ipv6-to-ipv4', ('::1', $P37))
s.close()
"
        sleep 0.5
        grep -q "cf-ipv6-to-ipv4" "$BACKEND_CF2_OUT" \
            && pass "$LABEL: cross-family: IPv6 client packet forwarded to IPv4 server" \
            || fail "$LABEL: cross-family: IPv6 client packet forwarded to IPv4 server"

    else
        echo "=== $LABEL: IPv6 support (SKIPPED: IPv6 loopback not available) ==="
    fi
}

# ---------------------------------------------------------------------------
# Run all tests
# ---------------------------------------------------------------------------

run_tests ./udp-redirect 0

# --- Results ---
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
