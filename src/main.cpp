#include "hotkeymanager.h"
#include "mainwindow.h"
#include "version.h"
#include <QApplication>
#include <QLocalSocket>
#include <QMessageBox>
#include <QTimer>
#include <Windows.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

int main(int argc, char *argv[]) {
  int exitCode = 0;

  do {
    QApplication app(argc, argv);

    // Check for command-line URL argument
    QString protocolUrl;
    QStringList args = app.arguments();
    if (args.size() > 1) {
      QString arg = args[1];
      if (arg.startsWith("eveapm://", Qt::CaseInsensitive)) {
        protocolUrl = arg;
        qDebug() << "Protocol URL received from command line:" << protocolUrl;
      }
    }

    HANDLE hMutex =
        CreateMutexW(nullptr, TRUE, L"Global\\EVE-APM-Preview-SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      if (hMutex) {
        CloseHandle(hMutex);
      }

      // If we have a URL, send it to the existing instance
      if (!protocolUrl.isEmpty()) {
        QLocalSocket socket;
        socket.connectToServer("EVE-APM-Preview-IPC");

        if (socket.waitForConnected(1000)) {
          qDebug() << "Connected to existing instance, sending URL...";
          socket.write(protocolUrl.toUtf8());
          socket.waitForBytesWritten(1000);
          socket.disconnectFromServer();
          qDebug() << "URL sent to existing instance";
        } else {
          qWarning() << "Failed to connect to existing instance:"
                     << socket.errorString();
        }
      }

      return 0;
    }

    BOOL compositionEnabled = FALSE;
    if (FAILED(DwmIsCompositionEnabled(&compositionEnabled)) ||
        !compositionEnabled) {
      QMessageBox::critical(
          nullptr, "DWM Required",
          "This application requires Desktop Window Manager "
          "(DWM) to be enabled.\n\n"
          "DWM is available on Windows Vista and later, and is "
          "always enabled on Windows 8+.\n"
          "Please ensure DWM composition is enabled or upgrade "
          "your operating system.");
      if (hMutex) {
        CloseHandle(hMutex);
      }
      return 1;
    }

    app.setApplicationName("EVE-APM Preview");
    app.setApplicationVersion(APP_VERSION);
    app.setOrganizationName("EVEAPMPreview");

    QIcon icon(":/bee.png");
    if (!icon.isNull()) {
      app.setWindowIcon(icon);
    }

    app.setQuitOnLastWindowClosed(false);

    MainWindow manager;

    // If we received a protocol URL, process it after initialization
    if (!protocolUrl.isEmpty()) {
      // Use a timer to process after event loop starts
      QTimer::singleShot(100, [&manager, protocolUrl]() {
        qDebug() << "Processing protocol URL from startup:" << protocolUrl;
        manager.processProtocolUrl(protocolUrl);
      });
    }

    exitCode = app.exec();

    if (hMutex) {
      CloseHandle(hMutex);
    }
  } while (exitCode == 1000);

  return exitCode;
}
