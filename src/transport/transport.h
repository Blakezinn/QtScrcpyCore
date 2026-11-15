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
#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <QIODevice>
#include <QSize>

#include "../../include/QtScrcpyCoreDef.h"

// Session interface. Each channel is a full-duplex sequential byte stream.
// QIODevice supplies nonblocking read/write, readyRead and bytesWritten.
class Transport : public QObject
{
    Q_OBJECT
public:
    explicit Transport(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~Transport() {}

    virtual bool open(const qsc::DeviceParams &params) = 0;
    virtual void close() = 0;
    virtual bool isReversePort(quint16 port) const = 0;
    // Transfer ownership of video to the demuxer, before moving it to its thread.
    virtual QIODevice *takeVideoStream() = 0;
    // Borrowed; valid until close(). Never move the control stream to another thread.
    virtual QIODevice *controlStream() const = 0;

signals:
    void connected(bool success, const QString &deviceName = "", const QSize &size = QSize());
    void disconnected();
    void error(const QString &message);
};

#endif // TRANSPORT_H
