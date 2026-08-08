QT += core gui widgets network serialport

CONFIG += c++11 warn_on
TEMPLATE = app
TARGET = TxDataTester
VERSION = 1.9.0

DEFINES += QT_DEPRECATED_WARNINGS

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    txworker.cpp \
    udptxworker.cpp \
    filegeneratorworker.cpp

HEADERS += \
    mainwindow.h \
    txworker.h \
    udptxworker.h \
    filegeneratorworker.h

FORMS += \
    mainwindow.ui

msvc:QMAKE_CXXFLAGS += /utf-8
