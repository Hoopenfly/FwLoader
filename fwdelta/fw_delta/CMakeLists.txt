cmake_minimum_required(VERSION 3.16)
project(fw_delta LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 SerialPort Network Concurrent)

qt_add_executable(fw_delta WIN32
    app/main.cpp
    app/fw_bridge.cpp
    transport/serial_transport.cpp
    transport/tcp_transport.cpp
    transport/udp_transport.cpp
    transport/firmware_uploader.cpp
    core/fw_image.cpp
    core/fw_delta_codec.cpp
    core/fw_fwd.cpp
    res/qml.qrc
    res/app.qrc
)

target_include_directories(fw_delta PRIVATE app core transport)
target_link_libraries(fw_delta PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::SerialPort
    Qt6::Network
    Qt6::Concurrent
)

set_target_properties(fw_delta PROPERTIES
    WIN32_EXECUTABLE TRUE
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../bin"
)
