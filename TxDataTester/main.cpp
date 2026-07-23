#include "mainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QString>

/**
 * @brief Runs the TxDataTester application.
 * @param argc Number of command-line arguments.
 * @param argv Array of command-line arguments.
 * @return Exit code returned by the Qt event loop.
 * @detail Creates QApplication, sets the QSettings identifiers and version 1.5, shows
 *         the main window, and enters the Qt event loop.
 */
int main(int argc, char *argv[])
{
    QApplication application(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("TxDataTester"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("txdatatester.local"));
    QCoreApplication::setApplicationName(QStringLiteral("TxDataTester"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.5"));

    MainWindow mainWindow;
    mainWindow.show();

    return application.exec();
}
