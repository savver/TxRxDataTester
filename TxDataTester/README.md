# TxDataTester (v.1.7)

TxDataTester is a Qt 5.12 test-pattern transmitter for COM and UDP transport.
The project uses qmake and C++11. CMake is not required.

The source code, Doxygen comments, GUI messages, EVENTS entries, and text-log
messages are written in English.

## Version 1.7 changes

Version 1.7 completes the UDP transmitter tab:

- one short-timeout IPv4 ping is used by `CONNECT` instead of four requests;
- the COM and UDP tabs are mutually locked while one transport is active;
- the UDP `Pattern` group contains the new `Togeth` burst-size field;
- `Period, ms` is placed on the third Pattern row opposite `SINGLE`;
- UDP `Statistics` contains `packets/s`, with `speed, Kb/s` on the fourth row;
- UDP packet generation and `QUdpSocket` operations run in a dedicated worker
  thread, independently of GUI repainting and log-file I/O;
- `START`, `STOP`, and `SINGLE` are implemented for UDP;
- payload bytes, current counter, packets per second, and payload speed are
  updated from real elapsed time;
- UDP Statistics are additionally written to the text log every 20 seconds;
- UDP destination, Pattern, and `Togeth` values are saved with `QSettings`.

## Build requirements

- Qt 5.12;
- Qt Widgets;
- Qt Network;
- Qt Serial Port;
- qmake;
- a C++11 compiler.

Example build commands:

```text
qmake TxDataTester.pro
make
```

On Windows with the appropriate Qt command prompt:

```text
qmake TxDataTester.pro
nmake
```

## Thread architecture

The application uses three event-loop threads.

### GUI thread

The main thread owns:

- `MainWindow` and all widgets;
- the shared `EVENTS` view;
- the UTF-8 text log and `flush()` operations;
- `QSettings`;
- COM-port enumeration through `QSerialPortInfo`;
- the external ping and neighbor-table helper processes.

### COM TX worker thread

`TxWorker` owns:

- `QSerialPort`;
- COM block generation;
- COM transmission timers;
- `bytesWritten()` processing;
- COM Statistics;
- the soft COM `STOP` state that lets queued serial data drain.

### UDP TX worker thread

`UdpTxWorker` owns:

- the persistent sending `QUdpSocket`;
- the ephemeral local UDP port reserved after a successful `CONNECT`;
- UDP burst generation;
- the UDP transmission timer;
- little-endian counter-pattern generation;
- UDP Statistics and 20-second log samples.

GUI repainting, window movement, and text-log flushing therefore do not execute
inside either transmission worker thread.

## COM tab

The COM implementation from v.1.5 is preserved.

- available COM ports are refreshed once per second while the port is closed;
- OPEN applies Port, Baud, Parity, and stop-bit settings;
- START transmits counter-filled blocks continuously;
- SINGLE transmits one block;
- STOP stops new block generation and lets the pending serial output drain;
- Statistics show start time, elapsed time, transmitted bytes, current counter,
  and payload speed.

While a COM port is open or a COM operation is pending, the UDP tab is disabled.
The UDP tab becomes available again only after the COM port is closed.

## UDP Connection group

The Connection group shows:

```text
our IP:  <local IPv4 address>    our MAC: <local interface MAC>
IP:      <destination IPv4>      Port:    <destination UDP port>   CONNECT
 dest MAC: <destination MAC when available>
```

The destination-IP editor accepts only decimal digits and dots. Full IPv4
validation is performed before connection.

The Port editor accepts only decimal digits. The allowed range is `1...65535`.

### CONNECT sequence

`CONNECT` performs the following steps:

1. validates the destination IPv4 address and UDP port;
2. asks the operating system to select the outgoing local IPv4 interface;
3. displays the selected local IP and MAC address;
4. starts one asynchronous IPv4 ping request;
5. applies a short ping timeout;
6. enables UDP transmission only when a ping reply is detected;
7. starts an ARP/neighbor-table lookup for the destination MAC;
8. asks `UdpTxWorker` to bind a persistent `QUdpSocket` to the selected local IP
   and an ephemeral local port.

On Windows, the native ping reply timeout is 500 ms. An application-level
watchdog terminates a ping process that runs longer than 1800 ms. Unix ping
utilities have platform-specific timeout granularity; the same watchdog still
limits the complete operation.

When CONNECT succeeds, destination IP and Port become read-only, the button text
changes to `DISCONNECT`, and the COM tab is disabled. The COM tab becomes
available again after `DISCONNECT` closes the UDP socket.

If ping fails, UDP START and SINGLE remain disabled and the COM tab is unlocked.

The destination MAC is normally available only for a destination in the same
layer-2 network. For a routed destination, the local neighbor table usually
contains the next-hop router MAC instead of the final remote-device MAC, so the
field may remain `--`.

## UDP Pattern group

The UDP Pattern controls are:

- `counter, bits`: `8`, `16`, `32`, or `64`;
- `block, bytes`: payload size of one UDP datagram;
- `init value`: decimal or `0x`-prefixed hexadecimal initial counter;
- `Togeth`: number of datagrams sent sequentially in one timer burst;
- `Period, ms`: delay between burst timer events;
- `START`, `STOP`, and `SINGLE`.

Only decimal digits are accepted in `block, bytes`, `Togeth`, and `Period, ms`.

`block, bytes` is rounded upward to a multiple of the selected counter size and
is limited to the maximum IPv4 UDP payload size. For counter widths that require
alignment, the largest usable value is the largest aligned value not exceeding
65507 bytes.

### Counter payload format

Each UDP datagram contains only consecutive counter values. Values are encoded
little-endian (`bo=LE`). The counter continues across all datagrams and all
bursts and wraps to zero after the maximum value of the selected width.

For example:

```text
counter bits = 32
init value   = 0x10
block bytes  = 128
Togeth       = 4
Period, ms   = 5
```

A 32-bit counter occupies four bytes, so one 128-byte datagram contains 32
counter values. The mathematically correct ranges are:

```text
packet 1: 0x10 ... 0x2F
packet 2: 0x30 ... 0x4F
packet 3: 0x50 ... 0x6F
packet 4: 0x70 ... 0x8F
```

After approximately 5 ms, the next burst begins with `0x90`.

### START

START resets Statistics and the counter to `init value`, sends the first burst
immediately, then schedules later bursts according to `Period, ms`.

During each burst, `Togeth` complete datagrams are generated and passed to
`QUdpSocket::writeDatagram()` one after another without waiting for delivery to
the receiver.

`Period = 0` selects cooperative continuous mode: after one burst returns to the
worker event loop, another zero-timeout burst is scheduled as soon as possible.
This keeps the worker event loop responsive but intentionally adds no software
pause between bursts.

### STOP

STOP immediately stops generation of new UDP datagrams. Unlike the COM soft
STOP, UDP has no portable Qt acknowledgement that an already accepted datagram
has physically left the network adapter, so the application reports the totals
accepted by `writeDatagram()` at the stop moment.

### SINGLE

SINGLE sends exactly one UDP datagram. `Togeth` applies to continuous START
bursts only. Every SINGLE operation starts its payload counter from the current
`init value` field.

## UDP Statistics

UDP Statistics are updated approximately once per second using a monotonic
`QElapsedTimer` and the actual elapsed interval.

- `start:` — wall-clock time of START or SINGLE;
- `time:` — elapsed duration with unlimited hours;
- `tx, bytes` — total application payload bytes successfully accepted by
  `writeDatagram()`;
- `curr_count` — next counter value to be written to a new datagram;
- `packets/s` — datagrams accepted during the latest real interval divided by
  that interval;
- `speed, Kb/s` — payload bits accepted during the latest real interval divided
  by that interval.

The payload-speed calculation is:

```text
speed_Kb_s = delta_payload_bytes * 8 / delta_time_ms
```

IP, UDP, Ethernet, VLAN, preamble, and inter-frame overhead are not included.

A successful `writeDatagram()` result means that the complete UDP datagram was
accepted by the local socket/network stack. It is not a delivery confirmation
from the destination and is not a portable physical-NIC transmission-complete
notification.

## Twenty-second text-log Statistics

During UDP START, a detailed line is written only to the text log every 20
seconds. It is not displayed in EVENTS.

Example:

```text
19:44:06.361, mode=UDP, time=00:00:20, tx_bytes=1782656, delta_tx_bytes=1782656, tx_packets=13927, delta_tx_packets=13927, curr_counter=445680, min_speed=522, avrg_speed=544.18, max_speed=621, min_packets_s=4078, avrg_packets_s=4352, max_packets_s=4788
```

The average values use the exact elapsed interval. Minimum and maximum values
come from the approximately one-second Statistics samples inside the same
20-second interval.

## EVENTS and text logs

At every application start, a new UTF-8 file is created next to the executable:

```text
logs/txdatatester_log__YYYY-MM-DD__HH-MM-SS-mmm.txt
```

All EVENTS entries are duplicated in that file. Timestamps include
milliseconds:

```text
HH:MM:SS.mmm - event text
```

Colors in EVENTS:

- green — direct button presses;
- red — validation, port, socket, ping, and transmission errors;
- black — service information and operation results.

Typical UDP entries are:

```text
18:04:11.105 - CONNECT button pressed
18:04:11.106 - Connection Settings: dest_IP=192.168.1.20; dest_PORT=5000; our_IP=192.168.1.10; our_MAC=00:11:22:33:44:55
18:04:11.107 - PING started: dest_IP=192.168.1.20; requests=1; reply_timeout=500 ms
18:04:11.642 - PING result: dest_IP=192.168.1.20; replies=1/1; packet_loss=0%
18:04:11.650 - UDP socket ready: local=192.168.1.10:53124; destination=192.168.1.20:5000
18:04:14.100 - UDP START button pressed
18:04:14.101 - UDP START: continuous transmission started; Pattern: counter=32 bits; init=0x10 (16); block=128 bytes; Togeth=4; period=5 ms; values/packet=32; bo=LE
```

## Saved settings

The application restores the following values through `QSettings`:

- window geometry;
- COM Port, Baud, Parity, and stop bits;
- COM Pattern fields;
- UDP destination IP and Port;
- UDP counter width, block size, initial value, Togeth, and Period.

COM ports and UDP destinations are not automatically opened or connected after
restart.

## Source files

```text
TxDataTester.pro
main.cpp
mainwindow.h
mainwindow.cpp
mainwindow.ui
txworker.h
txworker.cpp
udptxworker.h
udptxworker.cpp
README.md
```
