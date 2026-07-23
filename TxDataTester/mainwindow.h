#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QColor>
#include <QFile>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSet>
#include <QString>
#include <QTextStream>
#include <QThread>
#include <QTimer>

class QCloseEvent;
class QComboBox;
class TxWorker;

namespace Ui
{
class MainWindow;
}

/**
 * @brief Main application window.
 * @detail Owns the GUI, settings, EVENTS view, and text log, and communicates with
 *         TxWorker running in a dedicated TX thread.
 */
class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * @brief Creates the main application window.
     * @param parent Parent widget of the window.
     * @return none
     * @detail Initializes the GUI, validators, settings, event log, and dedicated TX
     *         worker thread.
     */
    explicit MainWindow(QWidget *parent = nullptr);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Destroys the main application window.
     * @param none
     * @return none
     * @detail Ensures orderly shutdown of the TX thread and log before releasing the Qt
     *         Designer user interface.
     */
    ~MainWindow() override;

signals:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests opening of a serial port in the TX thread.
     * @param portName Name of the selected serial port.
     * @param baudRate Selected baud rate.
     * @param parityValue Numeric value of QSerialPort::Parity.
     * @param stopBitsValue Numeric value of QSerialPort::StopBits.
     * @param settingsDescription Preformatted settings description for the log.
     * @return none
     * @detail Carries validated GUI settings to TxWorker through a queued connection.
     */
    void openPortRequested(const QString &portName,
                           qint32 baudRate,
                           int parityValue,
                           int stopBitsValue,
                           const QString &settingsDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests closing of the serial port in the TX thread.
     * @param none
     * @return none
     * @detail Keeps every QSerialPort operation in the thread that owns the port.
     */
    void closePortRequested();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests continuous test-pattern transmission.
     * @param counterBits Counter width in bits.
     * @param blockBytes Block size in bytes.
     * @param periodMs Block period in milliseconds.
     * @param initialValue Initial counter value.
     * @param patternDescription Preformatted Pattern description for the log.
     * @return none
     * @detail TxWorker validates the scalar values again before starting transmission.
     */
    void startContinuousRequested(int counterBits,
                                  int blockBytes,
                                  int periodMs,
                                  quint64 initialValue,
                                  const QString &patternDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests a soft stop of continuous transmission.
     * @param none
     * @return none
     * @detail Stops generation of new blocks and lets pending output drain without
     *         calling clear(Output).
     */
    void stopContinuousRequested();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests transmission of one test block.
     * @param counterBits Counter width in bits.
     * @param blockBytes Block size in bytes.
     * @param periodMs Current Period value used in the Pattern description.
     * @param initialValue Initial counter value.
     * @param patternDescription Preformatted Pattern description for the log.
     * @return none
     * @detail The block is built and written entirely in the TX worker thread.
     */
    void singleRequested(int counterBits,
                         int blockBytes,
                         int periodMs,
                         quint64 initialValue,
                         const QString &patternDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Notifies TxWorker that the open port disappeared.
     * @param reason Reason text for the red event-log entry.
     * @return none
     * @detail The GUI detects the loss through QSerialPortInfo, while TxWorker closes
     *         the port in its owning thread.
     */
    void externalPortLossDetected(const QString &reason);

protected:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles closing of the main window.
     * @param event Qt close event for the window.
     * @return none
     * @detail Stops the TX thread, saves settings, closes the log, and then passes the
     *         event to QMainWindow.
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
     * @detail The values are passed to the worker thread as independent scalar
     *         arguments.
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

private slots:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Refreshes the list of available serial ports.
     * @param none
     * @return none
     * @detail When closed, updates the combo box and logs added or removed ports; when
     *         open, monitors the selected device.
     */
    void refreshSerialPorts();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests opening of the selected serial port.
     * @param none
     * @return none
     * @detail Prevents duplicate operations and sends the selected settings to TxWorker
     *         without creating QSerialPort in the GUI thread.
     */
    void openSerialPort();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests closing of the open serial port.
     * @param none
     * @return none
     * @detail Sends a queued command to the QSerialPort owner and blocks duplicate
     *         commands until the state reply arrives.
     */
    void closeSerialPort();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Normalizes the transmitted block size.
     * @param none
     * @return none
     * @detail Replaces an empty or zero value with one counter, rounds upward to the
     *         required alignment, and clamps the result to int range.
     */
    void normalizeBlockSize();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Validates the initial counter value.
     * @param none
     * @return none
     * @detail Accepts decimal or 0x-prefixed hexadecimal input, preserves hexadecimal
     *         format, and falls back to zero on error or overflow.
     */
    void normalizeInitialValue();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Normalizes the block transmission period.
     * @param none
     * @return none
     * @detail Converts an empty field to zero and clamps values above INT_MAX to the
     *         largest QTimer interval supported by Qt 5.12.
     */
    void normalizePeriod();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Updates the calculated transmission time of one block.
     * @param none
     * @return none
     * @detail Uses the start bit, eight data bits, optional parity, selected stop bits,
     *         baud rate, and block size, then rounds upward to microseconds.
     */
    void updateBlockTransmissionTime();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles a change of counter bit width.
     * @param none
     * @return none
     * @detail Realigns block size and validates the initial-value range again.
     */
    void handleCounterBitsChanged();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests start of continuous transmission.
     * @param none
     * @return none
     * @detail Logs the START button press in green, validates Pattern, and sends the
     *         command to the dedicated TX thread.
     */
    void startTest();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests a soft stop of continuous transmission.
     * @param none
     * @return none
     * @detail Stores the exact click timestamp and asks TxWorker to stop generation and
     *         report the pending output size.
     */
    void stopTest();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Requests transmission of one block.
     * @param none
     * @return none
     * @detail Logs the SINGLE button press in green, validates Pattern, and sends
     *         scalar parameters to the worker thread.
     */
    void handleSingleButton();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles readiness of the dedicated TX worker thread.
     * @param none
     * @return none
     * @detail Marks QSerialPort and worker timers as ready and updates the controls.
     */
    void handleWorkerReady();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Receives a normal or error event from TxWorker.
     * @param timestamp Timestamp created in the worker thread.
     * @param text Event text without a timestamp.
     * @param error true for a red error entry; otherwise false.
     * @return none
     * @detail Displays and logs the event in the main GUI thread while preserving the
     *         worker-side timestamp.
     */
    void handleWorkerEvent(const QString &timestamp,
                           const QString &text,
                           bool error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Receives a new COM-port state from TxWorker.
     * @param open true when the port was opened successfully.
     * @param portName Name of the opened or just-closed port.
     * @param settingsDescription Description of the applied port settings.
     * @param causedByFailure true when closing was caused by a failure.
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
     * @brief Receives transmission-operation states from TxWorker.
     * @param testRunning true while continuous START transmission is active.
     * @param singleTransferActive true until a SINGLE block is fully written.
     * @param outputDrainActive true while the STOP remainder is draining.
     * @return none
     * @detail Clears pending queued-command state and synchronizes control
     *         availability.
     */
    void handleTransmissionStateChanged(bool testRunning,
                                        bool singleTransferActive,
                                        bool outputDrainActive);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Receives a prepared Statistics snapshot from TxWorker.
     * @param startTime Test start time in HH:MM:SS format.
     * @param elapsedMilliseconds Elapsed test time in milliseconds.
     * @param totalBytesWritten Total number of confirmed transmitted bytes.
     * @param currentCounter Next counter value for a new block.
     * @param speedKbps Measured speed for the latest interval in Kb/s.
     * @return none
     * @detail Formats and displays the values without reading any TX-thread state.
     */
    void handleStatisticsUpdated(const QString &startTime,
                                 qint64 elapsedMilliseconds,
                                 quint64 totalBytesWritten,
                                 quint64 currentCounter,
                                 double speedKbps);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Completes the green STOP button entry.
     * @param pendingBytes Output bytes pending after block generation stopped.
     * @return none
     * @detail Uses the timestamp captured at the click so queued delivery does not
     *         change the user-action time.
     */
    void handleStopButtonAccepted(qint64 pendingBytes);

private:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Creates and starts the dedicated TX worker thread.
     * @param none
     * @return none
     * @detail Moves TxWorker to QThread, connects both directions with queued
     *         connections, and starts the thread after setup is complete.
     */
    void initializeTxThread();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Updates availability of all controls.
     * @param none
     * @return none
     * @detail Considers worker readiness, port state, active transmission, output
     *         draining, and pending asynchronous GUI commands.
     */
    void updateControlStates();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Adds an event with the current timestamp.
     * @param eventText Event text without a timestamp.
     * @param eventType Event type that controls color and emphasis.
     * @return none
     * @detail Used for GUI actions and errors generated in the main thread.
     */
    void appendEvent(const QString &eventText, EventType eventType);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Adds an event with a prepared timestamp.
     * @param timestamp Timestamp in HH:MM:SS.mmm format.
     * @param eventText Event text without a timestamp.
     * @param eventType Event type that controls color and emphasis.
     * @return none
     * @detail Preserves the exact worker-event time even when the GUI is busy or
     *         minimized.
     */
    void appendTimestampedEvent(const QString &timestamp,
                                const QString &eventText,
                                EventType eventType);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Writes a prepared line only to the text log.
     * @param line Complete line without a trailing newline.
     * @return none
     * @detail Runs in the GUI thread, appends a newline, and flushes the file
     *         immediately.
     */
    void writeLogLine(const QString &line);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the color for an event-log entry.
     * @param eventType Type of event to color.
     * @return Black, green, or red QColor for the selected event type.
     * @detail Green is reserved for direct button presses.
     */
    QColor eventColor(EventType eventType) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Creates the logs directory and a new log file for this run.
     * @param none
     * @return none
     * @detail Opens a UTF-8 txdatatester_log__date__time.txt file next to the
     *         executable.
     */
    void initializeLogFile();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Flushes and closes the text log file.
     * @param none
     * @return none
     * @detail Flushes pending text and detaches QTextStream from QFile.
     */
    void closeLogFile();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Restores settings from the previous run.
     * @param none
     * @return none
     * @detail Loads window geometry, COM settings, and Pattern values, including
     *         migration from the former TxRxDataTester application name.
     */
    void loadSettings();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Saves the current application settings.
     * @param none
     * @return none
     * @detail Normalizes editable fields and stores window geometry, COM settings, and
     *         Pattern values through QSettings.
     */
    void saveSettings();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Performs one-time application shutdown.
     * @param none
     * @return none
     * @detail Synchronously shuts down TxWorker, waits for QThread, processes final
     *         events, saves settings, and closes the log.
     */
    void prepareShutdown();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Repopulates the serial-port combo box.
     * @param ports Current list of available serial ports.
     * @return none
     * @detail Preserves the preferred port when possible and leaves the list empty when
     *         no ports are available.
     */
    void updatePortComboBox(const QList<QSerialPortInfo> &ports);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds a set of serial-port names.
     * @param ports Serial-port information list to process.
     * @return Set containing every non-empty port name.
     * @detail The set is used to detect added and removed ports.
     */
    QSet<QString> portNames(const QList<QSerialPortInfo> &ports) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds descriptions indexed by serial-port name.
     * @param ports Serial-port information list to process.
     * @return Hash that maps each port name to its formatted description.
     * @detail Optional description and manufacturer fields are included when available.
     */
    QHash<QString, QString> portDescriptions(
        const QList<QSerialPortInfo> &ports) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Synchronizes the stored serial-port snapshot without logging.
     * @param ports Current list of available serial ports.
     * @return none
     * @detail Used during initialization and while an open port is monitored.
     */
    void synchronizePortSnapshot(const QList<QSerialPortInfo> &ports);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reads and validates Pattern values from the GUI.
     * @param settings Output pointer for validated Pattern settings.
     * @param errorText Output pointer for a validation error message.
     * @return true when all Pattern values are valid; otherwise false.
     * @detail Checks counter width, block alignment, period, and initial-value range.
     */
    bool readPatternSettings(PatternSettings *settings,
                             QString *errorText) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Parses the initial counter value.
     * @param value Output pointer for the parsed unsigned value.
     * @return true when decimal or 0x-prefixed input is valid and fits the counter.
     * @detail Does not modify the GUI and can therefore be called from const validation
     *         code.
     */
    bool parseInitialValue(quint64 *value) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Formats elapsed test time.
     * @param elapsedMilliseconds Elapsed duration in milliseconds.
     * @return HH:MM:SS string with an unlimited number of hours.
     * @detail Hours are calculated from the full duration and do not wrap after 23.
     */
    QString formatElapsedTime(qint64 elapsedMilliseconds) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Formats transmission speed.
     * @param speedKbps Speed in decimal kilobits per second.
     * @return String containing at most three digits after the decimal point.
     * @detail Removes insignificant trailing zeros and returns 0 for invalid values.
     */
    QString formatSpeed(double speedKbps) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds a Pattern description for the event log.
     * @param settings Validated Pattern settings.
     * @return String containing counter, init, block, period, values/block, and bo=LE.
     * @detail For hexadecimal input, the decimal equivalent is included as well.
     */
    QString patternDescription(const PatternSettings &settings) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds a description of the selected COM-port settings.
     * @param none
     * @return Baud/data bits/parity/stops/flow-control description string.
     * @detail The description is sent to TxWorker for open and close events.
     */
    QString serialSettingsDescription() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds a human-readable description of QSerialPortInfo.
     * @param portInfo Serial-port information to describe.
     * @return Port name with description and manufacturer when available.
     * @detail Empty and duplicate optional fields are omitted.
     */
    QString portDescription(const QSerialPortInfo &portInfo) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the selected counter size in bytes.
     * @param none
     * @return Counter size of 1, 2, 4, or 8 bytes.
     * @detail An unexpected GUI value is safely converted to one byte.
     */
    quint64 counterBytes() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the maximum value of the selected counter.
     * @param none
     * @return Maximum unsigned value for the current counter width.
     * @detail Uses numeric_limits for 64 bits to avoid an invalid shift.
     */
    quint64 maximumCounterValue() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Converts the selected parity to QSerialPort format.
     * @param none
     * @return QSerialPort::NoParity, EvenParity, or OddParity.
     * @detail Unknown text is safely interpreted as NONE.
     */
    QSerialPort::Parity selectedParity() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Converts the selected stop-bit count to QSerialPort format.
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
    TxWorker *m_txWorker;
    QThread m_txThread;
    QTimer m_portRefreshTimer;
    QFile m_logFile;
    QTextStream m_logStream;
    QSet<QString> m_knownPortNames;
    QHash<QString, QString> m_knownPortDescriptions;
    QString m_preferredPortName;
    QString m_openPortName;
    QString m_openPortSettingsDescription;
    QString m_pendingStopTimestamp;
    bool m_workerReady;
    bool m_portOpen;
    bool m_testRunning;
    bool m_singleTransferActive;
    bool m_outputDrainActive;
    bool m_portOperationPending;
    bool m_transmissionCommandPending;
    bool m_portLossRequestPending;
    bool m_portSnapshotInitialized;
    bool m_shutdownPrepared;
};

#endif // MAINWINDOW_H
