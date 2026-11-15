# Copyright 2026 qtscrcpy-net contributors
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#     http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
cmake_minimum_required(VERSION 3.19)
project(TransportSmoke LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 11)
set(QT_DESIRED_VERSION 5)
find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets Network)
add_subdirectory(../.. core)
add_executable(transport-smoke main.cpp)
target_link_libraries(transport-smoke PRIVATE QtScrcpyCore Qt5::Widgets Qt5::Network)
