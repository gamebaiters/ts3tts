#include "net/BackendLink.h"
#include "audio/TtsAudio.h"
#include "ts3log.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <cstring>

namespace gbtts {

namespace {

constexpr quint8  kTypeJson = 0x01;
constexpr quint8  kTypePcm  = 0x02;
constexpr quint8  kTypeMic  = 0x03;   // plugin -> backend: user's microphone (voice changer)
constexpr quint8  kTypeVc   = 0x04;   // backend -> plugin: converted voice
constexpr qint64  kMaxMicBacklogBytes = 256 * 1024;
constexpr quint32 kMaxFrame = 16u * 1024u * 1024u;

quint32 readLE32(const char *p)
{
    return static_cast<quint32>(static_cast<unsigned char>(p[0])) |
           (static_cast<quint32>(static_cast<unsigned char>(p[1])) << 8) |
           (static_cast<quint32>(static_cast<unsigned char>(p[2])) << 16) |
           (static_cast<quint32>(static_cast<unsigned char>(p[3])) << 24);
}

void writeLE32(char *p, quint32 v)
{
    p[0] = static_cast<char>(v & 0xFF);
    p[1] = static_cast<char>((v >> 8) & 0xFF);
    p[2] = static_cast<char>((v >> 16) & 0xFF);
    p[3] = static_cast<char>((v >> 24) & 0xFF);
}

QByteArray makeFrame(quint8 type, const QByteArray &payload)
{
    QByteArray f;
    f.resize(5 + payload.size());
    const quint32 len = static_cast<quint32>(payload.size() + 1);
    f[0] = static_cast<char>(len & 0xFF);
    f[1] = static_cast<char>((len >> 8) & 0xFF);
    f[2] = static_cast<char>((len >> 16) & 0xFF);
    f[3] = static_cast<char>((len >> 24) & 0xFF);
    f[4] = static_cast<char>(type);
    std::memcpy(f.data() + 5, payload.constData(), static_cast<size_t>(payload.size()));
    return f;
}

} // namespace

// ===========================================================================
// LinkWorker (link thread)
// ===========================================================================
LinkWorker::LinkWorker(TtsAudio *audio, QMutex *localMutex, QSet<uint32_t> *localJobs)
    : m_audio(audio), m_localMutex(localMutex), m_localJobs(localJobs)
{
}

quint16 LinkWorker::listen(const QString &token)
{
    m_token = token;
    if (!m_server) {
        m_server = new QTcpServer(this);
        m_server->setMaxPendingConnections(1);
        connect(m_server, &QTcpServer::newConnection, this, &LinkWorker::onNewConnection);
        m_retry = new QTimer(this);
        m_retry->setSingleShot(true);
        m_retry->setInterval(5);
        connect(m_retry, &QTimer::timeout, this, &LinkWorker::retryPending);
        // Voice changer uplink: the capture thread only fills a lock-free ring; this
        // timer turns it into 20 ms frames here, never on a TeamSpeak audio thread.
        m_micTimer = new QTimer(this);
        m_micTimer->setTimerType(Qt::PreciseTimer);
        m_micTimer->setInterval(10);
        connect(m_micTimer, &QTimer::timeout, this, &LinkWorker::pumpMic);
        m_micTimer->start();
    }
    if (!m_server->isListening() && !m_server->listen(QHostAddress::LocalHost, 0)) {
        logError("GBTTS link: cannot listen on loopback: %s", m_server->errorString().toUtf8().constData());
        return 0;
    }
    return m_server->serverPort();
}

void LinkWorker::resetPeerState()
{
    if (m_retry) m_retry->stop();
    m_authed = false;
    m_buf.clear();
    m_off = 0;
    m_pendKind = PendNone;
    m_pendPcm.clear();
    m_pendMsg = QJsonObject();
}

void LinkWorker::dropPeer()
{
    if (m_sock) {
        m_sock->disconnect(this);
        m_sock->flush();          // best effort for a queued "shutdown" frame
        m_sock->abort();
        delete m_sock;
        m_sock = nullptr;
    }
    resetPeerState();
    ++m_gen;                      // anything still queued for the old peer is stale
}

void LinkWorker::closeAll()
{
    dropPeer();
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
    delete m_micTimer;
    m_micTimer = nullptr;
    delete m_retry;
    m_retry = nullptr;
}

void LinkWorker::pumpMic()
{
    constexpr int kBlock = 960;   // 20 ms @ 48 kHz
    int16_t buf[kBlock];
    const bool live = m_sock && m_authed && m_audio->vcEnabled();
    while (m_audio->micUplinkAvailable() >= kBlock) {
        const size_t n = m_audio->readMicUplink(buf, kBlock);
        // No backend, or the socket is not draining: drop rather than grow memory
        // or deliver a voice seconds late.
        if (!live || m_sock->bytesToWrite() > kMaxMicBacklogBytes) continue;
        QByteArray payload(4 + static_cast<int>(n) * 2, Qt::Uninitialized);
        writeLE32(payload.data(), m_micSeq++);
        std::memcpy(payload.data() + 4, buf, n * 2);
        m_sock->write(makeFrame(kTypeMic, payload));
    }
}

void LinkWorker::onNewConnection()
{
    while (QTcpSocket *s = m_server->nextPendingConnection()) {
        const bool loopback = s->peerAddress() == QHostAddress(QHostAddress::LocalHost) ||
                              s->peerAddress() == QHostAddress(QHostAddress::LocalHostIPv6);
        if (!loopback || (m_sock && m_authed)) {
            // One backend at a time: a stray local connection must not evict the
            // authenticated one (a restart drops the old peer before relaunching).
            s->abort();
            s->deleteLater();
            if (loopback) emit protocolError(QStringLiteral("extra connection refused"));
            continue;
        }
        if (m_sock) {
            // Previous peer never authenticated: replace it silently.
            m_sock->disconnect(this);
            m_sock->abort();
            m_sock->deleteLater();
        }
        m_sock = s;
        resetPeerState();
        // Bounded Qt-side buffer: when we stop reading, TCP flow control reaches
        // the backend instead of Qt buffering the whole utterance in RAM.
        m_sock->setReadBufferSize(512 * 1024);
        m_sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        connect(m_sock, &QTcpSocket::readyRead, this, &LinkWorker::onReadyRead);
        connect(m_sock, &QTcpSocket::disconnected, this, &LinkWorker::onSocketGone);
        if (m_sock->bytesAvailable() > 0) onReadyRead();
    }
}

void LinkWorker::onSocketGone()
{
    if (!m_sock || sender() != m_sock) return;
    const bool wasAuthed = m_authed;
    m_sock->disconnect(this);
    m_sock->deleteLater();
    m_sock = nullptr;
    resetPeerState();
    if (wasAuthed) emit disconnected(m_gen);
}

void LinkWorker::writeFrame(const QByteArray &frame)
{
    if (!m_sock || !m_authed) return;
    m_sock->write(frame);
}

bool LinkWorker::isLocal(uint32_t job)
{
    QMutexLocker lk(m_localMutex);
    return m_localJobs->contains(job);
}

void LinkWorker::onReadyRead()
{
    if (!m_sock) return;
    if (m_pendKind != PendNone) return;           // back-pressured: retry timer resumes
    m_buf.append(m_sock->readAll());
    processBuffer();
}

void LinkWorker::retryPending()
{
    if (m_pendKind == PendPcm) {
        if (!m_audio->pushPcm(m_pendJob, m_pendPcm.constData(), m_pendPcm.size(), isLocal(m_pendJob))) {
            m_retry->start();
            return;
        }
    } else if (m_pendKind == PendEnd) {
        if (!m_audio->pushEnd(m_pendJob, isLocal(m_pendJob))) {
            m_retry->start();
            return;
        }
        emit message(m_gen, m_pendMsg);
    }
    m_pendKind = PendNone;
    if (m_sock) m_buf.append(m_sock->readAll());
    processBuffer();
}

void LinkWorker::processBuffer()
{
    while (m_pendKind == PendNone) {
        const int avail = m_buf.size() - m_off;
        if (avail < 5) break;
        const char *p = m_buf.constData() + m_off;
        const quint32 len = readLE32(p);
        if (len < 1 || len > kMaxFrame) {
            emit protocolError(QStringLiteral("bad frame length"));
            if (m_sock) m_sock->abort();
            return;
        }
        if (static_cast<quint32>(avail) < 4u + len) break;
        const quint8 type = static_cast<quint8>(p[4]);
        const char *payload = p + 5;
        const int plen = static_cast<int>(len - 1);
        m_off += 4 + static_cast<int>(len);

        if (type == kTypeJson) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromRawData(payload, plen), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;
            const QJsonObject obj = doc.object();
            const QString ev = obj.value(QStringLiteral("ev")).toString();

            if (!m_authed) {
                if (ev == QLatin1String("hello") && obj.value(QStringLiteral("token")).toString() == m_token) {
                    m_authed = true;
                    ++m_gen;
                    emit connected(m_gen);
                    emit message(m_gen, obj);
                } else {
                    emit protocolError(QStringLiteral("unauthenticated peer"));
                    if (m_sock) m_sock->abort();
                    return;
                }
                continue;
            }

            if (ev == QLatin1String("job_end")) {
                // In-band with the PCM stream so the audio core sees the end of an
                // utterance exactly after its last sample.
                const uint32_t job = static_cast<uint32_t>(obj.value(QStringLiteral("id")).toDouble());
                if (!m_audio->pushEnd(job, isLocal(job))) {
                    m_pendKind = PendEnd;
                    m_pendJob = job;
                    m_pendMsg = obj;
                    m_retry->start();
                    break;
                }
            }
            emit message(m_gen, obj);
        } else if (type == kTypeVc && m_authed && plen >= 4) {
            // Converted voice: never parked like TTS PCM - late audio is dropped by pushVc.
            const int n = (plen - 4) / 2;
            if (n > 0) {
                QVector<int16_t> pcm(n);
                std::memcpy(pcm.data(), payload + 4, static_cast<size_t>(n) * 2);
                m_audio->pushVc(pcm.constData(), n);
            }
        } else if (type == kTypePcm && m_authed && plen >= 4) {
            const uint32_t job = readLE32(payload);
            const int n = (plen - 4) / 2;
            if (n <= 0) continue;
            // int16 LE on the wire; x86/x64 hosts are LE, copy for alignment.
            QVector<int16_t> pcm(n);
            std::memcpy(pcm.data(), payload + 4, static_cast<size_t>(n) * 2);
            if (!m_audio->pushPcm(job, pcm.constData(), n, isLocal(job))) {
                m_pendKind = PendPcm;
                m_pendJob = job;
                m_pendPcm = std::move(pcm);
                m_retry->start();
                break;
            }
        }
    }
    if (m_off > 0 && (m_off > 1 << 20 || m_off == m_buf.size())) {
        m_buf.remove(0, m_off);
        m_off = 0;
    }
}

// ===========================================================================
// BackendLink (GUI thread facade)
// ===========================================================================
BackendLink::BackendLink(TtsAudio *audio, QObject *parent) : QObject(parent), m_audio(audio) {}

BackendLink::~BackendLink()
{
    shutdown();
}

quint16 BackendLink::start()
{
    if (!m_thread) {
        m_thread = new QThread();
        m_thread->setObjectName(QStringLiteral("gbtts-link"));
        m_worker = new LinkWorker(m_audio, &m_localMutex, &m_localJobs);
        m_worker->moveToThread(m_thread);
        connect(m_worker, &LinkWorker::connected, this, [this](quint64 gen) {
            m_liveGen = gen;
            m_connected.store(true, std::memory_order_relaxed);
            emit connected();
        });
        connect(m_worker, &LinkWorker::disconnected, this, [this](quint64 gen) {
            if (gen != m_liveGen) return;      // a peer we already dropped
            m_liveGen = 0;
            m_connected.store(false, std::memory_order_relaxed);
            emit disconnected();
        });
        connect(m_worker, &LinkWorker::message, this, [this](quint64 gen, const QJsonObject &obj) {
            if (gen == m_liveGen) emit message(obj);
        });
        connect(m_worker, &LinkWorker::protocolError, this, [](const QString &what) {
            logWarning("GBTTS link: %s", what.toUtf8().constData());
        });
        m_thread->start();
    }
    QByteArray raw(24, Qt::Uninitialized);
    for (char &c : raw) c = static_cast<char>(QRandomGenerator::system()->generate() & 0xFF);
    m_token = QString::fromLatin1(raw.toHex());

    quint16 port = 0;
    QMetaObject::invokeMethod(m_worker, "listen", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(quint16, port), Q_ARG(QString, m_token));
    return port;
}

void BackendLink::shutdown()
{
    if (!m_thread) return;
    QMetaObject::invokeMethod(m_worker, "closeAll", Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait(3000);
    delete m_worker;
    m_worker = nullptr;
    delete m_thread;
    m_thread = nullptr;
    m_liveGen = 0;
    m_connected.store(false, std::memory_order_relaxed);
}

void BackendLink::dropPeer()
{
    m_liveGen = 0;
    m_connected.store(false, std::memory_order_relaxed);
    if (m_worker) QMetaObject::invokeMethod(m_worker, "dropPeer", Qt::BlockingQueuedConnection);
}

bool BackendLink::send(const QJsonObject &op)
{
    if (!m_worker || !isConnected()) return false;
    const QByteArray frame = makeFrame(kTypeJson, QJsonDocument(op).toJson(QJsonDocument::Compact));
    LinkWorker *w = m_worker;
    QMetaObject::invokeMethod(m_worker, [w, frame] { w->writeFrame(frame); }, Qt::QueuedConnection);
    return true;
}

void BackendLink::markLocalJob(uint32_t id)
{
    QMutexLocker lk(&m_localMutex);
    m_localJobs.insert(id);
}

void BackendLink::forgetJob(uint32_t id)
{
    QMutexLocker lk(&m_localMutex);
    m_localJobs.remove(id);
}

} // namespace gbtts
