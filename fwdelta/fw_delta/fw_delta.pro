QT += quick quickcontrols2 serialport network concurrent

#生成QML类型信息，让qmllint能识别C++注册的FwBridge单例
CONFIG += qmltypes
QML_IMPORT_NAME = FwDelta
QML_IMPORT_MAJOR_VERSION = 1
#显式提供依赖模块的元类型，qmltyperegistrar才能解析QObject基类与QString等类型
QML_FOREIGN_METATYPES += $$[QT_INSTALL_ARCHDATA]/metatypes/qt6core_metatypes.json
QML_FOREIGN_METATYPES += $$[QT_INSTALL_ARCHDATA]/metatypes/qt6qml_metatypes.json

TEMPLATE = app
TARGET = fw_delta
VERSION = 1.0.0
CONFIG += c++17

#输出到工程上一级的bin目录，与Qt运行时放在一起
DESTDIR = $$PWD/../bin

INCLUDEPATH += app core transport
SOURCES += app/main.cpp \
           app/fw_bridge.cpp \
           app/transport_runner.cpp \
           transport/serial_transport.cpp \
           transport/tcp_transport.cpp \
           transport/udp_transport.cpp \
           transport/firmware_uploader.cpp \
           core/fw_image.cpp \
           core/fw_delta_codec.cpp \
           core/fw_fwd.cpp
HEADERS += app/fw_bridge.h \
           app/transport_runner.h \
           transport/transport.h \
           transport/serial_transport.h \
           transport/tcp_transport.h \
           transport/udp_transport.h \
           transport/firmware_uploader.h \
           core/fw_image.h \
           core/fw_delta_codec.h \
           core/fw_fwd.h

RESOURCES += res/qml.qrc res/app.qrc

win32: {
    #Windows程序图标与文件属性
    RC_ICONS = res/app.ico
    RC_LANG = 0x0004
    QMAKE_TARGET_COMPANY = "Hoopenfly"
    QMAKE_TARGET_PRODUCT = "FwDelta"
    QMAKE_TARGET_DESCRIPTION = "FwLoader差分升级工具"
    QMAKE_TARGET_COPYRIGHT = "Copyright (C) 2026"
    #构建后把类型信息整理成qmllint可导入的模块目录
    QMAKE_POST_LINK += $$shell_path($$PWD/tools/update_qmltypes.bat) $$shell_path($$OUT_PWD)
}
