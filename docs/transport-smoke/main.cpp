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
    if (argc != 4) {
        qCritical("usage: transport-smoke <deploy-directory> <adb-serial> <reverse|forward>");
        return 2;
    }
    FrameCounter counter;
    const QString deploy = QString::fromLocal8Bit(argv[1]);
    qsc::AdbProcess::setAdbPath(deploy + "/adb.exe");
    qsc::DeviceParams params;
    params.serial = QString::fromLocal8Bit(argv[2]);
    params.serverLocalPath = deploy + "/scrcpy-server";
    params.localPort = 28183;
    params.scid = 0x2002;
    params.useReverse = QString::fromLocal8Bit(argv[3]) == "reverse";
    params.maxSize = 720;
    params.maxFps = 30;
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
    device->registerDeviceObserver(&counter);
    QTimer::singleShot(15000, &app, [&]() {
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
