QT += core gui widgets network serialport

CONFIG += c++11 warn_on
TEMPLATE = app
TARGET = RxDataTester
VERSION = 1.4.0

DEFINES += QT_DEPRECATED_WARNINGS

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    rxworker.cpp \
    udprxworker.cpp

HEADERS += \
    mainwindow.h \
    rxworker.h \
    udprxworker.h

FORMS += \
    mainwindow.ui

msvc:QMAKE_CXXFLAGS += /utf-8
