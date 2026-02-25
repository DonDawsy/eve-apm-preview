#ifndef REGIONALERTMONITOR_H
#define REGIONALERTMONITOR_H

#include "config.h"
#include <QHash>
#include <QImage>
#include <QObject>
#include <QTimer>
#include <QVector>
#include <Windows.h>

class RegionAlertMonitor : public QObject {
  Q_OBJECT

public:
  explicit RegionAlertMonitor(QObject *parent = nullptr);

  void reloadFromConfig();
  void setCharacterWindows(const QHash<QString, HWND> &characterWindows);

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
  static QString effectiveRuleKey(const RegionAlertRule &rule);
  static QRect regionToPixels(const QRectF &normalizedRegion,
                              const QSize &sourceSize);
  static bool captureClientArea(HWND hwnd, QImage *outImage);
  static QImage preprocessForDiff(const QImage &input);
  static double changedPercent(const QImage &previous, const QImage &current);

  QTimer m_pollTimer;
  bool m_enabled = false;
  int m_pollIntervalMs = Config::DEFAULT_REGION_ALERTS_POLL_INTERVAL_MS;
  int m_cooldownMs = Config::DEFAULT_REGION_ALERTS_COOLDOWN_MS;
  QVector<RegionAlertRule> m_rules;
  QHash<QString, HWND> m_characterWindows;
  QHash<QString, RuleState> m_ruleStateById;
};

#endif
