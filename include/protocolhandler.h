#ifndef PROTOCOLHANDLER_H
#define PROTOCOLHANDLER_H

#include <QObject>
#include <QString>

/// Handles custom URL protocol (eveapm://) for external application control
///
/// This class provides functionality to:
/// - Parse eveapm:// URLs for profile and character actions
/// - Register the protocol with Windows registry
/// - Emit signals for requested actions
class ProtocolHandler : public QObject {
  Q_OBJECT

public:
  explicit ProtocolHandler(QObject *parent = nullptr);
  ~ProtocolHandler();

  /// Parse and handle a protocol URL
  /// @param url The URL to parse (e.g., "eveapm://profile/incursions")
  /// @return true if URL was valid and handled, false otherwise
  bool handleUrl(const QString &url);

  /// Register the eveapm:// protocol in Windows registry
  /// @return true if registration succeeded, false otherwise
  bool registerProtocol();

  /// Unregister the eveapm:// protocol from Windows registry
  /// @return true if unregistration succeeded, false otherwise
  bool unregisterProtocol();

  /// Check if the protocol is currently registered
  /// @return true if registered, false otherwise
  bool isProtocolRegistered() const;

  /// Get registry value helper (public for verification)
  /// @param key Registry key path
  /// @param valueName Value name (empty for default)
  /// @param defaultValue Default value if key doesn't exist
  /// @return Registry value or default
  QString getRegistryValue(const QString &key, const QString &valueName,
                           const QString &defaultValue = QString()) const;

signals:
  /// Emitted when a profile switch is requested via URL
  /// @param profileName The name of the profile to switch to
  void profileRequested(const QString &profileName);

  /// Emitted when character activation is requested via URL
  /// @param characterName The name of the character to activate
  void characterRequested(const QString &characterName);

  /// Emitted when hotkey suspend is requested
  void hotkeySuspendRequested();

  /// Emitted when hotkey resume is requested
  void hotkeyResumeRequested();

  /// Emitted when thumbnail hide is requested
  void thumbnailHideRequested();

  /// Emitted when thumbnail show is requested
  void thumbnailShowRequested();

  /// Emitted when config dialog should be opened
  void configOpenRequested();

  /// Emitted when an invalid URL is received
  /// @param url The invalid URL
  /// @param reason Description of why the URL is invalid
  void invalidUrl(const QString &url, const QString &reason);

private:
  /// Parse URL components and emit appropriate signal
  /// @param url QUrl object to parse
  /// @return true if successfully parsed and signal emitted
  bool parseAndEmit(const QUrl &url);

  /// Get the full path to the application executable
  /// @return Quoted executable path suitable for registry
  QString getExecutablePath() const;

  /// Validate that a profile name is safe and valid
  /// @param profileName The profile name to validate
  /// @return true if valid
  bool isValidProfileName(const QString &profileName) const;

  /// Validate that a character name is safe and valid
  /// @param characterName The character name to validate
  /// @return true if valid
  bool isValidCharacterName(const QString &characterName) const;

  /// Set registry value helper
  /// @param key Registry key path
  /// @param valueName Value name (empty for default)
  /// @param data Value data
  /// @return true if successful
  bool setRegistryValue(const QString &key, const QString &valueName,
                        const QString &data);

  /// Delete registry key helper
  /// @param key Registry key path to delete
  /// @return true if successful
  bool deleteRegistryKey(const QString &key);
};

#endif // PROTOCOLHANDLER_H
