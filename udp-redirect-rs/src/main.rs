// udp-redirect-rs: Rust port of udp-redirect.c
// Licensed GPLv2

use clap::Parser;
use std::ffi::CString;
use std::mem;
use std::process;
use std::time::{SystemTime, UNIX_EPOCH};

// ──────────────────────────────────────────────────────────────────────────
// libc functions/constants not always re-exported by the libc crate
// ──────────────────────────────────────────────────────────────────────────

const INET6_ADDRSTRLEN: usize = 46;

extern "C" {
    fn inet_pton(af: libc::c_int, src: *const libc::c_char, dst: *mut libc::c_void) -> libc::c_int;
    fn inet_ntop(af: libc::c_int, src: *const libc::c_void, dst: *mut libc::c_char, size: libc::socklen_t) -> *const libc::c_char;
}

// ──────────────────────────────────────────────────────────────────────────
// Constants
// ──────────────────────────────────────────────────────────────────────────

const VERSION: &str = "2.0.0";
const STATISTICS_DELAY_SECONDS: i64 = 60;
const NETWORK_BUFFER_SIZE: usize = 65535;

// ──────────────────────────────────────────────────────────────────────────
// Debug levels
// ──────────────────────────────────────────────────────────────────────────

const DEBUG_LEVEL_ERROR: i32 = 0;
const DEBUG_LEVEL_INFO: i32 = 1;
const DEBUG_LEVEL_VERBOSE: i32 = 2;
const DEBUG_LEVEL_DEBUG: i32 = 3;

// ──────────────────────────────────────────────────────────────────────────
// Debug macro
// ──────────────────────────────────────────────────────────────────────────

/// Print a debug message to stderr if local_level >= message_level.
/// Format: "file:line:timestamp:function(): message"
#[macro_export]
macro_rules! debug {
    ($local_level:expr, $message_level:expr, $fmt:literal $(, $args:expr)*) => {
        if ($local_level) >= ($message_level) {
            let ts = $crate::current_timestamp();
            let msg = format!($fmt $(, $args)*);
            eprintln!("{}:{}:{}:?(): {}", file!(), line!(), ts, msg);
        }
    };
}

pub fn current_timestamp() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

// ──────────────────────────────────────────────────────────────────────────
// CLI argument structure (clap derive)
// ──────────────────────────────────────────────────────────────────────────

#[derive(Parser, Debug)]
#[command(
    name = "udp-redirect-rs",
    about = "A simple and high-performance UDP redirector",
    // Disable clap's built-in --version so we can handle it ourselves
    disable_version_flag = true
)]
struct Cli {
    /// Verbose mode (repeatable: first occurrence sets level 2, each additional +1)
    #[arg(long, short = 'v', action = clap::ArgAction::Count)]
    verbose: u8,

    /// Debug mode (sets level 3)
    #[arg(long, short = 'd')]
    debug: bool,

    /// Listen address (optional)
    #[arg(long)]
    listen_address: Option<String>,

    /// Listen port (required)
    #[arg(long, value_parser = parse_port_arg)]
    listen_port: Option<u16>,

    /// Listen interface (optional)
    #[arg(long)]
    listen_interface: Option<String>,

    /// Only receive packets from the same source as the first packet
    #[arg(long)]
    listen_address_strict: bool,

    /// Connect address, IPv4 or IPv6 (required unless --connect-host is given)
    #[arg(long)]
    connect_address: Option<String>,

    /// Connect hostname (overwrites --connect-address if both are given)
    #[arg(long)]
    connect_host: Option<String>,

    /// Connect port (required)
    #[arg(long, value_parser = parse_port_arg)]
    connect_port: Option<u16>,

    /// Only receive packets from --connect-address / --connect-port
    #[arg(long)]
    connect_address_strict: bool,

    /// Send packets from address (optional)
    #[arg(long)]
    send_address: Option<String>,

    /// Send packets from port (optional)
    #[arg(long, value_parser = parse_port_arg)]
    send_port: Option<u16>,

    /// Send packets from interface (optional)
    #[arg(long)]
    send_interface: Option<String>,

    /// Listen endpoint only accepts packets from this source address (optional, must be paired with --listen-sender-port)
    #[arg(long)]
    listen_sender_address: Option<String>,

    /// Listen endpoint only accepts packets from this source port (optional, must be paired with --listen-sender-address)
    #[arg(long, value_parser = parse_port_arg)]
    listen_sender_port: Option<u16>,

    /// Ignore most receive or send errors (default)
    #[arg(long)]
    ignore_errors: bool,

    /// Exit on most receive or send errors
    #[arg(long)]
    stop_errors: bool,

    /// Display stats every 60 seconds
    #[arg(long)]
    stats: bool,

    /// Print version and exit
    #[arg(long = "version")]
    version: bool,
}

fn parse_port_arg(s: &str) -> Result<u16, String> {
    s.parse::<u16>()
        .map_err(|_| format!("'{}' is not a valid port number (0-65535)", s))
}

// ──────────────────────────────────────────────────────────────────────────
// Address helpers — thin wrappers around libc sockaddr_storage
// ──────────────────────────────────────────────────────────────────────────

/// Parse an IPv4 or IPv6 address string into a zeroed sockaddr_storage.
/// Returns Some((storage, family)) on success, None on failure.
/// Rejects strings containing '%' (zone IDs).
pub fn parse_addr(s: &str, port: u16) -> Option<(libc::sockaddr_storage, libc::c_int)> {
    if s.contains('%') {
        return None;
    }

    let mut storage: libc::sockaddr_storage = unsafe { mem::zeroed() };

    // Try IPv4
    {
        let a4 = unsafe { &mut *(&mut storage as *mut _ as *mut libc::sockaddr_in) };
        let c = CString::new(s).ok()?;
        let rc = unsafe { inet_pton(libc::AF_INET, c.as_ptr(), &mut a4.sin_addr as *mut _ as *mut libc::c_void) };
        if rc == 1 {
            storage.ss_family = libc::AF_INET as libc::sa_family_t;
            unsafe {
                (*(&mut storage as *mut _ as *mut libc::sockaddr_in)).sin_port = port.to_be();
            }
            return Some((storage, libc::AF_INET));
        }
    }

    // Reset storage for IPv6 attempt
    storage = unsafe { mem::zeroed() };

    // Try IPv6
    {
        let a6 = unsafe { &mut *(&mut storage as *mut _ as *mut libc::sockaddr_in6) };
        let c = CString::new(s).ok()?;
        let rc = unsafe { inet_pton(libc::AF_INET6, c.as_ptr(), &mut a6.sin6_addr as *mut _ as *mut libc::c_void) };
        if rc == 1 {
            storage.ss_family = libc::AF_INET6 as libc::sa_family_t;
            unsafe {
                (*(&mut storage as *mut _ as *mut libc::sockaddr_in6)).sin6_port = port.to_be();
            }
            return Some((storage, libc::AF_INET6));
        }
    }

    None
}

/// Format the address portion of a sockaddr_storage using inet_ntop.
/// Returns "?" on failure.
pub fn addr_tostring(sa: &libc::sockaddr_storage) -> String {
    let mut buf = vec![0u8; INET6_ADDRSTRLEN as usize];
    let family = sa.ss_family as libc::c_int;
    let addr_ptr: *const libc::c_void = if family == libc::AF_INET6 {
        unsafe { &(*( sa as *const _ as *const libc::sockaddr_in6)).sin6_addr as *const _ as *const libc::c_void }
    } else {
        unsafe { &(*(sa as *const _ as *const libc::sockaddr_in)).sin_addr as *const _ as *const libc::c_void }
    };
    let result = unsafe {
        inet_ntop(
            family,
            addr_ptr,
            buf.as_mut_ptr() as *mut libc::c_char,
            buf.len() as libc::socklen_t,
        )
    };
    if result.is_null() {
        return "?".to_string();
    }
    let end = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    String::from_utf8_lossy(&buf[..end]).to_string()
}

/// Return the port from a sockaddr_storage in host byte order.
pub fn addr_port(sa: &libc::sockaddr_storage) -> u16 {
    if sa.ss_family as libc::c_int == libc::AF_INET6 {
        let a6 = unsafe { &*(sa as *const _ as *const libc::sockaddr_in6) };
        u16::from_be(a6.sin6_port)
    } else {
        let a4 = unsafe { &*(sa as *const _ as *const libc::sockaddr_in) };
        u16::from_be(a4.sin_port)
    }
}

/// Return the wire length of a sockaddr_storage appropriate for its family.
pub fn addr_len(sa: &libc::sockaddr_storage) -> libc::socklen_t {
    if sa.ss_family as libc::c_int == libc::AF_INET6 {
        mem::size_of::<libc::sockaddr_in6>() as libc::socklen_t
    } else {
        mem::size_of::<libc::sockaddr_in>() as libc::socklen_t
    }
}

/// Return true if the storage has ss_family == 0 (zero-initialised sentinel).
pub fn addr_is_unset(sa: &libc::sockaddr_storage) -> bool {
    sa.ss_family == 0
}

/// Return true if both sockaddr_storage values have the same family, address, and port.
pub fn addr_equal(a: &libc::sockaddr_storage, b: &libc::sockaddr_storage) -> bool {
    if a.ss_family != b.ss_family {
        return false;
    }
    let family = a.ss_family as libc::c_int;
    if family == libc::AF_INET {
        let a4 = unsafe { &*(a as *const _ as *const libc::sockaddr_in) };
        let b4 = unsafe { &*(b as *const _ as *const libc::sockaddr_in) };
        return a4.sin_addr.s_addr == b4.sin_addr.s_addr && a4.sin_port == b4.sin_port;
    }
    if family == libc::AF_INET6 {
        let a6 = unsafe { &*(a as *const _ as *const libc::sockaddr_in6) };
        let b6 = unsafe { &*(b as *const _ as *const libc::sockaddr_in6) };
        return a6.sin6_addr.s6_addr == b6.sin6_addr.s6_addr && a6.sin6_port == b6.sin6_port;
    }
    false
}

// ──────────────────────────────────────────────────────────────────────────
// socket_setup
// ──────────────────────────────────────────────────────────────────────────

/// Create, configure, bind, and return a non-blocking UDP socket.
/// Returns (fd, bound_sockaddr_storage).
fn socket_setup(
    debug_level: i32,
    desc: &str,
    xaddr: Option<&str>,
    xport: u16,
    xif: Option<&str>,
    xfamily: libc::c_int,
) -> (libc::c_int, libc::sockaddr_storage) {
    let mut addr: libc::sockaddr_storage = unsafe { mem::zeroed() };
    let family: libc::c_int;

    if let Some(addr_str) = xaddr {
        match parse_addr(addr_str, xport) {
            Some((parsed, fam)) => {
                addr = parsed;
                family = fam;
                debug!(debug_level, DEBUG_LEVEL_INFO, "{} socket: bind to address {}", desc, addr_str);
            }
            None => {
                debug!(debug_level, DEBUG_LEVEL_ERROR, "{} address invalid: {}", desc, addr_str);
                process::exit(1);
            }
        }
    } else {
        family = xfamily;
        addr.ss_family = family as libc::sa_family_t;
        if family == libc::AF_INET6 {
            let a6 = unsafe { &mut *((&mut addr) as *mut _ as *mut libc::sockaddr_in6) };
            a6.sin6_addr = unsafe { libc::in6addr_any };
            a6.sin6_port = xport.to_be();
        } else {
            let a4 = unsafe { &mut *((&mut addr) as *mut _ as *mut libc::sockaddr_in) };
            a4.sin_addr.s_addr = libc::INADDR_ANY;
            a4.sin_port = xport.to_be();
        }
        debug!(debug_level, DEBUG_LEVEL_INFO, "{} socket: bind to address ANY", desc);
    }

    if xport != 0 {
        debug!(debug_level, DEBUG_LEVEL_INFO, "{} socket: bind to port {}", desc, xport);
    } else {
        debug!(debug_level, DEBUG_LEVEL_INFO, "{} socket: bind to port ANY", desc);
    }

    debug!(debug_level, DEBUG_LEVEL_INFO, "{} socket: create", desc);
    let xsock = unsafe { libc::socket(family, libc::SOCK_DGRAM, libc::IPPROTO_UDP) };
    if xsock == -1 {
        debug!(debug_level, DEBUG_LEVEL_ERROR, "Cannot create DGRAM socket ({})", errno());
        process::exit(1);
    }

    // Bind to interface if requested
    if let Some(iface) = xif {
        debug!(debug_level, DEBUG_LEVEL_INFO, "{} socket: bind to interface {}", desc, iface);

        #[cfg(target_os = "macos")]
        {
            let c_iface = match CString::new(iface) {
                Ok(s) => s,
                Err(_) => {
                    debug!(debug_level, DEBUG_LEVEL_ERROR, "Interface name contains null byte: {}", iface);
                    process::exit(1);
                }
            };
            let idx = unsafe { libc::if_nametoindex(c_iface.as_ptr()) };
            if idx == 0 {
                debug!(debug_level, DEBUG_LEVEL_ERROR, "Cannot get the interface ID ({})", errno());
                process::exit(1);
            }
            let (if_level, if_optname) = if family == libc::AF_INET6 {
                (libc::IPPROTO_IPV6, libc::IPV6_BOUND_IF)
            } else {
                (libc::IPPROTO_IP, libc::IP_BOUND_IF)
            };
            let rc = unsafe {
                libc::setsockopt(
                    xsock,
                    if_level,
                    if_optname,
                    &idx as *const _ as *const libc::c_void,
                    mem::size_of_val(&idx) as libc::socklen_t,
                )
            };
            if rc == -1 {
                debug!(debug_level, DEBUG_LEVEL_ERROR, "Cannot set socket interface ({})", errno());
                process::exit(1);
            }
        }

        #[cfg(target_os = "linux")]
        {
            if iface.len() >= libc::IFNAMSIZ {
                debug!(
                    debug_level, DEBUG_LEVEL_ERROR,
                    "Interface name too long (max {}): {}", libc::IFNAMSIZ - 1, iface
                );
                process::exit(1);
            }
            let c_iface = match CString::new(iface) {
                Ok(s) => s,
                Err(_) => {
                    debug!(debug_level, DEBUG_LEVEL_ERROR, "Interface name contains null byte: {}", iface);
                    process::exit(1);
                }
            };
            let rc = unsafe {
                libc::setsockopt(
                    xsock,
                    libc::SOL_SOCKET,
                    libc::SO_BINDTODEVICE,
                    c_iface.as_ptr() as *const libc::c_void,
                    (iface.len() + 1) as libc::socklen_t,
                )
            };
            if rc == -1 {
                debug!(debug_level, DEBUG_LEVEL_ERROR, "Cannot set socket interface ({})", errno());
                process::exit(1);
            }
        }

        // Non-macOS / non-Linux: no-op for interface binding
        #[cfg(not(any(target_os = "macos", target_os = "linux")))]
        {
            debug!(debug_level, DEBUG_LEVEL_ERROR, "Interface binding not supported on this platform");
            process::exit(1);
        }
    } else {
        debug!(debug_level, DEBUG_LEVEL_INFO, "{} socket: bind to interface ANY", desc);
    }

    debug!(debug_level, DEBUG_LEVEL_INFO, "{} socket: reuse local address", desc);
    let enable: libc::c_int = 1;
    let rc = unsafe {
        libc::setsockopt(
            xsock,
            libc::SOL_SOCKET,
            libc::SO_REUSEADDR,
            &enable as *const _ as *const libc::c_void,
            mem::size_of_val(&enable) as libc::socklen_t,
        )
    };
    if rc < 0 {
        debug!(debug_level, DEBUG_LEVEL_ERROR, "Cannot set socket SO_REUSEADDR ({})", errno());
        process::exit(1);
    }

    debug!(debug_level, DEBUG_LEVEL_INFO, "{} socket: set nonblocking", desc);
    let rc = unsafe { libc::fcntl(xsock, libc::F_SETFL, libc::O_NONBLOCK) };
    if rc == -1 {
        debug!(debug_level, DEBUG_LEVEL_ERROR, "Cannot set socket O_NONBLOCK ({})", errno());
        process::exit(1);
    }

    debug!(debug_level, DEBUG_LEVEL_INFO, "{} socket: bind", desc);
    let rc = unsafe {
        libc::bind(
            xsock,
            &addr as *const _ as *const libc::sockaddr,
            addr_len(&addr),
        )
    };
    if rc == -1 {
        debug!(debug_level, DEBUG_LEVEL_ERROR, "Cannot bind socket ({})", errno());
        process::exit(1);
    }

    let mut sock_name: libc::sockaddr_storage = unsafe { mem::zeroed() };
    let mut sock_name_len = mem::size_of::<libc::sockaddr_storage>() as libc::socklen_t;
    let rc = unsafe {
        libc::getsockname(
            xsock,
            &mut sock_name as *mut _ as *mut libc::sockaddr,
            &mut sock_name_len,
        )
    };
    if rc == -1 {
        debug!(debug_level, DEBUG_LEVEL_ERROR, "Cannot get socket name ({})", errno());
        process::exit(1);
    }

    (xsock, sock_name)
}

// ──────────────────────────────────────────────────────────────────────────
// resolve_host
// ──────────────────────────────────────────────────────────────────────────

fn resolve_host(debug_level: i32, host: &str) -> String {
    let c_host = match CString::new(host) {
        Ok(s) => s,
        Err(_) => {
            debug!(debug_level, DEBUG_LEVEL_ERROR, "Host contains null byte: {}", host);
            process::exit(1);
        }
    };

    let mut hints: libc::addrinfo = unsafe { mem::zeroed() };
    hints.ai_family = libc::AF_UNSPEC;
    hints.ai_socktype = libc::SOCK_DGRAM;

    let mut res: *mut libc::addrinfo = std::ptr::null_mut();
    let rc = unsafe { libc::getaddrinfo(c_host.as_ptr(), std::ptr::null(), &hints, &mut res) };
    if rc != 0 {
        let err_msg = unsafe {
            let ptr = libc::gai_strerror(rc);
            if ptr.is_null() {
                "unknown error".to_string()
            } else {
                std::ffi::CStr::from_ptr(ptr).to_string_lossy().to_string()
            }
        };
        debug!(debug_level, DEBUG_LEVEL_ERROR, "Could not resolve host {}: {}", host, err_msg);
        process::exit(1);
    }

    let ai = unsafe { &*res };
    let mut buf = vec![0u8; INET6_ADDRSTRLEN as usize];

    let (family, addr_ptr) = if ai.ai_family == libc::AF_INET6 {
        let a6 = unsafe { &*(ai.ai_addr as *const libc::sockaddr_in6) };
        (libc::AF_INET6, &a6.sin6_addr as *const _ as *const libc::c_void)
    } else {
        let a4 = unsafe { &*(ai.ai_addr as *const libc::sockaddr_in) };
        (libc::AF_INET, &a4.sin_addr as *const _ as *const libc::c_void)
    };

    let result = unsafe {
        inet_ntop(
            family,
            addr_ptr,
            buf.as_mut_ptr() as *mut libc::c_char,
            buf.len() as libc::socklen_t,
        )
    };

    unsafe { libc::freeaddrinfo(res) };

    if result.is_null() {
        debug!(debug_level, DEBUG_LEVEL_ERROR, "Could not format resolved address ({})", errno());
        process::exit(1);
    }

    let end = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    let ip = String::from_utf8_lossy(&buf[..end]).to_string();

    debug!(debug_level, DEBUG_LEVEL_DEBUG, "Resolved {} to {}", host, ip);
    ip
}

// ──────────────────────────────────────────────────────────────────────────
// Human-readable scale helpers
// ──────────────────────────────────────────────────────────────────────────

const HUMAN_SIZES: [char; 7] = [' ', 'K', 'M', 'G', 'T', 'P', 'E'];

fn int_to_human_scale(mut value: f64) -> (f64, usize) {
    let mut count = 0usize;
    while value >= 1000.0 && count < (HUMAN_SIZES.len() - 1) {
        value /= 1000.0;
        count += 1;
    }
    (value, count)
}

pub fn int_to_human_value(value: f64) -> f64 {
    int_to_human_scale(value).0
}

pub fn int_to_human_char(value: f64) -> char {
    HUMAN_SIZES[int_to_human_scale(value).1]
}

// ──────────────────────────────────────────────────────────────────────────
// Statistics
// ──────────────────────────────────────────────────────────────────────────

#[derive(Default)]
struct Statistics {
    time_display_last: i64,
    time_display_first: i64,

    count_listen_packet_receive: u64,
    count_listen_byte_receive: u64,
    count_listen_packet_send: u64,
    count_listen_byte_send: u64,
    count_connect_packet_receive: u64,
    count_connect_byte_receive: u64,
    count_connect_packet_send: u64,
    count_connect_byte_send: u64,

    count_listen_packet_receive_total: u64,
    count_listen_byte_receive_total: u64,
    count_listen_packet_send_total: u64,
    count_listen_byte_send_total: u64,
    count_connect_packet_receive_total: u64,
    count_connect_byte_receive_total: u64,
    count_connect_packet_send_total: u64,
    count_connect_byte_send_total: u64,
}

fn statistics_display(debug_level: i32, st: &mut Statistics, now: i64) {
    // Clamp debug_level to at least INFO so stats are always visible
    let dl = if debug_level < DEBUG_LEVEL_INFO { DEBUG_LEVEL_INFO } else { debug_level };

    let mut time_delta = now - st.time_display_last;
    if time_delta < 1 { time_delta = 1; }
    let mut time_delta_total = now - st.time_display_first;
    if time_delta_total < 1 { time_delta_total = 1; }

    // Accumulate window into totals
    st.count_listen_packet_receive_total += st.count_listen_packet_receive;
    st.count_listen_byte_receive_total += st.count_listen_byte_receive;
    st.count_listen_packet_send_total += st.count_listen_packet_send;
    st.count_listen_byte_send_total += st.count_listen_byte_send;
    st.count_connect_packet_receive_total += st.count_connect_packet_receive;
    st.count_connect_byte_receive_total += st.count_connect_byte_receive;
    st.count_connect_packet_send_total += st.count_connect_packet_send;
    st.count_connect_byte_send_total += st.count_connect_byte_send;

    let td = time_delta as f64;
    let tdt = time_delta_total as f64;

    debug!(dl, DEBUG_LEVEL_INFO, "---- STATS {}s ----", STATISTICS_DELAY_SECONDS);

    debug!(dl, DEBUG_LEVEL_INFO,
        "listen:receive:packets: {:.1}{} ({:.1}{}/s), listen:receive:bytes: {:.1}{} ({:.1}{}/s)",
        int_to_human_value(st.count_listen_packet_receive as f64),
        int_to_human_char(st.count_listen_packet_receive as f64),
        int_to_human_value(st.count_listen_packet_receive as f64 / td),
        int_to_human_char(st.count_listen_packet_receive as f64 / td),
        int_to_human_value(st.count_listen_byte_receive as f64),
        int_to_human_char(st.count_listen_byte_receive as f64),
        int_to_human_value(st.count_listen_byte_receive as f64 / td),
        int_to_human_char(st.count_listen_byte_receive as f64 / td)
    );

    debug!(dl, DEBUG_LEVEL_INFO,
        "listen:send:packets: {:.1}{} ({:.1}{}/s), listen:send:bytes: {:.1}{} ({:.1}{}/s)",
        int_to_human_value(st.count_listen_packet_send as f64),
        int_to_human_char(st.count_listen_packet_send as f64),
        int_to_human_value(st.count_listen_packet_send as f64 / td),
        int_to_human_char(st.count_listen_packet_send as f64 / td),
        int_to_human_value(st.count_listen_byte_send as f64),
        int_to_human_char(st.count_listen_byte_send as f64),
        int_to_human_value(st.count_listen_byte_send as f64 / td),
        int_to_human_char(st.count_listen_byte_send as f64 / td)
    );

    debug!(dl, DEBUG_LEVEL_INFO,
        "connect:receive:packets: {:.1}{} ({:.1}{}/s), connect:receive:bytes: {:.1}{} ({:.1}{}/s)",
        int_to_human_value(st.count_connect_packet_receive as f64),
        int_to_human_char(st.count_connect_packet_receive as f64),
        int_to_human_value(st.count_connect_packet_receive as f64 / td),
        int_to_human_char(st.count_connect_packet_receive as f64 / td),
        int_to_human_value(st.count_connect_byte_receive as f64),
        int_to_human_char(st.count_connect_byte_receive as f64),
        int_to_human_value(st.count_connect_byte_receive as f64 / td),
        int_to_human_char(st.count_connect_byte_receive as f64 / td)
    );

    debug!(dl, DEBUG_LEVEL_INFO,
        "connect:send:packets: {:.1}{} ({:.1}{}/s), connect:send:bytes: {:.1}{} ({:.1}{}/s)",
        int_to_human_value(st.count_connect_packet_send as f64),
        int_to_human_char(st.count_connect_packet_send as f64),
        int_to_human_value(st.count_connect_packet_send as f64 / td),
        int_to_human_char(st.count_connect_packet_send as f64 / td),
        int_to_human_value(st.count_connect_byte_send as f64),
        int_to_human_char(st.count_connect_byte_send as f64),
        int_to_human_value(st.count_connect_byte_send as f64 / td),
        int_to_human_char(st.count_connect_byte_send as f64 / td)
    );

    debug!(dl, DEBUG_LEVEL_INFO, "---- STATS TOTAL ----");

    debug!(dl, DEBUG_LEVEL_INFO,
        "listen:receive:packets: {:.1}{} ({:.1}{}/s), listen:receive:bytes: {:.1}{} ({:.1}{}/s)",
        int_to_human_value(st.count_listen_packet_receive_total as f64),
        int_to_human_char(st.count_listen_packet_receive_total as f64),
        int_to_human_value(st.count_listen_packet_receive_total as f64 / tdt),
        int_to_human_char(st.count_listen_packet_receive_total as f64 / tdt),
        int_to_human_value(st.count_listen_byte_receive_total as f64),
        int_to_human_char(st.count_listen_byte_receive_total as f64),
        int_to_human_value(st.count_listen_byte_receive_total as f64 / tdt),
        int_to_human_char(st.count_listen_byte_receive_total as f64 / tdt)
    );

    debug!(dl, DEBUG_LEVEL_INFO,
        "listen:send:packets: {:.1}{} ({:.1}{}/s), listen:send:bytes: {:.1}{} ({:.1}{}/s)",
        int_to_human_value(st.count_listen_packet_send_total as f64),
        int_to_human_char(st.count_listen_packet_send_total as f64),
        int_to_human_value(st.count_listen_packet_send_total as f64 / tdt),
        int_to_human_char(st.count_listen_packet_send_total as f64 / tdt),
        int_to_human_value(st.count_listen_byte_send_total as f64),
        int_to_human_char(st.count_listen_byte_send_total as f64),
        int_to_human_value(st.count_listen_byte_send_total as f64 / tdt),
        int_to_human_char(st.count_listen_byte_send_total as f64 / tdt)
    );

    debug!(dl, DEBUG_LEVEL_INFO,
        "connect:receive:packets: {:.1}{} ({:.1}{}/s), connect:receive:bytes: {:.1}{} ({:.1}{}/s)",
        int_to_human_value(st.count_connect_packet_receive_total as f64),
        int_to_human_char(st.count_connect_packet_receive_total as f64),
        int_to_human_value(st.count_connect_packet_receive_total as f64 / tdt),
        int_to_human_char(st.count_connect_packet_receive_total as f64 / tdt),
        int_to_human_value(st.count_connect_byte_receive_total as f64),
        int_to_human_char(st.count_connect_byte_receive_total as f64),
        int_to_human_value(st.count_connect_byte_receive_total as f64 / tdt),
        int_to_human_char(st.count_connect_byte_receive_total as f64 / tdt)
    );

    debug!(dl, DEBUG_LEVEL_INFO,
        "connect:send:packets: {:.1}{} ({:.1}{}/s), connect:send:bytes: {:.1}{} ({:.1}{}/s)",
        int_to_human_value(st.count_connect_packet_send_total as f64),
        int_to_human_char(st.count_connect_packet_send_total as f64),
        int_to_human_value(st.count_connect_packet_send_total as f64 / tdt),
        int_to_human_char(st.count_connect_packet_send_total as f64 / tdt),
        int_to_human_value(st.count_connect_byte_send_total as f64),
        int_to_human_char(st.count_connect_byte_send_total as f64),
        int_to_human_value(st.count_connect_byte_send_total as f64 / tdt),
        int_to_human_char(st.count_connect_byte_send_total as f64 / tdt)
    );

    // Reset window counters
    st.count_listen_packet_receive = 0;
    st.count_listen_byte_receive = 0;
    st.count_listen_packet_send = 0;
    st.count_listen_byte_send = 0;
    st.count_connect_packet_receive = 0;
    st.count_connect_byte_receive = 0;
    st.count_connect_packet_send = 0;
    st.count_connect_byte_send = 0;
}

// ──────────────────────────────────────────────────────────────────────────
// errno helper
// ──────────────────────────────────────────────────────────────────────────

fn errno() -> libc::c_int {
    unsafe { *libc::__errno_location() }
}

fn errno_is_ignored(ignore_set: &[bool], err: libc::c_int) -> bool {
    if err < 0 || err as usize >= ignore_set.len() {
        return false;
    }
    ignore_set[err as usize]
}

// ──────────────────────────────────────────────────────────────────────────
// main
// ──────────────────────────────────────────────────────────────────────────

fn main() {
    let cli = Cli::parse();

    // Handle --version early
    if cli.version {
        eprintln!("udp-redirect-rs v{}", VERSION);
        process::exit(0);
    }

    // Determine debug level
    let mut debug_level: i32 = DEBUG_LEVEL_ERROR;
    if cli.debug {
        debug_level = DEBUG_LEVEL_DEBUG;
    } else if cli.verbose > 0 {
        // First --verbose sets level 2; each additional increments by 1
        debug_level = DEBUG_LEVEL_VERBOSE + (cli.verbose as i32 - 1);
    }

    // Validate required arguments
    let listen_port = match cli.listen_port {
        Some(p) => p,
        None => {
            eprintln!("Listen port not specified");
            print_usage();
            process::exit(1);
        }
    };

    if cli.connect_address.is_none() && cli.connect_host.is_none() {
        eprintln!("Connect host or address not specified");
        print_usage();
        process::exit(1);
    }

    let connect_port = match cli.connect_port {
        Some(p) => p,
        None => {
            eprintln!("Connect port not specified");
            print_usage();
            process::exit(1);
        }
    };

    // listen-sender-address and listen-sender-port must both be set or both unset
    let lsaddr = cli.listen_sender_address.clone();
    let lsport = cli.listen_sender_port;
    if lsaddr.is_some() != lsport.is_some() {
        eprintln!("Options --listen-sender-port and --listen-sender-address must either both be specified or none");
        print_usage();
        process::exit(1);
    }

    let mut lstrict = cli.listen_address_strict;
    let cstrict = cli.connect_address_strict;

    // lstrict implied when listen-sender-address/port are both set
    if lsaddr.is_some() && lsport.is_some() {
        lstrict = true;
    }

    // Determine eignore: --stop-errors overrides --ignore-errors; default is ignore
    let eignore = if cli.stop_errors { false } else { true };

    // Resolve connect host if provided (overwrites connect_address)
    let caddr_str: String = if let Some(ref host) = cli.connect_host {
        debug!(debug_level, DEBUG_LEVEL_INFO, "Connect host: {}", host);
        let resolved = resolve_host(debug_level, host);
        debug!(debug_level, DEBUG_LEVEL_INFO, "Connect address: {}", resolved);
        resolved
    } else {
        cli.connect_address.clone().unwrap()
    };

    debug!(debug_level, DEBUG_LEVEL_INFO, "---- INFO ----");
    debug!(debug_level, DEBUG_LEVEL_INFO, "Listen address: {}",
        cli.listen_address.as_deref().unwrap_or("ANY"));
    debug!(debug_level, DEBUG_LEVEL_INFO, "Listen port: {}", listen_port);
    debug!(debug_level, DEBUG_LEVEL_INFO, "Listen interface: {}",
        cli.listen_interface.as_deref().unwrap_or("ANY"));
    debug!(debug_level, DEBUG_LEVEL_INFO, "Connect address: {}", caddr_str);
    debug!(debug_level, DEBUG_LEVEL_INFO, "Connect port: {}", connect_port);
    debug!(debug_level, DEBUG_LEVEL_INFO, "Send address: {}",
        cli.send_address.as_deref().unwrap_or("ANY"));
    if let Some(sp) = cli.send_port {
        debug!(debug_level, DEBUG_LEVEL_INFO, "Send port: {}", sp);
    } else {
        debug!(debug_level, DEBUG_LEVEL_INFO, "Send port: ANY");
    }
    debug!(debug_level, DEBUG_LEVEL_INFO, "Send interface: {}",
        cli.send_interface.as_deref().unwrap_or("ANY"));
    debug!(debug_level, DEBUG_LEVEL_INFO, "Listen strict: {}",
        if lstrict { "ENABLED" } else { "DISABLED" });
    debug!(debug_level, DEBUG_LEVEL_INFO, "Connect strict: {}",
        if cstrict { "ENABLED" } else { "DISABLED" });
    if let Some(ref a) = lsaddr {
        debug!(debug_level, DEBUG_LEVEL_INFO, "Listen only accepts packets from address: {}", a);
    }
    if let Some(p) = lsport {
        debug!(debug_level, DEBUG_LEVEL_INFO, "Listen only accepts packets from port: {}", p);
    }
    debug!(debug_level, DEBUG_LEVEL_INFO, "Ignore errors: {}",
        if eignore { "ENABLED" } else { "DISABLED" });
    debug!(debug_level, DEBUG_LEVEL_INFO, "Display stats: {}",
        if cli.stats { "ENABLED" } else { "DISABLED" });
    debug!(debug_level, DEBUG_LEVEL_INFO, "---- START ----");

    // Parse connect address to determine address family for both sockets
    let caddr = match parse_addr(&caddr_str, connect_port) {
        Some((sa, _)) => sa,
        None => {
            debug!(debug_level, DEBUG_LEVEL_ERROR, "Invalid connect address: {}", caddr_str);
            process::exit(1);
        }
    };
    let xfamily = caddr.ss_family as libc::c_int;

    let (lsock, lsock_name) = socket_setup(
        debug_level,
        "Listen",
        cli.listen_address.as_deref(),
        listen_port,
        cli.listen_interface.as_deref(),
        xfamily,
    );

    let (ssock, ssock_name) = socket_setup(
        debug_level,
        "Send",
        cli.send_address.as_deref(),
        cli.send_port.unwrap_or(0),
        cli.send_interface.as_deref(),
        xfamily,
    );

    // previous_endpoint: ss_family == 0 means "no endpoint seen yet"
    let mut previous_endpoint: libc::sockaddr_storage = unsafe { mem::zeroed() };

    // If listen-sender-address/port are set, pre-populate previous_endpoint
    if let (Some(ref addr_s), Some(port_s)) = (&lsaddr, lsport) {
        match parse_addr(addr_s, port_s) {
            Some((sa, _)) => {
                previous_endpoint = sa;
            }
            None => {
                debug!(debug_level, DEBUG_LEVEL_ERROR, "Invalid listen sender address: {}", addr_s);
                process::exit(1);
            }
        }
    }

    // Build errno ignore set
    const MAX_ERRNO: usize = 256;
    let mut errno_ignore = vec![false; MAX_ERRNO];
    errno_ignore[libc::EINTR as usize] = true;
    if eignore {
        errno_ignore[libc::EAGAIN as usize] = true;
        errno_ignore[libc::EHOSTUNREACH as usize] = true;
        errno_ignore[libc::ENETDOWN as usize] = true;
        errno_ignore[libc::ENETUNREACH as usize] = true;
        errno_ignore[libc::ENOBUFS as usize] = true;
        errno_ignore[libc::EPIPE as usize] = true;
        errno_ignore[libc::EADDRNOTAVAIL as usize] = true;
    }

    debug!(debug_level, DEBUG_LEVEL_VERBOSE, "entering infinite loop");

    let mut st = Statistics::default();
    let start_ts = current_timestamp();
    st.time_display_first = start_ts;
    st.time_display_last = start_ts;

    let mut network_buffer = vec![0u8; NETWORK_BUFFER_SIZE];

    // Main loop
    loop {
        let now = current_timestamp();

        if cli.stats && (now - st.time_display_last) >= STATISTICS_DELAY_SECONDS {
            statistics_display(debug_level, &mut st, now);
            st.time_display_last = now;
        }

        let mut ufds = [
            libc::pollfd { fd: lsock, events: libc::POLLIN | libc::POLLPRI, revents: 0 },
            libc::pollfd { fd: ssock, events: libc::POLLIN | libc::POLLPRI, revents: 0 },
        ];

        debug!(debug_level, DEBUG_LEVEL_DEBUG, "waiting for readable sockets");

        let poll_ret = unsafe { libc::poll(ufds.as_mut_ptr(), 2, 1000) };
        if poll_ret == -1 {
            let e = errno();
            if e == libc::EINTR {
                continue;
            }
            debug!(debug_level, DEBUG_LEVEL_ERROR, "Could not check readable sockets ({})", e);
            process::exit(1);
        }
        if poll_ret == 0 {
            debug!(debug_level, DEBUG_LEVEL_DEBUG, "poll timeout");
            continue;
        }

        // ── Listen socket readable ──────────────────────────────────────
        if (ufds[0].revents & (libc::POLLIN | libc::POLLPRI)) != 0 {
            let mut endpoint: libc::sockaddr_storage = unsafe { mem::zeroed() };
            let mut endpoint_len = mem::size_of::<libc::sockaddr_storage>() as libc::socklen_t;

            let recv_ret = unsafe {
                libc::recvfrom(
                    lsock,
                    network_buffer.as_mut_ptr() as *mut libc::c_void,
                    network_buffer.len(),
                    0,
                    &mut endpoint as *mut _ as *mut libc::sockaddr,
                    &mut endpoint_len,
                )
            };

            if recv_ret == -1 {
                let e = errno();
                if !errno_is_ignored(&errno_ignore, e) {
                    debug!(debug_level, DEBUG_LEVEL_INFO, "Listen cannot receive ({})", e);
                    process::exit(1);
                }
            }

            if recv_ret > 0 {
                st.count_listen_packet_receive += 1;
                st.count_listen_byte_receive += recv_ret as u64;

                debug!(debug_level, DEBUG_LEVEL_DEBUG,
                    "RECEIVE ({}, {}) -> ({}, {}) (LISTEN PORT): {} bytes",
                    addr_tostring(&endpoint), addr_port(&endpoint),
                    addr_tostring(&lsock_name), addr_port(&lsock_name),
                    recv_ret
                );

                // Accept if: no previous endpoint, OR not strict, OR same endpoint
                let accept = addr_is_unset(&previous_endpoint)
                    || !lstrict
                    || addr_equal(&previous_endpoint, &endpoint);

                if accept {
                    // Update previous_endpoint if not locked (unset or non-strict)
                    if addr_is_unset(&previous_endpoint) || !lstrict {
                        if !addr_equal(&previous_endpoint, &endpoint) {
                            debug!(debug_level, DEBUG_LEVEL_DEBUG,
                                "LISTEN remote endpoint set to ({}, {})",
                                addr_tostring(&endpoint), addr_port(&endpoint)
                            );
                        }
                        previous_endpoint = endpoint;
                    }

                    let send_ret = unsafe {
                        libc::sendto(
                            ssock,
                            network_buffer.as_ptr() as *const libc::c_void,
                            recv_ret as usize,
                            0,
                            &caddr as *const _ as *const libc::sockaddr,
                            addr_len(&caddr),
                        )
                    };

                    if send_ret == -1 {
                        let e = errno();
                        if !errno_is_ignored(&errno_ignore, e) {
                            debug!(debug_level, DEBUG_LEVEL_ERROR, "Cannot send packet to send port ({})", e);
                            process::exit(1);
                        }
                    } else {
                        st.count_connect_packet_send += 1;
                        st.count_connect_byte_send += send_ret as u64;
                    }

                    let full_or_ignore = send_ret == recv_ret || eignore;
                    let level = if full_or_ignore { DEBUG_LEVEL_DEBUG } else { DEBUG_LEVEL_ERROR };
                    debug!(debug_level, level,
                        "SEND ({}, {}) -> ({}, {}) (SEND PORT): {} bytes ({} WRITE {} bytes)",
                        addr_tostring(&ssock_name), addr_port(&ssock_name),
                        addr_tostring(&caddr), addr_port(&caddr),
                        send_ret,
                        if send_ret == recv_ret { "FULL" } else { "PARTIAL" },
                        recv_ret
                    );
                } else {
                    debug!(debug_level, DEBUG_LEVEL_ERROR,
                        "LISTEN PORT invalid source ({}, {}), was expecting ({}, {})",
                        addr_tostring(&endpoint), addr_port(&endpoint),
                        addr_tostring(&previous_endpoint), addr_port(&previous_endpoint)
                    );
                }
            }
        }

        // ── Send socket readable ────────────────────────────────────────
        if (ufds[1].revents & (libc::POLLIN | libc::POLLPRI)) != 0 {
            let mut endpoint: libc::sockaddr_storage = unsafe { mem::zeroed() };
            let mut endpoint_len = mem::size_of::<libc::sockaddr_storage>() as libc::socklen_t;

            let recv_ret = unsafe {
                libc::recvfrom(
                    ssock,
                    network_buffer.as_mut_ptr() as *mut libc::c_void,
                    network_buffer.len(),
                    0,
                    &mut endpoint as *mut _ as *mut libc::sockaddr,
                    &mut endpoint_len,
                )
            };

            if recv_ret == -1 {
                let e = errno();
                if !errno_is_ignored(&errno_ignore, e) {
                    debug!(debug_level, DEBUG_LEVEL_INFO, "Send cannot receive packet ({})", e);
                    process::exit(1);
                }
            }

            if recv_ret > 0 {
                st.count_connect_packet_receive += 1;
                st.count_connect_byte_receive += recv_ret as u64;

                debug!(debug_level, DEBUG_LEVEL_DEBUG,
                    "RECEIVE ({}, {}) -> ({}, {}) (SEND PORT): {} bytes",
                    addr_tostring(&endpoint), addr_port(&endpoint),
                    addr_tostring(&ssock_name), addr_port(&ssock_name),
                    recv_ret
                );

                // Accept if: previous_endpoint known AND (not cstrict OR from caddr)
                let accept = !addr_is_unset(&previous_endpoint)
                    && (!cstrict || addr_equal(&caddr, &endpoint));

                if accept {
                    let send_ret = unsafe {
                        libc::sendto(
                            lsock,
                            network_buffer.as_ptr() as *const libc::c_void,
                            recv_ret as usize,
                            0,
                            &previous_endpoint as *const _ as *const libc::sockaddr,
                            addr_len(&previous_endpoint),
                        )
                    };

                    if send_ret == -1 {
                        let e = errno();
                        if !errno_is_ignored(&errno_ignore, e) {
                            debug!(debug_level, DEBUG_LEVEL_INFO, "Cannot send packet to listen port ({})", e);
                            process::exit(1);
                        }
                    } else {
                        st.count_listen_packet_send += 1;
                        st.count_listen_byte_send += send_ret as u64;
                    }

                    let full_or_ignore = send_ret == recv_ret || eignore;
                    let level = if full_or_ignore { DEBUG_LEVEL_DEBUG } else { DEBUG_LEVEL_ERROR };
                    debug!(debug_level, level,
                        "SEND ({}, {}) -> ({}, {}) (LISTEN PORT): {} bytes ({} WRITE {} bytes)",
                        addr_tostring(&lsock_name), addr_port(&lsock_name),
                        addr_tostring(&previous_endpoint), addr_port(&previous_endpoint),
                        send_ret,
                        if send_ret == recv_ret { "FULL" } else { "PARTIAL" },
                        recv_ret
                    );
                } else {
                    debug!(debug_level, DEBUG_LEVEL_ERROR,
                        "SEND PORT invalid source ({}, {}), was expecting ({}, {})",
                        addr_tostring(&endpoint), addr_port(&endpoint),
                        addr_tostring(&caddr), addr_port(&caddr)
                    );
                }
            }
        }
    }
}

fn print_usage() {
    eprintln!("Usage: udp-redirect-rs");
    eprintln!("          [--listen-address <address>] --listen-port <port> [--listen-interface <interface>]");
    eprintln!("          [--connect-address <address> | --connect-host <hostname>] --connect-port <port>");
    eprintln!("          [--send-address <address>] [--send-port <port>] [--send-interface <interface>]");
    eprintln!("          [--listen-address-strict] [--connect-address-strict]");
    eprintln!("          [--listen-sender-address <address>] [--listen-sender-port <port>]");
    eprintln!("          [--ignore-errors] [--stop-errors]");
    eprintln!("          [--stats] [--verbose] [--debug] [--version]");
}

// ──────────────────────────────────────────────────────────────────────────
// Unit tests
// ──────────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    // ── parse_addr ───────────────────────────────────────────────────────

    #[test]
    fn parse_addr_ipv4_loopback() {
        let (sa, family) = parse_addr("127.0.0.1", 80).expect("should parse");
        assert_eq!(family, libc::AF_INET);
        assert_eq!(sa.ss_family as libc::c_int, libc::AF_INET);
        assert_eq!(addr_port(&sa), 80);
    }

    #[test]
    fn parse_addr_ipv6_loopback() {
        let (sa, family) = parse_addr("::1", 443).expect("should parse");
        assert_eq!(family, libc::AF_INET6);
        assert_eq!(sa.ss_family as libc::c_int, libc::AF_INET6);
        assert_eq!(addr_port(&sa), 443);
    }

    #[test]
    fn parse_addr_255_255_255_255() {
        assert!(parse_addr("255.255.255.255", 0).is_some());
    }

    #[test]
    fn parse_addr_2001_db8() {
        assert!(parse_addr("2001:db8::1", 0).is_some());
    }

    #[test]
    fn parse_addr_double_colon() {
        assert!(parse_addr("::", 0).is_some());
    }

    #[test]
    fn parse_addr_fe80_1() {
        assert!(parse_addr("fe80::1", 0).is_some());
    }

    #[test]
    fn parse_addr_ipv4_mapped() {
        assert!(parse_addr("::ffff:192.0.2.1", 0).is_some());
    }

    #[test]
    fn parse_addr_full_ipv6() {
        assert!(parse_addr("0:0:0:0:0:0:0:1", 0).is_some());
    }

    #[test]
    fn parse_addr_invalid_gggg() {
        assert!(parse_addr("gggg::1", 0).is_none());
    }

    #[test]
    fn parse_addr_invalid_too_many_groups() {
        assert!(parse_addr("1:2:3:4:5:6:7:8:9", 0).is_none());
    }

    #[test]
    fn parse_addr_invalid_hostname() {
        assert!(parse_addr("not.an.address", 0).is_none());
    }

    #[test]
    fn parse_addr_invalid_999() {
        assert!(parse_addr("999.0.0.1", 0).is_none());
    }

    #[test]
    fn parse_addr_empty_string() {
        assert!(parse_addr("", 0).is_none());
    }

    #[test]
    fn parse_addr_zone_id_rejected() {
        assert!(parse_addr("::1%eth0", 0).is_none());
    }

    // ── addr_port ────────────────────────────────────────────────────────

    #[test]
    fn addr_port_ipv4_443() {
        let (sa, _) = parse_addr("127.0.0.1", 443).unwrap();
        assert_eq!(addr_port(&sa), 443);
    }

    #[test]
    fn addr_port_ipv6_8080() {
        let (sa, _) = parse_addr("::1", 8080).unwrap();
        assert_eq!(addr_port(&sa), 8080);
    }

    #[test]
    fn addr_port_zero() {
        let (sa, _) = parse_addr("127.0.0.1", 0).unwrap();
        assert_eq!(addr_port(&sa), 0);
    }

    #[test]
    fn addr_port_65535() {
        let (sa, _) = parse_addr("127.0.0.1", 65535).unwrap();
        assert_eq!(addr_port(&sa), 65535);
    }

    // ── addr_is_unset ────────────────────────────────────────────────────

    #[test]
    fn addr_is_unset_zeroed() {
        let sa: libc::sockaddr_storage = unsafe { mem::zeroed() };
        assert!(addr_is_unset(&sa));
    }

    #[test]
    fn addr_is_unset_ipv4_is_set() {
        let (sa, _) = parse_addr("127.0.0.1", 80).unwrap();
        assert!(!addr_is_unset(&sa));
    }

    #[test]
    fn addr_is_unset_ipv6_is_set() {
        let (sa, _) = parse_addr("::1", 80).unwrap();
        assert!(!addr_is_unset(&sa));
    }

    // ── addr_equal ───────────────────────────────────────────────────────

    #[test]
    fn addr_equal_same_ipv4() {
        let (a, _) = parse_addr("1.2.3.4", 80).unwrap();
        let (b, _) = parse_addr("1.2.3.4", 80).unwrap();
        assert!(addr_equal(&a, &b));
    }

    #[test]
    fn addr_equal_diff_port_ipv4() {
        let (a, _) = parse_addr("1.2.3.4", 80).unwrap();
        let (b, _) = parse_addr("1.2.3.4", 81).unwrap();
        assert!(!addr_equal(&a, &b));
    }

    #[test]
    fn addr_equal_diff_addr_ipv4() {
        let (a, _) = parse_addr("1.2.3.4", 80).unwrap();
        let (b, _) = parse_addr("1.2.3.5", 80).unwrap();
        assert!(!addr_equal(&a, &b));
    }

    #[test]
    fn addr_equal_same_ipv6() {
        let (a, _) = parse_addr("::1", 443).unwrap();
        let (b, _) = parse_addr("::1", 443).unwrap();
        assert!(addr_equal(&a, &b));
    }

    #[test]
    fn addr_equal_diff_port_ipv6() {
        let (a, _) = parse_addr("::1", 443).unwrap();
        let (b, _) = parse_addr("::1", 444).unwrap();
        assert!(!addr_equal(&a, &b));
    }

    #[test]
    fn addr_equal_diff_addr_ipv6() {
        let (a, _) = parse_addr("::1", 443).unwrap();
        let (b, _) = parse_addr("::2", 443).unwrap();
        assert!(!addr_equal(&a, &b));
    }

    #[test]
    fn addr_equal_diff_families() {
        let (a, _) = parse_addr("127.0.0.1", 80).unwrap();
        let (b, _) = parse_addr("::1", 80).unwrap();
        assert!(!addr_equal(&a, &b));
    }

    // ── int_to_human_value ───────────────────────────────────────────────

    #[test]
    fn human_value_zero() {
        assert!((int_to_human_value(0.0) - 0.0).abs() < 1e-9);
    }

    #[test]
    fn human_value_999() {
        assert!((int_to_human_value(999.0) - 999.0).abs() < 1e-9);
    }

    #[test]
    fn human_value_1000() {
        assert!((int_to_human_value(1000.0) - 1.0).abs() < 1e-9);
    }

    #[test]
    fn human_value_1500() {
        assert!((int_to_human_value(1500.0) - 1.5).abs() < 1e-9);
    }

    #[test]
    fn human_value_1e6() {
        assert!((int_to_human_value(1_000_000.0) - 1.0).abs() < 1e-9);
    }

    #[test]
    fn human_value_1e9() {
        assert!((int_to_human_value(1_000_000_000.0) - 1.0).abs() < 1e-9);
    }

    // ── int_to_human_char ────────────────────────────────────────────────

    #[test]
    fn human_char_0() {
        assert_eq!(int_to_human_char(0.0), ' ');
    }

    #[test]
    fn human_char_1000() {
        assert_eq!(int_to_human_char(1000.0), 'K');
    }

    #[test]
    fn human_char_1e6() {
        assert_eq!(int_to_human_char(1e6), 'M');
    }

    #[test]
    fn human_char_1e9() {
        assert_eq!(int_to_human_char(1e9), 'G');
    }

    #[test]
    fn human_char_1e12() {
        assert_eq!(int_to_human_char(1e12), 'T');
    }

    #[test]
    fn human_char_1e15() {
        assert_eq!(int_to_human_char(1e15), 'P');
    }

    #[test]
    fn human_char_1e18() {
        assert_eq!(int_to_human_char(1e18), 'E');
    }

    #[test]
    fn human_char_1e21_clamp() {
        // Should clamp at 'E' (index 6)
        assert_eq!(int_to_human_char(1e21), 'E');
    }

    // ── parse_addr (additional) ──────────────────────────────────────────

    #[test]
    fn parse_addr_five_octet() {
        assert!(parse_addr("1.2.3.4.5", 80).is_none());
    }

    #[test]
    fn parse_addr_ipv4_port_stored() {
        let (sa, _) = parse_addr("127.0.0.1", 1234).unwrap();
        assert_eq!(addr_port(&sa), 1234);
    }

    #[test]
    fn parse_addr_ipv6_port_stored() {
        let (sa, _) = parse_addr("::1", 5678).unwrap();
        assert_eq!(addr_port(&sa), 5678);
    }

    #[test]
    fn parse_addr_double_colon_port_stored() {
        let (sa, _) = parse_addr("::", 80).unwrap();
        assert_eq!(addr_port(&sa), 80);
    }

    #[test]
    fn parse_addr_full_ipv6_port_stored() {
        let (sa, _) = parse_addr("0:0:0:0:0:0:0:1", 8080).unwrap();
        assert_eq!(addr_port(&sa), 8080);
    }

    #[test]
    fn parse_addr_port_zero_stored() {
        let (sa, _) = parse_addr("10.0.0.1", 0).unwrap();
        assert_eq!(addr_port(&sa), 0);
    }

    // ── addr_tostring ────────────────────────────────────────────────────

    #[test]
    fn addr_tostring_ipv4() {
        let (sa, _) = parse_addr("192.168.1.1", 0).unwrap();
        assert_eq!(addr_tostring(&sa), "192.168.1.1");
    }

    #[test]
    fn addr_tostring_ipv6_loopback() {
        let (sa, _) = parse_addr("::1", 0).unwrap();
        assert_eq!(addr_tostring(&sa), "::1");
    }

    #[test]
    fn addr_tostring_broadcast() {
        let (sa, _) = parse_addr("255.255.255.255", 0).unwrap();
        assert_eq!(addr_tostring(&sa), "255.255.255.255");
    }

    #[test]
    fn addr_tostring_unset_returns_question_mark() {
        let sa: libc::sockaddr_storage = unsafe { mem::zeroed() };
        assert_eq!(addr_tostring(&sa), "?");
    }

    // ── addr_len ─────────────────────────────────────────────────────────

    #[test]
    fn addr_len_ipv4() {
        let (sa, _) = parse_addr("1.2.3.4", 0).unwrap();
        assert_eq!(addr_len(&sa), mem::size_of::<libc::sockaddr_in>() as libc::socklen_t);
    }

    #[test]
    fn addr_len_ipv6() {
        let (sa, _) = parse_addr("::1", 0).unwrap();
        assert_eq!(addr_len(&sa), mem::size_of::<libc::sockaddr_in6>() as libc::socklen_t);
    }

    // ── addr_equal (additional) ──────────────────────────────────────────

    #[test]
    fn addr_equal_reflexive() {
        let (a, _) = parse_addr("192.168.0.1", 80).unwrap();
        assert!(addr_equal(&a, &a));
    }

    // ── int_to_human_value (additional) ──────────────────────────────────

    #[test]
    fn human_value_1_5e6() {
        assert!((int_to_human_value(1_500_000.0) - 1.5).abs() < 1e-9);
    }

    #[test]
    fn human_value_negative() {
        assert!((int_to_human_value(-500.0) - (-500.0)).abs() < 1e-9);
    }

    // ── int_to_human_char (additional) ───────────────────────────────────

    #[test]
    fn human_char_999() {
        assert_eq!(int_to_human_char(999.0), ' ');
    }

    #[test]
    fn human_char_1500() {
        assert_eq!(int_to_human_char(1500.0), 'K');
    }

    #[test]
    fn human_char_negative() {
        assert_eq!(int_to_human_char(-1.0), ' ');
    }

    #[test]
    fn human_value_and_char_agree_at_1000() {
        assert_eq!(int_to_human_char(1000.0), 'K');
        assert!((int_to_human_value(1000.0) - 1.0).abs() < 1e-9);
    }

    #[test]
    fn human_value_and_char_agree_at_1e6() {
        assert_eq!(int_to_human_char(1e6), 'M');
        assert!((int_to_human_value(1e6) - 1.0).abs() < 1e-9);
    }

    // ── Statistics::default ──────────────────────────────────────────────

    #[test]
    fn statistics_default_all_zero() {
        let st = Statistics::default();
        assert_eq!(st.count_listen_packet_receive,  0);
        assert_eq!(st.count_listen_byte_receive,    0);
        assert_eq!(st.count_listen_packet_send,     0);
        assert_eq!(st.count_listen_byte_send,       0);
        assert_eq!(st.count_connect_packet_receive, 0);
        assert_eq!(st.count_connect_byte_receive,   0);
        assert_eq!(st.count_connect_packet_send,    0);
        assert_eq!(st.count_connect_byte_send,      0);
        assert_eq!(st.count_listen_packet_receive_total,  0);
        assert_eq!(st.count_connect_byte_send_total,      0);
        assert_eq!(st.time_display_first, 0);
        assert_eq!(st.time_display_last,  0);
    }

    // ── statistics_display ───────────────────────────────────────────────

    #[test]
    fn statistics_display_accumulates_to_totals() {
        let mut st = Statistics::default();
        let t0: i64 = 1_000_000;
        st.time_display_first = t0 - 120;
        st.time_display_last  = t0 - 60;

        st.count_listen_packet_receive  = 100;
        st.count_listen_byte_receive    = 50_000;
        st.count_listen_packet_send     = 90;
        st.count_listen_byte_send       = 45_000;
        st.count_connect_packet_receive = 80;
        st.count_connect_byte_receive   = 40_000;
        st.count_connect_packet_send    = 70;
        st.count_connect_byte_send      = 35_000;

        statistics_display(DEBUG_LEVEL_ERROR, &mut st, t0);

        assert_eq!(st.count_listen_packet_receive_total,  100);
        assert_eq!(st.count_listen_byte_receive_total,    50_000);
        assert_eq!(st.count_listen_packet_send_total,     90);
        assert_eq!(st.count_listen_byte_send_total,       45_000);
        assert_eq!(st.count_connect_packet_receive_total, 80);
        assert_eq!(st.count_connect_byte_receive_total,   40_000);
        assert_eq!(st.count_connect_packet_send_total,    70);
        assert_eq!(st.count_connect_byte_send_total,      35_000);
    }

    #[test]
    fn statistics_display_resets_window_counters() {
        let mut st = Statistics::default();
        let t0: i64 = 1_000_000;
        st.time_display_first = t0 - 120;
        st.time_display_last  = t0 - 60;

        st.count_listen_packet_receive  = 100;
        st.count_listen_byte_receive    = 50_000;
        st.count_listen_packet_send     = 90;
        st.count_listen_byte_send       = 45_000;
        st.count_connect_packet_receive = 80;
        st.count_connect_byte_receive   = 40_000;
        st.count_connect_packet_send    = 70;
        st.count_connect_byte_send      = 35_000;

        statistics_display(DEBUG_LEVEL_ERROR, &mut st, t0);

        assert_eq!(st.count_listen_packet_receive,  0);
        assert_eq!(st.count_listen_byte_receive,    0);
        assert_eq!(st.count_listen_packet_send,     0);
        assert_eq!(st.count_listen_byte_send,       0);
        assert_eq!(st.count_connect_packet_receive, 0);
        assert_eq!(st.count_connect_byte_receive,   0);
        assert_eq!(st.count_connect_packet_send,    0);
        assert_eq!(st.count_connect_byte_send,      0);
    }

    #[test]
    fn statistics_display_totals_accumulate_across_calls() {
        let mut st = Statistics::default();
        let t0: i64 = 1_000_000;
        st.time_display_first = t0 - 120;
        st.time_display_last  = t0 - 60;

        st.count_listen_packet_receive = 100;
        st.count_listen_byte_receive   = 50_000;
        statistics_display(DEBUG_LEVEL_ERROR, &mut st, t0);

        st.count_listen_packet_receive = 50;
        st.count_listen_byte_receive   = 25_000;
        statistics_display(DEBUG_LEVEL_ERROR, &mut st, t0 + 60);

        assert_eq!(st.count_listen_packet_receive_total, 150);
        assert_eq!(st.count_listen_byte_receive_total,   75_000);
    }

    #[test]
    fn statistics_display_zero_time_delta_clamped() {
        let mut st = Statistics::default();
        let t0: i64 = 1_000_000;
        st.time_display_first = t0;
        st.time_display_last  = t0;
        st.count_listen_packet_receive = 10;

        // delta == 0 must be clamped to 1; must not panic
        statistics_display(DEBUG_LEVEL_ERROR, &mut st, t0);
        assert_eq!(st.count_listen_packet_receive_total, 10);
    }

    #[test]
    fn statistics_display_debug_level_not_mutated() {
        let mut st = Statistics::default();
        let t0: i64 = 1_000_000;
        st.time_display_first = t0 - 60;
        st.time_display_last  = t0 - 60;

        let dl = DEBUG_LEVEL_ERROR;
        statistics_display(dl, &mut st, t0);
        assert_eq!(dl, DEBUG_LEVEL_ERROR);
    }
}
