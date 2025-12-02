#ifndef HOTKEYCAPTURE_H
#define HOTKEYCAPTURE_H

#include <QLineEdit>
#include <QKeyEvent>
#include <QAbstractNativeEventFilter>
#include <Windows.h>

class HotkeyCapture : public QLineEdit, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit HotkeyCapture(QWidget *parent = nullptr);
    ~HotkeyCapture();
    
    void setHotkey(int keyCode, bool ctrl, bool alt, bool shift);
    void clearHotkey();
    int getKeyCode() const { return m_keyCode; }
    bool getCtrl() const { return m_ctrl; }
    bool getAlt() const { return m_alt; }
    bool getShift() const { return m_shift; }
    
signals:
    void hotkeyChanged();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    bool event(QEvent *event) override;
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    void updateDisplay();
    QString keyCodeToString(int keyCode) const;
    void installKeyboardHook();
    void uninstallKeyboardHook();
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    
    int m_keyCode;
    bool m_ctrl;
    bool m_alt;
    bool m_shift;
    bool m_capturing;
    QString m_savedText;
    
    static HotkeyCapture* s_activeInstance;
    static HHOOK s_keyboardHook;
};

#endif 
