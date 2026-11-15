<!--
Copyright 2026 qtscrcpy-net contributors
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->
# Transport seam (TASK-002)

```text
DeviceManage -> Device -> Transport::open(DeviceParams)
                            |
                      AdbTransport -> Server
                                      |
              AdbProcess -> AdbProcessImpl/QProcess -> adb
              push jar; reverse/forward localabstract:scrcpy_<scid>;
              shell CLASSPATH=... app_process ... scrcpy.Server

Android encoder -> scrcpy-server -> adb tunnel -> local TCP video socket
  -> Server::readInfo (device name + codec/session metadata)
  -> Transport::takeVideoStream() [QIODevice, ownership transferred]
  -> Demuxer worker: exact header/payload reads -> AVPacket
  -> Decoder::push -> VideoBuffer -> DeviceObserver/UI
                      \-> Recorder (optional, unchanged)

UI -> Device -> Controller -> ControlMsg serialization
  -> callback -> Transport::controlStream()->write(bytes)
  -> local TCP control socket -> adb tunnel -> scrcpy-server -> Android

Android clipboard/device messages -> same control socket
  -> QIODevice::readyRead -> DeviceMsg::deserialize(peek)
  -> read only consumed bytes -> Controller::recvDeviceMsg
```

**ADB is setup, not the media pipe.** QProcess stdout/stderr carry server
logs and process status, not video or serialized control packets. Reverse
mode accepts video first (`TcpServer` creates `VideoSocket`), then control;
forward mode connects two sockets and consumes the existing dummy bytes.
The adapter leaves Server's startup, retry, timeout and tunnel removal
sequence in place. USB and LAN ADB serials use the identical adapter.

**Contract.** `Transport` owns the session lifecycle: `open/close`,
`connected(success, name, size)`, `disconnected`, and `error(message)`.
Channels are unframed, sequential, full-duplex `QIODevice` streams:
nonblocking `read/write/peek/bytesAvailable`, `readyRead`, `bytesWritten`;
partial reads/writes retain Qt semantics. No transport adds packet headers.
Control stays in the application thread and is borrowed until close.
Video ownership transfers once to Demuxer, which moves it (parentless) to
its worker before starting; it closes/deletes it in that worker.
The existing blocking exact-read loop is retained there using
`QIODevice::waitForReadyRead(-1)`. Future stream adapters must support that
worker wait as well as asynchronous reads; do not block the UI event loop.
Closing the ADB session kills the Android server as before, ending video
reads before Device waits for Demuxer. Device owns the adapter; it is not
shared across sessions. File installation, push and show-touch ADB
management commands remain separate from this data-plane seam.

**Baseline conflict.** This checkout bundles scrcpy **4.1**, not v1:
`Server::readInfo` reads 64+16 bytes, Demuxer handles 12-byte session
metadata and flags. TASK-002 forbids changing protocol or packets, so these
existing bytes/parsers are preserved rather than downgraded. No relay or
new network transport is introduced. See the meta-repo `LOG.md`.

Supplementary decode smoke (not manual acceptance): from the client root,
configure `cmake -S QtScrcpy/QtScrcpyCore/docs/transport-smoke -B
out/transport-smoke -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
-DCMAKE_PREFIX_PATH=F:/Qt/5.15.2/msvc2019_64 -DQSC_DEPLOY_PATH=out/deploy`,
then `cmake --build out/transport-smoke --parallel 8` under vcvarsall x64.
Copy `transport-smoke.exe` to the deployed client directory (Qt/DLLs/plugins),
set `QT_FORCE_STDERR_LOGGING=1`, and run
`transport-smoke <absolute-deploy-directory> <adb-serial> <reverse|forward>`.
It opens the normal Device path, counts decoded frames for 15 seconds,
then exercises teardown; exit 0 requires connection and at least one frame.
