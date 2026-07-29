#ifndef UDPRXWORKER_H
#define UDPRXWORKER_H

#include <QAbstractSocket>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

class QTimer;
class QUdpSocket;

/**
 * @brief UDP counter-pattern reception worker.
 * @detail Runs in a dedicated high-priority QThread. A fast socket receive stage copies
 *         datagrams into a preallocated packet ring, while a separate bounded processing
 *         stage verifies the little-endian counter and calculates UDP Statistics.
 */
class UdpRxWorker final : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Creates the UDP receiver worker object.
     * @param parent Parent QObject; normally omitted before moveToThread().
     * @return none
     * @detail Initializes scalar state only. QUdpSocket and QTimer are created later by
     *         initialize() after the object is moved to its worker thread.
     */
    explicit UdpRxWorker(QObject *parent = nullptr);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Destroys the UDP receiver worker object.
     * @param none
     * @return none
     * @detail Stops the timer and closes the UDP socket as a safeguard after normal
     *         shutdown.
     */
    ~UdpRxWorker() override;

signals:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports that the UDP RX worker is ready.
     * @param none
     * @return none
     * @detail Emitted after QUdpSocket and the Statistics timer are created in the
     *         worker thread.
     */
    void workerReady();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a UDP event to the GUI and text log.
     * @param timestamp Event timestamp in HH:MM:SS.mmm format.
     * @param text Event text without a timestamp.
     * @param error true for a red error entry; otherwise false.
     * @return none
     * @detail The timestamp is generated in the UDP worker thread at the actual event
     *         time.
     */
    void eventGenerated(const QString &timestamp,
                        const QString &text,
                        bool error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports the persistent UDP receiver-socket state.
     * @param connected true when the socket is bound and ready to receive.
     * @param expectedSourceIp Configured IPv4 address of the expected transmitter.
     * @param listenPort Local UDP port reserved for reception.
     * @param localIp Local IPv4 address to which the socket is bound.
     * @param causedByFailure true when the socket was closed because of an error.
     * @return none
     * @detail The GUI uses this state snapshot without accessing QUdpSocket across
     *         threads.
     */
    void connectionStateChanged(bool connected,
                                const QString &expectedSourceIp,
                                quint16 listenPort,
                                const QString &localIp,
                                bool causedByFailure);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports the active UDP reception state.
     * @param running true between a successful START and completion of STOP.
     * @return none
     * @detail The GUI uses the signal to synchronize UDP START, STOP, and Pattern
     *         controls.
     */
    void receptionStateChanged(bool running);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends a prepared UDP Statistics snapshot to the GUI.
     * @param startTime Reception start time in HH:MM:SS format.
     * @param elapsedMilliseconds Elapsed monotonic test time in milliseconds.
     * @param totalPayloadBytes Total accepted UDP payload bytes.
     * @param currentCounter Last completely decoded counter value.
     * @param counterOk Number of values matching the expected counter.
     * @param counterErrors Number of values not matching the expected counter.
     * @param speedKbps Payload receive speed for the latest interval in Kb/s.
     * @param packetsPerSecond Accepted UDP datagrams per second for the latest interval.
     * @return none
     * @detail Network headers and ignored datagrams from unexpected source addresses are
     *         not included.
     */
    void statisticsUpdated(const QString &startTime,
                           qint64 elapsedMilliseconds,
                           quint64 totalPayloadBytes,
                           quint64 currentCounter,
                           quint64 counterOk,
                           quint64 counterErrors,
                           double speedKbps,
                           double packetsPerSecond);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Sends the additional twenty-second UDP Statistics line.
     * @param line Complete preformatted line with timestamp and values.
     * @return none
     * @detail The GUI writes the line only to the text log and does not display it in
     *         EVENTS.
     */
    void periodicLogLineReady(const QString &line);

public slots:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Initializes resources owned by the UDP RX worker thread.
     * @param none
     * @return none
     * @detail Creates QUdpSocket, receive-pump, packet-processing, and Statistics timers,
     *         allocates the persistent socket buffer, and reports readiness to the GUI.
     */
    void initialize();

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
    void configureConnection(const QString &expectedSourceIp,
                             quint16 listenPort,
                             const QString &localIp);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Closes the persistent UDP receiver socket.
     * @param none
     * @return none
     * @detail Stops active reception when necessary and releases the reserved local UDP
     *         port.
     */
    void disconnectConnection();

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
    void startReception(int counterBits,
                        int blockBytes,
                        int togetherCount,
                        int periodMs,
                        quint64 initialValue,
                        const QString &patternDescription);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Stops UDP reception and counter verification.
     * @param none
     * @return none
     * @detail Captures a final Statistics snapshot and leaves the bound UDP socket ready
     *         for another START operation.
     */
    void stopReception();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Shuts down the UDP RX worker before application exit.
     * @param none
     * @return none
     * @detail Stops active reception, closes the socket and timer, and leaves the object
     *         ready for QThread::quit().
     */
    void shutdown();

private slots:
/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles a real QUdpSocket readyRead notification.
     * @param none
     * @return none
     * @detail Drains a time-bounded batch only when a datagram is actually pending.
     *         Empty or stale notifications are counted and recovered by the native-data
     *         receive pump after a later datagram arrives.
     */
    void handleReadyRead();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Continues draining a large UDP receive backlog.
     * @param none
     * @return none
     * @detail Runs through a queued callback that is separate from the real readyRead
     *         handler and reads only datagrams confirmed as pending.
     */
    void continueReadBatch();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Pumps pending datagrams independently of readyRead notifications.
     * @param none
     * @return none
     * @detail Runs every millisecond, checks both Qt and native socket state, and drains a
     *         bounded batch whenever data is queued. This keeps reception alive even after
     *         Qt 5.12/Windows stops delivering readyRead notifications.
     */
    void serviceReceivePump();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Processes a bounded batch from the preallocated packet ring.
     * @param none
     * @return none
     * @detail Counter verification is separated from socket draining so the operating-system
     *         receive queue can be emptied quickly during dense packet bursts.
     */
    void processPacketQueue();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Performs the one-second UDP Statistics update.
     * @param none
     * @return none
     * @detail Calculates payload speed and packet rate from the actual elapsed interval
     *         and updates twenty-second sample statistics.
     */
    void updateStatistics();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles an asynchronous UDP socket error.
     * @param error QAbstractSocket error reported by QUdpSocket.
     * @return none
     * @detail Temporary errors are logged without closing the socket; fatal errors stop
     *         reception and release the connection.
     */
    void handleSocketError(QAbstractSocket::SocketError error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles an unexpected UDP socket state transition.
     * @param state New QAbstractSocket state.
     * @return none
     * @detail Detects an unrequested close of a logically connected receiver socket.
     */
    void handleSocketStateChanged(QAbstractSocket::SocketState state);

private:
    /**
     * @brief Identifies why a receive batch was started.
     * @detail ReadyRead is a real Qt notification, Continuation is a queued socket-drain
     *         callback, and ReceivePump is the periodic notification-independent fallback.
     */
    enum class ReadTrigger
    {
        ReadyRead,
        Continuation,
        ReceivePump
    };

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Result of one time-bounded receive batch.
     * @detail Reports how much work was completed and whether a queued continuation is
     *         needed without exposing QUdpSocket to the GUI thread.
     */
    struct ReadBatchResult
    {
        int datagramsRead = 0;
        int datagramsQueued = 0;
        int datagramsProcessedDirectly = 0;
        bool temporaryEmpty = false;
        bool budgetReached = false;
        bool fatalError = false;
    };

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Validated settings of the active UDP pattern.
     * @detail Counter verification depends on counterBytes and initialValue. blockBytes,
     *         togetherCount, and periodMs are retained for validation and the log.
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
     * @param blockBytes Expected UDP payload size in bytes.
     * @param togetherCount Informational packets-per-burst value.
     * @param periodMs Informational burst period in milliseconds.
     * @param initialValue First expected counter value.
     * @param settings Output pointer for validated settings.
     * @param errorText Output pointer for a validation error message.
     * @return true when all parameters are valid; otherwise false.
     * @detail Checks supported widths, IPv4 UDP payload limits, alignment, positive
     *         Togeth, Period range, and initial-value range.
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
     * @brief Resets UDP Statistics and verification state before START.
     * @param settings Validated settings for the new test.
     * @return none
     * @detail Sets expected to init, clears totals and interval samples, starts monotonic
     *         timing, and emits the initial zero-valued snapshot.
     */
    void resetStatistics(const PatternSettings &settings);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Drains one time-bounded batch from the UDP socket.
     * @param trigger Reason why the batch was started.
     * @return ReadBatchResult describing the completed batch.
     * @detail Uses one persistent maximum-size socket buffer, copies accepted packets into
     *         the preallocated ring, and yields when the receive time budget expires.
     */
    ReadBatchResult drainPendingDatagrams(ReadTrigger trigger);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Schedules one queued backlog-drain callback.
     * @param none
     * @return none
     * @detail Prevents duplicate queued continuations and reports an invocation failure
     *         without stopping the bound receiver socket.
     */
    void scheduleReadContinuation();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Discards UDP datagrams currently queued before START.
     * @param none
     * @return Number of datagrams removed from the receive queue.
     * @detail Reuses the persistent receive buffer and applies both a packet limit and a
     *         monotonic time budget under a continuously arriving stream.
     */
    quint64 discardPendingDatagrams();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Allocates the preallocated packet ring for a new START operation.
     * @param expectedPayloadBytes Payload size configured in Pattern.
     * @param errorText Output pointer for an allocation or size error.
     * @return true when the packet ring is ready; otherwise false.
     * @detail Uses a bounded memory budget, fixed-size slots, and no per-datagram heap
     *         allocation in the high-rate receive path.
     */
    bool configurePacketQueue(int expectedPayloadBytes, QString *errorText);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Clears packet-ring indices without releasing preallocated storage.
     * @param none
     * @return none
     * @detail Stops pending processing and resets head, tail, and depth for a new test.
     */
    void clearPacketQueue();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Copies one received payload into the packet ring.
     * @param data Pointer to the received payload.
     * @param payloadBytes Number of valid payload bytes.
     * @param senderPort Source UDP port retained for diagnostic messages.
     * @return true when the payload was queued; otherwise false.
     * @detail If the ring is full, the oldest packet is processed synchronously to preserve
     *         sequence order and make one slot available without dropping the new packet.
     */
    bool enqueueDatagram(const char *data, int payloadBytes, quint16 senderPort);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Schedules packet-ring processing through a zero-delay timer.
     * @param none
     * @return none
     * @detail Avoids duplicate scheduling while allowing socket receive callbacks to return
     *         quickly to the worker event loop.
     */
    void schedulePacketProcessing();

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
    int processQueuedDatagrams(qint64 timeBudgetNs,
                               int maximumDatagrams,
                               bool drainCompletely);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Adds one packet-verification work item to processing diagnostics.
     * @param elapsedNs Time spent in the processing work item in nanoseconds.
     * @param processedDatagrams Number of packets verified by the work item.
     * @return none
     * @detail Accounts both scheduled processing batches and inline FIFO-preserving work
     *         performed when the packet ring is full or an oversized payload is received.
     */
    void recordProcessingWork(qint64 elapsedNs, int processedDatagrams);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Processes and removes one oldest packet from the ring.
     * @param none
     * @return true when one packet was processed; otherwise false.
     * @detail The packet payload remains in preallocated storage and is never copied into a
     *         temporary QByteArray.
     */
    bool processOneQueuedDatagram();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Reports sustained pressure on the internal packet ring.
     * @param none
     * @return none
     * @detail Rate-limits service messages to avoid adding GUI or disk load during a dense
     *         UDP stream.
     */
    void reportQueuePressure();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Processes one accepted UDP payload.
     * @param data Pointer to the first payload byte in the persistent receive buffer.
     * @param payloadBytes Number of valid payload bytes.
     * @param senderAddress Source IPv4 address.
     * @param senderPort Source UDP port.
     * @return none
     * @detail Decodes complete little-endian counter fields without allocating or copying
     *         a QByteArray for each packet.
     */
    void processDatagram(const char *data,
                         int payloadBytes,
                         const QHostAddress &senderAddress,
                         quint16 senderPort);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Queries the number of bytes waiting in the native socket receive queue.
     * @param succeeded Output flag set true when the native query completed successfully.
     * @param nativeError Output operating-system error code when the query fails.
     * @return Pending native bytes, or -1 when the query is unavailable or fails.
     * @detail Uses FIONREAD on Windows and Unix-like systems only inside the UDP worker
     *         thread that owns the socket descriptor.
     */
    qint64 nativePendingBytes(bool *succeeded, int *nativeError) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the most recent native socket error for the current thread.
     * @param none
     * @return WSA error on Windows or errno on Unix-like systems.
     * @detail Must be called immediately after a failed socket operation before another
     *         operating-system call can overwrite the thread-local error value.
     */
    int lastNativeSocketError() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Classifies a failed nonblocking UDP read as temporary.
     * @param socketError Error reported by QUdpSocket.
     * @param nativeError Native WSA or errno value captured immediately after failure.
     * @return true for would-block, interrupted, or timeout conditions; otherwise false.
     * @detail Qt 5.12 on Windows may report an empty nonblocking UDP read as NetworkError,
     *         so the native error code is checked in addition to the Qt error value.
     */
    bool isTemporaryReadFailure(QAbstractSocket::SocketError socketError,
                                int nativeError) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Resets per-test UDP read-path diagnostics.
     * @param none
     * @return none
     * @detail Clears readyRead, receive-pump, queue, processing-time, batch-size, and
     *         read-error counters immediately before a new START.
     */
    void resetReadDiagnostics();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Formats current UDP read-path diagnostics.
     * @param none
     * @return Semicolon-separated diagnostic fields for service logs.
     * @detail Includes notification, receive-pump, queue-depth, processing-load, and
     *         maximum-batch counters without changing receiver state.
     */
    QString readDiagnosticsText() const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Decodes one little-endian counter value.
     * @param data Pointer to the first byte of the value.
     * @param byteCount Value size from one through eight bytes.
     * @return The received counter represented as an unsigned 64-bit value.
     * @detail Performs byte-by-byte conversion independently of host byte order and
     *         alignment.
     */
    quint64 decodeLittleEndianCounter(const char *data, int byteCount) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns the next counter value with wraparound.
     * @param value Current counter value.
     * @return value + 1, or zero after the maximum selected counter value.
     * @detail Uses the precomputed maximumCounterValue for 8 through 64 bits.
     */
    quint64 nextCounterValue(quint64 value) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Formats the shortest modular delta between expected and received values.
     * @param expectedCounter Expected counter value.
     * @param receivedCounter Received counter value.
     * @return Decimal delta text, for example 30, -2, or 1 across wraparound.
     * @detail Unsigned subtraction is used modulo the selected counter width, and the
     *         shorter forward or backward distance is reported.
     */
    QString counterDeltaText(quint64 expectedCounter,
                             quint64 receivedCounter) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Updates and emits the current UDP Statistics snapshot.
     * @param includePeriodicSample true only for the one-second timer tick.
     * @return none
     * @detail Calculates rates from byte and packet deltas over the exact monotonic
     *         interval and optionally accumulates one sample for the twenty-second log.
     */
    void updateStatisticsSnapshot(bool includePeriodicSample);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Creates the twenty-second UDP Statistics line when due.
     * @param none
     * @return none
     * @detail Uses total and delta byte and counter values plus minimum, arithmetic mean,
     *         and maximum of the one-second packet-rate and speed samples.
     */
    void emitPeriodicLogLineIfDue();

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Internally finalizes an active UDP reception test.
     * @param reasonText Optional stop reason; an empty string is not logged.
     * @param reasonIsError true for a red reason entry; false for a black service entry.
     * @return none
     * @detail Creates the final snapshot, stops the timer, clears per-test source-report
     *         state, and emits receptionStateChanged(false).
     */
    void stopReceptionInternal(const QString &reasonText,
                               bool reasonIsError);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Handles a fatal UDP socket failure.
     * @param reason Complete reason text for the red event-log entry.
     * @return none
     * @detail Prevents recursive handling, stops an active test, closes the socket, and
     *         reports a failed connection state to the GUI.
     */
    void handleSocketFailure(const QString &reason);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Closes the UDP receiver socket and emits a disconnected state.
     * @param causedByFailure true when closure was caused by an error.
     * @param emitEvent true to emit a normal socket-closed service event.
     * @return none
     * @detail Copies endpoint information before clearing internal connection state.
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
     * @brief Formats a nonnegative floating-point rate for logs.
     * @param value Speed or packet-rate value.
     * @return String containing at most three digits after the decimal point.
     * @detail Removes insignificant trailing zeros and returns zero for invalid values.
     */
    QString formatRate(double value) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Returns a fixed English description for a UDP socket error.
     * @param socketError Numeric QAbstractSocket error value.
     * @return English text independent of the operating-system language.
     * @detail Maps Qt 5.12 socket errors used by UDP reception.
     */
    QString socketErrorText(int socketError) const;

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Emits a normal or error event with an exact timestamp.
     * @param text Event text without a timestamp.
     * @param error true for an error; false for a normal entry.
     * @return none
     * @detail Creates the HH:MM:SS.mmm timestamp in the UDP worker thread before queued
     *         delivery to the GUI.
     */
    void emitWorkerEvent(const QString &text, bool error);

/*-----------------------------------------------------------------------------*/

    /**
     * @brief Emits the current UDP reception state.
     * @param none
     * @return none
     * @detail Centralizes delivery of m_testRunning to the GUI.
     */
    void emitReceptionState();

    QUdpSocket *m_udpSocket;
    QTimer *m_statisticsTimer;
    QTimer *m_receivePumpTimer;
    QTimer *m_packetProcessingTimer;
    QElapsedTimer m_elapsedTimer;
    QElapsedTimer m_workerUptimeTimer;
    QByteArray m_receiveBuffer;
    QByteArray m_packetQueueStorage;
    QVector<quint32> m_packetQueueLengths;
    QVector<quint16> m_packetQueueSenderPorts;
    PatternSettings m_activePattern;
    QHostAddress m_expectedSourceAddress;
    QString m_expectedSourceIp;
    QString m_localIp;
    QString m_startTime;
    QSet<QString> m_reportedUnexpectedSources;
    quint16 m_listenPort;
    quint64 m_expectedCounter;
    quint64 m_lastReceivedCounter;
    quint64 m_totalPayloadBytes;
    quint64 m_totalPackets;
    quint64 m_lastStatisticsBytes;
    quint64 m_lastStatisticsPackets;
    qint64 m_lastStatisticsElapsedMs;
    quint64 m_counterOk;
    quint64 m_counterErrors;
    quint64 m_lastPeriodicLogBytes;
    quint64 m_lastPeriodicCounterOk;
    quint64 m_lastPeriodicCounterErrors;
    qint64 m_lastPeriodicLogElapsedMs;
    double m_periodicMinimumSpeedKbps;
    double m_periodicMaximumSpeedKbps;
    double m_periodicSpeedSumKbps;
    double m_periodicMinimumPacketsPerSecond;
    double m_periodicMaximumPacketsPerSecond;
    double m_periodicPacketsPerSecondSum;
    quint64 m_periodicSampleCount;
    quint64 m_readyReadCalls;
    quint64 m_emptyReadyReadCalls;
    quint64 m_continuationCallbacks;
    quint64 m_receivePumpCalls;
    quint64 m_receivePumpRecoveries;
    quint64 m_suppressedReceivePumpEvents;
    quint64 m_readErrors;
    quint64 m_datagramsReadByWorker;
    quint64 m_datagramsProcessed;
    quint64 m_processingCallbacks;
    quint64 m_queuePressureEvents;
    quint64 m_queueDroppedDatagrams;
    quint64 m_oversizeDirectDatagrams;
    quint64 m_totalSocketReadTimeNs;
    quint64 m_totalProcessingTimeNs;
    quint64 m_lastStatisticsProcessingTimeNs;
    int m_maximumReadBatch;
    int m_maximumProcessingBatch;
    qint64 m_maximumSocketReadBatchNs;
    qint64 m_maximumProcessingBatchNs;
    qint64 m_lastProcessingLoadPercentTimes1000;
    qint64 m_lastReadyReadMs;
    qint64 m_lastDatagramReadMs;
    qint64 m_lastReceivePumpEventMs;
    qint64 m_lastQueuePressureEventMs;
    int m_packetQueueSlotBytes;
    int m_packetQueueCapacity;
    int m_packetQueueHead;
    int m_packetQueueTail;
    int m_packetQueueDepth;
    int m_maximumPacketQueueDepth;
    int m_actualReceiveBufferBytes;
    bool m_initialized;
    bool m_connectionConfigured;
    bool m_testRunning;
    bool m_readContinuationScheduled;
    bool m_packetProcessingScheduled;
    bool m_receivePumpFallbackActive;
    bool m_readDrainInProgress;
    bool m_packetProcessingInProgress;
    bool m_readRequestedDuringDrain;
    bool m_readAttemptInProgress;
    bool m_nativeQueryFailureReported;
    bool m_socketOperationInProgress;
    bool m_handlingSocketFailure;
    bool m_shuttingDown;
};

#endif // UDPRXWORKER_H
