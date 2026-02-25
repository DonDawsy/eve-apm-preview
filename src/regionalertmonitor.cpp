#include "regionalertmonitor.h"
#include "thumbnailwidget.h"
#include <QDateTime>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QPen>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <cmath>

namespace {
constexpr int kConsecutiveFramesRequired = 2;
constexpr int kCaptureFailureResetThreshold = 3;
constexpr int kPreprocessSize = 96;
constexpr int kPixelDeltaThreshold = 20;
constexpr int kMinRegionPixelSize = 8;
constexpr int kDebugMaxImages = 10;
constexpr qint64 kDebugLogMaxBytes = 2 * 1024 * 1024;

QString sanitizeForFileName(QString input) {
  input = input.trimmed();
  if (input.isEmpty()) {
    return QStringLiteral("unnamed");
  }

  input.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")),
                QStringLiteral("_"));
  if (input.size() > 80) {
    input = input.left(80);
  }
  return input;
}

bool isFrameAlmostSolidBlack(const QImage &image) {
  if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
    return true;
  }

  QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
  if (gray.isNull()) {
    return true;
  }

  constexpr int kTargetSampleCount = 2500;
  const int approxAxisSamples =
      qMax(1, static_cast<int>(std::sqrt(kTargetSampleCount)));
  const int stepX = qMax(1, gray.width() / approxAxisSamples);
  const int stepY = qMax(1, gray.height() / approxAxisSamples);

  qint64 sampleCount = 0;
  qint64 nearBlackCount = 0;
  int minValue = 255;
  int maxValue = 0;

  for (int y = 0; y < gray.height(); y += stepY) {
    const uchar *line = gray.constScanLine(y);
    for (int x = 0; x < gray.width(); x += stepX) {
      const int value = static_cast<int>(line[x]);
      minValue = qMin(minValue, value);
      maxValue = qMax(maxValue, value);
      if (value <= 2) {
        nearBlackCount++;
      }
      sampleCount++;
    }
  }

  if (sampleCount <= 0) {
    return true;
  }

  const double nearBlackRatio =
      static_cast<double>(nearBlackCount) / static_cast<double>(sampleCount);
  const int dynamicRange = maxValue - minValue;
  return nearBlackRatio >= 0.995 && dynamicRange <= 4;
}

bool isFrameLowContrastDark(const QImage &image) {
  if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
    return true;
  }

  QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
  if (gray.isNull()) {
    return true;
  }

  constexpr int kTargetSampleCount = 2500;
  const int approxAxisSamples =
      qMax(1, static_cast<int>(std::sqrt(kTargetSampleCount)));
  const int stepX = qMax(1, gray.width() / approxAxisSamples);
  const int stepY = qMax(1, gray.height() / approxAxisSamples);

  qint64 sampleCount = 0;
  qint64 sum = 0;
  int minValue = 255;
  int maxValue = 0;

  for (int y = 0; y < gray.height(); y += stepY) {
    const uchar *line = gray.constScanLine(y);
    for (int x = 0; x < gray.width(); x += stepX) {
      const int value = static_cast<int>(line[x]);
      minValue = qMin(minValue, value);
      maxValue = qMax(maxValue, value);
      sum += value;
      sampleCount++;
    }
  }

  if (sampleCount <= 0) {
    return true;
  }

  const double mean =
      static_cast<double>(sum) / static_cast<double>(sampleCount);
  const int dynamicRange = maxValue - minValue;
  return mean <= 40.0 && dynamicRange <= 18;
}
} // namespace

RegionAlertMonitor::RegionAlertMonitor(QObject *parent) : QObject(parent) {
  m_pollTimer.setSingleShot(false);
  connect(&m_pollTimer, &QTimer::timeout, this, &RegionAlertMonitor::pollRules);

  QDir debugDir(debugOutputDirectoryPath());
  if (!debugDir.exists()) {
    debugDir.mkpath(QStringLiteral("."));
  }

  const QStringList existingDebugImages =
      debugDir.entryList(QStringList() << QStringLiteral("*.png"), QDir::Files);
  for (const QString &fileName : existingDebugImages) {
    debugDir.remove(fileName);
  }

  writeDebugLog(QStringLiteral("RegionAlertMonitor initialized. Debug output: %1")
                    .arg(debugOutputDirectoryPath()));
}

void RegionAlertMonitor::reloadFromConfig() {
  const Config &cfg = Config::instance();

  m_enabled = cfg.regionAlertsEnabled();
  m_pollIntervalMs = qBound(100, cfg.regionAlertsPollIntervalMs(), 10000);
  m_cooldownMs = qBound(0, cfg.regionAlertsCooldownMs(), 60000);
  m_rules = cfg.regionAlertRules();

  writeDebugLog(
      QStringLiteral(
          "Reload config: enabled=%1 pollIntervalMs=%2 cooldownMs=%3 rules=%4")
          .arg(m_enabled ? QStringLiteral("true") : QStringLiteral("false"))
          .arg(m_pollIntervalMs)
          .arg(m_cooldownMs)
          .arg(m_rules.size()));

  QSet<QString> activeRuleKeys;
  for (const RegionAlertRule &rule : m_rules) {
    const QString key = effectiveRuleKey(rule);
    activeRuleKeys.insert(key);
    writeDebugLog(QStringLiteral(
                      "Rule loaded: key=%1 char='%2' label='%3' region=[%4,%5,%6,%7] "
                      "threshold=%8 enabled=%9")
                      .arg(key, rule.characterName, rule.label)
                      .arg(rule.regionNormalized.x(), 0, 'f', 4)
                      .arg(rule.regionNormalized.y(), 0, 'f', 4)
                      .arg(rule.regionNormalized.width(), 0, 'f', 4)
                      .arg(rule.regionNormalized.height(), 0, 'f', 4)
                      .arg(rule.thresholdPercent)
                      .arg(rule.enabled ? QStringLiteral("true")
                                        : QStringLiteral("false")));
  }

  auto it = m_ruleStateById.begin();
  while (it != m_ruleStateById.end()) {
    if (!activeRuleKeys.contains(it.key())) {
      it = m_ruleStateById.erase(it);
    } else {
      ++it;
    }
  }

  updateTimerState();
}

void RegionAlertMonitor::setCharacterWindows(
    const QHash<QString, HWND> &characterWindows) {
  m_characterWindows = characterWindows;
  writeDebugLog(QStringLiteral("Character windows updated: count=%1")
                    .arg(m_characterWindows.size()));
}

void RegionAlertMonitor::setCharacterThumbnails(
    const QHash<QString, ThumbnailWidget *> &characterThumbnails) {
  m_characterThumbnails.clear();
  for (auto it = characterThumbnails.constBegin();
       it != characterThumbnails.constEnd(); ++it) {
    m_characterThumbnails.insert(it.key(), it.value());
  }
  writeDebugLog(QStringLiteral("Character thumbnails updated: count=%1")
                    .arg(m_characterThumbnails.size()));
}

void RegionAlertMonitor::pollRules() {
  if (!m_enabled || m_rules.isEmpty()) {
    if (!m_enabled) {
      writeDebugLog(QStringLiteral("Poll skipped: monitor disabled"));
    } else if (m_rules.isEmpty()) {
      writeDebugLog(QStringLiteral("Poll skipped: no region alert rules"));
    }
    return;
  }

  const qint64 now = QDateTime::currentMSecsSinceEpoch();

  for (const RegionAlertRule &rule : m_rules) {
    if (!rule.enabled) {
      continue;
    }

    const QString characterName = rule.characterName.trimmed();
    if (characterName.isEmpty()) {
      continue;
    }

    const QString ruleKey = effectiveRuleKey(rule);
    RuleState &state = m_ruleStateById[ruleKey];

    writeDebugLog(
        QStringLiteral("Poll rule: key=%1 char='%2' enabled=%3")
            .arg(ruleKey, characterName,
                 rule.enabled ? QStringLiteral("true") : QStringLiteral("false")));

    QImage clientImage;
    QString captureMethod;
    bool capturedFromThumbnail = false;

    ThumbnailWidget *thumbnail = m_characterThumbnails.value(characterName, nullptr);
    if (!thumbnail) {
      for (auto it = m_characterThumbnails.constBegin();
           it != m_characterThumbnails.constEnd(); ++it) {
        if (it.key().compare(characterName, Qt::CaseInsensitive) == 0) {
          thumbnail = it.value();
          break;
        }
      }
    }

    if (thumbnail) {
      thumbnail->forceUpdate();
      writeDebugLog(QStringLiteral(
                        "Rule %1 thumbnail state: visible=%2 size=%3x%4")
                        .arg(ruleKey)
                        .arg(thumbnail->isVisible() ? QStringLiteral("true")
                                                    : QStringLiteral("false"))
                        .arg(thumbnail->width())
                        .arg(thumbnail->height()));
      const HWND thumbnailHwnd = reinterpret_cast<HWND>(thumbnail->winId());
      QImage thumbnailImage;
      QString thumbnailCaptureMethod;

      if (thumbnailHwnd && IsWindow(thumbnailHwnd) &&
          captureClientArea(thumbnailHwnd, &thumbnailImage,
                            &thumbnailCaptureMethod, false, true)) {
        const QRectF thumbnailCrop = thumbnail->cropRegionNormalized();
        const QRectF mappedRegion =
            mapSourceRegionToThumbnailRegion(rule.regionNormalized, thumbnailCrop,
                                             thumbnailImage.size());
        const QRect mappedPixelRegion =
            regionToPixels(mappedRegion, thumbnailImage.size());

        if (mappedPixelRegion.width() >= kMinRegionPixelSize &&
            mappedPixelRegion.height() >= kMinRegionPixelSize) {
          clientImage = thumbnailImage.copy(mappedPixelRegion);
          captureMethod =
              QStringLiteral("thumbnail_hwnd_capture:%1").arg(thumbnailCaptureMethod);
          capturedFromThumbnail = true;
          writeDebugLog(
              QStringLiteral(
                  "Rule %1 thumbnail mapped region: nx=%2 ny=%3 nw=%4 nh=%5 px=%6 py=%7 pw=%8 ph=%9")
                  .arg(ruleKey)
                  .arg(mappedRegion.x(), 0, 'f', 4)
                  .arg(mappedRegion.y(), 0, 'f', 4)
                  .arg(mappedRegion.width(), 0, 'f', 4)
                  .arg(mappedRegion.height(), 0, 'f', 4)
                  .arg(mappedPixelRegion.x())
                  .arg(mappedPixelRegion.y())
                  .arg(mappedPixelRegion.width())
                  .arg(mappedPixelRegion.height()));
        } else {
          writeDebugLog(
              QStringLiteral(
                  "Rule %1 thumbnail region too small after crop mapping: x=%2 y=%3 w=%4 h=%5")
                  .arg(ruleKey)
                  .arg(mappedPixelRegion.x())
                  .arg(mappedPixelRegion.y())
                  .arg(mappedPixelRegion.width())
                  .arg(mappedPixelRegion.height()));
        }
      } else {
        writeDebugLog(
            QStringLiteral("Rule %1 thumbnail capture failed (hwnd=%2 method=%3)")
                .arg(ruleKey)
                .arg(reinterpret_cast<quintptr>(thumbnailHwnd))
                .arg(thumbnailCaptureMethod));
      }
    }

    if (clientImage.isNull()) {
      HWND hwnd = m_characterWindows.value(characterName, nullptr);
      if (!hwnd) {
        for (auto it = m_characterWindows.constBegin();
             it != m_characterWindows.constEnd(); ++it) {
          if (it.key().compare(characterName, Qt::CaseInsensitive) == 0) {
            hwnd = it.value();
            break;
          }
        }
      }
      if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) {
        writeDebugLog(
            QStringLiteral("Rule %1 capture skipped: no thumbnail and hwnd invalid/not found/minimized")
                .arg(ruleKey));
        noteCaptureFailure(ruleKey);
        continue;
      }

      if (!captureClientArea(hwnd, &clientImage, &captureMethod)) {
        writeDebugLog(
            QStringLiteral(
                "Rule %1 capture failed: captureClientArea returned false (%2)")
                .arg(ruleKey, captureMethod));
        noteCaptureFailure(ruleKey);
        continue;
      }

      const QRect sourcePixelRegion =
          regionToPixels(rule.regionNormalized, clientImage.size());
      if (sourcePixelRegion.width() < kMinRegionPixelSize ||
          sourcePixelRegion.height() < kMinRegionPixelSize) {
        writeDebugLog(
            QStringLiteral(
                "Rule %1 source region too small after conversion: x=%2 y=%3 w=%4 h=%5")
                .arg(ruleKey)
                .arg(sourcePixelRegion.x())
                .arg(sourcePixelRegion.y())
                .arg(sourcePixelRegion.width())
                .arg(sourcePixelRegion.height()));
        resetRuleState(ruleKey);
        continue;
      }

      clientImage = clientImage.copy(sourcePixelRegion);
      captureMethod += QStringLiteral("+source_region");
    }

    writeDebugLog(
        QStringLiteral("Rule %1 capture succeeded via %2 (%3x%4, thumbnail=%5)")
            .arg(ruleKey, captureMethod)
            .arg(clientImage.width())
            .arg(clientImage.height())
            .arg(capturedFromThumbnail ? QStringLiteral("true")
                                       : QStringLiteral("false")));

    state.consecutiveCaptureFailures = 0;

    const QImage currentFrame = preprocessForDiff(clientImage);
    if (currentFrame.isNull()) {
      writeDebugLog(
          QStringLiteral("Rule %1 preprocess failed: current frame is null")
              .arg(ruleKey));
      noteCaptureFailure(ruleKey);
      continue;
    }

    if (state.baselineFrame.isNull() ||
        state.baselineFrame.size() != currentFrame.size()) {
      writeDebugLog(QStringLiteral("Rule %1 baseline initialized").arg(ruleKey));
      state.baselineFrame = currentFrame;
      state.consecutiveFramesAboveThreshold = 0;
      continue;
    }

    const double score = changedPercent(state.baselineFrame, currentFrame);
    const int threshold = qBound(1, rule.thresholdPercent, 100);
    const bool isAboveThreshold = score >= static_cast<double>(threshold);
    const bool inCooldown = now < state.cooldownUntilMs;

    writeComparisonDebugImage(ruleKey, characterName, state.baselineFrame,
                              currentFrame, score, threshold, isAboveThreshold,
                              inCooldown);
    writeDebugLog(
        QStringLiteral(
            "Rule %1 compare: score=%2 threshold=%3 above=%4 inCooldown=%5 consecutive=%6")
            .arg(ruleKey)
            .arg(score, 0, 'f', 3)
            .arg(threshold)
            .arg(isAboveThreshold ? QStringLiteral("true")
                                  : QStringLiteral("false"))
            .arg(inCooldown ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(state.consecutiveFramesAboveThreshold));

    if (isAboveThreshold) {
      state.consecutiveFramesAboveThreshold++;

      if (now >= state.cooldownUntilMs &&
          state.consecutiveFramesAboveThreshold >= kConsecutiveFramesRequired) {
        writeDebugLog(QStringLiteral("Rule %1 triggered alert").arg(ruleKey));
        emit regionAlertTriggered(characterName, rule.id, rule.label, score);
        state.cooldownUntilMs = now + m_cooldownMs;
        state.consecutiveFramesAboveThreshold = 0;

        // Accept the newly changed image as the next baseline.
        state.baselineFrame = currentFrame;
      } else if (now < state.cooldownUntilMs) {
        writeDebugLog(
            QStringLiteral("Rule %1 above threshold but cooling down").arg(ruleKey));
        // Absorb changes while cooling down so a stale diff does not fire later.
        state.consecutiveFramesAboveThreshold = 0;
        state.baselineFrame = currentFrame;
      }

      continue;
    }

    state.consecutiveFramesAboveThreshold = 0;
    state.baselineFrame = currentFrame;
  }
}

void RegionAlertMonitor::updateTimerState() {
  if (m_enabled) {
    m_pollTimer.start(m_pollIntervalMs);
  } else {
    m_pollTimer.stop();
    m_ruleStateById.clear();
  }
}

void RegionAlertMonitor::noteCaptureFailure(const QString &ruleKey) {
  RuleState &state = m_ruleStateById[ruleKey];
  state.consecutiveCaptureFailures++;
  writeDebugLog(QStringLiteral("Rule %1 capture failure count=%2")
                    .arg(ruleKey)
                    .arg(state.consecutiveCaptureFailures));
  if (state.consecutiveCaptureFailures >= kCaptureFailureResetThreshold) {
    writeDebugLog(
        QStringLiteral("Rule %1 capture failures reached reset threshold")
            .arg(ruleKey));
    resetRuleState(ruleKey);
  }
}

void RegionAlertMonitor::resetRuleState(const QString &ruleKey) {
  RuleState &state = m_ruleStateById[ruleKey];
  state.baselineFrame = QImage();
  state.consecutiveFramesAboveThreshold = 0;
  state.cooldownUntilMs = 0;
  state.consecutiveCaptureFailures = 0;
}

void RegionAlertMonitor::writeDebugLog(const QString &message) {
  const QString debugDirPath = debugOutputDirectoryPath();
  QDir debugDir(debugDirPath);
  if (!debugDir.exists()) {
    if (!debugDir.mkpath(QStringLiteral("."))) {
      return;
    }
  }

  const QString logPath = debugDir.filePath(QStringLiteral("region_alert_debug.log"));
  QFileInfo logInfo(logPath);
  if (logInfo.exists() && logInfo.size() > kDebugLogMaxBytes) {
    QFile rotateFile(logPath);
    if (rotateFile.open(QIODevice::WriteOnly | QIODevice::Truncate |
                        QIODevice::Text)) {
      QTextStream rotateStream(&rotateFile);
      rotateStream << "["
                   << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
                   << "] log rotated (exceeded " << kDebugLogMaxBytes
                   << " bytes)\n";
    }
  }

  QFile file(logPath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
    return;
  }

  QTextStream stream(&file);
  stream << "[" << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
         << "] " << message << "\n";
}

void RegionAlertMonitor::writeComparisonDebugImage(
    const QString &ruleKey, const QString &characterName, const QImage &baselineFrame,
    const QImage &currentFrame, double score, int threshold, bool isAboveThreshold,
    bool inCooldown) {
  const QString debugDirPath = debugOutputDirectoryPath();
  QDir debugDir(debugDirPath);
  if (!debugDir.exists()) {
    if (!debugDir.mkpath(QStringLiteral("."))) {
      return;
    }
  }

  QImage baseline = baselineFrame;
  QImage current = currentFrame;
  if (baseline.isNull() || current.isNull()) {
    return;
  }

  baseline = baseline.convertToFormat(QImage::Format_Grayscale8);
  current = current.convertToFormat(QImage::Format_Grayscale8);

  const int scale = 4;
  QImage baselineScaled =
      baseline.scaled(baseline.width() * scale, baseline.height() * scale,
                      Qt::IgnoreAspectRatio, Qt::FastTransformation);
  QImage currentScaled =
      current.scaled(current.width() * scale, current.height() * scale,
                     Qt::IgnoreAspectRatio, Qt::FastTransformation);

  const int padding = 10;
  const int textHeight = 54;
  const int width = baselineScaled.width() + currentScaled.width() + (padding * 3);
  const int height = qMax(baselineScaled.height(), currentScaled.height()) +
                     (padding * 2) + textHeight;

  QImage canvas(width, height, QImage::Format_ARGB32);
  canvas.fill(QColor(20, 20, 20));

  QPainter painter(&canvas);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.drawImage(padding, padding, baselineScaled);
  painter.drawImage((padding * 2) + baselineScaled.width(), padding, currentScaled);

  painter.setPen(QPen(QColor(230, 230, 230)));
  painter.drawText(QRect(padding, baselineScaled.height() + padding + 4, width - (padding * 2),
                         textHeight),
                   Qt::AlignLeft | Qt::TextWordWrap,
                   QStringLiteral(
                       "rule=%1 character=%2 score=%3 threshold=%4 above=%5 cooldown=%6")
                       .arg(ruleKey, characterName)
                       .arg(score, 0, 'f', 3)
                       .arg(threshold)
                       .arg(isAboveThreshold ? QStringLiteral("true")
                                             : QStringLiteral("false"))
                       .arg(inCooldown ? QStringLiteral("true")
                                       : QStringLiteral("false")));
  painter.setPen(QPen(QColor(180, 180, 180)));
  painter.drawText(QRect(padding, baselineScaled.height() + padding + 26, width - (padding * 2),
                         textHeight),
                   Qt::AlignLeft,
                   QStringLiteral("Left=baseline, Right=current (preprocessed frames)"));
  painter.end();

  m_debugComparisonSequence++;
  const QString timestamp =
      QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
  const QString fileName = QStringLiteral("%1_%2_%3.png")
                               .arg(timestamp,
                                    QString::number(m_debugComparisonSequence),
                                    sanitizeForFileName(ruleKey));
  const QString fullPath = debugDir.filePath(fileName);
  if (!canvas.save(fullPath)) {
    writeDebugLog(QStringLiteral("Failed to save debug image: %1").arg(fullPath));
    return;
  }

  m_recentDebugImagePaths.append(fullPath);
  while (m_recentDebugImagePaths.size() > kDebugMaxImages) {
    const QString oldPath = m_recentDebugImagePaths.takeFirst();
    QFile::remove(oldPath);
  }
}

QString RegionAlertMonitor::debugOutputDirectoryPath() {
  QDir workingDir(QDir::currentPath());
  return workingDir.filePath(QStringLiteral("region_alert_debug"));
}

QString RegionAlertMonitor::effectiveRuleKey(const RegionAlertRule &rule) {
  if (!rule.id.trimmed().isEmpty()) {
    return rule.id.trimmed();
  }
  return QStringLiteral("%1|%2|%3|%4|%5|%6")
      .arg(rule.characterName.trimmed(), rule.label.trimmed())
      .arg(rule.regionNormalized.x(), 0, 'f', 4)
      .arg(rule.regionNormalized.y(), 0, 'f', 4)
      .arg(rule.regionNormalized.width(), 0, 'f', 4)
      .arg(rule.regionNormalized.height(), 0, 'f', 4);
}

QRect RegionAlertMonitor::regionToPixels(const QRectF &normalizedRegion,
                                         const QSize &sourceSize) {
  if (sourceSize.width() <= 0 || sourceSize.height() <= 0) {
    return QRect();
  }

  const QRectF normalized = normalizedRegion.normalized();
  const qreal left = qBound(0.0, normalized.left(), 1.0);
  const qreal top = qBound(0.0, normalized.top(), 1.0);
  const qreal right = qBound(0.0, normalized.right(), 1.0);
  const qreal bottom = qBound(0.0, normalized.bottom(), 1.0);

  const int width = sourceSize.width();
  const int height = sourceSize.height();

  const int leftPx =
      qBound(0, static_cast<int>(std::floor(left * width)), width - 1);
  const int topPx =
      qBound(0, static_cast<int>(std::floor(top * height)), height - 1);
  const int rightPx = qBound(leftPx + 1,
                             static_cast<int>(std::ceil(right * width)), width);
  const int bottomPx = qBound(topPx + 1,
                              static_cast<int>(std::ceil(bottom * height)),
                              height);

  return QRect(leftPx, topPx, rightPx - leftPx, bottomPx - topPx);
}

QRectF RegionAlertMonitor::mapSourceRegionToThumbnailRegion(
    const QRectF &sourceRegion, const QRectF &thumbnailCrop,
    const QSize &thumbnailSize) {
  QRectF source = sourceRegion.normalized();
  QRectF crop = thumbnailCrop.normalized();

  const qreal cropLeft = qBound(0.0, crop.left(), 1.0);
  const qreal cropTop = qBound(0.0, crop.top(), 1.0);
  const qreal cropRight = qBound(0.0, crop.right(), 1.0);
  const qreal cropBottom = qBound(0.0, crop.bottom(), 1.0);

  const qreal cropWidth = cropRight - cropLeft;
  const qreal cropHeight = cropBottom - cropTop;
  if (cropWidth <= 0.0001 || cropHeight <= 0.0001) {
    return QRectF(0.0, 0.0, 1.0, 1.0);
  }

  qreal effectiveLeft = cropLeft;
  qreal effectiveTop = cropTop;
  qreal effectiveWidth = cropWidth;
  qreal effectiveHeight = cropHeight;

  if (thumbnailSize.width() > 0 && thumbnailSize.height() > 0) {
    const qreal destinationAspect = static_cast<qreal>(thumbnailSize.width()) /
                                    static_cast<qreal>(thumbnailSize.height());
    const qreal sourceAspect = cropWidth / cropHeight;

    if (sourceAspect > destinationAspect) {
      const qreal targetWidth = cropHeight * destinationAspect;
      const qreal trimX = (cropWidth - targetWidth) * 0.5;
      effectiveLeft += trimX;
      effectiveWidth = targetWidth;
    } else if (sourceAspect < destinationAspect) {
      const qreal targetHeight = cropWidth / destinationAspect;
      const qreal trimY = (cropHeight - targetHeight) * 0.5;
      effectiveTop += trimY;
      effectiveHeight = targetHeight;
    }
  }

  if (effectiveWidth <= 0.0001 || effectiveHeight <= 0.0001) {
    return QRectF(0.0, 0.0, 1.0, 1.0);
  }

  const qreal srcLeft = qBound(0.0, source.left(), 1.0);
  const qreal srcTop = qBound(0.0, source.top(), 1.0);
  const qreal srcRight = qBound(0.0, source.right(), 1.0);
  const qreal srcBottom = qBound(0.0, source.bottom(), 1.0);

  const qreal mappedLeft =
      qBound(0.0, (srcLeft - effectiveLeft) / effectiveWidth, 1.0);
  const qreal mappedTop =
      qBound(0.0, (srcTop - effectiveTop) / effectiveHeight, 1.0);
  const qreal mappedRight =
      qBound(0.0, (srcRight - effectiveLeft) / effectiveWidth, 1.0);
  const qreal mappedBottom =
      qBound(0.0, (srcBottom - effectiveTop) / effectiveHeight, 1.0);

  return QRectF(QPointF(mappedLeft, mappedTop),
                QPointF(mappedRight, mappedBottom))
      .normalized();
}

bool RegionAlertMonitor::captureClientArea(HWND hwnd, QImage *outImage,
                                           QString *outCaptureMethod,
                                           bool allowSolidBlack,
                                           bool preferScreenCapture) {
  if (!outImage || !hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) {
    return false;
  }
  if (outCaptureMethod) {
    *outCaptureMethod = QStringLiteral("none");
  }

  RECT clientRect {};
  if (!GetClientRect(hwnd, &clientRect)) {
    return false;
  }

  const int width = clientRect.right - clientRect.left;
  const int height = clientRect.bottom - clientRect.top;
  if (width <= 0 || height <= 0) {
    return false;
  }

  HDC screenDc = GetDC(nullptr);
  if (!screenDc) {
    return false;
  }

  HDC memoryDc = CreateCompatibleDC(screenDc);
  if (!memoryDc) {
    ReleaseDC(nullptr, screenDc);
    return false;
  }

  BITMAPINFO bmi {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height; // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void *pixels = nullptr;
  HBITMAP dib = CreateDIBSection(memoryDc, &bmi, DIB_RGB_COLORS, &pixels,
                                 nullptr, 0);
  if (!dib || !pixels) {
    if (dib) {
      DeleteObject(dib);
    }
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
    return false;
  }

  HGDIOBJ oldBitmap = SelectObject(memoryDc, dib);

  QString lastCaptureStatus = QStringLiteral("none");

  auto tryConsumeBuffer = [&](const QString &methodName) {
    QImage wrapped(static_cast<const uchar *>(pixels), width, height,
                   QImage::Format_ARGB32);
    QImage candidate = wrapped.copy();
    if (candidate.isNull()) {
      lastCaptureStatus = QStringLiteral("%1:null_frame").arg(methodName);
      return false;
    }
    if (!allowSolidBlack && isFrameAlmostSolidBlack(candidate)) {
      lastCaptureStatus = QStringLiteral("%1:black_frame").arg(methodName);
      return false;
    }
    if (!allowSolidBlack && isFrameLowContrastDark(candidate)) {
      lastCaptureStatus = QStringLiteral("%1:low_contrast_dark_frame")
                              .arg(methodName);
      return false;
    }
    *outImage = candidate;
    lastCaptureStatus = methodName;
    if (outCaptureMethod) {
      *outCaptureMethod = methodName;
    }
    return true;
  };

  auto tryScreenCapture = [&]() -> bool {
    POINT clientOrigin {0, 0};
    if (!ClientToScreen(hwnd, &clientOrigin)) {
      lastCaptureStatus = QStringLiteral("ClientToScreen:api_fail");
      return false;
    }

    const bool captured = (BitBlt(memoryDc, 0, 0, width, height, screenDc,
                                  clientOrigin.x, clientOrigin.y,
                                  SRCCOPY | CAPTUREBLT) == TRUE);
    if (!captured) {
      lastCaptureStatus = QStringLiteral("BitBlt(screenDC_clientRect):api_fail");
      return false;
    }

    return tryConsumeBuffer(QStringLiteral("BitBlt(screenDC_clientRect)"));
  };

  auto tryClientDcCapture = [&]() -> bool {
    HDC clientDc = GetDC(hwnd);
    if (!clientDc) {
      lastCaptureStatus = QStringLiteral("GetDC(hwnd):api_fail");
      return false;
    }

    const bool captured = (BitBlt(memoryDc, 0, 0, width, height, clientDc, 0, 0,
                                  SRCCOPY | CAPTUREBLT) == TRUE);
    ReleaseDC(hwnd, clientDc);
    if (!captured) {
      lastCaptureStatus = QStringLiteral("BitBlt(clientDC):api_fail");
      return false;
    }

    return tryConsumeBuffer(QStringLiteral("BitBlt(clientDC)"));
  };

  auto tryPrintWindowCapture = [&]() -> bool {
    const bool captured = (PrintWindow(hwnd, memoryDc, PW_CLIENTONLY) == TRUE);
    if (!captured) {
      lastCaptureStatus = QStringLiteral("PrintWindow(PW_CLIENTONLY):api_fail");
      return false;
    }

    return tryConsumeBuffer(QStringLiteral("PrintWindow(PW_CLIENTONLY)"));
  };

  bool succeeded = false;
  if (preferScreenCapture) {
    succeeded = tryScreenCapture() || tryClientDcCapture() || tryPrintWindowCapture();
  } else {
    succeeded = tryPrintWindowCapture() || tryClientDcCapture() || tryScreenCapture();
  }

  if (succeeded) {
    SelectObject(memoryDc, oldBitmap);
    DeleteObject(dib);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
    return true;
  }

  SelectObject(memoryDc, oldBitmap);
  DeleteObject(dib);
  DeleteDC(memoryDc);
  ReleaseDC(nullptr, screenDc);

  if (outCaptureMethod) {
    *outCaptureMethod = lastCaptureStatus;
  }
  return false;
}

QImage RegionAlertMonitor::preprocessForDiff(const QImage &input) {
  if (input.isNull()) {
    return QImage();
  }

  return input.convertToFormat(QImage::Format_Grayscale8)
      .scaled(kPreprocessSize, kPreprocessSize, Qt::IgnoreAspectRatio,
              Qt::FastTransformation);
}

double RegionAlertMonitor::changedPercent(const QImage &previous,
                                          const QImage &current) {
  if (previous.isNull() || current.isNull() || previous.size() != current.size()) {
    return 100.0;
  }

  const int width = previous.width();
  const int height = previous.height();
  if (width <= 0 || height <= 0) {
    return 0.0;
  }

  qint64 changedPixels = 0;
  const qint64 totalPixels = static_cast<qint64>(width) * height;

  for (int y = 0; y < height; ++y) {
    const uchar *prevLine = previous.constScanLine(y);
    const uchar *currLine = current.constScanLine(y);
    for (int x = 0; x < width; ++x) {
      const int delta = std::abs(static_cast<int>(currLine[x]) -
                                 static_cast<int>(prevLine[x]));
      if (delta >= kPixelDeltaThreshold) {
        changedPixels++;
      }
    }
  }

  return (static_cast<double>(changedPixels) * 100.0) /
         static_cast<double>(totalPixels);
}
