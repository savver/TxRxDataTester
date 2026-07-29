#if defined(_WIN32)
#include <winsock2.h>
#endif

#include "udprxworker.h"

#include <QDateTime>
#include <QIODevice>
#include <QMetaObject>
#include <QTime>
#include <QTimer>
#include <QUdpSocket>
#include <QVariant>

#include <cstring>
#include <limits>

#if !defined(_WIN32)
#include <cerrno>
#include <sys/ioctl.h>
#endif

namespace
{
constexpr int kStatisticsIntervalMs = 1000;
constexpr qint64 kPeriodicLogIntervalMs = 20 * 1000;
constexpr int kMaximumDatagramsPerSocketBatch = 4096;
constexpr int kMinimumDatagramsBeforeSocketTimeCheck = 32;
constexpr qint64 kSocketReadBatchTimeBudgetNs = 750 * 1000;
constexpr int kMaximumDatagramsPerProcessingBatch = 4096;
constexpr int kMinimumDatagramsBeforeProcessingTimeCheck = 32;
constexpr qint64 kProcessingBatchTimeBudgetNs = 1000 * 1000;
constexpr int kMaximumDatagramsToDiscardBeforeStart = 16384;
constexpr qint64 kDiscardBeforeStartTimeBudgetNs = 25 * 1000 * 1000;
constexpr int kRequestedReceiveBufferBytes = 16 * 1024 * 1024;
constexpr int kMaximumIpv4UdpPayloadBytes = 65507;
constexpr int kReceivePumpIntervalMs = 1;
constexpr qint64 kReceiveNotificationStallThresholdMs = 5;
constexpr qint64 kReceivePumpRecoveryLogIntervalMs = 1000;
constexpr int kPacketQueueMemoryBudgetBytes = 32 * 1024 * 1024;
constexpr int kMinimumPacketQueueSlotBytes = 2048;
constexpr int kMinimumPacketQueueSlots = 256;
constexpr int kMaximumPacketQueueSlots = 16384;
constexpr qint64 kQueuePressureLogIntervalMs = 1000;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates the UDP receiver worker object.
 * @param parent Parent QObject; normally omitted before moveToThread().
 * @return none
 * @detail Initializes scalar state only. QUdpSocket, QTimer objects, and the persistent
 *         receive buffer are created later by initialize() in the worker thread.
 */
UdpRxWorker::UdpRxWorker(QObject *parent)
    : QObject(parent)
    , m_udpSocket(nullptr)
    , m_statisticsTimer(nullptr)
    , m_receivePumpTimer(nullptr)
    , m_packetProcessingTimer(nullptr)
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
    , m_readyReadCalls(0)
    , m_emptyReadyReadCalls(0)
    , m_continuationCallbacks(0)
    , m_receivePumpCalls(0)
    , m_receivePumpRecoveries(0)
    , m_suppressedReceivePumpEvents(0)
    , m_readErrors(0)
    , m_datagramsReadByWorker(0)
    , m_datagramsProcessed(0)
    , m_processingCallbacks(0)
    , m_queuePressureEvents(0)
    , m_queueDroppedDatagrams(0)
    , m_oversizeDirectDatagrams(0)
    , m_totalSocketReadTimeNs(0)
    , m_totalProcessingTimeNs(0)
    , m_lastStatisticsProcessingTimeNs(0)
    , m_maximumReadBatch(0)
    , m_maximumProcessingBatch(0)
    , m_maximumSocketReadBatchNs(0)
    , m_maximumProcessingBatchNs(0)
    , m_lastProcessingLoadPercentTimes1000(0)
    , m_lastReadyReadMs(0)
    , m_lastDatagramReadMs(0)
    , m_lastReceivePumpEventMs(-1)
    , m_lastQueuePressureEventMs(-1)
    , m_packetQueueSlotBytes(0)
    , m_packetQueueCapacity(0)
    , m_packetQueueHead(0)
    , m_packetQueueTail(0)
    , m_packetQueueDepth(0)
    , m_maximumPacketQueueDepth(0)
    , m_actualReceiveBufferBytes(0)
    , m_initialized(false)
    , m_connectionConfigured(false)
    , m_testRunning(false)
    , m_readContinuationScheduled(false)
    , m_packetProcessingScheduled(false)
    , m_receivePumpFallbackActive(false)
    , m_readDrainInProgress(false)
    , m_packetProcessingInProgress(false)
    , m_readRequestedDuringDrain(false)
    , m_readAttemptInProgress(false)
    , m_nativeQueryFailureReported(false)
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
 * @detail Stops both timers and closes the UDP socket as a safeguard after normal
 *         shutdown.
 */
UdpRxWorker::~UdpRxWorker()
{
    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }

    if (m_receivePumpTimer != nullptr)
    {
        m_receivePumpTimer->stop();
    }

    if (m_packetProcessingTimer != nullptr)
    {
        m_packetProcessingTimer->stop();
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
 * @detail Creates QUdpSocket, Statistics, receive-pump, and processing timers, allocates one reusable
 *         maximum-size IPv4 UDP payload buffer, and reports readiness to the GUI.
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
    m_receivePumpTimer = new QTimer(this);
    m_packetProcessingTimer = new QTimer(this);
    m_receiveBuffer.resize(kMaximumIpv4UdpPayloadBytes);
    m_workerUptimeTimer.start();

    m_statisticsTimer->setInterval(kStatisticsIntervalMs);
    m_statisticsTimer->setSingleShot(false);
    m_statisticsTimer->setTimerType(Qt::PreciseTimer);

    m_receivePumpTimer->setInterval(kReceivePumpIntervalMs);
    m_receivePumpTimer->setSingleShot(false);
    m_receivePumpTimer->setTimerType(Qt::PreciseTimer);

    m_packetProcessingTimer->setInterval(0);
    m_packetProcessingTimer->setSingleShot(true);
    m_packetProcessingTimer->setTimerType(Qt::PreciseTimer);

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
    connect(m_receivePumpTimer,
            &QTimer::timeout,
            this,
            &UdpRxWorker::serviceReceivePump);
    connect(m_packetProcessingTimer,
            &QTimer::timeout,
            this,
            &UdpRxWorker::processPacketQueue);

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
 * @detail Reserves localIp:listenPort exclusively, requests a large operating-system
 *         receive buffer, starts the receive pump, and records actual diagnostics.
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

    if (m_receiveBuffer.size() != kMaximumIpv4UdpPayloadBytes)
    {
        m_receiveBuffer.resize(kMaximumIpv4UdpPayloadBytes);
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

    bool receiveBufferOk = false;
    m_actualReceiveBufferBytes =
        m_udpSocket->socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption)
            .toInt(&receiveBufferOk);
    if (!receiveBufferOk || m_actualReceiveBufferBytes < 0)
    {
        m_actualReceiveBufferBytes = 0;
    }

    m_expectedSourceAddress = sourceAddress;
    m_expectedSourceIp = sourceAddress.toString();
    m_localIp = localAddress.toString();
    m_listenPort = m_udpSocket->localPort();
    m_connectionConfigured = true;
    m_readContinuationScheduled = false;
    m_packetProcessingScheduled = false;
    m_receivePumpFallbackActive = false;
    m_readDrainInProgress = false;
    m_packetProcessingInProgress = false;
    m_readRequestedDuringDrain = false;
    m_readAttemptInProgress = false;
    m_nativeQueryFailureReported = false;
    m_reportedUnexpectedSources.clear();
    clearPacketQueue();

    const qint64 nowMs = m_workerUptimeTimer.isValid()
                             ? m_workerUptimeTimer.elapsed()
                             : 0;
    m_lastReadyReadMs = nowMs;
    m_lastDatagramReadMs = nowMs;
    resetReadDiagnostics();

    if (m_receivePumpTimer != nullptr)
    {
        m_receivePumpTimer->start();
    }

    emit connectionStateChanged(true,
                                m_expectedSourceIp,
                                m_listenPort,
                                m_localIp,
                                false);
    emitWorkerEvent(
        tr("UDP receiver ready: local=%1:%2; expected_source=%3; "
           "requested_rcvbuf=%4; actual_rcvbuf=%5; receive_buffer=%6; "
           "receive_pump=%7 ms; stall_threshold=%8 ms")
            .arg(m_localIp)
            .arg(m_listenPort)
            .arg(m_expectedSourceIp)
            .arg(kRequestedReceiveBufferBytes)
            .arg(m_actualReceiveBufferBytes)
            .arg(m_receiveBuffer.size())
            .arg(kReceivePumpIntervalMs)
            .arg(kReceiveNotificationStallThresholdMs),
        false);

    if (m_udpSocket->hasPendingDatagrams())
    {
        scheduleReadContinuation();
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
        if (m_packetProcessingTimer != nullptr)
        {
            m_packetProcessingTimer->stop();
        }
        m_packetProcessingScheduled = false;
        processQueuedDatagrams(0,
                               std::numeric_limits<int>::max(),
                               true);
        stopReceptionInternal(
            tr("UDP reception stopped because DISCONNECT was requested; %1")
                .arg(readDiagnosticsText()),
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
 * @detail Discards stale queued datagrams, resets Statistics and read diagnostics, and
 *         preserves counter continuity between all accepted UDP packets.
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

    if (!configurePacketQueue(settings.blockBytes, &errorText))
    {
        emitWorkerEvent(tr("UDP START failed: %1").arg(errorText), true);
        emitReceptionState();
        return;
    }

    const quint64 discardedDatagrams = discardPendingDatagrams();
    if (!m_connectionConfigured
        || m_udpSocket->state() != QAbstractSocket::BoundState)
    {
        emitReceptionState();
        return;
    }

    clearPacketQueue();
    resetStatistics(settings);
    resetReadDiagnostics();
    m_testRunning = true;
    m_statisticsTimer->start();
    emitReceptionState();
    emitWorkerEvent(
        tr("UDP START: reception and verification started; %1; "
           "discarded_before_start=%2; actual_rcvbuf=%3; receive_buffer=%4; "
           "packet_queue_slots=%5; packet_queue_slot_bytes=%6; "
           "packet_queue_memory=%7")
            .arg(patternDescription)
            .arg(discardedDatagrams)
            .arg(m_actualReceiveBufferBytes)
            .arg(m_receiveBuffer.size())
            .arg(m_packetQueueCapacity)
            .arg(m_packetQueueSlotBytes)
            .arg(m_packetQueueStorage.size()),
        false);

    if (m_udpSocket->hasPendingDatagrams())
    {
        scheduleReadContinuation();
    }
}
/*-----------------------------------------------------------------------------*/

/**
 * @brief Stops UDP reception and counter verification.
 * @param none
 * @return none
 * @detail Captures a final Statistics snapshot, appends read-path diagnostics, and leaves
 *         the bound UDP socket ready for another START operation.
 */
void UdpRxWorker::stopReception()
{
    if (!m_testRunning)
    {
        emitReceptionState();
        return;
    }

    if (m_packetProcessingTimer != nullptr)
    {
        m_packetProcessingTimer->stop();
    }
    m_packetProcessingScheduled = false;
    processQueuedDatagrams(0,
                           std::numeric_limits<int>::max(),
                           true);

    const QString reason =
        tr("UDP STOP: reception and verification stopped; received %1 payload bytes "
           "in %2 packets; counter ok=%3; counter err=%4; %5")
            .arg(m_totalPayloadBytes)
            .arg(m_totalPackets)
            .arg(m_counterOk)
            .arg(m_counterErrors)
            .arg(readDiagnosticsText());
    stopReceptionInternal(reason, false);
}
/*-----------------------------------------------------------------------------*/

/**
 * @brief Shuts down the UDP RX worker before application exit.
 * @param none
 * @return none
 * @detail Stops reception, both timers, closes the socket, and releases the persistent
 *         receive buffer before QThread::quit().
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
        if (m_packetProcessingTimer != nullptr)
        {
            m_packetProcessingTimer->stop();
        }
        m_packetProcessingScheduled = false;
        processQueuedDatagrams(0,
                               std::numeric_limits<int>::max(),
                               true);
        stopReceptionInternal(
            tr("UDP reception stopped because the application is closing; %1")
                .arg(readDiagnosticsText()),
            false);
    }

    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }

    if (m_receivePumpTimer != nullptr)
    {
        m_receivePumpTimer->stop();
    }

    if (m_packetProcessingTimer != nullptr)
    {
        m_packetProcessingTimer->stop();
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

    m_receiveBuffer.clear();
    m_packetQueueStorage.clear();
    m_packetQueueLengths.clear();
    m_packetQueueSenderPorts.clear();
    m_initialized = false;
}
/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles a real QUdpSocket readyRead notification.
 * @param none
 * @return none
 * @detail Drains only datagrams confirmed as pending. An empty or stale notification is
 *         counted without issuing an empty read; the receive pump recovers a later native
 *         backlog by performing a successful read that rearms Qt notifications.
 */
void UdpRxWorker::handleReadyRead()
{
    if (!m_initialized || m_udpSocket == nullptr)
    {
        return;
    }

    if (m_testRunning && m_readyReadCalls < std::numeric_limits<quint64>::max())
    {
        ++m_readyReadCalls;
    }

    if (m_workerUptimeTimer.isValid())
    {
        m_lastReadyReadMs = m_workerUptimeTimer.elapsed();
    }
    m_receivePumpFallbackActive = false;

    if (m_readDrainInProgress)
    {
        m_readRequestedDuringDrain = true;
        return;
    }

    const ReadBatchResult result =
        drainPendingDatagrams(ReadTrigger::ReadyRead);
    if (m_testRunning
        && result.datagramsRead == 0
        && result.temporaryEmpty
        && m_emptyReadyReadCalls < std::numeric_limits<quint64>::max())
    {
        ++m_emptyReadyReadCalls;
    }
}
/*-----------------------------------------------------------------------------*/

/**
 * @brief Continues draining a large UDP receive backlog.
 * @param none
 * @return none
 * @detail Uses a separate queued callback instead of calling the real readyRead handler
 *         synthetically, and reads only datagrams confirmed as pending.
 */
void UdpRxWorker::continueReadBatch()
{
    m_readContinuationScheduled = false;

    if (!m_initialized || m_udpSocket == nullptr)
    {
        return;
    }

    if (m_testRunning
        && m_continuationCallbacks < std::numeric_limits<quint64>::max())
    {
        ++m_continuationCallbacks;
    }

    if (m_readDrainInProgress)
    {
        m_readRequestedDuringDrain = true;
        return;
    }

    drainPendingDatagrams(ReadTrigger::Continuation);
}
/*-----------------------------------------------------------------------------*/

/**
 * @brief Checks for a stalled UDP read notification.
 * @param none
 * @return none
 * @detail Recovers queued native datagrams when no successful read has occurred for the
 *         receive-pump threshold, while leaving normal readyRead reception untouched.
 */
void UdpRxWorker::serviceReceivePump()
{
    if (!m_initialized
        || !m_connectionConfigured
        || m_udpSocket == nullptr
        || m_udpSocket->state() != QAbstractSocket::BoundState)
    {
        return;
    }

    if (m_testRunning && m_receivePumpCalls < std::numeric_limits<quint64>::max())
    {
        ++m_receivePumpCalls;
    }

    bool nativeQuerySucceeded = false;
    int nativeError = 0;
    const bool qtPending = m_udpSocket->hasPendingDatagrams();
    const qint64 nativeBytes =
        nativePendingBytes(&nativeQuerySucceeded, &nativeError);

    if (!nativeQuerySucceeded && !m_nativeQueryFailureReported)
    {
        m_nativeQueryFailureReported = true;
        emitWorkerEvent(
            tr("UDP RX diagnostics warning: native FIONREAD query failed; "
               "native_error=%1; Qt pending-datagram polling remains active")
                .arg(nativeError),
            false);
    }

    if (!qtPending && (!nativeQuerySucceeded || nativeBytes <= 0))
    {
        return;
    }

    if (m_readDrainInProgress)
    {
        m_readRequestedDuringDrain = true;
        return;
    }

    const qint64 nowMs = m_workerUptimeTimer.isValid()
                             ? m_workerUptimeTimer.elapsed()
                             : 0;
    const qint64 noReadyReadForMs =
        qMax<qint64>(0, nowMs - m_lastReadyReadMs);
    const bool notificationStalled =
        noReadyReadForMs >= kReceiveNotificationStallThresholdMs;

    const ReadBatchResult result =
        drainPendingDatagrams(ReadTrigger::ReceivePump);
    if (result.datagramsRead <= 0 || !m_testRunning || !notificationStalled)
    {
        return;
    }

    if (m_receivePumpFallbackActive)
    {
        return;
    }
    m_receivePumpFallbackActive = true;

    if (m_receivePumpRecoveries < std::numeric_limits<quint64>::max())
    {
        ++m_receivePumpRecoveries;
    }

    const bool logRecovery =
        m_lastReceivePumpEventMs < 0
        || (nowMs - m_lastReceivePumpEventMs)
               >= kReceivePumpRecoveryLogIntervalMs;
    if (logRecovery)
    {
        emitWorkerEvent(
            tr("UDP RX receive-pump recovery: no_ready_read_for=%1 ms; "
               "native_pending_bytes=%2; qt_pending=%3; received_datagrams=%4; "
               "queued_datagrams=%5; directly_processed=%6; "
               "suppressed_recovery_events=%7; %8")
                .arg(noReadyReadForMs)
                .arg(nativeQuerySucceeded
                         ? QString::number(nativeBytes)
                         : QStringLiteral("unknown"))
                .arg(qtPending ? QStringLiteral("true")
                               : QStringLiteral("false"))
                .arg(result.datagramsRead)
                .arg(result.datagramsQueued)
                .arg(result.datagramsProcessedDirectly)
                .arg(m_suppressedReceivePumpEvents)
                .arg(readDiagnosticsText()),
            false);
        m_lastReceivePumpEventMs = nowMs;
        m_suppressedReceivePumpEvents = 0;
    }
    else if (m_suppressedReceivePumpEvents
             < std::numeric_limits<quint64>::max())
    {
        ++m_suppressedReceivePumpEvents;
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Processes a bounded batch from the preallocated packet ring.
 * @param none
 * @return none
 * @detail Runs independently from socket readyRead handling and reschedules itself while
 *         queued packets remain.
 */
void UdpRxWorker::processPacketQueue()
{
    m_packetProcessingScheduled = false;

    if (!m_initialized || !m_testRunning)
    {
        return;
    }

    processQueuedDatagrams(kProcessingBatchTimeBudgetNs,
                           kMaximumDatagramsPerProcessingBatch,
                           false);
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
        || m_readAttemptInProgress
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
 * @brief Drains one time-bounded batch from the UDP socket.
 * @param trigger Reason why the batch was started.
 * @return ReadBatchResult describing the completed batch.
 * @detail Reuses the persistent receive buffer, avoids empty reads on Qt 5.12/Windows,
 *         and yields to the event loop after the packet or time budget is exhausted.
 */
UdpRxWorker::ReadBatchResult
UdpRxWorker::drainPendingDatagrams(ReadTrigger trigger)
{
    ReadBatchResult result;
    if (!m_initialized
        || m_udpSocket == nullptr
        || m_receiveBuffer.size() != kMaximumIpv4UdpPayloadBytes
        || m_udpSocket->state() != QAbstractSocket::BoundState)
    {
        return result;
    }

    if (m_readDrainInProgress)
    {
        m_readRequestedDuringDrain = true;
        return result;
    }

    m_readDrainInProgress = true;
    QElapsedTimer batchTimer;
    batchTimer.start();
    bool allowNativePumpRead = trigger == ReadTrigger::ReceivePump;

    while (true)
    {
        bool datagramPending = m_udpSocket->hasPendingDatagrams();
        if (!datagramPending && allowNativePumpRead)
        {
            bool nativeQuerySucceeded = false;
            int nativeQueryError = 0;
            const qint64 nativeBytes =
                nativePendingBytes(&nativeQuerySucceeded, &nativeQueryError);
            Q_UNUSED(nativeQueryError);
            datagramPending = nativeQuerySucceeded && nativeBytes > 0;
        }
        allowNativePumpRead = false;

        if (!datagramPending)
        {
            result.temporaryEmpty = true;
            break;
        }

        QHostAddress senderAddress;
        quint16 senderPort = 0;

        m_readAttemptInProgress = true;
        const qint64 bytesRead =
            m_udpSocket->readDatagram(m_receiveBuffer.data(),
                                      m_receiveBuffer.size(),
                                      &senderAddress,
                                      &senderPort);
        const int nativeReadError = bytesRead < 0
                                        ? lastNativeSocketError()
                                        : 0;
        m_readAttemptInProgress = false;

        if (bytesRead < 0)
        {
            const QAbstractSocket::SocketError socketError = m_udpSocket->error();
            if (isTemporaryReadFailure(socketError, nativeReadError))
            {
                result.temporaryEmpty = true;
                break;
            }

            if (m_testRunning && m_readErrors < std::numeric_limits<quint64>::max())
            {
                ++m_readErrors;
            }

            const int errorValue = static_cast<int>(socketError);
            result.fatalError = true;
            m_readDrainInProgress = false;
            m_readRequestedDuringDrain = false;
            handleSocketFailure(
                tr("UDP read error: %1 (Qt code %2; native code %3)")
                    .arg(socketErrorText(errorValue))
                    .arg(errorValue)
                    .arg(nativeReadError));
            return result;
        }

        ++result.datagramsRead;
        const qint64 nowMs = m_workerUptimeTimer.isValid()
                                 ? m_workerUptimeTimer.elapsed()
                                 : 0;
        m_lastDatagramReadMs = nowMs;

        if (m_testRunning)
        {
            if (m_datagramsReadByWorker < std::numeric_limits<quint64>::max())
            {
                ++m_datagramsReadByWorker;
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
                                 sourceKey.isEmpty() ? QStringLiteral("--")
                                                     : sourceKey)
                            .arg(senderPort),
                        true);
                }
            }
            else
            {
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

                if (enqueueDatagram(m_receiveBuffer.constData(),
                                    static_cast<int>(bytesRead),
                                    senderPort))
                {
                    ++result.datagramsQueued;
                    schedulePacketProcessing();
                }
                else
                {
                    processQueuedDatagrams(0,
                                           std::numeric_limits<int>::max(),
                                           true);
                    QElapsedTimer directProcessingTimer;
                    directProcessingTimer.start();
                    processDatagram(m_receiveBuffer.constData(),
                                    static_cast<int>(bytesRead),
                                    senderAddress,
                                    senderPort);
                    recordProcessingWork(directProcessingTimer.nsecsElapsed(), 1);
                    if (m_datagramsProcessed < std::numeric_limits<quint64>::max())
                    {
                        ++m_datagramsProcessed;
                    }
                    if (m_oversizeDirectDatagrams
                        < std::numeric_limits<quint64>::max())
                    {
                        ++m_oversizeDirectDatagrams;
                    }
                    ++result.datagramsProcessedDirectly;
                }
            }
        }

        if (result.datagramsRead >= kMaximumDatagramsPerSocketBatch)
        {
            result.budgetReached = true;
            break;
        }

        if (result.datagramsRead >= kMinimumDatagramsBeforeSocketTimeCheck
            && batchTimer.nsecsElapsed() >= kSocketReadBatchTimeBudgetNs)
        {
            result.budgetReached = true;
            break;
        }
    }

    const qint64 batchElapsedNs = batchTimer.nsecsElapsed();
    if (batchElapsedNs > 0)
    {
        const quint64 safeElapsedNs = static_cast<quint64>(batchElapsedNs);
        if (std::numeric_limits<quint64>::max() - m_totalSocketReadTimeNs
            < safeElapsedNs)
        {
            m_totalSocketReadTimeNs = std::numeric_limits<quint64>::max();
        }
        else
        {
            m_totalSocketReadTimeNs += safeElapsedNs;
        }
        if (batchElapsedNs > m_maximumSocketReadBatchNs)
        {
            m_maximumSocketReadBatchNs = batchElapsedNs;
        }
    }

    if (m_testRunning && result.datagramsRead > m_maximumReadBatch)
    {
        m_maximumReadBatch = result.datagramsRead;
    }

    const bool readRequestedDuringDrain = m_readRequestedDuringDrain;
    m_readRequestedDuringDrain = false;
    m_readDrainInProgress = false;

    if (!result.fatalError
        && (result.budgetReached
            || readRequestedDuringDrain
            || m_udpSocket->hasPendingDatagrams()))
    {
        scheduleReadContinuation();
    }

    return result;
}
/*-----------------------------------------------------------------------------*/

/**
 * @brief Schedules one queued backlog-drain callback.
 * @param none
 * @return none
 * @detail Prevents duplicate callbacks and keeps synthetic continuation work separate
 *         from the real QUdpSocket readyRead handler.
 */
void UdpRxWorker::scheduleReadContinuation()
{
    if (m_readContinuationScheduled
        || !m_initialized
        || m_udpSocket == nullptr
        || m_udpSocket->state() != QAbstractSocket::BoundState)
    {
        return;
    }

    m_readContinuationScheduled = true;
    const bool invoked =
        QMetaObject::invokeMethod(this,
                                  "continueReadBatch",
                                  Qt::QueuedConnection);
    if (!invoked)
    {
        m_readContinuationScheduled = false;
        emitWorkerEvent(
            tr("UDP RX internal error: failed to schedule a receive continuation"),
            true);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Discards UDP datagrams currently queued before START.
 * @param none
 * @return Number of datagrams removed from the receive queue.
 * @detail Reuses the persistent receive buffer and applies both a packet limit and a
 *         monotonic time budget under a continuously arriving stream.
 */
quint64 UdpRxWorker::discardPendingDatagrams()
{
    if (m_udpSocket == nullptr
        || m_receiveBuffer.size() != kMaximumIpv4UdpPayloadBytes
        || m_udpSocket->state() != QAbstractSocket::BoundState)
    {
        return 0;
    }

    if (m_readDrainInProgress)
    {
        m_readRequestedDuringDrain = true;
        return 0;
    }

    m_readDrainInProgress = true;
    quint64 discardedDatagrams = 0;
    QElapsedTimer discardTimer;
    discardTimer.start();

    while (discardedDatagrams
               < static_cast<quint64>(kMaximumDatagramsToDiscardBeforeStart)
           && discardTimer.nsecsElapsed() < kDiscardBeforeStartTimeBudgetNs)
    {
        bool datagramPending = m_udpSocket->hasPendingDatagrams();
        if (!datagramPending)
        {
            bool nativeQuerySucceeded = false;
            int nativeQueryError = 0;
            const qint64 nativeBytes =
                nativePendingBytes(&nativeQuerySucceeded, &nativeQueryError);
            Q_UNUSED(nativeQueryError);
            datagramPending = nativeQuerySucceeded && nativeBytes > 0;
        }

        if (!datagramPending)
        {
            break;
        }

        m_readAttemptInProgress = true;
        const qint64 bytesRead =
            m_udpSocket->readDatagram(m_receiveBuffer.data(),
                                      m_receiveBuffer.size());
        const int nativeReadError = bytesRead < 0
                                        ? lastNativeSocketError()
                                        : 0;
        m_readAttemptInProgress = false;

        if (bytesRead < 0)
        {
            const QAbstractSocket::SocketError socketError = m_udpSocket->error();
            if (isTemporaryReadFailure(socketError, nativeReadError))
            {
                break;
            }

            const int errorValue = static_cast<int>(socketError);
            m_readDrainInProgress = false;
            m_readRequestedDuringDrain = false;
            handleSocketFailure(
                tr("UDP discard error: %1 (Qt code %2; native code %3)")
                    .arg(socketErrorText(errorValue))
                    .arg(errorValue)
                    .arg(nativeReadError));
            return discardedDatagrams;
        }

        ++discardedDatagrams;
        if (m_workerUptimeTimer.isValid())
        {
            m_lastDatagramReadMs = m_workerUptimeTimer.elapsed();
        }
    }

    m_readRequestedDuringDrain = false;
    m_readDrainInProgress = false;
    return discardedDatagrams;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Allocates the preallocated packet ring for a new START operation.
 * @param expectedPayloadBytes Payload size configured in Pattern.
 * @param errorText Output pointer for an allocation or size error.
 * @return true when the packet ring is ready; otherwise false.
 * @detail Uses a bounded memory budget and fixed-size slots so the receive path performs no
 *         per-datagram heap allocation.
 */
bool UdpRxWorker::configurePacketQueue(int expectedPayloadBytes,
                                       QString *errorText)
{
    if (errorText == nullptr)
    {
        return false;
    }

    if (expectedPayloadBytes <= 0
        || expectedPayloadBytes > kMaximumIpv4UdpPayloadBytes)
    {
        *errorText = tr("invalid packet-queue payload size");
        return false;
    }

    const int slotBytes =
        qBound(kMinimumPacketQueueSlotBytes,
               expectedPayloadBytes,
               kMaximumIpv4UdpPayloadBytes);
    const int capacityByMemory =
        qMax(1, kPacketQueueMemoryBudgetBytes / slotBytes);
    const int capacity =
        qBound(kMinimumPacketQueueSlots,
               capacityByMemory,
               kMaximumPacketQueueSlots);
    const qint64 storageBytes =
        static_cast<qint64>(slotBytes) * static_cast<qint64>(capacity);

    if (storageBytes <= 0
        || storageBytes > std::numeric_limits<int>::max())
    {
        *errorText = tr("packet-queue memory size is invalid");
        return false;
    }

    m_packetQueueStorage.resize(static_cast<int>(storageBytes));
    m_packetQueueLengths.resize(capacity);
    m_packetQueueSenderPorts.resize(capacity);

    if (m_packetQueueStorage.size() != storageBytes
        || m_packetQueueLengths.size() != capacity
        || m_packetQueueSenderPorts.size() != capacity)
    {
        m_packetQueueStorage.clear();
        m_packetQueueLengths.clear();
        m_packetQueueSenderPorts.clear();
        m_packetQueueSlotBytes = 0;
        m_packetQueueCapacity = 0;
        *errorText = tr("failed to allocate the UDP packet queue");
        return false;
    }

    m_packetQueueSlotBytes = slotBytes;
    m_packetQueueCapacity = capacity;
    clearPacketQueue();
    errorText->clear();
    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Clears packet-ring indices without releasing preallocated storage.
 * @param none
 * @return none
 * @detail Stops pending processing and resets head, tail, and depth for a new test.
 */
void UdpRxWorker::clearPacketQueue()
{
    if (m_packetProcessingTimer != nullptr)
    {
        m_packetProcessingTimer->stop();
    }

    m_packetProcessingScheduled = false;
    m_packetProcessingInProgress = false;
    m_packetQueueHead = 0;
    m_packetQueueTail = 0;
    m_packetQueueDepth = 0;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Copies one received payload into the packet ring.
 * @param data Pointer to the received payload.
 * @param payloadBytes Number of valid payload bytes.
 * @param senderPort Source UDP port retained for diagnostic messages.
 * @return true when the payload was queued; otherwise false.
 * @detail If the ring is full, the oldest packet is processed synchronously so FIFO order
 *         is preserved and the new packet can be stored without allocation.
 */
bool UdpRxWorker::enqueueDatagram(const char *data,
                                  int payloadBytes,
                                  quint16 senderPort)
{
    if (data == nullptr
        || payloadBytes < 0
        || payloadBytes > m_packetQueueSlotBytes
        || m_packetQueueCapacity <= 0
        || m_packetQueueStorage.isEmpty())
    {
        return false;
    }

    if (m_packetQueueDepth >= m_packetQueueCapacity)
    {
        if (m_queuePressureEvents < std::numeric_limits<quint64>::max())
        {
            ++m_queuePressureEvents;
        }
        reportQueuePressure();

        QElapsedTimer inlineProcessingTimer;
        inlineProcessingTimer.start();
        const bool processedOldest = processOneQueuedDatagram();
        recordProcessingWork(inlineProcessingTimer.nsecsElapsed(),
                             processedOldest ? 1 : 0);
        if (!processedOldest)
        {
            return false;
        }
    }

    char *destination =
        m_packetQueueStorage.data()
        + (m_packetQueueTail * m_packetQueueSlotBytes);
    if (payloadBytes > 0)
    {
        std::memcpy(destination,
                    data,
                    static_cast<std::size_t>(payloadBytes));
    }
    m_packetQueueLengths[m_packetQueueTail] =
        static_cast<quint32>(payloadBytes);
    m_packetQueueSenderPorts[m_packetQueueTail] = senderPort;
    m_packetQueueTail = (m_packetQueueTail + 1) % m_packetQueueCapacity;
    ++m_packetQueueDepth;

    if (m_packetQueueDepth > m_maximumPacketQueueDepth)
    {
        m_maximumPacketQueueDepth = m_packetQueueDepth;
    }

    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Schedules packet-ring processing through a zero-delay timer.
 * @param none
 * @return none
 * @detail Avoids duplicate scheduling while allowing socket receive callbacks to return
 *         quickly to the worker event loop.
 */
void UdpRxWorker::schedulePacketProcessing()
{
    if (m_packetProcessingScheduled
        || m_packetProcessingTimer == nullptr
        || !m_testRunning
        || m_packetQueueDepth <= 0)
    {
        return;
    }

    m_packetProcessingScheduled = true;
    m_packetProcessingTimer->start(0);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Processes packets already stored in the packet ring.
 * @param timeBudgetNs Maximum processing time; ignored when drainCompletely is true.
 * @param maximumDatagrams Maximum packet count; ignored when drainCompletely is true.
 * @param drainCompletely true to empty the ring before returning.
 * @return Number of processed packets.
 * @detail Preserves FIFO order and records processing time, callback count, maximum batch,
 *         and queue-depth diagnostics.
 */
int UdpRxWorker::processQueuedDatagrams(qint64 timeBudgetNs,
                                        int maximumDatagrams,
                                        bool drainCompletely)
{
    if (m_packetProcessingInProgress
        || m_packetQueueDepth <= 0
        || m_packetQueueCapacity <= 0)
    {
        return 0;
    }

    m_packetProcessingInProgress = true;
    if (m_testRunning
        && m_processingCallbacks < std::numeric_limits<quint64>::max())
    {
        ++m_processingCallbacks;
    }

    QElapsedTimer processingTimer;
    processingTimer.start();
    int processedDatagrams = 0;

    while (m_packetQueueDepth > 0)
    {
        if (!processOneQueuedDatagram())
        {
            break;
        }
        ++processedDatagrams;

        if (!drainCompletely)
        {
            if (processedDatagrams >= maximumDatagrams)
            {
                break;
            }
            if (processedDatagrams >= kMinimumDatagramsBeforeProcessingTimeCheck
                && processingTimer.nsecsElapsed() >= timeBudgetNs)
            {
                break;
            }
        }
    }

    recordProcessingWork(processingTimer.nsecsElapsed(), processedDatagrams);

    m_packetProcessingInProgress = false;

    if (!drainCompletely && m_packetQueueDepth > 0)
    {
        schedulePacketProcessing();
    }

    return processedDatagrams;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Adds one packet-verification work item to processing diagnostics.
 * @param elapsedNs Time spent in the processing work item in nanoseconds.
 * @param processedDatagrams Number of packets verified by the work item.
 * @return none
 * @detail Accounts scheduled batches and FIFO-preserving inline processing through the
 *         same saturation-safe counters.
 */
void UdpRxWorker::recordProcessingWork(qint64 elapsedNs,
                                       int processedDatagrams)
{
    if (elapsedNs > 0)
    {
        const quint64 safeElapsedNs = static_cast<quint64>(elapsedNs);
        if (std::numeric_limits<quint64>::max() - m_totalProcessingTimeNs
            < safeElapsedNs)
        {
            m_totalProcessingTimeNs = std::numeric_limits<quint64>::max();
        }
        else
        {
            m_totalProcessingTimeNs += safeElapsedNs;
        }

        if (elapsedNs > m_maximumProcessingBatchNs)
        {
            m_maximumProcessingBatchNs = elapsedNs;
        }
    }

    if (processedDatagrams > m_maximumProcessingBatch)
    {
        m_maximumProcessingBatch = processedDatagrams;
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Processes and removes one oldest packet from the ring.
 * @param none
 * @return true when one packet was processed; otherwise false.
 * @detail The payload remains in preallocated storage and is never copied into a temporary
 *         QByteArray.
 */
bool UdpRxWorker::processOneQueuedDatagram()
{
    if (m_packetQueueDepth <= 0
        || m_packetQueueCapacity <= 0
        || m_packetQueueHead < 0
        || m_packetQueueHead >= m_packetQueueCapacity)
    {
        return false;
    }

    const int payloadBytes =
        static_cast<int>(m_packetQueueLengths[m_packetQueueHead]);
    const quint16 senderPort =
        m_packetQueueSenderPorts[m_packetQueueHead];
    const char *payload =
        m_packetQueueStorage.constData()
        + (m_packetQueueHead * m_packetQueueSlotBytes);

    processDatagram(payload,
                    payloadBytes,
                    m_expectedSourceAddress,
                    senderPort);

    m_packetQueueHead = (m_packetQueueHead + 1) % m_packetQueueCapacity;
    --m_packetQueueDepth;
    if (m_datagramsProcessed < std::numeric_limits<quint64>::max())
    {
        ++m_datagramsProcessed;
    }
    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Reports sustained pressure on the internal packet ring.
 * @param none
 * @return none
 * @detail Rate-limits service messages to avoid adding GUI or disk load during a dense UDP
 *         stream.
 */
void UdpRxWorker::reportQueuePressure()
{
    if (!m_testRunning || !m_workerUptimeTimer.isValid())
    {
        return;
    }

    const qint64 nowMs = m_workerUptimeTimer.elapsed();
    if (m_lastQueuePressureEventMs >= 0
        && (nowMs - m_lastQueuePressureEventMs) < kQueuePressureLogIntervalMs)
    {
        return;
    }

    m_lastQueuePressureEventMs = nowMs;
    emitWorkerEvent(
        tr("UDP RX packet-queue pressure: depth=%1; capacity=%2; "
           "oldest packet processed inline; queue_pressure_events=%3")
            .arg(m_packetQueueDepth)
            .arg(m_packetQueueCapacity)
            .arg(m_queuePressureEvents),
        false);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Processes one accepted UDP payload.
 * @param data Pointer to the first payload byte in the persistent receive buffer.
 * @param payloadBytes Number of valid payload bytes.
 * @param senderAddress Source IPv4 address.
 * @param senderPort Source UDP port.
 * @return none
 * @detail Decodes complete little-endian counter fields without per-packet allocation or
 *         payload copying and preserves sequence continuity across packet boundaries.
 */
void UdpRxWorker::processDatagram(const char *data,
                                  int payloadBytes,
                                  const QHostAddress &senderAddress,
                                  quint16 senderPort)
{
    const int counterBytes = m_activePattern.counterBytes;
    if (data == nullptr || payloadBytes < 0 || counterBytes <= 0)
    {
        return;
    }

    const int completeBytes =
        (payloadBytes / counterBytes) * counterBytes;
    const int trailingBytes = payloadBytes - completeBytes;
    if (trailingBytes > 0)
    {
        emitWorkerEvent(
            tr("UDP payload alignment error: source=%1:%2; payload=%3 bytes; "
               "counter_field=%4 bytes; trailing=%5 bytes")
                .arg(senderAddress.toString())
                .arg(senderPort)
                .arg(payloadBytes)
                .arg(counterBytes)
                .arg(trailingBytes),
            true);
    }

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
 * @brief Queries the number of bytes waiting in the native socket receive queue.
 * @param succeeded Output flag set true when the native query completed successfully.
 * @param nativeError Output operating-system error code when the query fails.
 * @return Pending native bytes, or -1 when the query fails.
 * @detail Uses FIONREAD only from the worker thread that owns the QUdpSocket descriptor.
 */
qint64 UdpRxWorker::nativePendingBytes(bool *succeeded, int *nativeError) const
{
    if (succeeded != nullptr)
    {
        *succeeded = false;
    }
    if (nativeError != nullptr)
    {
        *nativeError = 0;
    }

    if (m_udpSocket == nullptr || m_udpSocket->socketDescriptor() < 0)
    {
        return -1;
    }

#if defined(_WIN32)
    u_long availableBytes = 0;
    const SOCKET nativeSocket =
        static_cast<SOCKET>(m_udpSocket->socketDescriptor());
    if (::ioctlsocket(nativeSocket, FIONREAD, &availableBytes) == SOCKET_ERROR)
    {
        if (nativeError != nullptr)
        {
            *nativeError = ::WSAGetLastError();
        }
        return -1;
    }
#else
    int availableBytes = 0;
    if (::ioctl(static_cast<int>(m_udpSocket->socketDescriptor()),
                FIONREAD,
                &availableBytes) != 0)
    {
        if (nativeError != nullptr)
        {
            *nativeError = errno;
        }
        return -1;
    }
#endif

    if (succeeded != nullptr)
    {
        *succeeded = true;
    }
    return static_cast<qint64>(availableBytes);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns the most recent native socket error for the current thread.
 * @param none
 * @return WSA error on Windows or errno on Unix-like systems.
 * @detail The value is captured immediately after a failed readDatagram() call.
 */
int UdpRxWorker::lastNativeSocketError() const
{
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Classifies a failed nonblocking UDP read as temporary.
 * @param socketError Error reported by QUdpSocket.
 * @param nativeError Native WSA or errno value captured immediately after failure.
 * @return true for would-block, interrupted, or timeout conditions; otherwise false.
 * @detail Qt 5.12/Windows can map WSAEWOULDBLOCK from readDatagram() to NetworkError,
 *         therefore the native code is authoritative for an empty nonblocking read.
 */
bool UdpRxWorker::isTemporaryReadFailure(
    QAbstractSocket::SocketError socketError,
    int nativeError) const
{
    if (socketError == QAbstractSocket::TemporaryError
        || socketError == QAbstractSocket::SocketTimeoutError)
    {
        return true;
    }

#if defined(_WIN32)
    return nativeError == WSAEWOULDBLOCK || nativeError == WSAEINTR;
#else
    return nativeError == EAGAIN
           || nativeError == EWOULDBLOCK
           || nativeError == EINTR;
#endif
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Resets per-test UDP read-path diagnostics.
 * @param none
 * @return none
 * @detail Clears notification, continuation, receive-pump, queue, processing-time, and read-error, datagram, and maximum
 *         batch counters and restarts activity-gap measurement.
 */
void UdpRxWorker::resetReadDiagnostics()
{
    m_readyReadCalls = 0;
    m_emptyReadyReadCalls = 0;
    m_continuationCallbacks = 0;
    m_receivePumpCalls = 0;
    m_receivePumpRecoveries = 0;
    m_suppressedReceivePumpEvents = 0;
    m_readErrors = 0;
    m_datagramsReadByWorker = 0;
    m_datagramsProcessed = 0;
    m_processingCallbacks = 0;
    m_queuePressureEvents = 0;
    m_queueDroppedDatagrams = 0;
    m_oversizeDirectDatagrams = 0;
    m_totalSocketReadTimeNs = 0;
    m_totalProcessingTimeNs = 0;
    m_lastStatisticsProcessingTimeNs = 0;
    m_maximumReadBatch = 0;
    m_maximumProcessingBatch = 0;
    m_maximumSocketReadBatchNs = 0;
    m_maximumProcessingBatchNs = 0;
    m_lastProcessingLoadPercentTimes1000 = 0;
    m_maximumPacketQueueDepth = m_packetQueueDepth;

    const qint64 nowMs = m_workerUptimeTimer.isValid()
                             ? m_workerUptimeTimer.elapsed()
                             : 0;
    m_lastReadyReadMs = nowMs;
    m_lastDatagramReadMs = nowMs;
    m_lastReceivePumpEventMs = -1;
    m_lastQueuePressureEventMs = -1;
    m_receivePumpFallbackActive = false;
}
/*-----------------------------------------------------------------------------*/

/**
 * @brief Formats current UDP read-path diagnostics.
 * @param none
 * @return Semicolon-separated diagnostic fields for service logs.
 * @detail Includes notification counts, receive-pump recoveries, read errors, maximum batch,
 *         and current activity gaps measured by the worker monotonic clock.
 */
QString UdpRxWorker::readDiagnosticsText() const
{
    const qint64 nowMs = m_workerUptimeTimer.isValid()
                             ? m_workerUptimeTimer.elapsed()
                             : 0;
    const qint64 readyReadGapMs = qMax<qint64>(0, nowMs - m_lastReadyReadMs);
    const qint64 datagramGapMs =
        qMax<qint64>(0, nowMs - m_lastDatagramReadMs);
    const double processingLoadPercent =
        static_cast<double>(m_lastProcessingLoadPercentTimes1000) / 1000.0;
    bool nativeQuerySucceeded = false;
    int nativeQueryError = 0;
    const qint64 nativeBytes =
        nativePendingBytes(&nativeQuerySucceeded, &nativeQueryError);
    Q_UNUSED(nativeQueryError);
    const bool qtPending = m_udpSocket != nullptr
                           && m_udpSocket->hasPendingDatagrams();

    return QStringLiteral(
               "ready_read_calls=%1; empty_ready_read_calls=%2; "
               "read_continuations=%3; receive_pump_calls=%4; "
               "receive_pump_recoveries=%5; suppressed_receive_pump_events=%6; "
               "read_errors=%7; worker_datagrams=%8; processed_datagrams=%9; "
               "processing_callbacks=%10; queue_depth=%11; max_queue_depth=%12; "
               "queue_capacity=%13; queue_pressure_events=%14; queue_drops=%15; "
               "oversize_direct=%16; max_read_batch=%17; max_processing_batch=%18; "
               "socket_read_time_us=%19; processing_time_us=%20; "
               "max_socket_batch_us=%21; max_processing_batch_us=%22; "
               "processing_load_pct=%23; ready_read_gap_ms=%24; datagram_gap_ms=%25; "
               "native_pending_bytes=%26; qt_pending=%27")
        .arg(m_readyReadCalls)
        .arg(m_emptyReadyReadCalls)
        .arg(m_continuationCallbacks)
        .arg(m_receivePumpCalls)
        .arg(m_receivePumpRecoveries)
        .arg(m_suppressedReceivePumpEvents)
        .arg(m_readErrors)
        .arg(m_datagramsReadByWorker)
        .arg(m_datagramsProcessed)
        .arg(m_processingCallbacks)
        .arg(m_packetQueueDepth)
        .arg(m_maximumPacketQueueDepth)
        .arg(m_packetQueueCapacity)
        .arg(m_queuePressureEvents)
        .arg(m_queueDroppedDatagrams)
        .arg(m_oversizeDirectDatagrams)
        .arg(m_maximumReadBatch)
        .arg(m_maximumProcessingBatch)
        .arg(m_totalSocketReadTimeNs / 1000U)
        .arg(m_totalProcessingTimeNs / 1000U)
        .arg(m_maximumSocketReadBatchNs / 1000)
        .arg(m_maximumProcessingBatchNs / 1000)
        .arg(formatRate(processingLoadPercent))
        .arg(readyReadGapMs)
        .arg(datagramGapMs)
        .arg(nativeQuerySucceeded ? QString::number(nativeBytes)
                                  : QStringLiteral("unknown"))
        .arg(qtPending ? QStringLiteral("true")
                       : QStringLiteral("false"));
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

    const unsigned char *bytes =
        reinterpret_cast<const unsigned char *>(data);
    switch (byteCount)
    {
    case 1:
        return static_cast<quint64>(bytes[0]);

    case 2:
        return static_cast<quint64>(bytes[0])
               | (static_cast<quint64>(bytes[1]) << 8);

    case 4:
        return static_cast<quint64>(bytes[0])
               | (static_cast<quint64>(bytes[1]) << 8)
               | (static_cast<quint64>(bytes[2]) << 16)
               | (static_cast<quint64>(bytes[3]) << 24);

    case 8:
        return static_cast<quint64>(bytes[0])
               | (static_cast<quint64>(bytes[1]) << 8)
               | (static_cast<quint64>(bytes[2]) << 16)
               | (static_cast<quint64>(bytes[3]) << 24)
               | (static_cast<quint64>(bytes[4]) << 32)
               | (static_cast<quint64>(bytes[5]) << 40)
               | (static_cast<quint64>(bytes[6]) << 48)
               | (static_cast<quint64>(bytes[7]) << 56);

    default:
        break;
    }

    quint64 value = 0;
    for (int byteIndex = 0; byteIndex < byteCount; ++byteIndex)
    {
        value |= static_cast<quint64>(bytes[byteIndex])
                 << (byteIndex * 8);
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
    const quint64 deltaProcessingTimeNs =
        m_totalProcessingTimeNs >= m_lastStatisticsProcessingTimeNs
            ? m_totalProcessingTimeNs - m_lastStatisticsProcessingTimeNs
            : 0;

    double speedKbps = 0.0;
    double packetsPerSecond = 0.0;
    double processingLoadPercent = 0.0;
    if (deltaMilliseconds > 0)
    {
        speedKbps =
            (static_cast<double>(deltaBytes) * 8.0)
            / static_cast<double>(deltaMilliseconds);
        packetsPerSecond =
            (static_cast<double>(deltaPackets) * 1000.0)
            / static_cast<double>(deltaMilliseconds);
        processingLoadPercent =
            (static_cast<double>(deltaProcessingTimeNs) * 100.0)
            / (static_cast<double>(deltaMilliseconds) * 1000000.0);
    }

    m_lastStatisticsBytes = m_totalPayloadBytes;
    m_lastStatisticsPackets = m_totalPackets;
    m_lastStatisticsElapsedMs = elapsedMilliseconds;
    m_lastStatisticsProcessingTimeNs = m_totalProcessingTimeNs;
    m_lastProcessingLoadPercentTimes1000 =
        static_cast<qint64>(processingLoadPercent * 1000.0 + 0.5);

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
                       "min_speed_Kb/s=%13, avrg_speed_Kb/s=%14, max_speed_Kb/s=%15, "
                       "%16")
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
            .arg(formatRate(m_periodicMaximumSpeedKbps))
            .arg(readDiagnosticsText());

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
    if (m_packetProcessingTimer != nullptr)
    {
        m_packetProcessingTimer->stop();
    }

    m_packetProcessingScheduled = false;
    m_testRunning = false;
    m_reportedUnexpectedSources.clear();
    clearPacketQueue();
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
        if (m_packetProcessingTimer != nullptr)
        {
            m_packetProcessingTimer->stop();
        }
        m_packetProcessingScheduled = false;
        processQueuedDatagrams(0,
                               std::numeric_limits<int>::max(),
                               true);
        completeReason +=
            tr("; reception stopped; received %1 payload bytes in %2 packets; "
               "counter ok=%3; counter err=%4; %5")
                .arg(m_totalPayloadBytes)
                .arg(m_totalPackets)
                .arg(m_counterOk)
                .arg(m_counterErrors)
                .arg(readDiagnosticsText());
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

    if (m_receivePumpTimer != nullptr)
    {
        m_receivePumpTimer->stop();
    }
    if (m_packetProcessingTimer != nullptr)
    {
        m_packetProcessingTimer->stop();
    }

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
    m_actualReceiveBufferBytes = 0;
    m_readContinuationScheduled = false;
    m_packetProcessingScheduled = false;
    m_receivePumpFallbackActive = false;
    m_readDrainInProgress = false;
    m_packetProcessingInProgress = false;
    m_readRequestedDuringDrain = false;
    m_readAttemptInProgress = false;
    m_nativeQueryFailureReported = false;
    m_reportedUnexpectedSources.clear();
    clearPacketQueue();
    m_packetQueueStorage.clear();
    m_packetQueueLengths.clear();
    m_packetQueueSenderPorts.clear();
    m_packetQueueSlotBytes = 0;
    m_packetQueueCapacity = 0;

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
