#ifndef RXWORKER_H
#define RXWORKER_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QSerialPort>
#include <QString>

class QTimer;

/**
 * @brief Serial-port receiver worker object.
 * @detail The object runs entirely in a dedicated QThread and owns QSerialPort, the
 *         Statistics and port-health timers, the receive buffer, and all little-endian
 *         counter verification logic. The GUI and file log stay in the main thread and
 *         receive only prepared signals.
 */
class RxWorker final : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Creates the receiver worker object.
     * @param parent Parent QObject; normally omitted before moveToThread().
     * @return none
     * @detail Initializes scalar state only. QSerialPort and QTimer objects are created
     *         later by initialize() after the worker is moved to its thread.
     */
    explicit RxWorker(QObject *parent = nullptr);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Destroys the receiver worker object.
     * @param none
     * @return none
     * @detail Stops timers and closes the port as a safeguard. During normal shutdown
     *         these actions are already performed by shutdown().
     */
    ~RxWorker() override;

signals:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports that the RX worker thread is ready.
     * @param none
     * @return none
     * @detail Emitted after QSerialPort, QTimer objects, and internal connections are
     *         created.
     */
    void workerReady();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends an event to the GUI for EVENTS and the text log.
     * @param timestamp Event timestamp in HH:MM:SS.mmm format.
     * @param text Event text without a timestamp.
     * @param error true for a red error entry; false for a normal black entry.
     * @return none
     * @detail The timestamp is created in the RX thread at the actual event time.
     */
    void eventGenerated(const QString &timestamp,
                        const QString &text,
                        bool error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a new serial-port state to the GUI.
     * @param open true for an open port; false for a closed port.
     * @param portName Name of the opened or just-closed port.
     * @param settingsDescription Description of the applied COM-port settings.
     * @param causedByFailure true for an emergency close or device loss.
     * @return none
     * @detail The GUI uses this signal only to synchronize widgets and never accesses
     *         QSerialPort directly.
     */
    void portStateChanged(bool open,
                          const QString &portName,
                          const QString &settingsDescription,
                          bool causedByFailure);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends the active input-verification state.
     * @param running true between a successful START and completion of STOP.
     * @return none
     * @detail Clears the pending queued-command state and updates the GUI buttons.
     */
    void receptionStateChanged(bool running);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a prepared Statistics snapshot to the GUI.
     * @param startTime Verification start time in HH:MM:SS format.
     * @param elapsedMilliseconds Elapsed test time in milliseconds.
     * @param totalBytesReceived Total bytes received during the test.
     * @param currentCounter Last completely received counter value.
     * @param counterOk Number of values matching the expected counter.
     * @param counterErrors Number of values not matching the expected counter.
     * @param speedKbps Measured receive speed for the latest interval in Kb/s.
     * @return none
     * @detail All calculations are performed in the RX thread; the GUI only formats
     *         fields.
     */
    void statisticsUpdated(const QString &startTime,
                           qint64 elapsedMilliseconds,
                           quint64 totalBytesReceived,
                           quint64 currentCounter,
                           quint64 counterOk,
                           quint64 counterErrors,
                           double speedKbps);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends the additional twenty-second file-log line.
     * @param line Complete preformatted line with timestamp and values.
     * @return none
     * @detail The line is written only to the text file and is not displayed in EVENTS.
     */
    void periodicLogLineReady(const QString &line);

public slots:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Initializes resources owned by the RX worker thread.
     * @param none
     * @return none
     * @detail Creates QSerialPort and the timers with RxWorker as parent, connects
     *         their signals, and reports readiness to the GUI.
     */
    void initialize();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Opens and configures the serial port.
     * @param portName Name of the selected port, for example COM13.
     * @param baudRate Selected baud rate.
     * @param parityValue Numeric value of QSerialPort::Parity.
     * @param stopBitsValue Numeric value of QSerialPort::StopBits.
     * @param settingsDescription Preformatted settings description for the event log.
     * @return none
     * @detail The port is opened in ReadOnly mode with 8 data bits and no flow control.
     *         All operations execute in the RX worker thread.
     */
    void openPort(const QString &portName,
                  qint32 baudRate,
                  int parityValue,
                  int stopBitsValue,
                  const QString &settingsDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Closes the serial port in response to CLOSE.
     * @param none
     * @return none
     * @detail If a test is active, finalizes reception and Statistics first, then
     *         closes QSerialPort in its owning thread.
     */
    void closePort();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Starts reception and sequential-counter verification.
     * @param counterBits Counter width: 8, 16, 32, or 64 bits.
     * @param blockBytes Informational transmitter block size in bytes.
     * @param periodMs Informational transmitter block period in milliseconds.
     * @param initialValue First expected counter value.
     * @param patternDescription Preformatted Pattern description for the event log.
     * @return none
     * @detail Discards bytes accumulated before START and parses the input as one
     *         continuous stream independent of block and readyRead() boundaries.
     */
    void startReception(int counterBits,
                        int blockBytes,
                        int periodMs,
                        quint64 initialValue,
                        const QString &patternDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Stops reception and verification in response to STOP.
     * @param none
     * @return none
     * @detail Captures the final Statistics snapshot, stops the test, and leaves the
     *         COM port open. Later incoming bytes are read and discarded.
     */
    void stopReception();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles loss of the open port detected by the GUI.
     * @param reason Reason text created from QSerialPortInfo.
     * @return none
     * @detail Delegates handling to the common critical device-loss procedure.
     */
    void handleExternalPortLoss(const QString &reason);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Shuts down the RX worker before application exit.
     * @param none
     * @return none
     * @detail Stops an active test, closes the COM port and timers, and leaves the
     *         worker ready for the main thread to call QThread::quit().
     */
    void shutdown();

private slots:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reads all currently available serial-port bytes.
     * @param none
     * @return none
     * @detail When no test is active, discards data so START begins with fresh bytes.
     *         During a test, appends bytes to the receive buffer and verifies counters.
     */
    void handleReadyRead();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Performs the one-second Statistics update.
     * @param none
     * @return none
     * @detail Calculates speed from the actual interval, updates the twenty-second
     *         minimum and maximum, and sends the prepared snapshot to the GUI.
     */
    void updateStatistics();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Checks that the logically open port remains open.
     * @param none
     * @return none
     * @detail Once per second, detects an unexpected QSerialPort close even when no
     *         separate critical errorOccurred signal was received.
     */
    void checkPortHealth();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles a QSerialPort error in the worker thread.
     * @param error Serial-port error code.
     * @return none
     * @detail Critical resource, read, and device-not-found errors close the port.
     *         Parity, framing, and break errors are logged without closing it.
     */
    void handleSerialError(QSerialPort::SerialPortError error);

private:
    /**
     * @brief Validated settings of the active data pattern.
     * @detail blockBytes and periodMs are retained for the log, while input parsing
     *         depends only on counterBytes and initialValue.
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

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Validates Pattern arguments and builds the worker settings structure.
     * @param counterBits Counter bit width.
     * @param blockBytes Informational block size in bytes.
     * @param periodMs Informational block period in milliseconds.
     * @param initialValue First expected counter value.
     * @param settings Output pointer for the validated settings.
     * @param errorText Output pointer for a validation error message.
     * @return true when all parameters are valid; otherwise false.
     * @detail Validates supported widths, block alignment, period, and the init value
     *         range.
     */
    bool makePatternSettings(int counterBits,
                             int blockBytes,
                             int periodMs,
                             quint64 initialValue,
                             PatternSettings *settings,
                             QString *errorText) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Resets Statistics and verification state before START.
     * @param settings Validated settings for the new test.
     * @return none
     * @detail Stores the local start time, sets expected to init, clears interval and
     *         total counters, starts monotonic timing, and emits the initial snapshot.
     */
    void resetStatistics(const PatternSettings &settings);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Processes every complete counter value in the receive buffer.
     * @param none
     * @return none
     * @detail Incomplete trailing bytes remain for the next readyRead(). A mismatch
     *         increments counter err once and changes the next expected value to
     *         received plus one.
     */
    void processReceiveBuffer();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Decodes one little-endian counter value.
     * @param data Pointer to the first byte of the value.
     * @param byteCount Value size: 1, 2, 4, or 8 bytes.
     * @return The received counter represented as an unsigned 64-bit value.
     * @detail Performs byte-by-byte conversion independently of host byte order and
     *         alignment.
     */
    quint64 decodeLittleEndianCounter(const char *data, int byteCount) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the next counter value with wraparound.
     * @param value Current counter value.
     * @return value + 1, or 0 after the maximum value of the selected width.
     * @detail Uses the precomputed maximumCounterValue and works identically for 8
     *         through 64 bits.
     */
    quint64 nextCounterValue(quint64 value) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Updates and emits the current Statistics snapshot.
     * @param includePeriodicSample true only for the one-second timer tick.
     * @return none
     * @detail Calculates speed as delta_bytes * 8 / delta_time_ms, which yields decimal
     *         kilobits per second for the measured millisecond interval.
     */
    void updateStatisticsSnapshot(bool includePeriodicSample);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Creates the twenty-second log line when its interval has elapsed.
     * @param none
     * @return none
     * @detail Uses the actual monotonic interval, delta_rx_bytes, match and error
     *         counters, and the one-second minimum and maximum speeds. After emitting
     *         the line, starts a new interval.
     */
    void emitPeriodicLogLineIfDue();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Internally finalizes an active reception test.
     * @param reasonText Optional stop reason; an empty string is not logged.
     * @param reasonIsError true for a red reason entry; false for a normal black entry.
     * @return none
     * @detail Creates the final snapshot before clearing state, stops the timer, clears
     *         an incomplete tail, and emits receptionStateChanged(false).
     */
    void stopReceptionInternal(const QString &reasonText,
                               bool reasonIsError);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles critical loss of the serial port.
     * @param reason Complete reason text for the red event-log entry.
     * @return none
     * @detail Prevents recursive error handling, stops the test, closes the port, and
     *         marks portStateChanged as caused by a failure.
     */
    void handlePortFailure(const QString &reason);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Formats elapsed test time.
     * @param elapsedMilliseconds Elapsed duration in milliseconds.
     * @return A HH:MM:SS string with an unlimited number of hours.
     * @detail Hours are calculated from the full duration and do not wrap after 23.
     */
    QString formatElapsedTime(qint64 elapsedMilliseconds) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Formats receive speed.
     * @param speedKbps Speed in decimal kilobits per second.
     * @return A string containing at most three digits after the decimal point.
     * @detail Removes trailing zeros and returns 0 for non-positive or non-numeric
     *         values.
     */
    QString formatSpeed(double speedKbps) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Emits a normal or error event with an exact timestamp.
     * @param text Event text without a timestamp.
     * @param error true for an error; false for a normal entry.
     * @return none
     * @detail Creates the HH:MM:SS.mmm timestamp before queued delivery to the GUI.
     */
    void emitWorkerEvent(const QString &text, bool error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Emits the current reception state.
     * @param none
     * @return none
     * @detail Centralizes delivery of m_testRunning to the GUI.
     */
    void emitReceptionState();

    QSerialPort *m_serialPort;
    QTimer *m_statisticsTimer;
    QTimer *m_portHealthTimer;
    QElapsedTimer m_elapsedTimer;
    QByteArray m_receiveBuffer;
    PatternSettings m_activePattern;
    QString m_startTime;
    QString m_openPortName;
    QString m_openPortSettingsDescription;
    quint64 m_expectedCounter;
    quint64 m_lastReceivedCounter;
    quint64 m_totalBytesReceived;
    quint64 m_lastStatisticsBytes;
    qint64 m_lastStatisticsElapsedMs;
    quint64 m_counterOk;
    quint64 m_counterErrors;
    quint64 m_lastPeriodicLogBytes;
    quint64 m_lastPeriodicCounterOk;
    quint64 m_lastPeriodicCounterErrors;
    qint64 m_lastPeriodicLogElapsedMs;
    double m_periodicMinimumSpeedKbps;
    double m_periodicMaximumSpeedKbps;
    bool m_periodicSpeedSampleAvailable;
    bool m_initialized;
    bool m_testRunning;
    bool m_openOperationInProgress;
    bool m_handlingPortFailure;
    bool m_shuttingDown;
};

#endif // RXWORKER_H
