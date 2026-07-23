#include "mainwindow.h"
#include "rxworker.h"
#include "ui_mainwindow.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFileDevice>
#include <QFont>
#include <QFontDatabase>
#include <QIODevice>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QSignalBlocker>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>

#include <limits>

namespace
{
constexpr int kPortRefreshIntervalMs = 1000;

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
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Creates the main application window.
 * @param parent Parent widget of the window.
 * @return none
 * @detail Configures the GUI, validators, event log, settings persistence, and the
 *         dedicated QThread that hosts RxWorker and all reception logic.
 */
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_rxWorker(nullptr)
    , m_portRefreshTimer(this)
    , m_workerReady(false)
    , m_portOpen(false)
    , m_testRunning(false)
    , m_portOperationPending(false)
    , m_receptionCommandPending(false)
    , m_portLossRequestPending(false)
    , m_portSnapshotInitialized(false)
    , m_shutdownPrepared(false)
{
    ui->setupUi(this);

    ui->settingsGridLayout->setColumnStretch(1, 1);
    ui->settingsGridLayout->setColumnStretch(4, 1);
    ui->patternGridLayout->setColumnStretch(1, 1);
    ui->patternGridLayout->setColumnStretch(4, 1);
    ui->statisticsGridLayout->setColumnStretch(1, 1);
    ui->statisticsGridLayout->setColumnStretch(4, 1);

    const QRegularExpression decimalExpression(QStringLiteral("[0-9]{0,10}"));
    ui->blockBytesLineEdit->setValidator(
        new QRegularExpressionValidator(decimalExpression, this));
    ui->periodMsLineEdit->setValidator(
        new QRegularExpressionValidator(decimalExpression, this));

    const QRegularExpression initialValueExpression(
        QStringLiteral("(?:0[xX][0-9A-Fa-f]{0,16}|[0-9]{0,20})"));
    ui->initValueLineEdit->setValidator(
        new QRegularExpressionValidator(initialValueExpression, this));

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

    initializeLogFile();
    appendEvent(tr("RxDataTester (v.1.1) started"), EventType::Normal);

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

    ui->startTimeValueLabel->setText(QStringLiteral("--:--:--"));
    ui->elapsedTimeValueLabel->setText(QStringLiteral("00:00:00"));
    ui->rxBytesLineEdit->setText(QStringLiteral("0"));
    ui->speedLineEdit->setText(QStringLiteral("0"));
    ui->counterOkLineEdit->setText(QStringLiteral("0"));
    ui->counterErrLineEdit->setText(QStringLiteral("0"));

    quint64 initialValue = 0;
    parseInitialValue(&initialValue);
    ui->currentCountLineEdit->setText(QString::number(initialValue));

    initializeRxThread();
    refreshSerialPorts();
    updateControlStates();
    m_portRefreshTimer.start();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Destroys the main application window.
 * @param none
 * @return none
 * @detail Ensures orderly shutdown of the RX thread, closes the log file, and releases
 *         the Qt Designer user interface.
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
 * @detail Synchronously stops the RX thread, saves the settings, closes the log, and
 *         then passes the event to the base QMainWindow implementation.
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
 * @brief Receives a normal or error event from RxWorker.
 * @param timestamp Timestamp created in the worker thread.
 * @param text Event text without a timestamp.
 * @param error true for a red error entry; otherwise false.
 * @return none
 * @detail Displays the event in EVENTS and duplicates it to the log in the GUI thread.
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
 * @brief Updates enabled states of the controls.
 * @param none
 * @return none
 * @detail Accounts for worker readiness, port state, active reception, and pending
 *         asynchronous GUI commands.
 */
void MainWindow::updateControlStates()
{
    const bool asynchronousBusy = m_portOperationPending
                                  || m_receptionCommandPending
                                  || m_portLossRequestPending;
    const bool applicationReady = m_workerReady && !m_shutdownPrepared;

    ui->openButton->setEnabled(applicationReady
                               && !m_portOpen
                               && !asynchronousBusy
                               && ui->portComboBox->count() > 0);
    ui->closeButton->setEnabled(applicationReady
                                && m_portOpen
                                && !m_portOperationPending
                                && !m_portLossRequestPending);

    const bool portSettingsEnabled = applicationReady
                                     && !m_portOpen
                                     && !asynchronousBusy;
    ui->portComboBox->setEnabled(portSettingsEnabled);
    ui->baudComboBox->setEnabled(portSettingsEnabled);
    ui->parityComboBox->setEnabled(portSettingsEnabled);
    ui->stopsComboBox->setEnabled(portSettingsEnabled);

    const bool patternEnabled = applicationReady
                                && !m_testRunning
                                && !asynchronousBusy;
    ui->counterBitsComboBox->setEnabled(patternEnabled);
    ui->blockBytesLineEdit->setEnabled(patternEnabled);
    ui->initValueLineEdit->setEnabled(patternEnabled);
    ui->periodMsLineEdit->setEnabled(patternEnabled);

    ui->startButton->setEnabled(applicationReady
                                && m_portOpen
                                && !m_testRunning
                                && !asynchronousBusy);
    ui->stopButton->setEnabled(applicationReady
                               && m_portOpen
                               && m_testRunning
                               && !asynchronousBusy);
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
 * @detail Invokes RxWorker shutdown through a blocking queued call, waits for QThread,
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
    updateControlStates();

    if (m_rxWorker != nullptr && m_rxThread.isRunning())
    {
        const bool invoked = QMetaObject::invokeMethod(
            m_rxWorker,
            "shutdown",
            Qt::BlockingQueuedConnection);
        if (!invoked)
        {
            appendEvent(tr("failed to invoke RX worker shutdown"),
                        EventType::Error);
        }

        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        m_rxThread.quit();
        m_rxThread.wait();
        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        m_rxWorker = nullptr;
    }

    m_workerReady = false;
    m_portOpen = false;
    m_testRunning = false;
    m_portOperationPending = false;
    m_receptionCommandPending = false;
    m_portLossRequestPending = false;

    saveSettings();
    appendEvent(tr("RxDataTester (v.1.1) stopped"),
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
