# RxDataTester (v.1.1)

Test-counter receiver for a serial COM port.

The project targets **Qt 5.12**, is built with **qmake**, and uses the
`serialport` module. CMake is not used.

## Version 1.1 changes

- All application-generated EVENTS messages are in English.
- All application-generated text-log messages are in English.
- Serial-port and log-file errors use fixed English descriptions instead of localized system text.
- All remaining UI helper text and tooltips are in English.
- The README and Doxygen comments are in English.
- Reception logic and the two-thread architecture are unchanged.

## Project files

```text
RxDataTester.pro
main.cpp
mainwindow.h
mainwindow.cpp
mainwindow.ui
rxworker.h
rxworker.cpp
```

## Build

Open `RxDataTester.pro` in Qt Creator with a Qt 5.12 kit, or run:

```text
qmake RxDataTester.pro
make
```

For an MSVC kit, run the normal build command provided by the selected Qt kit
after `qmake`, such as `nmake` or `jom`.

## Thread architecture

The application is split into two threads.

```text
Main GUI thread
├─ MainWindow and all widgets
├─ OPEN / CLOSE / START / STOP buttons
├─ QSerialPortInfo enumeration once per second
├─ colored EVENTS view
├─ text log and flush()
└─ QSettings

RX worker thread
├─ RxWorker
├─ QSerialPort
├─ readyRead() handling
├─ input QByteArray with an incomplete trailing value
├─ little-endian counter decoding
├─ comparison with the expected value
├─ Statistics QTimer
├─ port-state monitoring QTimer
└─ speed and 20-second Statistics calculation
```

The GUI never reads `QSerialPort` directly. All commands are sent to `RxWorker`
through `Qt::QueuedConnection`; completed events and Statistics snapshots are
sent back to the GUI. Repainting, moving, resizing, or minimizing the window
does not stop serial-port reception.

## Settings

Supported settings:

```text
Baud:   115200, 230400, 460800, 921600, 1500000
Data:   8 bits
Parity: NONE, EVEN, ODD
Stops:  1 or 2
Flow:   NONE
```

While the port is closed, the COM-port list is refreshed once per second. Added
and removed devices are recorded in EVENTS and the text log. While the port is
open, the application monitors the selected device specifically.

`OPEN` opens the port in `QIODevice::ReadOnly` mode. Port, Baud, Parity, and
stops are locked until `CLOSE`.

## Pattern

```text
counter, bits : 8, 16, 32, or 64
block, bytes  : informational transmitter block size
init value    : decimal or hexadecimal with the 0x prefix
Period, ms    : informational transmitter block period
block len, us : calculated UART transmission time for one block
bo            : LE, little-endian
```

`block, bytes` is rounded upward to a multiple of the counter size. The
`block, bytes` and `Period, ms` fields are retained to match TxDataTester
settings, calculate `block len, us`, and describe the Pattern in the log.
**Receiver parsing does not use block boundaries.**

## Counter verification algorithm

After `START`, the application discards data accumulated before the test and
waits for new bytes. The stream is parsed continuously using only the selected
counter size:

```text
8 bits  -> 1 byte
16 bits -> 2 bytes
32 bits -> 4 bytes
64 bits -> 8 bytes
```

A `readyRead()` call may deliver any number of bytes. An incomplete trailing
value of 1...7 bytes remains in the internal buffer until more data arrives.
Every complete counter is decoded explicitly as little-endian, so the algorithm
does not depend on host byte order or alignment.

Example for `counter=32 bits`, `init value=10`:

```text
received 10 -> counter, ok + 1; next expected value is 11
received 11 -> counter, ok + 1; next expected value is 12
...
```

On a mismatch, `counter, err` is incremented once regardless of the numeric gap.
The next expected value becomes the received value plus one:

```text
expected 120
received 124
counter, err + 1
next expected value 125
```

A red line is written to EVENTS and the text log:

```text
19:44:06.361 - counter error: expected 120, received 124, next expected value is 125
```

After the maximum value, the unsigned counter wraps to zero. The algorithm
assumes byte alignment is preserved. If a single byte inside a counter value is
lost rather than a whole counter value, version 1.1 does not automatically scan
for a new alignment boundary.

## Statistics

The following values are updated once per second:

```text
start:        local time when START was pressed
 time:        elapsed test time without a 24-hour wrap
rx, bytes:    all bytes received after START
curr_count:   last completely received counter value
speed, Kb/s:  measured speed over the actual sample interval
counter, ok:  number of matching counter values
counter, err: number of mismatching counter values
```

Speed is calculated from received bytes and monotonic elapsed time:

```text
speed_Kb_s = delta_rx_bytes * 8 / delta_time_ms
```

Bits per millisecond are numerically equal to decimal kilobits per second.

## EVENTS and colors

Normal event format:

```text
HH:MM:SS.mmm - event
```

Colors:

```text
green - direct OPEN, CLOSE, START, or STOP button press
red   - port, read, Pattern, and counter-mismatch errors
black - application lifecycle, service information, and port open/close events
```

The EVENTS view uses a monospaced font, automatic scrolling, and a limit of
10,000 visible lines. The text log has no line-count limit.

## Log files

On every launch, the application creates a directory next to the executable:

```text
logs
```

A new file is created with a name such as:

```text
rxdatatester_log__2026-07-22__19-44-06-361.txt
```

Every EVENTS line is duplicated in the UTF-8 file. In addition, a Statistics
line is written only to the file once every 20 seconds, for example:

```text
19:44:26.361, time=00:00:20, rx_bytes=1782656, delta_rx_bytes=1782656, curr_counter=445689, counter_ok=445690, delta_counter_ok=445690, counter_err=0, delta_counter_err=0, min_speed=522, avrg_speed=544.18, max_speed=621
```

`avrg_speed` uses the exact measured interval. `min_speed` and `max_speed` are
taken from the one-second speed samples shown in the GUI during that interval.

## Saved settings

On a normal shutdown, `QSettings` stores:

```text
window geometry
selected COM port
Baud, Parity, stops
counter, bits
block, bytes
init value, including the 0x format
Period, ms
```

The values are restored and validated again on the next launch.

## Source style

Every function has an English Doxygen block with `@brief`, `@param`, `@return`,
and `@detail`. Functions with no parameters use `@param none`; functions with
no return value use `@return none`. Functions are separated by an 81-character
`/*-----------------------------------------------------------------------------*/` line.
