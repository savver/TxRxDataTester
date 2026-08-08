#include "filegeneratorworker.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr int kStatisticsIntervalMs = 1000;
constexpr int kWriteBufferBytes = 4 * 1024 * 1024;
constexpr double kBytesPerMegabyte = 1024.0 * 1024.0;

/**
 * @brief Returns a fixed English description for a QFileDevice error.
 * @param error QFileDevice error value to describe.
 * @return Locale-independent English file error text.
 * @detail Covers every QFileDevice::FileError value available in Qt 5.12.
 */
QString fileErrorText(QFileDevice::FileError error)
{
    switch (error)
    {
    case QFileDevice::NoError:
        return QStringLiteral("no error");
    case QFileDevice::ReadError:
        return QStringLiteral("read error");
    case QFileDevice::WriteError:
        return QStringLiteral("write error");
    case QFileDevice::FatalError:
        return QStringLiteral("fatal file error");
    case QFileDevice::ResourceError:
        return QStringLiteral("file resource error");
    case QFileDevice::OpenError:
        return QStringLiteral("file open error");
    case QFileDevice::AbortError:
        return QStringLiteral("file operation aborted");
    case QFileDevice::TimeOutError:
        return QStringLiteral("file operation timed out");
    case QFileDevice::UnspecifiedError:
        return QStringLiteral("unspecified file error");
    case QFileDevice::RemoveError:
        return QStringLiteral("file remove error");
    case QFileDevice::RenameError:
        return QStringLiteral("file rename error");
    case QFileDevice::PositionError:
        return QStringLiteral("file position error");
    case QFileDevice::ResizeError:
        return QStringLiteral("file resize error");
    case QFileDevice::PermissionsError:
        return QStringLiteral("file permission error");
    case QFileDevice::CopyError:
        return QStringLiteral("file copy error");
    }

    return QStringLiteral("unrecognized file error");
}
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates the file-generator worker object.
 * @param parent Parent QObject; normally omitted before moveToThread().
 * @return none
 * @detail Initializes scalar state only; thread-owned Qt resources are created by
 *         initialize().
 */
FileGeneratorWorker::FileGeneratorWorker(QObject *parent)
    : QObject(parent)
    , m_outputFile(nullptr)
    , m_statisticsTimer(nullptr)
    , m_counterBits(8)
    , m_counterBytes(1)
    , m_maximumCounterValue(0xFFU)
    , m_counterModulus(0x100U)
    , m_currentCounter(0)
    , m_initialValue(0)
    , m_lastValue(0)
    , m_valueCount(0)
    , m_targetBytes(0)
    , m_writtenBytes(0)
    , m_lastStatisticsBytes(0)
    , m_lastStatisticsElapsedMs(0)
    , m_minimumSpeedMBps(std::numeric_limits<double>::max())
    , m_maximumSpeedMBps(0.0)
    , m_speedSampleCount(0)
    , m_initialized(false)
    , m_active(false)
    , m_writeScheduled(false)
{
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Destroys the file-generator worker object.
 * @param none
 * @return none
 * @detail Stops the timer and closes QFile as a final safeguard.
 */
FileGeneratorWorker::~FileGeneratorWorker()
{
    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }

    if (m_outputFile != nullptr && m_outputFile->isOpen())
    {
        m_outputFile->close();
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Initializes resources owned by the FILE worker thread.
 * @param none
 * @return none
 * @detail Creates QFile, QTimer, and the reusable 4 MiB output buffer in the owning
 *         thread.
 */
void FileGeneratorWorker::initialize()
{
    if (m_initialized)
    {
        emit workerReady();
        return;
    }

    m_outputFile = new QFile(this);
    m_statisticsTimer = new QTimer(this);
    m_statisticsTimer->setInterval(kStatisticsIntervalMs);
    m_statisticsTimer->setSingleShot(false);
    m_statisticsTimer->setTimerType(Qt::PreciseTimer);
    connect(m_statisticsTimer,
            &QTimer::timeout,
            this,
            &FileGeneratorWorker::updateStatistics);

    m_writeBuffer.resize(kWriteBufferBytes);
    m_initialized = true;
    emit workerReady();
}

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
 * @detail Validates the request, creates the folder when needed, truncates the output
 *         file, resets Progress, and starts cooperative chunk generation.
 */
void FileGeneratorWorker::startGeneration(const QString &filePath,
                                          int counterBits,
                                          quint64 initialValue,
                                          quint64 lastValue,
                                          quint64 valueCount,
                                          quint64 targetBytes,
                                          const QString &patternDescription)
{
    if (!m_initialized || m_outputFile == nullptr || m_statisticsTimer == nullptr)
    {
        emitWorkerEvent(tr("FILE START failed: the FILE worker is not initialized"),
                        true);
        emit generationStateChanged(false);
        return;
    }

    if (m_active)
    {
        emitWorkerEvent(tr("FILE START failed: file generation is already active"),
                        true);
        emit generationStateChanged(true);
        return;
    }

    QString errorText;
    if (!validateRequest(filePath,
                         counterBits,
                         initialValue,
                         lastValue,
                         valueCount,
                         targetBytes,
                         &errorText))
    {
        emitWorkerEvent(tr("FILE Pattern error: %1").arg(errorText), true);
        emit generationStateChanged(false);
        return;
    }

    const QFileInfo outputInfo(filePath);
    QDir outputDirectory = outputInfo.dir();
    if (!outputDirectory.exists()
        && !QDir().mkpath(outputDirectory.absolutePath()))
    {
        emitWorkerEvent(
            tr("FILE START failed: cannot create output folder %1")
                .arg(QDir::toNativeSeparators(outputDirectory.absolutePath())),
            true);
        emit generationStateChanged(false);
        return;
    }

    m_outputFile->setFileName(outputInfo.absoluteFilePath());
    if (!m_outputFile->open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        emitWorkerEvent(
            tr("FILE START failed: cannot open %1: %2 (code %3)")
                .arg(QDir::toNativeSeparators(outputInfo.absoluteFilePath()),
                     currentFileErrorText())
                .arg(static_cast<int>(m_outputFile->error())),
            true);
        emit generationStateChanged(false);
        return;
    }

    m_filePath = outputInfo.absoluteFilePath();
    m_patternDescription = patternDescription;
    m_counterBits = counterBits;
    m_counterBytes = counterBits / 8;
    m_maximumCounterValue = counterBits == 32
                                ? 0xFFFFFFFFULL
                                : ((quint64(1) << counterBits) - 1U);
    m_counterModulus = m_maximumCounterValue + 1U;
    m_currentCounter = initialValue;
    m_initialValue = initialValue;
    m_lastValue = lastValue;
    m_valueCount = valueCount;
    m_targetBytes = targetBytes;
    m_writtenBytes = 0;
    m_lastStatisticsBytes = 0;
    m_lastStatisticsElapsedMs = 0;
    m_minimumSpeedMBps = std::numeric_limits<double>::max();
    m_maximumSpeedMBps = 0.0;
    m_speedSampleCount = 0;
    m_writeScheduled = false;
    m_active = true;
    m_elapsedTimer.restart();
    m_statisticsTimer->start();

    emit generationStateChanged(true);
    emitProgressSnapshot();
    emitWorkerEvent(
        tr("START file generation: %1; file=%2; bo=LE")
            .arg(m_patternDescription,
                 QDir::toNativeSeparators(m_filePath)),
        false);

    if (!scheduleNextWrite())
    {
        finishGeneration(FinishReason::Error,
                         tr("failed to schedule the first file write callback"));
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Stops active file generation.
 * @param none
 * @return none
 * @detail Runs between complete chunks, so the final file length remains aligned to a
 *         whole counter field.
 */
void FileGeneratorWorker::stopGeneration()
{
    if (!m_active)
    {
        emit generationStateChanged(false);
        return;
    }

    finishGeneration(FinishReason::Stopped);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Shuts down the FILE worker before application exit.
 * @param none
 * @return none
 * @detail Finalizes active generation, stops timers, and closes QFile in its owning
 *         thread.
 */
void FileGeneratorWorker::shutdown()
{
    if (m_active)
    {
        finishGeneration(FinishReason::Shutdown);
    }
    else
    {
        if (m_statisticsTimer != nullptr)
        {
            m_statisticsTimer->stop();
        }

        if (m_outputFile != nullptr && m_outputFile->isOpen())
        {
            m_outputFile->close();
        }
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Generates and writes the next aligned file chunk.
 * @param none
 * @return none
 * @detail Fills and writes at most 4 MiB, then returns to the event loop before the
 *         next chunk so STOP and Statistics events remain responsive.
 */
void FileGeneratorWorker::writeNextChunk()
{
    m_writeScheduled = false;

    if (!m_active || m_outputFile == nullptr || !m_outputFile->isOpen())
    {
        return;
    }

    if (m_writtenBytes >= m_targetBytes)
    {
        finishGeneration(FinishReason::Completed);
        return;
    }

    const quint64 remainingBytes = m_targetBytes - m_writtenBytes;
    quint64 chunkBytes = std::min<quint64>(remainingBytes,
                                           static_cast<quint64>(m_writeBuffer.size()));
    chunkBytes -= chunkBytes % static_cast<quint64>(m_counterBytes);

    if (chunkBytes == 0
        || chunkBytes > static_cast<quint64>(std::numeric_limits<int>::max()))
    {
        finishGeneration(FinishReason::Error,
                         tr("invalid aligned FILE write chunk size"));
        return;
    }

    const int chunkByteCount = static_cast<int>(chunkBytes);
    fillBuffer(chunkByteCount);

    QString errorText;
    if (!writeBuffer(chunkByteCount, &errorText))
    {
        finishGeneration(FinishReason::Error, errorText);
        return;
    }

    if (m_writtenBytes >= m_targetBytes)
    {
        finishGeneration(FinishReason::Completed);
        return;
    }

    if (!scheduleNextWrite())
    {
        finishGeneration(FinishReason::Error,
                         tr("failed to schedule the next file write callback"));
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Performs the one-second FILE Progress and speed update.
 * @param none
 * @return none
 * @detail Samples real interval speed and emits current written bytes and min/average/
 *         max values.
 */
void FileGeneratorWorker::updateStatistics()
{
    if (!m_active)
    {
        return;
    }

    sampleSpeed(true);
    emitProgressSnapshot();
}

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
 * @detail Rechecks width, range, alignment, multiplication, and wrapped final value.
 */
bool FileGeneratorWorker::validateRequest(const QString &filePath,
                                          int counterBits,
                                          quint64 initialValue,
                                          quint64 lastValue,
                                          quint64 valueCount,
                                          quint64 targetBytes,
                                          QString *errorText) const
{
    if (errorText == nullptr)
    {
        return false;
    }

    if (filePath.trimmed().isEmpty())
    {
        *errorText = tr("the output file path is empty");
        return false;
    }

    if (counterBits != 8 && counterBits != 16 && counterBits != 32)
    {
        *errorText = tr("counter width must be 8, 16, or 32 bits");
        return false;
    }

    const int counterBytes = counterBits / 8;
    const quint64 maximumCounterValue = counterBits == 32
                                            ? 0xFFFFFFFFULL
                                            : ((quint64(1) << counterBits) - 1U);
    const quint64 counterModulus = maximumCounterValue + 1U;

    if (initialValue > maximumCounterValue
        || lastValue > maximumCounterValue)
    {
        *errorText = tr("initial or last value exceeds the selected counter width");
        return false;
    }

    if (valueCount == 0 || targetBytes == 0)
    {
        *errorText = tr("value count and file size must be greater than zero");
        return false;
    }

    if (targetBytes > static_cast<quint64>(std::numeric_limits<qint64>::max()))
    {
        *errorText = tr("file size exceeds the maximum QFile range");
        return false;
    }

    if (targetBytes % static_cast<quint64>(counterBytes) != 0)
    {
        *errorText = tr("file size is not aligned to the counter width");
        return false;
    }

    if (valueCount
        > static_cast<quint64>(std::numeric_limits<qint64>::max())
              / static_cast<quint64>(counterBytes))
    {
        *errorText = tr("value count is too large for the output file");
        return false;
    }

    if (valueCount * static_cast<quint64>(counterBytes) != targetBytes)
    {
        *errorText = tr("value count and file size are inconsistent");
        return false;
    }

    const quint64 expectedLastValue =
        (initialValue + ((valueCount - 1U) % counterModulus))
        % counterModulus;
    if (expectedLastValue != lastValue)
    {
        *errorText = tr("last value is inconsistent with init value and value count");
        return false;
    }

    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Fills the reusable buffer with counter values.
 * @param byteCount Number of aligned bytes to fill from the beginning of the buffer.
 * @return none
 * @detail Writes little-endian 8-, 16-, or 32-bit fields and advances the counter with
 *         natural unsigned wraparound.
 */
void FileGeneratorWorker::fillBuffer(int byteCount)
{
    char *destination = m_writeBuffer.data();

    if (m_counterBits == 8)
    {
        for (int offset = 0; offset < byteCount; ++offset)
        {
            destination[offset] = static_cast<char>(m_currentCounter & 0xFFU);
            m_currentCounter = (m_currentCounter + 1U) & 0xFFU;
        }
        return;
    }

    if (m_counterBits == 16)
    {
        for (int offset = 0; offset < byteCount; offset += 2)
        {
            const quint16 value = static_cast<quint16>(m_currentCounter);
            destination[offset] = static_cast<char>(value & 0xFFU);
            destination[offset + 1] = static_cast<char>((value >> 8) & 0xFFU);
            m_currentCounter = (m_currentCounter + 1U) & 0xFFFFU;
        }
        return;
    }

    for (int offset = 0; offset < byteCount; offset += 4)
    {
        const quint32 value = static_cast<quint32>(m_currentCounter);
        destination[offset] = static_cast<char>(value & 0xFFU);
        destination[offset + 1] = static_cast<char>((value >> 8) & 0xFFU);
        destination[offset + 2] = static_cast<char>((value >> 16) & 0xFFU);
        destination[offset + 3] = static_cast<char>((value >> 24) & 0xFFU);
        m_currentCounter = (m_currentCounter + 1U) & 0xFFFFFFFFULL;
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Writes a prepared buffer region completely.
 * @param byteCount Number of bytes to write from the reusable buffer.
 * @param errorText Output fixed English error description when writing fails.
 * @return true when every requested byte was accepted by QFile; otherwise false.
 * @detail Repeats QFile::write() after partial writes and tracks the actual byte count.
 */
bool FileGeneratorWorker::writeBuffer(int byteCount, QString *errorText)
{
    if (errorText == nullptr
        || m_outputFile == nullptr
        || !m_outputFile->isOpen())
    {
        return false;
    }

    qint64 offset = 0;
    while (offset < byteCount)
    {
        const qint64 bytesWritten = m_outputFile->write(
            m_writeBuffer.constData() + offset,
            static_cast<qint64>(byteCount) - offset);
        if (bytesWritten <= 0)
        {
            *errorText = tr("write failed for %1: %2 (code %3)")
                             .arg(QDir::toNativeSeparators(m_filePath),
                                  currentFileErrorText())
                             .arg(static_cast<int>(m_outputFile->error()));
            return false;
        }

        offset += bytesWritten;
        m_writtenBytes += static_cast<quint64>(bytesWritten);
    }

    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Schedules the next cooperative write callback.
 * @param none
 * @return true when QMetaObject accepted the queued invocation.
 * @detail Prevents duplicate callbacks while allowing STOP and timer events between
 *         chunks.
 */
bool FileGeneratorWorker::scheduleNextWrite()
{
    if (!m_active || m_writeScheduled)
    {
        return m_active;
    }

    m_writeScheduled = true;
    const bool invoked = QMetaObject::invokeMethod(
        this,
        "writeNextChunk",
        Qt::QueuedConnection);
    if (!invoked)
    {
        m_writeScheduled = false;
    }

    return invoked;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Adds one interval-speed sample.
 * @param includeZero true to include a zero-byte timer interval in minimum speed.
 * @return none
 * @detail Uses actual monotonic milliseconds between samples rather than assuming an
 *         exact one-second timer period.
 */
void FileGeneratorWorker::sampleSpeed(bool includeZero)
{
    if (!m_elapsedTimer.isValid())
    {
        return;
    }

    const qint64 elapsedMilliseconds = m_elapsedTimer.elapsed();
    const qint64 intervalMilliseconds =
        elapsedMilliseconds - m_lastStatisticsElapsedMs;
    if (intervalMilliseconds <= 0)
    {
        return;
    }

    const quint64 intervalBytes = m_writtenBytes - m_lastStatisticsBytes;
    if (intervalBytes > 0 || includeZero)
    {
        const double speedMBps =
            (static_cast<double>(intervalBytes) * 1000.0)
            / static_cast<double>(intervalMilliseconds)
            / kBytesPerMegabyte;
        m_minimumSpeedMBps = std::min(m_minimumSpeedMBps, speedMBps);
        m_maximumSpeedMBps = std::max(m_maximumSpeedMBps, speedMBps);
        ++m_speedSampleCount;
    }

    m_lastStatisticsBytes = m_writtenBytes;
    m_lastStatisticsElapsedMs = elapsedMilliseconds;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Emits the current Progress snapshot.
 * @param none
 * @return none
 * @detail Combines current byte progress with sampled min/max and total average speed.
 */
void FileGeneratorWorker::emitProgressSnapshot()
{
    emit progressUpdated(m_writtenBytes,
                         m_targetBytes,
                         minimumSpeedMBps(),
                         averageSpeedMBps(),
                         maximumSpeedMBps());
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Finalizes generation and closes the output file.
 * @param reason Completion, STOP, shutdown, or error reason.
 * @param errorText Optional fixed English failure detail.
 * @return none
 * @detail Samples final speed, truncates a failed partial counter field, flushes and
 *         closes QFile, emits final Progress and EVENTS information, and reports idle.
 */
void FileGeneratorWorker::finishGeneration(FinishReason reason,
                                           const QString &errorText)
{
    if (!m_active)
    {
        return;
    }

    m_active = false;
    m_writeScheduled = false;
    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }

    if (reason == FinishReason::Error && m_counterBytes > 0)
    {
        const quint64 alignedBytes =
            m_writtenBytes - (m_writtenBytes % static_cast<quint64>(m_counterBytes));
        if (m_outputFile != nullptr
            && m_outputFile->isOpen()
            && m_outputFile->size() != static_cast<qint64>(alignedBytes))
        {
            m_outputFile->resize(static_cast<qint64>(alignedBytes));
        }
        m_writtenBytes = alignedBytes;
    }

    sampleSpeed(false);

    bool flushOk = true;
    QString flushErrorText;
    if (m_outputFile != nullptr && m_outputFile->isOpen())
    {
        flushOk = m_outputFile->flush();
        if (!flushOk)
        {
            flushErrorText = tr("flush failed for %1: %2 (code %3)")
                                 .arg(QDir::toNativeSeparators(m_filePath),
                                      currentFileErrorText())
                                 .arg(static_cast<int>(m_outputFile->error()));
        }
        m_outputFile->close();
    }

    if (!flushOk && reason != FinishReason::Error)
    {
        reason = FinishReason::Error;
    }

    emitProgressSnapshot();

    const QString speedSummary =
        tr("min speed=%1; avrg speed=%2; max speed=%3")
            .arg(formatSpeed(minimumSpeedMBps()),
                 formatSpeed(averageSpeedMBps()),
                 formatSpeed(maximumSpeedMBps()));
    const QString nativePath = QDir::toNativeSeparators(m_filePath);

    if (reason == FinishReason::Completed)
    {
        emitWorkerEvent(
            tr("FINISH file generation: file=%1; written=%2 B; %3")
                .arg(nativePath)
                .arg(m_writtenBytes)
                .arg(speedSummary),
            false);
    }
    else if (reason == FinishReason::Stopped)
    {
        emitWorkerEvent(
            tr("STOP file generation: file=%1; written=%2 / %3 B; %4")
                .arg(nativePath)
                .arg(m_writtenBytes)
                .arg(m_targetBytes)
                .arg(speedSummary),
            false);
    }
    else if (reason == FinishReason::Shutdown)
    {
        emitWorkerEvent(
            tr("file generation stopped because the application is closing: "
               "file=%1; written=%2 / %3 B; %4")
                .arg(nativePath)
                .arg(m_writtenBytes)
                .arg(m_targetBytes)
                .arg(speedSummary),
            false);
    }
    else
    {
        QString combinedError = errorText.trimmed();
        if (!flushErrorText.isEmpty())
        {
            if (!combinedError.isEmpty())
            {
                combinedError += QStringLiteral("; ");
            }
            combinedError += flushErrorText;
        }
        if (combinedError.isEmpty())
        {
            combinedError = tr("unspecified file generation error");
        }

        emitWorkerEvent(
            tr("FILE generation error: %1; file=%2; written=%3 B")
                .arg(combinedError, nativePath)
                .arg(m_writtenBytes),
            true);
    }

    emit generationStateChanged(false);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns the average write speed.
 * @param none
 * @return Average speed in MB/s based on all written bytes and elapsed time.
 * @detail Uses a one-millisecond minimum denominator for sub-millisecond completion.
 */
double FileGeneratorWorker::averageSpeedMBps() const
{
    if (!m_elapsedTimer.isValid() || m_writtenBytes == 0)
    {
        return 0.0;
    }

    const qint64 elapsedMilliseconds = qMax<qint64>(1, m_elapsedTimer.elapsed());
    return (static_cast<double>(m_writtenBytes) * 1000.0)
           / static_cast<double>(elapsedMilliseconds)
           / kBytesPerMegabyte;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns the effective minimum sampled speed.
 * @param none
 * @return Minimum sampled speed, or average speed when no interval sample exists.
 * @detail Supports meaningful final Statistics for files completed within one second.
 */
double FileGeneratorWorker::minimumSpeedMBps() const
{
    return m_speedSampleCount == 0 ? averageSpeedMBps()
                                   : m_minimumSpeedMBps;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns the effective maximum sampled speed.
 * @param none
 * @return Maximum sampled speed, or average speed when no interval sample exists.
 * @detail Supports meaningful final Statistics for files completed within one second.
 */
double FileGeneratorWorker::maximumSpeedMBps() const
{
    return m_speedSampleCount == 0 ? averageSpeedMBps()
                                   : m_maximumSpeedMBps;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Formats a FILE speed for EVENTS.
 * @param speedMBps Speed in MB/s.
 * @return English numeric text with at most three fractional digits and MB/s suffix.
 * @detail Removes insignificant trailing zeros and maps invalid values to zero.
 */
QString FileGeneratorWorker::formatSpeed(double speedMBps) const
{
    if (!(speedMBps > 0.0) || !std::isfinite(speedMBps))
    {
        return QStringLiteral("0 MB/s");
    }

    QString result = QString::number(speedMBps, 'f', 3);
    while (result.endsWith(QLatin1Char('0')))
    {
        result.chop(1);
    }
    if (result.endsWith(QLatin1Char('.')))
    {
        result.chop(1);
    }

    return result + QStringLiteral(" MB/s");
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Emits a timestamped worker event.
 * @param text Event text without a timestamp.
 * @param error true for a red error event; otherwise false.
 * @return none
 * @detail Captures the event time in the FILE worker thread.
 */
void FileGeneratorWorker::emitWorkerEvent(const QString &text, bool error)
{
    emit eventGenerated(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
        text,
        error);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns a fixed English description for the current QFile error.
 * @param none
 * @return Locale-independent file error description.
 * @detail Uses the worker-owned QFile error code and avoids localized errorString().
 */
QString FileGeneratorWorker::currentFileErrorText() const
{
    if (m_outputFile == nullptr)
    {
        return QStringLiteral("file object is unavailable");
    }

    return fileErrorText(m_outputFile->error());
}
