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
#ifndef RELAYTRANSPORT_H
#define RELAYTRANSPORT_H

#include "transport.h"
#include <QByteArray>
#include <QSharedPointer>

class QNetworkAccessManager;
class QNetworkReply;
class QTcpSocket;
class QTimer;
class RelayChannel;
struct RelayBuffer;

// One qscnet v1 TCP stream maps to two independent QIODevice channels.
class RelayTransport : public Transport
{
    Q_OBJECT
public:
    explicit RelayTransport(QObject *parent = nullptr);
    ~RelayTransport() override;

    bool open(const qsc::DeviceParams &params) override;
    void close() override;
    bool isReversePort(quint16 port) const override;
    QIODevice *takeVideoStream() override;
    QIODevice *controlStream() const override;

signals:
    void videoDataReady();
    void controlDataReady();

private:
    friend class RelayChannel;
    qint64 writeControl(const char *buffer, qint64 bytes);
    void handleFrames();
    void fail(const QString &message);

    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    QTcpSocket *m_socket = nullptr;
    QTimer *m_deadline = nullptr;
    RelayChannel *m_videoChannel = nullptr;   // ownership transferred exactly once
    RelayChannel *m_controlChannel = nullptr; // stays on the UI thread
    QSharedPointer<RelayBuffer> m_videoBuffer;
    QSharedPointer<RelayBuffer> m_controlBuffer;
    QByteArray m_frames;
    QByteArray m_hello;
    bool m_closed = true;
    bool m_bootstrapped = false;
};

#endif // RELAYTRANSPORT_H
