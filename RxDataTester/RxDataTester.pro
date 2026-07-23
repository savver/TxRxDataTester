QT += core gui widgets serialport

CONFIG += c++11 warn_on
TEMPLATE = app
TARGET = RxDataTester
VERSION = 1.1.0

DEFINES += QT_DEPRECATED_WARNINGS

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    rxworker.cpp

HEADERS += \
    mainwindow.h \
    rxworker.h

FORMS += \
    mainwindow.ui

msvc:QMAKE_CXXFLAGS += /utf-8
