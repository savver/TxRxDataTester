#include "mainwindow.h"
#include "txworker.h"
#include "udptxworker.h"
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
#include <QLineEdit>
#include <QMetaObject>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
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
 * @detail Initializes the GUI, validators, settings, event log, and dedicated COM and
 *         UDP transmission worker threads.
 */
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_txWorker(nullptr)
    , m_udpTxWorker(nullptr)
    , m_pingProcess(nullptr)
    , m_neighborLookupProcess(nullptr)
    , m_portRefreshTimer(this)
    , m_pingTimeoutTimer(this)
    , m_udpDestinationPort(0)
    , m_udpLocalPort(0)
    , m_workerReady(false)
    , m_udpWorkerReady(false)
    , m_portOpen(false)
    , m_testRunning(false)
    , m_singleTransferActive(false)
    , m_outputDrainActive(false)
    , m_portOperationPending(false)
    , m_transmissionCommandPending(false)
    , m_portLossRequestPending(false)
    , m_portSnapshotInitialized(false)
    , m_udpConnected(false)
    , m_udpTestRunning(false)
    , m_udpSingleTransferActive(false)
    , m_udpConnectionOperationPending(false)
    , m_udpTransmissionCommandPending(false)
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
    connect(ui->singleButton,
            &QPushButton::clicked,
            this,
            &MainWindow::handleSingleButton);
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
    connect(ui->udpSingleButton,
            &QPushButton::clicked,
            this,
            &MainWindow::handleUdpSingleButton);
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
            [this](int exitCode, QProcess::ExitStatus exitStatus)
            {
                handlePingFinished(exitCode, static_cast<int>(exitStatus));
            });
    connect(m_pingProcess,
            &QProcess::errorOccurred,
            this,
            [this](QProcess::ProcessError processError)
            {
                handlePingProcessError(static_cast<int>(processError));
            });

    m_neighborLookupProcess = new QProcess(this);
    m_neighborLookupProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_neighborLookupProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus)
            {
                handleNeighborLookupFinished(exitCode,
                                             static_cast<int>(exitStatus));
            });
    connect(m_neighborLookupProcess,
            &QProcess::errorOccurred,
            this,
            [this](QProcess::ProcessError processError)
            {
                handleNeighborLookupProcessError(
                    static_cast<int>(processError));
            });

    initializeLogFile();
    appendEvent(tr("TxDataTester (v.1.7) started"), EventType::Normal);

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
    ui->txBytesLineEdit->setText(QStringLiteral("0"));
    ui->speedLineEdit->setText(QStringLiteral("0"));

    quint64 initialValue = 0;
    parseInitialValue(&initialValue);
    ui->currentCountLineEdit->setText(QString::number(initialValue));

    ui->udpStartTimeValueLabel->setText(QStringLiteral("--:--:--"));
    ui->udpElapsedTimeValueLabel->setText(QStringLiteral("00:00:00"));
    ui->udpTxBytesLineEdit->setText(QStringLiteral("0"));
    ui->udpPacketsPerSecondLineEdit->setText(QStringLiteral("0"));
    ui->udpSpeedLineEdit->setText(QStringLiteral("0"));

    quint64 udpInitialValue = 0;
    parseUdpInitialValue(&udpInitialValue);
    ui->udpCurrentCountLineEdit->setText(QString::number(udpInitialValue));

    initializeTxThread();
    initializeUdpTxThread();
    refreshSerialPorts();
    updateControlStates();
    m_portRefreshTimer.start();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Destroys the main application window.
 * @param none
 * @return none
 * @detail Ensures orderly shutdown of both transmission threads and the log before
 *         releasing the Qt Designer user interface.
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
 * @detail Stops the COM and UDP transmission threads, saves settings, closes the log,
 *         and then passes the event to QMainWindow.
 */
void MainWindow::closeEvent(QCloseEvent *event)
{
    prepareShutdown();
    QMainWindow::closeEvent(event);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Refreshes the list of available serial ports.
 * @param none
 * @return none
 * @detail When closed, updates the combo box and logs added or removed ports; when
 *         open, monitors the selected device.
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
 * @detail Prevents duplicate operations and sends the selected settings to TxWorker
 *         without creating QSerialPort in the GUI thread.
 */
void MainWindow::openSerialPort()
{
    if (m_udpConnected
        || m_pingInProgress
        || m_udpConnectionOperationPending
        || m_udpTestRunning
        || m_udpSingleTransferActive)
    {
        appendEvent(tr("open error: disconnect the UDP destination before opening a COM port"),
                    EventType::Error);
        return;
    }

    if (!m_workerReady
        || m_portOpen
        || m_portOperationPending
        || m_txWorker == nullptr)
    {
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
 * @detail Sends a queued command to the QSerialPort owner and blocks duplicate commands
 *         until the state reply arrives.
 */
void MainWindow::closeSerialPort()
{
    if (!m_workerReady
        || !m_portOpen
        || m_portOperationPending
        || m_txWorker == nullptr)
    {
        updateControlStates();
        return;
    }

    m_portOperationPending = true;
    m_transmissionCommandPending = true;
    updateControlStates();
    emit closePortRequested();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Normalizes the transmitted block size.
 * @param none
 * @return none
 * @detail Replaces an empty or zero value with one counter, rounds upward to the
 *         required alignment, and clamps the result to int range.
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
 * @detail Accepts decimal or 0x-prefixed hexadecimal input, preserves hexadecimal
 *         format, and falls back to zero on error or overflow.
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
 * @brief Normalizes the block transmission period.
 * @param none
 * @return none
 * @detail Converts an empty field to zero and clamps values above INT_MAX to the
 *         largest QTimer interval supported by Qt 5.12.
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
 * @detail Uses the start bit, eight data bits, optional parity, selected stop bits,
 *         baud rate, and block size, then rounds upward to microseconds.
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
 * @detail Realigns block size and validates the initial-value range again.
 */
void MainWindow::handleCounterBitsChanged()
{
    normalizeBlockSize();
    normalizeInitialValue();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Requests start of continuous transmission.
 * @param none
 * @return none
 * @detail Logs the START button press in green, validates Pattern, and sends the
 *         command to the dedicated TX thread.
 */
void MainWindow::startTest()
{
    if (m_transmissionCommandPending || m_testRunning)
    {
        return;
    }

    appendEvent(tr("START button pressed"), EventType::Action);

    if (!m_workerReady || !m_portOpen || m_txWorker == nullptr)
    {
        appendEvent(tr("START failed: the COM port is not open"), EventType::Error);
        return;
    }

    if (m_singleTransferActive || m_outputDrainActive)
    {
        appendEvent(tr("START failed: the previous transmission has not finished yet"),
                    EventType::Error);
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

    m_transmissionCommandPending = true;
    updateControlStates();
    emit startContinuousRequested(settings.counterBits,
                                  settings.blockBytes,
                                  settings.periodMs,
                                  settings.initialValue,
                                  patternDescription(settings));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Requests a soft stop of continuous transmission.
 * @param none
 * @return none
 * @detail Stores the exact click timestamp and asks TxWorker to stop generation and
 *         report the pending output size.
 */
void MainWindow::stopTest()
{
    if (!m_testRunning
        || m_transmissionCommandPending
        || !m_workerReady
        || m_txWorker == nullptr)
    {
        return;
    }

    m_pendingStopTimestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    m_transmissionCommandPending = true;
    updateControlStates();
    emit stopContinuousRequested();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Requests transmission of one block.
 * @param none
 * @return none
 * @detail Logs the SINGLE button press in green, validates Pattern, and sends scalar
 *         parameters to the worker thread.
 */
void MainWindow::handleSingleButton()
{
    if (m_transmissionCommandPending)
    {
        return;
    }

    appendEvent(tr("SINGLE button pressed"), EventType::Action);

    if (!m_workerReady || !m_portOpen || m_txWorker == nullptr)
    {
        appendEvent(tr("SINGLE failed: the COM port is not open"), EventType::Error);
        return;
    }

    if (m_testRunning || m_singleTransferActive || m_outputDrainActive)
    {
        appendEvent(tr("SINGLE failed: the previous transmission has not finished yet"),
                    EventType::Error);
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

    m_transmissionCommandPending = true;
    updateControlStates();
    emit singleRequested(settings.counterBits,
                         settings.blockBytes,
                         settings.periodMs,
                         settings.initialValue,
                         patternDescription(settings));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Normalizes the UDP block size.
 * @param none
 * @return none
 * @detail Rounds upward to the counter alignment and clamps the payload to the maximum
 *         IPv4 UDP payload size.
 */
void MainWindow::normalizeUdpBlockSize()
{
    const quint64 alignment = udpCounterBytes();
    const quint64 maximumAligned =
        kMaximumIpv4UdpPayloadBytes
        - (kMaximumIpv4UdpPayloadBytes % alignment);

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
 * @detail Accepts decimal or 0x-prefixed hexadecimal input and replaces invalid or
 *         out-of-range values with zero.
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
 * @brief Normalizes the UDP packet period.
 * @param none
 * @return none
 * @detail Converts an empty field to zero and clamps values above INT_MAX to the
 *         largest QTimer interval supported by Qt 5.12.
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
 * @detail Converts an empty or zero value to one and clamps values above INT_MAX to
 *         the largest worker-loop count accepted by the interface.
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
 * @detail Realigns the UDP block size and validates the UDP initial counter value.
 */
void MainWindow::handleUdpCounterBitsChanged()
{
    normalizeUdpBlockSize();
    normalizeUdpInitialValue();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Requests start of continuous UDP transmission.
 * @param none
 * @return none
 * @detail Logs the UDP START button press in green, validates Pattern, and sends the
 *         command to the dedicated UDP worker thread.
 */
void MainWindow::startUdpTest()
{
    if (m_udpTransmissionCommandPending)
    {
        return;
    }

    appendEvent(tr("UDP START button pressed"), EventType::Action);

    if (!m_udpWorkerReady
        || !m_udpConnected
        || m_udpTxWorker == nullptr)
    {
        appendEvent(tr("UDP START failed: the destination did not pass CONNECT"),
                    EventType::Error);
        return;
    }

    if (m_udpTestRunning || m_udpSingleTransferActive)
    {
        appendEvent(tr("UDP START failed: the previous transmission has not finished yet"),
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

    m_udpTransmissionCommandPending = true;
    updateControlStates();
    emit startUdpContinuousRequested(settings.counterBits,
                                     settings.blockBytes,
                                     settings.togetherCount,
                                     settings.periodMs,
                                     settings.initialValue,
                                     udpPatternDescription(settings));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Requests stop of continuous UDP transmission.
 * @param none
 * @return none
 * @detail Logs the UDP STOP button press in green and stops generation of new
 *         datagrams in the UDP worker thread.
 */
void MainWindow::stopUdpTest()
{
    if (!m_udpWorkerReady
        || !m_udpTestRunning
        || m_udpTransmissionCommandPending
        || m_udpTxWorker == nullptr)
    {
        updateControlStates();
        return;
    }

    appendEvent(tr("UDP STOP button pressed"), EventType::Action);
    m_udpTransmissionCommandPending = true;
    updateControlStates();
    emit stopUdpContinuousRequested();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Requests transmission of one UDP datagram.
 * @param none
 * @return none
 * @detail Logs the UDP SINGLE button press in green and sends one Pattern datagram in
 *         the UDP worker thread.
 */
void MainWindow::handleUdpSingleButton()
{
    if (m_udpTransmissionCommandPending)
    {
        return;
    }

    appendEvent(tr("UDP SINGLE button pressed"), EventType::Action);

    if (!m_udpWorkerReady
        || !m_udpConnected
        || m_udpTxWorker == nullptr)
    {
        appendEvent(tr("UDP SINGLE failed: the destination did not pass CONNECT"),
                    EventType::Error);
        return;
    }

    if (m_udpTestRunning || m_udpSingleTransferActive)
    {
        appendEvent(tr("UDP SINGLE failed: the previous transmission has not finished yet"),
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

    m_udpTransmissionCommandPending = true;
    updateControlStates();
    emit singleUdpRequested(settings.counterBits,
                            settings.blockBytes,
                            settings.togetherCount,
                            settings.periodMs,
                            settings.initialValue,
                            udpPatternDescription(settings));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Connects or disconnects the logical UDP destination.
 * @param none
 * @return none
 * @detail CONNECT validates the destination, resolves the outgoing interface, and
 *         starts one short-timeout ping request. DISCONNECT closes the UDP socket.
 */
void MainWindow::handleUdpConnectButton()
{
    if (m_shutdownPrepared
        || m_pingInProgress
        || m_udpConnectionOperationPending
        || m_udpTransmissionCommandPending)
    {
        return;
    }

    if (m_udpConnected)
    {
        if (!m_udpWorkerReady
            || m_udpTxWorker == nullptr
            || m_udpTestRunning
            || m_udpSingleTransferActive)
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

    if (m_portOpen || m_portOperationPending)
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
    m_udpLocalPort = 0;
    ui->udpOurIpValueLabel->setText(m_udpOurIp);
    ui->udpOurMacValueLabel->setText(m_udpOurMac);
    ui->udpDestinationMacValueLabel->setText(QStringLiteral("--"));

    appendEvent(udpConnectionDescription(destinationIp, destinationPort),
                EventType::Normal);
    startUdpPing(destinationIp);
}
/*-----------------------------------------------------------------------------*/

/**
 * @brief Starts an asynchronous one-request IPv4 ping.
 * @param destinationIp Validated destination IPv4 address.
 * @return none
 * @detail Uses a short native reply timeout plus an application watchdog and records
 *         only fixed English result text in EVENTS and the log.
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
 * @param exitStatus Numeric QProcess::ExitStatus value.
 * @return none
 * @detail Counts TTL fields, reports packet loss, locks a reachable destination, and
 *         starts a destination-MAC lookup.
 */
void MainWindow::handlePingFinished(int exitCode, int exitStatus)
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
    const bool normalExit =
        exitStatus == static_cast<int>(QProcess::NormalExit);
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
        if (!m_udpWorkerReady || m_udpTxWorker == nullptr)
        {
            setUdpConnectionState(false);
            appendEvent(tr("UDP Connection error: the UDP worker thread is not ready"),
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
        setUdpConnectionState(false);
        m_udpDestinationMac.clear();
        ui->udpDestinationMacValueLabel->setText(
            QStringLiteral("--"));
        updateControlStates();
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles an asynchronous ping-process error.
 * @param processError Numeric QProcess::ProcessError value.
 * @return none
 * @detail Reports a fixed English process diagnostic and unlocks the destination
 *         fields after the failed connection attempt.
 */
void MainWindow::handlePingProcessError(int processError)
{
    if (!m_pingInProgress)
    {
        return;
    }

    m_pingTimeoutTimer.stop();
    m_pingInProgress = false;
    setUdpConnectionState(false);
    m_udpDestinationMac.clear();
    ui->udpDestinationMacValueLabel->setText(QStringLiteral("--"));
    appendEvent(tr("PING process error for %1: %2")
                    .arg(m_pingDestinationIp,
                         processErrorText(
                             static_cast<QProcess::ProcessError>(processError))),
                EventType::Error);
    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles expiration of the application-level ping watchdog.
 * @param none
 * @return none
 * @detail Terminates a ping process that exceeded the short connection timeout,
 *         reports an error, and unlocks the destination fields.
 */
void MainWindow::handlePingTimeout()
{
    if (!m_pingInProgress || m_pingProcess == nullptr)
    {
        return;
    }

    m_pingInProgress = false;
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
 * @param exitStatus Numeric QProcess::ExitStatus value.
 * @return none
 * @detail Extracts the target MAC from numeric output and updates Connection without
 *         displaying localized process text.
 */
void MainWindow::handleNeighborLookupFinished(int exitCode, int exitStatus)
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
        ui->udpDestinationMacValueLabel->setText(
            QStringLiteral("--"));
        appendEvent(tr("destination MAC is unavailable for %1; the target may be outside the local subnet or absent from the neighbor table")
                        .arg(m_neighborLookupIp),
                    EventType::Normal);
    }

    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles an asynchronous neighbor-table process error.
 * @param processError Numeric QProcess::ProcessError value.
 * @return none
 * @detail Keeps the successful logical connection, displays an unavailable MAC, and
 *         records a fixed English diagnostic.
 */
void MainWindow::handleNeighborLookupProcessError(int processError)
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
                         processErrorText(
                             static_cast<QProcess::ProcessError>(processError))),
                EventType::Error);
    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles readiness of the dedicated TX worker thread.
 * @param none
 * @return none
 * @detail Marks QSerialPort and worker timers as ready and updates the controls.
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
 * @brief Receives a normal or error event from TxWorker.
 * @param timestamp Timestamp created in the worker thread.
 * @param text Event text without a timestamp.
 * @param error true for a red error entry; otherwise false.
 * @return none
 * @detail Displays and logs the event in the main GUI thread while preserving the
 *         worker-side timestamp.
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
 * @brief Receives a new COM-port state from TxWorker.
 * @param open true when the port was opened successfully.
 * @param portName Name of the opened or just-closed port.
 * @param settingsDescription Description of the applied port settings.
 * @param causedByFailure true when closing was caused by a failure.
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
        m_singleTransferActive = false;
        m_outputDrainActive = false;
        m_transmissionCommandPending = false;
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
 * @brief Receives transmission-operation states from TxWorker.
 * @param testRunning true while continuous START transmission is active.
 * @param singleTransferActive true until a SINGLE block is fully written.
 * @param outputDrainActive true while the STOP remainder is draining.
 * @return none
 * @detail Clears pending queued-command state and synchronizes control availability.
 */
void MainWindow::handleTransmissionStateChanged(bool testRunning,
                                                bool singleTransferActive,
                                                bool outputDrainActive)
{
    m_testRunning = testRunning;
    m_singleTransferActive = singleTransferActive;
    m_outputDrainActive = outputDrainActive;
    m_transmissionCommandPending = false;
    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Receives a prepared Statistics snapshot from TxWorker.
 * @param startTime Test start time in HH:MM:SS format.
 * @param elapsedMilliseconds Elapsed test time in milliseconds.
 * @param totalBytesWritten Total number of confirmed transmitted bytes.
 * @param currentCounter Next counter value for a new block.
 * @param speedKbps Measured speed for the latest interval in Kb/s.
 * @return none
 * @detail Formats and displays the values without reading any TX-thread state.
 */
void MainWindow::handleStatisticsUpdated(const QString &startTime,
                                         qint64 elapsedMilliseconds,
                                         quint64 totalBytesWritten,
                                         quint64 currentCounter,
                                         double speedKbps)
{
    ui->startTimeValueLabel->setText(startTime.isEmpty()
                                         ? QStringLiteral("--:--:--")
                                         : startTime);
    ui->elapsedTimeValueLabel->setText(
        formatElapsedTime(elapsedMilliseconds));
    ui->txBytesLineEdit->setText(QString::number(totalBytesWritten));
    ui->currentCountLineEdit->setText(QString::number(currentCounter));
    ui->speedLineEdit->setText(formatSpeed(speedKbps));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Completes the green STOP button entry.
 * @param pendingBytes Output bytes pending after block generation stopped.
 * @return none
 * @detail Uses the timestamp captured at the click so queued delivery does not change
 *         the user-action time.
 */
void MainWindow::handleStopButtonAccepted(qint64 pendingBytes)
{
    const QString timestamp = m_pendingStopTimestamp.isEmpty()
                                  ? QDateTime::currentDateTime().toString(
                                        QStringLiteral("HH:mm:ss.zzz"))
                                  : m_pendingStopTimestamp;
    m_pendingStopTimestamp.clear();

    appendTimestampedEvent(
        timestamp,
        tr("STOP button pressed; generation of new blocks stopped; "
           "%1 bytes remain in the queue")
            .arg(qMax<qint64>(0, pendingBytes)),
        EventType::Action);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Handles readiness of the dedicated UDP TX worker thread.
 * @param none
 * @return none
 * @detail Marks QUdpSocket and UDP worker timers as ready and updates controls.
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
 * @brief Receives a normal or error event from UdpTxWorker.
 * @param timestamp Timestamp created in the UDP worker thread.
 * @param text Event text without a timestamp.
 * @param error true for a red error entry; otherwise false.
 * @return none
 * @detail Displays and logs the event in the GUI thread while preserving the
 *         worker-side timestamp.
 */
void MainWindow::handleUdpWorkerEvent(const QString &timestamp,
                                      const QString &text,
                                      bool error)
{
    appendTimestampedEvent(timestamp,
                           text,
                           error ? EventType::Error : EventType::Normal);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Receives a persistent UDP socket state from UdpTxWorker.
 * @param connected true when the socket is bound and ready.
 * @param destinationIp Configured destination IPv4 address.
 * @param destinationPort Configured destination UDP port.
 * @param localIp Bound local IPv4 address.
 * @param localPort Ephemeral local UDP port.
 * @param causedByFailure true when disconnection was caused by an error.
 * @return none
 * @detail Synchronizes logical connection state, tab locking, and editable destination
 *         controls without accessing QUdpSocket in the GUI thread.
 */
void MainWindow::handleUdpConnectionStateChanged(bool connected,
                                                 const QString &destinationIp,
                                                 quint16 destinationPort,
                                                 const QString &localIp,
                                                 quint16 localPort,
                                                 bool causedByFailure)
{
    const bool userDisconnect = m_udpDisconnectRequestedByUser;
    m_udpConnectionOperationPending = false;
    m_udpDisconnectRequestedByUser = false;
    setUdpConnectionState(connected);

    if (connected)
    {
        m_udpDestinationIp = destinationIp;
        m_udpDestinationPort = destinationPort;
        m_udpOurIp = localIp;
        m_udpLocalPort = localPort;
        ui->udpOurIpValueLabel->setText(
            m_udpOurIp.isEmpty() ? QStringLiteral("--") : m_udpOurIp);
    }
    else
    {
        m_udpTestRunning = false;
        m_udpSingleTransferActive = false;
        m_udpTransmissionCommandPending = false;
        m_udpLocalPort = 0;

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

        if (userDisconnect && !m_shutdownPrepared)
        {
            appendEvent(tr("UDP destination settings unlocked"),
                        EventType::Normal);
        }
    }

    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Receives UDP transmission-operation states from UdpTxWorker.
 * @param testRunning true while continuous burst generation is active.
 * @param singleTransferActive true while SINGLE is being processed.
 * @return none
 * @detail Clears pending command state and synchronizes UDP control availability.
 */
void MainWindow::handleUdpTransmissionStateChanged(bool testRunning,
                                                   bool singleTransferActive)
{
    m_udpTestRunning = testRunning;
    m_udpSingleTransferActive = singleTransferActive;
    m_udpTransmissionCommandPending = false;
    updateControlStates();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Receives a prepared UDP Statistics snapshot.
 * @param startTime Test start time in HH:MM:SS format.
 * @param elapsedMilliseconds Elapsed test time in milliseconds.
 * @param totalPayloadBytes Total payload bytes accepted by writeDatagram().
 * @param currentCounter Next counter value for a new payload field.
 * @param speedKbps Measured payload speed for the latest interval in Kb/s.
 * @param packetsPerSecond Measured datagrams per second for the latest interval.
 * @return none
 * @detail Formats and displays values without reading UDP worker-thread state.
 */
void MainWindow::handleUdpStatisticsUpdated(const QString &startTime,
                                            qint64 elapsedMilliseconds,
                                            quint64 totalPayloadBytes,
                                            quint64 currentCounter,
                                            double speedKbps,
                                            double packetsPerSecond)
{
    ui->udpStartTimeValueLabel->setText(
        startTime.isEmpty() ? QStringLiteral("--:--:--") : startTime);
    ui->udpElapsedTimeValueLabel->setText(
        formatElapsedTime(elapsedMilliseconds));
    ui->udpTxBytesLineEdit->setText(QString::number(totalPayloadBytes));
    ui->udpCurrentCountLineEdit->setText(QString::number(currentCounter));
    ui->udpPacketsPerSecondLineEdit->setText(formatSpeed(packetsPerSecond));
    ui->udpSpeedLineEdit->setText(formatSpeed(speedKbps));
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates and starts the dedicated TX worker thread.
 * @param none
 * @return none
 * @detail Moves TxWorker to QThread, connects both directions with queued connections,
 *         and starts the thread after setup is complete.
 */
void MainWindow::initializeTxThread()
{
    m_txWorker = new TxWorker;
    m_txWorker->moveToThread(&m_txThread);
    m_txThread.setObjectName(QStringLiteral("TxDataTester_TX_Worker"));

    connect(&m_txThread,
            &QThread::started,
            m_txWorker,
            &TxWorker::initialize);
    connect(&m_txThread,
            &QThread::finished,
            m_txWorker,
            &QObject::deleteLater);

    connect(this,
            &MainWindow::openPortRequested,
            m_txWorker,
            &TxWorker::openPort,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::closePortRequested,
            m_txWorker,
            &TxWorker::closePort,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::startContinuousRequested,
            m_txWorker,
            &TxWorker::startContinuous,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::stopContinuousRequested,
            m_txWorker,
            &TxWorker::stopContinuous,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::singleRequested,
            m_txWorker,
            &TxWorker::sendSingle,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::externalPortLossDetected,
            m_txWorker,
            &TxWorker::handleExternalPortLoss,
            Qt::QueuedConnection);

    connect(m_txWorker,
            &TxWorker::workerReady,
            this,
            &MainWindow::handleWorkerReady,
            Qt::QueuedConnection);
    connect(m_txWorker,
            &TxWorker::eventGenerated,
            this,
            &MainWindow::handleWorkerEvent,
            Qt::QueuedConnection);
    connect(m_txWorker,
            &TxWorker::portStateChanged,
            this,
            &MainWindow::handlePortStateChanged,
            Qt::QueuedConnection);
    connect(m_txWorker,
            &TxWorker::transmissionStateChanged,
            this,
            &MainWindow::handleTransmissionStateChanged,
            Qt::QueuedConnection);
    connect(m_txWorker,
            &TxWorker::statisticsUpdated,
            this,
            &MainWindow::handleStatisticsUpdated,
            Qt::QueuedConnection);
    connect(m_txWorker,
            &TxWorker::periodicLogLineReady,
            this,
            &MainWindow::writeLogLine,
            Qt::QueuedConnection);
    connect(m_txWorker,
            &TxWorker::stopButtonAccepted,
            this,
            &MainWindow::handleStopButtonAccepted,
            Qt::QueuedConnection);

    m_txThread.start();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates and starts the dedicated UDP TX worker thread.
 * @param none
 * @return none
 * @detail Moves UdpTxWorker to QThread, connects both directions with queued
 *         connections, and starts the thread after setup is complete.
 */
void MainWindow::initializeUdpTxThread()
{
    m_udpTxWorker = new UdpTxWorker;
    m_udpTxWorker->moveToThread(&m_udpTxThread);
    m_udpTxThread.setObjectName(QStringLiteral("TxDataTester_UDP_TX_Worker"));

    connect(&m_udpTxThread,
            &QThread::started,
            m_udpTxWorker,
            &UdpTxWorker::initialize);
    connect(&m_udpTxThread,
            &QThread::finished,
            m_udpTxWorker,
            &QObject::deleteLater);

    connect(this,
            &MainWindow::configureUdpConnectionRequested,
            m_udpTxWorker,
            &UdpTxWorker::configureConnection,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::disconnectUdpConnectionRequested,
            m_udpTxWorker,
            &UdpTxWorker::disconnectConnection,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::startUdpContinuousRequested,
            m_udpTxWorker,
            &UdpTxWorker::startContinuous,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::stopUdpContinuousRequested,
            m_udpTxWorker,
            &UdpTxWorker::stopContinuous,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::singleUdpRequested,
            m_udpTxWorker,
            &UdpTxWorker::sendSingle,
            Qt::QueuedConnection);

    connect(m_udpTxWorker,
            &UdpTxWorker::workerReady,
            this,
            &MainWindow::handleUdpWorkerReady,
            Qt::QueuedConnection);
    connect(m_udpTxWorker,
            &UdpTxWorker::eventGenerated,
            this,
            &MainWindow::handleUdpWorkerEvent,
            Qt::QueuedConnection);
    connect(m_udpTxWorker,
            &UdpTxWorker::connectionStateChanged,
            this,
            &MainWindow::handleUdpConnectionStateChanged,
            Qt::QueuedConnection);
    connect(m_udpTxWorker,
            &UdpTxWorker::transmissionStateChanged,
            this,
            &MainWindow::handleUdpTransmissionStateChanged,
            Qt::QueuedConnection);
    connect(m_udpTxWorker,
            &UdpTxWorker::statisticsUpdated,
            this,
            &MainWindow::handleUdpStatisticsUpdated,
            Qt::QueuedConnection);
    connect(m_udpTxWorker,
            &UdpTxWorker::periodicLogLineReady,
            this,
            &MainWindow::writeLogLine,
            Qt::QueuedConnection);

    m_udpTxThread.start();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Updates availability of all controls.
 * @param none
 * @return none
 * @detail Considers worker readiness, port state, active transmission, output draining,
 *         and pending asynchronous GUI commands.
 */
void MainWindow::updateControlStates()
{
    const bool comTransferBusy = m_testRunning
                                 || m_singleTransferActive
                                 || m_outputDrainActive;
    const bool comAsynchronousBusy = m_portOperationPending
                                     || m_transmissionCommandPending
                                     || m_portLossRequestPending;
    const bool comWorkerAvailable = m_workerReady && !m_shutdownPrepared;

    const bool udpTransferBusy = m_udpTestRunning
                                 || m_udpSingleTransferActive;
    const bool udpConnectionBusy = m_pingInProgress
                                   || m_udpConnectionOperationPending;
    const bool udpCommandBusy = m_udpTransmissionCommandPending;
    const bool udpWorkerAvailable = m_udpWorkerReady && !m_shutdownPrepared;

    const bool udpModeLocksCom = m_pingInProgress
                                 || m_udpConnectionOperationPending
                                 || m_udpConnected
                                 || m_udpTestRunning
                                 || m_udpSingleTransferActive
                                 || m_udpTransmissionCommandPending;
    const bool comModeLocksUdp = m_portOpen
                                 || m_portOperationPending
                                 || m_testRunning
                                 || m_singleTransferActive
                                 || m_outputDrainActive
                                 || m_transmissionCommandPending
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
                                   && !comTransferBusy
                                   && !comAsynchronousBusy;
    ui->counterBitsComboBox->setEnabled(comPatternEnabled);
    ui->blockBytesLineEdit->setEnabled(comPatternEnabled);
    ui->initValueLineEdit->setEnabled(comPatternEnabled);
    ui->periodMsLineEdit->setEnabled(comPatternEnabled);

    ui->startButton->setEnabled(comModeAvailable
                                && m_portOpen
                                && !comTransferBusy
                                && !comAsynchronousBusy);
    ui->stopButton->setEnabled(comWorkerAvailable
                               && m_portOpen
                               && m_testRunning
                               && !comAsynchronousBusy);
    ui->singleButton->setEnabled(comModeAvailable
                                 && m_portOpen
                                 && !comTransferBusy
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
                                        && !udpTransferBusy
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
                                   && !udpTransferBusy
                                   && !udpConnectionBusy
                                   && !udpCommandBusy;
    ui->udpCounterBitsComboBox->setEnabled(udpPatternEnabled);
    ui->udpBlockBytesLineEdit->setEnabled(udpPatternEnabled);
    ui->udpInitValueLineEdit->setEnabled(udpPatternEnabled);
    ui->udpTogetherLineEdit->setEnabled(udpPatternEnabled);
    ui->udpPeriodMsLineEdit->setEnabled(udpPatternEnabled);

    ui->udpStartButton->setEnabled(udpModeAvailable
                                   && m_udpConnected
                                   && !udpTransferBusy
                                   && !udpConnectionBusy
                                   && !udpCommandBusy);
    ui->udpStopButton->setEnabled(udpWorkerAvailable
                                  && m_udpConnected
                                  && m_udpTestRunning
                                  && !udpConnectionBusy
                                  && !udpCommandBusy);
    ui->udpSingleButton->setEnabled(udpModeAvailable
                                    && m_udpConnected
                                    && !udpTransferBusy
                                    && !udpConnectionBusy
                                    && !udpCommandBusy);
}


/*-----------------------------------------------------------------------------*/

/**
 * @brief Sets the logical UDP connection state.
 * @param connected true to lock destination settings; false to unlock them.
 * @return none
 * @detail Mirrors the persistent bound socket state reported by UdpTxWorker.
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
 * @detail Prevents stale route information from remaining visible after the
 *         destination is unlocked or edited.
 */
void MainWindow::clearUdpNetworkInformation()
{
    m_udpOurIp.clear();
    m_udpOurMac.clear();
    m_udpDestinationMac.clear();
    m_udpDestinationIp.clear();
    m_udpDestinationPort = 0;
    m_udpLocalPort = 0;
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
 * @detail Character validators block unwanted input, while this method verifies the
 *         complete numeric address and range.
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
        *errorText = tr("destination Port must be in range 1...65535");
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
 * @detail Checks counter width, IPv4 UDP payload size, alignment, Togeth, Period, and
 *         initial-value range.
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
 * @detail Searches active QNetworkInterface address entries and never returns a
 *         localized interface description.
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

    return tr("Connection Settings: dest_IP=%1; dest_PORT=%2; our_IP=%3; our_MAC=%4")
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
 * @detail Terminates ping or neighbor lookup, then force-kills a process that does not
 *         exit promptly.
 */
void MainWindow::stopUdpProcesses()
{
    m_pingTimeoutTimer.stop();
    m_pingInProgress = false;
    m_neighborLookupInProgress = false;

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
 * @detail Uses arp on Windows and macOS or ip neigh on Linux, then parses only numeric
 *         IP and MAC patterns from the output.
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
 * @detail TTL is stable across localized Windows and Unix ping output and avoids
 *         placing localized utility text in EVENTS.
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
 * @detail The method does not modify the interface and is used by normalization and
 *         settings restoration.
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
 * @return Counter size of 1, 2, 4, or 8 bytes.
 * @detail Unexpected GUI text is safely converted to one byte.
 */
quint64 MainWindow::udpCounterBytes() const
{
    const int counterBitsValue =
        ui->udpCounterBitsComboBox->currentText().toInt();
    if (counterBitsValue <= 0 || (counterBitsValue % 8) != 0)
    {
        return 1;
    }

    return static_cast<quint64>(counterBitsValue / 8);
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
    const int counterBitsValue =
        ui->udpCounterBitsComboBox->currentText().toInt();

    if (counterBitsValue >= 64)
    {
        return std::numeric_limits<quint64>::max();
    }

    if (counterBitsValue <= 0)
    {
        return 0;
    }

    return (quint64(1) << counterBitsValue) - 1;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Builds a UDP Pattern description for the event log.
 * @param settings Validated UDP Pattern settings.
 * @return String containing counter, init, block, Togeth, Period, values/packet, and
 *         bo=LE.
 * @detail For hexadecimal input, the decimal equivalent is included as well.
 */
QString MainWindow::udpPatternDescription(
    const UdpPatternSettings &settings) const
{
    const QString initText = ui->udpInitValueLineEdit->text().trimmed();
    const bool hexadecimal =
        initText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive);
    const QString initDescription = hexadecimal
                                        ? tr("%1 (%2)")
                                              .arg(initText)
                                              .arg(settings.initialValue)
                                        : QString::number(settings.initialValue);
    const int valuesPerPacket = settings.blockBytes / settings.counterBytes;

    return tr("Pattern: counter=%1 bits; init=%2; block=%3 bytes; Togeth=%4; period=%5 ms; values/packet=%6; bo=LE")
        .arg(settings.counterBits)
        .arg(initDescription)
        .arg(settings.blockBytes)
        .arg(settings.togetherCount)
        .arg(settings.periodMs)
        .arg(valuesPerPacket);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Adds an event with the current timestamp.
 * @param eventText Event text without a timestamp.
 * @param eventType Event type that controls color and emphasis.
 * @return none
 * @detail Used for GUI actions and errors generated in the main thread.
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
 * @brief Adds an event with a prepared timestamp.
 * @param timestamp Timestamp in HH:MM:SS.mmm format.
 * @param eventText Event text without a timestamp.
 * @param eventType Event type that controls color and emphasis.
 * @return none
 * @detail Preserves the exact worker-event time in the shared EVENTS view and writes
 *         the same line once to the text log.
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

    QTextCursor cursor(ui->eventsPlainTextEdit->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(completeLine + QLatin1Char('\n'), format);
    ui->eventsPlainTextEdit->setTextCursor(cursor);
    ui->eventsPlainTextEdit->ensureCursorVisible();

    writeLogLine(completeLine);
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Writes a prepared line only to the text log.
 * @param line Complete line without a trailing newline.
 * @return none
 * @detail Runs in the GUI thread, appends a newline, and flushes the file immediately.
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
 * @param eventType Type of event to color.
 * @return Black, green, or red QColor for the selected event type.
 * @detail Green is reserved for direct button presses.
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
 * @brief Creates the logs directory and a new log file for this run.
 * @param none
 * @return none
 * @detail Opens a UTF-8 txdatatester_log__date__time.txt file next to the executable.
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
        QStringLiteral("txdatatester_log__%1.txt").arg(timestamp);
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
 * @brief Flushes and closes the text log file.
 * @param none
 * @return none
 * @detail Flushes pending text and detaches QTextStream from QFile.
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
 * @detail Loads window geometry, COM settings, COM Pattern values, UDP destination,
 *         and UDP Pattern values, including legacy-name migration.
 */
void MainWindow::loadSettings()
{
    QSettings currentSettings;
    QSettings legacySettings(QSettings::NativeFormat,
                             QSettings::UserScope,
                             QStringLiteral("TxRxDataTester"),
                             QStringLiteral("TxRxDataTester"));

    QSettings *settings = &currentSettings;
    if (currentSettings.allKeys().isEmpty()
        && !legacySettings.allKeys().isEmpty())
    {
        settings = &legacySettings;
    }

    const QByteArray geometry =
        settings->value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty())
    {
        restoreGeometry(geometry);
    }

    m_preferredPortName =
        settings->value(QStringLiteral("com/port"), QString()).toString();
    selectComboBoxText(
        ui->baudComboBox,
        settings->value(QStringLiteral("com/baud"), QStringLiteral("921600"))
            .toString());
    selectComboBoxText(
        ui->parityComboBox,
        settings->value(QStringLiteral("com/parity"), QStringLiteral("NONE"))
            .toString());
    selectComboBoxText(
        ui->stopsComboBox,
        settings->value(QStringLiteral("com/stops"), QStringLiteral("2"))
            .toString());
    selectComboBoxText(
        ui->counterBitsComboBox,
        settings->value(QStringLiteral("pattern/counterBits"),
                        QStringLiteral("32"))
            .toString());

    ui->blockBytesLineEdit->setText(
        settings->value(QStringLiteral("pattern/blockBytes"),
                        QStringLiteral("128"))
            .toString());
    ui->initValueLineEdit->setText(
        settings->value(QStringLiteral("pattern/initValue"), QStringLiteral("0"))
            .toString());
    ui->periodMsLineEdit->setText(
        settings->value(QStringLiteral("pattern/periodMs"),
                        QStringLiteral("100"))
            .toString());

    ui->udpDestinationIpLineEdit->setText(
        settings->value(QStringLiteral("udp/destinationIp"), QString())
            .toString());
    ui->udpDestinationPortLineEdit->setText(
        settings->value(QStringLiteral("udp/destinationPort"), QString())
            .toString());
    selectComboBoxText(
        ui->udpCounterBitsComboBox,
        settings->value(QStringLiteral("udpPattern/counterBits"),
                        QStringLiteral("32"))
            .toString());
    ui->udpBlockBytesLineEdit->setText(
        settings->value(QStringLiteral("udpPattern/blockBytes"),
                        QStringLiteral("128"))
            .toString());
    ui->udpInitValueLineEdit->setText(
        settings->value(QStringLiteral("udpPattern/initValue"),
                        QStringLiteral("0"))
            .toString());
    ui->udpTogetherLineEdit->setText(
        settings->value(QStringLiteral("udpPattern/together"),
                        QStringLiteral("1"))
            .toString());
    ui->udpPeriodMsLineEdit->setText(
        settings->value(QStringLiteral("udpPattern/periodMs"),
                        QStringLiteral("100"))
            .toString());

    if (settings->status() != QSettings::NoError)
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
 * @detail Normalizes editable fields and stores window geometry, COM settings, COM
 *         Pattern values, UDP destination, and UDP Pattern values.
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
    settings.setValue(QStringLiteral("udpPattern/counterBits"),
                      ui->udpCounterBitsComboBox->currentText());
    settings.setValue(QStringLiteral("udpPattern/blockBytes"),
                      ui->udpBlockBytesLineEdit->text());
    settings.setValue(QStringLiteral("udpPattern/initValue"),
                      ui->udpInitValueLineEdit->text());
    settings.setValue(QStringLiteral("udpPattern/together"),
                      ui->udpTogetherLineEdit->text());
    settings.setValue(QStringLiteral("udpPattern/periodMs"),
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
 * @detail Synchronously shuts down both worker objects, waits for both QThreads,
 *         processes final events, saves settings, and closes the log.
 */
void MainWindow::prepareShutdown()
{
    if (m_shutdownPrepared)
    {
        return;
    }

    m_shutdownPrepared = true;
    m_portRefreshTimer.stop();
    stopUdpProcesses();
    updateControlStates();

    if (m_txWorker != nullptr && m_txThread.isRunning())
    {
        const bool invoked = QMetaObject::invokeMethod(
            m_txWorker,
            "shutdown",
            Qt::BlockingQueuedConnection);
        if (!invoked)
        {
            appendEvent(tr("failed to invoke COM TX worker shutdown"),
                        EventType::Error);
        }

        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        m_txThread.quit();
        m_txThread.wait();
        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        m_txWorker = nullptr;
    }

    if (m_udpTxWorker != nullptr && m_udpTxThread.isRunning())
    {
        const bool invoked = QMetaObject::invokeMethod(
            m_udpTxWorker,
            "shutdown",
            Qt::BlockingQueuedConnection);
        if (!invoked)
        {
            appendEvent(tr("failed to invoke UDP TX worker shutdown"),
                        EventType::Error);
        }

        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        m_udpTxThread.quit();
        m_udpTxThread.wait();
        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        m_udpTxWorker = nullptr;
    }

    m_workerReady = false;
    m_udpWorkerReady = false;
    m_portOpen = false;
    m_testRunning = false;
    m_singleTransferActive = false;
    m_outputDrainActive = false;
    m_portOperationPending = false;
    m_transmissionCommandPending = false;
    m_portLossRequestPending = false;
    m_udpConnected = false;
    m_udpTestRunning = false;
    m_udpSingleTransferActive = false;
    m_udpConnectionOperationPending = false;
    m_udpTransmissionCommandPending = false;
    m_udpDisconnectRequestedByUser = false;

    saveSettings();
    appendEvent(tr("TxDataTester (v.1.7) stopped"),
                EventType::Normal);
    closeLogFile();
}


/*-----------------------------------------------------------------------------*/

/**
 * @brief Repopulates the serial-port combo box.
 * @param ports Current list of available serial ports.
 * @return none
 * @detail Preserves the preferred port when possible and leaves the list empty when no
 *         ports are available.
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
 * @brief Builds a set of serial-port names.
 * @param ports Serial-port information list to process.
 * @return Set containing every non-empty port name.
 * @detail The set is used to detect added and removed ports.
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
 * @brief Builds descriptions indexed by serial-port name.
 * @param ports Serial-port information list to process.
 * @return Hash that maps each port name to its formatted description.
 * @detail Optional description and manufacturer fields are included when available.
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
 * @brief Synchronizes the stored serial-port snapshot without logging.
 * @param ports Current list of available serial ports.
 * @return none
 * @detail Used during initialization and while an open port is monitored.
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
 * @brief Reads and validates Pattern values from the GUI.
 * @param settings Output pointer for validated Pattern settings.
 * @param errorText Output pointer for a validation error message.
 * @return true when all Pattern values are valid; otherwise false.
 * @detail Checks counter width, block alignment, period, and initial-value range.
 */
bool MainWindow::readPatternSettings(PatternSettings *settings,
                                     QString *errorText) const
{
    if (settings == nullptr || errorText == nullptr)
    {
        return false;
    }

    const int counterBitsValue =
        ui->counterBitsComboBox->currentText().toInt();
    if (counterBitsValue != 8
        && counterBitsValue != 16
        && counterBitsValue != 32
        && counterBitsValue != 64)
    {
        *errorText = tr("unsupported counter width");
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
 * @brief Parses the initial counter value.
 * @param value Output pointer for the parsed unsigned value.
 * @return true when decimal or 0x-prefixed input is valid and fits the counter.
 * @detail Does not modify the GUI and can therefore be called from const validation
 *         code.
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
 * @return HH:MM:SS string with an unlimited number of hours.
 * @detail Hours are calculated from the full duration and do not wrap after 23.
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
 * @brief Formats transmission speed.
 * @param speedKbps Speed in decimal kilobits per second.
 * @return String containing at most three digits after the decimal point.
 * @detail Removes insignificant trailing zeros and returns 0 for invalid values.
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
 * @return String containing counter, init, block, period, values/block, and bo=LE.
 * @detail For hexadecimal input, the decimal equivalent is included as well.
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
 * @return Baud/data bits/parity/stops/flow-control description string.
 * @detail The description is sent to TxWorker for open and close events.
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
 * @return Port name with description and manufacturer when available.
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
 * @return Counter size of 1, 2, 4, or 8 bytes.
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
 * @return Maximum unsigned value for the current counter width.
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

    return (quint64(1) << counterBitsValue) - 1;
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Converts the selected parity to QSerialPort format.
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
 * @brief Converts the selected stop-bit count to QSerialPort format.
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
