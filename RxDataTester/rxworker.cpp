#include "rxworker.h"

#include <QDateTime>
#include <QIODevice>
#include <QTime>
#include <QTimer>

#include <limits>

namespace
{
constexpr int kStatisticsIntervalMs = 1000;
constexpr int kPortHealthIntervalMs = 1000;
constexpr qint64 kPeriodicLogIntervalMs = 20 * 1000;

/**
 * @brief Returns a fixed English description for a serial-port error.
 * @param error QSerialPort error value to describe.
 * @return English text that does not depend on the operating-system language.
 * @detail Maps every QSerialPort::SerialPortError available in Qt 5.12 to a concise
 *         message suitable for EVENTS and text logs.
 */
QString serialPortErrorText(QSerialPort::SerialPortError error)
{
    switch (error)
    {
    case QSerialPort::NoError:
        return QStringLiteral("no error");
    case QSerialPort::DeviceNotFoundError:
        return QStringLiteral("device not found");
    case QSerialPort::PermissionError:
        return QStringLiteral("permission denied");
    case QSerialPort::OpenError:
        return QStringLiteral("open error");
    case QSerialPort::NotOpenError:
        return QStringLiteral("port is not open");
    case QSerialPort::ParityError:
        return QStringLiteral("parity error");
    case QSerialPort::FramingError:
        return QStringLiteral("framing error");
    case QSerialPort::BreakConditionError:
        return QStringLiteral("break condition");
    case QSerialPort::WriteError:
        return QStringLiteral("write error");
    case QSerialPort::ReadError:
        return QStringLiteral("read error");
    case QSerialPort::ResourceError:
        return QStringLiteral("resource error or operation aborted");
    case QSerialPort::UnsupportedOperationError:
        return QStringLiteral("unsupported operation");
    case QSerialPort::UnknownError:
        return QStringLiteral("unknown serial-port error");
    case QSerialPort::TimeoutError:
        return QStringLiteral("operation timed out");
    }

    return QStringLiteral("unrecognized serial-port error");
}
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates the receiver worker object.
 * @param parent Parent QObject; normally omitted before moveToThread().
 * @return none
 * @detail Initializes scalar state only. QSerialPort and QTimer objects are created
 *         later by initialize() after the worker is moved to its thread.
 */
RxWorker::RxWorker(QObject *parent)
    : QObject(parent)
    , m_serialPort(nullptr)
    , m_statisticsTimer(nullptr)
    , m_portHealthTimer(nullptr)
    , m_expectedCounter(0)
    , m_lastReceivedCounter(0)
    , m_totalBytesReceived(0)
    , m_lastStatisticsBytes(0)
    , m_lastStatisticsElapsedMs(0)
    , m_counterOk(0)
    , m_counterErrors(0)
    , m_lastPeriodicLogBytes(0)
    , m_lastPeriodicCounterOk(0)
    , m_lastPeriodicCounterErrors(0)
    , m_lastPeriodicLogElapsedMs(0)
    , m_periodicMinimumSpeedKbps(0.0)
    , m_periodicMaximumSpeedKbps(0.0)
    , m_periodicSpeedSampleAvailable(false)
    , m_initialized(false)
    , m_testRunning(false)
    , m_openOperationInProgress(false)
    , m_handlingPortFailure(false)
    , m_shuttingDown(false)
{
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Destroys the receiver worker object.
 * @param none
 * @return none
 * @detail Stops timers and closes the port as a safeguard. During normal shutdown these
 *         actions are already performed by shutdown().
 */
RxWorker::~RxWorker()
{
    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }

    if (m_portHealthTimer != nullptr)
    {
        m_portHealthTimer->stop();
    }

    if (m_serialPort != nullptr && m_serialPort->isOpen())
    {
        m_serialPort->close();
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Initializes resources owned by the RX worker thread.
 * @param none
 * @return none
 * @detail Creates QSerialPort and the timers with RxWorker as parent, connects their
 *         signals, and reports readiness to the GUI.
 */
void RxWorker::initialize()
{
    if (m_initialized)
    {
        emit workerReady();
        return;
    }

    m_serialPort = new QSerialPort(this);
    m_statisticsTimer = new QTimer(this);
    m_portHealthTimer = new QTimer(this);

    m_serialPort->setReadBufferSize(0);

    m_statisticsTimer->setInterval(kStatisticsIntervalMs);
    m_statisticsTimer->setSingleShot(false);
    m_statisticsTimer->setTimerType(Qt::PreciseTimer);

    m_portHealthTimer->setInterval(kPortHealthIntervalMs);
    m_portHealthTimer->setSingleShot(false);
    m_portHealthTimer->setTimerType(Qt::CoarseTimer);

    connect(m_serialPort,
            &QSerialPort::readyRead,
            this,
            &RxWorker::handleReadyRead);
    connect(m_serialPort,
            &QSerialPort::errorOccurred,
            this,
            &RxWorker::handleSerialError);
    connect(m_statisticsTimer,
            &QTimer::timeout,
            this,
            &RxWorker::updateStatistics);
    connect(m_portHealthTimer,
            &QTimer::timeout,
            this,
            &RxWorker::checkPortHealth);

    m_portHealthTimer->start();
    m_initialized = true;
    emit receptionStateChanged(false);
    emit portStateChanged(false, QString(), QString(), false);
    emit workerReady();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Opens and configures the serial port.
 * @param portName Name of the selected port, for example COM13.
 * @param baudRate Selected baud rate.
 * @param parityValue Numeric value of QSerialPort::Parity.
 * @param stopBitsValue Numeric value of QSerialPort::StopBits.
 * @param settingsDescription Preformatted settings description for the event log.
 * @return none
 * @detail The port is opened in ReadOnly mode with 8 data bits and no flow control. All
 *         operations execute in the RX worker thread.
 */
void RxWorker::openPort(const QString &portName,
                        qint32 baudRate,
                        int parityValue,
                        int stopBitsValue,
                        const QString &settingsDescription)
{
    if (!m_initialized || m_serialPort == nullptr)
    {
        emitWorkerEvent(tr("open error: the RX worker thread is not initialized"),
                        true);
        emit portStateChanged(false, portName, settingsDescription, false);
        return;
    }

    if (m_serialPort->isOpen())
    {
        emitWorkerEvent(tr("port %1 is already open").arg(m_serialPort->portName()),
                        true);
        emit portStateChanged(true,
                              m_openPortName,
                              m_openPortSettingsDescription,
                              false);
        return;
    }

    const QSerialPort::Parity parity =
        static_cast<QSerialPort::Parity>(parityValue);
    const QSerialPort::StopBits stopBits =
        static_cast<QSerialPort::StopBits>(stopBitsValue);
    const bool parityValid = parity == QSerialPort::NoParity
                             || parity == QSerialPort::EvenParity
                             || parity == QSerialPort::OddParity;
    const bool stopBitsValid = stopBits == QSerialPort::OneStop
                               || stopBits == QSerialPort::TwoStop;

    if (portName.trimmed().isEmpty()
        || baudRate <= 0
        || !parityValid
        || !stopBitsValid)
    {
        emitWorkerEvent(tr("open error: invalid port settings were provided"),
                        true);
        emit portStateChanged(false, portName, settingsDescription, false);
        return;
    }

    m_openOperationInProgress = true;
    m_serialPort->setPortName(portName.trimmed());
    m_serialPort->clearError();

    if (!m_serialPort->open(QIODevice::ReadOnly))
    {
        const QString errorText = serialPortErrorText(m_serialPort->error());
        m_openOperationInProgress = false;
        emitWorkerEvent(tr("failed to open port %1: %2")
                            .arg(portName, errorText),
                        true);
        emit portStateChanged(false, portName, settingsDescription, false);
        return;
    }

    const bool configured =
        m_serialPort->setBaudRate(baudRate, QSerialPort::AllDirections)
        && m_serialPort->setDataBits(QSerialPort::Data8)
        && m_serialPort->setParity(parity)
        && m_serialPort->setStopBits(stopBits)
        && m_serialPort->setFlowControl(QSerialPort::NoFlowControl);

    if (!configured)
    {
        const QString errorText = serialPortErrorText(m_serialPort->error());
        m_serialPort->close();
        m_openOperationInProgress = false;
        emitWorkerEvent(tr("failed to configure port %1: %2")
                            .arg(portName, errorText),
                        true);
        emit portStateChanged(false, portName, settingsDescription, false);
        return;
    }

    m_serialPort->readAll();
    m_receiveBuffer.clear();
    m_openOperationInProgress = false;
    m_openPortName = portName.trimmed();
    m_openPortSettingsDescription = settingsDescription;

    emit portStateChanged(true,
                          m_openPortName,
                          m_openPortSettingsDescription,
                          false);
    emitWorkerEvent(tr("port %1 opened: %2")
                        .arg(m_openPortName, m_openPortSettingsDescription),
                    false);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Closes the serial port in response to CLOSE.
 * @param none
 * @return none
 * @detail If a test is active, finalizes reception and Statistics first, then closes
 *         QSerialPort in its owning thread.
 */
void RxWorker::closePort()
{
    if (!m_initialized || m_serialPort == nullptr)
    {
        emit portStateChanged(false, QString(), QString(), false);
        return;
    }

    if (m_testRunning)
    {
        const QString reason =
            tr("reception stopped by CLOSE; received %1 bytes; "
               "counter ok=%2; counter err=%3; incomplete tail=%4 bytes")
                .arg(m_totalBytesReceived)
                .arg(m_counterOk)
                .arg(m_counterErrors)
                .arg(m_receiveBuffer.size());
        stopReceptionInternal(reason, false);
    }

    const QString portName = m_openPortName.isEmpty()
                                 ? m_serialPort->portName()
                                 : m_openPortName;
    const QString settingsText = m_openPortSettingsDescription;

    if (m_serialPort->isOpen())
    {
        m_openOperationInProgress = true;
        m_serialPort->close();
        m_openOperationInProgress = false;
    }

    m_receiveBuffer.clear();
    m_openPortName.clear();
    m_openPortSettingsDescription.clear();

    emit portStateChanged(false, portName, settingsText, false);

    if (!portName.trimmed().isEmpty())
    {
        emitWorkerEvent(tr("port %1 closed: %2")
                            .arg(portName, settingsText),
                        false);
    }
}

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
void RxWorker::startReception(int counterBits,
                              int blockBytes,
                              int periodMs,
                              quint64 initialValue,
                              const QString &patternDescription)
{
    if (!m_initialized || m_serialPort == nullptr || !m_serialPort->isOpen())
    {
        emitWorkerEvent(tr("START failed: the COM port is not open"), true);
        emitReceptionState();
        return;
    }

    if (m_testRunning)
    {
        emitWorkerEvent(tr("START failed: reception is already running"), true);
        emitReceptionState();
        return;
    }

    PatternSettings settings;
    QString errorText;
    if (!makePatternSettings(counterBits,
                             blockBytes,
                             periodMs,
                             initialValue,
                             &settings,
                             &errorText))
    {
        emitWorkerEvent(tr("Pattern error: %1").arg(errorText), true);
        emitReceptionState();
        return;
    }

    const QByteArray discardedData = m_serialPort->readAll();
    m_receiveBuffer.clear();
    m_activePattern = settings;
    m_testRunning = true;
    resetStatistics(settings);
    emitReceptionState();
    m_statisticsTimer->start();

    emitWorkerEvent(tr("START: reception and verification started; %1")
                        .arg(patternDescription),
                    false);

    if (!discardedData.isEmpty())
    {
        emitWorkerEvent(tr("START: discarded %1 bytes received before the test started")
                            .arg(discardedData.size()),
                        false);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Stops reception and verification in response to STOP.
 * @param none
 * @return none
 * @detail Captures the final Statistics snapshot, stops the test, and leaves the COM
 *         port open. Later incoming bytes are read and discarded.
 */
void RxWorker::stopReception()
{
    if (!m_testRunning)
    {
        emitReceptionState();
        return;
    }

    const QString reason =
        tr("STOP: reception and verification stopped; received %1 bytes; "
           "counter ok=%2; counter err=%3; incomplete tail=%4 bytes")
            .arg(m_totalBytesReceived)
            .arg(m_counterOk)
            .arg(m_counterErrors)
            .arg(m_receiveBuffer.size());
    stopReceptionInternal(reason, false);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles loss of the open port detected by the GUI.
 * @param reason Reason text created from QSerialPortInfo.
 * @return none
 * @detail Delegates handling to the common critical device-loss procedure.
 */
void RxWorker::handleExternalPortLoss(const QString &reason)
{
    handlePortFailure(reason);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Shuts down the RX worker before application exit.
 * @param none
 * @return none
 * @detail Stops an active test, closes the COM port and timers, and leaves the worker
 *         ready for the main thread to call QThread::quit().
 */
void RxWorker::shutdown()
{
    if (m_shuttingDown)
    {
        return;
    }

    m_shuttingDown = true;

    if (!m_initialized || m_serialPort == nullptr)
    {
        return;
    }

    if (m_testRunning)
    {
        const QString reason =
            tr("reception stopped during application shutdown; received %1 bytes; "
               "counter ok=%2; counter err=%3; incomplete tail=%4 bytes")
                .arg(m_totalBytesReceived)
                .arg(m_counterOk)
                .arg(m_counterErrors)
                .arg(m_receiveBuffer.size());
        stopReceptionInternal(reason, false);
    }

    if (m_serialPort->isOpen())
    {
        const QString portName = m_openPortName.isEmpty()
                                     ? m_serialPort->portName()
                                     : m_openPortName;
        const QString settingsText = m_openPortSettingsDescription;

        m_openOperationInProgress = true;
        m_serialPort->close();
        m_openOperationInProgress = false;

        emit portStateChanged(false, portName, settingsText, false);
        emitWorkerEvent(tr("port %1 closed during application shutdown: %2")
                            .arg(portName, settingsText),
                        false);
    }
    else
    {
        emit portStateChanged(false,
                              m_openPortName,
                              m_openPortSettingsDescription,
                              false);
    }

    m_receiveBuffer.clear();
    m_openPortName.clear();
    m_openPortSettingsDescription.clear();
    m_statisticsTimer->stop();
    m_portHealthTimer->stop();
    m_testRunning = false;
    emitReceptionState();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Reads all currently available serial-port bytes.
 * @param none
 * @return none
 * @detail When no test is active, discards data so START begins with fresh bytes.
 *         During a test, appends bytes to the receive buffer and verifies counters.
 */
void RxWorker::handleReadyRead()
{
    if (m_serialPort == nullptr)
    {
        return;
    }

    const QByteArray receivedData = m_serialPort->readAll();
    if (receivedData.isEmpty() || !m_testRunning)
    {
        return;
    }

    const quint64 bytes = static_cast<quint64>(receivedData.size());
    const quint64 maximumValue = std::numeric_limits<quint64>::max();
    if (m_totalBytesReceived > maximumValue - bytes)
    {
        m_totalBytesReceived = maximumValue;
    }
    else
    {
        m_totalBytesReceived += bytes;
    }

    m_receiveBuffer.append(receivedData);
    processReceiveBuffer();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Performs the one-second Statistics update.
 * @param none
 * @return none
 * @detail Calculates speed from the actual interval, updates the twenty-second minimum
 *         and maximum, and sends the prepared snapshot to the GUI.
 */
void RxWorker::updateStatistics()
{
    if (!m_testRunning)
    {
        return;
    }

    updateStatisticsSnapshot(true);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Checks that the logically open port remains open.
 * @param none
 * @return none
 * @detail Once per second, detects an unexpected QSerialPort close even when no
 *         separate critical errorOccurred signal was received.
 */
void RxWorker::checkPortHealth()
{
    if (!m_initialized
        || m_shuttingDown
        || m_openOperationInProgress
        || m_handlingPortFailure
        || m_serialPort == nullptr
        || m_openPortName.isEmpty()
        || m_serialPort->isOpen())
    {
        return;
    }

    handlePortFailure(
        tr("open port %1 closed unexpectedly").arg(m_openPortName));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles a QSerialPort error in the worker thread.
 * @param error Serial-port error code.
 * @return none
 * @detail Critical resource, read, and device-not-found errors close the port. Parity,
 *         framing, and break errors are logged without closing it.
 */
void RxWorker::handleSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError
        || m_serialPort == nullptr
        || m_openOperationInProgress
        || m_handlingPortFailure
        || m_shuttingDown)
    {
        return;
    }

    if (m_openPortName.isEmpty() && !m_serialPort->isOpen())
    {
        return;
    }

    const QString portName = m_openPortName.isEmpty()
                                 ? m_serialPort->portName()
                                 : m_openPortName;
    const QString errorText =
        tr("port %1 error: %2 (code %3)")
            .arg(portName, serialPortErrorText(error))
            .arg(static_cast<int>(error));

    const bool criticalError =
        error == QSerialPort::DeviceNotFoundError
        || error == QSerialPort::PermissionError
        || error == QSerialPort::OpenError
        || error == QSerialPort::ReadError
        || error == QSerialPort::ResourceError
        || error == QSerialPort::UnknownError
        || error == QSerialPort::NotOpenError;

    if (criticalError)
    {
        handlePortFailure(errorText);
        return;
    }

    emitWorkerEvent(errorText, true);
}

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
bool RxWorker::makePatternSettings(int counterBits,
                                   int blockBytes,
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
        *errorText = tr("counter, bits must be 8, 16, 32, or 64");
        return false;
    }

    const int counterBytes = counterBits / 8;
    if (blockBytes <= 0 || (blockBytes % counterBytes) != 0)
    {
        *errorText = tr("block, bytes must be positive and a multiple of the counter size");
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
            : (quint64(1) << counterBits) - 1U;

    if (initialValue > maximumCounterValue)
    {
        *errorText = tr("init value does not fit the selected counter width");
        return false;
    }

    settings->counterBits = counterBits;
    settings->counterBytes = counterBytes;
    settings->blockBytes = blockBytes;
    settings->periodMs = periodMs;
    settings->initialValue = initialValue;
    settings->maximumCounterValue = maximumCounterValue;
    errorText->clear();
    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Resets Statistics and verification state before START.
 * @param settings Validated settings for the new test.
 * @return none
 * @detail Stores the local start time, sets expected to init, clears interval and total
 *         counters, starts monotonic timing, and emits the initial snapshot.
 */
void RxWorker::resetStatistics(const PatternSettings &settings)
{
    m_activePattern = settings;
    m_startTime = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    m_expectedCounter = settings.initialValue;
    m_lastReceivedCounter = settings.initialValue;
    m_totalBytesReceived = 0;
    m_lastStatisticsBytes = 0;
    m_lastStatisticsElapsedMs = 0;
    m_counterOk = 0;
    m_counterErrors = 0;
    m_lastPeriodicLogBytes = 0;
    m_lastPeriodicCounterOk = 0;
    m_lastPeriodicCounterErrors = 0;
    m_lastPeriodicLogElapsedMs = 0;
    m_periodicMinimumSpeedKbps = 0.0;
    m_periodicMaximumSpeedKbps = 0.0;
    m_periodicSpeedSampleAvailable = false;
    m_receiveBuffer.clear();
    m_elapsedTimer.start();

    emit statisticsUpdated(m_startTime,
                           0,
                           0,
                           m_lastReceivedCounter,
                           0,
                           0,
                           0.0);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Processes every complete counter value in the receive buffer.
 * @param none
 * @return none
 * @detail Incomplete trailing bytes remain for the next readyRead(). A mismatch
 *         increments counter err once and changes the next expected value to received
 *         plus one.
 */
void RxWorker::processReceiveBuffer()
{
    const int counterBytes = m_activePattern.counterBytes;
    if (counterBytes <= 0 || m_receiveBuffer.size() < counterBytes)
    {
        return;
    }

    const char *data = m_receiveBuffer.constData();
    const int completeBytes =
        (m_receiveBuffer.size() / counterBytes) * counterBytes;

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
                tr("counter error: expected %1, received %2, "
                   "next expected value is %3")
                    .arg(expectedCounter)
                    .arg(receivedCounter)
                    .arg(nextExpectedCounter),
                true);
        }

        m_expectedCounter = nextExpectedCounter;
    }

    m_receiveBuffer.remove(0, completeBytes);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Decodes one little-endian counter value.
 * @param data Pointer to the first byte of the value.
 * @param byteCount Value size: 1, 2, 4, or 8 bytes.
 * @return The received counter represented as an unsigned 64-bit value.
 * @detail Performs byte-by-byte conversion independently of host byte order and
 *         alignment.
 */
quint64 RxWorker::decodeLittleEndianCounter(const char *data,
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
 * @return value + 1, or 0 after the maximum value of the selected width.
 * @detail Uses the precomputed maximumCounterValue and works identically for 8 through
 *         64 bits.
 */
quint64 RxWorker::nextCounterValue(quint64 value) const
{
    if (value >= m_activePattern.maximumCounterValue)
    {
        return 0;
    }

    return value + 1U;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Updates and emits the current Statistics snapshot.
 * @param includePeriodicSample true only for the one-second timer tick.
 * @return none
 * @detail Calculates speed as delta_bytes * 8 / delta_time_ms, which yields decimal
 *         kilobits per second for the measured millisecond interval.
 */
void RxWorker::updateStatisticsSnapshot(bool includePeriodicSample)
{
    if (!m_elapsedTimer.isValid())
    {
        return;
    }

    const qint64 elapsedMilliseconds = m_elapsedTimer.elapsed();
    const qint64 deltaMilliseconds =
        elapsedMilliseconds - m_lastStatisticsElapsedMs;
    const quint64 deltaBytes =
        m_totalBytesReceived >= m_lastStatisticsBytes
            ? m_totalBytesReceived - m_lastStatisticsBytes
            : 0;

    double speedKbps = 0.0;
    if (deltaMilliseconds > 0)
    {
        speedKbps =
            (static_cast<double>(deltaBytes) * 8.0)
            / static_cast<double>(deltaMilliseconds);
    }

    m_lastStatisticsBytes = m_totalBytesReceived;
    m_lastStatisticsElapsedMs = elapsedMilliseconds;

    if (includePeriodicSample)
    {
        if (!m_periodicSpeedSampleAvailable)
        {
            m_periodicMinimumSpeedKbps = speedKbps;
            m_periodicMaximumSpeedKbps = speedKbps;
            m_periodicSpeedSampleAvailable = true;
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
        }
    }

    emit statisticsUpdated(m_startTime,
                           elapsedMilliseconds,
                           m_totalBytesReceived,
                           m_lastReceivedCounter,
                           m_counterOk,
                           m_counterErrors,
                           speedKbps);

    emitPeriodicLogLineIfDue();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates the twenty-second log line when its interval has elapsed.
 * @param none
 * @return none
 * @detail Uses the actual monotonic interval, delta_rx_bytes, match and error counters,
 *         and the one-second minimum and maximum speeds. After emitting the line,
 *         starts a new interval.
 */
void RxWorker::emitPeriodicLogLineIfDue()
{
    if (!m_testRunning || !m_elapsedTimer.isValid())
    {
        return;
    }

    const qint64 elapsedMilliseconds = m_elapsedTimer.elapsed();
    const qint64 intervalMilliseconds =
        elapsedMilliseconds - m_lastPeriodicLogElapsedMs;
    if (intervalMilliseconds < kPeriodicLogIntervalMs)
    {
        return;
    }

    const quint64 deltaBytes =
        m_totalBytesReceived >= m_lastPeriodicLogBytes
            ? m_totalBytesReceived - m_lastPeriodicLogBytes
            : 0;
    const quint64 deltaCounterOk =
        m_counterOk >= m_lastPeriodicCounterOk
            ? m_counterOk - m_lastPeriodicCounterOk
            : 0;
    const quint64 deltaCounterErrors =
        m_counterErrors >= m_lastPeriodicCounterErrors
            ? m_counterErrors - m_lastPeriodicCounterErrors
            : 0;
    const double averageSpeedKbps =
        intervalMilliseconds > 0
            ? (static_cast<double>(deltaBytes) * 8.0)
                  / static_cast<double>(intervalMilliseconds)
            : 0.0;
    const double minimumSpeedKbps =
        m_periodicSpeedSampleAvailable
            ? m_periodicMinimumSpeedKbps
            : averageSpeedKbps;
    const double maximumSpeedKbps =
        m_periodicSpeedSampleAvailable
            ? m_periodicMaximumSpeedKbps
            : averageSpeedKbps;

    const QString line =
        QStringLiteral("%1, time=%2, rx_bytes=%3, delta_rx_bytes=%4, "
                       "curr_counter=%5, counter_ok=%6, delta_counter_ok=%7, "
                       "counter_err=%8, delta_counter_err=%9, min_speed=%10, "
                       "avrg_speed=%11, max_speed=%12")
            .arg(QDateTime::currentDateTime().toString(
                QStringLiteral("HH:mm:ss.zzz")))
            .arg(formatElapsedTime(elapsedMilliseconds))
            .arg(m_totalBytesReceived)
            .arg(deltaBytes)
            .arg(m_lastReceivedCounter)
            .arg(m_counterOk)
            .arg(deltaCounterOk)
            .arg(m_counterErrors)
            .arg(deltaCounterErrors)
            .arg(formatSpeed(minimumSpeedKbps))
            .arg(formatSpeed(averageSpeedKbps))
            .arg(formatSpeed(maximumSpeedKbps));

    emit periodicLogLineReady(line);

    m_lastPeriodicLogBytes = m_totalBytesReceived;
    m_lastPeriodicCounterOk = m_counterOk;
    m_lastPeriodicCounterErrors = m_counterErrors;
    m_lastPeriodicLogElapsedMs = elapsedMilliseconds;
    m_periodicMinimumSpeedKbps = 0.0;
    m_periodicMaximumSpeedKbps = 0.0;
    m_periodicSpeedSampleAvailable = false;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Internally finalizes an active reception test.
 * @param reasonText Optional stop reason; an empty string is not logged.
 * @param reasonIsError true for a red reason entry; false for a normal black entry.
 * @return none
 * @detail Creates the final snapshot before clearing state, stops the timer, clears an
 *         incomplete tail, and emits receptionStateChanged(false).
 */
void RxWorker::stopReceptionInternal(const QString &reasonText,
                                     bool reasonIsError)
{
    if (!m_testRunning)
    {
        emitReceptionState();
        return;
    }

    updateStatisticsSnapshot(false);
    m_statisticsTimer->stop();
    m_testRunning = false;
    m_receiveBuffer.clear();
    m_periodicSpeedSampleAvailable = false;
    emitReceptionState();

    if (!reasonText.trimmed().isEmpty())
    {
        emitWorkerEvent(reasonText, reasonIsError);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles critical loss of the serial port.
 * @param reason Complete reason text for the red event-log entry.
 * @return none
 * @detail Prevents recursive error handling, stops the test, closes the port, and marks
 *         portStateChanged as caused by a failure.
 */
void RxWorker::handlePortFailure(const QString &reason)
{
    if (m_handlingPortFailure
        || m_shuttingDown
        || m_serialPort == nullptr
        || (m_openPortName.isEmpty() && !m_serialPort->isOpen()))
    {
        return;
    }

    m_handlingPortFailure = true;

    QString completeReason = reason;
    if (m_testRunning)
    {
        completeReason +=
            tr("; reception stopped; received %1 bytes; counter ok=%2; "
               "counter err=%3; incomplete tail=%4 bytes")
                .arg(m_totalBytesReceived)
                .arg(m_counterOk)
                .arg(m_counterErrors)
                .arg(m_receiveBuffer.size());
        stopReceptionInternal(QString(), false);
    }

    const QString portName = m_openPortName.isEmpty()
                                 ? (m_serialPort != nullptr
                                        ? m_serialPort->portName()
                                        : QString())
                                 : m_openPortName;
    const QString settingsText = m_openPortSettingsDescription;

    emitWorkerEvent(completeReason, true);

    if (m_serialPort != nullptr && m_serialPort->isOpen())
    {
        m_openOperationInProgress = true;
        m_serialPort->close();
        m_openOperationInProgress = false;
    }

    m_receiveBuffer.clear();
    m_openPortName.clear();
    m_openPortSettingsDescription.clear();
    emit portStateChanged(false, portName, settingsText, true);
    m_handlingPortFailure = false;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Formats elapsed test time.
 * @param elapsedMilliseconds Elapsed duration in milliseconds.
 * @return A HH:MM:SS string with an unlimited number of hours.
 * @detail Hours are calculated from the full duration and do not wrap after 23.
 */
QString RxWorker::formatElapsedTime(qint64 elapsedMilliseconds) const
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
 * @brief Formats receive speed.
 * @param speedKbps Speed in decimal kilobits per second.
 * @return A string containing at most three digits after the decimal point.
 * @detail Removes trailing zeros and returns 0 for non-positive or non-numeric values.
 */
QString RxWorker::formatSpeed(double speedKbps) const
{
    if (!(speedKbps > 0.0))
    {
        return QStringLiteral("0");
    }

    QString result = QString::number(speedKbps, 'f', 3);
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
 * @brief Emits a normal or error event with an exact timestamp.
 * @param text Event text without a timestamp.
 * @param error true for an error; false for a normal entry.
 * @return none
 * @detail Creates the HH:MM:SS.mmm timestamp before queued delivery to the GUI.
 */
void RxWorker::emitWorkerEvent(const QString &text, bool error)
{
    emit eventGenerated(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
        text,
        error);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Emits the current reception state.
 * @param none
 * @return none
 * @detail Centralizes delivery of m_testRunning to the GUI.
 */
void RxWorker::emitReceptionState()
{
    emit receptionStateChanged(m_testRunning);
}
