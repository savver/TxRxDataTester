#ifndef TXWORKER_H
#define TXWORKER_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QSerialPort>
#include <QString>

class QTimer;

/**
 * @brief Serial-port transmission worker.
 * @detail Runs entirely in a dedicated QThread and owns QSerialPort, transmission and
 *         Statistics timers, pattern generation, and asynchronous write handling.
 */
class TxWorker final : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Creates the transmitter worker object.
     * @param parent Parent QObject; normally omitted before moveToThread().
     * @return none
     * @detail Initializes scalar state only; QSerialPort and timers are created later
     *         by initialize() in the worker thread.
     */
    explicit TxWorker(QObject *parent = nullptr);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Destroys the transmitter worker object.
     * @param none
     * @return none
     * @detail Stops timers and closes the port as a safeguard after normal shutdown.
     */
    ~TxWorker() override;

signals:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports that the TX worker is ready.
     * @param none
     * @return none
     * @detail Emitted after QSerialPort, timers, and internal connections are created
     *         in the worker thread.
     */
    void workerReady();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends an event to the GUI and text log.
     * @param timestamp Event timestamp in HH:MM:SS.mmm format.
     * @param text Event text without a timestamp.
     * @param error true for a red error entry; otherwise false.
     * @return none
     * @detail The timestamp is generated in the TX thread at the moment of the event.
     */
    void eventGenerated(const QString &timestamp,
                        const QString &text,
                        bool error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports the current COM-port state.
     * @param open true when the port is open.
     * @param portName Name of the opened or just-closed port.
     * @param settingsDescription Description of the applied settings.
     * @param causedByFailure true when closing was caused by an error or loss.
     * @return none
     * @detail The GUI receives a state snapshot and never accesses the worker
     *         QSerialPort.
     */
    void portStateChanged(bool open,
                          const QString &portName,
                          const QString &settingsDescription,
                          bool causedByFailure);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports current transmission-operation states.
     * @param testRunning true during continuous block generation.
     * @param singleTransferActive true until a SINGLE block is fully written.
     * @param outputDrainActive true while pending bytes drain after STOP.
     * @return none
     * @detail The GUI uses these flags to enable or disable controls.
     */
    void transmissionStateChanged(bool testRunning,
                                  bool singleTransferActive,
                                  bool outputDrainActive);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a prepared Statistics snapshot to the GUI.
     * @param startTime Transmission start time in HH:MM:SS format.
     * @param elapsedMilliseconds Elapsed monotonic time in milliseconds.
     * @param totalBytesWritten Total number of confirmed transmitted bytes.
     * @param currentCounter Next counter value for a new block.
     * @param speedKbps Speed for the latest measured interval in Kb/s.
     * @return none
     * @detail All calculations occur in the TX thread; the GUI only displays the
     *         result.
     */
    void statisticsUpdated(const QString &startTime,
                           qint64 elapsedMilliseconds,
                           quint64 totalBytesWritten,
                           quint64 currentCounter,
                           double speedKbps);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends the twenty-second Statistics line to the text log.
     * @param line Fully formatted log line with timestamp and Statistics values.
     * @return none
     * @detail The GUI writes the line to QFile without displaying it in EVENTS.
     */
    void periodicLogLineReady(const QString &line);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Confirms STOP processing and reports the pending output size.
     * @param pendingBytes Bytes remaining in QSerialPort after generation stops.
     * @return none
     * @detail The GUI completes the green STOP button entry with this value.
     */
    void stopButtonAccepted(qint64 pendingBytes);

public slots:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Initializes resources owned by the TX worker thread.
     * @param none
     * @return none
     * @detail Creates QSerialPort and timers with TxWorker as parent, connects their
     *         signals, and reports readiness to the GUI.
     */
    void initialize();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Opens and configures the serial port.
     * @param portName Name of the selected port, for example COM13.
     * @param baudRate Selected baud rate.
     * @param parityValue Numeric value of QSerialPort::Parity.
     * @param stopBitsValue Numeric value of QSerialPort::StopBits.
     * @param settingsDescription Preformatted settings description for the log.
     * @return none
     * @detail Performs all QSerialPort operations in the TX thread and reports any
     *         failure to the GUI.
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
     * @detail Aborts active operations, clears device buffers, and closes QSerialPort
     *         in its owning thread.
     */
    void closePort();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Starts continuous transmission of test blocks.
     * @param counterBits Counter width: 8, 16, 32, or 64 bits.
     * @param blockBytes Size of one block in bytes.
     * @param periodMs Block enqueue period in milliseconds.
     * @param initialValue Initial counter value.
     * @param patternDescription Preformatted Pattern description for the log.
     * @return none
     * @detail Validates the port and Pattern, resets Statistics, queues the first
     *         block, and starts the transmission timer.
     */
    void startContinuous(int counterBits,
                         int blockBytes,
                         int periodMs,
                         quint64 initialValue,
                         const QString &patternDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Performs a soft stop of continuous transmission.
     * @param none
     * @return none
     * @detail Stops block generation, captures bytesToWrite(), and lets the queued
     *         output drain completely.
     */
    void stopContinuous();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Transmits one test block.
     * @param counterBits Counter width: 8, 16, 32, or 64 bits.
     * @param blockBytes Size of one block in bytes.
     * @param periodMs Period value used in the Pattern description.
     * @param initialValue Initial counter value.
     * @param patternDescription Preformatted Pattern description for the log.
     * @return none
     * @detail Validates the request, resets Statistics, writes one block, and waits for
     *         bytesWritten confirmation.
     */
    void sendSingle(int counterBits,
                    int blockBytes,
                    int periodMs,
                    quint64 initialValue,
                    const QString &patternDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles loss of the open port detected by the GUI.
     * @param reason Reason text created from QSerialPortInfo.
     * @return none
     * @detail Delegates handling to the common critical port-failure procedure.
     */
    void handleExternalPortLoss(const QString &reason);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Shuts down the TX worker before application exit.
     * @param none
     * @return none
     * @detail Stops transfers and timers, closes the COM port, and leaves the worker
     *         ready for QThread::quit().
     */
    void shutdown();

private slots:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds and queues the next continuous-transmission block.
     * @param none
     * @return none
     * @detail Checks port state and queue capacity; period zero uses a single-shot zero
     *         timer without an endless slot loop.
     */
    void sendNextBlock();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Performs the one-second Statistics update.
     * @param none
     * @return none
     * @detail Calculates the current speed, updates the twenty-second minimum and
     *         maximum, and emits a prepared snapshot.
     */
    void updateStatistics();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Accounts for bytes confirmed by QSerialPort.
     * @param bytes Byte count from QSerialPort::bytesWritten.
     * @return none
     * @detail Updates total bytes, refills period-zero output, and completes SINGLE or
     *         soft STOP when bytesToWrite() reaches zero.
     */
    void handleBytesWritten(qint64 bytes);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Checks that the logically open port remains open.
     * @param none
     * @return none
     * @detail Detects an unexpected QSerialPort close even when no separate critical
     *         errorOccurred signal was received.
     */
    void checkPortHealth();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles a QSerialPort error in the worker thread.
     * @param error Serial-port error code.
     * @return none
     * @detail Logs noncritical errors and stops transmission and closes the port for
     *         critical errors.
     */
    void handleSerialError(QSerialPort::SerialPortError error);

private:
    /**
     * @brief Validated settings of the active data pattern.
     * @detail The structure is used only inside the TX worker thread.
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
     * @brief Validates Pattern arguments and builds worker settings.
     * @param counterBits Counter bit width.
     * @param blockBytes Block size in bytes.
     * @param periodMs Transmission period in milliseconds.
     * @param initialValue Initial counter value.
     * @param settings Output pointer for validated settings.
     * @param errorText Output pointer for a validation error message.
     * @return true when all parameters are valid; otherwise false.
     * @detail Validates supported widths, block alignment, period, and initial-value
     *         range inside the TX thread.
     */
    bool makePatternSettings(int counterBits,
                             int blockBytes,
                             int periodMs,
                             quint64 initialValue,
                             PatternSettings *settings,
                             QString *errorText) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Resets Statistics before START or SINGLE.
     * @param settings Validated settings of the new transfer.
     * @return none
     * @detail Clears totals and interval samples, stores the start time, starts
     *         monotonic timing, and emits the initial zero-speed snapshot.
     */
    void resetStatistics(const PatternSettings &settings);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Updates and emits the current Statistics snapshot.
     * @param includePeriodicSample true only for the one-second timer tick.
     * @return none
     * @detail Calculates speed from byte and monotonic-time deltas and optionally
     *         includes it in the twenty-second minimum and maximum.
     */
    void updateStatisticsSnapshot(bool includePeriodicSample);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Creates the twenty-second log line when its interval expires.
     * @param none
     * @return none
     * @detail Uses the actual monotonic interval, transmitted-byte delta, current
     *         counter, and one-second minimum and maximum speeds.
     */
    void emitPeriodicLogLineIfDue();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Stops continuous transmission internally.
     * @param eventText Optional final event text; an empty string is not logged.
     * @param eventIsError true for a red final event entry.
     * @param clearOutputQueue true to discard pending output forcibly.
     * @return none
     * @detail A soft stop keeps Statistics active until the queue drains; a hard stop
     *         finishes immediately and may call clear(Output).
     */
    void stopContinuousInternal(const QString &eventText,
                                bool eventIsError,
                                bool clearOutputQueue);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Finishes a soft STOP after the output queue becomes empty.
     * @param none
     * @return none
     * @detail Emits the final Statistics snapshot, clears the drain state, and logs the
     *         confirmed transmitted-byte total.
     */
    void finishOutputDrain();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Cancels an unfinished SINGLE transfer.
     * @param clearOutputQueue true to clear pending output forcibly.
     * @return none
     * @detail Captures available Statistics, ends SINGLE state, and optionally discards
     *         bytes not yet transmitted.
     */
    void cancelSingleTransfer(bool clearOutputQueue);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles critical loss of the serial port.
     * @param reason Complete reason text for the red event-log entry.
     * @return none
     * @detail Stops timers and Statistics, closes the device with recursive-error
     *         protection, and reports a failed closed state.
     */
    void handlePortFailure(const QString &reason);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Aborts active transfers before CLOSE or shutdown.
     * @param shutdownMode true while the entire application is shutting down.
     * @return none
     * @detail Captures Statistics and logs normal completion messages; the caller
     *         performs the actual buffer cleanup.
     */
    void abortTransfersForPortClose(bool shutdownMode);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds and writes one block to QSerialPort.
     * @param settings Settings of the block to build and transmit.
     * @return true when the complete block is accepted by the output buffer.
     * @detail Reports memory, write, and partial-write errors; advances the counter
     *         only after the full QByteArray is accepted.
     */
    bool sendDataBlock(const PatternSettings &settings);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the allowed amount of pending output.
     * @param none
     * @return Maximum bytesToWrite window in bytes.
     * @detail Uses at least 4096 bytes for small blocks or four blocks for large ones
     *         to limit the soft-STOP tail.
     */
    qint64 maximumPendingOutputBytes() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Creates a binary block of sequential counter values.
     * @param settings Block and counter settings.
     * @param firstValue First counter value in the block.
     * @param nextValue Output pointer for the next block value.
     * @return Constructed QByteArray.
     * @detail Writes each value least-significant byte first and wraps the counter to
     *         zero after its maximum.
     */
    QByteArray buildDataBlock(const PatternSettings &settings,
                              quint64 firstValue,
                              quint64 *nextValue) const;

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
     * @detail Removes trailing zeros and the decimal point when the fractional part is
     *         zero.
     */
    QString formatSpeed(double speedKbps) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Emits a normal or error event with an exact timestamp.
     * @param text Event text without a timestamp.
     * @param error true for an error; false for a normal entry.
     * @return none
     * @detail Creates the HH:MM:SS.mmm timestamp inside the TX thread before queued
     *         delivery to the GUI.
     */
    void emitWorkerEvent(const QString &text, bool error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Emits the current transmission states.
     * @param none
     * @return none
     * @detail Sends only three Boolean flags to the GUI and never exposes QSerialPort
     *         or QTimer objects across threads.
     */
    void emitTransmissionState();

    QSerialPort *m_serialPort;
    QTimer *m_transmitTimer;
    QTimer *m_statisticsTimer;
    QTimer *m_portHealthTimer;
    QElapsedTimer m_elapsedTimer;
    PatternSettings m_activePattern;
    QString m_startTime;
    QString m_openPortName;
    QString m_openPortSettingsDescription;
    quint64 m_currentCounter;
    quint64 m_totalBytesWritten;
    quint64 m_lastStatisticsBytes;
    qint64 m_lastStatisticsElapsedMs;
    quint64 m_lastPeriodicLogBytes;
    qint64 m_lastPeriodicLogElapsedMs;
    double m_periodicMinimumSpeedKbps;
    double m_periodicMaximumSpeedKbps;
    qint64 m_singleBytesRemaining;
    bool m_initialized;
    bool m_testRunning;
    bool m_singleTransferActive;
    bool m_outputDrainActive;
    bool m_collectTransmissionStatistics;
    bool m_periodicLogStatisticsActive;
    bool m_periodicSpeedSampleAvailable;
    bool m_openOperationInProgress;
    bool m_handlingPortFailure;
    bool m_shuttingDown;
};

#endif // TXWORKER_H
