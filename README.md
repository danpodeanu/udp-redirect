# udp-redirect
A simple yet flexible and very fast UDP redirector. Tested to run on Linux x64 and MacOS / Darwin arm64.

Useful for redirecting UDP traffic (e.g., Wireguard VPN) where doing it at a different layer (e.g., from a firewall) is challenging / impossible.

[socat](http://www.dest-unreach.org/socat/) is a very good alternative, albeit not fit for all purposes.

[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)

**Community contributions are welcome.**

# Compile

```# make```

or

```gcc udp-redirect.c -o udp-redirect -O3```

# Documentation

Doxygen generated documentation in the ```doc``` folder (open ```index.html```).

# Command Line Arguments

```udp-redirect [arguments]```

Runs in foreground and expects process control to be managed externally (svscan, nohup, etc.)

## Debug

| Argument | Parameters | Req/Opt | Description |
| --- | --- | --- | --- |
| ```--verbose``` | | *optional* | Verbose mode, can be specified multiple times. |
| ```--debug``` | | *optional* | Debug mode (e.g., very verbose). |

## Listener

The UDP sender (e.g., wireguard client) sends packets to the UDP redirector here.

| Argument | Parameters | Req/Opt | Description |
| --- | --- | --- | --- |
| ```--laddr``` | address | *required* | Listen address. |
| ```--lport``` | port | *required* | Listen port. |
| ```--lif``` | interface | *optional* | Listen interface name. |
| ```--lstrict``` | | *optional* | **Security:** By default, packets received from the connect endpoint will be sent to the source of the last packet received on the listener endpoint. In ```lstrict``` mode, only accept packets from the same source as the first packet. For added security, when specified, the ```lsaddr``` and ```lsport``` parameters set the sender endpoint and ```lstrict``` (e.g., the listsener endpoint is known and fixed). |

## Connect

The UDP redirector sends packets here (e.g., to the wireguard server):

| Argument | Parameters | Req/Opt | Description |
| --- | --- | --- | --- |
| ```--caddr``` | address | *required* | Connect address. |
| ```--chost``` | address | *required* | Connect host, overwrites caddr if both are specified. |
| ```--cport``` | port | *required* | Connect port. |
| ```--cstrict``` | | *optional* | **Security**: Only accept packets from the connect caddr / cport, otherwise accept from all sources. |

# Sender

The UDP redirector sends packets from here (e.g., to the wireguard server). If any is missing, it will be selected by the operating system (usually ```0.0.0.0```, random port, default interface).

| Argument | Parameters | Req/Opt | Description |
| --- | --- | --- | --- |
| ```--saddr``` | address | *optional* | Send packets from address. |
| ```--sport``` | port | *optional* | Send packets from port. |
| ```--sif``` | interface | *optional* | Send packets from interface. |

# Listener security

Both must be specified; listener drops packets if they do not arrive from this address / port.

| Argument | Parameters | Req/Opt | Description |
| --- | --- | --- | --- |
| ```--lsaddr``` | address | *optional* | Listen address receives packets from this source address. |
| ```--lsport``` | port | *optional* | Listen port receives packets from this source port (must be set together, ```--lstrict``` is implied). |

# Miscellaneous

| Argument | Parameters | Req/Opt | Description |
| --- | --- | --- | --- |
| ```--ignore-errors``` | | *optional* | Ignore most receive or send errors (host / network unreachable, etc.) instead of exiting. |

# Example

```mermaid
graph TD;
    A-->B;
    A-->C;
    B-->D;
    C-->D;
```

```
./udp-redirect --debug --laddr 192.168.1.32 --lport 51820 --lif en0 --chost example.endpoint.net --cport 51820 --sif utun5 --lstrict --cstrict --lsaddr 192.168.1.1 --lsport 51820 --ignore-errors
```