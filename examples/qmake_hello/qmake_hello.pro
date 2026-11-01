QT += core network serialport
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = qmake_hello

ROOT = $$clean_path($$PWD/../..)
BUILD = $$ROOT/build
GEN_DIR = $$OUT_PWD/generated
GEN_EXE = $$BUILD/bin/ddsrpcgen
IDL = $$PWD/../hello/hello.idl

gen.input = IDL
gen.commands = $$GEN_EXE --role all --out-dir $$GEN_DIR --namespace demo $$IDL
gen.output = \
    $$GEN_DIR/hello.ddsrpc.types.h \
    $$GEN_DIR/hello.ddsrpc.service.h \
    $$GEN_DIR/hello.ddsrpc.service.cpp \
    $$GEN_DIR/hello.ddsrpc.client.h \
    $$GEN_DIR/hello.ddsrpc.client.cpp
gen.depends = $$GEN_EXE $$IDL
gen.CONFIG += no_link target_predeps
QMAKE_EXTRA_COMPILERS += gen

SOURCES += $$PWD/../hello/main.cpp \
    $$GEN_DIR/hello.ddsrpc.service.cpp \
    $$GEN_DIR/hello.ddsrpc.client.cpp
HEADERS += \
    $$GEN_DIR/hello.ddsrpc.types.h \
    $$GEN_DIR/hello.ddsrpc.service.h \
    $$GEN_DIR/hello.ddsrpc.client.h

INCLUDEPATH += $$GEN_DIR \
    $$ROOT/include \
    $$ROOT/third_party/Transport/include \
    $$ROOT/third_party/AsyncTask/coro
DEFINES += ASYNC_HAS_QTCORE
LIBS += -L$$BUILD/lib -lddsrpc_runtime -ltransport -lasynctask
LIBS += /usr/local/lib/libboost_fiber.a \
    /usr/local/lib/libboost_context.a \
    /usr/local/lib/libboost_thread.a \
    /usr/local/lib/libboost_chrono.a -lpthread
