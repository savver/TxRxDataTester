#include "txworker.h"

#include <QDateTime>
#include <QIODevice>
#include <QTimer>
#include <QTime>

#include <limits>
#include <new>

namespace
{
constexpr int kStatisticsIntervalMs = 1000;
constexpr qint64 kPeriodicLogIntervalMs = 20 * 1000;
constexpr qint64 kMinimumPendingOutputBytes = 4 * 1024;

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
 * @brief Creates the transmitter worker object.
 * @param parent Parent QObject; normally omitted before moveToThread().
 * @return none
 * @detail Initializes scalar state only; QSerialPort and timers are created later by
 *         initialize() in the worker thread.
 */
TxWorker::TxWorker(QObject *parent)
    : QObject(parent)
    , m_serialPort(nullptr)
    , m_transmitTimer(nullptr)
    , m_statisticsTimer(nullptr)
    , m_portHealthTimer(nullptr)
    , m_currentCounter(0)
    , m_totalBytesWritten(0)
    , m_lastStatisticsBytes(0)
    , m_lastStatisticsElapsedMs(0)
    , m_lastPeriodicLogBytes(0)
    , m_lastPeriodicLogElapsedMs(0)
    , m_periodicMinimumSpeedKbps(0.0)
    , m_periodicMaximumSpeedKbps(0.0)
    , m_singleBytesRemaining(0)
    , m_initialized(false)
    , m_testRunning(false)
    , m_singleTransferActive(false)
    , m_outputDrainActive(false)
    , m_collectTransmissionStatistics(false)
    , m_periodicLogStatisticsActive(false)
    , m_periodicSpeedSampleAvailable(false)
    , m_openOperationInProgress(false)
    , m_handlingPortFailure(false)
    , m_shuttingDown(false)
{
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Destroys the transmitter worker object.
 * @param none
 * @return none
 * @detail Stops timers and closes the port as a safeguard after normal shutdown.
 */
TxWorker::~TxWorker()
{
    if (m_transmitTimer != nullptr)
    {
        m_transmitTimer->stop();
    }

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
 * @brief Initializes resources owned by the TX worker thread.
 * @param none
 * @return none
 * @detail Creates QSerialPort and timers with TxWorker as parent, connects their
 *         signals, and reports readiness to the GUI.
 */
void TxWorker::initialize()
{
    if (m_initialized)
    {
        emit workerReady();
        return;
    }

    m_serialPort = new QSerialPort(this);
    m_transmitTimer = new QTimer(this);
    m_statisticsTimer = new QTimer(this);
    m_portHealthTimer = new QTimer(this);

    m_transmitTimer->setSingleShot(false);
    m_transmitTimer->setTimerType(Qt::PreciseTimer);

    m_statisticsTimer->setInterval(kStatisticsIntervalMs);
    m_statisticsTimer->setSingleShot(false);
    m_statisticsTimer->setTimerType(Qt::PreciseTimer);

    m_portHealthTimer->setInterval(1000);
    m_portHealthTimer->setSingleShot(false);
    m_portHealthTimer->setTimerType(Qt::CoarseTimer);

    connect(m_transmitTimer,
            &QTimer::timeout,
            this,
            &TxWorker::sendNextBlock);
    connect(m_statisticsTimer,
            &QTimer::timeout,
            this,
            &TxWorker::updateStatistics);
    connect(m_serialPort,
            &QSerialPort::bytesWritten,
            this,
            &TxWorker::handleBytesWritten);
    connect(m_serialPort,
            &QSerialPort::errorOccurred,
            this,
            &TxWorker::handleSerialError);
    connect(m_portHealthTimer,
            &QTimer::timeout,
            this,
            &TxWorker::checkPortHealth);

    m_portHealthTimer->start();
    m_initialized = true;
    emit transmissionStateChanged(false, false, false);
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
 * @param settingsDescription Preformatted settings description for the log.
 * @return none
 * @detail Performs all QSerialPort operations in the TX thread and reports any failure
 *         to the GUI.
 */
void TxWorker::openPort(const QString &portName,
                        qint32 baudRate,
                        int parityValue,
                        int stopBitsValue,
                        const QString &settingsDescription)
{
    if (!m_initialized || m_serialPort == nullptr)
    {
        emitWorkerEvent(tr("open error: the TX worker thread is not initialized"),
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

    if (!m_serialPort->open(QIODevice::ReadWrite))
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

    if (!m_serialPort->clear(QSerialPort::AllDirections))
    {
        const QString errorText = serialPortErrorText(m_serialPort->error());
        m_serialPort->close();
        m_openOperationInProgress = false;
        emitWorkerEvent(tr("failed to clear port %1 buffers after opening: %2")
                            .arg(portName, errorText),
                        true);
        emit portStateChanged(false, portName, settingsDescription, false);
        return;
    }

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
 * @detail Aborts active operations, clears device buffers, and closes QSerialPort in
 *         its owning thread.
 */
void TxWorker::closePort()
{
    if (!m_initialized || m_serialPort == nullptr)
    {
        emit portStateChanged(false, QString(), QString(), false);
        return;
    }

    if (!m_serialPort->isOpen())
    {
        const QString oldPortName = m_openPortName;
        const QString oldSettings = m_openPortSettingsDescription;
        abortTransfersForPortClose(false);
        m_openPortName.clear();
        m_openPortSettingsDescription.clear();
        emit portStateChanged(false, oldPortName, oldSettings, false);
        return;
    }

    const QString portName = m_openPortName.isEmpty()
                                 ? m_serialPort->portName()
                                 : m_openPortName;
    const QString settingsText = m_openPortSettingsDescription;

    abortTransfersForPortClose(false);

    m_openOperationInProgress = true;
    const bool buffersCleared = m_serialPort->clear(QSerialPort::AllDirections);
    const QString clearErrorText = buffersCleared
                                       ? QString()
                                       : serialPortErrorText(m_serialPort->error());
    m_serialPort->close();
    m_openOperationInProgress = false;

    m_openPortName.clear();
    m_openPortSettingsDescription.clear();

    if (!buffersCleared)
    {
        emitWorkerEvent(tr("failed to clear port %1 buffers before closing: %2")
                            .arg(portName, clearErrorText),
                        true);
    }

    emit portStateChanged(false, portName, settingsText, false);
    emitWorkerEvent(tr("port %1 closed: %2").arg(portName, settingsText),
                    false);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Starts continuous transmission of test blocks.
 * @param counterBits Counter width: 8, 16, 32, or 64 bits.
 * @param blockBytes Size of one block in bytes.
 * @param periodMs Block enqueue period in milliseconds.
 * @param initialValue Initial counter value.
 * @param patternDescription Preformatted Pattern description for the log.
 * @return none
 * @detail Validates the port and Pattern, resets Statistics, queues the first block,
 *         and starts the transmission timer.
 */
void TxWorker::startContinuous(int counterBits,
                               int blockBytes,
                               int periodMs,
                               quint64 initialValue,
                               const QString &patternDescription)
{
    if (!m_initialized || m_serialPort == nullptr || !m_serialPort->isOpen())
    {
        emitWorkerEvent(tr("START failed: the COM port is not open"), true);
        emitTransmissionState();
        return;
    }

    if (m_testRunning
        || m_singleTransferActive
        || m_outputDrainActive
        || m_serialPort->bytesToWrite() > 0)
    {
        emitWorkerEvent(tr("START failed: the previous transmission has not finished yet"),
                        true);
        emitTransmissionState();
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
        emitTransmissionState();
        return;
    }

    m_activePattern = settings;
    m_testRunning = true;
    m_singleTransferActive = false;
    m_outputDrainActive = false;
    m_collectTransmissionStatistics = true;
    m_periodicLogStatisticsActive = true;
    resetStatistics(settings);
    emitTransmissionState();

    emitWorkerEvent(tr("START: continuous transmission started; %1")
                        .arg(patternDescription),
                    false);

    if (!sendDataBlock(m_activePattern))
    {
        stopContinuousInternal(
            tr("continuous transmission stopped because the first block failed"),
            true,
            true);
        return;
    }

    m_transmitTimer->setSingleShot(m_activePattern.periodMs == 0);
    m_transmitTimer->setInterval(m_activePattern.periodMs);
    m_transmitTimer->start();
    m_statisticsTimer->start();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Performs a soft stop of continuous transmission.
 * @param none
 * @return none
 * @detail Stops block generation, captures bytesToWrite(), and lets the queued output
 *         drain completely.
 */
void TxWorker::stopContinuous()
{
    if (!m_testRunning || m_serialPort == nullptr)
    {
        emit stopButtonAccepted(0);
        emitTransmissionState();
        return;
    }

    m_transmitTimer->stop();
    const qint64 pendingBytes = m_serialPort->isOpen()
                                    ? qMax<qint64>(0,
                                                  m_serialPort->bytesToWrite())
                                    : 0;
    emit stopButtonAccepted(pendingBytes);
    stopContinuousInternal(QString(), false, false);
}

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
void TxWorker::sendSingle(int counterBits,
                          int blockBytes,
                          int periodMs,
                          quint64 initialValue,
                          const QString &patternDescription)
{
    if (!m_initialized || m_serialPort == nullptr || !m_serialPort->isOpen())
    {
        emitWorkerEvent(tr("SINGLE failed: the COM port is not open"), true);
        emitTransmissionState();
        return;
    }

    if (m_testRunning
        || m_singleTransferActive
        || m_outputDrainActive
        || m_serialPort->bytesToWrite() > 0)
    {
        emitWorkerEvent(tr("SINGLE failed: the previous transmission has not finished yet"),
                        true);
        emitTransmissionState();
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
        emitTransmissionState();
        return;
    }

    m_activePattern = settings;
    m_testRunning = false;
    m_singleTransferActive = true;
    m_outputDrainActive = false;
    m_collectTransmissionStatistics = true;
    m_periodicLogStatisticsActive = false;
    resetStatistics(settings);
    m_singleBytesRemaining = settings.blockBytes;
    emitTransmissionState();
    m_statisticsTimer->start();

    if (!sendDataBlock(settings))
    {
        cancelSingleTransfer(true);
        return;
    }

    emitWorkerEvent(tr("SINGLE: one block queued for transmission; %1")
                        .arg(patternDescription),
                    false);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles loss of the open port detected by the GUI.
 * @param reason Reason text created from QSerialPortInfo.
 * @return none
 * @detail Delegates handling to the common critical port-failure procedure.
 */
void TxWorker::handleExternalPortLoss(const QString &reason)
{
    handlePortFailure(reason);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Shuts down the TX worker before application exit.
 * @param none
 * @return none
 * @detail Stops transfers and timers, closes the COM port, and leaves the worker ready
 *         for QThread::quit().
 */
void TxWorker::shutdown()
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

    abortTransfersForPortClose(true);

    if (m_serialPort->isOpen())
    {
        const QString portName = m_openPortName.isEmpty()
                                     ? m_serialPort->portName()
                                     : m_openPortName;
        const QString settingsText = m_openPortSettingsDescription;

        m_openOperationInProgress = true;
        const bool buffersCleared =
            m_serialPort->clear(QSerialPort::AllDirections);
        const QString clearErrorText = buffersCleared
                                           ? QString()
                                           : serialPortErrorText(m_serialPort->error());
        m_serialPort->close();
        m_openOperationInProgress = false;

        if (!buffersCleared)
        {
            emitWorkerEvent(
                tr("failed to clear port %1 buffers during shutdown: %2")
                    .arg(portName, clearErrorText),
                true);
        }

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

    m_openPortName.clear();
    m_openPortSettingsDescription.clear();
    m_transmitTimer->stop();
    m_statisticsTimer->stop();
    m_portHealthTimer->stop();
    m_testRunning = false;
    m_singleTransferActive = false;
    m_outputDrainActive = false;
    m_collectTransmissionStatistics = false;
    m_periodicLogStatisticsActive = false;
    emitTransmissionState();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Builds and queues the next continuous-transmission block.
 * @param none
 * @return none
 * @detail Checks port state and queue capacity; period zero uses a single-shot zero
 *         timer without an endless slot loop.
 */
void TxWorker::sendNextBlock()
{
    if (!m_testRunning || m_serialPort == nullptr)
    {
        return;
    }

    if (!m_serialPort->isOpen())
    {
        handlePortFailure(
            tr("transmission stopped: the COM port is no longer open"));
        return;
    }

    const qint64 maximumPendingBytes = maximumPendingOutputBytes();
    if (m_serialPort->bytesToWrite() >= maximumPendingBytes)
    {
        return;
    }

    if (!sendDataBlock(m_activePattern))
    {
        if (m_testRunning)
        {
            stopContinuousInternal(
                tr("continuous transmission stopped because of a write error"),
                true,
                true);
        }
        return;
    }

    if (m_testRunning
        && m_activePattern.periodMs == 0
        && m_serialPort->bytesToWrite() < maximumPendingBytes)
    {
        m_transmitTimer->start(0);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Performs the one-second Statistics update.
 * @param none
 * @return none
 * @detail Calculates the current speed, updates the twenty-second minimum and maximum,
 *         and emits a prepared snapshot.
 */
void TxWorker::updateStatistics()
{
    updateStatisticsSnapshot(true);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Accounts for bytes confirmed by QSerialPort.
 * @param bytes Byte count from QSerialPort::bytesWritten.
 * @return none
 * @detail Updates total bytes, refills period-zero output, and completes SINGLE or soft
 *         STOP when bytesToWrite() reaches zero.
 */
void TxWorker::handleBytesWritten(qint64 bytes)
{
    if (!m_collectTransmissionStatistics || bytes <= 0)
    {
        return;
    }

    const quint64 unsignedBytes = static_cast<quint64>(bytes);
    const quint64 maximumValue = std::numeric_limits<quint64>::max();
    if (m_totalBytesWritten > maximumValue - unsignedBytes)
    {
        m_totalBytesWritten = maximumValue;
    }
    else
    {
        m_totalBytesWritten += unsignedBytes;
    }

    if (m_testRunning
        && m_activePattern.periodMs == 0
        && !m_transmitTimer->isActive()
        && m_serialPort->bytesToWrite() < maximumPendingOutputBytes())
    {
        m_transmitTimer->start(0);
    }

    if (m_outputDrainActive)
    {
        if (m_serialPort->bytesToWrite() == 0)
        {
            finishOutputDrain();
        }
        return;
    }

    if (!m_singleTransferActive)
    {
        return;
    }

    if (bytes >= m_singleBytesRemaining)
    {
        m_singleBytesRemaining = 0;
    }
    else
    {
        m_singleBytesRemaining -= bytes;
    }

    if (m_singleBytesRemaining > 0)
    {
        return;
    }

    updateStatisticsSnapshot(false);
    m_statisticsTimer->stop();
    m_singleTransferActive = false;
    m_collectTransmissionStatistics = false;
    m_periodicLogStatisticsActive = false;
    emitTransmissionState();

    emitWorkerEvent(tr("SINGLE: block transmission completed, %1 bytes transmitted")
                        .arg(m_totalBytesWritten),
                    false);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Checks that the logically open port remains open.
 * @param none
 * @return none
 * @detail Detects an unexpected QSerialPort close even when no separate critical
 *         errorOccurred signal was received.
 */
void TxWorker::checkPortHealth()
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
        tr("open port %1 closed unexpectedly")
            .arg(m_openPortName));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles a QSerialPort error in the worker thread.
 * @param error Serial-port error code.
 * @return none
 * @detail Logs noncritical errors and stops transmission and closes the port for
 *         critical errors.
 */
void TxWorker::handleSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError
        || m_openOperationInProgress
        || m_handlingPortFailure
        || m_shuttingDown
        || m_serialPort == nullptr)
    {
        return;
    }

    const bool portWasActive = m_serialPort->isOpen()
                               || !m_openPortName.isEmpty()
                               || m_testRunning
                               || m_singleTransferActive
                               || m_outputDrainActive;
    if (!portWasActive)
    {
        return;
    }

    const QString serialPortName = m_serialPort->portName().trimmed();
    const QString portName = !serialPortName.isEmpty()
                                 ? serialPortName
                                 : (!m_openPortName.isEmpty()
                                        ? m_openPortName
                                        : tr("unknown port"));
    const QString message =
        tr("port %1 error: %2 (code %3)")
            .arg(portName, serialPortErrorText(error))
            .arg(static_cast<int>(error));

    const bool criticalError =
        error == QSerialPort::DeviceNotFoundError
        || error == QSerialPort::PermissionError
        || error == QSerialPort::OpenError
        || error == QSerialPort::NotOpenError
        || error == QSerialPort::WriteError
        || error == QSerialPort::ReadError
        || error == QSerialPort::ResourceError
        || error == QSerialPort::UnsupportedOperationError
        || error == QSerialPort::UnknownError;

    if (criticalError)
    {
        handlePortFailure(message);
    }
    else
    {
        emitWorkerEvent(message, true);
    }
}

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
 * @detail Validates supported widths, block alignment, period, and initial-value range
 *         inside the TX thread.
 */
bool TxWorker::makePatternSettings(int counterBits,
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
        *errorText = tr("unsupported counter width");
        return false;
    }

    const int counterBytes = counterBits / 8;
    if (blockBytes <= 0 || (blockBytes % counterBytes) != 0)
    {
        *errorText = tr("block size must be positive and a multiple of %1 bytes")
                         .arg(counterBytes);
        return false;
    }

    if (periodMs < 0)
    {
        *errorText = tr("period cannot be negative");
        return false;
    }

    const quint64 maximumCounterValue =
        counterBits == 64
            ? std::numeric_limits<quint64>::max()
            : (quint64(1) << counterBits) - 1;
    if (initialValue > maximumCounterValue)
    {
        *errorText = tr("initial value does not fit in %1 bits")
                         .arg(counterBits);
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
 * @brief Resets Statistics before START or SINGLE.
 * @param settings Validated settings of the new transfer.
 * @return none
 * @detail Clears totals and interval samples, stores the start time, starts monotonic
 *         timing, and emits the initial zero-speed snapshot.
 */
void TxWorker::resetStatistics(const PatternSettings &settings)
{
    m_currentCounter = settings.initialValue;
    m_totalBytesWritten = 0;
    m_lastStatisticsBytes = 0;
    m_lastStatisticsElapsedMs = 0;
    m_lastPeriodicLogBytes = 0;
    m_lastPeriodicLogElapsedMs = 0;
    m_periodicMinimumSpeedKbps = 0.0;
    m_periodicMaximumSpeedKbps = 0.0;
    m_periodicSpeedSampleAvailable = false;
    m_singleBytesRemaining = 0;
    m_startTime = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    m_elapsedTimer.start();

    emit statisticsUpdated(m_startTime,
                           0,
                           0,
                           m_currentCounter,
                           0.0);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Updates and emits the current Statistics snapshot.
 * @param includePeriodicSample true only for the one-second timer tick.
 * @return none
 * @detail Calculates speed from byte and monotonic-time deltas and optionally includes
 *         it in the twenty-second minimum and maximum.
 */
void TxWorker::updateStatisticsSnapshot(bool includePeriodicSample)
{
    if (!m_collectTransmissionStatistics || !m_elapsedTimer.isValid())
    {
        return;
    }

    const qint64 elapsedMilliseconds = m_elapsedTimer.elapsed();
    const qint64 intervalMilliseconds =
        elapsedMilliseconds - m_lastStatisticsElapsedMs;
    const quint64 intervalBytes =
        m_totalBytesWritten >= m_lastStatisticsBytes
            ? m_totalBytesWritten - m_lastStatisticsBytes
            : 0;

    double speedKbps = 0.0;
    if (intervalMilliseconds > 0)
    {
        speedKbps = (static_cast<double>(intervalBytes) * 8.0)
                    / static_cast<double>(intervalMilliseconds);
    }

    if (m_periodicLogStatisticsActive && includePeriodicSample)
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

    m_lastStatisticsElapsedMs = elapsedMilliseconds;
    m_lastStatisticsBytes = m_totalBytesWritten;

    emit statisticsUpdated(m_startTime,
                           elapsedMilliseconds,
                           m_totalBytesWritten,
                           m_currentCounter,
                           speedKbps);
    emitPeriodicLogLineIfDue();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates the twenty-second log line when its interval expires.
 * @param none
 * @return none
 * @detail Uses the actual monotonic interval, transmitted-byte delta, current counter,
 *         and one-second minimum and maximum speeds.
 */
void TxWorker::emitPeriodicLogLineIfDue()
{
    if (!m_periodicLogStatisticsActive || !m_elapsedTimer.isValid())
    {
        return;
    }

    const qint64 elapsedMilliseconds = m_lastStatisticsElapsedMs;
    const qint64 intervalMilliseconds =
        elapsedMilliseconds - m_lastPeriodicLogElapsedMs;
    if (intervalMilliseconds < kPeriodicLogIntervalMs)
    {
        return;
    }

    const quint64 intervalBytes =
        m_totalBytesWritten >= m_lastPeriodicLogBytes
            ? m_totalBytesWritten - m_lastPeriodicLogBytes
            : 0;
    const double averageSpeedKbps =
        intervalMilliseconds > 0
            ? (static_cast<double>(intervalBytes) * 8.0)
                  / static_cast<double>(intervalMilliseconds)
            : 0.0;

    const double minimumSpeedKbps = m_periodicSpeedSampleAvailable
                                        ? m_periodicMinimumSpeedKbps
                                        : averageSpeedKbps;
    const double maximumSpeedKbps = m_periodicSpeedSampleAvailable
                                        ? m_periodicMaximumSpeedKbps
                                        : averageSpeedKbps;

    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QString logLine =
        QStringLiteral(
            "%1, time=%2, tx_bytes=%3, delta_tx_bytes=%4, curr_counter=%5, "
            "min_speed=%6, avrg_speed=%7, max_speed=%8")
            .arg(timestamp)
            .arg(formatElapsedTime(elapsedMilliseconds))
            .arg(m_totalBytesWritten)
            .arg(intervalBytes)
            .arg(m_currentCounter)
            .arg(formatSpeed(minimumSpeedKbps))
            .arg(formatSpeed(averageSpeedKbps))
            .arg(formatSpeed(maximumSpeedKbps));
    emit periodicLogLineReady(logLine);

    m_lastPeriodicLogBytes = m_totalBytesWritten;
    m_lastPeriodicLogElapsedMs = elapsedMilliseconds;
    m_periodicMinimumSpeedKbps = 0.0;
    m_periodicMaximumSpeedKbps = 0.0;
    m_periodicSpeedSampleAvailable = false;
}

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
void TxWorker::stopContinuousInternal(const QString &eventText,
                                      bool eventIsError,
                                      bool clearOutputQueue)
{
    if (!m_testRunning || m_serialPort == nullptr)
    {
        return;
    }

    m_transmitTimer->stop();
    updateStatisticsSnapshot(false);
    m_testRunning = false;

    const qint64 pendingBytes = m_serialPort->isOpen()
                                    ? qMax<qint64>(0,
                                                  m_serialPort->bytesToWrite())
                                    : 0;

    if (!clearOutputQueue && m_serialPort->isOpen())
    {
        m_outputDrainActive = true;
        m_collectTransmissionStatistics = true;
        if (!m_statisticsTimer->isActive())
        {
            m_statisticsTimer->start();
        }
    }
    else
    {
        m_outputDrainActive = false;
        m_collectTransmissionStatistics = false;
        m_periodicLogStatisticsActive = false;
        m_statisticsTimer->stop();
    }

    if (clearOutputQueue && m_serialPort->isOpen())
    {
        m_openOperationInProgress = true;
        const bool outputCleared = m_serialPort->clear(QSerialPort::Output);
        const QString clearErrorText = outputCleared
                                           ? QString()
                                           : serialPortErrorText(m_serialPort->error());
        m_openOperationInProgress = false;

        if (!outputCleared)
        {
            emitWorkerEvent(tr("failed to clear the %1 output queue: %2")
                                .arg(m_serialPort->portName(), clearErrorText),
                            true);
        }
    }

    emitTransmissionState();

    if (!eventText.isEmpty())
    {
        emitWorkerEvent(eventText, eventIsError);
    }

    if (!clearOutputQueue && pendingBytes == 0)
    {
        finishOutputDrain();
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Finishes a soft STOP after the output queue becomes empty.
 * @param none
 * @return none
 * @detail Emits the final Statistics snapshot, clears the drain state, and logs the
 *         confirmed transmitted-byte total.
 */
void TxWorker::finishOutputDrain()
{
    if (!m_outputDrainActive
        || m_serialPort == nullptr
        || !m_serialPort->isOpen()
        || m_serialPort->bytesToWrite() > 0)
    {
        return;
    }

    updateStatisticsSnapshot(false);
    m_outputDrainActive = false;
    m_collectTransmissionStatistics = false;
    m_periodicLogStatisticsActive = false;
    m_statisticsTimer->stop();
    emitTransmissionState();

    emitWorkerEvent(tr("STOP completed; the remaining output queue was transmitted; "
                       "%1 bytes transmitted in total")
                        .arg(m_totalBytesWritten),
                    false);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Cancels an unfinished SINGLE transfer.
 * @param clearOutputQueue true to clear pending output forcibly.
 * @return none
 * @detail Captures available Statistics, ends SINGLE state, and optionally discards
 *         bytes not yet transmitted.
 */
void TxWorker::cancelSingleTransfer(bool clearOutputQueue)
{
    if (!m_singleTransferActive || m_serialPort == nullptr)
    {
        return;
    }

    updateStatisticsSnapshot(false);
    m_singleBytesRemaining = 0;
    m_singleTransferActive = false;
    m_collectTransmissionStatistics = false;
    m_periodicLogStatisticsActive = false;
    m_statisticsTimer->stop();

    if (clearOutputQueue && m_serialPort->isOpen())
    {
        m_openOperationInProgress = true;
        const bool outputCleared = m_serialPort->clear(QSerialPort::Output);
        const QString clearErrorText = outputCleared
                                           ? QString()
                                           : serialPortErrorText(m_serialPort->error());
        m_openOperationInProgress = false;

        if (!outputCleared)
        {
            emitWorkerEvent(tr("failed to clear the %1 output queue: %2")
                                .arg(m_serialPort->portName(), clearErrorText),
                            true);
        }
    }

    emitTransmissionState();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles critical loss of the serial port.
 * @param reason Complete reason text for the red event-log entry.
 * @return none
 * @detail Stops timers and Statistics, closes the device with recursive-error
 *         protection, and reports a failed closed state.
 */
void TxWorker::handlePortFailure(const QString &reason)
{
    if (m_handlingPortFailure || m_shuttingDown || m_serialPort == nullptr)
    {
        return;
    }

    const bool portWasActive = m_serialPort->isOpen()
                               || !m_openPortName.isEmpty()
                               || m_testRunning
                               || m_singleTransferActive
                               || m_outputDrainActive;
    if (!portWasActive)
    {
        emit portStateChanged(false, QString(), QString(), true);
        return;
    }

    m_handlingPortFailure = true;
    const QString portName = !m_openPortName.isEmpty()
                                 ? m_openPortName
                                 : (!m_serialPort->portName().trimmed().isEmpty()
                                        ? m_serialPort->portName().trimmed()
                                        : tr("unknown port"));
    const QString settingsText = m_openPortSettingsDescription;

    if (m_testRunning)
    {
        m_transmitTimer->stop();
        updateStatisticsSnapshot(false);
        m_testRunning = false;
        emitWorkerEvent(
            tr("continuous transmission stopped because the COM port was lost"),
            true);
    }

    if (m_singleTransferActive)
    {
        updateStatisticsSnapshot(false);
        m_singleTransferActive = false;
        m_singleBytesRemaining = 0;
        emitWorkerEvent(
            tr("single-block transmission was interrupted because the COM port was lost"),
            true);
    }

    if (m_outputDrainActive)
    {
        updateStatisticsSnapshot(false);
        m_outputDrainActive = false;
        emitWorkerEvent(
            tr("transmission of the remaining STOP queue was interrupted because the COM port was lost"),
            true);
    }

    m_collectTransmissionStatistics = false;
    m_periodicLogStatisticsActive = false;
    m_statisticsTimer->stop();

    m_openOperationInProgress = true;
    if (m_serialPort->isOpen())
    {
        m_serialPort->clear(QSerialPort::AllDirections);
        m_serialPort->close();
    }
    m_openOperationInProgress = false;

    m_openPortName.clear();
    m_openPortSettingsDescription.clear();
    emitTransmissionState();
    emit portStateChanged(false, portName, settingsText, true);
    emitWorkerEvent(reason, true);
    m_handlingPortFailure = false;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Aborts active transfers before CLOSE or shutdown.
 * @param shutdownMode true while the entire application is shutting down.
 * @return none
 * @detail Captures Statistics and logs normal completion messages; the caller performs
 *         the actual buffer cleanup.
 */
void TxWorker::abortTransfersForPortClose(bool shutdownMode)
{
    const QString operationSuffix = shutdownMode
                                        ? tr("during application shutdown")
                                        : tr("before closing the port");

    if (m_testRunning)
    {
        m_transmitTimer->stop();
        updateStatisticsSnapshot(false);
        m_testRunning = false;
        emitWorkerEvent(tr("continuous transmission stopped %1")
                            .arg(operationSuffix),
                        false);
    }

    if (m_singleTransferActive)
    {
        updateStatisticsSnapshot(false);
        m_singleTransferActive = false;
        m_singleBytesRemaining = 0;
        emitWorkerEvent(tr("single-block transmission interrupted %1")
                            .arg(operationSuffix),
                        false);
    }

    if (m_outputDrainActive)
    {
        const qint64 pendingBytes = m_serialPort != nullptr
                                            && m_serialPort->isOpen()
                                        ? qMax<qint64>(
                                              0,
                                              m_serialPort->bytesToWrite())
                                        : 0;
        updateStatisticsSnapshot(false);
        m_outputDrainActive = false;
        emitWorkerEvent(
            tr("waiting for the remaining queue was interrupted %1; %2 bytes remained in the queue")
                .arg(operationSuffix)
                .arg(pendingBytes),
            false);
    }

    m_collectTransmissionStatistics = false;
    m_periodicLogStatisticsActive = false;
    if (m_statisticsTimer != nullptr)
    {
        m_statisticsTimer->stop();
    }
    emitTransmissionState();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Builds and writes one block to QSerialPort.
 * @param settings Settings of the block to build and transmit.
 * @return true when the complete block is accepted by the output buffer.
 * @detail Reports memory, write, and partial-write errors; advances the counter only
 *         after the full QByteArray is accepted.
 */
bool TxWorker::sendDataBlock(const PatternSettings &settings)
{
    if (m_serialPort == nullptr || !m_serialPort->isOpen())
    {
        emitWorkerEvent(tr("transmission error: the COM port is not open"), true);
        return false;
    }

    QByteArray block;
    quint64 nextCounterValue = m_currentCounter;

    try
    {
        block = buildDataBlock(settings,
                               m_currentCounter,
                               &nextCounterValue);
    }
    catch (const std::bad_alloc &)
    {
        emitWorkerEvent(
            tr("block-generation error: not enough memory for %1 bytes")
                .arg(settings.blockBytes),
            true);
        return false;
    }

    const qint64 acceptedBytes = m_serialPort->write(block);
    if (acceptedBytes != block.size())
    {
        if (acceptedBytes < 0)
        {
            emitWorkerEvent(tr("failed to write a block to %1: %2")
                                .arg(m_serialPort->portName(),
                                     serialPortErrorText(m_serialPort->error())),
                            true);
        }
        else
        {
            emitWorkerEvent(tr("partial write to %1: %2 of %3 bytes accepted")
                                .arg(m_serialPort->portName())
                                .arg(acceptedBytes)
                                .arg(block.size()),
                            true);
        }

        return false;
    }

    m_currentCounter = nextCounterValue;
    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns the allowed amount of pending output.
 * @param none
 * @return Maximum bytesToWrite window in bytes.
 * @detail Uses at least 4096 bytes for small blocks or four blocks for large ones to
 *         limit the soft-STOP tail.
 */
qint64 TxWorker::maximumPendingOutputBytes() const
{
    const qint64 blockWindow =
        static_cast<qint64>(m_activePattern.blockBytes) * 4;
    return qMax(kMinimumPendingOutputBytes, blockWindow);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates a binary block of sequential counter values.
 * @param settings Block and counter settings.
 * @param firstValue First counter value in the block.
 * @param nextValue Output pointer for the next block value.
 * @return Constructed QByteArray.
 * @detail Writes each value least-significant byte first and wraps the counter to zero
 *         after its maximum.
 */
QByteArray TxWorker::buildDataBlock(const PatternSettings &settings,
                                    quint64 firstValue,
                                    quint64 *nextValue) const
{
    QByteArray block;
    block.resize(settings.blockBytes);

    char *data = block.data();
    int outputIndex = 0;
    quint64 counterValue = firstValue;
    const int valuesInBlock = settings.blockBytes / settings.counterBytes;

    for (int valueIndex = 0; valueIndex < valuesInBlock; ++valueIndex)
    {
        for (int byteIndex = 0;
             byteIndex < settings.counterBytes;
             ++byteIndex)
        {
            const int shift = byteIndex * 8;
            data[outputIndex] = static_cast<char>(
                (counterValue >> shift) & quint64(0xFFU));
            ++outputIndex;
        }

        if (counterValue == settings.maximumCounterValue)
        {
            counterValue = 0;
        }
        else
        {
            ++counterValue;
        }
    }

    if (nextValue != nullptr)
    {
        *nextValue = counterValue;
    }

    return block;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Formats elapsed test time.
 * @param elapsedMilliseconds Elapsed duration in milliseconds.
 * @return HH:MM:SS string with an unlimited number of hours.
 * @detail Hours are calculated from the full duration and do not wrap after 23.
 */
QString TxWorker::formatElapsedTime(qint64 elapsedMilliseconds) const
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
 * @brief Formats transmission speed.
 * @param speedKbps Speed in decimal kilobits per second.
 * @return String containing at most three digits after the decimal point.
 * @detail Removes trailing zeros and the decimal point when the fractional part is
 *         zero.
 */
QString TxWorker::formatSpeed(double speedKbps) const
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
 * @detail Creates the HH:MM:SS.mmm timestamp inside the TX thread before queued
 *         delivery to the GUI.
 */
void TxWorker::emitWorkerEvent(const QString &text, bool error)
{
    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    emit eventGenerated(timestamp, text, error);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Emits the current transmission states.
 * @param none
 * @return none
 * @detail Sends only three Boolean flags to the GUI and never exposes QSerialPort or
 *         QTimer objects across threads.
 */
void TxWorker::emitTransmissionState()
{
    emit transmissionStateChanged(m_testRunning,
                                  m_singleTransferActive,
                                  m_outputDrainActive);
}
