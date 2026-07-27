#include "udptxworker.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QTimer>
#include <QUdpSocket>

#include <limits>
#include <new>

namespace
{
constexpr int kStatisticsIntervalMs = 1000;
constexpr qint64 kPeriodicLogIntervalMs = 20 * 1000;
constexpr int kMaximumIpv4UdpPayloadBytes = 65507;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates the UDP transmitter worker object.
 * @param parent Parent QObject; normally omitted before moveToThread().
 * @return none
 * @detail Initializes scalar state only; QUdpSocket and timers are created later by
 *         initialize() in the worker thread.
 */
UdpTxWorker::UdpTxWorker(QObject *parent)
    : QObject(parent)
    , m_udpSocket(nullptr)
    , m_transmitTimer(nullptr)
    , m_statisticsTimer(nullptr)
    , m_destinationPort(0)
    , m_localPort(0)
    , m_currentCounter(0)
    , m_totalPayloadBytes(0)
    , m_totalPackets(0)
    , m_lastStatisticsBytes(0)
    , m_lastStatisticsPackets(0)
    , m_lastStatisticsElapsedMs(0)
    , m_lastPeriodicLogBytes(0)
    , m_lastPeriodicLogPackets(0)
    , m_lastPeriodicLogElapsedMs(0)
    , m_periodicMinimumSpeedKbps(0.0)
    , m_periodicMaximumSpeedKbps(0.0)
    , m_periodicMinimumPacketsPerSecond(0.0)
    , m_periodicMaximumPacketsPerSecond(0.0)
    , m_initialized(false)
    , m_connectionConfigured(false)
    , m_testRunning(false)
    , m_singleTransferActive(false)
    , m_collectStatistics(false)
    , m_periodicLogStatisticsActive(false)
    , m_periodicSampleAvailable(false)
    , m_socketOperationInProgress(false)
    , m_shuttingDown(false)
{
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Destroys the UDP transmitter worker object.
 * @param none
 * @return none
 * @detail Stops timers and closes the UDP socket as a safeguard after normal shutdown.
 */
UdpTxWorker::~UdpTxWorker()
{
    if (m_transmitTimer != nullptr)
    {
        m_transmitTimer->stop();
    }

    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }

    if (m_udpSocket != nullptr)
    {
        m_udpSocket->close();
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Initializes resources owned by the UDP worker thread.
 * @param none
 * @return none
 * @detail Creates QUdpSocket and timers with UdpTxWorker as parent, connects their
 *         signals, and reports readiness to the GUI.
 */
void UdpTxWorker::initialize()
{
    if (m_initialized)
    {
        emit workerReady();
        return;
    }

    m_udpSocket = new QUdpSocket(this);
    m_transmitTimer = new QTimer(this);
    m_statisticsTimer = new QTimer(this);

    m_transmitTimer->setSingleShot(false);
    m_transmitTimer->setTimerType(Qt::PreciseTimer);

    m_statisticsTimer->setInterval(kStatisticsIntervalMs);
    m_statisticsTimer->setSingleShot(false);
    m_statisticsTimer->setTimerType(Qt::PreciseTimer);

    connect(m_transmitTimer,
            &QTimer::timeout,
            this,
            &UdpTxWorker::sendNextBurst);
    connect(m_statisticsTimer,
            &QTimer::timeout,
            this,
            &UdpTxWorker::updateStatistics);
    connect(m_udpSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QUdpSocket::error),
            this,
            [this](QAbstractSocket::SocketError socketError)
            {
                handleSocketError(static_cast<int>(socketError));
            });

    m_initialized = true;
    emit transmissionStateChanged(false, false);
    emit connectionStateChanged(false,
                                QString(),
                                0,
                                QString(),
                                0,
                                false);
    emit workerReady();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates and binds the persistent UDP sending socket.
 * @param destinationIp Validated destination IPv4 address.
 * @param destinationPort Validated destination UDP port.
 * @param localIp Local IPv4 address selected by the operating-system route.
 * @return none
 * @detail Binds an ephemeral local port to the selected interface and stores the
 *         destination for later writeDatagram() calls.
 */
void UdpTxWorker::configureConnection(const QString &destinationIp,
                                      quint16 destinationPort,
                                      const QString &localIp)
{
    if (!m_initialized || m_udpSocket == nullptr)
    {
        emitWorkerEvent(tr("UDP Connection error: the UDP worker is not initialized"),
                        true);
        emit connectionStateChanged(false,
                                    destinationIp,
                                    destinationPort,
                                    localIp,
                                    0,
                                    true);
        return;
    }

    if (m_testRunning || m_singleTransferActive)
    {
        emitWorkerEvent(tr("UDP Connection error: transmission is active"), true);
        emit connectionStateChanged(m_connectionConfigured,
                                    m_destinationIp,
                                    m_destinationPort,
                                    m_localIp,
                                    m_localPort,
                                    false);
        return;
    }

    QHostAddress destinationAddress;
    QHostAddress localAddress;
    if (!destinationAddress.setAddress(destinationIp)
        || destinationAddress.protocol() != QAbstractSocket::IPv4Protocol
        || !localAddress.setAddress(localIp)
        || localAddress.protocol() != QAbstractSocket::IPv4Protocol
        || destinationPort == 0)
    {
        emitWorkerEvent(tr("UDP Connection error: invalid IPv4 endpoint data"), true);
        emit connectionStateChanged(false,
                                    destinationIp,
                                    destinationPort,
                                    localIp,
                                    0,
                                    true);
        return;
    }

    if (m_udpSocket->state() != QAbstractSocket::UnconnectedState)
    {
        m_udpSocket->close();
    }

    m_socketOperationInProgress = true;
    const bool bound = m_udpSocket->bind(localAddress,
                                        0,
                                        QAbstractSocket::DontShareAddress);
    m_socketOperationInProgress = false;

    if (!bound)
    {
        const int errorValue = static_cast<int>(m_udpSocket->error());
        emitWorkerEvent(tr("UDP Connection error: failed to bind %1 to an ephemeral port: %2 (code %3)")
                            .arg(localIp,
                                 socketErrorText(errorValue))
                            .arg(errorValue),
                        true);
        m_udpSocket->close();
        emit connectionStateChanged(false,
                                    destinationIp,
                                    destinationPort,
                                    localIp,
                                    0,
                                    true);
        return;
    }

    m_destinationAddress = destinationAddress;
    m_destinationIp = destinationIp;
    m_destinationPort = destinationPort;
    m_localIp = localIp;
    m_localPort = m_udpSocket->localPort();
    m_connectionConfigured = true;

    emit connectionStateChanged(true,
                                m_destinationIp,
                                m_destinationPort,
                                m_localIp,
                                m_localPort,
                                false);
    emitWorkerEvent(tr("UDP socket ready: local=%1:%2; destination=%3:%4")
                        .arg(m_localIp)
                        .arg(m_localPort)
                        .arg(m_destinationIp)
                        .arg(m_destinationPort),
                    false);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Closes the persistent UDP sending socket.
 * @param none
 * @return none
 * @detail Requires UDP transmission to be idle and releases the ephemeral local port
 *         reserved during CONNECT.
 */
void UdpTxWorker::disconnectConnection()
{
    if (!m_initialized || m_udpSocket == nullptr)
    {
        emit connectionStateChanged(false,
                                    QString(),
                                    0,
                                    QString(),
                                    0,
                                    false);
        return;
    }

    if (m_testRunning || m_singleTransferActive)
    {
        emitWorkerEvent(tr("DISCONNECT failed: UDP transmission is active"), true);
        emit connectionStateChanged(m_connectionConfigured,
                                    m_destinationIp,
                                    m_destinationPort,
                                    m_localIp,
                                    m_localPort,
                                    false);
        return;
    }

    closeSocket(false, true);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Starts continuous transmission of UDP bursts.
 * @param counterBits Counter width: 8, 16, 32, or 64 bits.
 * @param blockBytes Payload size of one UDP datagram in bytes.
 * @param togetherCount Number of datagrams generated in each timer burst.
 * @param periodMs Delay between burst timer events in milliseconds.
 * @param initialValue Initial counter value.
 * @param patternDescription Preformatted Pattern description for the log.
 * @return none
 * @detail Queues the first burst immediately, then continues according to Period;
 *         Period zero uses cooperative zero-timeout timer events.
 */
void UdpTxWorker::startContinuous(int counterBits,
                                  int blockBytes,
                                  int togetherCount,
                                  int periodMs,
                                  quint64 initialValue,
                                  const QString &patternDescription)
{
    if (!m_initialized
        || m_udpSocket == nullptr
        || !m_connectionConfigured
        || m_udpSocket->state() == QAbstractSocket::UnconnectedState)
    {
        emitWorkerEvent(tr("UDP START failed: the UDP destination is not connected"),
                        true);
        emitTransmissionState();
        return;
    }

    if (m_testRunning || m_singleTransferActive)
    {
        emitWorkerEvent(tr("UDP START failed: the previous transmission has not finished yet"),
                        true);
        emitTransmissionState();
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
        emitTransmissionState();
        return;
    }

    m_activePattern = settings;
    m_testRunning = true;
    m_singleTransferActive = false;
    m_collectStatistics = true;
    m_periodicLogStatisticsActive = true;
    resetStatistics(settings);
    emitTransmissionState();

    emitWorkerEvent(tr("UDP START: continuous transmission started; %1")
                        .arg(patternDescription),
                    false);

    if (!sendBurst(m_activePattern))
    {
        stopTransmissionInternal(
            tr("UDP continuous transmission stopped because the first burst failed"),
            true,
            false);
        return;
    }

    m_transmitTimer->setSingleShot(m_activePattern.periodMs == 0);
    m_transmitTimer->setInterval(m_activePattern.periodMs);
    m_transmitTimer->start();
    m_statisticsTimer->start();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Stops continuous UDP packet generation.
 * @param none
 * @return none
 * @detail Stops the burst timer immediately; UDP provides no portable physical-NIC
 *         completion acknowledgement for already accepted datagrams.
 */
void UdpTxWorker::stopContinuous()
{
    if (!m_testRunning)
    {
        emitTransmissionState();
        return;
    }

    m_transmitTimer->stop();
    updateStatisticsSnapshot(false);
    m_statisticsTimer->stop();
    m_testRunning = false;
    m_collectStatistics = false;
    m_periodicLogStatisticsActive = false;
    emitTransmissionState();

    emitWorkerEvent(tr("UDP STOP: packet generation stopped; %1 payload bytes and %2 packets were accepted by the socket")
                        .arg(m_totalPayloadBytes)
                        .arg(m_totalPackets),
                    false);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Transmits one UDP test-pattern datagram.
 * @param counterBits Counter width: 8, 16, 32, or 64 bits.
 * @param blockBytes Payload size of the datagram in bytes.
 * @param togetherCount Current Togeth value included in the Pattern description.
 * @param periodMs Current Period value included in the Pattern description.
 * @param initialValue Initial counter value.
 * @param patternDescription Preformatted Pattern description for the log.
 * @return none
 * @detail SINGLE sends exactly one datagram; Togeth applies only to continuous START
 *         bursts.
 */
void UdpTxWorker::sendSingle(int counterBits,
                             int blockBytes,
                             int togetherCount,
                             int periodMs,
                             quint64 initialValue,
                             const QString &patternDescription)
{
    if (!m_initialized
        || m_udpSocket == nullptr
        || !m_connectionConfigured
        || m_udpSocket->state() == QAbstractSocket::UnconnectedState)
    {
        emitWorkerEvent(tr("UDP SINGLE failed: the UDP destination is not connected"),
                        true);
        emitTransmissionState();
        return;
    }

    if (m_testRunning || m_singleTransferActive)
    {
        emitWorkerEvent(tr("UDP SINGLE failed: the previous transmission has not finished yet"),
                        true);
        emitTransmissionState();
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
        emitTransmissionState();
        return;
    }

    m_activePattern = settings;
    m_testRunning = false;
    m_singleTransferActive = true;
    m_collectStatistics = true;
    m_periodicLogStatisticsActive = false;
    resetStatistics(settings);
    emitTransmissionState();

    emitWorkerEvent(tr("UDP SINGLE: one datagram is being sent; %1")
                        .arg(patternDescription),
                    false);

    if (!sendOneDatagram(settings))
    {
        m_singleTransferActive = false;
        m_collectStatistics = false;
        emitTransmissionState();
        return;
    }

    updateStatisticsSnapshot(false);
    m_singleTransferActive = false;
    m_collectStatistics = false;
    emitTransmissionState();

    emitWorkerEvent(tr("UDP SINGLE: one datagram was accepted by the socket, %1 payload bytes")
                        .arg(m_totalPayloadBytes),
                    false);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Shuts down the UDP worker before application exit.
 * @param none
 * @return none
 * @detail Stops timers, closes the socket, and leaves the worker ready for
 *         QThread::quit().
 */
void UdpTxWorker::shutdown()
{
    if (m_shuttingDown)
    {
        return;
    }

    m_shuttingDown = true;

    if (m_transmitTimer != nullptr)
    {
        m_transmitTimer->stop();
    }

    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }

    m_testRunning = false;
    m_singleTransferActive = false;
    m_collectStatistics = false;
    m_periodicLogStatisticsActive = false;
    emitTransmissionState();

    if (m_connectionConfigured || (m_udpSocket != nullptr && m_udpSocket->isOpen()))
    {
        closeSocket(false, false);
        emitWorkerEvent(tr("UDP socket closed during application shutdown"), false);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Generates and sends the next continuous UDP burst.
 * @param none
 * @return none
 * @detail Sends Togeth datagrams sequentially in one worker-thread callback and
 *         reschedules the cooperative zero-period mode when required.
 */
void UdpTxWorker::sendNextBurst()
{
    if (!m_testRunning)
    {
        return;
    }

    if (!sendBurst(m_activePattern))
    {
        stopTransmissionInternal(
            tr("UDP continuous transmission stopped because a burst failed"),
            true,
            false);
        return;
    }

    if (m_testRunning && m_activePattern.periodMs == 0)
    {
        m_transmitTimer->start(0);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Performs the one-second UDP Statistics update.
 * @param none
 * @return none
 * @detail Calculates payload speed and packets per second from real elapsed time and
 *         emits a prepared snapshot.
 */
void UdpTxWorker::updateStatistics()
{
    updateStatisticsSnapshot(true);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles an asynchronous UDP socket error.
 * @param socketError Numeric QAbstractSocket::SocketError value.
 * @return none
 * @detail Stops active generation and closes the logical UDP connection for fatal
 *         errors not already handled by a synchronous socket call.
 */
void UdpTxWorker::handleSocketError(int socketError)
{
    if (m_shuttingDown
        || m_socketOperationInProgress
        || socketError == static_cast<int>(QAbstractSocket::UnknownSocketError))
    {
        return;
    }

    const QString message =
        tr("UDP socket error: %1 (code %2)")
            .arg(socketErrorText(socketError))
            .arg(socketError);

    if (m_testRunning || m_singleTransferActive || m_connectionConfigured)
    {
        stopTransmissionInternal(message, true, true);
    }
    else
    {
        emitWorkerEvent(message, true);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Validates UDP Pattern arguments and builds worker settings.
 * @param counterBits Counter bit width.
 * @param blockBytes UDP payload size in bytes.
 * @param togetherCount Number of datagrams per burst.
 * @param periodMs Burst period in milliseconds.
 * @param initialValue Initial counter value.
 * @param settings Output pointer for validated settings.
 * @param errorText Output pointer for a validation error message.
 * @return true when all parameters are valid; otherwise false.
 * @detail Checks supported widths, UDP payload limits, alignment, positive Togeth,
 *         Period range, and initial-value range.
 */
bool UdpTxWorker::makePatternSettings(int counterBits,
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
        *errorText = tr("block, bytes must be 1...65507 and a multiple of %1 bytes")
                         .arg(counterBytes);
        return false;
    }

    if (togetherCount <= 0)
    {
        *errorText = tr("Togeth must be a positive decimal number");
        return false;
    }

    if (periodMs < 0)
    {
        *errorText = tr("Period, ms is outside the valid range");
        return false;
    }

    quint64 maximumCounterValue = 0;
    if (counterBits == 64)
    {
        maximumCounterValue = std::numeric_limits<quint64>::max();
    }
    else
    {
        maximumCounterValue = (quint64(1) << counterBits) - quint64(1);
    }

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
 * @brief Resets UDP Statistics for a new START or SINGLE operation.
 * @param settings Validated Pattern settings.
 * @return none
 * @detail Initializes the counter, timestamps, byte and packet baselines, and
 *         twenty-second extrema.
 */
void UdpTxWorker::resetStatistics(const PatternSettings &settings)
{
    m_currentCounter = settings.initialValue;
    m_totalPayloadBytes = 0;
    m_totalPackets = 0;
    m_lastStatisticsBytes = 0;
    m_lastStatisticsPackets = 0;
    m_lastStatisticsElapsedMs = 0;
    m_lastPeriodicLogBytes = 0;
    m_lastPeriodicLogPackets = 0;
    m_lastPeriodicLogElapsedMs = 0;
    m_periodicMinimumSpeedKbps = 0.0;
    m_periodicMaximumSpeedKbps = 0.0;
    m_periodicMinimumPacketsPerSecond = 0.0;
    m_periodicMaximumPacketsPerSecond = 0.0;
    m_periodicSampleAvailable = false;
    m_startTime = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    m_elapsedTimer.start();

    emit statisticsUpdated(m_startTime,
                           0,
                           0,
                           m_currentCounter,
                           0.0,
                           0.0);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Updates and emits the current UDP Statistics snapshot.
 * @param includePeriodicSample true to include the sample in 20-second extrema.
 * @return none
 * @detail Uses monotonic elapsed time and saturating totals to calculate the latest
 *         payload speed and packet rate.
 */
void UdpTxWorker::updateStatisticsSnapshot(bool includePeriodicSample)
{
    if (!m_collectStatistics || !m_elapsedTimer.isValid())
    {
        return;
    }

    const qint64 elapsedMilliseconds = qMax<qint64>(0, m_elapsedTimer.elapsed());
    const qint64 intervalMilliseconds =
        elapsedMilliseconds - m_lastStatisticsElapsedMs;
    const quint64 deltaBytes =
        m_totalPayloadBytes - m_lastStatisticsBytes;
    const quint64 deltaPackets =
        m_totalPackets - m_lastStatisticsPackets;

    double speedKbps = 0.0;
    double packetsPerSecond = 0.0;
    if (intervalMilliseconds > 0)
    {
        speedKbps = (static_cast<double>(deltaBytes) * 8.0)
                    / static_cast<double>(intervalMilliseconds);
        packetsPerSecond = (static_cast<double>(deltaPackets) * 1000.0)
                           / static_cast<double>(intervalMilliseconds);
    }

    m_lastStatisticsBytes = m_totalPayloadBytes;
    m_lastStatisticsPackets = m_totalPackets;
    m_lastStatisticsElapsedMs = elapsedMilliseconds;

    if (includePeriodicSample && m_periodicLogStatisticsActive)
    {
        if (!m_periodicSampleAvailable)
        {
            m_periodicMinimumSpeedKbps = speedKbps;
            m_periodicMaximumSpeedKbps = speedKbps;
            m_periodicMinimumPacketsPerSecond = packetsPerSecond;
            m_periodicMaximumPacketsPerSecond = packetsPerSecond;
            m_periodicSampleAvailable = true;
        }
        else
        {
            m_periodicMinimumSpeedKbps =
                qMin(m_periodicMinimumSpeedKbps, speedKbps);
            m_periodicMaximumSpeedKbps =
                qMax(m_periodicMaximumSpeedKbps, speedKbps);
            m_periodicMinimumPacketsPerSecond =
                qMin(m_periodicMinimumPacketsPerSecond, packetsPerSecond);
            m_periodicMaximumPacketsPerSecond =
                qMax(m_periodicMaximumPacketsPerSecond, packetsPerSecond);
        }
    }

    emit statisticsUpdated(m_startTime,
                           elapsedMilliseconds,
                           m_totalPayloadBytes,
                           m_currentCounter,
                           speedKbps,
                           packetsPerSecond);

    if (includePeriodicSample)
    {
        emitPeriodicLogLineIfDue();
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Emits a twenty-second UDP Statistics line when due.
 * @param none
 * @return none
 * @detail Uses exact elapsed time, payload-byte and packet deltas, and the minimum and
 *         maximum one-second samples accumulated during the interval.
 */
void UdpTxWorker::emitPeriodicLogLineIfDue()
{
    if (!m_periodicLogStatisticsActive || !m_elapsedTimer.isValid())
    {
        return;
    }

    const qint64 elapsedMilliseconds = qMax<qint64>(0, m_elapsedTimer.elapsed());
    const qint64 intervalMilliseconds =
        elapsedMilliseconds - m_lastPeriodicLogElapsedMs;
    if (intervalMilliseconds < kPeriodicLogIntervalMs)
    {
        return;
    }

    const quint64 deltaBytes =
        m_totalPayloadBytes - m_lastPeriodicLogBytes;
    const quint64 deltaPackets =
        m_totalPackets - m_lastPeriodicLogPackets;
    const double averageSpeedKbps =
        intervalMilliseconds > 0
            ? (static_cast<double>(deltaBytes) * 8.0)
                  / static_cast<double>(intervalMilliseconds)
            : 0.0;
    const double averagePacketsPerSecond =
        intervalMilliseconds > 0
            ? (static_cast<double>(deltaPackets) * 1000.0)
                  / static_cast<double>(intervalMilliseconds)
            : 0.0;

    const double minimumSpeed =
        m_periodicSampleAvailable ? m_periodicMinimumSpeedKbps : 0.0;
    const double maximumSpeed =
        m_periodicSampleAvailable ? m_periodicMaximumSpeedKbps : 0.0;
    const double minimumPackets =
        m_periodicSampleAvailable ? m_periodicMinimumPacketsPerSecond : 0.0;
    const double maximumPackets =
        m_periodicSampleAvailable ? m_periodicMaximumPacketsPerSecond : 0.0;

    const QString line =
        tr("%1, mode=UDP, time=%2, tx_bytes=%3, delta_tx_bytes=%4, tx_packets=%5, delta_tx_packets=%6, curr_counter=%7, min_speed=%8, avrg_speed=%9, max_speed=%10, min_packets_s=%11, avrg_packets_s=%12, max_packets_s=%13")
            .arg(QDateTime::currentDateTime()
                     .toString(QStringLiteral("HH:mm:ss.zzz")))
            .arg(formatElapsedTime(elapsedMilliseconds))
            .arg(m_totalPayloadBytes)
            .arg(deltaBytes)
            .arg(m_totalPackets)
            .arg(deltaPackets)
            .arg(m_currentCounter)
            .arg(formatRate(minimumSpeed))
            .arg(formatRate(averageSpeedKbps))
            .arg(formatRate(maximumSpeed))
            .arg(formatRate(minimumPackets))
            .arg(formatRate(averagePacketsPerSecond))
            .arg(formatRate(maximumPackets));
    emit periodicLogLineReady(line);

    m_lastPeriodicLogBytes = m_totalPayloadBytes;
    m_lastPeriodicLogPackets = m_totalPackets;
    m_lastPeriodicLogElapsedMs = elapsedMilliseconds;
    m_periodicMinimumSpeedKbps = 0.0;
    m_periodicMaximumSpeedKbps = 0.0;
    m_periodicMinimumPacketsPerSecond = 0.0;
    m_periodicMaximumPacketsPerSecond = 0.0;
    m_periodicSampleAvailable = false;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Sends one complete burst of UDP datagrams.
 * @param settings Active validated Pattern settings.
 * @return true when every datagram in the burst was accepted by the socket.
 * @detail Counter progression is committed only after each complete datagram is
 *         accepted by writeDatagram().
 */
bool UdpTxWorker::sendBurst(const PatternSettings &settings)
{
    for (int packetIndex = 0;
         packetIndex < settings.togetherCount;
         ++packetIndex)
    {
        if (!sendOneDatagram(settings))
        {
            return false;
        }
    }

    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Builds and sends one UDP datagram.
 * @param settings Active validated Pattern settings.
 * @return true when writeDatagram() accepted the complete payload.
 * @detail Counts only application payload bytes and advances the counter only after a
 *         successful complete-datagram result.
 */
bool UdpTxWorker::sendOneDatagram(const PatternSettings &settings)
{
    if (m_udpSocket == nullptr
        || !m_connectionConfigured
        || m_udpSocket->state() == QAbstractSocket::UnconnectedState)
    {
        emitWorkerEvent(tr("UDP write error: the sending socket is not ready"), true);
        return false;
    }

    quint64 nextCounter = m_currentCounter;
    QByteArray datagram;
    try
    {
        datagram = buildDatagram(settings,
                                 m_currentCounter,
                                 &nextCounter);
    }
    catch (const std::bad_alloc &)
    {
        emitWorkerEvent(tr("UDP write error: not enough memory to build the datagram"),
                        true);
        return false;
    }

    if (datagram.size() != settings.blockBytes)
    {
        emitWorkerEvent(tr("UDP write error: generated payload size is invalid"), true);
        return false;
    }

    m_socketOperationInProgress = true;
    const qint64 bytesAccepted =
        m_udpSocket->writeDatagram(datagram,
                                   m_destinationAddress,
                                   m_destinationPort);
    m_socketOperationInProgress = false;

    if (bytesAccepted != datagram.size())
    {
        const int errorValue = static_cast<int>(m_udpSocket->error());
        emitWorkerEvent(tr("UDP write error: writeDatagram accepted %1 of %2 bytes: %3 (code %4)")
                            .arg(bytesAccepted)
                            .arg(datagram.size())
                            .arg(socketErrorText(errorValue))
                            .arg(errorValue),
                        true);
        return false;
    }

    m_currentCounter = nextCounter;

    const quint64 unsignedBytes = static_cast<quint64>(bytesAccepted);
    const quint64 maximumValue = std::numeric_limits<quint64>::max();
    if (m_totalPayloadBytes > maximumValue - unsignedBytes)
    {
        m_totalPayloadBytes = maximumValue;
    }
    else
    {
        m_totalPayloadBytes += unsignedBytes;
    }

    if (m_totalPackets < maximumValue)
    {
        ++m_totalPackets;
    }

    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Builds one little-endian counter-pattern payload.
 * @param settings Active validated Pattern settings.
 * @param startCounter First counter value to place into the payload.
 * @param nextCounter Output next counter value after the payload.
 * @return QByteArray containing exactly blockBytes bytes.
 * @detail Counter values wrap to zero after the selected unsigned maximum.
 */
QByteArray UdpTxWorker::buildDatagram(const PatternSettings &settings,
                                      quint64 startCounter,
                                      quint64 *nextCounter) const
{
    if (nextCounter == nullptr)
    {
        return QByteArray();
    }

    QByteArray datagram;
    datagram.resize(settings.blockBytes);
    quint64 counter = startCounter;

    for (int offset = 0;
         offset < settings.blockBytes;
         offset += settings.counterBytes)
    {
        for (int byteIndex = 0;
             byteIndex < settings.counterBytes;
             ++byteIndex)
        {
            datagram[offset + byteIndex] =
                static_cast<char>((counter >> (8 * byteIndex)) & 0xFFU);
        }

        if (counter == settings.maximumCounterValue)
        {
            counter = 0;
        }
        else
        {
            ++counter;
        }
    }

    *nextCounter = counter;
    return datagram;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Stops active UDP generation after an error.
 * @param eventText Error or service text to emit; an empty string emits nothing.
 * @param error true to emit eventText as a red entry.
 * @param closeConnection true to close the persistent UDP socket.
 * @return none
 * @detail Finalizes Statistics, resets transmission state, and optionally reports a
 *         failed connection to the GUI.
 */
void UdpTxWorker::stopTransmissionInternal(const QString &eventText,
                                           bool error,
                                           bool closeConnection)
{
    if (m_transmitTimer != nullptr)
    {
        m_transmitTimer->stop();
    }

    if (m_collectStatistics)
    {
        updateStatisticsSnapshot(false);
    }

    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }

    m_testRunning = false;
    m_singleTransferActive = false;
    m_collectStatistics = false;
    m_periodicLogStatisticsActive = false;
    emitTransmissionState();

    if (!eventText.isEmpty())
    {
        emitWorkerEvent(eventText, error);
    }

    if (closeConnection)
    {
        closeSocket(true, false);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Closes the UDP socket and emits a disconnected state.
 * @param causedByFailure true when closure was caused by a network error.
 * @param emitEvent true to emit a socket-closed service event.
 * @return none
 * @detail Clears destination state after copying values needed by the state signal.
 */
void UdpTxWorker::closeSocket(bool causedByFailure, bool emitEvent)
{
    const QString destinationIp = m_destinationIp;
    const quint16 destinationPort = m_destinationPort;
    const QString localIp = m_localIp;
    const quint16 localPort = m_localPort;

    if (m_udpSocket != nullptr)
    {
        m_socketOperationInProgress = true;
        m_udpSocket->close();
        m_socketOperationInProgress = false;
    }

    m_connectionConfigured = false;
    m_destinationAddress.clear();
    m_destinationIp.clear();
    m_destinationPort = 0;
    m_localIp.clear();
    m_localPort = 0;

    emit connectionStateChanged(false,
                                destinationIp,
                                destinationPort,
                                localIp,
                                localPort,
                                causedByFailure);

    if (emitEvent && !destinationIp.isEmpty())
    {
        emitWorkerEvent(tr("UDP destination disconnected: %1:%2; local socket %3:%4 closed")
                            .arg(destinationIp)
                            .arg(destinationPort)
                            .arg(localIp)
                            .arg(localPort),
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
QString UdpTxWorker::formatElapsedTime(qint64 elapsedMilliseconds) const
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
 * @brief Formats a floating-point rate for logs.
 * @param value Nonnegative speed or packet-rate value.
 * @return String containing at most three digits after the decimal point.
 * @detail Removes insignificant trailing zeros and returns 0 for invalid values.
 */
QString UdpTxWorker::formatRate(double value) const
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
 * @brief Returns a fixed English description for a socket error.
 * @param socketError Numeric QAbstractSocket::SocketError value.
 * @return English text independent of the operating-system language.
 * @detail Maps the Qt 5.12 socket-error values used by UDP transmission.
 */
QString UdpTxWorker::socketErrorText(int socketError) const
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
        return QStringLiteral("local address is already in use");
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
        return QStringLiteral("temporary error or send buffer is full");
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
 * @detail Creates the HH:MM:SS.mmm timestamp inside the UDP worker thread before queued
 *         delivery to the GUI.
 */
void UdpTxWorker::emitWorkerEvent(const QString &text, bool error)
{
    emit eventGenerated(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
        text,
        error);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Emits the current UDP transmission states.
 * @param none
 * @return none
 * @detail Sends Boolean state only and never exposes QUdpSocket or QTimer objects across
 *         threads.
 */
void UdpTxWorker::emitTransmissionState()
{
    emit transmissionStateChanged(m_testRunning, m_singleTransferActive);
}
