/*
 * Copyright 2026 qtscrcpy-net contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "relaytransport.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QWaitCondition>
#include <cstring>
#include <limits>

namespace {
const quint32 kMaxFrame = 16u * 1024u * 1024u;
const int kMaxBuffered = 64 * 1024 * 1024;

quint32 read32(const char *data)
{
    const auto *p = reinterpret_cast<const uchar *>(data);
    return (static_cast<quint32>(p[0]) << 24)
            | (static_cast<quint32>(p[1]) << 16)
            | (static_cast<quint32>(p[2]) << 8)
            | static_cast<quint32>(p[3]);
}

void append32(QByteArray &result, quint32 value)
{
    result.append(char((value >> 24) & 0xff));
    result.append(char((value >> 16) & 0xff));
    result.append(char((value >> 8) & 0xff));
    result.append(char(value & 0xff));
}
}

// This shared buffer survives transfer of the video QIODevice to Demuxer.
// RelayTransport never dereferences the moved QIODevice from its socket thread.
struct RelayBuffer {
    mutable QMutex mutex;
    QWaitCondition changed;
    QByteArray bytes;
    quint64 revision = 0;
    bool closed = false;

    bool append(const QByteArray &chunk)
    {
        QMutexLocker lock(&mutex);
        if (closed || chunk.size() > kMaxBuffered - bytes.size()) {
            return false;
        }
        bytes.append(chunk);
        ++revision;
        changed.wakeAll();
        return true;
    }

    void finish()
    {
        QMutexLocker lock(&mutex);
        closed = true;
        ++revision;
        changed.wakeAll();
    }
};

// QIODevice's internal peek/read cache remains in its worker thread. All
// producers write only to RelayBuffer; blocked exact reads wait for a change.
class RelayChannel : public QIODevice
{
public:
    RelayChannel(const QSharedPointer<RelayBuffer> &buffer, bool control,
                 RelayTransport *transport, QObject *parent = nullptr)
        : QIODevice(parent)
        , m_buffer(buffer)
        , m_control(control)
        , m_transport(transport)
    {
        QIODevice::open(control ? QIODevice::ReadWrite : QIODevice::ReadOnly);
    }

    ~RelayChannel() override { close(); }

    bool isSequential() const override { return true; }

    qint64 bytesAvailable() const override
    {
        QMutexLocker lock(&m_buffer->mutex);
        m_observedRevision = m_buffer->revision;
        return QIODevice::bytesAvailable() + m_buffer->bytes.size();
    }

    bool waitForReadyRead(int msecs) override
    {
        QMutexLocker lock(&m_buffer->mutex);
        if (m_buffer->closed) {
            return false;
        }
        // Do not spin on a partial 12-byte header. The Demuxer waits for a
        // *new arrival* whenever its available count is less than the target.
        if (m_buffer->revision > m_observedRevision) {
            return true;
        }
        if (msecs < 0) {
            m_buffer->changed.wait(&m_buffer->mutex);
        } else {
            m_buffer->changed.wait(&m_buffer->mutex, static_cast<unsigned long>(msecs));
        }
        return !m_buffer->closed && m_buffer->revision > m_observedRevision;
    }

    void close() override
    {
        m_buffer->finish();
        QIODevice::close();
    }

    void notifyReady() { emit readyRead(); }

protected:
    qint64 readData(char *data, qint64 limit) override
    {
        QMutexLocker lock(&m_buffer->mutex);
        const qint64 count = qMin(limit, static_cast<qint64>(m_buffer->bytes.size()));
        if (count <= 0) {
            return 0;
        }
        memcpy(data, m_buffer->bytes.constData(), static_cast<size_t>(count));
        m_buffer->bytes.remove(0, static_cast<int>(count));
        return count;
    }

    qint64 writeData(const char *data, qint64 length) override
    {
        if (!m_control || !m_transport) {
            return -1;
        }
        return m_transport->writeControl(data, length);
    }

private:
    QSharedPointer<RelayBuffer> m_buffer;
    bool m_control;
    RelayTransport *m_transport;
    mutable quint64 m_observedRevision = 0;
};

RelayTransport::RelayTransport(QObject *parent)
    : Transport(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_socket(new QTcpSocket(this))
    , m_deadline(new QTimer(this))
{
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, [this]() {
        fail(m_bootstrapped ? QStringLiteral("Relay data timeout")
                            : QStringLiteral("Relay registration or bootstrap timed out"));
    });
    connect(m_socket, &QTcpSocket::connected, this, [this]() {
        if (!m_closed) {
            m_socket->write(m_hello);
            m_deadline->start(5000);
        }
    });
    connect(m_socket, &QTcpSocket::readyRead, this, [this]() {
        if (m_closed) return;
        m_frames += m_socket->readAll();
        handleFrames();
    });
    connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
        if (!m_closed) fail(QStringLiteral("Relay TCP connection closed"));
    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_socket, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
        if (!m_closed) fail(QStringLiteral("Relay TCP: ") + m_socket->errorString());
    });
#else
    connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this, [this](QAbstractSocket::SocketError) {
        if (!m_closed) fail(QStringLiteral("Relay TCP: ") + m_socket->errorString());
    });
#endif
}

RelayTransport::~RelayTransport()
{
    close();
}

bool RelayTransport::open(const qsc::DeviceParams &params)
{
    if (!m_closed) {
        return false;
    }
    const QString host = params.relayHost.trimmed();
    const QString id = params.relayDeviceId.trimmed();
    if (host.isEmpty() || id.isEmpty() || params.relayToken.isEmpty()
            || !params.relayCtlPort || !params.relayDataPort) {
        emit error(QStringLiteral("Relay connection requires host, ports, device ID and token"));
        emit connected(false);
        return false;
    }
    m_closed = false;
    m_bootstrapped = false;
    m_frames.clear();
    m_videoBuffer = QSharedPointer<RelayBuffer>::create();
    m_controlBuffer = QSharedPointer<RelayBuffer>::create();
    m_videoChannel = new RelayChannel(m_videoBuffer, false, this);
    m_controlChannel = new RelayChannel(m_controlBuffer, true, this, this);
    // Direct delivery emits QIODevice::readyRead in the producer (socket) thread.
    // Qt automatically disconnects the lambda when Demuxer deletes the video stream.
    RelayChannel *video = m_videoChannel;
    RelayChannel *control = m_controlChannel;
    connect(this, &RelayTransport::videoDataReady, video,
            [video]() { video->notifyReady(); }, Qt::DirectConnection);
    connect(this, &RelayTransport::controlDataReady, control,
            [control]() { control->notifyReady(); }, Qt::DirectConnection);

    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(host);
    url.setPort(params.relayCtlPort);
    url.setPath(QStringLiteral("/v1/register"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QJsonObject body{
        {QStringLiteral("device_id"), id},
        {QStringLiteral("token"), params.relayToken},
        {QStringLiteral("role"), QStringLiteral("client")}
    };
    m_reply = m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_deadline->start(5000);
    const quint16 dataPort = params.relayDataPort;
    connect(m_reply, &QNetworkReply::finished, this, [this, host, dataPort]() {
        if (m_closed || !m_reply) return;
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->deleteLater();
        m_deadline->stop();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || code != 200) {
            fail(QStringLiteral("Relay registration failed (HTTP %1): %2")
                    .arg(code).arg(reply->errorString()));
            return;
        }
        const QJsonDocument parsed = QJsonDocument::fromJson(reply->readAll());
        const QString sessionId = parsed.object().value(QStringLiteral("session_id")).toString();
        static const QRegularExpression kId(QStringLiteral("^[0-9a-f]{64}$"));
        if (!kId.match(sessionId).hasMatch()) {
            fail(QStringLiteral("Relay returned an invalid session ID"));
            return;
        }
        m_hello = QByteArrayLiteral("HELLO ") + sessionId.toLatin1() + QByteArrayLiteral("\r\n");
        m_socket->connectToHost(host, dataPort);
        m_deadline->start(5000);
    });
    return true;
}

void RelayTransport::close()
{
    if (m_closed) return;
    m_closed = true; // guard against synchronous socket callbacks reentering Device
    m_deadline->stop();
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    m_socket->abort();
    if (m_videoBuffer) m_videoBuffer->finish(); // wake Demuxer before its stopDecode()
    if (m_controlBuffer) m_controlBuffer->finish();
    if (m_videoChannel) {
        delete m_videoChannel; // only if never transferred to Demuxer
        m_videoChannel = nullptr;
    }
    if (m_controlChannel) {
        delete m_controlChannel;
        m_controlChannel = nullptr;
    }
}

void RelayTransport::fail(const QString &message)
{
    if (m_closed) return;
    const bool wasConnected = m_bootstrapped;
    qWarning().noquote() << message;
    emit error(message);
    close();
    if (wasConnected) emit disconnected();
    else emit connected(false);
}

bool RelayTransport::isReversePort(quint16) const
{
    return false;
}

QIODevice *RelayTransport::takeVideoStream()
{
    RelayChannel *stream = m_videoChannel;
    m_videoChannel = nullptr;
    return stream;
}

QIODevice *RelayTransport::controlStream() const
{
    return m_controlChannel;
}

qint64 RelayTransport::writeControl(const char *buffer, qint64 count)
{
    if (m_closed || !m_bootstrapped || count < 0 || count > kMaxFrame) {
        return -1;
    }
    if (count == 0) return 0;
    QByteArray frame;
    frame.reserve(static_cast<int>(count) + 5);
    frame.append(char(0x02));
    append32(frame, static_cast<quint32>(count));
    frame.append(buffer, static_cast<int>(count));
    return m_socket->write(frame) == frame.size() ? count : -1;
}

void RelayTransport::handleFrames()
{
    while (!m_closed && m_frames.size() >= 5) {
        const uchar channel = static_cast<uchar>(m_frames[0]);
        const quint32 length = read32(m_frames.constData() + 1);
        if (length > kMaxFrame) {
            fail(QStringLiteral("Relay protocol error: message exceeds 16 MiB"));
            return;
        }
        if (static_cast<quint64>(m_frames.size()) < static_cast<quint64>(length) + 5u) {
            return;
        }
        const QByteArray payload = m_frames.mid(5, static_cast<int>(length));
        m_frames.remove(0, static_cast<int>(length) + 5);
        if (!m_bootstrapped) {
            if (channel != 0xff || payload.size() < 13) {
                fail(QStringLiteral("Relay protocol error: expected bootstrap first"));
                return;
            }
            const quint32 nameLength = read32(payload.constData());
            if (nameLength == 0 || nameLength > 64 || payload.size() != static_cast<int>(nameLength + 12)) {
                fail(QStringLiteral("Relay protocol error: invalid bootstrap name length"));
                return;
            }
            const QByteArray rawName = payload.mid(4, static_cast<int>(nameLength));
            const QString name = QString::fromUtf8(rawName);
            const quint32 width = read32(payload.constData() + 4 + nameLength);
            const quint32 height = read32(payload.constData() + 8 + nameLength);
            if (name.isEmpty() || name.toUtf8() != rawName || width == 0 || height == 0
                    || width > static_cast<quint32>(std::numeric_limits<int>::max())
                    || height > static_cast<quint32>(std::numeric_limits<int>::max())) {
                fail(QStringLiteral("Relay protocol error: invalid bootstrap metadata"));
                return;
            }
            m_bootstrapped = true;
            m_deadline->stop();
            emit connected(true, name, QSize(static_cast<int>(width), static_cast<int>(height)));
        } else if (channel == 0x01) {
            if (!m_videoBuffer || !m_videoBuffer->append(payload)) {
                fail(QStringLiteral("Relay video buffer exceeded limit"));
                return;
            }
            emit videoDataReady();
        } else if (channel == 0x02) {
            if (!m_controlBuffer || !m_controlBuffer->append(payload)) {
                fail(QStringLiteral("Relay control buffer exceeded limit"));
                return;
            }
            emit controlDataReady();
        } else {
            fail(QStringLiteral("Relay protocol error: unknown channel 0x%1")
                    .arg(channel, 2, 16, QLatin1Char('0')));
            return;
        }
    }
}
