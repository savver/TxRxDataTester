#ifndef FILEGENERATORWORKER_H
#define FILEGENERATORWORKER_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>

class QFile;
class QTimer;

/**
 * @brief Binary counter-file generation worker.
 * @detail Runs in a dedicated QThread, owns QFile and its Statistics timer, fills a
 *         reusable buffer with little-endian counter values, and keeps disk I/O out of
 *         the GUI thread.
 */
class FileGeneratorWorker final : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Creates the file-generator worker object.
     * @param parent Parent QObject; normally omitted before moveToThread().
     * @return none
     * @detail Initializes scalar state only; QFile, QTimer, and the reusable output
     *         buffer are created later by initialize() in the worker thread.
     */
    explicit FileGeneratorWorker(QObject *parent = nullptr);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Destroys the file-generator worker object.
     * @param none
     * @return none
     * @detail Stops the Statistics timer and closes an open output file as a final
     *         safeguard after normal shutdown.
     */
    ~FileGeneratorWorker() override;

signals:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports that the FILE worker is ready.
     * @param none
     * @return none
     * @detail Emitted after QFile, QTimer, and the reusable output buffer have been
     *         created in the worker thread.
     */
    void workerReady();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a FILE event to the GUI and text log.
     * @param timestamp Event timestamp in HH:MM:SS.mmm format.
     * @param text Event text without a timestamp.
     * @param error true for a red error entry; otherwise false.
     * @return none
     * @detail The timestamp is captured in the FILE worker thread at the moment of the
     *         event.
     */
    void eventGenerated(const QString &timestamp,
                        const QString &text,
                        bool error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports whether file generation is active.
     * @param active true while the output file is open and generation is running.
     * @return none
     * @detail The GUI uses this state to switch the button between START and STOP and
     *         to lock Pattern and other transmission tabs.
     */
    void generationStateChanged(bool active);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a FILE Progress snapshot to the GUI.
     * @param writtenBytes Number of complete counter bytes written to the file.
     * @param targetBytes Requested final file size in bytes.
     * @param minimumSpeedMBps Minimum sampled write speed in MB/s.
     * @param averageSpeedMBps Average write speed from total bytes and elapsed time.
     * @param maximumSpeedMBps Maximum sampled write speed in MB/s.
     * @return none
     * @detail Progress normally updates once per second and is also emitted at START,
     *         STOP, error, and natural completion.
     */
    void progressUpdated(quint64 writtenBytes,
                         quint64 targetBytes,
                         double minimumSpeedMBps,
                         double averageSpeedMBps,
                         double maximumSpeedMBps);

public slots:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Initializes resources owned by the FILE worker thread.
     * @param none
     * @return none
     * @detail Creates QFile and the one-second Statistics timer, allocates one reusable
     *         4 MiB buffer, and reports readiness to the GUI.
     */
    void initialize();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Starts binary counter-file generation.
     * @param filePath Absolute output file path.
     * @param counterBits Counter width: 8, 16, or 32 bits.
     * @param initialValue First counter value written to the file.
     * @param lastValue Expected final counter value after valueCount fields.
     * @param valueCount Number of complete counter fields to write.
     * @param targetBytes Exact aligned output size in bytes.
     * @param patternDescription Preformatted FILE Pattern description for EVENTS.
     * @return none
     * @detail Opens the file with truncation, resets Statistics, emits START service
     *         information, and schedules cooperative chunk generation.
     */
    void startGeneration(const QString &filePath,
                         int counterBits,
                         quint64 initialValue,
                         quint64 lastValue,
                         quint64 valueCount,
                         quint64 targetBytes,
                         const QString &patternDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Stops active file generation.
     * @param none
     * @return none
     * @detail Stops between complete aligned chunks, flushes and closes the file, and
     *         therefore never leaves a partial counter field at the end.
     */
    void stopGeneration();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Shuts down the FILE worker before application exit.
     * @param none
     * @return none
     * @detail Stops active generation after the current complete chunk, closes QFile,
     *         and leaves the worker ready for QThread::quit().
     */
    void shutdown();

private slots:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Generates and writes the next aligned file chunk.
     * @param none
     * @return none
     * @detail Uses the preallocated buffer, writes at most 4 MiB, and reschedules
     *         itself through the worker event loop so STOP remains responsive.
     */
    void writeNextChunk();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Performs the one-second FILE Progress and speed update.
     * @param none
     * @return none
     * @detail Calculates the latest interval speed from actual elapsed milliseconds,
     *         updates min/max samples, and emits a GUI snapshot.
     */
    void updateStatistics();

private:
    /**
     * @brief File-generation completion reason.
     * @detail Separates natural completion, user STOP, application shutdown, and I/O
     *         failure so EVENTS receives an accurate final message.
     */
    enum class FinishReason
    {
        Completed,
        Stopped,
        Shutdown,
        Error
    };

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Validates a FILE generation request.
     * @param filePath Absolute or relative output path to validate.
     * @param counterBits Counter width supplied by the GUI.
     * @param initialValue Initial counter value supplied by the GUI.
     * @param lastValue Expected final counter value supplied by the GUI.
     * @param valueCount Number of counter fields supplied by the GUI.
     * @param targetBytes Requested aligned file size supplied by the GUI.
     * @param errorText Output fixed English validation error.
     * @return true when the request is internally consistent; otherwise false.
     * @detail Rechecks width, range, byte alignment, multiplication, and modulo-wrapped
     *         final counter value in the worker thread.
     */
    bool validateRequest(const QString &filePath,
                         int counterBits,
                         quint64 initialValue,
                         quint64 lastValue,
                         quint64 valueCount,
                         quint64 targetBytes,
                         QString *errorText) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Fills the reusable buffer with counter values.
     * @param byteCount Number of aligned bytes to fill from the beginning of the buffer.
     * @return none
     * @detail Writes every field in little-endian byte order and wraps naturally at the
     *         maximum value selected by counterBits.
     */
    void fillBuffer(int byteCount);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Writes a prepared buffer region completely.
     * @param byteCount Number of bytes to write from the reusable buffer.
     * @param errorText Output fixed English error description when writing fails.
     * @return true when every requested byte was accepted by QFile; otherwise false.
     * @detail Repeats QFile::write() after partial writes and tracks the exact file
     *         position for later aligned cleanup on failure.
     */
    bool writeBuffer(int byteCount, QString *errorText);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Schedules the next cooperative write callback.
     * @param none
     * @return true when QMetaObject accepted the queued invocation.
     * @detail Prevents duplicate write callbacks and keeps long file generation
     *         responsive to STOP and the Statistics timer.
     */
    bool scheduleNextWrite();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Adds one interval-speed sample.
     * @param includeZero true to include a zero-byte timer interval in minimum speed.
     * @return none
     * @detail Uses monotonic elapsed time and avoids adding an artificial zero sample
     *         during finalization immediately after a timer update.
     */
    void sampleSpeed(bool includeZero);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Emits the current Progress snapshot.
     * @param none
     * @return none
     * @detail Uses sampled min/max values and total-bytes-over-total-time average speed.
     */
    void emitProgressSnapshot();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Finalizes generation and closes the output file.
     * @param reason Completion, STOP, shutdown, or error reason.
     * @param errorText Optional fixed English failure detail.
     * @return none
     * @detail Stops timers, preserves complete counter alignment, flushes and closes
     *         QFile, emits final Progress and EVENTS data, and reports idle state.
     */
    void finishGeneration(FinishReason reason,
                          const QString &errorText = QString());

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the average write speed.
     * @param none
     * @return Average speed in MB/s based on all written bytes and elapsed time.
     * @detail Uses a minimum denominator of one millisecond for very small files.
     */
    double averageSpeedMBps() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the effective minimum sampled speed.
     * @param none
     * @return Minimum sampled speed, or average speed when no interval sample exists.
     * @detail Allows sub-second files to report meaningful min/average/max values.
     */
    double minimumSpeedMBps() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the effective maximum sampled speed.
     * @param none
     * @return Maximum sampled speed, or average speed when no interval sample exists.
     * @detail Allows sub-second files to report meaningful min/average/max values.
     */
    double maximumSpeedMBps() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Formats a FILE speed for EVENTS.
     * @param speedMBps Speed in MB/s.
     * @return English numeric text with at most three fractional digits and MB/s suffix.
     * @detail Removes insignificant trailing zeros and normalizes invalid values to zero.
     */
    QString formatSpeed(double speedMBps) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Emits a timestamped worker event.
     * @param text Event text without a timestamp.
     * @param error true for a red error event; otherwise false.
     * @return none
     * @detail Captures the timestamp in the FILE worker thread.
     */
    void emitWorkerEvent(const QString &text, bool error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns a fixed English description for the current QFile error.
     * @param none
     * @return Locale-independent file error description.
     * @detail Avoids placing operating-system-localized error strings in EVENTS.
     */
    QString currentFileErrorText() const;

    QFile *m_outputFile;
    QTimer *m_statisticsTimer;
    QByteArray m_writeBuffer;
    QElapsedTimer m_elapsedTimer;
    QString m_filePath;
    QString m_patternDescription;
    int m_counterBits;
    int m_counterBytes;
    quint64 m_maximumCounterValue;
    quint64 m_counterModulus;
    quint64 m_currentCounter;
    quint64 m_initialValue;
    quint64 m_lastValue;
    quint64 m_valueCount;
    quint64 m_targetBytes;
    quint64 m_writtenBytes;
    quint64 m_lastStatisticsBytes;
    qint64 m_lastStatisticsElapsedMs;
    double m_minimumSpeedMBps;
    double m_maximumSpeedMBps;
    quint64 m_speedSampleCount;
    bool m_initialized;
    bool m_active;
    bool m_writeScheduled;
};

#endif // FILEGENERATORWORKER_H
