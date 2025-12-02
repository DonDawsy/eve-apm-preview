#ifndef HOTKEYMANAGER_H
#define HOTKEYMANAGER_H

#include <QObject>
#include <QHash>
#include <QVector>
#include <QString>
#include <Windows.h>

struct HotkeyBinding
{
    int keyCode;        
    bool ctrl;
    bool alt;
    bool shift;
    bool enabled;
    
    HotkeyBinding()
        : keyCode(0)
        , ctrl(false)
        , alt(false)
        , shift(false)
        , enabled(false)
    {}
    
    HotkeyBinding(int key, bool c = false, bool a = false, bool s = false, bool en = true)
        : keyCode(key)
        , ctrl(c)
        , alt(a)
        , shift(s)
        , enabled(en)
    {}
    
    UINT getModifiers() const
    {
        UINT mods = 0;
        if (ctrl) mods |= MOD_CONTROL;
        if (alt) mods |= MOD_ALT;
        if (shift) mods |= MOD_SHIFT;
        return mods;
    }
    
    QString toString() const;
    static HotkeyBinding fromString(const QString& str);
    
    bool operator<(const HotkeyBinding& other) const;
    bool operator==(const HotkeyBinding& other) const;
};

struct CharacterHotkey
{
    QString characterName;
    HotkeyBinding binding;
    
    CharacterHotkey() {}
    CharacterHotkey(const QString& name, const HotkeyBinding& b) 
        : characterName(name), binding(b) {}
};

struct CycleGroup
{
    QString groupName;
    QVector<QString> characterNames;
    HotkeyBinding forwardBinding;
    HotkeyBinding backwardBinding;
    bool includeNotLoggedIn;  
    bool noLoop;              
    
    CycleGroup() : includeNotLoggedIn(false), noLoop(false) {}
    CycleGroup(const QString& name) : groupName(name), includeNotLoggedIn(false), noLoop(false) {}
};

class HotkeyManager : public QObject
{
    Q_OBJECT

public:
    explicit HotkeyManager(QObject *parent = nullptr);
    ~HotkeyManager();
    
    static HotkeyManager* instance() { return s_instance; }
    
    bool registerHotkeys();
    void unregisterHotkeys();
    
    void setSuspended(bool suspended);
    bool isSuspended() const { return m_suspended; }
    void toggleSuspended();
    
    void setSuspendHotkey(const HotkeyBinding& binding);
    HotkeyBinding getSuspendHotkey() const { return m_suspendHotkey; }
    
    void setCharacterHotkey(const QString& characterName, const HotkeyBinding& binding);
    void removeCharacterHotkey(const QString& characterName);
    HotkeyBinding getCharacterHotkey(const QString& characterName) const;
    QString getCharacterForHotkey(const HotkeyBinding& binding) const;
    QHash<QString, HotkeyBinding> getAllCharacterHotkeys() const { return m_characterHotkeys; }
    
    void createCycleGroup(const QString& groupName, const QVector<QString>& characterNames,
                         const HotkeyBinding& forwardKey, const HotkeyBinding& backwardKey);
    void createCycleGroup(const CycleGroup& group);  
    void removeCycleGroup(const QString& groupName);
    CycleGroup getCycleGroup(const QString& groupName) const;
    QHash<QString, CycleGroup> getAllCycleGroups() const { return m_cycleGroups; }
    
    void setNotLoggedInCycleHotkeys(const HotkeyBinding& forwardKey, const HotkeyBinding& backwardKey);
    HotkeyBinding getNotLoggedInForwardHotkey() const { return m_notLoggedInForwardHotkey; }
    HotkeyBinding getNotLoggedInBackwardHotkey() const { return m_notLoggedInBackwardHotkey; }
    
    void setNonEVECycleHotkeys(const HotkeyBinding& forwardKey, const HotkeyBinding& backwardKey);
    HotkeyBinding getNonEVEForwardHotkey() const { return m_nonEVEForwardHotkey; }
    HotkeyBinding getNonEVEBackwardHotkey() const { return m_nonEVEBackwardHotkey; }
    
    void setCloseAllClientsHotkey(const HotkeyBinding& binding);
    HotkeyBinding getCloseAllClientsHotkey() const { return m_closeAllClientsHotkey; }
    
    void registerProfileHotkeys();
    void unregisterProfileHotkeys();
    
    QString findHotkeyConflict(const HotkeyBinding& binding, const QString& excludeProfile = QString()) const;
    bool hasHotkeyConflict(const HotkeyBinding& binding, const QString& excludeProfile = QString()) const;
    
    void loadFromConfig();
    void saveToConfig();
    
    void updateCharacterWindows(const QHash<QString, HWND>& characterWindows);
    HWND getWindowForCharacter(const QString& characterName) const;
    QString getCharacterForWindow(HWND hwnd) const;
    
    static bool nativeEventFilter(void* message, long* result);
    
signals:
    void characterHotkeyPressed(QString characterName);
    void namedCycleForwardPressed(QString groupName);
    void namedCycleBackwardPressed(QString groupName);
    void notLoggedInCycleForwardPressed();  
    void notLoggedInCycleBackwardPressed(); 
    void nonEVECycleForwardPressed();       
    void nonEVECycleBackwardPressed();      
    void suspendedChanged(bool suspended);
    void profileSwitchRequested(QString profileName);
    void closeAllClientsRequested();

private:
    QHash<QString, HotkeyBinding> m_characterHotkeys;  
    QHash<int, QString> m_hotkeyIdToCharacter;  
    QHash<int, QString> m_hotkeyIdToCycleGroup;  
    QHash<int, bool> m_hotkeyIdIsForward;  
    QHash<int, int> m_wildcardAliases;  
    QHash<int, QString> m_hotkeyIdToProfile;  
    
    QHash<QString, HWND> m_characterWindows;  
    QHash<QString, CycleGroup> m_cycleGroups;  
    
    HotkeyBinding m_suspendHotkey;  
    int m_suspendHotkeyId;  
    bool m_suspended;  
    
    HotkeyBinding m_notLoggedInForwardHotkey;   
    HotkeyBinding m_notLoggedInBackwardHotkey;  
    int m_notLoggedInForwardHotkeyId;           
    int m_notLoggedInBackwardHotkeyId;          
    
    HotkeyBinding m_nonEVEForwardHotkey;        
    HotkeyBinding m_nonEVEBackwardHotkey;       
    int m_nonEVEForwardHotkeyId;                
    int m_nonEVEBackwardHotkeyId;               
    
    HotkeyBinding m_closeAllClientsHotkey;
    int m_closeAllClientsHotkeyId;
    
    int m_nextHotkeyId;
    
    static HotkeyManager* s_instance;
    
    int generateHotkeyId();
    
    bool registerHotkey(const HotkeyBinding& binding, int& outHotkeyId);
    void unregisterHotkey(int hotkeyId);
};

#endif 
