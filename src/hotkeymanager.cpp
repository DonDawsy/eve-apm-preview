#include "hotkeymanager.h"
#include "config.h"
#include "windowcapture.h"
#include <QStringList>
#include <QSettings>
#include <Psapi.h>

HotkeyManager* HotkeyManager::s_instance = nullptr;

HotkeyManager::HotkeyManager(QObject *parent)
    : QObject(parent)
    , m_nextHotkeyId(1000)
    , m_suspendHotkeyId(-1)
    , m_suspended(false)
    , m_notLoggedInForwardHotkeyId(-1)
    , m_notLoggedInBackwardHotkeyId(-1)
    , m_nonEVEForwardHotkeyId(-1)
    , m_nonEVEBackwardHotkeyId(-1)
    , m_closeAllClientsHotkeyId(-1)
{
    s_instance = this;
    loadFromConfig();
}

HotkeyManager::~HotkeyManager()
{
    unregisterHotkeys();
    s_instance = nullptr;
}

bool HotkeyManager::registerHotkey(const HotkeyBinding& binding, int& outHotkeyId)
{
    if (!binding.enabled)
        return false;

    UINT modifiers = MOD_NOREPEAT;  
    if (binding.ctrl) modifiers |= MOD_CONTROL;
    if (binding.alt) modifiers |= MOD_ALT;
    if (binding.shift) modifiers |= MOD_SHIFT;

    int hotkeyId = m_nextHotkeyId++;
    
    bool wildcardMode = Config::instance().wildcardHotkeys();
    
    if (RegisterHotKey(nullptr, hotkeyId, modifiers, binding.keyCode))
    {
        outHotkeyId = hotkeyId;
        
        if (wildcardMode)
        {
            QVector<UINT> additionalMods;
            
            if (!binding.ctrl)
            {
                additionalMods.append(modifiers | MOD_CONTROL);
            }
            
            if (!binding.alt)
            {
                additionalMods.append(modifiers | MOD_ALT);
            }
            
            if (!binding.shift)
            {
                additionalMods.append(modifiers | MOD_SHIFT);
            }
            
            if (!binding.ctrl && !binding.alt)
            {
                additionalMods.append(modifiers | MOD_CONTROL | MOD_ALT);
            }
            if (!binding.ctrl && !binding.shift)
            {
                additionalMods.append(modifiers | MOD_CONTROL | MOD_SHIFT);
            }
            if (!binding.alt && !binding.shift)
            {
                additionalMods.append(modifiers | MOD_ALT | MOD_SHIFT);
            }
            if (!binding.ctrl && !binding.alt && !binding.shift)
            {
                additionalMods.append(modifiers | MOD_CONTROL | MOD_ALT | MOD_SHIFT);
            }
            
            for (UINT extraMod : additionalMods)
            {
                int extraHotkeyId = m_nextHotkeyId++;
                if (RegisterHotKey(nullptr, extraHotkeyId, extraMod, binding.keyCode))
                {
                    m_wildcardAliases.insert(extraHotkeyId, hotkeyId);
                }
            }
        }
        
        return true;
    }
    else
    {
        return false;
    }
}

void HotkeyManager::unregisterHotkey(int hotkeyId)
{
    UnregisterHotKey(nullptr, hotkeyId);
}

bool HotkeyManager::registerHotkeys()
{
    unregisterHotkeys();
    m_hotkeyIdToCharacter.clear();
    m_hotkeyIdToCycleGroup.clear();
    m_hotkeyIdIsForward.clear();

    if (m_suspendHotkey.enabled)
    {
        registerHotkey(m_suspendHotkey, m_suspendHotkeyId);
    }

    if (m_suspended)
    {
        return true;
    }

    for (auto it = m_characterHotkeys.begin(); it != m_characterHotkeys.end(); ++it)
    {
        const QString& characterName = it.key();
        const HotkeyBinding& binding = it.value();
        
        int hotkeyId;
        if (registerHotkey(binding, hotkeyId))
        {
            m_hotkeyIdToCharacter.insert(hotkeyId, characterName);
        }
    }

    for (auto it = m_cycleGroups.begin(); it != m_cycleGroups.end(); ++it)
    {
        const QString& groupName = it.key();
        const CycleGroup& group = it.value();
        
        if (group.forwardBinding.enabled)
        {
            int hotkeyId;
            if (registerHotkey(group.forwardBinding, hotkeyId))
            {
                m_hotkeyIdToCycleGroup.insert(hotkeyId, groupName);
                m_hotkeyIdIsForward.insert(hotkeyId, true);
            }
        }
        
        if (group.backwardBinding.enabled)
        {
            int hotkeyId;
            if (registerHotkey(group.backwardBinding, hotkeyId))
            {
                m_hotkeyIdToCycleGroup.insert(hotkeyId, groupName);
                m_hotkeyIdIsForward.insert(hotkeyId, false);
            }
        }
    }
    
    if (m_notLoggedInForwardHotkey.enabled)
    {
        registerHotkey(m_notLoggedInForwardHotkey, m_notLoggedInForwardHotkeyId);
    }
    
    if (m_notLoggedInBackwardHotkey.enabled)
    {
        registerHotkey(m_notLoggedInBackwardHotkey, m_notLoggedInBackwardHotkeyId);
    }
    
    if (m_nonEVEForwardHotkey.enabled)
    {
        registerHotkey(m_nonEVEForwardHotkey, m_nonEVEForwardHotkeyId);
    }
    
    if (m_nonEVEBackwardHotkey.enabled)
    {
        registerHotkey(m_nonEVEBackwardHotkey, m_nonEVEBackwardHotkeyId);
    }
    
    if (m_closeAllClientsHotkey.enabled)
    {
        registerHotkey(m_closeAllClientsHotkey, m_closeAllClientsHotkeyId);
    }
    
    registerProfileHotkeys();
    
    return true;
}

void HotkeyManager::unregisterHotkeys()
{
    for (int hotkeyId : m_hotkeyIdToCharacter.keys())
    {
        unregisterHotkey(hotkeyId);
    }

    for (int hotkeyId : m_hotkeyIdToCycleGroup.keys())
    {
        unregisterHotkey(hotkeyId);
    }
    
    unregisterProfileHotkeys();
    
    for (int aliasId : m_wildcardAliases.keys())
    {
        unregisterHotkey(aliasId);
    }
    
    if (m_suspendHotkeyId != -1)
    {
        unregisterHotkey(m_suspendHotkeyId);
        m_suspendHotkeyId = -1;
    }
    
    if (m_notLoggedInForwardHotkeyId != -1)
    {
        unregisterHotkey(m_notLoggedInForwardHotkeyId);
        m_notLoggedInForwardHotkeyId = -1;
    }
    
    if (m_notLoggedInBackwardHotkeyId != -1)
    {
        unregisterHotkey(m_notLoggedInBackwardHotkeyId);
        m_notLoggedInBackwardHotkeyId = -1;
    }
    
    if (m_nonEVEForwardHotkeyId != -1)
    {
        unregisterHotkey(m_nonEVEForwardHotkeyId);
        m_nonEVEForwardHotkeyId = -1;
    }
    
    if (m_nonEVEBackwardHotkeyId != -1)
    {
        unregisterHotkey(m_nonEVEBackwardHotkeyId);
        m_nonEVEBackwardHotkeyId = -1;
    }
    
    if (m_closeAllClientsHotkeyId != -1)
    {
        unregisterHotkey(m_closeAllClientsHotkeyId);
        m_closeAllClientsHotkeyId = -1;
    }
    
    m_hotkeyIdToCharacter.clear();
    m_hotkeyIdToCycleGroup.clear();
    m_hotkeyIdIsForward.clear();
    m_wildcardAliases.clear();
}

static bool isForegroundWindowEVEClient()
{
    HWND foregroundWindow = GetForegroundWindow();
    if (!foregroundWindow)
        return false;
    
    DWORD processId = 0;
    GetWindowThreadProcessId(foregroundWindow, &processId);
    
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (!hProcess)
        return false;
    
    wchar_t processNameBuffer[MAX_PATH];
    QString processName;
    if (GetModuleBaseNameW(hProcess, NULL, processNameBuffer, MAX_PATH)) {
        processName = QString::fromWCharArray(processNameBuffer);
    }
    CloseHandle(hProcess);
    
    if (processName.isEmpty())
        return false;
    
    QStringList allowedProcessNames = Config::instance().processNames();
    for (const QString& allowedName : allowedProcessNames) {
        if (processName.compare(allowedName, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    
    return false;
}

bool HotkeyManager::nativeEventFilter(void* message, long* result)
{
    if (!s_instance)
        return false;
        
    MSG* msg = static_cast<MSG*>(message);
    
    if (msg->message == WM_HOTKEY)
    {
        int hotkeyId = static_cast<int>(msg->wParam);
        
        if (s_instance->m_wildcardAliases.contains(hotkeyId))
        {
            hotkeyId = s_instance->m_wildcardAliases.value(hotkeyId);
        }
        
        if (hotkeyId == s_instance->m_suspendHotkeyId)
        {
            s_instance->toggleSuspended();
            return true;
        }
        
        if (s_instance->m_suspended)
        {
            return true;
        }
        
        bool onlyWhenEVEFocused = Config::instance().hotkeysOnlyWhenEVEFocused();
        if (onlyWhenEVEFocused && !isForegroundWindowEVEClient())
        {
            return false;
        }
        
        if (s_instance->m_hotkeyIdToCharacter.contains(hotkeyId))
        {
            QString characterName = s_instance->m_hotkeyIdToCharacter.value(hotkeyId);
            emit s_instance->characterHotkeyPressed(characterName);
            return true;
        }
        
        if (s_instance->m_hotkeyIdToCycleGroup.contains(hotkeyId))
        {
            QString groupName = s_instance->m_hotkeyIdToCycleGroup.value(hotkeyId);
            bool isForward = s_instance->m_hotkeyIdIsForward.value(hotkeyId, true);
            
            if (isForward)
                emit s_instance->namedCycleForwardPressed(groupName);
            else
                emit s_instance->namedCycleBackwardPressed(groupName);
            
            return true;
        }
        
        if (hotkeyId == s_instance->m_notLoggedInForwardHotkeyId)
        {
            emit s_instance->notLoggedInCycleForwardPressed();
            return true;
        }
        
        if (hotkeyId == s_instance->m_notLoggedInBackwardHotkeyId)
        {
            emit s_instance->notLoggedInCycleBackwardPressed();
            return true;
        }
        
        if (hotkeyId == s_instance->m_nonEVEForwardHotkeyId)
        {
            emit s_instance->nonEVECycleForwardPressed();
            return true;
        }
        
        if (hotkeyId == s_instance->m_nonEVEBackwardHotkeyId)
        {
            emit s_instance->nonEVECycleBackwardPressed();
            return true;
        }
        
        if (hotkeyId == s_instance->m_closeAllClientsHotkeyId)
        {
            emit s_instance->closeAllClientsRequested();
            return true;
        }
        
        if (s_instance->m_hotkeyIdToProfile.contains(hotkeyId))
        {
            QString profileName = s_instance->m_hotkeyIdToProfile.value(hotkeyId);
            emit s_instance->profileSwitchRequested(profileName);
            return true;
        }
    }
    
    return false;
}

void HotkeyManager::setSuspended(bool suspended)
{
    if (m_suspended == suspended)
        return;
        
    m_suspended = suspended;
    registerHotkeys();
    emit suspendedChanged(m_suspended);
}

void HotkeyManager::toggleSuspended()
{
    setSuspended(!m_suspended);
}

void HotkeyManager::setSuspendHotkey(const HotkeyBinding& binding)
{
    m_suspendHotkey = binding;
    registerHotkeys();
}

void HotkeyManager::setCharacterHotkey(const QString& characterName, const HotkeyBinding& binding)
{
    m_characterHotkeys.insert(characterName, binding);
    registerHotkeys();
}

void HotkeyManager::removeCharacterHotkey(const QString& characterName)
{
    m_characterHotkeys.remove(characterName);
    registerHotkeys();
}

HotkeyBinding HotkeyManager::getCharacterHotkey(const QString& characterName) const
{
    return m_characterHotkeys.value(characterName, HotkeyBinding());
}

QString HotkeyManager::getCharacterForHotkey(const HotkeyBinding& binding) const
{
    for (auto it = m_characterHotkeys.begin(); it != m_characterHotkeys.end(); ++it)
    {
        if (it.value() == binding)
            return it.key();
    }
    return QString();
}

void HotkeyManager::createCycleGroup(const QString& groupName, const QVector<QString>& characterNames,
                                     const HotkeyBinding& forwardKey, const HotkeyBinding& backwardKey)
{
    CycleGroup group;
    group.groupName = groupName;
    group.characterNames = characterNames;
    group.forwardBinding = forwardKey;
    group.backwardBinding = backwardKey;
    group.includeNotLoggedIn = false;
    
    m_cycleGroups.insert(groupName, group);
    registerHotkeys();
    saveToConfig();
}

void HotkeyManager::createCycleGroup(const CycleGroup& group)
{
    m_cycleGroups.insert(group.groupName, group);
    registerHotkeys();
}

void HotkeyManager::removeCycleGroup(const QString& groupName)
{
    m_cycleGroups.remove(groupName);
    registerHotkeys();
}

CycleGroup HotkeyManager::getCycleGroup(const QString& groupName) const
{
    return m_cycleGroups.value(groupName, CycleGroup());
}

void HotkeyManager::setNotLoggedInCycleHotkeys(const HotkeyBinding& forwardKey, const HotkeyBinding& backwardKey)
{
    m_notLoggedInForwardHotkey = forwardKey;
    m_notLoggedInBackwardHotkey = backwardKey;
    registerHotkeys();
}

void HotkeyManager::setNonEVECycleHotkeys(const HotkeyBinding& forwardKey, const HotkeyBinding& backwardKey)
{
    m_nonEVEForwardHotkey = forwardKey;
    m_nonEVEBackwardHotkey = backwardKey;
    registerHotkeys();
}

void HotkeyManager::setCloseAllClientsHotkey(const HotkeyBinding& binding)
{
    m_closeAllClientsHotkey = binding;
    registerHotkeys();
}

void HotkeyManager::updateCharacterWindows(const QHash<QString, HWND>& characterWindows)
{
    m_characterWindows = characterWindows;
}

HWND HotkeyManager::getWindowForCharacter(const QString& characterName) const
{
    return m_characterWindows.value(characterName, nullptr);
}

QString HotkeyManager::getCharacterForWindow(HWND hwnd) const
{
    for (auto it = m_characterWindows.constBegin(); it != m_characterWindows.constEnd(); ++it) {
        if (it.value() == hwnd) {
            return it.key();
        }
    }
    return QString(); 
}

int HotkeyManager::generateHotkeyId()
{
    return m_nextHotkeyId++;
}

void HotkeyManager::loadFromConfig()
{
    QSettings settings(Config::instance().configFilePath(), QSettings::IniFormat);
    
    settings.beginGroup("hotkeys");
    QVariant suspendVar = settings.value("suspendHotkey");
    
    QString suspendStr;
    if (suspendVar.canConvert<QStringList>()) {
        QStringList parts = suspendVar.toStringList();
        suspendStr = parts.join(',');
    } else {
        suspendStr = suspendVar.toString();
    }
    
    if (!suspendStr.isEmpty())
    {
        m_suspendHotkey = HotkeyBinding::fromString(suspendStr);
    }
    else
    {
        m_suspendHotkey = HotkeyBinding(VK_F12, true, true, true, true);
    }
    settings.endGroup();
    
    m_characterHotkeys.clear();  
    settings.beginGroup("characterHotkeys");
    QStringList characterKeys = settings.childKeys();
    for (const QString& characterName : characterKeys)
    {
        QString bindingStr = settings.value(characterName).toString();
        HotkeyBinding binding = HotkeyBinding::fromString(bindingStr);
        if (binding.enabled)
        {
            m_characterHotkeys.insert(characterName, binding);
        }
    }
    settings.endGroup();
    
    m_cycleGroups.clear();  
    
    settings.beginGroup("cycleGroups");
    QStringList groupKeys = settings.childKeys();
    for (const QString& groupName : groupKeys)
    {
        QVariant groupValue = settings.value(groupName);
        
        QString groupStr;
        if (groupValue.canConvert<QStringList>())
        {
            QStringList parts = groupValue.toStringList();
            groupStr = parts.join(',');
        }
        else
        {
            groupStr = groupValue.toString();
        }
        
        QStringList parts = groupStr.split('|');
        if (parts.size() >= 3)
        {
            CycleGroup group;
            group.groupName = groupName;
            QStringList charNames = parts[0].split(',', Qt::SkipEmptyParts);
            for (const QString& name : charNames)
                group.characterNames.append(name);
            group.forwardBinding = HotkeyBinding::fromString(parts[1]);
            group.backwardBinding = HotkeyBinding::fromString(parts[2]);
            group.includeNotLoggedIn = (parts.size() >= 4) ? (parts[3].toInt() != 0) : false;
            group.noLoop = (parts.size() >= 5) ? (parts[4].toInt() != 0) : false;
            
            m_cycleGroups.insert(groupName, group);
        }
    }
    settings.endGroup();
    
    settings.beginGroup("notLoggedInHotkeys");
    QString forwardStr = settings.value("forward").toString();
    QString backwardStr = settings.value("backward").toString();
    if (!forwardStr.isEmpty())
        m_notLoggedInForwardHotkey = HotkeyBinding::fromString(forwardStr);
    if (!backwardStr.isEmpty())
        m_notLoggedInBackwardHotkey = HotkeyBinding::fromString(backwardStr);
    settings.endGroup();
    
    settings.beginGroup("nonEVEHotkeys");
    QString nonEVEForwardStr = settings.value("forward").toString();
    QString nonEVEBackwardStr = settings.value("backward").toString();
    if (!nonEVEForwardStr.isEmpty())
        m_nonEVEForwardHotkey = HotkeyBinding::fromString(nonEVEForwardStr);
    if (!nonEVEBackwardStr.isEmpty())
        m_nonEVEBackwardHotkey = HotkeyBinding::fromString(nonEVEBackwardStr);
    settings.endGroup();
    
    settings.beginGroup("closeAllHotkeys");
    QString closeAllStr = settings.value("closeAllClients").toString();
    if (!closeAllStr.isEmpty())
        m_closeAllClientsHotkey = HotkeyBinding::fromString(closeAllStr);
    settings.endGroup();
    
    registerHotkeys();
}

void HotkeyManager::saveToConfig()
{
    QSettings settings(Config::instance().configFilePath(), QSettings::IniFormat);
    
    settings.beginGroup("hotkeys");
    settings.setValue("suspendHotkey", m_suspendHotkey.toString());
    settings.endGroup();
    
    settings.beginGroup("characterHotkeys");
    settings.remove("");
    for (auto it = m_characterHotkeys.begin(); it != m_characterHotkeys.end(); ++it)
    {
        settings.setValue(it.key(), it.value().toString());
    }
    settings.endGroup();
    
    settings.beginGroup("cycleGroups");
    settings.remove("");
    for (auto it = m_cycleGroups.begin(); it != m_cycleGroups.end(); ++it)
    {
        const CycleGroup& group = it.value();
        QStringList charNames;
        for (const QString& name : group.characterNames)
            charNames.append(name);
        QString groupStr = charNames.join(',') + "|" +
                          group.forwardBinding.toString() + "|" +
                          group.backwardBinding.toString() + "|" +
                          QString::number(group.includeNotLoggedIn ? 1 : 0) + "|" +
                          QString::number(group.noLoop ? 1 : 0);
        settings.setValue(it.key(), groupStr);
    }
    settings.endGroup();
    
    settings.beginGroup("notLoggedInHotkeys");
    settings.setValue("forward", m_notLoggedInForwardHotkey.toString());
    settings.setValue("backward", m_notLoggedInBackwardHotkey.toString());
    settings.endGroup();
    
    settings.beginGroup("nonEVEHotkeys");
    settings.setValue("forward", m_nonEVEForwardHotkey.toString());
    settings.setValue("backward", m_nonEVEBackwardHotkey.toString());
    settings.endGroup();
    
    settings.beginGroup("closeAllHotkeys");
    settings.setValue("closeAllClients", m_closeAllClientsHotkey.toString());
    settings.endGroup();
    
    settings.sync();
}

bool HotkeyBinding::operator<(const HotkeyBinding& other) const
{
    if (keyCode != other.keyCode) return keyCode < other.keyCode;
    if (ctrl != other.ctrl) return ctrl < other.ctrl;
    if (alt != other.alt) return alt < other.alt;
    if (shift != other.shift) return shift < other.shift;
    return enabled < other.enabled;
}

bool HotkeyBinding::operator==(const HotkeyBinding& other) const
{
    return enabled == other.enabled &&
           keyCode == other.keyCode &&
           ctrl == other.ctrl &&
           alt == other.alt &&
           shift == other.shift;
}

QString HotkeyBinding::toString() const
{
    return QString("%1,%2,%3,%4,%5")
        .arg(enabled ? 1 : 0)
        .arg(keyCode)
        .arg(ctrl ? 1 : 0)
        .arg(alt ? 1 : 0)
        .arg(shift ? 1 : 0);
}

HotkeyBinding HotkeyBinding::fromString(const QString& str)
{
    QStringList parts = str.split(',');
    if (parts.size() == 5)
    {
        HotkeyBinding binding;
        binding.enabled = parts[0].toInt() != 0;
        binding.keyCode = parts[1].toInt();
        binding.ctrl = parts[2].toInt() != 0;
        binding.alt = parts[3].toInt() != 0;
        binding.shift = parts[4].toInt() != 0;
        return binding;
    }
    return HotkeyBinding();
}

void HotkeyManager::registerProfileHotkeys()
{
    QMap<QString, QPair<int, int>> profileHotkeys = Config::instance().getAllProfileHotkeys();
    
    for (auto it = profileHotkeys.constBegin(); it != profileHotkeys.constEnd(); ++it)
    {
        const QString& profileName = it.key();
        int key = it.value().first;
        int modifiers = it.value().second;
        
        HotkeyBinding binding;
        binding.keyCode = key;
        binding.ctrl = (modifiers & Qt::ControlModifier) != 0;
        binding.alt = (modifiers & Qt::AltModifier) != 0;
        binding.shift = (modifiers & Qt::ShiftModifier) != 0;
        binding.enabled = true;
        
        QString conflict = findHotkeyConflict(binding, profileName);
        if (!conflict.isEmpty())
        {
            qWarning() << "Profile hotkey for" << profileName << "conflicts with" << conflict;
        }
        
        int hotkeyId;
        if (registerHotkey(binding, hotkeyId))
        {
            m_hotkeyIdToProfile.insert(hotkeyId, profileName);
            qDebug() << "Registered profile hotkey for" << profileName << "with ID" << hotkeyId;
        }
        else
        {
            qWarning() << "Failed to register profile hotkey for" << profileName << "- hotkey may already be in use";
        }
    }
}

void HotkeyManager::unregisterProfileHotkeys()
{
    for (int hotkeyId : m_hotkeyIdToProfile.keys())
    {
        unregisterHotkey(hotkeyId);
    }
    m_hotkeyIdToProfile.clear();
}

QString HotkeyManager::findHotkeyConflict(const HotkeyBinding& binding, const QString& excludeProfile) const
{
    if (!binding.enabled)
        return QString();
    
    if (m_suspendHotkey.enabled && m_suspendHotkey == binding)
    {
        return "Suspend/Resume Hotkey";
    }
    
    for (auto it = m_characterHotkeys.constBegin(); it != m_characterHotkeys.constEnd(); ++it)
    {
        if (it.value() == binding)
        {
            return QString("Character: %1").arg(it.key());
        }
    }
    
    for (auto it = m_cycleGroups.constBegin(); it != m_cycleGroups.constEnd(); ++it)
    {
        if (it.value().forwardBinding == binding)
        {
            return QString("Cycle Group '%1' (Forward)").arg(it.key());
        }
        if (it.value().backwardBinding == binding)
        {
            return QString("Cycle Group '%1' (Backward)").arg(it.key());
        }
    }
    
    if (m_notLoggedInForwardHotkey.enabled && m_notLoggedInForwardHotkey == binding)
    {
        return "Not Logged In Cycle (Forward)";
    }
    if (m_notLoggedInBackwardHotkey.enabled && m_notLoggedInBackwardHotkey == binding)
    {
        return "Not Logged In Cycle (Backward)";
    }
    
    if (m_nonEVEForwardHotkey.enabled && m_nonEVEForwardHotkey == binding)
    {
        return "Non-EVE Cycle (Forward)";
    }
    if (m_nonEVEBackwardHotkey.enabled && m_nonEVEBackwardHotkey == binding)
    {
        return "Non-EVE Cycle (Backward)";
    }
    
    QMap<QString, QPair<int, int>> profileHotkeys = Config::instance().getAllProfileHotkeys();
    for (auto it = profileHotkeys.constBegin(); it != profileHotkeys.constEnd(); ++it)
    {
        const QString& profileName = it.key();
        
        if (!excludeProfile.isEmpty() && profileName == excludeProfile)
            continue;
        
        int key = it.value().first;
        int modifiers = it.value().second;
        
        HotkeyBinding profileBinding;
        profileBinding.keyCode = key;
        profileBinding.ctrl = (modifiers & Qt::ControlModifier) != 0;
        profileBinding.alt = (modifiers & Qt::AltModifier) != 0;
        profileBinding.shift = (modifiers & Qt::ShiftModifier) != 0;
        profileBinding.enabled = true;
        
        if (profileBinding == binding)
        {
            return QString("Profile: %1").arg(profileName);
        }
    }
    
    return QString();  
}

bool HotkeyManager::hasHotkeyConflict(const HotkeyBinding& binding, const QString& excludeProfile) const
{
    return !findHotkeyConflict(binding, excludeProfile).isEmpty();
}
