# TxDataTester (v.1.9)

TxDataTester is a Qt 5.12 test-pattern generator for COM, UDP, and binary FILE
output. The project uses qmake and C++11; CMake is not required.

All source comments, README text, GUI messages, EVENTS entries, and text-log
messages are written in English.

## Version 1.9 changes

Version 1.9 implements the FILE generator that was introduced as a GUI-only tab
in v.1.8.

- `last value`, `value count`, and `file size` are linked calculations;
- the most recently edited field becomes the calculation driver;
- FILE size is always aligned to a complete 8-, 16-, or 32-bit counter field;
- changing B, KB, MB, or GB preserves the exact underlying byte count;
- fractional KB, MB, and GB display values are supported;
- a dedicated `FileGeneratorWorker` and `QThread` keep disk generation outside
  the GUI thread;
- one reusable 4 MiB buffer is filled with little-endian counter values;
- the `START` button changes to `STOP` during generation;
- manual STOP closes the file only after a complete counter field;
- file progress and min/average/max write speed update once per second;
- START, STOP, FINISH, and file-I/O errors are written to EVENTS and the text log;
- custom FILE names, the output folder, Pattern fields, selected calculation
  driver, and size unit are restored through `QSettings`;
- COM, UDP, and FILE modes are mutually locked while one mode is active.

## Build requirements

- Qt 5.12;
- Qt Widgets;
- Qt Network;
- Qt Serial Port;
- qmake;
- a C++11 compiler.

Example build:

```text
qmake TxDataTester.pro
make
```

On Windows with an MSVC Qt command prompt:

```text
qmake TxDataTester.pro
nmake
```

For a MinGW kit, use the matching `mingw32-make` command.

## Thread architecture

The application uses four event-loop threads.

### GUI thread

The main thread owns:

- `MainWindow` and all widgets;
- the shared color-formatted EVENTS view;
- the UTF-8 text log and its `flush()` calls;
- `QSettings`;
- COM-port enumeration through `QSerialPortInfo`;
- ping and neighbor-table helper processes;
- validation and linked FILE Pattern calculations.

### COM TX worker thread

`TxWorker` owns:

- `QSerialPort`;
- COM block generation;
- COM transmission timers;
- `bytesWritten()` processing;
- COM Statistics;
- soft STOP with output-queue draining.

### UDP TX worker thread

`UdpTxWorker` owns:

- the persistent sending `QUdpSocket`;
- UDP burst generation;
- the UDP transmission timer;
- little-endian counter-pattern generation;
- UDP Statistics and 20-second log samples.

### FILE generator worker thread

`FileGeneratorWorker` owns:

- `QFile`;
- one reusable 4 MiB output buffer;
- little-endian 8-, 16-, or 32-bit counter generation;
- cooperative aligned write callbacks;
- the one-second Progress timer;
- min/average/max file-write speed calculations.

GUI repainting, window movement, and text-log flushing do not execute inside the
COM, UDP, or FILE worker thread.

## COM tab

- available COM ports are refreshed once per second while the port is closed;
- OPEN applies Port, Baud, Parity, and stop-bit settings;
- START transmits counter-filled blocks continuously;
- SINGLE transmits one block;
- STOP stops new block generation and lets pending serial output drain;
- Statistics show start time, elapsed time, transmitted bytes, current counter,
  and payload speed.

Opening a COM port locks the UDP and FILE tabs until CLOSE.

## UDP tab

### Connection

CONNECT validates the destination IPv4 address and UDP port, determines the local
outgoing interface, executes one short ping, resolves the destination MAC when it
is available in the local neighbor table, and creates the persistent UDP sending
socket.

A successful ping checks IP reachability; it does not prove that an application is
listening on the destination UDP port.

### Pattern

- `counter, bits`: 8, 16, 32, or 64;
- `block, bytes`: UDP payload size;
- `init value`: decimal or `0x`-prefixed hexadecimal;
- `Togeth`: datagrams sent sequentially in one burst;
- `Period, ms`: delay between burst callbacks;
- START, STOP, and SINGLE are supported.

The counter continues across datagrams and bursts and wraps naturally at the
selected width. Payload byte order is little-endian.

### Statistics

- `tx, bytes`: payload bytes accepted by the local UDP socket;
- `curr_count`: next counter value;
- `packets/s`: datagrams accepted per real measured second;
- `speed, Kb/s`: payload speed, excluding network headers.

Successful `writeDatagram()` means that the local networking stack accepted the
datagram. It is not a delivery acknowledgement from the remote device.

CONNECT locks the COM and FILE tabs until DISCONNECT.

## FILE tab

### Output group

- `Folder` can be typed, pasted, or selected with the standard
  `QFileDialog::getExistingDirectory()` dialog opened by `...`;
- the last folder is restored on the next launch;
- the executable directory is used on the first launch;
- `File` accepts a file name with extension;
- when the File field has not been customized, an automatic name is generated,
  for example:

```text
counter_32b_init=0x1000_last=0xFFFFF_size=100MB.bin
```

Clearing a custom file name restores automatic naming. Existing files require an
explicit overwrite confirmation.

### Pattern group

- `counter, bits`: 8, 16, or 32;
- `init value`: decimal or `0x`-prefixed hexadecimal;
- `last value`: decimal or `0x`-prefixed hexadecimal;
- `value count`: decimal or `0x`-prefixed hexadecimal;
- `file size`: decimal number;
- size unit: B, KB, MB, or GB.

KB, MB, and GB use binary multipliers:

```text
1 KB = 1024 B
1 MB = 1024 × 1024 B
1 GB = 1024 × 1024 × 1024 B
```

The last edited field among `last value`, `value count`, and `file size` becomes
the driver.

#### Last-value driver

For a 32-bit counter:

```text
init value = 0
last value = 1000
value count = 1001
file size = 1001 × 4 = 4004 B
```

If last value is below init value, the shortest forward range through one natural
counter wrap is used.

#### Value-count driver

```text
counter, bits = 32
init value = 0
value count = 1000
last value = 999
file size = 1000 × 4 = 4000 B
```

If value count spans more than one counter cycle, last value is calculated with
natural unsigned wraparound.

#### File-size driver

```text
counter, bits = 32
init value = 0
entered file size = 10001 B
aligned file size = 10000 B
value count = 2500
last value = 2499
```

The requested byte size is rounded downward to a multiple of the counter-field
size. A size smaller than one counter field is normalized to one complete field.
Changing the unit only changes the displayed representation; exact bytes are
preserved.

### File generation

START opens the selected output file with truncation and fills it with consecutive
little-endian counter values. Generation uses one reusable 4 MiB buffer and
cooperative queued write callbacks.

During generation:

- the button text changes from START to STOP;
- output and Pattern fields are locked;
- COM and UDP tabs are locked;
- `file:` shows `written / target B (percentage)`;
- `speed:` shows minimum, average, and maximum MB/s;
- Progress updates approximately once per second using actual elapsed milliseconds.

Manual STOP is processed between aligned chunks. The file is flushed and closed,
and its final length always contains a whole number of counter fields. Natural
completion displays 100% and records FINISH in EVENTS.

The average speed is total written bytes divided by total elapsed time. Minimum
and maximum are based on real one-second intervals, including a final partial
interval when it contains data. These values measure bytes accepted through
`QFile` and the operating-system cache; they are not a hardware-level guarantee
that every byte has already reached the physical storage medium.

Example service events:

```text
12:30:00.100 - START file generation: counter, bits=32; init value=0; last value=999; value count=1000; file size=4000 B; file=C:\\data\\counter.bin; bo=LE
12:30:00.180 - FINISH file generation: file=C:\\data\\counter.bin; written=4000 B; min speed=47.684 MB/s; avrg speed=47.684 MB/s; max speed=47.684 MB/s
```

Direct START and STOP button presses are green in EVENTS, service information is
black, and errors are red.

## Text logs

At each application launch, TxDataTester creates:

```text
logs/txdatatester_log__YYYY-MM-DD__HH-MM-SS-mmm.txt
```

Every EVENTS line is duplicated in UTF-8 text form. COM and UDP also write their
20-second Statistics snapshots only to the text log.

## Settings

The application restores:

- window geometry;
- COM port and COM Pattern;
- UDP destination and UDP Pattern;
- FILE folder, custom name state, counter width, init value, last value,
  value count, exact size representation, selected size unit, and calculation
  driver.

## Deployment to another Windows computer

Build Release and run `windeployqt` from the same Qt 5.12 kit:

```text
windeployqt --release --compiler-runtime TxDataTester.exe
```

Copy the entire deployment directory, including:

```text
platforms/qwindows.dll
```

Installing the full Qt SDK on the target computer is not required.
