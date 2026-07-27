#include "mainwindow.h"
#include "rxworker.h"
#include "udprxworker.h"
#include "ui_mainwindow.h"

#include <QAbstractSocket>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFileDevice>
#include <QFont>
#include <QFontDatabase>
#include <QHostAddress>
#include <QIODevice>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QSignalBlocker>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QUdpSocket>

#include <limits>

namespace
{
constexpr int kPortRefreshIntervalMs = 1000;
constexpr int kPingRequestCount = 1;
constexpr int kPingReplyTimeoutMs = 500;
constexpr int kPingProcessTimeoutMs = 1800;
constexpr int kRouteProbeTimeoutMs = 1000;
constexpr quint64 kMaximumIpv4UdpPayloadBytes = 65507;

/**
 * @brief Returns a fixed English description for a file error.
 * @param error QFileDevice error value to describe.
 * @return English text that does not depend on the operating-system language.
 * @detail Converts QFileDevice::FileError values used during log creation into concise
 *         messages suitable for EVENTS.
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

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns a fixed English description for a process error.
 * @param error QProcess error value to describe.
 * @return English text that does not depend on the operating-system language.
 * @detail Converts process-start and process-I/O failures used by ping and neighbor
 *         lookup into concise EVENTS messages.
 */
QString processErrorText(QProcess::ProcessError error)
{
    switch (error)
    {
    case QProcess::FailedToStart:
        return QStringLiteral("failed to start the process");
    case QProcess::Crashed:
        return QStringLiteral("the process crashed");
    case QProcess::Timedout:
        return QStringLiteral("the process operation timed out");
    case QProcess::WriteError:
        return QStringLiteral("process write error");
    case QProcess::ReadError:
        return QStringLiteral("process read error");
    case QProcess::UnknownError:
        return QStringLiteral("unknown process error");
    }

    return QStringLiteral("unrecognized process error");
}
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates the main application window.
 * @param parent Parent widget of the window.
 * @return none
 * @detail Configures the GUI, validators, event log, settings persistence, and the
 *         dedicated COM and UDP receiver worker threads.
 */
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_rxWorker(nullptr)
    , m_udpRxWorker(nullptr)
    , m_pingProcess(nullptr)
    , m_neighborLookupProcess(nullptr)
    , m_portRefreshTimer(this)
    , m_pingTimeoutTimer(this)
    , m_udpDestinationPort(0)
    , m_workerReady(false)
    , m_udpWorkerReady(false)
    , m_portOpen(false)
    , m_testRunning(false)
    , m_portOperationPending(false)
    , m_receptionCommandPending(false)
    , m_portLossRequestPending(false)
    , m_portSnapshotInitialized(false)
    , m_udpConnected(false)
    , m_udpConnectionOperationPending(false)
    , m_udpTestRunning(false)
    , m_udpReceptionCommandPending(false)
    , m_udpDisconnectRequestedByUser(false)
    , m_pingInProgress(false)
    , m_neighborLookupInProgress(false)
    , m_shutdownPrepared(false)
{
    ui->setupUi(this);

    ui->settingsGridLayout->setColumnStretch(1, 1);
    ui->settingsGridLayout->setColumnStretch(4, 1);
    ui->patternGridLayout->setColumnStretch(1, 1);
    ui->patternGridLayout->setColumnStretch(4, 1);
    ui->statisticsGridLayout->setColumnStretch(1, 1);
    ui->statisticsGridLayout->setColumnStretch(4, 1);
    ui->udpConnectionGridLayout->setColumnStretch(1, 1);
    ui->udpConnectionGridLayout->setColumnStretch(4, 1);
    ui->udpPatternGridLayout->setColumnStretch(1, 1);
    ui->udpPatternGridLayout->setColumnStretch(4, 1);
    ui->udpStatisticsGridLayout->setColumnStretch(1, 1);
    ui->udpStatisticsGridLayout->setColumnStretch(4, 1);

    const QRegularExpression decimalExpression(QStringLiteral("[0-9]{0,10}"));
    ui->blockBytesLineEdit->setValidator(
        new QRegularExpressionValidator(decimalExpression, this));
    ui->periodMsLineEdit->setValidator(
        new QRegularExpressionValidator(decimalExpression, this));
    ui->udpBlockBytesLineEdit->setValidator(
        new QRegularExpressionValidator(decimalExpression, this));
    ui->udpPeriodMsLineEdit->setValidator(
        new QRegularExpressionValidator(decimalExpression, this));
    ui->udpTogetherLineEdit->setValidator(
        new QRegularExpressionValidator(decimalExpression, this));

    const QRegularExpression initialValueExpression(
        QStringLiteral("(?:0[xX][0-9A-Fa-f]{0,16}|[0-9]{0,20})"));
    ui->initValueLineEdit->setValidator(
        new QRegularExpressionValidator(initialValueExpression, this));
    ui->udpInitValueLineEdit->setValidator(
        new QRegularExpressionValidator(initialValueExpression, this));

    const QRegularExpression ipv4CharacterExpression(
        QStringLiteral("[0-9.]{0,15}"));
    ui->udpDestinationIpLineEdit->setValidator(
        new QRegularExpressionValidator(ipv4CharacterExpression, this));

    const QRegularExpression udpPortExpression(QStringLiteral("[0-9]{0,5}"));
    ui->udpDestinationPortLineEdit->setValidator(
        new QRegularExpressionValidator(udpPortExpression, this));

    QFont eventsFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    eventsFont.setPointSize(10);
    ui->eventsPlainTextEdit->setFont(eventsFont);
    ui->eventsPlainTextEdit->document()->setMaximumBlockCount(10000);
    ui->eventsPlainTextEdit->setStyleSheet(
        QStringLiteral("QPlainTextEdit {"
                       " background-color: #FCFCFC;"
                       " border: 1px solid #8A8A8A;"
                       " border-radius: 5px;"
                       " padding: 6px;"
                       " selection-background-color: #B8D8FF;"
                       " selection-color: #000000;"
                       "}"));

    QFont eventsLabelFont = ui->eventsLabel->font();
    eventsLabelFont.setBold(true);
    ui->eventsLabel->setFont(eventsLabelFont);

    m_portRefreshTimer.setInterval(kPortRefreshIntervalMs);
    m_portRefreshTimer.setSingleShot(false);
    m_portRefreshTimer.setTimerType(Qt::CoarseTimer);

    connect(&m_portRefreshTimer,
            &QTimer::timeout,
            this,
            &MainWindow::refreshSerialPorts);
    connect(ui->openButton,
            &QPushButton::clicked,
            this,
            &MainWindow::openSerialPort);
    connect(ui->closeButton,
            &QPushButton::clicked,
            this,
            &MainWindow::closeSerialPort);
    connect(ui->blockBytesLineEdit,
            &QLineEdit::editingFinished,
            this,
            &MainWindow::normalizeBlockSize);
    connect(ui->blockBytesLineEdit,
            &QLineEdit::textChanged,
            this,
            &MainWindow::updateBlockTransmissionTime);
    connect(ui->initValueLineEdit,
            &QLineEdit::editingFinished,
            this,
            &MainWindow::normalizeInitialValue);
    connect(ui->periodMsLineEdit,
            &QLineEdit::editingFinished,
            this,
            &MainWindow::normalizePeriod);
    connect(ui->counterBitsComboBox,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &MainWindow::handleCounterBitsChanged);
    connect(ui->baudComboBox,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &MainWindow::updateBlockTransmissionTime);
    connect(ui->parityComboBox,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &MainWindow::updateBlockTransmissionTime);
    connect(ui->stopsComboBox,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &MainWindow::updateBlockTransmissionTime);
    connect(ui->startButton,
            &QPushButton::clicked,
            this,
            &MainWindow::startTest);
    connect(ui->stopButton,
            &QPushButton::clicked,
            this,
            &MainWindow::stopTest);
    connect(ui->udpBlockBytesLineEdit,
            &QLineEdit::editingFinished,
            this,
            &MainWindow::normalizeUdpBlockSize);
    connect(ui->udpInitValueLineEdit,
            &QLineEdit::editingFinished,
            this,
            &MainWindow::normalizeUdpInitialValue);
    connect(ui->udpPeriodMsLineEdit,
            &QLineEdit::editingFinished,
            this,
            &MainWindow::normalizeUdpPeriod);
    connect(ui->udpTogetherLineEdit,
            &QLineEdit::editingFinished,
            this,
            &MainWindow::normalizeUdpTogether);
    connect(ui->udpCounterBitsComboBox,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &MainWindow::handleUdpCounterBitsChanged);
    connect(ui->udpConnectButton,
            &QPushButton::clicked,
            this,
            &MainWindow::handleUdpConnectButton);
    connect(ui->udpStartButton,
            &QPushButton::clicked,
            this,
            &MainWindow::startUdpTest);
    connect(ui->udpStopButton,
            &QPushButton::clicked,
            this,
            &MainWindow::stopUdpTest);
    connect(ui->udpDestinationIpLineEdit,
            &QLineEdit::textChanged,
            this,
            [this](const QString &)
            {
                if (!m_udpConnected
                    && !m_pingInProgress
                    && !m_neighborLookupInProgress
                    && !m_udpConnectionOperationPending)
                {
                    clearUdpNetworkInformation();
                }
                updateControlStates();
            });
    connect(ui->udpDestinationPortLineEdit,
            &QLineEdit::textChanged,
            this,
            [this](const QString &)
            {
                if (!m_udpConnected
                    && !m_pingInProgress
                    && !m_neighborLookupInProgress
                    && !m_udpConnectionOperationPending)
                {
                    clearUdpNetworkInformation();
                }
                updateControlStates();
            });

    m_pingTimeoutTimer.setSingleShot(true);
    m_pingTimeoutTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_pingTimeoutTimer,
            &QTimer::timeout,
            this,
            &MainWindow::handlePingTimeout);

    m_pingProcess = new QProcess(this);
    m_pingProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_pingProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &MainWindow::handlePingFinished);
    connect(m_pingProcess,
            &QProcess::errorOccurred,
            this,
            &MainWindow::handlePingProcessError);

    m_neighborLookupProcess = new QProcess(this);
    m_neighborLookupProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_neighborLookupProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &MainWindow::handleNeighborLookupFinished);
    connect(m_neighborLookupProcess,
            &QProcess::errorOccurred,
            this,
            &MainWindow::handleNeighborLookupProcessError);

    initializeLogFile();
    appendEvent(tr("RxDataTester (v.1.4) started"), EventType::Normal);

    if (m_logFile.isOpen())
    {
        appendEvent(tr("log file created: %1")
                        .arg(QDir::toNativeSeparators(m_logFile.fileName())),
                    EventType::Normal);
    }

    loadSettings();
    normalizeBlockSize();
    normalizeInitialValue();
    normalizePeriod();
    updateBlockTransmissionTime();
    normalizeUdpBlockSize();
    normalizeUdpInitialValue();
    normalizeUdpTogether();
    normalizeUdpPeriod();
    clearUdpNetworkInformation();

    ui->startTimeValueLabel->setText(QStringLiteral("--:--:--"));
    ui->elapsedTimeValueLabel->setText(QStringLiteral("00:00:00"));
    ui->rxBytesLineEdit->setText(QStringLiteral("0"));
    ui->speedLineEdit->setText(QStringLiteral("0"));
    ui->counterOkLineEdit->setText(QStringLiteral("0"));
    ui->counterErrLineEdit->setText(QStringLiteral("0"));

    quint64 initialValue = 0;
    parseInitialValue(&initialValue);
    ui->currentCountLineEdit->setText(QString::number(initialValue));

    ui->udpStartTimeValueLabel->setText(QStringLiteral("--:--:--"));
    ui->udpElapsedTimeValueLabel->setText(QStringLiteral("00:00:00"));
    ui->udpRxBytesLineEdit->setText(QStringLiteral("0"));
    ui->udpPacketsPerSecondLineEdit->setText(QStringLiteral("0"));
    ui->udpSpeedLineEdit->setText(QStringLiteral("0"));
    ui->udpCounterOkLineEdit->setText(QStringLiteral("0"));
    ui->udpCounterErrLineEdit->setText(QStringLiteral("0"));

    quint64 udpInitialValue = 0;
    parseUdpInitialValue(&udpInitialValue);
    ui->udpCurrentCountLineEdit->setText(QString::number(udpInitialValue));

    initializeRxThread();
    initializeUdpRxThread();
    refreshSerialPorts();
    updateControlStates();
    m_portRefreshTimer.start();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Destroys the main application window.
 * @param none
 * @return none
 * @detail Ensures orderly shutdown of both receiver threads, closes the log file, and
 *         releases the Qt Designer user interface.
 */
MainWindow::~MainWindow()
{
    if (!m_shutdownPrepared)
    {
        prepareShutdown();
    }

    delete ui;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles closing of the main window.
 * @param event Qt close event for the window.
 * @return none
 * @detail Synchronously stops both receiver threads, saves the settings, closes the
 *         log, and then passes the event to the base QMainWindow implementation.
 */
void MainWindow::closeEvent(QCloseEvent *event)
{
    prepareShutdown();
    QMainWindow::closeEvent(event);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Checks the list of available serial ports.
 * @param none
 * @return none
 * @detail When the port is closed, updates the combo box and logs added or removed
 *         devices. When the port is open, monitors the selected device.
 */
void MainWindow::refreshSerialPorts()
{
    if (m_shutdownPrepared)
    {
        return;
    }

    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    const QSet<QString> currentPortNames = portNames(ports);
    const QHash<QString, QString> currentDescriptions = portDescriptions(ports);

    if (m_portOpen)
    {
        if (!m_openPortName.isEmpty()
            && !currentPortNames.contains(m_openPortName)
            && !m_portLossRequestPending)
        {
            m_portLossRequestPending = true;
            updateControlStates();
            emit externalPortLossDetected(
                tr("open port %1 was disconnected from the system")
                    .arg(m_openPortName));
        }

        m_knownPortNames = currentPortNames;
        m_knownPortDescriptions = currentDescriptions;
        m_portSnapshotInitialized = true;
        return;
    }

    if (m_portSnapshotInitialized)
    {
        QSet<QString> addedPorts = currentPortNames;
        addedPorts.subtract(m_knownPortNames);

        QSet<QString> removedPorts = m_knownPortNames;
        removedPorts.subtract(currentPortNames);

        QStringList addedNames = addedPorts.values();
        QStringList removedNames = removedPorts.values();
        addedNames.sort(Qt::CaseInsensitive);
        removedNames.sort(Qt::CaseInsensitive);

        for (const QString &portName : addedNames)
        {
            appendEvent(tr("new port detected: %1")
                            .arg(currentDescriptions.value(portName,
                                                           portName)),
                        EventType::Normal);
        }

        for (const QString &portName : removedNames)
        {
            appendEvent(tr("port disappeared from the available-port list: %1")
                            .arg(m_knownPortDescriptions.value(portName,
                                                              portName)),
                        EventType::Normal);
        }
    }

    updatePortComboBox(ports);
    m_knownPortNames = currentPortNames;
    m_knownPortDescriptions = currentDescriptions;
    m_portSnapshotInitialized = true;
    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Requests opening of the selected serial port.
 * @param none
 * @return none
 * @detail Logs the OPEN button press in green, validates the selected settings, and
 *         sends a queued command to RxWorker.
 */
void MainWindow::openSerialPort()
{
    if (m_portOperationPending || m_shutdownPrepared)
    {
        return;
    }

    appendEvent(tr("OPEN button pressed"), EventType::Action);

    if (m_udpConnected
        || m_pingInProgress
        || m_neighborLookupInProgress
        || m_udpConnectionOperationPending)
    {
        appendEvent(tr("open error: disconnect the UDP destination before opening a COM port"),
                    EventType::Error);
        return;
    }

    if (!m_workerReady || m_portOpen || m_rxWorker == nullptr)
    {
        appendEvent(tr("OPEN failed: the RX worker thread is not ready"),
                    EventType::Error);
        return;
    }

    const QString portName = ui->portComboBox->currentText().trimmed();
    if (portName.isEmpty())
    {
        appendEvent(tr("open error: no available COM port is selected"),
                    EventType::Error);
        return;
    }

    bool baudOk = false;
    const qint32 baudRate =
        ui->baudComboBox->currentText().toInt(&baudOk, 10);
    if (!baudOk || baudRate <= 0)
    {
        appendEvent(tr("open error: invalid COM-port baud rate"),
                    EventType::Error);
        return;
    }

    m_portOperationPending = true;
    updateControlStates();

    emit openPortRequested(portName,
                           baudRate,
                           static_cast<int>(selectedParity()),
                           static_cast<int>(selectedStopBits()),
                           serialSettingsDescription());
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Requests closing of the open serial port.
 * @param none
 * @return none
 * @detail Logs the CLOSE button press in green and sends the close command to the
 *         object that owns the port.
 */
void MainWindow::closeSerialPort()
{
    if (m_portOperationPending || m_shutdownPrepared)
    {
        return;
    }

    appendEvent(tr("CLOSE button pressed"), EventType::Action);

    if (!m_workerReady || !m_portOpen || m_rxWorker == nullptr)
    {
        appendEvent(tr("CLOSE failed: the COM port is not open"), EventType::Error);
        updateControlStates();
        return;
    }

    m_portOperationPending = true;
    m_receptionCommandPending = true;
    updateControlStates();
    emit closePortRequested();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Normalizes the informational block size.
 * @param none
 * @return none
 * @detail Rounds block, bytes upward to a multiple of the counter size and limits the
 *         value to the positive int range.
 */
void MainWindow::normalizeBlockSize()
{
    const quint64 alignment = counterBytes();
    const quint64 maximumInt =
        static_cast<quint64>(std::numeric_limits<int>::max());
    const quint64 maximumAligned = maximumInt - (maximumInt % alignment);

    bool conversionOk = false;
    quint64 blockSize =
        ui->blockBytesLineEdit->text().trimmed().toULongLong(&conversionOk, 10);

    if (!conversionOk || blockSize == 0)
    {
        blockSize = alignment;
    }

    if (blockSize > maximumAligned)
    {
        blockSize = maximumAligned;
    }
    else if ((blockSize % alignment) != 0)
    {
        blockSize = ((blockSize + alignment - 1) / alignment) * alignment;
    }

    ui->blockBytesLineEdit->setText(QString::number(blockSize));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Validates the initial counter value.
 * @param none
 * @return none
 * @detail Supports decimal input and hexadecimal input with the 0x prefix while
 *         preserving the user-selected format after normalization.
 */
void MainWindow::normalizeInitialValue()
{
    const QString originalText = ui->initValueLineEdit->text().trimmed();
    const bool hexadecimal =
        originalText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive);

    quint64 value = 0;
    if (!parseInitialValue(&value))
    {
        ui->initValueLineEdit->setText(QStringLiteral("0"));
        return;
    }

    if (hexadecimal)
    {
        ui->initValueLineEdit->setText(
            QStringLiteral("0x") + QString::number(value, 16).toUpper());
    }
    else
    {
        ui->initValueLineEdit->setText(QString::number(value));
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Normalizes the informational block period.
 * @param none
 * @return none
 * @detail Replaces an empty or invalid field with zero and limits an oversized value to
 *         the maximum int value.
 */
void MainWindow::normalizePeriod()
{
    bool conversionOk = false;
    quint64 period =
        ui->periodMsLineEdit->text().trimmed().toULongLong(&conversionOk, 10);

    if (!conversionOk)
    {
        period = 0;
    }

    const quint64 maximumPeriod =
        static_cast<quint64>(std::numeric_limits<int>::max());
    if (period > maximumPeriod)
    {
        period = maximumPeriod;
    }

    ui->periodMsLineEdit->setText(QString::number(period));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Updates the calculated transmission time of one block.
 * @param none
 * @return none
 * @detail Calculates the theoretical duration from baud, parity, stop bits, and block
 *         bytes, then displays it as block len, us => N.
 */
void MainWindow::updateBlockTransmissionTime()
{
    bool blockOk = false;
    const quint64 blockBytes =
        ui->blockBytesLineEdit->text().trimmed().toULongLong(&blockOk, 10);

    bool baudOk = false;
    const quint64 baudRate =
        ui->baudComboBox->currentText().trimmed().toULongLong(&baudOk, 10);

    if (!blockOk || !baudOk || blockBytes == 0 || baudRate == 0)
    {
        ui->blockLengthUsLabel->setText(
            QStringLiteral("block len, us => 0"));
        return;
    }

    const quint64 parityBits =
        selectedParity() == QSerialPort::NoParity ? 0U : 1U;
    const quint64 stopBits =
        selectedStopBits() == QSerialPort::TwoStop ? 2U : 1U;
    const quint64 bitsPerByte = 1U + 8U + parityBits + stopBits;

    const quint64 totalBitMicroseconds =
        blockBytes * bitsPerByte * quint64(1000000U);
    const quint64 blockLengthUs =
        (totalBitMicroseconds + baudRate - 1U) / baudRate;

    ui->blockLengthUsLabel->setText(
        tr("block len, us => %1").arg(blockLengthUs));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles a change of counter bit width.
 * @param none
 * @return none
 * @detail Realigns block, bytes and validates the init value range again.
 */
void MainWindow::handleCounterBitsChanged()
{
    normalizeBlockSize();
    normalizeInitialValue();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Requests start of reception and counter verification.
 * @param none
 * @return none
 * @detail Logs the START button press in green, validates Pattern, and sends the
 *         validated scalar values to the RX worker thread.
 */
void MainWindow::startTest()
{
    if (m_receptionCommandPending || m_testRunning || m_shutdownPrepared)
    {
        return;
    }

    appendEvent(tr("START button pressed"), EventType::Action);

    if (!m_workerReady || !m_portOpen || m_rxWorker == nullptr)
    {
        appendEvent(tr("START failed: the COM port is not open"), EventType::Error);
        return;
    }

    normalizeBlockSize();
    normalizeInitialValue();
    normalizePeriod();

    PatternSettings settings;
    QString errorText;
    if (!readPatternSettings(&settings, &errorText))
    {
        appendEvent(tr("Pattern error: %1").arg(errorText), EventType::Error);
        return;
    }

    m_receptionCommandPending = true;
    updateControlStates();
    emit startReceptionRequested(settings.counterBits,
                                 settings.blockBytes,
                                 settings.periodMs,
                                 settings.initialValue,
                                 patternDescription(settings));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Requests stop of reception and counter verification.
 * @param none
 * @return none
 * @detail Logs the STOP button press in green and waits for the final black service
 *         entry from RxWorker.
 */
void MainWindow::stopTest()
{
    if (!m_testRunning
        || m_receptionCommandPending
        || !m_workerReady
        || m_rxWorker == nullptr
        || m_shutdownPrepared)
    {
        return;
    }

    appendEvent(tr("STOP button pressed"), EventType::Action);
    m_receptionCommandPending = true;
    updateControlStates();
    emit stopReceptionRequested();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Normalizes the UDP informational block size.
 * @param none
 * @return none
 * @detail Rounds block, bytes upward to a multiple of the selected UDP counter size
 *         and clamps it to the IPv4 UDP payload limit.
 */
void MainWindow::normalizeUdpBlockSize()
{
    const quint64 alignment = udpCounterBytes();
    const quint64 maximumAligned =
        kMaximumIpv4UdpPayloadBytes - (kMaximumIpv4UdpPayloadBytes % alignment);

    bool conversionOk = false;
    quint64 blockSize =
        ui->udpBlockBytesLineEdit->text().trimmed().toULongLong(
            &conversionOk,
            10);

    if (!conversionOk || blockSize == 0)
    {
        blockSize = alignment;
    }

    if (blockSize > maximumAligned)
    {
        blockSize = maximumAligned;
    }
    else if ((blockSize % alignment) != 0)
    {
        blockSize = ((blockSize + alignment - 1U) / alignment) * alignment;
    }

    ui->udpBlockBytesLineEdit->setText(QString::number(blockSize));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Validates the UDP initial counter value.
 * @param none
 * @return none
 * @detail Accepts decimal and 0x-prefixed hexadecimal input and mirrors the normalized
 *         value in the UDP curr_count field.
 */
void MainWindow::normalizeUdpInitialValue()
{
    const QString originalText = ui->udpInitValueLineEdit->text().trimmed();
    const bool hexadecimal =
        originalText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive);

    quint64 value = 0;
    if (!parseUdpInitialValue(&value))
    {
        ui->udpInitValueLineEdit->setText(QStringLiteral("0"));
        ui->udpCurrentCountLineEdit->setText(QStringLiteral("0"));
        return;
    }

    if (hexadecimal)
    {
        ui->udpInitValueLineEdit->setText(
            QStringLiteral("0x") + QString::number(value, 16).toUpper());
    }
    else
    {
        ui->udpInitValueLineEdit->setText(QString::number(value));
    }

    ui->udpCurrentCountLineEdit->setText(QString::number(value));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Normalizes the UDP informational packet period.
 * @param none
 * @return none
 * @detail Converts an empty field to zero and clamps values above the maximum QTimer
 *         interval representable by int.
 */
void MainWindow::normalizeUdpPeriod()
{
    bool conversionOk = false;
    quint64 period =
        ui->udpPeriodMsLineEdit->text().trimmed().toULongLong(
            &conversionOk,
            10);

    if (!conversionOk)
    {
        period = 0;
    }

    const quint64 maximumPeriod =
        static_cast<quint64>(std::numeric_limits<int>::max());
    if (period > maximumPeriod)
    {
        period = maximumPeriod;
    }

    ui->udpPeriodMsLineEdit->setText(QString::number(period));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Normalizes the UDP Togeth packet count.
 * @param none
 * @return none
 * @detail Converts empty or zero input to one and clamps values to int range.
 */
void MainWindow::normalizeUdpTogether()
{
    bool conversionOk = false;
    quint64 togetherCount =
        ui->udpTogetherLineEdit->text().trimmed().toULongLong(
            &conversionOk,
            10);

    if (!conversionOk || togetherCount == 0)
    {
        togetherCount = 1;
    }

    const quint64 maximumTogether =
        static_cast<quint64>(std::numeric_limits<int>::max());
    if (togetherCount > maximumTogether)
    {
        togetherCount = maximumTogether;
    }

    ui->udpTogetherLineEdit->setText(QString::number(togetherCount));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles a change of the UDP counter width.
 * @param none
 * @return none
 * @detail Realigns UDP block, bytes and validates the UDP init value range again.
 */
void MainWindow::handleUdpCounterBitsChanged()
{
    normalizeUdpBlockSize();
    normalizeUdpInitialValue();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Connects or disconnects the UDP receiver endpoint.
 * @param none
 * @return none
 * @detail CONNECT validates the transmitter IP and local listening Port, resolves the
 *         local interface, runs one short-timeout ping, and then binds the receiver
 *         socket. DISCONNECT closes the socket in UdpRxWorker.
 */
void MainWindow::handleUdpConnectButton()
{
    if (m_shutdownPrepared
        || m_pingInProgress
        || m_udpConnectionOperationPending
        || m_udpReceptionCommandPending)
    {
        return;
    }

    if (m_udpConnected)
    {
        if (!m_udpWorkerReady
            || m_udpRxWorker == nullptr
            || m_udpTestRunning)
        {
            updateControlStates();
            return;
        }

        appendEvent(tr("DISCONNECT button pressed"), EventType::Action);

        if (m_neighborLookupProcess != nullptr
            && m_neighborLookupProcess->state() != QProcess::NotRunning)
        {
            m_neighborLookupInProgress = false;
            m_neighborLookupProcess->kill();
            m_neighborLookupProcess->waitForFinished(100);
            m_neighborLookupProcess->readAll();
        }

        m_udpConnectionOperationPending = true;
        m_udpDisconnectRequestedByUser = true;
        updateControlStates();
        emit disconnectUdpConnectionRequested();
        return;
    }

    if (m_portOpen
        || m_portOperationPending
        || m_testRunning
        || m_receptionCommandPending)
    {
        appendEvent(tr("UDP Connection error: close the COM port before CONNECT"),
                    EventType::Error);
        return;
    }

    appendEvent(tr("CONNECT button pressed"), EventType::Action);

    QString destinationIp;
    quint16 destinationPort = 0;
    QString errorText;
    if (!readUdpDestination(&destinationIp,
                            &destinationPort,
                            &errorText))
    {
        appendEvent(tr("UDP Connection error: %1").arg(errorText),
                    EventType::Error);
        return;
    }

    QString localIp;
    QString localMac;
    if (!resolveUdpRoute(destinationIp,
                         destinationPort,
                         &localIp,
                         &localMac,
                         &errorText))
    {
        appendEvent(tr("UDP Connection error: %1").arg(errorText),
                    EventType::Error);
        return;
    }

    m_udpDestinationIp = destinationIp;
    m_udpDestinationPort = destinationPort;
    m_udpOurIp = localIp;
    m_udpOurMac = localMac;
    m_udpDestinationMac.clear();
    ui->udpOurIpValueLabel->setText(m_udpOurIp);
    ui->udpOurMacValueLabel->setText(m_udpOurMac);
    ui->udpDestinationMacValueLabel->setText(QStringLiteral("--"));

    appendEvent(udpConnectionDescription(destinationIp, destinationPort),
                EventType::Normal);
    startUdpPing(destinationIp);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Requests start of UDP reception and counter verification.
 * @param none
 * @return none
 * @detail Logs the UDP START button press in green, validates Pattern, and sends the
 *         command to the dedicated UDP RX worker thread.
 */
void MainWindow::startUdpTest()
{
    if (m_udpReceptionCommandPending || m_shutdownPrepared)
    {
        return;
    }

    appendEvent(tr("UDP START button pressed"), EventType::Action);

    if (!m_udpWorkerReady
        || !m_udpConnected
        || m_udpRxWorker == nullptr)
    {
        appendEvent(tr("UDP START failed: the receiver socket is not connected"),
                    EventType::Error);
        return;
    }

    if (m_udpTestRunning)
    {
        appendEvent(tr("UDP START failed: reception is already active"),
                    EventType::Error);
        return;
    }

    normalizeUdpBlockSize();
    normalizeUdpInitialValue();
    normalizeUdpTogether();
    normalizeUdpPeriod();

    UdpPatternSettings settings;
    QString errorText;
    if (!readUdpPatternSettings(&settings, &errorText))
    {
        appendEvent(tr("UDP Pattern error: %1").arg(errorText),
                    EventType::Error);
        return;
    }

    m_udpReceptionCommandPending = true;
    updateControlStates();
    emit startUdpReceptionRequested(settings.counterBits,
                                    settings.blockBytes,
                                    settings.togetherCount,
                                    settings.periodMs,
                                    settings.initialValue,
                                    udpPatternDescription(settings));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Requests stop of UDP reception and counter verification.
 * @param none
 * @return none
 * @detail Logs the UDP STOP button press in green and sends a queued STOP command to
 *         UdpRxWorker while leaving the receiver socket bound.
 */
void MainWindow::stopUdpTest()
{
    if (!m_udpWorkerReady
        || !m_udpTestRunning
        || m_udpReceptionCommandPending
        || m_udpRxWorker == nullptr
        || m_shutdownPrepared)
    {
        updateControlStates();
        return;
    }

    appendEvent(tr("UDP STOP button pressed"), EventType::Action);
    m_udpReceptionCommandPending = true;
    updateControlStates();
    emit stopUdpReceptionRequested();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Starts an asynchronous one-request IPv4 ping.
 * @param destinationIp Validated destination IPv4 address.
 * @return none
 * @detail Uses a short native timeout and records only fixed English result text.
 */
void MainWindow::startUdpPing(const QString &destinationIp)
{
    if (m_pingProcess == nullptr
        || m_pingProcess->state() != QProcess::NotRunning)
    {
        appendEvent(tr("PING process is already running"), EventType::Error);
        return;
    }

    m_pingDestinationIp = destinationIp;
    m_pingInProgress = true;
    m_udpConnectionOperationPending = true;
    updateControlStates();

    QString program = QStringLiteral("ping");
    QStringList arguments;
#ifdef Q_OS_WIN
    arguments << QStringLiteral("-4")
              << QStringLiteral("-n")
              << QString::number(kPingRequestCount)
              << QStringLiteral("-w")
              << QString::number(kPingReplyTimeoutMs)
              << destinationIp;
#elif defined(Q_OS_MACOS)
    arguments << QStringLiteral("-c")
              << QString::number(kPingRequestCount)
              << QStringLiteral("-W")
              << QString::number(kPingReplyTimeoutMs)
              << destinationIp;
#else
    arguments << QStringLiteral("-4")
              << QStringLiteral("-c")
              << QString::number(kPingRequestCount)
              << QStringLiteral("-W")
              << QStringLiteral("1")
              << destinationIp;
#endif

    appendEvent(tr("PING started: dest_IP=%1; requests=%2; reply_timeout=%3 ms")
                    .arg(destinationIp)
                    .arg(kPingRequestCount)
                    .arg(kPingReplyTimeoutMs),
                EventType::Normal);
    m_pingProcess->start(program, arguments);
    m_pingTimeoutTimer.start(kPingProcessTimeoutMs);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles completion of the ping utility.
 * @param exitCode Process exit code.
 * @param exitStatus QProcess::ExitStatus value.
 * @return none
 * @detail Counts replies, logs a fixed English result, and requests creation of the
 *         persistent UDP receiver socket when the target is reachable.
 */
void MainWindow::handlePingFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitCode);

    if (!m_pingInProgress || m_pingProcess == nullptr)
    {
        return;
    }

    m_pingTimeoutTimer.stop();
    m_pingInProgress = false;
    const QString output =
        QString::fromLocal8Bit(m_pingProcess->readAll());
    const int replies = qMin(kPingRequestCount, pingReplyCount(output));
    const bool normalExit = exitStatus == QProcess::NormalExit;
    const bool reachable = normalExit && replies > 0;

    if (replies == kPingRequestCount)
    {
        appendEvent(tr("PING result: dest_IP=%1; replies=%2/%3; packet_loss=0%")
                        .arg(m_pingDestinationIp)
                        .arg(replies)
                        .arg(kPingRequestCount),
                    EventType::Normal);
    }
    else if (replies > 0)
    {
        const int packetLoss =
            ((kPingRequestCount - replies) * 100) / kPingRequestCount;
        appendEvent(tr("PING result: dest_IP=%1; replies=%2/%3; packet_loss=%4%")
                        .arg(m_pingDestinationIp)
                        .arg(replies)
                        .arg(kPingRequestCount)
                        .arg(packetLoss),
                    EventType::Error);
    }
    else
    {
        appendEvent(tr("PING result: dest_IP=%1; replies=0/%2; destination is unreachable")
                        .arg(m_pingDestinationIp)
                        .arg(kPingRequestCount),
                    EventType::Error);
    }

    if (reachable)
    {
        if (!m_udpWorkerReady || m_udpRxWorker == nullptr)
        {
            m_udpConnectionOperationPending = false;
            setUdpConnectionState(false);
            appendEvent(tr("UDP Connection error: the UDP RX worker thread is not ready"),
                        EventType::Error);
            return;
        }

        startDestinationMacLookup(m_pingDestinationIp);
        m_udpConnectionOperationPending = true;
        m_udpDisconnectRequestedByUser = false;
        updateControlStates();
        emit configureUdpConnectionRequested(m_udpDestinationIp,
                                             m_udpDestinationPort,
                                             m_udpOurIp);
    }
    else
    {
        m_udpConnectionOperationPending = false;
        setUdpConnectionState(false);
        m_udpDestinationMac.clear();
        ui->udpDestinationMacValueLabel->setText(QStringLiteral("--"));
        updateControlStates();
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles an asynchronous ping-process error.
 * @param processError QProcess::ProcessError value.
 * @return none
 * @detail Reports a fixed English diagnostic and unlocks the destination fields.
 */
void MainWindow::handlePingProcessError(QProcess::ProcessError processError)
{
    if (!m_pingInProgress)
    {
        return;
    }

    m_pingTimeoutTimer.stop();
    m_pingInProgress = false;
    m_udpConnectionOperationPending = false;
    setUdpConnectionState(false);
    m_udpDestinationMac.clear();
    ui->udpDestinationMacValueLabel->setText(QStringLiteral("--"));
    appendEvent(tr("PING process error for %1: %2")
                    .arg(m_pingDestinationIp,
                         processErrorText(processError)),
                EventType::Error);
    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles expiration of the application-level ping watchdog.
 * @param none
 * @return none
 * @detail Terminates a ping process that exceeded the short connection timeout.
 */
void MainWindow::handlePingTimeout()
{
    if (!m_pingInProgress || m_pingProcess == nullptr)
    {
        return;
    }

    m_pingInProgress = false;
    m_udpConnectionOperationPending = false;
    if (m_pingProcess->state() != QProcess::NotRunning)
    {
        m_pingProcess->kill();
        m_pingProcess->waitForFinished(100);
        m_pingProcess->readAll();
    }

    setUdpConnectionState(false);
    m_udpDestinationMac.clear();
    ui->udpDestinationMacValueLabel->setText(QStringLiteral("--"));
    appendEvent(tr("PING timeout: dest_IP=%1; process_timeout=%2 ms")
                    .arg(m_pingDestinationIp)
                    .arg(kPingProcessTimeoutMs),
                EventType::Error);
    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles completion of the neighbor-table lookup.
 * @param exitCode Process exit code.
 * @param exitStatus QProcess::ExitStatus value.
 * @return none
 * @detail Extracts the target MAC address from numeric process output.
 */
void MainWindow::handleNeighborLookupFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitCode);
    Q_UNUSED(exitStatus);

    if (!m_neighborLookupInProgress || m_neighborLookupProcess == nullptr)
    {
        return;
    }

    m_neighborLookupInProgress = false;
    const QString output =
        QString::fromLocal8Bit(m_neighborLookupProcess->readAll());
    const QString macAddress =
        parseDestinationMac(output, m_neighborLookupIp);

    if (!macAddress.isEmpty())
    {
        m_udpDestinationMac = macAddress;
        ui->udpDestinationMacValueLabel->setText(m_udpDestinationMac);
        appendEvent(tr("destination MAC resolved: dest_IP=%1; dest_MAC=%2")
                        .arg(m_neighborLookupIp, m_udpDestinationMac),
                    EventType::Normal);
    }
    else
    {
        m_udpDestinationMac.clear();
        ui->udpDestinationMacValueLabel->setText(QStringLiteral("--"));
        appendEvent(tr("destination MAC is unavailable for %1; the target may be outside the local subnet or absent from the neighbor table")
                        .arg(m_neighborLookupIp),
                    EventType::Normal);
    }

    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles an asynchronous neighbor-table process error.
 * @param processError QProcess::ProcessError value.
 * @return none
 * @detail Keeps the logical connection and records a fixed English diagnostic.
 */
void MainWindow::handleNeighborLookupProcessError(QProcess::ProcessError processError)
{
    if (!m_neighborLookupInProgress)
    {
        return;
    }

    m_neighborLookupInProgress = false;
    m_udpDestinationMac.clear();
    ui->udpDestinationMacValueLabel->setText(QStringLiteral("--"));
    appendEvent(tr("destination MAC lookup error for %1: %2")
                    .arg(m_neighborLookupIp,
                         processErrorText(processError)),
                EventType::Error);
    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles readiness of the dedicated RX worker thread.
 * @param none
 * @return none
 * @detail Enables controls after QSerialPort and the worker timers are created.
 */
void MainWindow::handleWorkerReady()
{
    if (m_shutdownPrepared)
    {
        return;
    }

    m_workerReady = true;
    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Receives a normal or error event from a receiver worker.
 * @param timestamp Timestamp created in the worker thread.
 * @param text Event text without a timestamp.
 * @param error true for a red error entry; otherwise false.
 * @return none
 * @detail Displays COM or UDP worker events in EVENTS and duplicates them to the text
 *         log in the GUI thread.
 */
void MainWindow::handleWorkerEvent(const QString &timestamp,
                                   const QString &text,
                                   bool error)
{
    appendTimestampedEvent(timestamp,
                           text,
                           error ? EventType::Error : EventType::Normal);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Receives a new COM-port state from RxWorker.
 * @param open true when the port was opened successfully.
 * @param portName Name of the opened or just-closed port.
 * @param settingsDescription Description of the applied port settings.
 * @param causedByFailure true when the port was closed because of a failure.
 * @return none
 * @detail Updates only the GUI-side state snapshot and never accesses QSerialPort from
 *         the main thread.
 */
void MainWindow::handlePortStateChanged(bool open,
                                        const QString &portName,
                                        const QString &settingsDescription,
                                        bool causedByFailure)
{
    m_portOperationPending = false;
    m_portLossRequestPending = false;
    m_portOpen = open;

    if (open)
    {
        m_openPortName = portName;
        m_openPortSettingsDescription = settingsDescription;
        m_preferredPortName = portName;
    }
    else
    {
        if (!portName.trimmed().isEmpty())
        {
            m_preferredPortName = portName.trimmed();
        }

        m_openPortName.clear();
        m_openPortSettingsDescription.clear();
        m_testRunning = false;
        m_receptionCommandPending = false;
    }

    if (!m_shutdownPrepared)
    {
        const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
        if (causedByFailure)
        {
            synchronizePortSnapshot(ports);
            updatePortComboBox(ports);
        }
        else if (!open)
        {
            refreshSerialPorts();
        }
    }

    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Receives the counter-verification state from RxWorker.
 * @param running true between a successful START and completion of STOP.
 * @return none
 * @detail Clears the pending queued-command state and synchronizes START and STOP.
 */
void MainWindow::handleReceptionStateChanged(bool running)
{
    m_testRunning = running;
    m_receptionCommandPending = false;
    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Receives a prepared Statistics snapshot from RxWorker.
 * @param startTime Test start time in HH:MM:SS format.
 * @param elapsedMilliseconds Elapsed test time in milliseconds.
 * @param totalBytesReceived Total number of bytes received during the test.
 * @param currentCounter Last completely received counter value.
 * @param counterOk Number of values that matched the expected counter.
 * @param counterErrors Number of values that did not match the expected counter.
 * @param speedKbps Measured receive speed for the latest interval in Kb/s.
 * @return none
 * @detail Formats and displays the already calculated values without doing RX work.
 */
void MainWindow::handleStatisticsUpdated(const QString &startTime,
                                         qint64 elapsedMilliseconds,
                                         quint64 totalBytesReceived,
                                         quint64 currentCounter,
                                         quint64 counterOk,
                                         quint64 counterErrors,
                                         double speedKbps)
{
    ui->startTimeValueLabel->setText(startTime.isEmpty()
                                         ? QStringLiteral("--:--:--")
                                         : startTime);
    ui->elapsedTimeValueLabel->setText(
        formatElapsedTime(elapsedMilliseconds));
    ui->rxBytesLineEdit->setText(QString::number(totalBytesReceived));
    ui->currentCountLineEdit->setText(QString::number(currentCounter));
    ui->speedLineEdit->setText(formatSpeed(speedKbps));
    ui->counterOkLineEdit->setText(QString::number(counterOk));
    ui->counterErrLineEdit->setText(QString::number(counterErrors));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles readiness of the dedicated UDP RX worker thread.
 * @param none
 * @return none
 * @detail Enables UDP controls after QUdpSocket and the Statistics timer are created in
 *         the worker thread.
 */
void MainWindow::handleUdpWorkerReady()
{
    if (m_shutdownPrepared)
    {
        return;
    }

    m_udpWorkerReady = true;
    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Receives the persistent UDP receiver-socket state.
 * @param connected true when the socket is bound and ready to receive.
 * @param expectedSourceIp Configured IPv4 address of the expected transmitter.
 * @param listenPort Local UDP port reserved for reception.
 * @param localIp Local IPv4 address to which the socket is bound.
 * @param causedByFailure true when the socket was closed because of an error.
 * @return none
 * @detail Synchronizes CONNECT, tab locking, and UDP controls without accessing
 *         QUdpSocket from the GUI thread.
 */
void MainWindow::handleUdpConnectionStateChanged(
    bool connected,
    const QString &expectedSourceIp,
    quint16 listenPort,
    const QString &localIp,
    bool causedByFailure)
{
    const bool userDisconnect = m_udpDisconnectRequestedByUser;
    m_udpConnectionOperationPending = false;
    m_udpDisconnectRequestedByUser = false;
    setUdpConnectionState(connected);

    if (connected)
    {
        m_udpDestinationIp = expectedSourceIp;
        m_udpDestinationPort = listenPort;
        m_udpOurIp = localIp;
        ui->udpOurIpValueLabel->setText(m_udpOurIp);
    }
    else
    {
        m_udpTestRunning = false;
        m_udpReceptionCommandPending = false;

        if (userDisconnect || causedByFailure)
        {
            if (m_neighborLookupProcess != nullptr
                && m_neighborLookupProcess->state() != QProcess::NotRunning)
            {
                m_neighborLookupInProgress = false;
                m_neighborLookupProcess->kill();
                m_neighborLookupProcess->waitForFinished(100);
                m_neighborLookupProcess->readAll();
            }
            clearUdpNetworkInformation();
        }

        if (userDisconnect)
        {
            appendEvent(tr("UDP destination settings unlocked"),
                        EventType::Normal);
        }
    }

    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Receives the active UDP verification state.
 * @param running true between successful UDP START and STOP completion.
 * @return none
 * @detail Clears the pending queued-command state and synchronizes START and STOP.
 */
void MainWindow::handleUdpReceptionStateChanged(bool running)
{
    m_udpTestRunning = running;
    m_udpReceptionCommandPending = false;
    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Receives a prepared UDP Statistics snapshot.
 * @param startTime Reception start time in HH:MM:SS format.
 * @param elapsedMilliseconds Elapsed monotonic test time in milliseconds.
 * @param totalPayloadBytes Total accepted UDP payload bytes.
 * @param currentCounter Last completely decoded counter value.
 * @param counterOk Number of values matching the expected counter.
 * @param counterErrors Number of counter mismatches.
 * @param speedKbps Payload receive speed for the latest interval in Kb/s.
 * @param packetsPerSecond Accepted UDP datagrams per second for the latest interval.
 * @return none
 * @detail Formats and displays values already calculated in UdpRxWorker.
 */
void MainWindow::handleUdpStatisticsUpdated(
    const QString &startTime,
    qint64 elapsedMilliseconds,
    quint64 totalPayloadBytes,
    quint64 currentCounter,
    quint64 counterOk,
    quint64 counterErrors,
    double speedKbps,
    double packetsPerSecond)
{
    ui->udpStartTimeValueLabel->setText(startTime.isEmpty()
                                            ? QStringLiteral("--:--:--")
                                            : startTime);
    ui->udpElapsedTimeValueLabel->setText(
        formatElapsedTime(elapsedMilliseconds));
    ui->udpRxBytesLineEdit->setText(QString::number(totalPayloadBytes));
    ui->udpCurrentCountLineEdit->setText(QString::number(currentCounter));
    ui->udpPacketsPerSecondLineEdit->setText(formatSpeed(packetsPerSecond));
    ui->udpSpeedLineEdit->setText(formatSpeed(speedKbps));
    ui->udpCounterOkLineEdit->setText(QString::number(counterOk));
    ui->udpCounterErrLineEdit->setText(QString::number(counterErrors));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates and starts the dedicated RX worker thread.
 * @param none
 * @return none
 * @detail Moves RxWorker to QThread, connects all cross-thread signals with queued
 *         connections, and starts the thread after all connections are configured.
 */
void MainWindow::initializeRxThread()
{
    m_rxWorker = new RxWorker;
    m_rxWorker->moveToThread(&m_rxThread);
    m_rxThread.setObjectName(QStringLiteral("RxDataTester_RX_Worker"));

    connect(&m_rxThread,
            &QThread::started,
            m_rxWorker,
            &RxWorker::initialize);
    connect(&m_rxThread,
            &QThread::finished,
            m_rxWorker,
            &QObject::deleteLater);

    connect(this,
            &MainWindow::openPortRequested,
            m_rxWorker,
            &RxWorker::openPort,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::closePortRequested,
            m_rxWorker,
            &RxWorker::closePort,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::startReceptionRequested,
            m_rxWorker,
            &RxWorker::startReception,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::stopReceptionRequested,
            m_rxWorker,
            &RxWorker::stopReception,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::externalPortLossDetected,
            m_rxWorker,
            &RxWorker::handleExternalPortLoss,
            Qt::QueuedConnection);

    connect(m_rxWorker,
            &RxWorker::workerReady,
            this,
            &MainWindow::handleWorkerReady,
            Qt::QueuedConnection);
    connect(m_rxWorker,
            &RxWorker::eventGenerated,
            this,
            &MainWindow::handleWorkerEvent,
            Qt::QueuedConnection);
    connect(m_rxWorker,
            &RxWorker::portStateChanged,
            this,
            &MainWindow::handlePortStateChanged,
            Qt::QueuedConnection);
    connect(m_rxWorker,
            &RxWorker::receptionStateChanged,
            this,
            &MainWindow::handleReceptionStateChanged,
            Qt::QueuedConnection);
    connect(m_rxWorker,
            &RxWorker::statisticsUpdated,
            this,
            &MainWindow::handleStatisticsUpdated,
            Qt::QueuedConnection);
    connect(m_rxWorker,
            &RxWorker::periodicLogLineReady,
            this,
            &MainWindow::writeLogLine,
            Qt::QueuedConnection);

    m_rxThread.start();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates and starts the dedicated UDP RX worker thread.
 * @param none
 * @return none
 * @detail Moves UdpRxWorker to QThread, connects all queued commands and worker
 *         replies, and starts the thread after all connections are configured.
 */
void MainWindow::initializeUdpRxThread()
{
    m_udpRxWorker = new UdpRxWorker;
    m_udpRxWorker->moveToThread(&m_udpRxThread);
    m_udpRxThread.setObjectName(QStringLiteral("RxDataTester_UDP_RX_Worker"));

    connect(&m_udpRxThread,
            &QThread::started,
            m_udpRxWorker,
            &UdpRxWorker::initialize);
    connect(&m_udpRxThread,
            &QThread::finished,
            m_udpRxWorker,
            &QObject::deleteLater);

    connect(this,
            &MainWindow::configureUdpConnectionRequested,
            m_udpRxWorker,
            &UdpRxWorker::configureConnection,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::disconnectUdpConnectionRequested,
            m_udpRxWorker,
            &UdpRxWorker::disconnectConnection,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::startUdpReceptionRequested,
            m_udpRxWorker,
            &UdpRxWorker::startReception,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::stopUdpReceptionRequested,
            m_udpRxWorker,
            &UdpRxWorker::stopReception,
            Qt::QueuedConnection);

    connect(m_udpRxWorker,
            &UdpRxWorker::workerReady,
            this,
            &MainWindow::handleUdpWorkerReady,
            Qt::QueuedConnection);
    connect(m_udpRxWorker,
            &UdpRxWorker::eventGenerated,
            this,
            &MainWindow::handleWorkerEvent,
            Qt::QueuedConnection);
    connect(m_udpRxWorker,
            &UdpRxWorker::connectionStateChanged,
            this,
            &MainWindow::handleUdpConnectionStateChanged,
            Qt::QueuedConnection);
    connect(m_udpRxWorker,
            &UdpRxWorker::receptionStateChanged,
            this,
            &MainWindow::handleUdpReceptionStateChanged,
            Qt::QueuedConnection);
    connect(m_udpRxWorker,
            &UdpRxWorker::statisticsUpdated,
            this,
            &MainWindow::handleUdpStatisticsUpdated,
            Qt::QueuedConnection);
    connect(m_udpRxWorker,
            &UdpRxWorker::periodicLogLineReady,
            this,
            &MainWindow::writeLogLine,
            Qt::QueuedConnection);

    m_udpRxThread.start();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Updates enabled states of all controls.
 * @param none
 * @return none
 * @detail Considers both worker threads, COM and UDP connection states, active
 *         reception, tab locking, and pending asynchronous commands.
 */
void MainWindow::updateControlStates()
{
    const bool comAsynchronousBusy = m_portOperationPending
                                      || m_receptionCommandPending
                                      || m_portLossRequestPending;
    const bool comWorkerAvailable = m_workerReady && !m_shutdownPrepared;

    const bool udpConnectionBusy = m_pingInProgress
                                   || m_udpConnectionOperationPending;
    const bool udpCommandBusy = m_udpReceptionCommandPending;
    const bool udpWorkerAvailable = m_udpWorkerReady && !m_shutdownPrepared;

    const bool udpModeLocksCom = m_pingInProgress
                                 || m_udpConnectionOperationPending
                                 || m_udpConnected
                                 || m_udpTestRunning
                                 || m_udpReceptionCommandPending;
    const bool comModeLocksUdp = m_portOpen
                                 || m_portOperationPending
                                 || m_testRunning
                                 || m_receptionCommandPending
                                 || m_portLossRequestPending;

    const int comTabIndex = ui->tabWidget->indexOf(ui->comTab);
    const int udpTabIndex = ui->tabWidget->indexOf(ui->udpTab);
    if (comTabIndex >= 0)
    {
        ui->tabWidget->setTabEnabled(comTabIndex,
                                     !m_shutdownPrepared && !udpModeLocksCom);
    }
    if (udpTabIndex >= 0)
    {
        ui->tabWidget->setTabEnabled(udpTabIndex,
                                     !m_shutdownPrepared && !comModeLocksUdp);
    }

    const bool comModeAvailable = comWorkerAvailable && !udpModeLocksCom;
    ui->openButton->setEnabled(comModeAvailable
                               && !m_portOpen
                               && !comAsynchronousBusy
                               && ui->portComboBox->count() > 0);
    ui->closeButton->setEnabled(comWorkerAvailable
                                && m_portOpen
                                && !m_portOperationPending
                                && !m_portLossRequestPending);

    const bool portSettingsEnabled = comModeAvailable
                                     && !m_portOpen
                                     && !comAsynchronousBusy;
    ui->portComboBox->setEnabled(portSettingsEnabled);
    ui->baudComboBox->setEnabled(portSettingsEnabled);
    ui->parityComboBox->setEnabled(portSettingsEnabled);
    ui->stopsComboBox->setEnabled(portSettingsEnabled);

    const bool comPatternEnabled = comModeAvailable
                                   && !m_testRunning
                                   && !comAsynchronousBusy;
    ui->counterBitsComboBox->setEnabled(comPatternEnabled);
    ui->blockBytesLineEdit->setEnabled(comPatternEnabled);
    ui->initValueLineEdit->setEnabled(comPatternEnabled);
    ui->periodMsLineEdit->setEnabled(comPatternEnabled);

    ui->startButton->setEnabled(comModeAvailable
                                && m_portOpen
                                && !m_testRunning
                                && !comAsynchronousBusy);
    ui->stopButton->setEnabled(comWorkerAvailable
                               && m_portOpen
                               && m_testRunning
                               && !comAsynchronousBusy);

    const bool udpModeAvailable = udpWorkerAvailable && !comModeLocksUdp;
    const bool udpDestinationEditable = udpModeAvailable
                                        && !m_udpConnected
                                        && !udpConnectionBusy
                                        && !udpCommandBusy;
    ui->udpDestinationIpLineEdit->setEnabled(udpDestinationEditable);
    ui->udpDestinationPortLineEdit->setEnabled(udpDestinationEditable);

    const bool destinationIpPresent =
        !ui->udpDestinationIpLineEdit->text().trimmed().isEmpty();
    const bool destinationPortPresent =
        !ui->udpDestinationPortLineEdit->text().trimmed().isEmpty();
    const bool udpDisconnectAvailable = m_udpConnected
                                        && !m_udpTestRunning
                                        && !udpConnectionBusy
                                        && !udpCommandBusy;
    const bool udpConnectAvailable = !m_udpConnected
                                     && !udpConnectionBusy
                                     && !udpCommandBusy
                                     && destinationIpPresent
                                     && destinationPortPresent;
    ui->udpConnectButton->setEnabled(udpModeAvailable
                                     && (udpDisconnectAvailable
                                         || udpConnectAvailable));
    ui->udpConnectButton->setText(
        m_udpConnected ? QStringLiteral("DISCONNECT")
                       : QStringLiteral("CONNECT"));

    const bool udpPatternEnabled = udpModeAvailable
                                   && !m_udpTestRunning
                                   && !udpConnectionBusy
                                   && !udpCommandBusy;
    ui->udpCounterBitsComboBox->setEnabled(udpPatternEnabled);
    ui->udpBlockBytesLineEdit->setEnabled(udpPatternEnabled);
    ui->udpInitValueLineEdit->setEnabled(udpPatternEnabled);
    ui->udpTogetherLineEdit->setEnabled(udpPatternEnabled);
    ui->udpPeriodMsLineEdit->setEnabled(udpPatternEnabled);

    ui->udpStartButton->setEnabled(udpModeAvailable
                                   && m_udpConnected
                                   && !m_udpTestRunning
                                   && !udpConnectionBusy
                                   && !udpCommandBusy);
    ui->udpStopButton->setEnabled(udpWorkerAvailable
                                  && m_udpConnected
                                  && m_udpTestRunning
                                  && !udpConnectionBusy
                                  && !udpCommandBusy);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Adds an event with the current timestamp.
 * @param eventText Event text without a timestamp.
 * @param eventType Color and style category of the entry.
 * @return none
 * @detail Used for user actions and GUI-local errors.
 */
void MainWindow::appendEvent(const QString &eventText, EventType eventType)
{
    appendTimestampedEvent(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
        eventText,
        eventType);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Adds an event with a preformatted timestamp.
 * @param timestamp Timestamp in HH:MM:SS.mmm format.
 * @param eventText Event text without a timestamp.
 * @param eventType Color and style category of the entry.
 * @return none
 * @detail Preserves the actual RX event time even when the GUI is temporarily busy.
 */
void MainWindow::appendTimestampedEvent(const QString &timestamp,
                                        const QString &eventText,
                                        EventType eventType)
{
    QString singleLineText = eventText;
    singleLineText.replace(QLatin1Char('\r'), QLatin1Char(' '));
    singleLineText.replace(QLatin1Char('\n'), QLatin1Char(' '));

    const QString safeTimestamp = timestamp.trimmed().isEmpty()
                                      ? QDateTime::currentDateTime().toString(
                                            QStringLiteral("HH:mm:ss.zzz"))
                                      : timestamp.trimmed();
    const QString completeLine =
        safeTimestamp + QStringLiteral(" - ") + singleLineText;

    QTextCursor cursor(ui->eventsPlainTextEdit->document());
    cursor.movePosition(QTextCursor::End);

    QTextCharFormat format;
    format.setForeground(eventColor(eventType));
    if (eventType == EventType::Action)
    {
        format.setFontWeight(QFont::DemiBold);
    }
    else if (eventType == EventType::Error)
    {
        format.setFontWeight(QFont::Bold);
    }

    cursor.insertText(completeLine + QLatin1Char('\n'), format);
    ui->eventsPlainTextEdit->setTextCursor(cursor);
    ui->eventsPlainTextEdit->ensureCursorVisible();
    writeLogLine(completeLine);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Writes a prepared line only to the text log file.
 * @param line Complete line without a trailing newline.
 * @return none
 * @detail Appends a newline and flushes the stream in the main GUI thread.
 */
void MainWindow::writeLogLine(const QString &line)
{
    if (!m_logFile.isOpen())
    {
        return;
    }

    m_logStream << line << '\n';
    m_logStream.flush();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns the color for an event-log entry.
 * @param eventType Event type to convert to a color.
 * @return A black, green, or red QColor.
 * @detail Green is used only for direct button presses.
 */
QColor MainWindow::eventColor(EventType eventType) const
{
    if (eventType == EventType::Action)
    {
        return QColor(0, 128, 0);
    }

    if (eventType == EventType::Error)
    {
        return QColor(190, 0, 0);
    }

    return QColor(0, 0, 0);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates the logs directory and a new log file for the current run.
 * @param none
 * @return none
 * @detail Opens a UTF-8 rxdatatester_log__date__time.txt file next to the executable.
 */
void MainWindow::initializeLogFile()
{
    QDir applicationDirectory(QCoreApplication::applicationDirPath());
    if (!applicationDirectory.mkpath(QStringLiteral("logs")))
    {
        appendEvent(tr("failed to create the logs directory in %1")
                        .arg(QDir::toNativeSeparators(
                            applicationDirectory.absolutePath())),
                    EventType::Error);
        return;
    }

    const QString timestamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd__HH-mm-ss-zzz"));
    const QString fileName =
        QStringLiteral("rxdatatester_log__%1.txt").arg(timestamp);
    const QString filePath =
        applicationDirectory.filePath(QStringLiteral("logs/") + fileName);

    m_logFile.setFileName(filePath);
    if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        appendEvent(tr("failed to create log file %1: %2")
                        .arg(QDir::toNativeSeparators(filePath),
                             fileErrorText(m_logFile.error())),
                    EventType::Error);
        return;
    }

    m_logStream.setDevice(&m_logFile);
    m_logStream.setCodec("UTF-8");
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Flushes and closes the log file.
 * @param none
 * @return none
 * @detail Detaches QTextStream from QFile after the mandatory flush.
 */
void MainWindow::closeLogFile()
{
    if (!m_logFile.isOpen())
    {
        return;
    }

    m_logStream.flush();
    m_logStream.setDevice(nullptr);
    m_logFile.close();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Restores settings from the previous run.
 * @param none
 * @return none
 * @detail Loads window geometry, the selected COM port, and Pattern fields from
 *         QSettings.
 */
void MainWindow::loadSettings()
{
    QSettings settings;

    const QByteArray geometry =
        settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty())
    {
        restoreGeometry(geometry);
    }

    m_preferredPortName =
        settings.value(QStringLiteral("com/port"), QString()).toString();
    selectComboBoxText(
        ui->baudComboBox,
        settings.value(QStringLiteral("com/baud"), QStringLiteral("921600"))
            .toString());
    selectComboBoxText(
        ui->parityComboBox,
        settings.value(QStringLiteral("com/parity"), QStringLiteral("NONE"))
            .toString());
    selectComboBoxText(
        ui->stopsComboBox,
        settings.value(QStringLiteral("com/stops"), QStringLiteral("2"))
            .toString());
    selectComboBoxText(
        ui->counterBitsComboBox,
        settings.value(QStringLiteral("pattern/counterBits"),
                       QStringLiteral("32"))
            .toString());

    ui->blockBytesLineEdit->setText(
        settings.value(QStringLiteral("pattern/blockBytes"),
                       QStringLiteral("128"))
            .toString());
    ui->initValueLineEdit->setText(
        settings.value(QStringLiteral("pattern/initValue"), QStringLiteral("0"))
            .toString());
    ui->periodMsLineEdit->setText(
        settings.value(QStringLiteral("pattern/periodMs"),
                       QStringLiteral("100"))
            .toString());

    ui->udpDestinationIpLineEdit->setText(
        settings.value(QStringLiteral("udp/destinationIp"), QString()).toString());
    ui->udpDestinationPortLineEdit->setText(
        settings.value(QStringLiteral("udp/destinationPort"), QString()).toString());
    selectComboBoxText(
        ui->udpCounterBitsComboBox,
        settings.value(QStringLiteral("udp/pattern/counterBits"),
                       QStringLiteral("32"))
            .toString());
    ui->udpBlockBytesLineEdit->setText(
        settings.value(QStringLiteral("udp/pattern/blockBytes"),
                       QStringLiteral("128"))
            .toString());
    ui->udpInitValueLineEdit->setText(
        settings.value(QStringLiteral("udp/pattern/initValue"),
                       QStringLiteral("0"))
            .toString());
    ui->udpTogetherLineEdit->setText(
        settings.value(QStringLiteral("udp/pattern/together"),
                       QStringLiteral("1"))
            .toString());
    ui->udpPeriodMsLineEdit->setText(
        settings.value(QStringLiteral("udp/pattern/periodMs"),
                       QStringLiteral("100"))
            .toString());

    if (settings.status() != QSettings::NoError)
    {
        appendEvent(tr("failed to read saved QSettings"),
                    EventType::Error);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Saves the current application settings.
 * @param none
 * @return none
 * @detail Normalizes the fields and saves window geometry, COM settings, and Pattern
 *         through QSettings.
 */
void MainWindow::saveSettings()
{
    normalizeBlockSize();
    normalizeInitialValue();
    normalizePeriod();
    normalizeUdpBlockSize();
    normalizeUdpInitialValue();
    normalizeUdpTogether();
    normalizeUdpPeriod();

    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());

    const QString selectedPort = ui->portComboBox->currentText().trimmed();
    if (!selectedPort.isEmpty())
    {
        settings.setValue(QStringLiteral("com/port"), selectedPort);
    }
    else if (!m_preferredPortName.isEmpty())
    {
        settings.setValue(QStringLiteral("com/port"), m_preferredPortName);
    }

    settings.setValue(QStringLiteral("com/baud"),
                      ui->baudComboBox->currentText());
    settings.setValue(QStringLiteral("com/parity"),
                      ui->parityComboBox->currentText());
    settings.setValue(QStringLiteral("com/stops"),
                      ui->stopsComboBox->currentText());
    settings.setValue(QStringLiteral("pattern/counterBits"),
                      ui->counterBitsComboBox->currentText());
    settings.setValue(QStringLiteral("pattern/blockBytes"),
                      ui->blockBytesLineEdit->text());
    settings.setValue(QStringLiteral("pattern/initValue"),
                      ui->initValueLineEdit->text());
    settings.setValue(QStringLiteral("pattern/periodMs"),
                      ui->periodMsLineEdit->text());
    settings.setValue(QStringLiteral("udp/destinationIp"),
                      ui->udpDestinationIpLineEdit->text().trimmed());
    settings.setValue(QStringLiteral("udp/destinationPort"),
                      ui->udpDestinationPortLineEdit->text().trimmed());
    settings.setValue(QStringLiteral("udp/pattern/counterBits"),
                      ui->udpCounterBitsComboBox->currentText());
    settings.setValue(QStringLiteral("udp/pattern/blockBytes"),
                      ui->udpBlockBytesLineEdit->text());
    settings.setValue(QStringLiteral("udp/pattern/initValue"),
                      ui->udpInitValueLineEdit->text());
    settings.setValue(QStringLiteral("udp/pattern/together"),
                      ui->udpTogetherLineEdit->text());
    settings.setValue(QStringLiteral("udp/pattern/periodMs"),
                      ui->udpPeriodMsLineEdit->text());
    settings.sync();

    if (settings.status() != QSettings::NoError)
    {
        appendEvent(tr("failed to save QSettings"),
                    EventType::Error);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Performs one-time application shutdown.
 * @param none
 * @return none
 * @detail Invokes both receiver-worker shutdown slots through blocking queued calls,
 *         waits for both QThreads, processes final events, saves settings, and closes
 *         the log.
 */
void MainWindow::prepareShutdown()
{
    if (m_shutdownPrepared)
    {
        return;
    }

    m_shutdownPrepared = true;
    stopUdpProcesses();
    m_portRefreshTimer.stop();
    updateControlStates();

    if (m_rxWorker != nullptr && m_rxThread.isRunning())
    {
        const bool invoked = QMetaObject::invokeMethod(
            m_rxWorker,
            "shutdown",
            Qt::BlockingQueuedConnection);
        if (!invoked)
        {
            appendEvent(tr("failed to invoke COM RX worker shutdown"),
                        EventType::Error);
        }

        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        m_rxThread.quit();
        m_rxThread.wait();
        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        m_rxWorker = nullptr;
    }

    if (m_udpRxWorker != nullptr && m_udpRxThread.isRunning())
    {
        const bool invoked = QMetaObject::invokeMethod(
            m_udpRxWorker,
            "shutdown",
            Qt::BlockingQueuedConnection);
        if (!invoked)
        {
            appendEvent(tr("failed to invoke UDP RX worker shutdown"),
                        EventType::Error);
        }

        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        m_udpRxThread.quit();
        m_udpRxThread.wait();
        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        m_udpRxWorker = nullptr;
    }

    m_workerReady = false;
    m_udpWorkerReady = false;
    m_portOpen = false;
    m_testRunning = false;
    m_portOperationPending = false;
    m_receptionCommandPending = false;
    m_portLossRequestPending = false;
    m_udpConnected = false;
    m_udpConnectionOperationPending = false;
    m_udpTestRunning = false;
    m_udpReceptionCommandPending = false;
    m_udpDisconnectRequestedByUser = false;

    saveSettings();
    appendEvent(tr("RxDataTester (v.1.4) stopped"),
                EventType::Normal);
    closeLogFile();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Repopulates the COM-port combo box.
 * @param ports Current list of QSerialPortInfo objects.
 * @return none
 * @detail Preserves the preferred selection and stores each device description as a
 *         tooltip.
 */
void MainWindow::updatePortComboBox(const QList<QSerialPortInfo> &ports)
{
    QString portToSelect = ui->portComboBox->currentText().trimmed();
    if (portToSelect.isEmpty())
    {
        portToSelect = m_preferredPortName;
    }

    const QSignalBlocker blocker(ui->portComboBox);
    ui->portComboBox->clear();

    for (const QSerialPortInfo &portInfo : ports)
    {
        ui->portComboBox->addItem(portInfo.portName(),
                                  portInfo.systemLocation());
        const int index = ui->portComboBox->count() - 1;
        ui->portComboBox->setItemData(index,
                                      portDescription(portInfo),
                                      Qt::ToolTipRole);
    }

    const int restoredIndex = ui->portComboBox->findText(portToSelect);
    if (restoredIndex >= 0)
    {
        ui->portComboBox->setCurrentIndex(restoredIndex);
    }
    else if (ui->portComboBox->count() > 0)
    {
        ui->portComboBox->setCurrentIndex(0);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Builds a set of available port names.
 * @param ports List of serial-port information objects.
 * @return A set containing all non-empty port names.
 * @detail Used to compare port snapshots and monitor the open device.
 */
QSet<QString> MainWindow::portNames(
    const QList<QSerialPortInfo> &ports) const
{
    QSet<QString> result;

    for (const QSerialPortInfo &portInfo : ports)
    {
        const QString portName = portInfo.portName().trimmed();
        if (!portName.isEmpty())
        {
            result.insert(portName);
        }
    }

    return result;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Builds descriptions of available ports indexed by name.
 * @param ports List of serial-port information objects.
 * @return A hash table that maps a port name to its full description.
 * @detail Stores device text for a later port-disappearance event.
 */
QHash<QString, QString> MainWindow::portDescriptions(
    const QList<QSerialPortInfo> &ports) const
{
    QHash<QString, QString> result;

    for (const QSerialPortInfo &portInfo : ports)
    {
        const QString portName = portInfo.portName().trimmed();
        if (!portName.isEmpty())
        {
            result.insert(portName, portDescription(portInfo));
        }
    }

    return result;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Synchronizes the internal port snapshot without addition events.
 * @param ports Current list of QSerialPortInfo objects.
 * @return none
 * @detail Used after an emergency close so that a red port-loss event is not duplicated
 *         by a normal "port disappeared" entry.
 */
void MainWindow::synchronizePortSnapshot(
    const QList<QSerialPortInfo> &ports)
{
    m_knownPortNames = portNames(ports);
    m_knownPortDescriptions = portDescriptions(ports);
    m_portSnapshotInitialized = true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Sets the GUI-side UDP receiver connection state.
 * @param connected true when the persistent UDP socket is bound and ready.
 * @return none
 * @detail Locks or unlocks the UDP endpoint fields and updates tab states.
 */
void MainWindow::setUdpConnectionState(bool connected)
{
    m_udpConnected = connected;
    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Clears displayed UDP interface and destination-MAC information.
 * @param none
 * @return none
 * @detail Prevents stale route information from remaining visible after edits.
 */
void MainWindow::clearUdpNetworkInformation()
{
    m_udpOurIp.clear();
    m_udpOurMac.clear();
    m_udpDestinationMac.clear();
    m_udpDestinationIp.clear();
    m_udpDestinationPort = 0;
    m_pingDestinationIp.clear();
    m_neighborLookupIp.clear();

    ui->udpOurIpValueLabel->setText(QStringLiteral("--"));
    ui->udpOurMacValueLabel->setText(QStringLiteral("--"));
    ui->udpDestinationMacValueLabel->setText(QStringLiteral("--"));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Reads and validates the UDP destination fields.
 * @param destinationIp Output destination IPv4 string.
 * @param destinationPort Output destination UDP port.
 * @param errorText Output fixed English validation error.
 * @return true when the IPv4 address and port 1...65535 are valid.
 * @detail Character validators block unwanted input while this method verifies the
 *         complete numeric address and port range.
 */
bool MainWindow::readUdpDestination(QString *destinationIp,
                                    quint16 *destinationPort,
                                    QString *errorText) const
{
    if (destinationIp == nullptr
        || destinationPort == nullptr
        || errorText == nullptr)
    {
        return false;
    }

    const QString ipText = ui->udpDestinationIpLineEdit->text().trimmed();
    if (ipText.isEmpty())
    {
        *errorText = tr("destination IP is empty");
        return false;
    }

    QHostAddress address;
    if (!address.setAddress(ipText)
        || address.protocol() != QAbstractSocket::IPv4Protocol)
    {
        *errorText = tr("destination IP is not a valid IPv4 address");
        return false;
    }

    if (address == QHostAddress(QHostAddress::AnyIPv4))
    {
        *errorText = tr("destination IP 0.0.0.0 cannot be used");
        return false;
    }

    bool portOk = false;
    const uint portValue =
        ui->udpDestinationPortLineEdit->text().trimmed().toUInt(&portOk, 10);
    if (!portOk || portValue == 0U || portValue > 65535U)
    {
        *errorText = tr("listening Port must be in range 1...65535");
        return false;
    }

    *destinationIp = address.toString();
    *destinationPort = static_cast<quint16>(portValue);
    errorText->clear();
    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Reads and validates UDP Pattern values from the GUI.
 * @param settings Output pointer for validated UDP Pattern settings.
 * @param errorText Output pointer for a validation error message.
 * @return true when all UDP Pattern values are valid; otherwise false.
 * @detail Checks counter width, payload size, alignment, Togeth, Period, and initial
 *         value range.
 */
bool MainWindow::readUdpPatternSettings(UdpPatternSettings *settings,
                                        QString *errorText) const
{
    if (settings == nullptr || errorText == nullptr)
    {
        return false;
    }

    bool counterOk = false;
    const int counterBits =
        ui->udpCounterBitsComboBox->currentText().toInt(&counterOk, 10);
    if (!counterOk
        || (counterBits != 8
            && counterBits != 16
            && counterBits != 32
            && counterBits != 64))
    {
        *errorText = tr("unsupported counter width");
        return false;
    }

    const int counterBytes = counterBits / 8;
    bool blockOk = false;
    const qlonglong blockBytesValue =
        ui->udpBlockBytesLineEdit->text().trimmed().toLongLong(&blockOk, 10);
    if (!blockOk
        || blockBytesValue <= 0
        || blockBytesValue > static_cast<qlonglong>(kMaximumIpv4UdpPayloadBytes)
        || (blockBytesValue % counterBytes) != 0)
    {
        *errorText = tr("block, bytes must be 1...65507 and a multiple of %1 bytes")
                         .arg(counterBytes);
        return false;
    }

    bool togetherOk = false;
    const qlonglong togetherValue =
        ui->udpTogetherLineEdit->text().trimmed().toLongLong(&togetherOk, 10);
    if (!togetherOk
        || togetherValue <= 0
        || togetherValue > std::numeric_limits<int>::max())
    {
        *errorText = tr("Togeth must be a positive decimal number within int range");
        return false;
    }

    bool periodOk = false;
    const qlonglong periodValue =
        ui->udpPeriodMsLineEdit->text().trimmed().toLongLong(&periodOk, 10);
    if (!periodOk
        || periodValue < 0
        || periodValue > std::numeric_limits<int>::max())
    {
        *errorText = tr("Period, ms is outside the valid range");
        return false;
    }

    quint64 initialValue = 0;
    if (!parseUdpInitialValue(&initialValue))
    {
        *errorText = tr("init value is invalid or does not fit the selected counter width");
        return false;
    }

    settings->counterBits = counterBits;
    settings->counterBytes = counterBytes;
    settings->blockBytes = static_cast<int>(blockBytesValue);
    settings->togetherCount = static_cast<int>(togetherValue);
    settings->periodMs = static_cast<int>(periodValue);
    settings->initialValue = initialValue;
    settings->maximumCounterValue = counterBits == 64
                                        ? std::numeric_limits<quint64>::max()
                                        : (quint64(1) << counterBits) - quint64(1);
    errorText->clear();
    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Builds a UDP Pattern description for the event log.
 * @param settings Validated UDP Pattern settings.
 * @return A counter/init/block/Togeth/period/values-per-packet/bo=LE string.
 * @detail Hexadecimal input also includes its decimal equivalent.
 */
QString MainWindow::udpPatternDescription(
    const UdpPatternSettings &settings) const
{
    QString initialValueText = ui->udpInitValueLineEdit->text().trimmed();
    if (initialValueText.startsWith(QStringLiteral("0x"),
                                    Qt::CaseInsensitive))
    {
        initialValueText +=
            tr(" (dec %1)").arg(QString::number(settings.initialValue));
    }

    const int valuesPerPacket = settings.blockBytes / settings.counterBytes;
    return tr("Pattern: counter=%1 bits; init=%2; block=%3 bytes; "
              "Togeth=%4; period=%5 ms; values/packet=%6; bo=LE")
        .arg(settings.counterBits)
        .arg(initialValueText)
        .arg(settings.blockBytes)
        .arg(settings.togetherCount)
        .arg(settings.periodMs)
        .arg(valuesPerPacket);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Resolves the local interface selected for a UDP destination.
 * @param destinationIp Destination IPv4 address.
 * @param destinationPort Destination UDP port used for route selection.
 * @param localIp Output local IPv4 address.
 * @param localMac Output local interface MAC address or -- when unavailable.
 * @param errorText Output fixed English route-selection error.
 * @return true when an outgoing IPv4 address was selected.
 * @detail Uses a temporary connected QUdpSocket only as an operating-system route
 *         probe and closes it before returning.
 */
bool MainWindow::resolveUdpRoute(const QString &destinationIp,
                                 quint16 destinationPort,
                                 QString *localIp,
                                 QString *localMac,
                                 QString *errorText) const
{
    if (localIp == nullptr || localMac == nullptr || errorText == nullptr)
    {
        return false;
    }

    QHostAddress destinationAddress;
    if (!destinationAddress.setAddress(destinationIp)
        || destinationAddress.protocol() != QAbstractSocket::IPv4Protocol)
    {
        *errorText = tr("destination IP is not a valid IPv4 address");
        return false;
    }

    QUdpSocket routeProbe;
    routeProbe.connectToHost(destinationAddress,
                             destinationPort,
                             QIODevice::WriteOnly);
    if (routeProbe.state() != QAbstractSocket::ConnectedState
        && !routeProbe.waitForConnected(kRouteProbeTimeoutMs))
    {
        *errorText = tr("the operating system could not select an outgoing interface");
        routeProbe.abort();
        return false;
    }

    const QHostAddress selectedAddress = routeProbe.localAddress();
    routeProbe.abort();

    if (selectedAddress.isNull()
        || selectedAddress == QHostAddress(QHostAddress::AnyIPv4)
        || selectedAddress.protocol() != QAbstractSocket::IPv4Protocol)
    {
        *errorText = tr("the operating system did not provide an outgoing IPv4 address");
        return false;
    }

    *localIp = selectedAddress.toString();
    *localMac = localMacForIp(*localIp);
    errorText->clear();
    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Finds a network-interface MAC address by local IPv4 address.
 * @param localIp Local IPv4 address to match.
 * @return Normalized uppercase MAC address or -- when unavailable.
 * @detail Searches active QNetworkInterface address entries.
 */
QString MainWindow::localMacForIp(const QString &localIp) const
{
    QHostAddress targetAddress;
    if (!targetAddress.setAddress(localIp))
    {
        return QStringLiteral("--");
    }

    const QList<QNetworkInterface> interfaces =
        QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &networkInterface : interfaces)
    {
        if (!networkInterface.flags().testFlag(QNetworkInterface::IsUp))
        {
            continue;
        }

        const QList<QNetworkAddressEntry> entries =
            networkInterface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries)
        {
            if (entry.ip() != targetAddress)
            {
                continue;
            }

            const QString normalizedMac =
                normalizeMacAddress(networkInterface.hardwareAddress());
            return normalizedMac.isEmpty() ? QStringLiteral("--")
                                           : normalizedMac;
        }
    }

    return QStringLiteral("--");
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Builds the UDP Connection Settings event text.
 * @param destinationIp Validated destination IPv4 address.
 * @param destinationPort Validated destination UDP port.
 * @return One-line destination and local-interface description.
 * @detail The destination MAC is logged separately after the neighbor lookup.
 */
QString MainWindow::udpConnectionDescription(const QString &destinationIp,
                                             quint16 destinationPort) const
{
    const QString localIp =
        m_udpOurIp.isEmpty() ? QStringLiteral("--") : m_udpOurIp;
    const QString localMac =
        m_udpOurMac.isEmpty() ? QStringLiteral("--") : m_udpOurMac;

    return tr("Connection Settings: dest_IP=%1; listen_PORT=%2; our_IP=%3; our_MAC=%4")
        .arg(destinationIp)
        .arg(destinationPort)
        .arg(localIp)
        .arg(localMac);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Stops active UDP helper processes during application shutdown.
 * @param none
 * @return none
 * @detail Terminates ping or neighbor lookup and force-kills a process if needed.
 */
void MainWindow::stopUdpProcesses()
{
    m_pingTimeoutTimer.stop();
    m_pingInProgress = false;
    m_neighborLookupInProgress = false;
    m_udpConnectionOperationPending = false;

    const QList<QProcess *> processes = {
        m_pingProcess,
        m_neighborLookupProcess
    };
    for (QProcess *process : processes)
    {
        if (process == nullptr || process->state() == QProcess::NotRunning)
        {
            continue;
        }

        process->terminate();
        if (!process->waitForFinished(250))
        {
            process->kill();
            process->waitForFinished(250);
        }

        process->readAll();
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Starts an asynchronous destination-MAC lookup.
 * @param destinationIp IPv4 address that replied to ping.
 * @return none
 * @detail Uses arp on Windows and macOS or ip neigh on Linux, then parses numeric IP
 *         and MAC patterns from the output.
 */
void MainWindow::startDestinationMacLookup(const QString &destinationIp)
{
    if (m_neighborLookupProcess == nullptr
        || m_neighborLookupProcess->state() != QProcess::NotRunning)
    {
        appendEvent(tr("destination MAC lookup process is already running"),
                    EventType::Error);
        updateControlStates();
        return;
    }

    QString program;
    QStringList arguments;
#ifdef Q_OS_WIN
    program = QStringLiteral("arp");
    arguments << QStringLiteral("-a") << destinationIp;
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    program = QStringLiteral("arp");
    arguments << QStringLiteral("-n") << destinationIp;
#else
    program = QStringLiteral("ip");
    arguments << QStringLiteral("neigh")
              << QStringLiteral("show")
              << destinationIp;
#endif

    m_neighborLookupIp = destinationIp;
    m_neighborLookupInProgress = true;
    updateControlStates();
    m_neighborLookupProcess->start(program, arguments);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Extracts a destination MAC address from neighbor-table output.
 * @param output Process output converted with the local 8-bit codec.
 * @param destinationIp Destination IPv4 address used to select the correct line.
 * @return Normalized uppercase MAC address or an empty string when not found.
 * @detail Accepts both colon-separated and hyphen-separated MAC formats.
 */
QString MainWindow::parseDestinationMac(const QString &output,
                                        const QString &destinationIp) const
{
    const QRegularExpression destinationExpression(
        QStringLiteral("(^|[^0-9.])%1([^0-9.]|$)")
            .arg(QRegularExpression::escape(destinationIp)));
    const QRegularExpression macExpression(
        QStringLiteral("\\b([0-9A-Fa-f]{1,2}(?:[:-][0-9A-Fa-f]{1,2}){5})\\b"));

    const QStringList lines =
        output.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                     QString::SkipEmptyParts);
    for (const QString &line : lines)
    {
        if (!destinationExpression.match(line).hasMatch())
        {
            continue;
        }

        const QRegularExpressionMatch macMatch = macExpression.match(line);
        if (!macMatch.hasMatch())
        {
            continue;
        }

        return normalizeMacAddress(macMatch.captured(1));
    }

    return QString();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Normalizes a textual MAC address.
 * @param macAddress Colon-separated or hyphen-separated MAC address.
 * @return Uppercase colon-separated MAC address or an empty string when invalid.
 * @detail Validation requires exactly six hexadecimal octets.
 */
QString MainWindow::normalizeMacAddress(const QString &macAddress) const
{
    QString normalized = macAddress.trimmed();
    normalized.replace(QLatin1Char('-'), QLatin1Char(':'));

    const QStringList octets = normalized.split(QLatin1Char(':'));
    if (octets.size() != 6)
    {
        return QString();
    }

    const QRegularExpression octetExpression(
        QStringLiteral("^[0-9A-Fa-f]{1,2}$"));
    QStringList uppercaseOctets;
    for (const QString &octet : octets)
    {
        if (!octetExpression.match(octet).hasMatch())
        {
            return QString();
        }

        uppercaseOctets.append(
            octet.rightJustified(2, QLatin1Char('0')).toUpper());
    }

    return uppercaseOctets.join(QLatin1Char(':'));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Counts successful ping replies.
 * @param output Merged ping standard output and standard error.
 * @return Number of case-insensitive TTL fields found in the output.
 * @detail TTL is stable across localized ping output and avoids localized EVENTS.
 */
int MainWindow::pingReplyCount(const QString &output) const
{
    const QRegularExpression ttlExpression(
        QStringLiteral("\\bttl\\s*[=:]\\s*\\d+\\b"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator iterator =
        ttlExpression.globalMatch(output);

    int count = 0;
    while (iterator.hasNext())
    {
        iterator.next();
        ++count;
    }

    return count;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Parses the UDP initial counter value.
 * @param value Output pointer for the parsed unsigned value.
 * @return true when decimal or 0x-prefixed input fits the selected UDP counter.
 * @detail The method does not modify the interface and is used by normalization.
 */
bool MainWindow::parseUdpInitialValue(quint64 *value) const
{
    if (value == nullptr)
    {
        return false;
    }

    QString text = ui->udpInitValueLineEdit->text().trimmed();
    if (text.isEmpty())
    {
        return false;
    }

    const bool hexadecimal =
        text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive);
    if (hexadecimal)
    {
        text.remove(0, 2);
    }

    if (text.isEmpty())
    {
        return false;
    }

    bool conversionOk = false;
    const quint64 convertedValue =
        text.toULongLong(&conversionOk, hexadecimal ? 16 : 10);
    if (!conversionOk || convertedValue > udpMaximumCounterValue())
    {
        return false;
    }

    *value = convertedValue;
    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns the selected UDP counter size in bytes.
 * @param none
 * @return A size of 1, 2, 4, or 8 bytes.
 * @detail An unexpected GUI value is safely converted to one byte.
 */
quint64 MainWindow::udpCounterBytes() const
{
    bool conversionOk = false;
    const int counterBits =
        ui->udpCounterBitsComboBox->currentText().toInt(&conversionOk, 10);
    if (!conversionOk || counterBits <= 0)
    {
        return 1U;
    }

    return static_cast<quint64>(counterBits / 8);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns the maximum value of the selected UDP counter.
 * @param none
 * @return Maximum unsigned value for the current UDP counter width.
 * @detail Uses numeric_limits for 64 bits to avoid an invalid shift.
 */
quint64 MainWindow::udpMaximumCounterValue() const
{
    bool conversionOk = false;
    const int counterBits =
        ui->udpCounterBitsComboBox->currentText().toInt(&conversionOk, 10);

    if (!conversionOk || counterBits <= 0)
    {
        return std::numeric_limits<quint8>::max();
    }

    if (counterBits >= 64)
    {
        return std::numeric_limits<quint64>::max();
    }

    return (quint64(1) << counterBits) - quint64(1);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Reads and validates Pattern settings.

 * @param settings Output pointer for the validated settings.
 * @param errorText Output pointer for a validation error message.
 * @return true when all values are valid; otherwise false.
 * @detail Validates bit width, ranges, block alignment, period, and init value.
 */
bool MainWindow::readPatternSettings(PatternSettings *settings,
                                     QString *errorText) const
{
    if (settings == nullptr || errorText == nullptr)
    {
        return false;
    }

    bool counterBitsOk = false;
    const int counterBitsValue =
        ui->counterBitsComboBox->currentText().toInt(&counterBitsOk, 10);
    if (!counterBitsOk
        || (counterBitsValue != 8
            && counterBitsValue != 16
            && counterBitsValue != 32
            && counterBitsValue != 64))
    {
        *errorText = tr("invalid counter width");
        return false;
    }

    bool blockOk = false;
    const quint64 blockBytesValue =
        ui->blockBytesLineEdit->text().toULongLong(&blockOk, 10);
    if (!blockOk
        || blockBytesValue == 0
        || blockBytesValue
               > static_cast<quint64>(std::numeric_limits<int>::max()))
    {
        *errorText = tr("block, bytes must be a positive number");
        return false;
    }

    const quint64 counterBytesValue =
        static_cast<quint64>(counterBitsValue / 8);
    if ((blockBytesValue % counterBytesValue) != 0)
    {
        *errorText = tr("block, bytes must be a multiple of the counter size");
        return false;
    }

    bool periodOk = false;
    const quint64 periodValue =
        ui->periodMsLineEdit->text().toULongLong(&periodOk, 10);
    if (!periodOk
        || periodValue
               > static_cast<quint64>(std::numeric_limits<int>::max()))
    {
        *errorText = tr("Period, ms is outside the valid range");
        return false;
    }

    quint64 initialValue = 0;
    if (!parseInitialValue(&initialValue))
    {
        *errorText = tr("init value does not fit the selected counter width");
        return false;
    }

    settings->counterBits = counterBitsValue;
    settings->counterBytes = static_cast<int>(counterBytesValue);
    settings->blockBytes = static_cast<int>(blockBytesValue);
    settings->periodMs = static_cast<int>(periodValue);
    settings->initialValue = initialValue;
    settings->maximumCounterValue = maximumCounterValue();
    errorText->clear();
    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Converts the init value field to an integer.
 * @param value Output pointer for the converted value.
 * @return true when conversion succeeds and the value fits the selected width.
 * @detail Uses base 16 only for the 0x prefix; otherwise uses base 10.
 */
bool MainWindow::parseInitialValue(quint64 *value) const
{
    if (value == nullptr)
    {
        return false;
    }

    QString text = ui->initValueLineEdit->text().trimmed();
    if (text.isEmpty())
    {
        return false;
    }

    const bool hexadecimal =
        text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive);
    if (hexadecimal)
    {
        text.remove(0, 2);
    }

    if (text.isEmpty())
    {
        return false;
    }

    bool conversionOk = false;
    const quint64 convertedValue =
        text.toULongLong(&conversionOk, hexadecimal ? 16 : 10);
    if (!conversionOk || convertedValue > maximumCounterValue())
    {
        return false;
    }

    *value = convertedValue;
    return true;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Formats elapsed test time.
 * @param elapsedMilliseconds Elapsed duration in milliseconds.
 * @return A HH:MM:SS string without a 24-hour limit.
 * @detail Hours are calculated from the complete duration and never wrap at midnight.
 */
QString MainWindow::formatElapsedTime(qint64 elapsedMilliseconds) const
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
 * @param speedKbps Receive speed in decimal kilobits per second.
 * @return A string containing at most three digits after the decimal point.
 * @detail Removes insignificant trailing zeros and returns 0 for non-positive values.
 */
QString MainWindow::formatSpeed(double speedKbps) const
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
 * @brief Builds a Pattern description for the event log.
 * @param settings Validated Pattern settings.
 * @return A string containing counter, init, block, period, values/block, and bo=LE.
 * @detail For hexadecimal input, also includes the decimal equivalent.
 */
QString MainWindow::patternDescription(
    const PatternSettings &settings) const
{
    QString initialValueText = ui->initValueLineEdit->text().trimmed();
    if (initialValueText.startsWith(QStringLiteral("0x"),
                                    Qt::CaseInsensitive))
    {
        initialValueText +=
            tr(" (dec %1)").arg(QString::number(settings.initialValue));
    }

    const int valuesInBlock = settings.blockBytes / settings.counterBytes;
    return tr("Pattern: counter=%1 bits; init=%2; block=%3 bytes; "
              "period=%4 ms; values/block=%5; bo=LE")
        .arg(settings.counterBits)
        .arg(initialValueText)
        .arg(settings.blockBytes)
        .arg(settings.periodMs)
        .arg(valuesInBlock);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Builds a description of the selected COM-port settings.
 * @param none
 * @return A baud/data bits/parity/stops/flow control description string.
 * @detail Used by RxWorker in port-open and port-close events.
 */
QString MainWindow::serialSettingsDescription() const
{
    return tr("baud=%1; data bits=8; parity=%2; stops=%3; flow control=NONE")
        .arg(ui->baudComboBox->currentText(),
             ui->parityComboBox->currentText(),
             ui->stopsComboBox->currentText());
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Builds a human-readable description of QSerialPortInfo.
 * @param portInfo Serial-port information to describe.
 * @return The port name with description and manufacturer when available.
 * @detail Empty and duplicate optional fields are omitted.
 */
QString MainWindow::portDescription(
    const QSerialPortInfo &portInfo) const
{
    QStringList details;
    const QString description = portInfo.description().trimmed();
    const QString manufacturer = portInfo.manufacturer().trimmed();

    if (!description.isEmpty())
    {
        details.append(description);
    }

    if (!manufacturer.isEmpty() && manufacturer != description)
    {
        details.append(manufacturer);
    }

    if (details.isEmpty())
    {
        return portInfo.portName();
    }

    return QStringLiteral("%1 (%2)")
        .arg(portInfo.portName(), details.join(QStringLiteral("; ")));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns the selected counter size in bytes.
 * @param none
 * @return A size of 1, 2, 4, or 8 bytes.
 * @detail An unexpected GUI value is safely converted to one byte.
 */
quint64 MainWindow::counterBytes() const
{
    const int counterBitsValue =
        ui->counterBitsComboBox->currentText().toInt();
    if (counterBitsValue <= 0 || (counterBitsValue % 8) != 0)
    {
        return 1;
    }

    return static_cast<quint64>(counterBitsValue / 8);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Returns the maximum value of the selected counter.
 * @param none
 * @return The maximum unsigned value for the current counter width.
 * @detail Uses numeric_limits for 64 bits to avoid an invalid shift.
 */
quint64 MainWindow::maximumCounterValue() const
{
    const int counterBitsValue =
        ui->counterBitsComboBox->currentText().toInt();

    if (counterBitsValue >= 64)
    {
        return std::numeric_limits<quint64>::max();
    }

    if (counterBitsValue <= 0)
    {
        return 0;
    }

    return (quint64(1) << counterBitsValue) - 1U;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Converts the selected parity to a QSerialPort value.
 * @param none
 * @return QSerialPort::NoParity, EvenParity, or OddParity.
 * @detail Unknown text is safely interpreted as NONE.
 */
QSerialPort::Parity MainWindow::selectedParity() const
{
    const QString parityText = ui->parityComboBox->currentText();
    if (parityText == QStringLiteral("EVEN"))
    {
        return QSerialPort::EvenParity;
    }

    if (parityText == QStringLiteral("ODD"))
    {
        return QSerialPort::OddParity;
    }

    return QSerialPort::NoParity;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Converts the selected stop-bit count to a QSerialPort value.
 * @param none
 * @return QSerialPort::OneStop or QSerialPort::TwoStop.
 * @detail Text 2 maps to TwoStop; every other value maps to OneStop.
 */
QSerialPort::StopBits MainWindow::selectedStopBits() const
{
    if (ui->stopsComboBox->currentText() == QStringLiteral("2"))
    {
        return QSerialPort::TwoStop;
    }

    return QSerialPort::OneStop;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Selects a text value in a QComboBox.
 * @param comboBox Pointer to the combo box.
 * @param value Text value to select.
 * @return none
 * @detail Changes the current index only when an exact match is found.
 */
void MainWindow::selectComboBoxText(QComboBox *comboBox,
                                    const QString &value) const
{
    if (comboBox == nullptr)
    {
        return;
    }

    const int index = comboBox->findText(value);
    if (index >= 0)
    {
        comboBox->setCurrentIndex(index);
    }
}
