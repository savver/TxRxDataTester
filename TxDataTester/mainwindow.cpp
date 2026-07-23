#include "mainwindow.h"
#include "txworker.h"
#include "ui_mainwindow.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
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
}

/**
 * @brief Creates the main application window.
 * @param parent Parent widget of the window.
 * @return none
 * @detail Initializes the GUI, validators, settings, event log, and dedicated TX worker
 *         thread.
 */
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_txWorker(nullptr)
    , m_portRefreshTimer(this)
    , m_workerReady(false)
    , m_portOpen(false)
    , m_testRunning(false)
    , m_singleTransferActive(false)
    , m_outputDrainActive(false)
    , m_portOperationPending(false)
    , m_transmissionCommandPending(false)
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
    connect(ui->singleButton,
            &QPushButton::clicked,
            this,
            &MainWindow::handleSingleButton);

    initializeLogFile();
    appendEvent(tr("запуск программы TxDataTester (v.1.4)"), EventType::Normal);

    if (m_logFile.isOpen())
    {
        appendEvent(tr("создан лог-файл: %1")
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
    ui->txBytesLineEdit->setText(QStringLiteral("0"));
    ui->speedLineEdit->setText(QStringLiteral("0"));

    quint64 initialValue = 0;
    parseInitialValue(&initialValue);
    ui->currentCountLineEdit->setText(QString::number(initialValue));

    initializeTxThread();
    refreshSerialPorts();
    updateControlStates();
    m_portRefreshTimer.start();
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Destroys the main application window.
 * @param none
 * @return none
 * @detail Ensures orderly shutdown of the TX thread and log before releasing the Qt
 *         Designer user interface.
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
 * @detail Stops the TX thread, saves settings, closes the log, and then passes the
 *         event to QMainWindow.
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
                tr("открытый порт %1 отключен от системы")
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
            appendEvent(tr("обнаружен новый порт: %1")
                            .arg(currentDescriptions.value(portName,
                                                           portName)),
                        EventType::Normal);
        }

        for (const QString &portName : removedNames)
        {
            appendEvent(tr("порт исчез из списка доступных: %1")
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
        appendEvent(tr("ошибка открытия: не выбран доступный COM-порт"),
                    EventType::Error);
        return;
    }

    bool baudOk = false;
    const qint32 baudRate =
        ui->baudComboBox->currentText().toInt(&baudOk, 10);
    if (!baudOk || baudRate <= 0)
    {
        appendEvent(tr("ошибка открытия: некорректная скорость COM-порта"),
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

    appendEvent(tr("нажата кнопка START"), EventType::Action);

    if (!m_workerReady || !m_portOpen || m_txWorker == nullptr)
    {
        appendEvent(tr("START невозможен: COM-порт не открыт"), EventType::Error);
        return;
    }

    if (m_singleTransferActive || m_outputDrainActive)
    {
        appendEvent(tr("START невозможен: предыдущая передача еще не завершена"),
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
        appendEvent(tr("ошибка Pattern: %1").arg(errorText), EventType::Error);
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

    appendEvent(tr("нажата кнопка SINGLE"), EventType::Action);

    if (!m_workerReady || !m_portOpen || m_txWorker == nullptr)
    {
        appendEvent(tr("SINGLE невозможен: COM-порт не открыт"), EventType::Error);
        return;
    }

    if (m_testRunning || m_singleTransferActive || m_outputDrainActive)
    {
        appendEvent(tr("SINGLE невозможен: предыдущая передача еще не завершена"),
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
        appendEvent(tr("ошибка Pattern: %1").arg(errorText), EventType::Error);
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
        tr("нажата кнопка STOP; формирование новых блоков остановлено; "
           "в очереди %1 байт")
            .arg(qMax<qint64>(0, pendingBytes)),
        EventType::Action);
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
 * @brief Updates availability of all controls.
 * @param none
 * @return none
 * @detail Considers worker readiness, port state, active transmission, output draining,
 *         and pending asynchronous GUI commands.
 */
void MainWindow::updateControlStates()
{
    const bool transferBusy = m_testRunning
                              || m_singleTransferActive
                              || m_outputDrainActive;
    const bool asynchronousBusy = m_portOperationPending
                                  || m_transmissionCommandPending
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
                                && !transferBusy
                                && !asynchronousBusy;
    ui->counterBitsComboBox->setEnabled(patternEnabled);
    ui->blockBytesLineEdit->setEnabled(patternEnabled);
    ui->initValueLineEdit->setEnabled(patternEnabled);
    ui->periodMsLineEdit->setEnabled(patternEnabled);

    ui->startButton->setEnabled(applicationReady
                                && m_portOpen
                                && !transferBusy
                                && !asynchronousBusy);
    ui->stopButton->setEnabled(applicationReady
                               && m_portOpen
                               && m_testRunning
                               && !asynchronousBusy);
    ui->singleButton->setEnabled(applicationReady
                                 && m_portOpen
                                 && !transferBusy
                                 && !asynchronousBusy);
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
 * @detail Preserves the exact worker-event time even when the GUI is busy or minimized.
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
        appendEvent(tr("не удалось создать каталог logs в %1")
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
        appendEvent(tr("не удалось создать лог-файл %1: %2")
                        .arg(QDir::toNativeSeparators(filePath),
                             m_logFile.errorString()),
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
 * @detail Loads window geometry, COM settings, and Pattern values, including migration
 *         from the former TxRxDataTester application name.
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

    if (settings->status() != QSettings::NoError)
    {
        appendEvent(tr("ошибка чтения сохраненных настроек QSettings"),
                    EventType::Error);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Saves the current application settings.
 * @param none
 * @return none
 * @detail Normalizes editable fields and stores window geometry, COM settings, and
 *         Pattern values through QSettings.
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
        appendEvent(tr("ошибка сохранения настроек QSettings"),
                    EventType::Error);
    }
}

/*-----------------------------------------------------------------------------*/

/**
 * @brief Performs one-time application shutdown.
 * @param none
 * @return none
 * @detail Synchronously shuts down TxWorker, waits for QThread, processes final events,
 *         saves settings, and closes the log.
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

    if (m_txWorker != nullptr && m_txThread.isRunning())
    {
        const bool invoked = QMetaObject::invokeMethod(
            m_txWorker,
            "shutdown",
            Qt::BlockingQueuedConnection);
        if (!invoked)
        {
            appendEvent(tr("не удалось вызвать завершение рабочего TX-потока"),
                        EventType::Error);
        }

        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        m_txThread.quit();
        m_txThread.wait();
        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        m_txWorker = nullptr;
    }

    m_workerReady = false;
    m_portOpen = false;
    m_testRunning = false;
    m_singleTransferActive = false;
    m_outputDrainActive = false;
    m_portOperationPending = false;
    m_transmissionCommandPending = false;
    m_portLossRequestPending = false;

    saveSettings();
    appendEvent(tr("завершение программы TxDataTester (v.1.4)"),
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
        *errorText = tr("неподдерживаемая разрядность счетчика");
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
        *errorText = tr("block, bytes должен быть положительным числом");
        return false;
    }

    const quint64 counterBytesValue =
        static_cast<quint64>(counterBitsValue / 8);
    if ((blockBytesValue % counterBytesValue) != 0)
    {
        *errorText = tr("block, bytes не кратен размеру счетчика");
        return false;
    }

    bool periodOk = false;
    const quint64 periodValue =
        ui->periodMsLineEdit->text().toULongLong(&periodOk, 10);
    if (!periodOk
        || periodValue
               > static_cast<quint64>(std::numeric_limits<int>::max()))
    {
        *errorText = tr("Period, ms выходит за допустимый диапазон");
        return false;
    }

    quint64 initialValue = 0;
    if (!parseInitialValue(&initialValue))
    {
        *errorText = tr("init value не соответствует разрядности счетчика");
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
