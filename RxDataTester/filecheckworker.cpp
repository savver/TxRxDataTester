#include "filecheckworker.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QTimer>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

namespace
{
constexpr int kStatisticsIntervalMs = 1000;
constexpr qint64 kReadBufferBytes = 4LL * 1024LL * 1024LL;
constexpr quint64 kMaximumDetailedErrorEvents = 10000;

/**
 * @brief Removes insignificant zeros from a fixed-point decimal string.
 * @param text Fixed-point decimal string to normalize.
 * @return Compact decimal text without redundant fractional zeros.
 * @detail Preserves at least one digit and removes the decimal point when the fractional
 *         part becomes empty.
 */
QString trimFixedPoint(QString text)
{
    if (text.contains(QLatin1Char('.')))
    {
        while (text.endsWith(QLatin1Char('0')))
        {
            text.chop(1);
        }
        if (text.endsWith(QLatin1Char('.')))
        {
            text.chop(1);
        }
    }

    return text.isEmpty() ? QStringLiteral("0") : text;
}
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates the file-check worker object.
 * @param parent Parent QObject; normally omitted before moveToThread().
 * @return none
 * @detail Initializes scalar state only. QFile, QTimer objects, and the persistent read
 *         buffer are created later by initialize() in the worker thread.
 */
FileCheckWorker::FileCheckWorker(QObject *parent)
    : QObject(parent)
    , m_file(nullptr)
    , m_processTimer(nullptr)
    , m_statisticsTimer(nullptr)
    , m_counterBits(8)
    , m_counterBytes(1)
    , m_carryBytes(0)
    , m_counterMask(0xFFU)
    , m_initialValue(0)
    , m_expectedValue(0)
    , m_lastReceivedValue(0)
    , m_totalCompleteValues(0)
    , m_processedValues(0)
    , m_counterOk(0)
    , m_counterErrors(0)
    , m_minimumSkipped(0)
    , m_maximumSkipped(0)
    , m_chunkSampleCount(0)
    , m_skippedSum(0.0L)
    , m_detailedErrorsLogged(0)
    , m_suppressedErrorEvents(0)
    , m_totalFileBytes(0)
    , m_fileBytesRead(0)
    , m_streamOffset(0)
    , m_trailingBytes(0)
    , m_hexadecimalDisplay(false)
    , m_initialized(false)
    , m_checkRunning(false)
    , m_stopRequested(false)
    , m_shuttingDown(false)
{
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Destroys the file-check worker object.
 * @param none
 * @return none
 * @detail Stops timers and closes the selected file as a safeguard. Normal shutdown
 *         performs these actions earlier through shutdown().
 */
FileCheckWorker::~FileCheckWorker()
{
    if (m_processTimer != nullptr)
    {
        m_processTimer->stop();
    }

    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }

    if (m_file != nullptr && m_file->isOpen())
    {
        m_file->close();
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Initializes resources owned by the FILE worker thread.
 * @param none
 * @return none
 * @detail Creates QFile and timers, allocates one persistent read buffer, connects
 *         internal callbacks, and reports readiness to the GUI.
 */
void FileCheckWorker::initialize()
{
    if (m_initialized)
    {
        emit workerReady();
        return;
    }

    m_file = new QFile(this);
    m_processTimer = new QTimer(this);
    m_statisticsTimer = new QTimer(this);

    m_readBuffer.resize(static_cast<int>(kReadBufferBytes + 4));

    m_processTimer->setSingleShot(true);
    m_processTimer->setTimerType(Qt::PreciseTimer);

    m_statisticsTimer->setInterval(kStatisticsIntervalMs);
    m_statisticsTimer->setSingleShot(false);
    m_statisticsTimer->setTimerType(Qt::PreciseTimer);

    connect(m_processTimer,
            &QTimer::timeout,
            this,
            &FileCheckWorker::processNextBlock);
    connect(m_statisticsTimer,
            &QTimer::timeout,
            this,
            &FileCheckWorker::updateStatistics);

    m_initialized = true;
    emit checkingStateChanged(false);
    emit workerReady();
}

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
void FileCheckWorker::startCheck(const QString &filePath,
                                 int counterBits,
                                 quint64 initialValue,
                                 bool hexadecimalDisplay,
                                 const QString &patternDescription)
{
    if (!m_initialized || m_file == nullptr || m_processTimer == nullptr
        || m_statisticsTimer == nullptr)
    {
        emitWorkerEvent(tr("FILE check error: the worker thread is not initialized"),
                        true);
        emit checkingStateChanged(false);
        return;
    }

    if (m_checkRunning)
    {
        emitWorkerEvent(tr("FILE check error: another file check is already active"),
                        true);
        emit checkingStateChanged(true);
        return;
    }

    if (counterBits != 8 && counterBits != 16 && counterBits != 32)
    {
        emitWorkerEvent(tr("FILE check error: unsupported counter width %1 bits")
                            .arg(counterBits),
                        true);
        emit checkingStateChanged(false);
        return;
    }

    const int counterBytes = counterBits / 8;
    const quint64 counterMask = counterBits == 32
                                    ? 0xFFFFFFFFULL
                                    : ((quint64(1) << counterBits) - quint64(1));
    if (initialValue > counterMask)
    {
        emitWorkerEvent(tr("FILE check error: init value is outside the selected "
                           "counter range"),
                        true);
        emit checkingStateChanged(false);
        return;
    }

    if (m_file->isOpen())
    {
        m_file->close();
    }

    m_file->setFileName(filePath);
    if (!m_file->open(QIODevice::ReadOnly))
    {
        emitWorkerEvent(
            tr("FILE open error: file=%1; error=%2 (code %3)")
                .arg(QDir::toNativeSeparators(filePath),
                     fileErrorText(static_cast<int>(m_file->error())))
                .arg(static_cast<int>(m_file->error())),
            true);
        emit checkingStateChanged(false);
        return;
    }

    resetCheckState();
    m_filePath = filePath;
    m_patternDescription = patternDescription;
    m_counterBits = counterBits;
    m_counterBytes = counterBytes;
    m_counterMask = counterMask;
    m_initialValue = initialValue;
    m_expectedValue = initialValue;
    m_lastReceivedValue = initialValue;
    m_hexadecimalDisplay = hexadecimalDisplay;
    m_totalFileBytes = m_file->size();
    m_totalCompleteValues = static_cast<quint64>(m_totalFileBytes / m_counterBytes);
    m_trailingBytes = m_totalFileBytes % m_counterBytes;
    m_startTime = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    m_elapsedTimer.start();
    m_checkRunning = true;

    emit checkingStateChanged(true);
    emitStatisticsSnapshot();
    emitWorkerEvent(tr("START file check: %1").arg(patternDescription), false);

    if (m_totalFileBytes == 0)
    {
        finishCheck(true, false);
        return;
    }

    m_statisticsTimer->start();
    m_processTimer->start(0);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Stops an active file check at a complete counter boundary.
 * @param none
 * @return none
 * @detail Closes the file after the current worker callback, emits final partial
 *         Statistics, and leaves no truncated counter state in memory.
 */
void FileCheckWorker::stopCheck()
{
    if (!m_checkRunning)
    {
        emit checkingStateChanged(false);
        return;
    }

    m_stopRequested = true;
    finishCheck(false, true);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Shuts down the FILE worker before application exit.
 * @param none
 * @return none
 * @detail Stops an active check, closes QFile and timers, and leaves the object ready
 *         for the main thread to stop its QThread.
 */
void FileCheckWorker::shutdown()
{
    if (m_shuttingDown)
    {
        return;
    }

    m_shuttingDown = true;
    if (m_checkRunning)
    {
        emitWorkerEvent(tr("FILE check is stopping because the application is closing"),
                        false);
        finishCheck(false, false);
    }

    if (m_processTimer != nullptr)
    {
        m_processTimer->stop();
    }
    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }
    if (m_file != nullptr && m_file->isOpen())
    {
        m_file->close();
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Reads and verifies the next file block.
 * @param none
 * @return none
 * @detail Uses one persistent 4 MiB buffer, preserves up to three carry bytes between
 *         short reads, verifies complete counters, and reschedules itself through the
 *         worker event loop until EOF or STOP.
 */
void FileCheckWorker::processNextBlock()
{
    if (!m_checkRunning || m_file == nullptr || !m_file->isOpen())
    {
        return;
    }

    if (m_stopRequested)
    {
        finishCheck(false, true);
        return;
    }

    const qint64 remainingBytes = m_totalFileBytes - m_fileBytesRead;
    if (remainingBytes <= 0)
    {
        finishCheck(true, false);
        return;
    }

    const qint64 readCapacity = qMin(kReadBufferBytes, remainingBytes);
    const qint64 bytesRead = m_file->read(m_readBuffer.data() + m_carryBytes,
                                          readCapacity);
    if (bytesRead < 0)
    {
        finishCheck(false,
                    false,
                    tr("read error: %1 (code %2)")
                        .arg(fileErrorText(static_cast<int>(m_file->error())))
                        .arg(static_cast<int>(m_file->error())));
        return;
    }

    if (bytesRead == 0)
    {
        finishCheck(true, false);
        return;
    }

    m_fileBytesRead += bytesRead;
    const qint64 availableBytes = qint64(m_carryBytes) + bytesRead;
    const qint64 completeBytes =
        (availableBytes / m_counterBytes) * m_counterBytes;

    if (completeBytes > 0)
    {
        verifyBuffer(m_readBuffer.constData(), completeBytes, m_streamOffset);
        m_streamOffset += completeBytes;
    }

    const int trailingBytes = static_cast<int>(availableBytes - completeBytes);
    if (trailingBytes > 0)
    {
        std::memmove(m_readBuffer.data(),
                     m_readBuffer.constData() + completeBytes,
                     static_cast<std::size_t>(trailingBytes));
    }
    m_carryBytes = trailingBytes;

    if (m_fileBytesRead >= m_totalFileBytes)
    {
        finishCheck(true, false);
        return;
    }

    m_processTimer->start(0);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Performs the one-second FILE Statistics update.
 * @param none
 * @return none
 * @detail Sends current OK, ERR, and Chunks values to the GUI without reading or
 *         modifying the selected file.
 */
void FileCheckWorker::updateStatistics()
{
    if (!m_checkRunning)
    {
        return;
    }

    emitStatisticsSnapshot();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Resets all per-check counters and timing state.
 * @param none
 * @return none
 * @detail Preserves initialized worker resources while clearing the previous result.
 */
void FileCheckWorker::resetCheckState()
{
    if (m_processTimer != nullptr)
    {
        m_processTimer->stop();
    }
    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }

    m_filePath.clear();
    m_patternDescription.clear();
    m_startTime.clear();
    m_carryBytes = 0;
    m_initialValue = 0;
    m_expectedValue = 0;
    m_lastReceivedValue = 0;
    m_totalCompleteValues = 0;
    m_processedValues = 0;
    m_counterOk = 0;
    m_counterErrors = 0;
    m_minimumSkipped = 0;
    m_maximumSkipped = 0;
    m_chunkSampleCount = 0;
    m_skippedSum = 0.0L;
    m_detailedErrorsLogged = 0;
    m_suppressedErrorEvents = 0;
    m_totalFileBytes = 0;
    m_fileBytesRead = 0;
    m_streamOffset = 0;
    m_trailingBytes = 0;
    m_hexadecimalDisplay = false;
    m_checkRunning = false;
    m_stopRequested = false;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Verifies complete counters stored in the read buffer.
 * @param data Pointer to the first byte to inspect.
 * @param byteCount Number of complete-counter bytes to inspect.
 * @param fileOffset Byte offset of data from the beginning of the file.
 * @return none
 * @detail Decodes little-endian 8-, 16-, or 32-bit values, resynchronizes after each
 *         mismatch, updates Chunks Statistics, and emits detailed errors.
 */
void FileCheckWorker::verifyBuffer(const char *data,
                                   qint64 byteCount,
                                   qint64 fileOffset)
{
    const unsigned char *bytes =
        reinterpret_cast<const unsigned char *>(data);
    const qint64 valueCount = byteCount / m_counterBytes;

    for (qint64 index = 0; index < valueCount; ++index)
    {
        const qint64 byteIndex = index * m_counterBytes;
        quint64 receivedValue = 0;
        for (int byte = 0; byte < m_counterBytes; ++byte)
        {
            receivedValue |= quint64(bytes[byteIndex + byte]) << (8 * byte);
        }
        receivedValue &= m_counterMask;

        const quint64 expectedValue = m_expectedValue;
        if (receivedValue == expectedValue)
        {
            ++m_counterOk;
        }
        else
        {
            ++m_counterErrors;
            const quint64 skippedValues =
                (receivedValue - m_lastReceivedValue) & m_counterMask;

            if (m_chunkSampleCount == 0)
            {
                m_minimumSkipped = skippedValues;
                m_maximumSkipped = skippedValues;
            }
            else
            {
                m_minimumSkipped = std::min(m_minimumSkipped, skippedValues);
                m_maximumSkipped = std::max(m_maximumSkipped, skippedValues);
            }
            ++m_chunkSampleCount;
            m_skippedSum += static_cast<long double>(skippedValues);

            const quint64 nextExpected = (receivedValue + quint64(1)) & m_counterMask;
            if (m_detailedErrorsLogged < kMaximumDetailedErrorEvents)
            {
                const quint64 absoluteOffset =
                    static_cast<quint64>(fileOffset + byteIndex);
                emitWorkerEvent(
                    tr("FILE counter error: offset=%1 B; expected=%2; received=%3; "
                       "skipped=%4; next_expected=%5")
                        .arg(QString::number(absoluteOffset),
                             formatCounterValue(expectedValue),
                             formatCounterValue(receivedValue),
                             QString::number(skippedValues),
                             formatCounterValue(nextExpected)),
                    true);
                ++m_detailedErrorsLogged;
            }
            else
            {
                ++m_suppressedErrorEvents;
                if (m_suppressedErrorEvents == 1)
                {
                    emitWorkerEvent(
                        tr("WARNING: detailed FILE counter-error logging is limited "
                           "to %1 entries; later errors remain included in Statistics")
                            .arg(kMaximumDetailedErrorEvents),
                        false);
                }
            }
        }

        m_lastReceivedValue = receivedValue;
        m_expectedValue = (receivedValue + quint64(1)) & m_counterMask;
        ++m_processedValues;
    }
}

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
void FileCheckWorker::finishCheck(bool completed,
                                  bool stoppedByUser,
                                  const QString &failureText)
{
    if (!m_checkRunning)
    {
        return;
    }

    if (m_processTimer != nullptr)
    {
        m_processTimer->stop();
    }
    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }

    emitStatisticsSnapshot();

    if (m_file != nullptr && m_file->isOpen())
    {
        m_file->close();
    }

    const quint64 checkedValues = m_counterOk + m_counterErrors;
    const QString okPercent = formatPercentage(m_counterOk, checkedValues);
    const QString errorPercent = formatPercentage(m_counterErrors, checkedValues);
    const double averageSkipped = m_chunkSampleCount == 0
                                      ? 0.0
                                      : static_cast<double>(
                                            m_skippedSum
                                            / static_cast<long double>(
                                                m_chunkSampleCount));
    const QString averageSkippedText =
        trimFixedPoint(QString::number(averageSkipped, 'f', 6));
    const QString chunkText = m_chunkSampleCount == 0
                                  ? QStringLiteral("min=0; avrg=0; max=0")
                                  : tr("min=%1; avrg=%2; max=%3")
                                        .arg(QString::number(m_minimumSkipped),
                                             averageSkippedText,
                                             QString::number(m_maximumSkipped));
    const qint64 elapsedMs = m_elapsedTimer.isValid() ? m_elapsedTimer.elapsed() : 0;

    QString prefix;
    if (!failureText.isEmpty())
    {
        prefix = tr("FILE check failed");
    }
    else if (stoppedByUser)
    {
        prefix = tr("STOP file check");
    }
    else if (completed)
    {
        prefix = tr("FINISH file check");
    }
    else
    {
        prefix = tr("FILE check stopped");
    }

    QString resultText =
        tr("%1: file=%2; checked_values=%3/%4; OK=%5 (%6%); ERR=%7 (%8%); "
           "chunks %9; trailing_bytes=%10; elapsed_ms=%11; "
           "detailed_errors=%12; suppressed_errors=%13")
            .arg(prefix)
            .arg(QDir::toNativeSeparators(m_filePath))
            .arg(QString::number(checkedValues))
            .arg(QString::number(m_totalCompleteValues))
            .arg(QString::number(m_counterOk))
            .arg(okPercent)
            .arg(QString::number(m_counterErrors))
            .arg(errorPercent)
            .arg(chunkText)
            .arg(QString::number(m_trailingBytes))
            .arg(QString::number(elapsedMs))
            .arg(QString::number(m_detailedErrorsLogged))
            .arg(QString::number(m_suppressedErrorEvents));

    if (!failureText.isEmpty())
    {
        resultText += tr("; reason=%1").arg(failureText);
    }

    emitWorkerEvent(resultText, !failureText.isEmpty());

    m_checkRunning = false;
    m_stopRequested = false;
    emit checkingStateChanged(false);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Emits the current FILE Statistics snapshot.
 * @param none
 * @return none
 * @detail Calculates the average counter-jump chunk only when mismatch samples exist.
 */
void FileCheckWorker::emitStatisticsSnapshot()
{
    const double averageSkipped = m_chunkSampleCount == 0
                                      ? 0.0
                                      : static_cast<double>(
                                            m_skippedSum
                                            / static_cast<long double>(
                                                m_chunkSampleCount));
    emit statisticsUpdated(m_processedValues,
                           m_totalCompleteValues,
                           m_counterOk,
                           m_counterErrors,
                           m_minimumSkipped,
                           averageSkipped,
                           m_maximumSkipped,
                           m_chunkSampleCount > 0);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Sends a worker event with the current millisecond timestamp.
 * @param text Event text without a timestamp.
 * @param error true for a red error entry; otherwise false.
 * @return none
 * @detail Keeps event time generation in the FILE worker thread.
 */
void FileCheckWorker::emitWorkerEvent(const QString &text, bool error)
{
    emit eventGenerated(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
        text,
        error);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Formats a counter value for FILE events.
 * @param value Unsigned counter value to format.
 * @return Decimal text or uppercase 0x-prefixed hexadecimal text.
 * @detail Uses the same display base as the init value selected in the GUI.
 */
QString FileCheckWorker::formatCounterValue(quint64 value) const
{
    if (m_hexadecimalDisplay)
    {
        return QStringLiteral("0x%1")
            .arg(QString::number(value & m_counterMask, 16).toUpper());
    }

    return QString::number(value & m_counterMask);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Formats a percentage for FILE result events.
 * @param part Numerator value.
 * @param total Denominator value.
 * @return Percentage text with insignificant trailing zeros removed.
 * @detail Returns 0 when total is zero.
 */
QString FileCheckWorker::formatPercentage(quint64 part, quint64 total) const
{
    if (total == 0)
    {
        return QStringLiteral("0");
    }

    const double percentage =
        (static_cast<double>(part) * 100.0) / static_cast<double>(total);
    return trimFixedPoint(QString::number(percentage, 'f', 6));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns a fixed English description for a QFile error.
 * @param error QFileDevice error value to describe.
 * @return English text independent of the operating-system language.
 * @detail Covers every QFileDevice::FileError available in Qt 5.12.
 */
QString FileCheckWorker::fileErrorText(int error) const
{
    switch (static_cast<QFileDevice::FileError>(error))
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
