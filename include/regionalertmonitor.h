#ifndef REGIONALERTMONITOR_H
#define REGIONALERTMONITOR_H

#include "config.h"
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include <Windows.h>

class ThumbnailWidget;

class RegionAlertMonitor : public QObject {
  Q_OBJECT

public:
  explicit RegionAlertMonitor(QObject *parent = nullptr);

  void reloadFromConfig();
  void setCharacterWindows(const QHash<QString, HWND> &characterWindows);
  void setCharacterThumbnails(
      const QHash<QString, ThumbnailWidget *> &characterThumbnails);

signals:
  void regionAlertTriggered(const QString &characterName, const QString &ruleId,
                            const QString &label, double scorePercent);

private slots:
  void pollRules();

private:
  struct RuleState {
    QImage baselineFrame;
    int consecutiveFramesAboveThreshold = 0;
    qint64 cooldownUntilMs = 0;
    int consecutiveCaptureFailures = 0;
  };

  void updateTimerState();
  void noteCaptureFailure(const QString &ruleKey);
  void resetRuleState(const QString &ruleKey);
  void writeDebugLog(const QString &message);
  void writeComparisonDebugImage(const QString &ruleKey,
                                 const QString &characterName,
                                 const QImage &baselineFrame,
                                 const QImage &currentFrame, double score,
                                 int threshold, bool isAboveThreshold,
                                 bool inCooldown, bool triggered);
  QString debugOutputDirectoryPath();
  static QString effectiveRuleKey(const RegionAlertRule &rule);
  static QRect regionToPixels(const QRectF &normalizedRegion,
                              const QSize &sourceSize);
  static QRectF mapSourceRegionToThumbnailRegion(const QRectF &sourceRegion,
                                                 const QRectF &thumbnailCrop,
                                                 const QSize &sourceClientSize,
                                                 const QSize &thumbnailSize);
  static bool captureClientArea(HWND hwnd, QImage *outImage,
                                QString *outCaptureMethod = nullptr,
                                bool allowSolidBlack = false,
                                bool preferScreenCapture = false,
                                bool allowPrintWindow = true,
                                bool rejectLowContrast = true,
                                bool allowClientDc = true);
  static QImage preprocessForDiff(const QImage &input);
  static double changedPercent(const QImage &previous, const QImage &current);

  QTimer m_pollTimer;
  bool m_enabled = false;
  int m_pollIntervalMs = Config::DEFAULT_REGION_ALERTS_POLL_INTERVAL_MS;
  int m_cooldownMs = Config::DEFAULT_REGION_ALERTS_COOLDOWN_MS;
  QVector<RegionAlertRule> m_rules;
  QHash<QString, HWND> m_characterWindows;
  QHash<QString, QPointer<ThumbnailWidget>> m_characterThumbnails;
  QHash<QString, RuleState> m_ruleStateById;
  bool m_debugOutputEnabled = false;
  quint64 m_debugComparisonSequence = 0;
};

#endif
