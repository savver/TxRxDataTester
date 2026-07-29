# RxDataTester (v.1.7)

RxDataTester is a Qt 5.12 + qmake utility that receives a binary counter
pattern through COM or UDP and verifies that counter values increase
continuously. The GUI, source comments, EVENTS messages, and text logs are
entirely in English.

## Version 1.7 high-rate UDP receive pipeline

Version 1.7 changes the UDP hot path from direct receive-and-verify processing
into two bounded stages inside the dedicated UDP worker thread:

```text
QUdpSocket / native receive queue
        |
        | persistent 65,507-byte socket buffer
        v
preallocated packet ring
        |
        | zero-delay bounded processing timer
        v
little-endian counter verification and Statistics
```

The changes are intended for dense bursts and sustained high packet rates:

- one reusable 65,507-byte `QByteArray` remains the only socket read buffer;
- accepted datagrams are copied into a preallocated fixed-slot packet ring;
- the packet ring uses a bounded 32 MiB memory budget and up to 16,384 slots;
- no `QByteArray` is allocated, resized, or destroyed for each received packet;
- socket draining and counter verification have separate time budgets;
- a real `readyRead` notification still starts reception immediately;
- an independent 1 ms receive pump checks Qt and native `FIONREAD` state and
  drains data even if Qt 5.12/Windows stops delivering `readyRead` callbacks;
- a missing `readyRead` stream is considered stalled after 5 ms, but reception
  does not wait for that threshold: the pump reads pending data on every tick;
- the UDP worker thread is started with `QThread::HighPriority`;
- the system UDP receive buffer request remains 16 MiB and its actual value is
  read back and logged;
- socket receive batches are limited to approximately 0.75 ms;
- packet verification batches are limited to approximately 1 ms;
- if the packet ring becomes full, the oldest packet is verified immediately to
  preserve FIFO order and make room without intentionally dropping the new
  packet;
- STOP, DISCONNECT, socket failure, and application shutdown process packets
  already present in the internal ring before final Statistics are captured;
- the 20-second log and STOP diagnostics now include packet-ring depth,
  receive-pump recoveries, processing load, receive and processing batch times,
  queue pressure, and processed-datagram totals.

The packet-ring slot size is at least 2,048 bytes and grows to the configured
`block, bytes` value when larger. For example, a 1,280-byte Pattern normally
creates 16,384 slots and approximately 32 MiB of queue storage. A packet larger
than the configured slot is handled through an ordered direct-processing
fallback and counted in `oversize_direct` diagnostics.

A typical connection entry is:

```text
UDP receiver ready: local=192.168.1.2:8890; expected_source=192.168.1.3; requested_rcvbuf=16777216; actual_rcvbuf=16777216; receive_buffer=65507; receive_pump=1 ms; stall_threshold=5 ms
```

A typical START entry includes the allocated ring:

```text
UDP START: reception and verification started; Pattern: counter=32 bits; init=0; block=1280 bytes; Togeth=16; period=1 ms; values/packet=320; bo=LE; discarded_before_start=0; actual_rcvbuf=16777216; receive_buffer=65507; packet_queue_slots=16384; packet_queue_slot_bytes=2048; packet_queue_memory=33554432
```

If Qt notification delivery stalls while data is still arriving, the service
log records a rate-limited receive-pump recovery and reception continues:

```text
UDP RX receive-pump recovery: no_ready_read_for=7 ms; native_pending_bytes=10240; qt_pending=false; received_datagrams=8; queued_datagrams=8; directly_processed=0; suppressed_recovery_events=0; ...
```

`rx, bytes` and `packets/s` count payloads accepted from the configured source
IP. Counter verification is performed from the internal FIFO ring, so under an
extreme temporary backlog `counter, ok`, `counter, err`, and `curr_count` may
lag the receive totals briefly. `queue_depth` and `max_queue_depth` show that
lag explicitly.

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

UDP RX high-priority thread
|- UdpRxWorker
|- persistent bound QUdpSocket
|- persistent maximum-size socket buffer
|- immediate readyRead socket draining
|- independent 1 ms native/Qt receive pump
|- preallocated fixed-slot packet ring
|- bounded packet-processing timer
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
7. Start the 1 ms receive pump that is independent of `readyRead`.
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
discarded, and the fixed-slot packet ring is allocated from the configured
`block, bytes` value. The expected counter is initialized from `init value`.
Socket callbacks copy accepted datagrams from the configured source IP into the
ring, and a separate bounded processing callback decodes little-endian unsigned
counter values in FIFO order. Counter continuity is preserved between adjacent
UDP datagrams.

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
19:44:06.361, time=00:00:20, rx_bytes=25600000, delta_rx_bytes=25600000, curr_counter=6399999, counter_ok=6400000, delta_counter_ok=6400000, counter_err=0, delta_counter_err=0, min_packet/s=997, avrg_packets/s=999.8, max_packet/s=1002, min_speed_Kb/s=10209, avrg_speed_Kb/s=10237.9, max_speed_Kb/s=10260, ready_read_calls=18000; empty_ready_read_calls=3; read_continuations=20; receive_pump_calls=19984; receive_pump_recoveries=2; suppressed_receive_pump_events=0; read_errors=0; worker_datagrams=20000; processed_datagrams=20000; processing_callbacks=15000; queue_depth=0; max_queue_depth=64; queue_capacity=16384; queue_pressure_events=0; queue_drops=0; oversize_direct=0; max_read_batch=128; max_processing_batch=96; socket_read_time_us=420000; processing_time_us=790000; max_socket_batch_us=748; max_processing_batch_us=996; processing_load_pct=3.95; ready_read_gap_ms=0; datagram_gap_ms=0; native_pending_bytes=0; qt_pending=false
```

The minimum, average, and maximum packet rates and speeds are calculated from
the one-second values displayed during the current 20-second interval. The
average is the arithmetic mean of those samples. Delta counters are measured
from the previous 20-second line.

The appended diagnostics make it possible to distinguish normal packet loss
from a stalled notification, an internal processing backlog, packet-ring
pressure, or a socket read error. `processing_load_pct` is the fraction of the
latest Statistics interval spent inside bounded packet-verification callbacks;
`queue_depth` is the current internal backlog at the moment the line is built.
`native_pending_bytes` and `qt_pending` show whether data is still waiting in
the operating-system or Qt socket path when the Statistics line is created.

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
