#include "udprxworker.h"

#include <QByteArray>
#include <QDateTime>
#include <QIODevice>
#include <QMetaObject>
#include <QTime>
#include <QTimer>
#include <QUdpSocket>
#include <QVariant>

#include <limits>

namespace
{
constexpr int kStatisticsIntervalMs = 1000;
constexpr qint64 kPeriodicLogIntervalMs = 20 * 1000;
constexpr int kMaximumDatagramsPerReadBatch = 256;
constexpr int kMaximumDatagramsToDiscardBeforeStart = 4096;
constexpr qint64 kDiscardBeforeStartTimeBudgetMs = 25;
constexpr int kRequestedReceiveBufferBytes = 4 * 1024 * 1024;
constexpr int kMaximumIpv4UdpPayloadBytes = 65507;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates the UDP receiver worker object.
 * @param parent Parent QObject; normally omitted before moveToThread().
 * @return none
 * @detail Initializes scalar state only. QUdpSocket and QTimer are created later by
 *         initialize() after the object is moved to its worker thread.
 */
UdpRxWorker::UdpRxWorker(QObject *parent)
    : QObject(parent)
    , m_udpSocket(nullptr)
    , m_statisticsTimer(nullptr)
    , m_listenPort(0)
    , m_expectedCounter(0)
    , m_lastReceivedCounter(0)
    , m_totalPayloadBytes(0)
    , m_totalPackets(0)
    , m_lastStatisticsBytes(0)
    , m_lastStatisticsPackets(0)
    , m_lastStatisticsElapsedMs(0)
    , m_counterOk(0)
    , m_counterErrors(0)
    , m_lastPeriodicLogBytes(0)
    , m_lastPeriodicCounterOk(0)
    , m_lastPeriodicCounterErrors(0)
    , m_lastPeriodicLogElapsedMs(0)
    , m_periodicMinimumSpeedKbps(0.0)
    , m_periodicMaximumSpeedKbps(0.0)
    , m_periodicSpeedSumKbps(0.0)
    , m_periodicMinimumPacketsPerSecond(0.0)
    , m_periodicMaximumPacketsPerSecond(0.0)
    , m_periodicPacketsPerSecondSum(0.0)
    , m_periodicSampleCount(0)
    , m_initialized(false)
    , m_connectionConfigured(false)
    , m_testRunning(false)
    , m_readContinuationScheduled(false)
    , m_socketOperationInProgress(false)
    , m_handlingSocketFailure(false)
    , m_shuttingDown(false)
{
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Destroys the UDP receiver worker object.
 * @param none
 * @return none
 * @detail Stops the timer and closes the UDP socket as a safeguard after normal
 *         shutdown.
 */
UdpRxWorker::~UdpRxWorker()
{
    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }

    if (m_udpSocket != nullptr
        && m_udpSocket->state() != QAbstractSocket::UnconnectedState)
    {
        m_udpSocket->close();
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Initializes resources owned by the UDP RX worker thread.
 * @param none
 * @return none
 * @detail Creates QUdpSocket and the Statistics timer, connects their signals, and
 *         reports readiness to the GUI.
 */
void UdpRxWorker::initialize()
{
    if (m_initialized)
    {
        emit workerReady();
        return;
    }

    m_udpSocket = new QUdpSocket(this);
    m_statisticsTimer = new QTimer(this);

    m_statisticsTimer->setInterval(kStatisticsIntervalMs);
    m_statisticsTimer->setSingleShot(false);
    m_statisticsTimer->setTimerType(Qt::PreciseTimer);

    connect(m_udpSocket,
            &QUdpSocket::readyRead,
            this,
            &UdpRxWorker::handleReadyRead,
            Qt::DirectConnection);
    connect(m_udpSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QUdpSocket::error),
            this,
            &UdpRxWorker::handleSocketError);
    connect(m_udpSocket,
            &QUdpSocket::stateChanged,
            this,
            &UdpRxWorker::handleSocketStateChanged);
    connect(m_statisticsTimer,
            &QTimer::timeout,
            this,
            &UdpRxWorker::updateStatistics);

    m_initialized = true;
    emit receptionStateChanged(false);
    emit connectionStateChanged(false, QString(), 0, QString(), false);
    emit workerReady();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates and binds the persistent UDP receiver socket.
 * @param expectedSourceIp IPv4 address of the transmitter selected in Connection.
 * @param listenPort Local UDP port on which datagrams are expected.
 * @param localIp Local IPv4 address selected by the operating-system route.
 * @return none
 * @detail Reserves localIp:listenPort exclusively and ignores datagrams received from
 *         other source IP addresses during an active test.
 */
void UdpRxWorker::configureConnection(const QString &expectedSourceIp,
                                      quint16 listenPort,
                                      const QString &localIp)
{
    if (!m_initialized || m_udpSocket == nullptr)
    {
        emitWorkerEvent(tr("UDP Connection error: the UDP RX worker is not initialized"),
                        true);
        emit connectionStateChanged(false,
                                    expectedSourceIp,
                                    listenPort,
                                    localIp,
                                    true);
        return;
    }

    if (m_testRunning)
    {
        emitWorkerEvent(tr("UDP Connection error: reception is active"), true);
        emit connectionStateChanged(m_connectionConfigured,
                                    m_expectedSourceIp,
                                    m_listenPort,
                                    m_localIp,
                                    false);
        return;
    }

    if (m_connectionConfigured)
    {
        emitWorkerEvent(tr("UDP Connection error: the receiver socket is already connected"),
                        true);
        emit connectionStateChanged(true,
                                    m_expectedSourceIp,
                                    m_listenPort,
                                    m_localIp,
                                    false);
        return;
    }

    QHostAddress sourceAddress;
    QHostAddress localAddress;
    if (!sourceAddress.setAddress(expectedSourceIp)
        || sourceAddress.protocol() != QAbstractSocket::IPv4Protocol
        || !localAddress.setAddress(localIp)
        || localAddress.protocol() != QAbstractSocket::IPv4Protocol
        || listenPort == 0)
    {
        emitWorkerEvent(tr("UDP Connection error: invalid IPv4 endpoint data"), true);
        emit connectionStateChanged(false,
                                    expectedSourceIp,
                                    listenPort,
                                    localIp,
                                    true);
        return;
    }

    if (m_udpSocket->state() != QAbstractSocket::UnconnectedState)
    {
        m_socketOperationInProgress = true;
        m_udpSocket->close();
        m_socketOperationInProgress = false;
    }

    m_socketOperationInProgress = true;
    const bool bound = m_udpSocket->bind(localAddress,
                                        listenPort,
                                        QAbstractSocket::DontShareAddress);
    m_socketOperationInProgress = false;

    if (!bound)
    {
        const int errorValue = static_cast<int>(m_udpSocket->error());
        emitWorkerEvent(tr("UDP Connection error: failed to bind receiver socket %1:%2: %3 (code %4)")
                            .arg(localIp)
                            .arg(listenPort)
                            .arg(socketErrorText(errorValue))
                            .arg(errorValue),
                        true);
        m_udpSocket->close();
        emit connectionStateChanged(false,
                                    expectedSourceIp,
                                    listenPort,
                                    localIp,
                                    true);
        return;
    }

    m_socketOperationInProgress = true;
    m_udpSocket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,
                                 kRequestedReceiveBufferBytes);
    m_socketOperationInProgress = false;

    m_expectedSourceAddress = sourceAddress;
    m_expectedSourceIp = sourceAddress.toString();
    m_localIp = localAddress.toString();
    m_listenPort = m_udpSocket->localPort();
    m_connectionConfigured = true;
    m_reportedUnexpectedSources.clear();

    emit connectionStateChanged(true,
                                m_expectedSourceIp,
                                m_listenPort,
                                m_localIp,
                                false);
    emitWorkerEvent(tr("UDP receiver ready: local=%1:%2; expected_source=%3")
                        .arg(m_localIp)
                        .arg(m_listenPort)
                        .arg(m_expectedSourceIp),
                    false);

    if (m_udpSocket->hasPendingDatagrams() && !m_readContinuationScheduled)
    {
        m_readContinuationScheduled = true;
        QMetaObject::invokeMethod(this,
                                  "handleReadyRead",
                                  Qt::QueuedConnection);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Closes the persistent UDP receiver socket.
 * @param none
 * @return none
 * @detail Stops active reception when necessary and releases the reserved local UDP
 *         port.
 */
void UdpRxWorker::disconnectConnection()
{
    if (!m_initialized || m_udpSocket == nullptr)
    {
        emit connectionStateChanged(false,
                                    m_expectedSourceIp,
                                    m_listenPort,
                                    m_localIp,
                                    false);
        return;
    }

    if (m_testRunning)
    {
        stopReceptionInternal(
            tr("UDP reception stopped because DISCONNECT was requested"),
            false);
    }

    if (!m_connectionConfigured)
    {
        emit connectionStateChanged(false,
                                    m_expectedSourceIp,
                                    m_listenPort,
                                    m_localIp,
                                    false);
        return;
    }

    closeSocket(false, true);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Starts UDP reception and sequential-counter verification.
 * @param counterBits Counter width: 8, 16, 32, or 64 bits.
 * @param blockBytes Expected transmitter payload size in bytes.
 * @param togetherCount Informational number of packets in one transmitter burst.
 * @param periodMs Informational transmitter burst period in milliseconds.
 * @param initialValue First expected counter value.
 * @param patternDescription Preformatted Pattern description for the event log.
 * @return none
 * @detail Discards datagrams queued before START and verifies each accepted datagram
 *         independently while preserving counter continuity between datagrams.
 */
void UdpRxWorker::startReception(int counterBits,
                                 int blockBytes,
                                 int togetherCount,
                                 int periodMs,
                                 quint64 initialValue,
                                 const QString &patternDescription)
{
    if (!m_initialized
        || m_udpSocket == nullptr
        || !m_connectionConfigured
        || m_udpSocket->state() != QAbstractSocket::BoundState)
    {
        emitWorkerEvent(tr("UDP START failed: the receiver socket is not connected"),
                        true);
        emitReceptionState();
        return;
    }

    if (m_testRunning)
    {
        emitWorkerEvent(tr("UDP START failed: reception is already active"), true);
        emitReceptionState();
        return;
    }

    PatternSettings settings;
    QString errorText;
    if (!makePatternSettings(counterBits,
                             blockBytes,
                             togetherCount,
                             periodMs,
                             initialValue,
                             &settings,
                             &errorText))
    {
        emitWorkerEvent(tr("UDP Pattern error: %1").arg(errorText), true);
        emitReceptionState();
        return;
    }

    discardPendingDatagrams();
    resetStatistics(settings);
    m_testRunning = true;
    m_statisticsTimer->start();
    emitReceptionState();
    emitWorkerEvent(tr("UDP START: reception and verification started; %1")
                        .arg(patternDescription),
                    false);

    if (m_udpSocket->hasPendingDatagrams() && !m_readContinuationScheduled)
    {
        m_readContinuationScheduled = true;
        QMetaObject::invokeMethod(this,
                                  "handleReadyRead",
                                  Qt::QueuedConnection);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Stops UDP reception and counter verification.
 * @param none
 * @return none
 * @detail Captures a final Statistics snapshot and leaves the bound UDP socket ready for
 *         another START operation.
 */
void UdpRxWorker::stopReception()
{
    if (!m_testRunning)
    {
        emitReceptionState();
        return;
    }

    const QString reason =
        tr("UDP STOP: reception and verification stopped; received %1 payload bytes in %2 packets; counter ok=%3; counter err=%4")
            .arg(m_totalPayloadBytes)
            .arg(m_totalPackets)
            .arg(m_counterOk)
            .arg(m_counterErrors);
    stopReceptionInternal(reason, false);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Shuts down the UDP RX worker before application exit.
 * @param none
 * @return none
 * @detail Stops active reception, closes the socket and timer, and leaves the object
 *         ready for QThread::quit().
 */
void UdpRxWorker::shutdown()
{
    if (m_shuttingDown)
    {
        return;
    }

    m_shuttingDown = true;

    if (m_testRunning)
    {
        stopReceptionInternal(
            tr("UDP reception stopped because the application is closing"),
            false);
    }

    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }

    if (m_connectionConfigured)
    {
        closeSocket(false, true);
    }
    else if (m_udpSocket != nullptr
             && m_udpSocket->state() != QAbstractSocket::UnconnectedState)
    {
        m_socketOperationInProgress = true;
        m_udpSocket->close();
        m_socketOperationInProgress = false;
    }

    m_initialized = false;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Reads and processes pending UDP datagrams.
 * @param none
 * @return none
 * @detail Processes a bounded batch per event-loop callback so Statistics and STOP remain
 *         responsive under a dense packet stream.
 */
void UdpRxWorker::handleReadyRead()
{
    m_readContinuationScheduled = false;

    if (!m_initialized || m_udpSocket == nullptr)
    {
        return;
    }

    int processedDatagrams = 0;
    while (m_udpSocket->hasPendingDatagrams()
           && processedDatagrams < kMaximumDatagramsPerReadBatch)
    {
        const qint64 pendingSize = m_udpSocket->pendingDatagramSize();
        if (pendingSize < 0
            || pendingSize > static_cast<qint64>(std::numeric_limits<int>::max()))
        {
            handleSocketFailure(
                tr("UDP read error: invalid pending datagram size %1")
                    .arg(pendingSize));
            return;
        }

        QByteArray datagram;
        datagram.resize(static_cast<int>(pendingSize));
        QHostAddress senderAddress;
        quint16 senderPort = 0;
        const qint64 bytesRead =
            m_udpSocket->readDatagram(datagram.data(),
                                      datagram.size(),
                                      &senderAddress,
                                      &senderPort);
        if (bytesRead < 0)
        {
            const int errorValue = static_cast<int>(m_udpSocket->error());
            const QAbstractSocket::SocketError socketError = m_udpSocket->error();
            if (socketError == QAbstractSocket::TemporaryError
                || socketError == QAbstractSocket::SocketTimeoutError)
            {
                emitWorkerEvent(tr("UDP read error: %1 (code %2)")
                                    .arg(socketErrorText(errorValue))
                                    .arg(errorValue),
                                true);
                break;
            }

            handleSocketFailure(tr("UDP read error: %1 (code %2)")
                                    .arg(socketErrorText(errorValue))
                                    .arg(errorValue));
            return;
        }

        if (bytesRead < datagram.size())
        {
            datagram.resize(static_cast<int>(bytesRead));
        }

        ++processedDatagrams;

        if (!m_testRunning)
        {
            continue;
        }

        if (!m_expectedSourceAddress.isNull()
            && senderAddress != m_expectedSourceAddress)
        {
            const QString sourceKey = senderAddress.toString();
            if (!m_reportedUnexpectedSources.contains(sourceKey))
            {
                m_reportedUnexpectedSources.insert(sourceKey);
                emitWorkerEvent(
                    tr("UDP datagram ignored: expected source IP %1, received from %2:%3")
                        .arg(m_expectedSourceIp,
                             sourceKey.isEmpty() ? QStringLiteral("--") : sourceKey)
                        .arg(senderPort),
                    true);
            }
            continue;
        }

        const quint64 payloadBytes = static_cast<quint64>(bytesRead);
        if (std::numeric_limits<quint64>::max() - m_totalPayloadBytes
            < payloadBytes)
        {
            m_totalPayloadBytes = std::numeric_limits<quint64>::max();
        }
        else
        {
            m_totalPayloadBytes += payloadBytes;
        }

        if (m_totalPackets < std::numeric_limits<quint64>::max())
        {
            ++m_totalPackets;
        }

        processDatagram(datagram, senderAddress, senderPort);
    }

    if (m_udpSocket->hasPendingDatagrams() && !m_readContinuationScheduled)
    {
        m_readContinuationScheduled = true;
        QMetaObject::invokeMethod(this,
                                  "handleReadyRead",
                                  Qt::QueuedConnection);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Performs the one-second UDP Statistics update.
 * @param none
 * @return none
 * @detail Calculates payload speed and packet rate from the actual elapsed interval and
 *         updates twenty-second sample statistics.
 */
void UdpRxWorker::updateStatistics()
{
    if (!m_testRunning)
    {
        return;
    }

    updateStatisticsSnapshot(true);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles an asynchronous UDP socket error.
 * @param error QAbstractSocket error reported by QUdpSocket.
 * @return none
 * @detail Temporary errors are logged without closing the socket; fatal errors stop
 *         reception and release the connection.
 */
void UdpRxWorker::handleSocketError(QAbstractSocket::SocketError error)
{
    if (error == QAbstractSocket::UnknownSocketError
        || m_socketOperationInProgress
        || m_shuttingDown)
    {
        return;
    }

    const int errorValue = static_cast<int>(error);
    const QString message =
        tr("UDP socket error on %1:%2: %3 (code %4)")
            .arg(m_localIp.isEmpty() ? QStringLiteral("--") : m_localIp)
            .arg(m_listenPort)
            .arg(socketErrorText(errorValue))
            .arg(errorValue);

    if (error == QAbstractSocket::TemporaryError
        || error == QAbstractSocket::SocketTimeoutError)
    {
        emitWorkerEvent(message, true);
        return;
    }

    handleSocketFailure(message);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles an unexpected UDP socket state transition.
 * @param state New QAbstractSocket state.
 * @return none
 * @detail Detects an unrequested close of a logically connected receiver socket.
 */
void UdpRxWorker::handleSocketStateChanged(QAbstractSocket::SocketState state)
{
    if (m_connectionConfigured
        && state == QAbstractSocket::UnconnectedState
        && !m_socketOperationInProgress
        && !m_shuttingDown)
    {
        handleSocketFailure(
            tr("UDP receiver socket closed unexpectedly: local=%1:%2")
                .arg(m_localIp)
                .arg(m_listenPort));
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Validates UDP Pattern arguments and builds worker settings.
 * @param counterBits Counter bit width.
 * @param blockBytes Expected UDP payload size in bytes.
 * @param togetherCount Informational packets-per-burst value.
 * @param periodMs Informational burst period in milliseconds.
 * @param initialValue First expected counter value.
 * @param settings Output pointer for validated settings.
 * @param errorText Output pointer for a validation error message.
 * @return true when all parameters are valid; otherwise false.
 * @detail Checks supported widths, IPv4 UDP payload limits, alignment, positive Togeth,
 *         Period range, and initial-value range.
 */
bool UdpRxWorker::makePatternSettings(int counterBits,
                                      int blockBytes,
                                      int togetherCount,
                                      int periodMs,
                                      quint64 initialValue,
                                      PatternSettings *settings,
                                      QString *errorText) const
{
    if (settings == nullptr || errorText == nullptr)
    {
        return false;
    }

    if (counterBits != 8
        && counterBits != 16
        && counterBits != 32
        && counterBits != 64)
    {
        *errorText = tr("unsupported counter width");
        return false;
    }

    const int counterBytes = counterBits / 8;
    if (blockBytes <= 0
        || blockBytes > kMaximumIpv4UdpPayloadBytes
        || (blockBytes % counterBytes) != 0)
    {
        *errorText =
            tr("block, bytes must be 1...65507 and a multiple of %1 bytes")
                .arg(counterBytes);
        return false;
    }

    if (togetherCount <= 0)
    {
        *errorText = tr("Togeth must be greater than zero");
        return false;
    }

    if (periodMs < 0)
    {
        *errorText = tr("Period, ms cannot be negative");
        return false;
    }

    const quint64 maximumCounterValue =
        counterBits == 64
            ? std::numeric_limits<quint64>::max()
            : (quint64(1) << counterBits) - quint64(1);
    if (initialValue > maximumCounterValue)
    {
        *errorText = tr("init value does not fit the selected counter width");
        return false;
    }

    settings->counterBits = counterBits;
    settings->counterBytes = counterBytes;
    settings->blockBytes = blockBytes;
    settings->togetherCount = togetherCount;
    settings->periodMs = periodMs;
    settings->initialValue = initialValue;
    settings->maximumCounterValue = maximumCounterValue;
    errorText->clear();
    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Resets UDP Statistics and verification state before START.
 * @param settings Validated settings for the new test.
 * @return none
 * @detail Sets expected to init, clears totals and interval samples, starts monotonic
 *         timing, and emits the initial zero-valued snapshot.
 */
void UdpRxWorker::resetStatistics(const PatternSettings &settings)
{
    m_activePattern = settings;
    m_startTime = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    m_expectedCounter = settings.initialValue;
    m_lastReceivedCounter = settings.initialValue;
    m_totalPayloadBytes = 0;
    m_totalPackets = 0;
    m_lastStatisticsBytes = 0;
    m_lastStatisticsPackets = 0;
    m_lastStatisticsElapsedMs = 0;
    m_counterOk = 0;
    m_counterErrors = 0;
    m_lastPeriodicLogBytes = 0;
    m_lastPeriodicCounterOk = 0;
    m_lastPeriodicCounterErrors = 0;
    m_lastPeriodicLogElapsedMs = 0;
    m_periodicMinimumSpeedKbps = 0.0;
    m_periodicMaximumSpeedKbps = 0.0;
    m_periodicSpeedSumKbps = 0.0;
    m_periodicMinimumPacketsPerSecond = 0.0;
    m_periodicMaximumPacketsPerSecond = 0.0;
    m_periodicPacketsPerSecondSum = 0.0;
    m_periodicSampleCount = 0;
    m_reportedUnexpectedSources.clear();
    m_elapsedTimer.start();

    emit statisticsUpdated(m_startTime,
                           0,
                           0,
                           m_lastReceivedCounter,
                           0,
                           0,
                           0.0,
                           0.0);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Discards every UDP datagram currently queued in the socket.
 * @param none
 * @return Number of datagrams removed from the receive queue.
 * @detail Removes a bounded amount of stale data before START so a continuously
 *         arriving stream cannot block the worker event loop indefinitely.
 */
quint64 UdpRxWorker::discardPendingDatagrams()
{
    if (m_udpSocket == nullptr)
    {
        return 0;
    }

    quint64 discardedDatagrams = 0;
    QElapsedTimer discardTimer;
    discardTimer.start();

    while (m_udpSocket->hasPendingDatagrams()
           && discardedDatagrams
                  < static_cast<quint64>(kMaximumDatagramsToDiscardBeforeStart)
           && discardTimer.elapsed() < kDiscardBeforeStartTimeBudgetMs)
    {
        const qint64 pendingSize = m_udpSocket->pendingDatagramSize();
        if (pendingSize < 0
            || pendingSize > static_cast<qint64>(std::numeric_limits<int>::max()))
        {
            break;
        }

        QByteArray datagram;
        datagram.resize(static_cast<int>(pendingSize));
        if (m_udpSocket->readDatagram(datagram.data(), datagram.size()) < 0)
        {
            break;
        }

        ++discardedDatagrams;
    }

    return discardedDatagrams;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Processes one accepted UDP payload.
 * @param datagram Complete UDP payload bytes.
 * @param senderAddress Source IPv4 address.
 * @param senderPort Source UDP port.
 * @return none
 * @detail Decodes complete little-endian counter fields, keeps sequence continuity across
 *         packets, and reports payload alignment errors.
 */
void UdpRxWorker::processDatagram(const QByteArray &datagram,
                                  const QHostAddress &senderAddress,
                                  quint16 senderPort)
{
    const int counterBytes = m_activePattern.counterBytes;
    if (counterBytes <= 0)
    {
        return;
    }

    const int completeBytes =
        (datagram.size() / counterBytes) * counterBytes;
    const int trailingBytes = datagram.size() - completeBytes;
    if (trailingBytes > 0)
    {
        emitWorkerEvent(
            tr("UDP payload alignment error: source=%1:%2; payload=%3 bytes; counter_field=%4 bytes; trailing=%5 bytes")
                .arg(senderAddress.toString())
                .arg(senderPort)
                .arg(datagram.size())
                .arg(counterBytes)
                .arg(trailingBytes),
            true);
    }

    const char *data = datagram.constData();
    for (int offset = 0; offset < completeBytes; offset += counterBytes)
    {
        const quint64 receivedCounter =
            decodeLittleEndianCounter(data + offset, counterBytes);
        const quint64 expectedCounter = m_expectedCounter;
        const quint64 nextExpectedCounter = nextCounterValue(receivedCounter);
        m_lastReceivedCounter = receivedCounter;

        if (receivedCounter == expectedCounter)
        {
            if (m_counterOk < std::numeric_limits<quint64>::max())
            {
                ++m_counterOk;
            }
        }
        else
        {
            if (m_counterErrors < std::numeric_limits<quint64>::max())
            {
                ++m_counterErrors;
            }

            emitWorkerEvent(
                tr("UDP counter error: expected=%1; received=%2; delta=%3; next_expected=%4")
                    .arg(expectedCounter)
                    .arg(receivedCounter)
                    .arg(counterDeltaText(expectedCounter, receivedCounter))
                    .arg(nextExpectedCounter),
                true);
        }

        m_expectedCounter = nextExpectedCounter;
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Decodes one little-endian counter value.
 * @param data Pointer to the first byte of the value.
 * @param byteCount Value size from one through eight bytes.
 * @return The received counter represented as an unsigned 64-bit value.
 * @detail Performs byte-by-byte conversion independently of host byte order and
 *         alignment.
 */
quint64 UdpRxWorker::decodeLittleEndianCounter(const char *data,
                                               int byteCount) const
{
    if (data == nullptr || byteCount <= 0 || byteCount > 8)
    {
        return 0;
    }

    quint64 value = 0;
    for (int byteIndex = 0; byteIndex < byteCount; ++byteIndex)
    {
        const quint64 byteValue =
            static_cast<quint64>(static_cast<unsigned char>(data[byteIndex]));
        value |= byteValue << (byteIndex * 8);
    }

    return value;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns the next counter value with wraparound.
 * @param value Current counter value.
 * @return value + 1, or zero after the maximum selected counter value.
 * @detail Uses the precomputed maximumCounterValue for 8 through 64 bits.
 */
quint64 UdpRxWorker::nextCounterValue(quint64 value) const
{
    if (value >= m_activePattern.maximumCounterValue)
    {
        return 0;
    }

    return value + quint64(1);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Formats the shortest modular delta between expected and received values.
 * @param expectedCounter Expected counter value.
 * @param receivedCounter Received counter value.
 * @return Decimal delta text, for example 30, -2, or 1 across wraparound.
 * @detail Unsigned subtraction is used modulo the selected counter width, and the
 *         shorter forward or backward distance is reported.
 */
QString UdpRxWorker::counterDeltaText(quint64 expectedCounter,
                                      quint64 receivedCounter) const
{
    const quint64 mask = m_activePattern.maximumCounterValue;
    const quint64 forwardDistance = (receivedCounter - expectedCounter) & mask;
    const quint64 backwardDistance = (expectedCounter - receivedCounter) & mask;

    if (forwardDistance <= backwardDistance)
    {
        return QString::number(forwardDistance);
    }

    return QStringLiteral("-") + QString::number(backwardDistance);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Updates and emits the current UDP Statistics snapshot.
 * @param includePeriodicSample true only for the one-second timer tick.
 * @return none
 * @detail Calculates rates from byte and packet deltas over the exact monotonic interval
 *         and optionally accumulates one sample for the twenty-second log.
 */
void UdpRxWorker::updateStatisticsSnapshot(bool includePeriodicSample)
{
    if (!m_elapsedTimer.isValid())
    {
        return;
    }

    const qint64 elapsedMilliseconds = m_elapsedTimer.elapsed();
    const qint64 deltaMilliseconds =
        elapsedMilliseconds - m_lastStatisticsElapsedMs;
    const quint64 deltaBytes =
        m_totalPayloadBytes >= m_lastStatisticsBytes
            ? m_totalPayloadBytes - m_lastStatisticsBytes
            : 0;
    const quint64 deltaPackets =
        m_totalPackets >= m_lastStatisticsPackets
            ? m_totalPackets - m_lastStatisticsPackets
            : 0;

    double speedKbps = 0.0;
    double packetsPerSecond = 0.0;
    if (deltaMilliseconds > 0)
    {
        speedKbps =
            (static_cast<double>(deltaBytes) * 8.0)
            / static_cast<double>(deltaMilliseconds);
        packetsPerSecond =
            (static_cast<double>(deltaPackets) * 1000.0)
            / static_cast<double>(deltaMilliseconds);
    }

    m_lastStatisticsBytes = m_totalPayloadBytes;
    m_lastStatisticsPackets = m_totalPackets;
    m_lastStatisticsElapsedMs = elapsedMilliseconds;

    if (includePeriodicSample)
    {
        if (m_periodicSampleCount == 0)
        {
            m_periodicMinimumSpeedKbps = speedKbps;
            m_periodicMaximumSpeedKbps = speedKbps;
            m_periodicMinimumPacketsPerSecond = packetsPerSecond;
            m_periodicMaximumPacketsPerSecond = packetsPerSecond;
        }
        else
        {
            if (speedKbps < m_periodicMinimumSpeedKbps)
            {
                m_periodicMinimumSpeedKbps = speedKbps;
            }
            if (speedKbps > m_periodicMaximumSpeedKbps)
            {
                m_periodicMaximumSpeedKbps = speedKbps;
            }
            if (packetsPerSecond < m_periodicMinimumPacketsPerSecond)
            {
                m_periodicMinimumPacketsPerSecond = packetsPerSecond;
            }
            if (packetsPerSecond > m_periodicMaximumPacketsPerSecond)
            {
                m_periodicMaximumPacketsPerSecond = packetsPerSecond;
            }
        }

        m_periodicSpeedSumKbps += speedKbps;
        m_periodicPacketsPerSecondSum += packetsPerSecond;
        if (m_periodicSampleCount < std::numeric_limits<quint64>::max())
        {
            ++m_periodicSampleCount;
        }
    }

    emit statisticsUpdated(m_startTime,
                           elapsedMilliseconds,
                           m_totalPayloadBytes,
                           m_lastReceivedCounter,
                           m_counterOk,
                           m_counterErrors,
                           speedKbps,
                           packetsPerSecond);

    emitPeriodicLogLineIfDue();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates the twenty-second UDP Statistics line when due.
 * @param none
 * @return none
 * @detail Uses total and delta byte and counter values plus minimum, arithmetic mean,
 *         and maximum of the one-second packet-rate and speed samples.
 */
void UdpRxWorker::emitPeriodicLogLineIfDue()
{
    if (!m_testRunning || !m_elapsedTimer.isValid())
    {
        return;
    }

    const qint64 elapsedMilliseconds = m_elapsedTimer.elapsed();
    const qint64 intervalMilliseconds =
        elapsedMilliseconds - m_lastPeriodicLogElapsedMs;
    if (intervalMilliseconds < kPeriodicLogIntervalMs
        || m_periodicSampleCount == 0)
    {
        return;
    }

    const quint64 deltaBytes =
        m_totalPayloadBytes >= m_lastPeriodicLogBytes
            ? m_totalPayloadBytes - m_lastPeriodicLogBytes
            : 0;
    const quint64 deltaCounterOk =
        m_counterOk >= m_lastPeriodicCounterOk
            ? m_counterOk - m_lastPeriodicCounterOk
            : 0;
    const quint64 deltaCounterErrors =
        m_counterErrors >= m_lastPeriodicCounterErrors
            ? m_counterErrors - m_lastPeriodicCounterErrors
            : 0;

    const double averagePacketsPerSecond =
        m_periodicPacketsPerSecondSum
        / static_cast<double>(m_periodicSampleCount);
    const double averageSpeedKbps =
        m_periodicSpeedSumKbps
        / static_cast<double>(m_periodicSampleCount);

    const QString line =
        QStringLiteral("%1, time=%2, rx_bytes=%3, delta_rx_bytes=%4, "
                       "curr_counter=%5, counter_ok=%6, delta_counter_ok=%7, "
                       "counter_err=%8, delta_counter_err=%9, "
                       "min_packet/s=%10, avrg_packets/s=%11, max_packet/s=%12, "
                       "min_speed_Kb/s=%13, avrg_speed_Kb/s=%14, max_speed_Kb/s=%15")
            .arg(QDateTime::currentDateTime().toString(
                QStringLiteral("HH:mm:ss.zzz")))
            .arg(formatElapsedTime(elapsedMilliseconds))
            .arg(m_totalPayloadBytes)
            .arg(deltaBytes)
            .arg(m_lastReceivedCounter)
            .arg(m_counterOk)
            .arg(deltaCounterOk)
            .arg(m_counterErrors)
            .arg(deltaCounterErrors)
            .arg(formatRate(m_periodicMinimumPacketsPerSecond))
            .arg(formatRate(averagePacketsPerSecond))
            .arg(formatRate(m_periodicMaximumPacketsPerSecond))
            .arg(formatRate(m_periodicMinimumSpeedKbps))
            .arg(formatRate(averageSpeedKbps))
            .arg(formatRate(m_periodicMaximumSpeedKbps));

    emit periodicLogLineReady(line);

    m_lastPeriodicLogBytes = m_totalPayloadBytes;
    m_lastPeriodicCounterOk = m_counterOk;
    m_lastPeriodicCounterErrors = m_counterErrors;
    m_lastPeriodicLogElapsedMs = elapsedMilliseconds;
    m_periodicMinimumSpeedKbps = 0.0;
    m_periodicMaximumSpeedKbps = 0.0;
    m_periodicSpeedSumKbps = 0.0;
    m_periodicMinimumPacketsPerSecond = 0.0;
    m_periodicMaximumPacketsPerSecond = 0.0;
    m_periodicPacketsPerSecondSum = 0.0;
    m_periodicSampleCount = 0;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Internally finalizes an active UDP reception test.
 * @param reasonText Optional stop reason; an empty string is not logged.
 * @param reasonIsError true for a red reason entry; false for a black service entry.
 * @return none
 * @detail Creates the final snapshot, stops the timer, clears per-test source-report
 *         state, and emits receptionStateChanged(false).
 */
void UdpRxWorker::stopReceptionInternal(const QString &reasonText,
                                        bool reasonIsError)
{
    if (!m_testRunning)
    {
        emitReceptionState();
        return;
    }

    updateStatisticsSnapshot(false);
    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }

    m_testRunning = false;
    m_reportedUnexpectedSources.clear();
    emitReceptionState();

    if (!reasonText.trimmed().isEmpty())
    {
        emitWorkerEvent(reasonText, reasonIsError);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles a fatal UDP socket failure.
 * @param reason Complete reason text for the red event-log entry.
 * @return none
 * @detail Prevents recursive handling, stops an active test, closes the socket, and
 *         reports a failed connection state to the GUI.
 */
void UdpRxWorker::handleSocketFailure(const QString &reason)
{
    if (m_handlingSocketFailure
        || m_shuttingDown
        || !m_connectionConfigured)
    {
        return;
    }

    m_handlingSocketFailure = true;
    QString completeReason = reason;
    if (m_testRunning)
    {
        completeReason +=
            tr("; reception stopped; received %1 payload bytes in %2 packets; counter ok=%3; counter err=%4")
                .arg(m_totalPayloadBytes)
                .arg(m_totalPackets)
                .arg(m_counterOk)
                .arg(m_counterErrors);
        stopReceptionInternal(QString(), false);
    }

    emitWorkerEvent(completeReason, true);
    closeSocket(true, false);
    m_handlingSocketFailure = false;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Closes the UDP receiver socket and emits a disconnected state.
 * @param causedByFailure true when closure was caused by an error.
 * @param emitEvent true to emit a normal socket-closed service event.
 * @return none
 * @detail Copies endpoint information before clearing internal connection state.
 */
void UdpRxWorker::closeSocket(bool causedByFailure, bool emitEvent)
{
    const QString expectedSourceIp = m_expectedSourceIp;
    const quint16 listenPort = m_listenPort;
    const QString localIp = m_localIp;

    if (m_udpSocket != nullptr)
    {
        m_socketOperationInProgress = true;
        m_udpSocket->close();
        m_socketOperationInProgress = false;
    }

    m_connectionConfigured = false;
    m_expectedSourceAddress = QHostAddress();
    m_expectedSourceIp.clear();
    m_localIp.clear();
    m_listenPort = 0;
    m_readContinuationScheduled = false;
    m_reportedUnexpectedSources.clear();

    emit connectionStateChanged(false,
                                expectedSourceIp,
                                listenPort,
                                localIp,
                                causedByFailure);

    if (emitEvent && !localIp.isEmpty())
    {
        emitWorkerEvent(tr("UDP receiver disconnected: local=%1:%2; expected_source=%3")
                            .arg(localIp)
                            .arg(listenPort)
                            .arg(expectedSourceIp),
                        false);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Formats elapsed test time.
 * @param elapsedMilliseconds Elapsed duration in milliseconds.
 * @return HH:MM:SS string with an unlimited number of hours.
 * @detail Hours are calculated from the full duration and do not wrap after 23.
 */
QString UdpRxWorker::formatElapsedTime(qint64 elapsedMilliseconds) const
{
    const qint64 safeMilliseconds = qMax<qint64>(0, elapsedMilliseconds);
    const qint64 totalSeconds = safeMilliseconds / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;

    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Formats a nonnegative floating-point rate for logs.
 * @param value Speed or packet-rate value.
 * @return String containing at most three digits after the decimal point.
 * @detail Removes insignificant trailing zeros and returns zero for invalid values.
 */
QString UdpRxWorker::formatRate(double value) const
{
    if (!(value > 0.0))
    {
        return QStringLiteral("0");
    }

    QString result = QString::number(value, 'f', 3);
    while (result.endsWith(QLatin1Char('0')))
    {
        result.chop(1);
    }

    if (result.endsWith(QLatin1Char('.')))
    {
        result.chop(1);
    }

    return result;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns a fixed English description for a UDP socket error.
 * @param socketError Numeric QAbstractSocket error value.
 * @return English text independent of the operating-system language.
 * @detail Maps Qt 5.12 socket errors used by UDP reception.
 */
QString UdpRxWorker::socketErrorText(int socketError) const
{
    switch (static_cast<QAbstractSocket::SocketError>(socketError))
    {
    case QAbstractSocket::ConnectionRefusedError:
        return QStringLiteral("connection refused or ICMP port unreachable");
    case QAbstractSocket::RemoteHostClosedError:
        return QStringLiteral("remote host closed the connection");
    case QAbstractSocket::HostNotFoundError:
        return QStringLiteral("host not found");
    case QAbstractSocket::SocketAccessError:
        return QStringLiteral("socket access denied");
    case QAbstractSocket::SocketResourceError:
        return QStringLiteral("insufficient socket resources");
    case QAbstractSocket::SocketTimeoutError:
        return QStringLiteral("socket operation timed out");
    case QAbstractSocket::DatagramTooLargeError:
        return QStringLiteral("UDP datagram is too large");
    case QAbstractSocket::NetworkError:
        return QStringLiteral("network error");
    case QAbstractSocket::AddressInUseError:
        return QStringLiteral("local address or port is already in use");
    case QAbstractSocket::SocketAddressNotAvailableError:
        return QStringLiteral("local address is not available");
    case QAbstractSocket::UnsupportedSocketOperationError:
        return QStringLiteral("unsupported socket operation");
    case QAbstractSocket::UnfinishedSocketOperationError:
        return QStringLiteral("unfinished socket operation");
    case QAbstractSocket::ProxyAuthenticationRequiredError:
        return QStringLiteral("proxy authentication required");
    case QAbstractSocket::SslHandshakeFailedError:
        return QStringLiteral("SSL handshake failed");
    case QAbstractSocket::ProxyConnectionRefusedError:
        return QStringLiteral("proxy connection refused");
    case QAbstractSocket::ProxyConnectionClosedError:
        return QStringLiteral("proxy connection closed");
    case QAbstractSocket::ProxyConnectionTimeoutError:
        return QStringLiteral("proxy connection timed out");
    case QAbstractSocket::ProxyNotFoundError:
        return QStringLiteral("proxy not found");
    case QAbstractSocket::ProxyProtocolError:
        return QStringLiteral("proxy protocol error");
    case QAbstractSocket::OperationError:
        return QStringLiteral("operation is not permitted in the current state");
    case QAbstractSocket::SslInternalError:
        return QStringLiteral("internal SSL error");
    case QAbstractSocket::SslInvalidUserDataError:
        return QStringLiteral("invalid SSL user data");
    case QAbstractSocket::TemporaryError:
        return QStringLiteral("temporary socket error");
    case QAbstractSocket::UnknownSocketError:
        return QStringLiteral("unknown socket error");
    }

    return QStringLiteral("unrecognized socket error");
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Emits a normal or error event with an exact timestamp.
 * @param text Event text without a timestamp.
 * @param error true for an error; false for a normal entry.
 * @return none
 * @detail Creates the HH:MM:SS.mmm timestamp in the UDP worker thread before queued
 *         delivery to the GUI.
 */
void UdpRxWorker::emitWorkerEvent(const QString &text, bool error)
{
    emit eventGenerated(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
        text,
        error);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Emits the current UDP reception state.
 * @param none
 * @return none
 * @detail Centralizes delivery of m_testRunning to the GUI.
 */
void UdpRxWorker::emitReceptionState()
{
    emit receptionStateChanged(m_testRunning);
}
