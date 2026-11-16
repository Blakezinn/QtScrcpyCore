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
#include <atomic>
#include <QApplication>
#include <QDebug>
#include <QTimer>

#include "QtScrcpyCore.h"
#include "adbprocess.h"

// Supplementary real-device decode smoke, NOT a replacement for manual a-e.
class FrameCounter : public qsc::DeviceObserver
{
public:
    std::atomic<unsigned> frames { 0 };
    void onFrame(int, int, uint8_t *, uint8_t *, uint8_t *, int, int, int) override
    {
        ++frames;
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const bool relay = argc > 1 && QString::fromLatin1(argv[1]) == QStringLiteral("--relay");
    if ((!relay && argc != 4) || (relay && argc != 8)) {
        qCritical("usage: transport-smoke <deploy-directory> <adb-serial> <reverse|forward>");
        qCritical("   or: transport-smoke --relay <deploy-directory> <host> <ctl> <data> <device-id> <token>");
        return 2;
    }
    FrameCounter counter;
    const QString deploy = QString::fromLocal8Bit(argv[relay ? 2 : 1]);
    qsc::DeviceParams params;
    if (relay) {
        bool validCtl = false, validData = false;
        const quint16 ctlPort = QString::fromLocal8Bit(argv[4]).toUShort(&validCtl);
        const quint16 dataPort = QString::fromLocal8Bit(argv[5]).toUShort(&validData);
        if (!validCtl || !validData || !ctlPort || !dataPort) {
            qCritical("relay ports must be valid nonzero numbers");
            return 2;
        }
        params.transport = qsc::TRANSPORT_RELAY;
        params.relayHost = QString::fromLocal8Bit(argv[3]);
        params.relayCtlPort = ctlPort;
        params.relayDataPort = dataPort;
        params.relayDeviceId = QString::fromLocal8Bit(argv[6]);
        params.relayToken = QString::fromLocal8Bit(argv[7]);
        params.serial = QStringLiteral("relay/") + params.relayDeviceId;
    } else {
        qsc::AdbProcess::setAdbPath(deploy + "/adb.exe");
        params.serial = QString::fromLocal8Bit(argv[2]);
        params.serverLocalPath = deploy + "/scrcpy-server";
        params.localPort = 28183;
        params.scid = 0x2002;
        params.useReverse = QString::fromLocal8Bit(argv[3]) == "reverse";
        params.maxSize = 720;
        params.maxFps = 30;
    }
    auto &devices = qsc::IDeviceManage::getInstance();
    bool opened = false;
    QObject::connect(&devices, &qsc::IDeviceManage::deviceConnected, &app,
                     [&](bool success, const QString &, const QString &, const QSize &size) {
        opened = success;
        qInfo() << "SESSION_CONNECTED" << success << size;
        if (!success) {
            app.exit(3);
        }
    });
    if (!devices.connectDevice(params)) {
        return 4;
    }
    auto device = devices.getDevice(params.serial);
    if (!device) return 4;
    device->registerDeviceObserver(&counter);
    if (relay) {
        QObject::connect(&devices, &qsc::IDeviceManage::deviceDisconnected, &app,
                         [&](const QString &serial) {
            if (serial == params.serial) {
                qInfo() << "SESSION_DISCONNECTED";
                app.exit(6);
            }
        });
    }
    QTimer progress;
    if (relay) {
        QObject::connect(&progress, &QTimer::timeout, &app, [&]() {
            qInfo() << "DECODED_FRAMES" << counter.frames.load();
        });
        progress.start(2000);
    }
    QTimer::singleShot(relay ? 10000 : 15000, &app, [&]() {
        qInfo() << "DECODED_FRAMES" << counter.frames.load();
        app.exit(opened && counter.frames.load() > 0 ? 0 : 5);
    });
    const int result = app.exec();
    if (device) {
        device->deRegisterDeviceObserver(&counter);
    }
    devices.disconnectDevice(params.serial);
    qInfo() << "SMOKE_EXIT" << result;
    return result;
}
