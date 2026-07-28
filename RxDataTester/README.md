# RxDataTester (v.1.6)

RxDataTester is a Qt 5.12 + qmake utility that receives a binary counter
pattern through COM or UDP and verifies that counter values increase
continuously. The GUI, source comments, EVENTS messages, and text logs are
entirely in English.

## Version 1.6 high-rate UDP receiver

Version 1.6 keeps the persistent 65,507-byte payload buffer, bounded batch
processing, 16 MiB receive buffer request, and native-data watchdog introduced
in version 1.5. It fixes a START failure specific to Qt 5.12 on Windows.

Version 1.5 deliberately called `readDatagram()` once even when the UDP queue
was empty, intending to rearm a stale `readyRead` notification. On Windows, an
empty nonblocking `WSARecvFrom` returns `WSAEWOULDBLOCK`; Qt 5.12 can expose that
result as `QAbstractSocket::NetworkError` instead of `TemporaryError`. The START
cleanup therefore produced `UDP discard error: network error (code 7)` and
closed an otherwise healthy bound socket.

The version 1.6 receive policy is:

- never issue an unconditional empty `readDatagram()` call;
- read immediately from a real `readyRead` callback only when Qt reports a
  pending datagram;
- use native `FIONREAD` as a watchdog fallback when Qt notification delivery
  stalls but data is present in the operating-system queue;
- after native data is confirmed, a successful `readDatagram()` both consumes
  the packet and rearms Qt read notifications;
- classify `WSAEWOULDBLOCK`, `EAGAIN`, `EWOULDBLOCK`, and interrupted reads as
  temporary races instead of fatal socket failures;
- include both Qt and native error codes in a genuine UDP read or discard error.

The high-rate optimizations remain:

- one reusable 65,507-byte `QByteArray` is allocated in the UDP worker thread;
  no payload array is allocated or resized for every datagram;
- a real `QUdpSocket::readyRead` notification is handled immediately;
- queued backlog processing uses a separate `continueReadBatch()` slot;
- each receive pass is limited to approximately 2 ms or 4,096 datagrams, then
  yields to the worker event loop so STOP and Statistics remain responsive;
- a 10 ms watchdog checks both Qt pending-datagram state and native `FIONREAD`;
  if data remains queued and no datagram has been read for 30 ms, reception is
  drained automatically;
- watchdog recovery EVENTS are rate-limited;
- the requested receive buffer is 16 MiB and the actual accepted value is
  written to EVENTS and the text log;
- 20-second log lines and STOP include readyRead, continuation, watchdog, read
  error, datagram, maximum-batch, and activity-gap diagnostics.

A typical connection entry is:

```text
UDP receiver ready: local=192.168.1.2:8890; expected_source=192.168.1.3; requested_rcvbuf=16777216; actual_rcvbuf=16777216; receive_buffer=65507; watchdog=10 ms; stall_threshold=30 ms
```

If the watchdog detects queued native data after Qt notification delivery has
stalled, it records a black service event and resumes draining:

```text
UDP RX watchdog recovery: no_read_for=37 ms; native_next_datagram_bytes=1280; qt_pending=true; recovered_datagrams=12; suppressed_recovery_events=0; ready_read_calls=4412; empty_ready_read_calls=1; read_continuations=18; watchdog_recoveries=1; suppressed_watchdog_events=0; read_errors=0; worker_datagrams=4423; max_read_batch=256; ready_read_gap_ms=37; datagram_gap_ms=0
```

## Build

Open `RxDataTester.pro` in Qt Creator configured for Qt 5.12, or build from a
Qt command prompt:

```text
qmake RxDataTester.pro
mingw32-make
```

For an MSVC kit, use the corresponding `nmake` command. The project enables
UTF-8 source handling for MSVC. On Windows it explicitly links `ws2_32` because
the UDP diagnostics call `ioctlsocket(FIONREAD)` and `WSAGetLastError()`.

Required Qt modules:

```text
core gui widgets network serialport
```

## Source style

Doxygen comments are placed before functions and use `@brief`, `@param`,
`@return`, and `@detail`. Functions without parameters use `@param none`.
Functions without a return value use `@return none`. Function implementations
are separated with 81-character divider lines.

## Thread architecture

The application uses three event-loop threads:

```text
GUI thread
|- widgets and tab locking
|- EVENTS and text-log file
|- QSettings
|- COM-port discovery
|- ping and neighbor-table lookup

COM RX thread
|- RxWorker
|- QSerialPort and readyRead()
|- COM stream reassembly
|- counter verification
|- COM Statistics

UDP RX thread
|- UdpRxWorker
|- persistent bound QUdpSocket
|- persistent maximum-size payload buffer
|- immediate readyRead processing
|- bounded queued backlog draining
|- native receive watchdog
|- counter verification across datagrams
|- UDP Statistics and 20-second log snapshots
```

GUI painting, window movement, and log-file flushing do not execute in the COM
or UDP receiver threads.

## COM tab

The COM receiver retains the established behavior:

- available ports are refreshed once per second while the port is closed;
- OPEN applies Port, Baud, Parity, and stop-bit settings;
- START verifies a continuous little-endian counter stream;
- incomplete counter fields are retained until more COM bytes arrive;
- STOP ends verification but leaves the COM port open;
- a mismatch increments `counter, err`, logs expected and received values, and
  resynchronizes the next expected value to `received + 1`;
- `counter, ok`, `rx, bytes`, `curr_count`, and `speed, Kb/s` are updated by the
  COM worker.

## UDP Connection

The UDP Connection group contains:

```text
our IP: <local IPv4>       our MAC: <local interface MAC>
IP [expected transmitter]  Port [local listening port]  CONNECT
                               dest MAC: <transmitter MAC when resolvable>
```

`IP` is the IPv4 address of the expected transmitter. `Port` is the local UDP
destination port on which RxDataTester binds and receives datagrams. The
TxDataTester destination port must match this value. The transmitter source
port is not required to match it.

CONNECT performs these steps:

1. Validate IPv4 and Port.
2. Resolve the local interface selected by the operating-system route.
3. Display `our IP` and `our MAC`.
4. Send one short-timeout ping request.
5. If ping succeeds, bind a persistent UDP socket to `our IP:Port`.
6. Request a 16 MiB operating-system receive buffer and read back its actual
   value.
7. Start the receive watchdog.
8. Try to resolve `dest MAC` through the neighbor table.

A successful ping confirms IP reachability, not that the transmitter has
started sending UDP data. START becomes available only after the UDP socket has
been bound successfully.

The COM and UDP modes are mutually exclusive:

```text
UDP CONNECT -> COM tab is disabled until DISCONNECT
COM OPEN     -> UDP tab is disabled until CLOSE
```

## UDP Pattern and START

UDP Pattern contains:

- `counter, bits`: 8, 16, 32, or 64;
- `block, bytes`: expected transmitter payload size and Pattern description;
- `init value`: decimal or `0x`-prefixed hexadecimal;
- `Togeth`: informational number of packets in one transmitter burst;
- `Period, ms`: informational transmitter burst period;
- START and STOP.

When START is pressed, a bounded amount of data already queued before START is
discarded. The expected counter is initialized from `init value`. Each accepted
datagram from the configured source IP is decoded independently as a sequence
of little-endian unsigned counter values. Counter continuity is preserved
between adjacent UDP datagrams.

For a 32-bit counter and a 128-byte payload, one datagram contains:

```text
128 / 4 = 32 counter values
```

Starting from decimal 10, the first packet therefore contains values 10 through
41, and the next packet is expected to start with 42.

For each value:

```text
received == expected
    counter, ok += 1
    next expected = received + 1

received != expected
    counter, err += 1
    log expected, received, delta, and next_expected
    next expected = received + 1
```

The selected counter wraps from its maximum value to zero. If a UDP payload is
not divisible by the counter-field size, complete values are still checked and
the trailing bytes are reported as a red payload-alignment error. Trailing bytes
are not combined with the next datagram because UDP packet boundaries are
preserved.

Datagrams received from another source IP are ignored and reported once per
unexpected source during the current test.

## UDP Statistics

UDP Statistics is updated approximately once per second using the actual
monotonic interval:

- `start`: wall-clock time when START was accepted;
- `time`: elapsed time with unlimited hours;
- `rx, bytes`: total accepted UDP payload bytes; network headers are excluded;
- `curr_count`: last completely decoded counter value;
- `packets/s`: accepted datagrams divided by the real interval;
- `speed, Kb/s`: accepted payload bits divided by the real interval;
- `counter, ok`: total matching counter values;
- `counter, err`: total mismatching counter values.

STOP records a final Statistics snapshot and leaves the bound UDP socket ready
for another START. Datagrams received while the test is stopped are drained and
ignored so stale data cannot accumulate indefinitely.

## Twenty-second UDP log snapshot

While UDP reception is active, an additional line is written only to the text
log approximately every 20 seconds:

```text
19:44:06.361, time=00:00:20, rx_bytes=1782656, delta_rx_bytes=1782656, curr_counter=445689, counter_ok=445690, delta_counter_ok=445690, counter_err=0, delta_counter_err=0, min_packet/s=4078, avrg_packets/s=4352, max_packet/s=4788, min_speed_Kb/s=522, avrg_speed_Kb/s=544.18, max_speed_Kb/s=621, ready_read_calls=19984; empty_ready_read_calls=2; read_continuations=31; watchdog_recoveries=1; suppressed_watchdog_events=0; read_errors=0; worker_datagrams=20000; max_read_batch=184; ready_read_gap_ms=0; datagram_gap_ms=0
```

The minimum, average, and maximum packet rates and speeds are calculated from
the one-second values displayed during the current 20-second interval. The
average is the arithmetic mean of those samples. Delta counters are measured
from the previous 20-second line.

The appended read diagnostics make it possible to distinguish normal packet
loss from a stalled notification, a queued receive backlog, or a socket read
error.

## EVENTS and text logs

At startup the application creates a `logs` directory next to the executable
and opens a new UTF-8 file named like:

```text
rxdatatester_log__YYYY-MM-DD__HH-MM-SS-mmm.txt
```

Every EVENTS line is duplicated to the text log. Direct button presses are
green, errors are red, and service information is black. Worker events receive
timestamps in the worker thread in `HH:MM:SS.mmm` format.

A UDP counter mismatch is logged in this form:

```text
06:20:10.123 - UDP counter error: expected=33; received=63; delta=30; next_expected=64
```

## Saved settings

QSettings stores window geometry, COM settings, COM Pattern, UDP IP and Port,
and UDP Pattern values. COM and UDP connections are not restored automatically
after restart; use OPEN or CONNECT again.
