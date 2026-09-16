#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <atomic>
#include <cstdint>

class QThread;
class QTcpServer;
class QTcpSocket;
class QTimer;

namespace gbtts {

class TtsAudio;

// Loopback link to the backend process (protocol: backend/gbtts/protocol.py).
//
// All socket work happens on a dedicated thread so the TeamSpeak GUI thread
// never parses PCM and a busy client UI can never starve the audio. PCM goes
// straight into TtsAudio from that thread; when its rings are full the link
// stops reading, the kernel socket buffer fills and the backend's sendall()
// blocks - memory stays bounded end to end.
//
// Every authenticated connection gets a generation number carried by all its
// signals. The GUI side drops anything from an older generation, so the late
// "disconnected" of a backend that was deliberately restarted can never be
// mistaken for a crash of its successor.
class LinkWorker : public QObject
{
    Q_OBJECT
public:
    LinkWorker(TtsAudio *audio, QMutex *localMutex, QSet<uint32_t> *localJobs);

    Q_INVOKABLE quint16 listen(const QString &token);
    Q_INVOKABLE void    closeAll();
    Q_INVOKABLE void    dropPeer();
    void writeFrame(const QByteArray &frame);

signals:
    void connected(quint64 generation);
    void disconnected(quint64 generation);
    void message(quint64 generation, const QJsonObject &obj);
    void protocolError(const QString &what);

private:
    void onNewConnection();
    void onReadyRead();
    void onSocketGone();
    void retryPending();
    void processBuffer();
    bool isLocal(uint32_t job);
    void resetPeerState();
    void pumpMic();

    TtsAudio       *m_audio;
    QMutex         *m_localMutex;
    QSet<uint32_t> *m_localJobs;
    QTcpServer     *m_server = nullptr;
    QTcpSocket     *m_sock = nullptr;
    QTimer         *m_retry = nullptr;
    QTimer         *m_micTimer = nullptr;   // voice changer uplink pump (link thread)
    quint32         m_micSeq = 0;
    QString         m_token;
    bool            m_authed = false;
    quint64         m_gen = 0;
    QByteArray      m_buf;
    int             m_off = 0;

    // A frame the audio rings could not take yet.
    enum PendingKind { PendNone, PendPcm, PendEnd };
    PendingKind      m_pendKind = PendNone;
    uint32_t         m_pendJob = 0;
    QVector<int16_t> m_pendPcm;
    QJsonObject      m_pendMsg;
};

class BackendLink : public QObject
{
    Q_OBJECT
public:
    explicit BackendLink(TtsAudio *audio, QObject *parent = nullptr);
    ~BackendLink() override;

    // Starts the thread + listener with a fresh token. Returns the port (0 on failure).
    quint16 start();
    void    shutdown();                 // idempotent, joins the link thread
    // Forget the current backend connection without emitting anything for it.
    void    dropPeer();

    QString token() const { return m_token; }
    bool    isConnected() const { return m_connected.load(std::memory_order_relaxed); }

    // Thread-safe.
    bool send(const QJsonObject &op);
    void markLocalJob(uint32_t id);
    void forgetJob(uint32_t id);

signals:
    void connected();
    void disconnected();
    void message(const QJsonObject &obj);

private:
    TtsAudio         *m_audio;
    QThread          *m_thread = nullptr;
    LinkWorker       *m_worker = nullptr;
    QString           m_token;
    QMutex            m_localMutex;
    QSet<uint32_t>    m_localJobs;
    std::atomic<bool> m_connected{false};
    quint64           m_liveGen = 0;       // GUI thread only
};

} // namespace gbtts
