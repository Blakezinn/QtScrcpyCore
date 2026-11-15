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
#include "adbtransport.h"
#include "server.h"

AdbTransport::AdbTransport(QObject *parent)
    : Transport(parent)
    , m_server(new Server(this))
{
    connect(m_server, &Server::serverStarted, this, [this](bool success, const QString &name, const QSize &size) {
        if (m_closed) {
            return;
        }
        if (!success) {
            emit error(QStringLiteral("ADB session could not be opened"));
        } else {
            auto socket = m_server->getControlSocket();
            connect(socket, &QTcpSocket::disconnected, this, &Transport::disconnected);
            connect(socket, &QTcpSocket::errorOccurred,
                    this, [this, socket](QAbstractSocket::SocketError) {
                emit error(socket->errorString());
            });
        }
        emit connected(success, name, size);
    });
    connect(m_server, &Server::serverStoped, this, [this]() {
        if (!m_closed) {
            emit disconnected();
        }
    });
}

AdbTransport::~AdbTransport()
{
    close();
}

bool AdbTransport::open(const qsc::DeviceParams &source)
{
    if (!m_closed) {
        return false;
    }
    m_closed = false;
    Server::ServerParams params;
    params.serverLocalPath = source.serverLocalPath;
    params.serverRemotePath = source.serverRemotePath;
    params.serial = source.serial;
    params.localPort = source.localPort;
    params.maxSize = source.maxSize;
    params.bitRate = source.bitRate;
    params.maxFps = source.maxFps;
    params.videoSource = source.videoSource;
    params.cameraFacing = source.cameraFacing;
    params.cameraId = source.cameraId;
    params.useReverse = source.useReverse;
    params.captureOrientationLock = source.captureOrientationLock;
    params.captureOrientation = source.captureOrientation;
    params.stayAwake = source.videoSource == qsc::VIDEO_SOURCE_CAMERA ? false : source.stayAwake;
    params.serverVersion = source.serverVersion;
    params.logLevel = source.logLevel;
    params.codecOptions = source.codecOptions;
    params.codecName = source.codecName;
    params.scid = source.scid;
    params.crop = source.crop;
    params.displayId = source.displayId;
    params.newDisplay = source.newDisplay;
    params.flexDisplay = source.flexDisplay;
    params.vdDestroyContent = source.vdDestroyContent;
    params.vdSystemDecorations = source.vdSystemDecorations;
    params.displayImePolicy = source.displayImePolicy;
    params.keepActive = source.keepActive;
    params.control = true;
    return m_server->start(params);
}

void AdbTransport::close()
{
    if (m_closed) {
        return;
    }
    // Prevent synchronous signals from reentering Device teardown.
    m_closed = true;
    if (auto socket = m_server->getControlSocket()) {
        disconnect(socket, nullptr, this, nullptr);
    }
    m_server->stop();
}

bool AdbTransport::isReversePort(quint16 port) const
{
    return m_server->isReverse() && port == m_server->getParams().localPort;
}

QIODevice *AdbTransport::takeVideoStream()
{
    return m_server->removeVideoSocket();
}

QIODevice *AdbTransport::controlStream() const
{
    return m_server->getControlSocket();
}
