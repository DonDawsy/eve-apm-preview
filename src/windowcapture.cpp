#include "windowcapture.h"
#include "config.h"
#include <QDebug>
#include <dwmapi.h>
#include <psapi.h>

static const QString EVEOPREVIEW_PROCESS = QStringLiteral("eveapmpreview");

WindowCapture::WindowCapture() {}

WindowCapture::~WindowCapture() {}

QVector<WindowInfo> WindowCapture::getEVEWindows() {
  static int cleanupCounter = 0;
  if (++cleanupCounter >= 10) {
    cleanupCounter = 0;

    if (!m_cleanupIteratorInitialized ||
        m_cleanupIterator == m_processNameCache.end()) {
      m_cleanupIterator = m_processNameCache.begin();
      m_cleanupIteratorInitialized = true;
    }

    int entriesToCheck = 10;
    while (entriesToCheck > 0 &&
           m_cleanupIterator != m_processNameCache.end()) {
      if (!IsWindow(m_cleanupIterator.key())) {
        m_cleanupIterator = m_processNameCache.erase(m_cleanupIterator);
      } else {
        ++m_cleanupIterator;
      }
      --entriesToCheck;
    }

    if (m_cleanupIterator == m_processNameCache.end()) {
      m_cleanupIterator = m_processNameCache.begin();
    }
  }

  QVector<WindowInfo> windows;
  windows.reserve(40);
  EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&windows));
  return windows;
}

BOOL CALLBACK WindowCapture::enumWindowsProc(HWND hwnd, LPARAM lParam) {
  auto *windows = reinterpret_cast<QVector<WindowInfo> *>(lParam);
  WindowCapture capture;

  QString title, processName;
  if (capture.isEVEWindow(hwnd, title, processName)) {
    qint64 creationTime = capture.getProcessCreationTime(hwnd);
    windows->append(WindowInfo(hwnd, title, processName, creationTime));
  }

  return TRUE;
}

bool WindowCapture::isEVEWindow(HWND hwnd, QString &title,
                                QString &processName) {
  if (!IsWindowVisible(hwnd)) {
    return false;
  }

  title = getWindowTitle(hwnd);
  if (title.isEmpty()) {
    return false;
  }

  processName = getProcessName(hwnd);

  if (processName.toLower().contains(EVEOPREVIEW_PROCESS)) {
    return false;
  }

  QStringList allowedProcessNames = Config::instance().processNames();
  for (const QString &allowedName : allowedProcessNames) {
    if (processName.compare(allowedName, Qt::CaseInsensitive) == 0) {
      return true;
    }
  }

  return false;
}

QString WindowCapture::getWindowTitle(HWND hwnd) {
  wchar_t title[256];
  int length = GetWindowTextW(hwnd, title, sizeof(title) / sizeof(wchar_t));
  if (length > 0) {
    return QString::fromWCharArray(title, length);
  }
  return QString();
}

QString WindowCapture::getProcessName(HWND hwnd) {
  auto it = m_processNameCache.find(hwnd);
  if (it != m_processNameCache.end()) {
    return it.value();
  }

  DWORD processId = 0;
  GetWindowThreadProcessId(hwnd, &processId);

  QString processName;
  HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                FALSE, processId);
  if (hProcess) {
    wchar_t processNameBuffer[MAX_PATH];
    if (GetModuleBaseNameW(hProcess, NULL, processNameBuffer, MAX_PATH)) {
      processName = QString::fromWCharArray(processNameBuffer);
    }
    CloseHandle(hProcess);
  }

  m_processNameCache.insert(hwnd, processName);
  return processName;
}

qint64 WindowCapture::getProcessCreationTime(HWND hwnd) {
  DWORD processId = 0;
  GetWindowThreadProcessId(hwnd, &processId);

  qint64 creationTime = 0;
  HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
  if (hProcess) {
    FILETIME createTime, exitTime, kernelTime, userTime;
    if (GetProcessTimes(hProcess, &createTime, &exitTime, &kernelTime,
                        &userTime)) {
      ULARGE_INTEGER uli;
      uli.LowPart = createTime.dwLowDateTime;
      uli.HighPart = createTime.dwHighDateTime;
      creationTime = (uli.QuadPart / 10000) - 11644473600000LL;
    }
    CloseHandle(hProcess);
  }

  return creationTime;
}

void WindowCapture::activateWindow(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) {
    return;
  }

  // Save the current window placement to preserve maximized state
  WINDOWPLACEMENT placement;
  placement.length = sizeof(WINDOWPLACEMENT);
  GetWindowPlacement(hwnd, &placement);

  bool wasMinimized = (placement.showCmd == SW_SHOWMINIMIZED);
  bool wasMaximized = (placement.showCmd == SW_SHOWMAXIMIZED);

  if (wasMinimized) {
    // Restore minimized window to its previous state (maximized or normal)
    ShowWindowAsync(hwnd, wasMaximized ? SW_SHOWMAXIMIZED : SW_RESTORE);
    // Give the window time to restore before setting focus
    // This prevents input issues where clicks are ignored
    Sleep(30);
  }

  // Get thread information for robust activation
  HWND currentForeground = GetForegroundWindow();
  DWORD foregroundThread = 0;
  if (currentForeground) {
    foregroundThread = GetWindowThreadProcessId(currentForeground, nullptr);
  }

  DWORD thisThread = GetCurrentThreadId();
  BOOL attached = FALSE;
  if (foregroundThread != 0 && foregroundThread != thisThread) {
    // Attach to the foreground thread to bypass focus stealing prevention
    attached = AttachThreadInput(foregroundThread, thisThread, TRUE);
  }

  // First attempt to activate (with thread input attachment for reliability)
  // For maximized windows, use BringWindowToTop; for others, use SetWindowPos
  // to avoid any potential state changes
  if (wasMaximized && !wasMinimized) {
    BringWindowToTop(hwnd);
  } else {
    // Use SetWindowPos with flags to skip animations for faster activation
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOCOPYBITS | SWP_ASYNCWINDOWPOS);
  }

  SetForegroundWindow(hwnd);
  SetFocus(hwnd);

  if (attached) {
    AttachThreadInput(foregroundThread, thisThread, FALSE);
  }

  // Double-check and restore maximized state if it was lost (issue #26)
  if (wasMaximized && !wasMinimized) {
    WINDOWPLACEMENT currentPlacement;
    currentPlacement.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(hwnd, &currentPlacement);

    if (currentPlacement.showCmd != SW_SHOWMAXIMIZED) {
      ShowWindow(hwnd, SW_MAXIMIZE);
    }
  }

  // Final validation: Check if window became foreground (issue #35)
  // If first attempt failed, try one more time with a small delay
  if (GetForegroundWindow() != hwnd) {
    qDebug() << "WindowCapture: First activation attempt failed, retrying "
                "after brief delay (issue #35)";
    Sleep(10);

    // Re-attach thread input and retry
    currentForeground = GetForegroundWindow();
    foregroundThread = 0;
    if (currentForeground) {
      foregroundThread = GetWindowThreadProcessId(currentForeground, nullptr);
    }

    attached = FALSE;
    if (foregroundThread != 0 && foregroundThread != thisThread) {
      attached = AttachThreadInput(foregroundThread, thisThread, TRUE);
    }

    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    if (attached) {
      AttachThreadInput(foregroundThread, thisThread, FALSE);
    }

    // Log if activation still failed
    if (GetForegroundWindow() != hwnd) {
      qDebug() << "WindowCapture: WARNING - Window activation failed after "
                  "all retries (issue #35)";
    }
  }
}

void WindowCapture::clearCache() {
  auto it = m_processNameCache.begin();
  while (it != m_processNameCache.end()) {
    if (!IsWindow(it.key())) {
      it = m_processNameCache.erase(it);
    } else {
      ++it;
    }
  }
}
