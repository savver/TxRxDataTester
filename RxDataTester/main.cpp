#include "mainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QString>

/**
 * @brief Runs the RxDataTester application.
 * @param argc Number of command-line arguments.
 * @param argv Array of command-line arguments.
 * @return Exit code returned by the Qt event loop.
 * @detail Creates QApplication, configures the QSettings identifiers and version 1.1,
 *         shows the main window, and enters the Qt event loop.
 */
int main(int argc, char *argv[])
{
    QApplication application(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("RxDataTester"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("rxdatatester.local"));
    QCoreApplication::setApplicationName(QStringLiteral("RxDataTester"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.1"));

    MainWindow mainWindow;
    mainWindow.show();

    return application.exec();
}
