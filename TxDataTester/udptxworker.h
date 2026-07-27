#ifndef UDPTXWORKER_H
#define UDPTXWORKER_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QString>

class QTimer;
class QUdpSocket;

/**
 * @brief UDP test-pattern transmission worker.
 * @detail Runs in a dedicated QThread and owns QUdpSocket, transmission timers,
 *         little-endian pattern generation, and UDP Statistics calculations.
 */
class UdpTxWorker final : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Creates the UDP transmitter worker object.
     * @param parent Parent QObject; normally omitted before moveToThread().
     * @return none
     * @detail Initializes scalar state only; QUdpSocket and timers are created later by
     *         initialize() in the worker thread.
     */
    explicit UdpTxWorker(QObject *parent = nullptr);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Destroys the UDP transmitter worker object.
     * @param none
     * @return none
     * @detail Stops timers and closes the UDP socket as a safeguard after normal
     *         shutdown.
     */
    ~UdpTxWorker() override;

signals:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports that the UDP TX worker is ready.
     * @param none
     * @return none
     * @detail Emitted after QUdpSocket and all worker timers are created in the worker
     *         thread.
     */
    void workerReady();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a UDP event to the GUI and text log.
     * @param timestamp Event timestamp in HH:MM:SS.mmm format.
     * @param text Event text without a timestamp.
     * @param error true for a red error entry; otherwise false.
     * @return none
     * @detail The timestamp is generated in the UDP worker thread at the moment of the
     *         event.
     */
    void eventGenerated(const QString &timestamp,
                        const QString &text,
                        bool error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports the persistent UDP socket state.
     * @param connected true when the local UDP socket is ready for transmission.
     * @param destinationIp Configured destination IPv4 address.
     * @param destinationPort Configured destination UDP port.
     * @param localIp Bound local IPv4 address.
     * @param localPort Ephemeral local UDP port reserved by the socket.
     * @param causedByFailure true when the socket was closed because of an error.
     * @return none
     * @detail The GUI uses this snapshot to lock tabs and destination fields without
     *         accessing QUdpSocket across threads.
     */
    void connectionStateChanged(bool connected,
                                const QString &destinationIp,
                                quint16 destinationPort,
                                const QString &localIp,
                                quint16 localPort,
                                bool causedByFailure);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports current UDP transmission-operation states.
     * @param testRunning true during continuous burst generation.
     * @param singleTransferActive true while a SINGLE datagram is being processed.
     * @return none
     * @detail The GUI uses these flags to enable or disable UDP controls.
     */
    void transmissionStateChanged(bool testRunning,
                                  bool singleTransferActive);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a prepared UDP Statistics snapshot to the GUI.
     * @param startTime Transmission start time in HH:MM:SS format.
     * @param elapsedMilliseconds Elapsed monotonic time in milliseconds.
     * @param totalPayloadBytes Total payload bytes accepted by writeDatagram().
     * @param currentCounter Next counter value for a new payload field.
     * @param speedKbps Payload speed for the latest interval in Kb/s.
     * @param packetsPerSecond Datagrams accepted per second for the latest interval.
     * @return none
     * @detail Network headers are not included in byte or speed calculations.
     */
    void statisticsUpdated(const QString &startTime,
                           qint64 elapsedMilliseconds,
                           quint64 totalPayloadBytes,
                           quint64 currentCounter,
                           double speedKbps,
                           double packetsPerSecond);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends the twenty-second UDP Statistics line to the text log.
     * @param line Fully formatted log line with timestamp and Statistics values.
     * @return none
     * @detail The GUI writes the line to QFile without displaying it in EVENTS.
     */
    void periodicLogLineReady(const QString &line);

public slots:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Initializes resources owned by the UDP worker thread.
     * @param none
     * @return none
     * @detail Creates QUdpSocket and timers with UdpTxWorker as parent, connects their
     *         signals, and reports readiness to the GUI.
     */
    void initialize();

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
    void configureConnection(const QString &destinationIp,
                             quint16 destinationPort,
                             const QString &localIp);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Closes the persistent UDP sending socket.
     * @param none
     * @return none
     * @detail Requires UDP transmission to be idle and releases the ephemeral local
     *         port reserved during CONNECT.
     */
    void disconnectConnection();

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
    void startContinuous(int counterBits,
                         int blockBytes,
                         int togetherCount,
                         int periodMs,
                         quint64 initialValue,
                         const QString &patternDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Stops continuous UDP packet generation.
     * @param none
     * @return none
     * @detail Stops the burst timer immediately; UDP provides no portable physical-NIC
     *         completion acknowledgement for already accepted datagrams.
     */
    void stopContinuous();

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
     * @detail SINGLE sends exactly one datagram; Togeth applies only to continuous
     *         START bursts.
     */
    void sendSingle(int counterBits,
                    int blockBytes,
                    int togetherCount,
                    int periodMs,
                    quint64 initialValue,
                    const QString &patternDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Shuts down the UDP worker before application exit.
     * @param none
     * @return none
     * @detail Stops timers, closes the socket, and leaves the worker ready for
     *         QThread::quit().
     */
    void shutdown();

private slots:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Generates and sends the next continuous UDP burst.
     * @param none
     * @return none
     * @detail Sends Togeth datagrams sequentially in one worker-thread callback and
     *         reschedules the cooperative zero-period mode when required.
     */
    void sendNextBurst();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Performs the one-second UDP Statistics update.
     * @param none
     * @return none
     * @detail Calculates payload speed and packets per second from real elapsed time
     *         and emits a prepared snapshot.
     */
    void updateStatistics();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles an asynchronous UDP socket error.
     * @param socketError Numeric QAbstractSocket::SocketError value.
     * @return none
     * @detail Stops active generation and closes the logical UDP connection for fatal
     *         errors not already handled by a synchronous socket call.
     */
    void handleSocketError(int socketError);

private:
    /**
     * @brief Validated UDP Pattern settings.
     * @detail Stores all scalar values required for payload generation in the worker
     *         thread.
     */
    struct PatternSettings
    {
        int counterBits = 8;
        int counterBytes = 1;
        int blockBytes = 1;
        int togetherCount = 1;
        int periodMs = 0;
        quint64 initialValue = 0;
        quint64 maximumCounterValue = 0xFFU;
    };

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
    bool makePatternSettings(int counterBits,
                             int blockBytes,
                             int togetherCount,
                             int periodMs,
                             quint64 initialValue,
                             PatternSettings *settings,
                             QString *errorText) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Resets UDP Statistics for a new START or SINGLE operation.
     * @param settings Validated Pattern settings.
     * @return none
     * @detail Initializes the counter, timestamps, byte and packet baselines, and
     *         twenty-second extrema.
     */
    void resetStatistics(const PatternSettings &settings);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Updates and emits the current UDP Statistics snapshot.
     * @param includePeriodicSample true to include the sample in 20-second extrema.
     * @return none
     * @detail Uses monotonic elapsed time and saturating totals to calculate the latest
     *         payload speed and packet rate.
     */
    void updateStatisticsSnapshot(bool includePeriodicSample);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Emits a twenty-second UDP Statistics line when due.
     * @param none
     * @return none
     * @detail Uses exact elapsed time, payload-byte and packet deltas, and the minimum
     *         and maximum one-second samples accumulated during the interval.
     */
    void emitPeriodicLogLineIfDue();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends one complete burst of UDP datagrams.
     * @param settings Active validated Pattern settings.
     * @return true when every datagram in the burst was accepted by the socket.
     * @detail Counter progression is committed only after each complete datagram is
     *         accepted by writeDatagram().
     */
    bool sendBurst(const PatternSettings &settings);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds and sends one UDP datagram.
     * @param settings Active validated Pattern settings.
     * @return true when writeDatagram() accepted the complete payload.
     * @detail Counts only application payload bytes and advances the counter only after
     *         a successful complete-datagram result.
     */
    bool sendOneDatagram(const PatternSettings &settings);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Builds one little-endian counter-pattern payload.
     * @param settings Active validated Pattern settings.
     * @param startCounter First counter value to place into the payload.
     * @param nextCounter Output next counter value after the payload.
     * @return QByteArray containing exactly blockBytes bytes.
     * @detail Counter values wrap to zero after the selected unsigned maximum.
     */
    QByteArray buildDatagram(const PatternSettings &settings,
                             quint64 startCounter,
                             quint64 *nextCounter) const;

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
    void stopTransmissionInternal(const QString &eventText,
                                  bool error,
                                  bool closeConnection);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Closes the UDP socket and emits a disconnected state.
     * @param causedByFailure true when closure was caused by a network error.
     * @param emitEvent true to emit a socket-closed service event.
     * @return none
     * @detail Clears destination state after copying values needed by the state signal.
     */
    void closeSocket(bool causedByFailure, bool emitEvent);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Formats elapsed test time.
     * @param elapsedMilliseconds Elapsed duration in milliseconds.
     * @return HH:MM:SS string with an unlimited number of hours.
     * @detail Hours are calculated from the full duration and do not wrap after 23.
     */
    QString formatElapsedTime(qint64 elapsedMilliseconds) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Formats a floating-point rate for logs.
     * @param value Nonnegative speed or packet-rate value.
     * @return String containing at most three digits after the decimal point.
     * @detail Removes insignificant trailing zeros and returns 0 for invalid values.
     */
    QString formatRate(double value) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns a fixed English description for a socket error.
     * @param socketError Numeric QAbstractSocket::SocketError value.
     * @return English text independent of the operating-system language.
     * @detail Maps the Qt 5.12 socket-error values used by UDP transmission.
     */
    QString socketErrorText(int socketError) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Emits a normal or error event with an exact timestamp.
     * @param text Event text without a timestamp.
     * @param error true for an error; false for a normal entry.
     * @return none
     * @detail Creates the HH:MM:SS.mmm timestamp inside the UDP worker thread before
     *         queued delivery to the GUI.
     */
    void emitWorkerEvent(const QString &text, bool error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Emits the current UDP transmission states.
     * @param none
     * @return none
     * @detail Sends Boolean state only and never exposes QUdpSocket or QTimer objects
     *         across threads.
     */
    void emitTransmissionState();

    QUdpSocket *m_udpSocket;
    QTimer *m_transmitTimer;
    QTimer *m_statisticsTimer;
    QElapsedTimer m_elapsedTimer;
    PatternSettings m_activePattern;
    QHostAddress m_destinationAddress;
    QString m_destinationIp;
    QString m_localIp;
    QString m_startTime;
    quint16 m_destinationPort;
    quint16 m_localPort;
    quint64 m_currentCounter;
    quint64 m_totalPayloadBytes;
    quint64 m_totalPackets;
    quint64 m_lastStatisticsBytes;
    quint64 m_lastStatisticsPackets;
    qint64 m_lastStatisticsElapsedMs;
    quint64 m_lastPeriodicLogBytes;
    quint64 m_lastPeriodicLogPackets;
    qint64 m_lastPeriodicLogElapsedMs;
    double m_periodicMinimumSpeedKbps;
    double m_periodicMaximumSpeedKbps;
    double m_periodicMinimumPacketsPerSecond;
    double m_periodicMaximumPacketsPerSecond;
    bool m_initialized;
    bool m_connectionConfigured;
    bool m_testRunning;
    bool m_singleTransferActive;
    bool m_collectStatistics;
    bool m_periodicLogStatisticsActive;
    bool m_periodicSampleAvailable;
    bool m_socketOperationInProgress;
    bool m_shuttingDown;
};

#endif // UDPTXWORKER_H
