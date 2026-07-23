# TxDataTester (v.1.5)

Test-pattern transmitter for a serial COM port.

The project targets **Qt 5.12**, uses the **Qt Serial Port** module, and is built
with **qmake**. CMake is not used. The UDP tab is currently empty.

## Version 1.5 changes

- All application-generated EVENTS messages are in English.
- All application-generated text-log messages are in English.
- Serial-port and log-file errors use fixed English descriptions instead of localized system text.
- All remaining UI helper text and tooltips are in English.
- The README and Doxygen comments are in English.
- Transmission logic and the two-thread architecture are unchanged.

## Build

Open `TxDataTester.pro` in Qt Creator, or run the following commands in a Qt
5.12 kit environment:

```text
qmake TxDataTester.pro
mingw32-make
```

For an MSVC kit, use the corresponding `nmake` or `jom` command instead of
`mingw32-make`.

## Thread architecture

Data transmission is isolated from the interface in a dedicated worker thread.

```text
Main GUI thread
├─ MainWindow and all widgets
├─ OPEN / CLOSE / START / STOP / SINGLE buttons
├─ QSerialPortInfo enumeration once per second
├─ colored EVENTS view
├─ text-log write and flush()
└─ QSettings

TX worker QThread
├─ TxWorker
├─ QSerialPort
├─ block-generation QTimer
├─ Statistics QTimer
├─ unexpected-port-close monitoring QTimer
├─ QByteArray counter-pattern generation
├─ QSerialPort::write() calls
├─ bytesWritten and errorOccurred handling
├─ soft STOP with output-queue draining
└─ speed and 20-second Statistics calculation
```

`QSerialPort` and all worker timers are created by `TxWorker::initialize()`
after `TxWorker` has been moved to its `QThread`. They therefore belong to the
worker thread from creation. The GUI never reads `isOpen()`, `bytesToWrite()`,
or other `QSerialPort` properties directly.

Commands are sent only through queued signals:

```text
GUI -> openPortRequested(...)
GUI -> closePortRequested()
GUI -> startContinuousRequested(...)
GUI -> stopContinuousRequested()
GUI -> singleRequested(...)
GUI -> externalPortLossDetected(...)
```

Results are returned through queued signals:

```text
TX -> portStateChanged(...)
TX -> transmissionStateChanged(...)
TX -> statisticsUpdated(...)
TX -> eventGenerated(...)
TX -> periodicLogLineReady(...)
TX -> stopButtonAccepted(...)
```

Practical effects:

- repainting, moving, and resizing the window no longer delay generation of the
  next block;
- minimizing the window does not affect the transmission timer;
- a slow text-log `flush()` does not stop `QSerialPort`;
- COM-port enumeration in the GUI does not interrupt data generation;
- with `Period = 0`, the worker thread maintains the output queue itself;
- one-second and 20-second Statistics calculations are independent of GUI
  repainting.

Windows and `QTimer` are still not hard real-time systems. System load may add
small variations to 0-1 ms intervals. The worker thread removes GUI-related
jitter but does not replace a hardware timer.

Worker events receive an `HH:MM:SS.mmm` timestamp at the actual operation time.
Even if the GUI is temporarily busy, EVENTS and the log contain the event time,
not the later display time.

During application shutdown, the GUI invokes `TxWorker::shutdown()` through
`Qt::BlockingQueuedConnection`, waits for the port to close and the worker to
stop, receives final events, and only then closes the log file.

## Main features

- Window title: `TxDataTester (v.1.5)`.
- `Tranceiver` header: 12 pt, regular font.
- COM and UDP tabs; the UDP tab is currently empty.
- Real COM-port opening and closing through `QSerialPort`.
- Port settings: 8 data bits, selected baud/parity/stops, and
  `NoFlowControl`.
- Port, Baud, Parity, and stops cannot be changed while the port is open.
- The application enumerates ports once per second. While the port is closed,
  added and removed devices are recorded in EVENTS. While it is open, the
  selected device is monitored specifically.
- The worker additionally checks once per second that a logically open
  `QSerialPort` has not closed unexpectedly without a device-list event.
- Errors are red; normal and service events are black. Green is used only for
  direct START, STOP, and SINGLE button presses.
- EVENTS uses a monospaced font, colored lines, automatic scrolling, and a
  history limit of 10,000 lines.
- Every event line uses an `HH:MM:SS.mmm` timestamp.
- On every launch, a `logs` directory and a new UTF-8 file are created next to
  the executable, for example:

```text
txdatatester_log__2026-07-22__19-43-46-127.txt
```

- `QSettings` stores window geometry, COM settings, and Pattern fields.
- `block, bytes` and `Period, ms` accept only digits `0...9`.
- `block, bytes` is rounded upward to a multiple of the counter size.
- `init value` accepts decimal or hexadecimal with the `0x` prefix.
- Statistics elapsed time does not wrap after 24 hours.

## Block generation

The counter is unsigned and may be 8, 16, 32, or 64 bits wide. Values are
written sequentially into each block in **little-endian** order. After the
maximum value, the counter wraps to zero. The counter is not reset between
adjacent START blocks.

For `block = 64 bytes` and `counter = 16 bits`, one block contains:

```text
64 / 2 = 32 values
```

With `init = 0`, the first block contains `0...31`, the second contains
`32...63`, and the third contains `64...95`. The value `1` is transmitted as
bytes `01 00`.

## Period, ms

- `Period > 0`: the worker timer attempts to generate the next block after the
  selected number of milliseconds.
- `Period = 0`: no intentional software delay is inserted. A single-shot
  zero-duration timer fills the queue and then waits for `bytesWritten` when
  the queue window is full.
- No new block is created while the queue window is full.
- The window size is the larger of 4096 bytes and four active blocks.

The period controls when blocks are queued. It does not guarantee a hardware
idle interval after the previous block's final stop bit.

## block len, us

The label is recalculated automatically when `block, bytes`, Baud, Parity, or
stops changes. One byte includes:

```text
1 start bit + 8 data bits + 1 parity bit for EVEN/ODD + 1 or 2 stop bits
```

Calculation:

```text
block_len_us = ceil(block_bytes * bits_per_byte * 1 000 000 / baud)
```

For example, 128 bytes at 921600 baud with `NONE` parity and 2 stop bits gives:

```text
block len, us => 1528
```

This is the theoretical on-wire time and does not include driver, USB, or OS
latency.

## STOP behavior

1. The STOP command reaches the worker thread through a queued signal.
2. The worker block-generation timer stops immediately.
3. `TxWorker` reads `bytesToWrite()` and reports the remaining queue size to
   the GUI.
4. The GUI writes the green button-press line:

```text
02:08:01.123 - STOP button pressed; generation of new blocks stopped; 4096 bytes remain in the queue
```

5. `clear(Output)` is not called.
6. Already queued bytes continue to be transmitted.
7. `bytesWritten`, Statistics, and the 20-second log continue to be updated.
8. After `bytesToWrite() == 0`, the worker writes a normal black line:

```text
02:08:01.171 - STOP completed; the remaining output queue was transmitted; 12345678 bytes transmitted in total
```

An explicit CLOSE, application shutdown, or port loss may forcibly discard a
remaining queue because the device must be closed in those cases.

## EVENTS

Example:

```text
02:30:43.004 - START button pressed
02:30:43.005 - START: continuous transmission started; Pattern: counter=8 bits; init=0; block=128 bytes; period=2 ms; values/block=128; bo=LE
02:36:22.100 - SINGLE button pressed
02:36:22.101 - SINGLE: one block queued for transmission; Pattern: counter=8 bits; init=0; block=128 bytes; period=3 ms; values/block=128; bo=LE
02:36:22.345 - SINGLE: block transmission completed, 128 bytes transmitted
```

Normal EVENTS lines are duplicated exactly in the text log.

## Statistics

Statistics are calculated in the worker thread:

- `start:`: local time when START or SINGLE begins;
- `time:`: elapsed duration from a monotonic `QElapsedTimer`;
- `tx, bytes`: sum of confirmations from `QSerialPort::bytesWritten`;
- `curr_count`: next counter value after the most recently generated block;
- `speed, Kb/s`: speed over the actual one-second sample interval:

```text
speed = delta_bytes * 8 / delta_time_ms
```

Completed values are sent to the GUI through `statisticsUpdated(...)`.

## Additional text-log line

During START, `TxWorker` generates a line once every 20 seconds:

```text
19:44:06.361, time=00:00:20, tx_bytes=1360450, delta_tx_bytes=1360450, curr_counter=445689, min_speed=522, avrg_speed=544.18, max_speed=621
```

The line is not displayed in EVENTS. The GUI thread only writes the completed
line to the file.

- `delta_tx_bytes`: difference in total bytes between adjacent lines;
- `min_speed` and `max_speed`: minimum and maximum one-second samples in the
  current window;
- `avrg_speed`: `delta_tx_bytes * 8 / actual_interval_ms`.

## Source style

Every function has an English Doxygen block with `@brief`, `@param`, `@return`,
and `@detail`. Functions with no parameters use `@param none`; functions with
no return value use `@return none`. Functions are separated by an 81-character
`/*-----------------------------------------------------------------------------*/` line.
