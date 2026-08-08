#ifndef FILECHECKWORKER_H
#define FILECHECKWORKER_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>

class QFile;
class QTimer;

/**
 * @brief Binary counter-file verification worker.
 * @detail Runs entirely in a dedicated QThread, owns QFile and worker timers, reads the
 *         selected file through one persistent buffer, verifies little-endian counter
 *         values, and sends prepared events and Statistics snapshots to the GUI thread.
 */
class FileCheckWorker final : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Creates the file-check worker object.
     * @param parent Parent QObject; normally omitted before moveToThread().
     * @return none
     * @detail Initializes scalar state only. QFile, QTimer objects, and the persistent
     *         read buffer are created later by initialize() in the worker thread.
     */
    explicit FileCheckWorker(QObject *parent = nullptr);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Destroys the file-check worker object.
     * @param none
     * @return none
     * @detail Stops timers and closes the selected file as a safeguard. Normal shutdown
     *         performs these actions earlier through shutdown().
     */
    ~FileCheckWorker() override;

signals:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports that worker-thread resources are ready.
     * @param none
     * @return none
     * @detail Emitted after QFile, timers, and the persistent read buffer are created.
     */
    void workerReady();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends an event to the GUI for EVENTS and the text log.
     * @param timestamp Event timestamp in HH:MM:SS.mmm format.
     * @param text Event text without a timestamp.
     * @param error true for a red error entry; false for a normal black entry.
     * @return none
     * @detail The timestamp is generated in the worker thread at the actual event time.
     */
    void eventGenerated(const QString &timestamp,
                        const QString &text,
                        bool error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports whether file verification is active.
     * @param running true while a selected file is being checked.
     * @return none
     * @detail The GUI uses the signal to switch CHECK to STOP and lock other modes.
     */
    void checkingStateChanged(bool running);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a prepared FILE Statistics snapshot.
     * @param processedValues Number of complete counter values already checked.
     * @param totalValues Total number of complete counter values in the file.
     * @param counterOk Number of values matching the expected counter.
     * @param counterErrors Number of mismatching values.
     * @param minimumSkipped Minimum counter-jump size recorded for one mismatch.
     * @param averageSkipped Arithmetic mean of counter-jump sizes over mismatch events.
     * @param maximumSkipped Maximum counter-jump size recorded for one mismatch.
     * @param chunkSampleAvailable true after at least one mismatch was recorded.
     * @return none
     * @detail Calculations are performed in the worker thread; the GUI only formats and
     *         displays the supplied values.
     */
    void statisticsUpdated(quint64 processedValues,
                           quint64 totalValues,
                           quint64 counterOk,
                           quint64 counterErrors,
                           quint64 minimumSkipped,
                           double averageSkipped,
                           quint64 maximumSkipped,
                           bool chunkSampleAvailable);

public slots:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Initializes resources owned by the FILE worker thread.
     * @param none
     * @return none
     * @detail Creates QFile and timers, allocates one persistent read buffer, connects
     *         internal callbacks, and reports readiness to the GUI.
     */
    void initialize();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Starts verification of a binary counter file.
     * @param filePath Absolute path of the file to verify.
     * @param counterBits Counter width: 8, 16, or 32 bits.
     * @param initialValue First expected counter value.
     * @param hexadecimalDisplay true to format counter values as hexadecimal in EVENTS.
     * @param patternDescription Preformatted FILE Pattern description for the log.
     * @return none
     * @detail Opens the file in ReadOnly mode, resets Statistics, and schedules repeated
     *         reads through the persistent buffer without blocking the GUI thread.
     */
    void startCheck(const QString &filePath,
                    int counterBits,
                    quint64 initialValue,
                    bool hexadecimalDisplay,
                    const QString &patternDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Stops an active file check at a complete counter boundary.
     * @param none
     * @return none
     * @detail Closes the file after the current worker callback, emits final partial
     *         Statistics, and leaves no truncated counter state in memory.
     */
    void stopCheck();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Shuts down the FILE worker before application exit.
     * @param none
     * @return none
     * @detail Stops an active check, closes QFile and timers, and leaves the object ready
     *         for the main thread to stop its QThread.
     */
    void shutdown();

private slots:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reads and verifies the next file block.
     * @param none
     * @return none
     * @detail Uses one persistent 4 MiB buffer, preserves up to three carry bytes between
     *         short reads, verifies complete counters, and reschedules itself through the
     *         worker event loop until EOF or STOP.
     */
    void processNextBlock();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Performs the one-second FILE Statistics update.
     * @param none
     * @return none
     * @detail Sends current OK, ERR, and Chunks values to the GUI without reading
     *         or modifying the selected file.
     */
    void updateStatistics();

private:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Resets all per-check counters and timing state.
     * @param none
     * @return none
     * @detail Preserves initialized worker resources while clearing the previous result.
     */
    void resetCheckState();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Verifies complete counters stored in the read buffer.
     * @param data Pointer to the first byte to inspect.
     * @param byteCount Number of complete-counter bytes to inspect.
     * @param fileOffset Byte offset of data from the beginning of the file.
     * @return none
     * @detail Decodes little-endian 8-, 16-, or 32-bit values, resynchronizes after each
     *         mismatch, measures the counter jump from the previous received value,
     *         updates Chunks Statistics, and emits detailed errors.
     */
    void verifyBuffer(const char *data,
                      qint64 byteCount,
                      qint64 fileOffset);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Completes an active file check.
     * @param completed true when EOF was reached naturally.
     * @param stoppedByUser true for an explicit STOP command.
     * @param failureText Non-empty English failure description for an abnormal finish.
     * @return none
     * @detail Stops timers, closes QFile, emits final Statistics and a result event, then
     *         clears the active state reported to the GUI.
     */
    void finishCheck(bool completed,
                     bool stoppedByUser,
                     const QString &failureText = QString());

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Emits the current FILE Statistics snapshot.
     * @param none
     * @return none
     * @detail Calculates the average counter-jump chunk only when mismatch samples exist.
     */
    void emitStatisticsSnapshot();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a worker event with the current millisecond timestamp.
     * @param text Event text without a timestamp.
     * @param error true for a red error entry; otherwise false.
     * @return none
     * @detail Keeps event time generation in the FILE worker thread.
     */
    void emitWorkerEvent(const QString &text, bool error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Formats a counter value for FILE events.
     * @param value Unsigned counter value to format.
     * @return Decimal text or uppercase 0x-prefixed hexadecimal text.
     * @detail Uses the same display base as the init value selected in the GUI.
     */
    QString formatCounterValue(quint64 value) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Formats a percentage for FILE result events.
     * @param part Numerator value.
     * @param total Denominator value.
     * @return Percentage text with insignificant trailing zeros removed.
     * @detail Returns 0 when total is zero.
     */
    QString formatPercentage(quint64 part, quint64 total) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns a fixed English description for a QFile error.
     * @param error QFileDevice error value to describe.
     * @return English text independent of the operating-system language.
     * @detail Covers every QFileDevice::FileError available in Qt 5.12.
     */
    QString fileErrorText(int error) const;

    QFile *m_file;
    QTimer *m_processTimer;
    QTimer *m_statisticsTimer;
    QByteArray m_readBuffer;
    QElapsedTimer m_elapsedTimer;
    QString m_filePath;
    QString m_patternDescription;
    QString m_startTime;
    int m_counterBits;
    int m_counterBytes;
    int m_carryBytes;
    quint64 m_counterMask;
    quint64 m_initialValue;
    quint64 m_expectedValue;
    quint64 m_lastReceivedValue;
    quint64 m_totalCompleteValues;
    quint64 m_processedValues;
    quint64 m_counterOk;
    quint64 m_counterErrors;
    quint64 m_minimumSkipped;
    quint64 m_maximumSkipped;
    quint64 m_chunkSampleCount;
    long double m_skippedSum;
    quint64 m_detailedErrorsLogged;
    quint64 m_suppressedErrorEvents;
    qint64 m_totalFileBytes;
    qint64 m_fileBytesRead;
    qint64 m_streamOffset;
    qint64 m_trailingBytes;
    bool m_hexadecimalDisplay;
    bool m_initialized;
    bool m_checkRunning;
    bool m_stopRequested;
    bool m_shuttingDown;
};

#endif // FILECHECKWORKER_H
