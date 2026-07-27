#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QColor>
#include <QFile>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QProcess>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSet>
#include <QString>
#include <QTextStream>
#include <QThread>
#include <QTimer>

class QCloseEvent;
class QComboBox;
class QProcess;
class RxWorker;
class UdpRxWorker;

namespace Ui
{
class MainWindow;
}

/**
 * @brief Main application window for RxDataTester.
 * @detail Runs in the main GUI thread and manages widgets, COM-port discovery, UDP
 *         connection probing, EVENTS, the text log, and QSettings. COM reception runs
 *         in RxWorker, while UDP reception runs in UdpRxWorker in a second dedicated
 *         worker thread.
 */
class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * @brief Creates the main application window.
     * @param parent Parent widget of the window.
     * @return none
     * @detail Configures the GUI, validators, event log, settings persistence, and the
     *         dedicated COM and UDP receiver worker threads.
     */
    explicit MainWindow(QWidget *parent = nullptr);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Destroys the main application window.
     * @param none
     * @return none
     * @detail Ensures orderly shutdown of both receiver threads, closes the log file,
     *         and releases the Qt Designer user interface.
     */
    ~MainWindow() override;

signals:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a COM-port open request to the worker thread.
     * @param portName Name of the selected port.
     * @param baudRate Selected baud rate.
     * @param parityValue Numeric value of QSerialPort::Parity.
     * @param stopBitsValue Numeric value of QSerialPort::StopBits.
     * @param settingsDescription Port settings description for the event log.
     * @return none
     * @detail The queued connection guarantees that QSerialPort is opened only in the
     *         RX worker thread.
     */
    void openPortRequested(const QString &portName,
                           qint32 baudRate,
                           int parityValue,
                           int stopBitsValue,
                           const QString &settingsDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a COM-port close request to the worker thread.
     * @param none
     * @return none
     * @detail The GUI never calls QSerialPort::close() directly.
     */
    void closePortRequested();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a START command to the worker thread.
     * @param counterBits Bit width of the counter to verify.
     * @param blockBytes Informational transmitter block size in bytes.
     * @param periodMs Informational transmitter block period in milliseconds.
     * @param initialValue First expected counter value.
     * @param patternDescription Preformatted Pattern description for the event log.
     * @return none
     * @detail RxWorker validates the parameters again before reception starts.
     */
    void startReceptionRequested(int counterBits,
                                 int blockBytes,
                                 int periodMs,
                                 quint64 initialValue,
                                 const QString &patternDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a STOP command to the worker thread.
     * @param none
     * @return none
     * @detail Stops counter verification while leaving the COM port open.
     */
    void stopReceptionRequested();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Notifies the worker thread that the open port disappeared.
     * @param reason Reason text for the red event-log entry.
     * @return none
     * @detail The GUI timer checks QSerialPortInfo, while only the owning RxWorker is
     *         allowed to close the port.
     */
    void externalPortLossDetected(const QString &reason);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests creation of the persistent UDP receiver socket.
     * @param expectedSourceIp Validated IPv4 address of the expected transmitter.
     * @param listenPort Local UDP destination port reserved for reception.
     * @param localIp Local IPv4 address selected by the operating-system route.
     * @return none
     * @detail Carries validated GUI connection data to UdpRxWorker after ping succeeds.
     */
    void configureUdpConnectionRequested(const QString &expectedSourceIp,
                                         quint16 listenPort,
                                         const QString &localIp);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests closure of the persistent UDP receiver socket.
     * @param none
     * @return none
     * @detail Releases the local listening port in the UDP worker thread.
     */
    void disconnectUdpConnectionRequested();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests UDP reception and sequential-counter verification.
     * @param counterBits Counter width in bits.
     * @param blockBytes Expected UDP payload size in bytes.
     * @param togetherCount Informational packets-per-burst value.
     * @param periodMs Informational burst period in milliseconds.
     * @param initialValue First expected counter value.
     * @param patternDescription Preformatted UDP Pattern description for the log.
     * @return none
     * @detail UdpRxWorker validates the scalar values again before reception starts.
     */
    void startUdpReceptionRequested(int counterBits,
                                    int blockBytes,
                                    int togetherCount,
                                    int periodMs,
                                    quint64 initialValue,
                                    const QString &patternDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests stop of UDP reception and counter verification.
     * @param none
     * @return none
     * @detail Leaves the bound receiver socket ready for another START operation.
     */
    void stopUdpReceptionRequested();

protected:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles closing of the main window.
     * @param event Qt close event for the window.
     * @return none
     * @detail Synchronously stops both receiver threads, saves the settings, closes the
     *         log, and then passes the event to the base QMainWindow implementation.
     */
    void closeEvent(QCloseEvent *event) override;

private:
    /**
     * @brief Event-log entry type.
     * @detail Normal entries are black, Action entries are green, and Error entries are
     *         red.
     */
    enum class EventType
    {
        Normal,
        Action,
        Error
    };

    /**
     * @brief Validated Pattern settings read from the GUI.
     * @detail blockBytes and periodMs are used by the interface and the log. Input
     *         stream verification uses counterBits, counterBytes, and initialValue.
     */
    struct PatternSettings
    {
        int counterBits = 8;
        int counterBytes = 1;
        int blockBytes = 1;
        int periodMs = 0;
        quint64 initialValue = 0;
        quint64 maximumCounterValue = 0xFFU;
    };

    /**
     * @brief Validated UDP Pattern settings read from the GUI.
     * @detail blockBytes describes the expected payload size, while Togeth and Period
     *         describe the transmitter burst. Counter verification uses counterBits,
     *         counterBytes, and initialValue.
     */
    struct UdpPatternSettings
    {
        int counterBits = 8;
        int counterBytes = 1;
        int blockBytes = 1;
        int togetherCount = 1;
        int periodMs = 0;
        quint64 initialValue = 0;
        quint64 maximumCounterValue = 0xFFU;
    };

private slots:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Checks the list of available serial ports.
     * @param none
     * @return none
     * @detail When the port is closed, updates the combo box and logs added or removed
     *         devices. When the port is open, monitors the selected device.
     */
    void refreshSerialPorts();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests opening of the selected serial port.
     * @param none
     * @return none
     * @detail Logs the OPEN button press in green, validates the selected settings, and
     *         sends a queued command to RxWorker.
     */
    void openSerialPort();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests closing of the open serial port.
     * @param none
     * @return none
     * @detail Logs the CLOSE button press in green and sends the close command to the
     *         object that owns the port.
     */
    void closeSerialPort();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Normalizes the informational block size.
     * @param none
     * @return none
     * @detail Rounds block, bytes upward to a multiple of the counter size and limits
     *         the value to the positive int range.
     */
    void normalizeBlockSize();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Validates the initial counter value.
     * @param none
     * @return none
     * @detail Supports decimal input and hexadecimal input with the 0x prefix while
     *         preserving the user-selected format after normalization.
     */
    void normalizeInitialValue();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Normalizes the informational block period.
     * @param none
     * @return none
     * @detail Replaces an empty or invalid field with zero and limits an oversized
     *         value to the maximum int value.
     */
    void normalizePeriod();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Updates the calculated transmission time of one block.
     * @param none
     * @return none
     * @detail Calculates the theoretical duration from baud, parity, stop bits, and
     *         block bytes, then displays it as block len, us => N.
     */
    void updateBlockTransmissionTime();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles a change of counter bit width.
     * @param none
     * @return none
     * @detail Realigns block, bytes and validates the init value range again.
     */
    void handleCounterBitsChanged();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests start of reception and counter verification.
     * @param none
     * @return none
     * @detail Logs the START button press in green, validates Pattern, and sends the
     *         validated scalar values to the RX worker thread.
     */
    void startTest();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests stop of reception and counter verification.
     * @param none
     * @return none
     * @detail Logs the STOP button press in green and waits for the final black service
     *         entry from RxWorker.
     */
    void stopTest();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Normalizes the UDP informational block size.
     * @param none
     * @return none
     * @detail Rounds block, bytes upward to a multiple of the selected UDP counter
     *         size and clamps it to the IPv4 UDP payload limit.
     */
    void normalizeUdpBlockSize();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Validates the UDP initial counter value.
     * @param none
     * @return none
     * @detail Accepts decimal and 0x-prefixed hexadecimal input and mirrors the
     *         normalized value in the UDP curr_count field.
     */
    void normalizeUdpInitialValue();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Normalizes the UDP informational packet period.
     * @param none
     * @return none
     * @detail Converts an empty field to zero and clamps values above the maximum
     *         QTimer interval representable by int.
     */
    void normalizeUdpPeriod();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Normalizes the UDP Togeth packet count.
     * @param none
     * @return none
     * @detail Converts empty or zero input to one and clamps values to int range.
     */
    void normalizeUdpTogether();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles a change of the UDP counter width.
     * @param none
     * @return none
     * @detail Realigns UDP block, bytes and validates the UDP init value range again.
     */
    void handleUdpCounterBitsChanged();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Connects or disconnects the logical UDP destination.
     * @param none
     * @return none
     * @detail CONNECT validates the destination, resolves the outgoing interface, and
     *         runs one short-timeout ping request. DISCONNECT unlocks the UDP page.
     */
    void handleUdpConnectButton();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests start of UDP reception and counter verification.
     * @param none
     * @return none
     * @detail Logs the button press, validates UDP Pattern, and sends a queued START
     *         command to UdpRxWorker.
     */
    void startUdpTest();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests stop of UDP reception and counter verification.
     * @param none
     * @return none
     * @detail Logs the button press and waits for the final worker state and Statistics
     *         snapshot.
     */
    void stopUdpTest();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles completion of the ping utility.
     * @param exitCode Process exit code.
     * @param exitStatus QProcess::ExitStatus value.
     * @return none
     * @detail Counts replies, logs a fixed English result, and requests creation of the
     *         persistent UDP receiver socket when the target is reachable.
     */
    void handlePingFinished(int exitCode, QProcess::ExitStatus exitStatus);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles an asynchronous ping-process error.
     * @param processError QProcess::ProcessError value.
     * @return none
     * @detail Reports a fixed English diagnostic and unlocks the destination fields.
     */
    void handlePingProcessError(QProcess::ProcessError processError);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles expiration of the application-level ping watchdog.
     * @param none
     * @return none
     * @detail Terminates a ping process that exceeded the short connection timeout.
     */
    void handlePingTimeout();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles completion of the neighbor-table lookup.
     * @param exitCode Process exit code.
     * @param exitStatus QProcess::ExitStatus value.
     * @return none
     * @detail Extracts the target MAC address from numeric process output.
     */
    void handleNeighborLookupFinished(int exitCode, QProcess::ExitStatus exitStatus);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles an asynchronous neighbor-table process error.
     * @param processError QProcess::ProcessError value.
     * @return none
     * @detail Keeps the logical connection and records a fixed English diagnostic.
     */
    void handleNeighborLookupProcessError(QProcess::ProcessError processError);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles readiness of the dedicated RX worker thread.
     * @param none
     * @return none
     * @detail Enables controls after QSerialPort and the worker timers are created.
     */
    void handleWorkerReady();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Receives a normal or error event from a receiver worker.
     * @param timestamp Timestamp created in the worker thread.
     * @param text Event text without a timestamp.
     * @param error true for a red error entry; otherwise false.
     * @return none
     * @detail Displays COM or UDP worker events in EVENTS and duplicates them to the
     *         text log in the GUI thread.
     */
    void handleWorkerEvent(const QString &timestamp,
                           const QString &text,
                           bool error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Receives a new COM-port state from RxWorker.
     * @param open true when the port was opened successfully.
     * @param portName Name of the opened or just-closed port.
     * @param settingsDescription Description of the applied port settings.
     * @param causedByFailure true when the port was closed because of a failure.
     * @return none
     * @detail Updates only the GUI-side state snapshot and never accesses QSerialPort
     *         from the main thread.
     */
    void handlePortStateChanged(bool open,
                                const QString &portName,
                                const QString &settingsDescription,
                                bool causedByFailure);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Receives the counter-verification state from RxWorker.
     * @param running true between a successful START and completion of STOP.
     * @return none
     * @detail Clears the pending queued-command state and synchronizes START and STOP.
     */
    void handleReceptionStateChanged(bool running);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Receives a prepared Statistics snapshot from RxWorker.
     * @param startTime Test start time in HH:MM:SS format.
     * @param elapsedMilliseconds Elapsed test time in milliseconds.
     * @param totalBytesReceived Total number of bytes received during the test.
     * @param currentCounter Last completely received counter value.
     * @param counterOk Number of values that matched the expected counter.
     * @param counterErrors Number of values that did not match the expected counter.
     * @param speedKbps Measured receive speed for the latest interval in Kb/s.
     * @return none
     * @detail Formats and displays the already calculated values without doing RX work.
     */
    void handleStatisticsUpdated(const QString &startTime,
                                 qint64 elapsedMilliseconds,
                                 quint64 totalBytesReceived,
                                 quint64 currentCounter,
                                 quint64 counterOk,
                                 quint64 counterErrors,
                                 double speedKbps);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles readiness of the dedicated UDP RX worker thread.
     * @param none
     * @return none
     * @detail Enables UDP connection controls after QUdpSocket and worker timers are
     *         created in the worker thread.
     */
    void handleUdpWorkerReady();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Receives the persistent UDP receiver-socket state.
     * @param connected true when the socket is bound and ready to receive.
     * @param expectedSourceIp Configured IPv4 address of the expected transmitter.
     * @param listenPort Local UDP port reserved for reception.
     * @param localIp Local IPv4 address to which the socket is bound.
     * @param causedByFailure true when the socket was closed because of an error.
     * @return none
     * @detail Synchronizes CONNECT, tab locking, and UDP control states without touching
     *         QUdpSocket from the GUI thread.
     */
    void handleUdpConnectionStateChanged(bool connected,
                                         const QString &expectedSourceIp,
                                         quint16 listenPort,
                                         const QString &localIp,
                                         bool causedByFailure);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Receives the active UDP verification state.
     * @param running true between successful UDP START and STOP completion.
     * @return none
     * @detail Clears the pending command flag and synchronizes UDP START and STOP.
     */
    void handleUdpReceptionStateChanged(bool running);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Receives a prepared UDP Statistics snapshot.
     * @param startTime Reception start time in HH:MM:SS format.
     * @param elapsedMilliseconds Elapsed monotonic test time in milliseconds.
     * @param totalPayloadBytes Total accepted UDP payload bytes.
     * @param currentCounter Last completely decoded counter value.
     * @param counterOk Number of values matching the expected counter.
     * @param counterErrors Number of counter mismatches.
     * @param speedKbps Payload receive speed for the latest interval in Kb/s.
     * @param packetsPerSecond Accepted UDP datagrams per second for the latest interval.
     * @return none
     * @detail Formats and displays values already calculated in UdpRxWorker.
     */
    void handleUdpStatisticsUpdated(const QString &startTime,
                                    qint64 elapsedMilliseconds,
                                    quint64 totalPayloadBytes,
                                    quint64 currentCounter,
                                    quint64 counterOk,
                                    quint64 counterErrors,
                                    double speedKbps,
                                    double packetsPerSecond);

private:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Creates and starts the dedicated RX worker thread.
     * @param none
     * @return none
     * @detail Moves RxWorker to QThread, connects all cross-thread signals with queued
     *         connections, and starts the thread after all connections are configured.
     */
    void initializeRxThread();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Creates and starts the dedicated UDP RX worker thread.
     * @param none
     * @return none
     * @detail Moves UdpRxWorker to QThread, connects queued commands and worker replies,
     *         and starts the thread after all connections are configured.
     */
    void initializeUdpRxThread();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Updates enabled states of the controls.
     * @param none
     * @return none
     * @detail Accounts for both worker threads, COM and UDP connection states, active
     *         reception, tab locking, and pending asynchronous commands.
     */
    void updateControlStates();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Adds an event with the current timestamp.
     * @param eventText Event text without a timestamp.
     * @param eventType Color and style category of the entry.
     * @return none
     * @detail Used for user actions and GUI-local errors.
     */
    void appendEvent(const QString &eventText, EventType eventType);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Adds an event with a preformatted timestamp.
     * @param timestamp Timestamp in HH:MM:SS.mmm format.
     * @param eventText Event text without a timestamp.
     * @param eventType Color and style category of the entry.
     * @return none
     * @detail Preserves the actual RX event time even when the GUI is temporarily busy.
     */
    void appendTimestampedEvent(const QString &timestamp,
                                const QString &eventText,
                                EventType eventType);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Writes a prepared line only to the text log file.
     * @param line Complete line without a trailing newline.
     * @return none
     * @detail Appends a newline and flushes the stream in the main GUI thread.
     */
    void writeLogLine(const QString &line);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the color for an event-log entry.
     * @param eventType Event type to convert to a color.
     * @return A black, green, or red QColor.
     * @detail Green is used only for direct button presses.
     */
    QColor eventColor(EventType eventType) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Creates the logs directory and a new log file for the current run.
     * @param none
     * @return none
     * @detail Opens a UTF-8 rxdatatester_log__date__time.txt file next to the
     *         executable.
     */
    void initializeLogFile();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Flushes and closes the log file.
     * @param none
     * @return none
     * @detail Detaches QTextStream from QFile after the mandatory flush.
     */
    void closeLogFile();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Restores settings from the previous run.
     * @param none
     * @return none
     * @detail Loads window geometry, the selected COM port, and Pattern fields from
     *         QSettings.
     */
    void loadSettings();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Saves the current application settings.
     * @param none
     * @return none
     * @detail Normalizes the fields and saves window geometry, COM settings, and
     *         Pattern through QSettings.
     */
    void saveSettings();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Performs one-time application shutdown.
     * @param none
     * @return none
     * @detail Invokes both worker shutdown slots through blocking queued calls, waits
     *         for both QThreads, processes final events, saves settings, and closes the
     *         log.
     */
    void prepareShutdown();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Repopulates the COM-port combo box.
     * @param ports Current list of QSerialPortInfo objects.
     * @return none
     * @detail Preserves the preferred selection and stores each device description as a
     *         tooltip.
     */
    void updatePortComboBox(const QList<QSerialPortInfo> &ports);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds a set of available port names.
     * @param ports List of serial-port information objects.
     * @return A set containing all non-empty port names.
     * @detail Used to compare port snapshots and monitor the open device.
     */
    QSet<QString> portNames(const QList<QSerialPortInfo> &ports) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds descriptions of available ports indexed by name.
     * @param ports List of serial-port information objects.
     * @return A hash table that maps a port name to its full description.
     * @detail Stores device text for a later port-disappearance event.
     */
    QHash<QString, QString> portDescriptions(
        const QList<QSerialPortInfo> &ports) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Synchronizes the internal port snapshot without addition events.
     * @param ports Current list of QSerialPortInfo objects.
     * @return none
     * @detail Used after an emergency close so that a red port-loss event is not
     *         duplicated by a normal "port disappeared" entry.
     */
    void synchronizePortSnapshot(const QList<QSerialPortInfo> &ports);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sets the logical UDP connection state.
     * @param connected true when the persistent UDP socket is bound.
     * @return none
     * @detail Mirrors the receiver-socket state reported by UdpRxWorker and updates tab
     *         states.
     */
    void setUdpConnectionState(bool connected);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Clears displayed UDP interface and destination-MAC information.
     * @param none
     * @return none
     * @detail Prevents stale route information from remaining visible after edits.
     */
    void clearUdpNetworkInformation();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reads and validates the UDP destination fields.
     * @param destinationIp Output destination IPv4 string.
     * @param destinationPort Output destination UDP port.
     * @param errorText Output fixed English validation error.
     * @return true when the IPv4 address and port 1...65535 are valid.
     * @detail Character validators block unwanted input while this method verifies the
     *         complete numeric address and port range.
     */
    bool readUdpDestination(QString *destinationIp,
                            quint16 *destinationPort,
                            QString *errorText) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Resolves the local interface selected for a UDP destination.
     * @param destinationIp Destination IPv4 address.
     * @param destinationPort Destination UDP port used for route selection.
     * @param localIp Output local IPv4 address.
     * @param localMac Output local interface MAC address or -- when unavailable.
     * @param errorText Output fixed English route-selection error.
     * @return true when an outgoing IPv4 address was selected.
     * @detail Uses a temporary connected QUdpSocket only as an operating-system route
     *         probe and closes it before returning.
     */
    bool resolveUdpRoute(const QString &destinationIp,
                         quint16 destinationPort,
                         QString *localIp,
                         QString *localMac,
                         QString *errorText) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Finds a network-interface MAC address by local IPv4 address.
     * @param localIp Local IPv4 address to match.
     * @return Normalized uppercase MAC address or -- when unavailable.
     * @detail Searches active QNetworkInterface address entries.
     */
    QString localMacForIp(const QString &localIp) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds the UDP Connection Settings event text.
     * @param destinationIp Validated destination IPv4 address.
     * @param destinationPort Validated destination UDP port.
     * @return One-line destination and local-interface description.
     * @detail The destination MAC is logged separately after the neighbor lookup.
     */
    QString udpConnectionDescription(const QString &destinationIp,
                                     quint16 destinationPort) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Starts an asynchronous one-request IPv4 ping.
     * @param destinationIp Validated destination IPv4 address.
     * @return none
     * @detail Uses a short native timeout and records only fixed English result text.
     */
    void startUdpPing(const QString &destinationIp);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Stops active UDP helper processes during application shutdown.
     * @param none
     * @return none
     * @detail Terminates ping or neighbor lookup and force-kills a process if needed.
     */
    void stopUdpProcesses();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Starts an asynchronous destination-MAC lookup.
     * @param destinationIp IPv4 address that replied to ping.
     * @return none
     * @detail Uses arp on Windows and macOS or ip neigh on Linux, then parses numeric
     *         IP and MAC patterns from the output.
     */
    void startDestinationMacLookup(const QString &destinationIp);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Extracts a destination MAC address from neighbor-table output.
     * @param output Process output converted with the local 8-bit codec.
     * @param destinationIp Destination IPv4 address used to select the correct line.
     * @return Normalized uppercase MAC address or an empty string when not found.
     * @detail Accepts both colon-separated and hyphen-separated MAC formats.
     */
    QString parseDestinationMac(const QString &output,
                                const QString &destinationIp) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Normalizes a textual MAC address.
     * @param macAddress Colon-separated or hyphen-separated MAC address.
     * @return Uppercase colon-separated MAC address or an empty string when invalid.
     * @detail Validation requires exactly six hexadecimal octets.
     */
    QString normalizeMacAddress(const QString &macAddress) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Counts successful ping replies.
     * @param output Merged ping standard output and standard error.
     * @return Number of case-insensitive TTL fields found in the output.
     * @detail TTL is stable across localized ping output and avoids localized EVENTS.
     */
    int pingReplyCount(const QString &output) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reads and validates UDP Pattern values from the GUI.
     * @param settings Output pointer for validated UDP Pattern settings.
     * @param errorText Output pointer for a validation error message.
     * @return true when all UDP Pattern values are valid; otherwise false.
     * @detail Checks counter width, payload size, alignment, Togeth, Period, and initial
     *         value range.
     */
    bool readUdpPatternSettings(UdpPatternSettings *settings,
                                QString *errorText) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds a UDP Pattern description for the event log.
     * @param settings Validated UDP Pattern settings.
     * @return A counter/init/block/Togeth/period/values-per-packet/bo=LE string.
     * @detail Hexadecimal input also includes its decimal equivalent.
     */
    QString udpPatternDescription(const UdpPatternSettings &settings) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Parses the UDP initial counter value.
     * @param value Output pointer for the parsed unsigned value.
     * @return true when decimal or 0x-prefixed input fits the selected UDP counter.
     * @detail The method does not modify the interface and is used by normalization.
     */
    bool parseUdpInitialValue(quint64 *value) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the selected UDP counter size in bytes.
     * @param none
     * @return A size of 1, 2, 4, or 8 bytes.
     * @detail An unexpected GUI value is safely converted to one byte.
     */
    quint64 udpCounterBytes() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the maximum value of the selected UDP counter.
     * @param none
     * @return Maximum unsigned value for the current UDP counter width.
     * @detail Uses numeric_limits for 64 bits to avoid an invalid shift.
     */
    quint64 udpMaximumCounterValue() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reads and validates Pattern settings.
     * @param settings Output pointer for the validated settings.
     * @param errorText Output pointer for a validation error message.
     * @return true when all values are valid; otherwise false.
     * @detail Validates bit width, ranges, block alignment, period, and init value.
     */
    bool readPatternSettings(PatternSettings *settings,
                             QString *errorText) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Converts the init value field to an integer.
     * @param value Output pointer for the converted value.
     * @return true when conversion succeeds and the value fits the selected width.
     * @detail Uses base 16 only for the 0x prefix; otherwise uses base 10.
     */
    bool parseInitialValue(quint64 *value) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Formats elapsed test time.
     * @param elapsedMilliseconds Elapsed duration in milliseconds.
     * @return A HH:MM:SS string without a 24-hour limit.
     * @detail Hours are calculated from the complete duration and never wrap at
     *         midnight.
     */
    QString formatElapsedTime(qint64 elapsedMilliseconds) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Formats receive speed.
     * @param speedKbps Receive speed in decimal kilobits per second.
     * @return A string containing at most three digits after the decimal point.
     * @detail Removes insignificant trailing zeros and returns 0 for non-positive
     *         values.
     */
    QString formatSpeed(double speedKbps) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds a Pattern description for the event log.
     * @param settings Validated Pattern settings.
     * @return A string containing counter, init, block, period, values/block, and
     *         bo=LE.
     * @detail For hexadecimal input, also includes the decimal equivalent.
     */
    QString patternDescription(const PatternSettings &settings) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds a description of the selected COM-port settings.
     * @param none
     * @return A baud/data bits/parity/stops/flow control description string.
     * @detail Used by RxWorker in port-open and port-close events.
     */
    QString serialSettingsDescription() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds a human-readable description of QSerialPortInfo.
     * @param portInfo Serial-port information to describe.
     * @return The port name with description and manufacturer when available.
     * @detail Empty and duplicate optional fields are omitted.
     */
    QString portDescription(const QSerialPortInfo &portInfo) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the selected counter size in bytes.
     * @param none
     * @return A size of 1, 2, 4, or 8 bytes.
     * @detail An unexpected GUI value is safely converted to one byte.
     */
    quint64 counterBytes() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the maximum value of the selected counter.
     * @param none
     * @return The maximum unsigned value for the current counter width.
     * @detail Uses numeric_limits for 64 bits to avoid an invalid shift.
     */
    quint64 maximumCounterValue() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Converts the selected parity to a QSerialPort value.
     * @param none
     * @return QSerialPort::NoParity, EvenParity, or OddParity.
     * @detail Unknown text is safely interpreted as NONE.
     */
    QSerialPort::Parity selectedParity() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Converts the selected stop-bit count to a QSerialPort value.
     * @param none
     * @return QSerialPort::OneStop or QSerialPort::TwoStop.
     * @detail Text 2 maps to TwoStop; every other value maps to OneStop.
     */
    QSerialPort::StopBits selectedStopBits() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Selects a text value in a QComboBox.
     * @param comboBox Pointer to the combo box.
     * @param value Text value to select.
     * @return none
     * @detail Changes the current index only when an exact match is found.
     */
    void selectComboBoxText(QComboBox *comboBox, const QString &value) const;

    Ui::MainWindow *ui;
    RxWorker *m_rxWorker;
    UdpRxWorker *m_udpRxWorker;
    QProcess *m_pingProcess;
    QProcess *m_neighborLookupProcess;
    QThread m_rxThread;
    QThread m_udpRxThread;
    QTimer m_portRefreshTimer;
    QTimer m_pingTimeoutTimer;
    QFile m_logFile;
    QTextStream m_logStream;
    QSet<QString> m_knownPortNames;
    QHash<QString, QString> m_knownPortDescriptions;
    QString m_preferredPortName;
    QString m_openPortName;
    QString m_openPortSettingsDescription;
    QString m_pingDestinationIp;
    QString m_neighborLookupIp;
    QString m_udpOurIp;
    QString m_udpOurMac;
    QString m_udpDestinationMac;
    QString m_udpDestinationIp;
    quint16 m_udpDestinationPort;
    bool m_workerReady;
    bool m_udpWorkerReady;
    bool m_portOpen;
    bool m_testRunning;
    bool m_portOperationPending;
    bool m_receptionCommandPending;
    bool m_portLossRequestPending;
    bool m_portSnapshotInitialized;
    bool m_udpConnected;
    bool m_udpConnectionOperationPending;
    bool m_udpTestRunning;
    bool m_udpReceptionCommandPending;
    bool m_udpDisconnectRequestedByUser;
    bool m_pingInProgress;
    bool m_neighborLookupInProgress;
    bool m_shutdownPrepared;
};

#endif // MAINWINDOW_H
