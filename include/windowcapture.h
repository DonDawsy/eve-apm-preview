#ifndef WINDOWCAPTURE_H
#define WINDOWCAPTURE_H

#include <QVector>
#include <QString>
#include <QHash>
#include <Windows.h>

struct WindowInfo {
    HWND handle;
    quintptr id;
    QString title;
    QString processName;
    qint64 creationTime;
    
    WindowInfo() : handle(nullptr), id(0), creationTime(0) {}
    WindowInfo(HWND h, const QString& t, const QString& p, qint64 ct = 0) 
        : handle(h), id(reinterpret_cast<quintptr>(h)), title(t), processName(p), creationTime(ct) {}
    
    WindowInfo(const WindowInfo& other) = default;
    
    WindowInfo& operator=(const WindowInfo& other) = default;
    
    WindowInfo(WindowInfo&& other) noexcept
        : handle(other.handle)
        , id(other.id)
        , title(std::move(other.title))
        , processName(std::move(other.processName))
        , creationTime(other.creationTime)
    {
        other.handle = nullptr;
        other.id = 0;
        other.creationTime = 0;
    }
    
    WindowInfo& operator=(WindowInfo&& other) noexcept
    {
        if (this != &other) {
            handle = other.handle;
            id = other.id;
            title = std::move(other.title);
            processName = std::move(other.processName);
            creationTime = other.creationTime;
            
            other.handle = nullptr;
            other.id = 0;
            other.creationTime = 0;
        }
        return *this;
    }
};

class WindowCapture
{
public:
    WindowCapture();
    ~WindowCapture();
    
    QVector<WindowInfo> getEVEWindows();
    
    static void activateWindow(HWND hwnd);
    
    void clearCache();

    QString getWindowTitle(HWND hwnd);
    
private:
    static BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam);
    bool isEVEWindow(HWND hwnd, QString& title, QString& processName);
    QString getProcessName(HWND hwnd);
    qint64 getProcessCreationTime(HWND hwnd);

    QHash<HWND, QString> m_processNameCache;

    mutable QHash<HWND, QString>::iterator m_cleanupIterator;
    mutable bool m_cleanupIteratorInitialized = false;
};

#endif 
