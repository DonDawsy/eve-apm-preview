#ifndef CHATLOGREADER_H
#define CHATLOGREADER_H

#include <QDir>
#include <QFileSystemWatcher>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QThread>
#include <QTimer>

struct CharacterLocation {
  QString characterName;
  QString systemName;
  qint64 lastUpdate;

  CharacterLocation() : lastUpdate(0) {}
  CharacterLocation(const QString &name, const QString &system, qint64 time)
      : characterName(name), systemName(system), lastUpdate(time) {}
};

/// State for a single log file being monitored via polling
struct LogFileState {
  QString filePath;
  QString characterName;
  qint64 position;          // Current read position in file
  qint64 lastSize;          // Last known file size
  qint64 lastModified;      // Last modified timestamp in ms since epoch
  QString partialLine;      // Incomplete line from previous read
  bool isChatLog;           // true for chatlog, false for gamelog
  bool hadActivityLastPoll; // Had new data in last poll
};

class ChatLogWorker : public QObject {
  Q_OBJECT

public:
  explicit ChatLogWorker(QObject *parent = nullptr);
  ~ChatLogWorker();

  void setCharacterNames(const QStringList &characters);
  void setLogDirectory(const QString &directory);
  void setGameLogDirectory(const QString &directory);
  void setEnableChatLogMonitoring(bool enabled);
  void setEnableGameLogMonitoring(bool enabled);

signals:
  void systemChanged(const QString &characterName, const QString &systemName);
  void characterLoggedIn(const QString &characterName);
  void characterLoggedOut(const QString &characterName);
  void combatEventDetected(const QString &characterName,
                           const QString &eventType, const QString &eventText);
  void combatDetected(const QString &characterName, const QString &combatData);

public slots:
  void startMonitoring();
  void stopMonitoring();
  void refreshMonitoring();
  void pollLogFiles();
  void checkForNewFiles();
  QHash<QString, QString> buildListenerToFileMap(const QDir &dir,
                                                 const QStringList &filters,
                                                 int maxAgeHours = 24);

private:
  QString findLogFileForCharacter(const QString &characterName);
  QString findChatLogFileForCharacter(const QString &characterName);
  QString findGameLogFileForCharacter(const QString &characterName);
  QString extractSystemFromLine(const QString &logLine);
  QString sanitizeSystemName(const QString &system);
  QString extractCharacterFromLogFile(const QString &filePath);
  void parseLogLine(const QString &line, const QString &characterName);
  void scanExistingLogs();
  void handleMiningEvent(const QString &characterName, const QString &ore);
  void onMiningTimeout(const QString &characterName);
  void updateCustomNameCache();

  // Polling-based monitoring methods
  bool readNewLines(LogFileState *state);
  bool shouldParseLine(const QString &line, bool isChatLog);
  void updatePollingRate(bool hadActivity);
  void readInitialState(LogFileState *state);

  QString m_logDirectory;
  QString m_gameLogDirectory;
  QStringList m_characterNames;

  // Polling-based monitoring state
  QHash<QString, LogFileState *> m_logFiles; // filePath -> state
  QTimer *m_pollTimer;
  QTimer *m_scanTimer;
  int m_currentPollInterval;
  int m_activeFilesLastPoll;

  // Character tracking
  QHash<QString, CharacterLocation> m_characterLocations;
  QHash<QString, QString> m_cachedCustomNames;
  QHash<QString, QPair<QString, qint64>> m_fileToCharacterCache;

  QMutex m_mutex;
  bool m_running;
  bool m_enableChatLogMonitoring;
  bool m_enableGameLogMonitoring;
  QDateTime m_lastChatDirScanTime;
  QDateTime m_lastGameDirScanTime;
  QHash<QString, QString> m_cachedChatListenerMap;
  QHash<QString, QString> m_cachedGameListenerMap;
  QHash<QString, QTimer *> m_miningTimers;
  QHash<QString, bool> m_miningActiveState;
  QSet<QString> m_knownChatLogFiles;
  QSet<QString> m_knownGameLogFiles;

  // Polling rate constants
  static constexpr int FAST_POLL_MS =
      500; // Poll every 500ms when files are active
  static constexpr int SLOW_POLL_MS = 1000; // Poll every 1000ms when idle
  static constexpr int SCAN_INTERVAL_MS =
      300000; // Scan for new files every 5 min
};

class ChatLogReader : public QObject {
  Q_OBJECT

public:
  explicit ChatLogReader(QObject *parent = nullptr);
  ~ChatLogReader();

  void setCharacterNames(const QStringList &characters);
  void setLogDirectory(const QString &directory);
  void setGameLogDirectory(const QString &directory);
  void setEnableChatLogMonitoring(bool enabled);
  void setEnableGameLogMonitoring(bool enabled);
  void start();
  void stop();
  void refreshMonitoring();

  QString getSystemForCharacter(const QString &characterName) const;
  bool isMonitoring() const;

signals:
  void systemChanged(const QString &characterName, const QString &systemName);
  void characterLoggedIn(const QString &characterName);
  void characterLoggedOut(const QString &characterName);
  void combatEventDetected(const QString &characterName,
                           const QString &eventType, const QString &eventText);
  void monitoringStarted();
  void monitoringStopped();

private slots:
  void handleSystemChanged(const QString &characterName,
                           const QString &systemName);

private:
  QThread *m_workerThread;
  ChatLogWorker *m_worker;
  mutable QMutex m_locationMutex;
  QHash<QString, QString> m_characterSystems;
  bool m_monitoring;
  QSet<QString> m_lastCharacterSet;
};

#endif
