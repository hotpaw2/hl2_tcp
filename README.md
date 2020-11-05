## hl2_tcp

This repository contains source code for hl2_tcp, 
a rtl_tcp server for [Hermes Lite 2](http://www.hermeslite.com)
SDR radio.

The Hermes-Lite is a low-cost
direct conversion 
software defined amateur radio HF transceiver
based on the AD9866 broadband modem chip,
an Altera Cyclone IV EP4CE22 FPGA,
and the [HPSDR/Hermes SDR project](http://openhpsdr.org). 


The hl2_tcp server 
can connect to a Hermes Lite 2 SDR 
via UDP over ethernet, 
transcode received RF IQ samples,
and then serve that data
via the rtl_tcp protocol.
 
[rtl_tcp](https://github.com/osmocom/rtl-sdr)
is a command-line tool developed by OsmoCom and others,
for various Realtek RTL2832-based DVB-T USB peripherals,
to serve IQ samples from those USB devices over TCP.
There are multiple SDR applications,
available for Linux, macOS, or Wintel systems,
that can connect to a local or remote SDR radio peripheral
via the rtl_tcp protocol.
This server allows using those SDR applications 
with a Hermes Lite 2 SDR,
but only if the application supports the
HL2 sample rates (48k, 96k, 192k, and 384k).
Among the SDR applications 
supporting those sample rates
are the iOS and macOS apps:
_rtl_tcp SDR_
and
_SDR Receiver_,
allowing HL2 SDR reception an iPhone, iPad, or Mac.

Usage:

    hl2_tcp -d

Discovers Hermes Lite 2 IP Address.
Prints random diagnostics.

    hl2_tcp -a Hermes_IP_Addr [-p tcp_server_port] [-b 8/16]

Starts a server for the rtl_tcp protocol
    on a local TCP server port (default rtl_tcp port 1234)
    and waits for a TCP connection.
Upon opening an rcp_tcp TCP connection,
    starts a UDP connection to the Hermes Lite 2
    at Hermes_IP_Addr on UDP port 1024,
    and transcodes OpenHPSDR/Metis UDP data to rtl_tcp TCP data.
    Also prints more random diagnostics.

Important note: Under macOS Catalina 10.15.7 or later, or macOS Big Sur,
this server may need to be installed and run from an Admin account,
due to new macOS firewall security/privacy features.

More information on the Hermes Lite 2 can be found on this web sites:
 [Hermes-Lite2 wiki](https://github.com/softerhardware/Hermes-Lite2/wiki)

--

rhn@nicholson.com  \
N6YWU  \
http://www.nicholson.com/rhn/

License: MPL 2.0 with exhibit B \
No warrantees implied.

--
