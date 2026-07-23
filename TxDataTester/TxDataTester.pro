QT += core gui widgets serialport

CONFIG += c++11 warn_on
TEMPLATE = app
TARGET = TxDataTester
VERSION = 1.5.0

DEFINES += QT_DEPRECATED_WARNINGS

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    txworker.cpp

HEADERS += \
    mainwindow.h \
    txworker.h

FORMS += \
    mainwindow.ui

msvc:QMAKE_CXXFLAGS += /utf-8
