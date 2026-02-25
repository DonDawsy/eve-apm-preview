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
#include <QWidget>
#include <cmath>
#include <dwmapi.h>

namespace {
constexpr int kConsecutiveFramesRequired = 2;
constexpr int kCaptureFailureResetThreshold = 3;
constexpr int kPreprocessSize = 96;
constexpr int kPixelDeltaThreshold = 20;
constexpr int kMinRegionPixelSize = 8;
constexpr int kInternalCaptureLongestEdgePx = 192;
constexpr int kInternalCaptureMinShortEdgePx = 48;
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

class InternalRegionAlertCaptureSurface {
public:
  InternalRegionAlertCaptureSurface() = default;

  ~InternalRegionAlertCaptureSurface() { cleanup(); }

  HWND hostHwnd() const {
    return m_hostWidget ? reinterpret_cast<HWND>(m_hostWidget->winId()) : nullptr;
  }

  void cleanup() {
    releaseThumbnail();
    if (m_hostWidget) {
      m_hostWidget->hide();
      delete m_hostWidget;
      m_hostWidget = nullptr;
    }
  }

  bool ensureReady(HWND sourceHwnd, const QRect &sourcePixelRegion,
                   const QSize &captureSize, QString *outStatus) {
    if (!sourceHwnd || !IsWindow(sourceHwnd)) {
      if (outStatus) {
        *outStatus = QStringLiteral("source_window_invalid");
      }
      return false;
    }
    if (sourcePixelRegion.width() <= 0 || sourcePixelRegion.height() <= 0) {
      if (outStatus) {
        *outStatus = QStringLiteral("source_region_invalid");
      }
      return false;
    }
    if (captureSize.width() <= 0 || captureSize.height() <= 0) {
      if (outStatus) {
        *outStatus = QStringLiteral("capture_size_invalid");
      }
      return false;
    }

    if (!ensureHost(outStatus)) {
      return false;
    }

    if (m_hostWidget->size() != captureSize) {
      m_hostWidget->resize(captureSize);
      m_hostWidget->move(-32000, -32000);
      m_hostWidget->show();
    }

    if (!ensureThumbnail(sourceHwnd, outStatus)) {
      return false;
    }

    DWM_THUMBNAIL_PROPERTIES props {};
    props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION |
                    DWM_TNP_VISIBLE | DWM_TNP_OPACITY |
                    DWM_TNP_SOURCECLIENTAREAONLY;
    props.fVisible = TRUE;
    props.opacity = 255;
    props.fSourceClientAreaOnly = TRUE;

    props.rcSource.left = sourcePixelRegion.left();
    props.rcSource.top = sourcePixelRegion.top();
    props.rcSource.right = sourcePixelRegion.left() + sourcePixelRegion.width();
    props.rcSource.bottom = sourcePixelRegion.top() + sourcePixelRegion.height();

    props.rcDestination.left = 0;
    props.rcDestination.top = 0;
    props.rcDestination.right = captureSize.width();
    props.rcDestination.bottom = captureSize.height();

    const HRESULT updateHr = DwmUpdateThumbnailProperties(m_thumbnail, &props);
    if (FAILED(updateHr)) {
      if (outStatus) {
        *outStatus = QStringLiteral("DwmUpdateThumbnailProperties:%1")
                         .arg(static_cast<qint64>(updateHr));
      }
      return false;
    }

    DwmFlush();

    if (outStatus) {
      *outStatus = QStringLiteral("ok");
    }
    return true;
  }

private:
  bool ensureHost(QString *outStatus) {
    if (m_hostWidget) {
      return true;
    }

    m_hostWidget = new QWidget();
    m_hostWidget->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    m_hostWidget->setAttribute(Qt::WA_NativeWindow, true);
    m_hostWidget->setAttribute(Qt::WA_DontShowOnScreen, true);
    m_hostWidget->setAttribute(Qt::WA_ShowWithoutActivating, true);
    m_hostWidget->setAttribute(Qt::WA_NoSystemBackground, true);
    m_hostWidget->setGeometry(-32000, -32000, 64, 64);
    m_hostWidget->show();

    const HWND hwnd = reinterpret_cast<HWND>(m_hostWidget->winId());
    if (!hwnd || !IsWindow(hwnd)) {
      cleanup();
      if (outStatus) {
        *outStatus = QStringLiteral("host_window_create_failed");
      }
      return false;
    }
    return true;
  }

  bool ensureThumbnail(HWND sourceHwnd, QString *outStatus) {
    if (m_thumbnail && m_registeredSource == sourceHwnd) {
      return true;
    }

    releaseThumbnail();

    const HWND destinationHwnd = hostHwnd();
    if (!destinationHwnd || !IsWindow(destinationHwnd)) {
      if (outStatus) {
        *outStatus = QStringLiteral("destination_window_invalid");
      }
      return false;
    }

    const HRESULT registerHr =
        DwmRegisterThumbnail(destinationHwnd, sourceHwnd, &m_thumbnail);
    if (FAILED(registerHr) || !m_thumbnail) {
      m_thumbnail = nullptr;
      m_registeredSource = nullptr;
      if (outStatus) {
        *outStatus = QStringLiteral("DwmRegisterThumbnail:%1")
                         .arg(static_cast<qint64>(registerHr));
      }
      return false;
    }

    m_registeredSource = sourceHwnd;
    return true;
  }

  void releaseThumbnail() {
    if (m_thumbnail) {
      DwmUnregisterThumbnail(m_thumbnail);
      m_thumbnail = nullptr;
    }
    m_registeredSource = nullptr;
  }

  QWidget *m_hostWidget = nullptr;
  HTHUMBNAIL m_thumbnail = nullptr;
  HWND m_registeredSource = nullptr;
};

RegionAlertMonitor::RegionAlertMonitor(QObject *parent) : QObject(parent) {
  m_pollTimer.setSingleShot(false);
  connect(&m_pollTimer, &QTimer::timeout, this, &RegionAlertMonitor::pollRules);
}

RegionAlertMonitor::~RegionAlertMonitor() { clearInternalCaptureSurfaces(); }

void RegionAlertMonitor::reloadFromConfig() {
  const Config &cfg = Config::instance();
  const bool debugOutputEnabled = cfg.regionAlertsDebugOutputEnabled();

  if (debugOutputEnabled && !m_debugOutputEnabled) {
    m_debugComparisonSequence = 0;

    QDir debugDir(debugOutputDirectoryPath());
    if (!debugDir.exists()) {
      debugDir.mkpath(QStringLiteral("."));
    }

    const QStringList existingDebugImages =
        debugDir.entryList(QStringList() << QStringLiteral("*.png"), QDir::Files);
    for (const QString &fileName : existingDebugImages) {
      debugDir.remove(fileName);
    }
  } else if (!debugOutputEnabled && m_debugOutputEnabled) {
    m_debugComparisonSequence = 0;
  }

  m_debugOutputEnabled = debugOutputEnabled;

  m_enabled = cfg.regionAlertsEnabled();
  m_pollIntervalMs = qBound(100, cfg.regionAlertsPollIntervalMs(), 10000);
  m_cooldownMs = qBound(0, cfg.regionAlertsCooldownMs(), 60000);
  m_rules = cfg.regionAlertRules();

  writeDebugLog(
      QStringLiteral("Reload config: enabled=%1 pollIntervalMs=%2 cooldownMs=%3 "
                     "rules=%4 debugOutput=%5")
          .arg(m_enabled ? QStringLiteral("true") : QStringLiteral("false"))
          .arg(m_pollIntervalMs)
          .arg(m_cooldownMs)
          .arg(m_rules.size())
          .arg(m_debugOutputEnabled ? QStringLiteral("true")
                                    : QStringLiteral("false")));

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

  QSet<QString> activeCharacterKeys;
  for (const RegionAlertRule &rule : m_rules) {
    if (!rule.enabled) {
      continue;
    }
    const QString normalizedCharacter = normalizeCharacterKey(rule.characterName);
    if (!normalizedCharacter.isEmpty()) {
      activeCharacterKeys.insert(normalizedCharacter);
    }
  }
  pruneStaleInternalCaptureSurfaces(activeCharacterKeys);

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

QString RegionAlertMonitor::normalizeCharacterKey(const QString &characterName) {
  return characterName.trimmed().toLower();
}

QSize RegionAlertMonitor::internalCaptureSizeForRegion(const QSize &regionSize) {
  const int sourceWidth = qMax(1, regionSize.width());
  const int sourceHeight = qMax(1, regionSize.height());

  if (sourceWidth >= sourceHeight) {
    int targetHeight = static_cast<int>(std::lround(
        static_cast<double>(kInternalCaptureLongestEdgePx) *
        static_cast<double>(sourceHeight) / static_cast<double>(sourceWidth)));
    targetHeight = qMax(1, targetHeight);
    targetHeight = qMax(kInternalCaptureMinShortEdgePx, targetHeight);
    return QSize(kInternalCaptureLongestEdgePx, targetHeight);
  }

  int targetWidth = static_cast<int>(std::lround(
      static_cast<double>(kInternalCaptureLongestEdgePx) *
      static_cast<double>(sourceWidth) / static_cast<double>(sourceHeight)));
  targetWidth = qMax(1, targetWidth);
  targetWidth = qMax(kInternalCaptureMinShortEdgePx, targetWidth);
  return QSize(targetWidth, kInternalCaptureLongestEdgePx);
}

InternalRegionAlertCaptureSurface *
RegionAlertMonitor::ensureInternalCaptureSurface(const QString &characterKey) {
  const QString normalizedKey = normalizeCharacterKey(characterKey);
  if (normalizedKey.isEmpty()) {
    return nullptr;
  }

  auto it = m_internalCaptureSurfacesByCharacter.find(normalizedKey);
  if (it != m_internalCaptureSurfacesByCharacter.end() && it.value()) {
    return it.value();
  }

  InternalRegionAlertCaptureSurface *surface =
      new InternalRegionAlertCaptureSurface();
  m_internalCaptureSurfacesByCharacter.insert(normalizedKey, surface);
  return surface;
}

void RegionAlertMonitor::pruneStaleInternalCaptureSurfaces(
    const QSet<QString> &activeCharacterKeys) {
  auto it = m_internalCaptureSurfacesByCharacter.begin();
  while (it != m_internalCaptureSurfacesByCharacter.end()) {
    if (!activeCharacterKeys.contains(it.key())) {
      delete it.value();
      it = m_internalCaptureSurfacesByCharacter.erase(it);
    } else {
      ++it;
    }
  }
}

void RegionAlertMonitor::clearInternalCaptureSurfaces() {
  for (auto it = m_internalCaptureSurfacesByCharacter.begin();
       it != m_internalCaptureSurfacesByCharacter.end(); ++it) {
    delete it.value();
  }
  m_internalCaptureSurfacesByCharacter.clear();
}

bool RegionAlertMonitor::captureFromInternalCroppedThumbnail(
    const QString &ruleKey, const QString &characterName, HWND sourceHwnd,
    const QRectF &regionNormalized, const QSize &sourceClientSize,
    QImage *outImage, QString *outCaptureMethod) {
  if (!outImage) {
    if (outCaptureMethod) {
      *outCaptureMethod = QStringLiteral("output_image_missing");
    }
    return false;
  }

  *outImage = QImage();
  if (outCaptureMethod) {
    *outCaptureMethod = QStringLiteral("internal_capture_unset");
  }

  if (!sourceHwnd || !IsWindow(sourceHwnd) || IsIconic(sourceHwnd)) {
    if (outCaptureMethod) {
      *outCaptureMethod = QStringLiteral("source_window_unavailable");
    }
    return false;
  }

  QSize effectiveSourceSize = sourceClientSize;
  if (effectiveSourceSize.width() <= 0 || effectiveSourceSize.height() <= 0) {
    RECT sourceClientRect {};
    if (GetClientRect(sourceHwnd, &sourceClientRect)) {
      effectiveSourceSize = QSize(
          qMax(0, sourceClientRect.right - sourceClientRect.left),
          qMax(0, sourceClientRect.bottom - sourceClientRect.top));
    }
  }
  if (effectiveSourceSize.width() <= 0 || effectiveSourceSize.height() <= 0) {
    if (outCaptureMethod) {
      *outCaptureMethod = QStringLiteral("source_client_size_unavailable");
    }
    return false;
  }

  const QRect sourcePixelRegion =
      regionToPixels(regionNormalized, effectiveSourceSize);
  if (sourcePixelRegion.width() < kMinRegionPixelSize ||
      sourcePixelRegion.height() < kMinRegionPixelSize) {
    if (outCaptureMethod) {
      *outCaptureMethod = QStringLiteral("source_region_too_small");
    }
    return false;
  }

  const QSize captureSize =
      internalCaptureSizeForRegion(sourcePixelRegion.size());
  InternalRegionAlertCaptureSurface *surface =
      ensureInternalCaptureSurface(characterName);
  if (!surface) {
    if (outCaptureMethod) {
      *outCaptureMethod = QStringLiteral("surface_init_failed");
    }
    return false;
  }

  QString surfaceStatus;
  if (!surface->ensureReady(sourceHwnd, sourcePixelRegion, captureSize,
                            &surfaceStatus)) {
    if (outCaptureMethod) {
      *outCaptureMethod =
          QStringLiteral("surface_prepare_failed:%1").arg(surfaceStatus);
    }
    return false;
  }

  QImage capturedImage;
  QString captureStatus;
  const HWND captureHwnd = surface->hostHwnd();
  if (!captureHwnd || !IsWindow(captureHwnd) ||
      !captureClientArea(captureHwnd, &capturedImage, &captureStatus, false,
                        false, false, false, true)) {
    if (outCaptureMethod) {
      *outCaptureMethod =
          QStringLiteral("surface_capture_failed:%1").arg(captureStatus);
    }
    return false;
  }

  if (capturedImage.width() < kMinRegionPixelSize ||
      capturedImage.height() < kMinRegionPixelSize) {
    if (outCaptureMethod) {
      *outCaptureMethod = QStringLiteral("captured_frame_too_small");
    }
    return false;
  }

  *outImage = capturedImage;
  if (outCaptureMethod) {
    *outCaptureMethod = QStringLiteral("internal_cropped_thumbnail:%1:%2x%3")
                            .arg(captureStatus)
                            .arg(capturedImage.width())
                            .arg(capturedImage.height());
  }

  writeDebugLog(QStringLiteral("Rule %1 internal capture prepared: src=%2x%3 "
                               "region=[%4,%5,%6,%7] target=%8x%9")
                    .arg(ruleKey)
                    .arg(effectiveSourceSize.width())
                    .arg(effectiveSourceSize.height())
                    .arg(sourcePixelRegion.x())
                    .arg(sourcePixelRegion.y())
                    .arg(sourcePixelRegion.width())
                    .arg(sourcePixelRegion.height())
                    .arg(captureSize.width())
                    .arg(captureSize.height()));
  return true;
}

bool RegionAlertMonitor::captureFromVisibleThumbnailFallback(
    const QString &ruleKey, const QString &characterName,
    const QRectF &regionNormalized, const QSize &sourceClientSize,
    QImage *outImage, QString *outCaptureMethod) {
  if (!outImage) {
    if (outCaptureMethod) {
      *outCaptureMethod = QStringLiteral("output_image_missing");
    }
    return false;
  }

  *outImage = QImage();
  if (outCaptureMethod) {
    *outCaptureMethod = QStringLiteral("thumbnail_capture_unset");
  }

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

  if (!thumbnail) {
    if (outCaptureMethod) {
      *outCaptureMethod = QStringLiteral("no_thumbnail_widget");
    }
    return false;
  }

  thumbnail->forceUpdate();
  writeDebugLog(QStringLiteral("Rule %1 fallback thumbnail state: visible=%2 size=%3x%4")
                    .arg(ruleKey)
                    .arg(thumbnail->isVisible() ? QStringLiteral("true")
                                                : QStringLiteral("false"))
                    .arg(thumbnail->width())
                    .arg(thumbnail->height()));

  const HWND thumbnailHwnd = reinterpret_cast<HWND>(thumbnail->winId());
  QImage thumbnailImage;
  QString thumbnailCaptureMethod;
  if (!thumbnailHwnd || !IsWindow(thumbnailHwnd) ||
      !captureClientArea(thumbnailHwnd, &thumbnailImage, &thumbnailCaptureMethod,
                        false, true, false, false, false)) {
    if (outCaptureMethod) {
      *outCaptureMethod =
          QStringLiteral("thumbnail_capture_failed:%1").arg(thumbnailCaptureMethod);
    }
    return false;
  }

  const QRectF thumbnailCrop = thumbnail->cropRegionNormalized();
  QSize mappingSourceSize = sourceClientSize;
  if (mappingSourceSize.width() <= 0 || mappingSourceSize.height() <= 0) {
    mappingSourceSize = thumbnailImage.size();
    writeDebugLog(
        QStringLiteral("Rule %1 fallback source client size unavailable; using "
                       "thumbnail size for mapping (%2x%3)")
            .arg(ruleKey)
            .arg(mappingSourceSize.width())
            .arg(mappingSourceSize.height()));
  }

  const QRectF mappedRegion = mapSourceRegionToThumbnailRegion(
      regionNormalized, thumbnailCrop, mappingSourceSize, thumbnailImage.size());
  if (!mappedRegion.isValid() || mappedRegion.width() <= 0.0 ||
      mappedRegion.height() <= 0.0) {
    if (outCaptureMethod) {
      *outCaptureMethod = QStringLiteral("thumbnail_mapping_empty");
    }
    writeDebugLog(
        QStringLiteral("Rule %1 fallback mapping produced empty region: "
                       "sourceClient=%2x%3 thumb=%4x%5 srcRegion=[%6,%7,%8,%9] "
                       "thumbCrop=[%10,%11,%12,%13]")
            .arg(ruleKey)
            .arg(mappingSourceSize.width())
            .arg(mappingSourceSize.height())
            .arg(thumbnailImage.width())
            .arg(thumbnailImage.height())
            .arg(regionNormalized.x(), 0, 'f', 4)
            .arg(regionNormalized.y(), 0, 'f', 4)
            .arg(regionNormalized.width(), 0, 'f', 4)
            .arg(regionNormalized.height(), 0, 'f', 4)
            .arg(thumbnailCrop.x(), 0, 'f', 4)
            .arg(thumbnailCrop.y(), 0, 'f', 4)
            .arg(thumbnailCrop.width(), 0, 'f', 4)
            .arg(thumbnailCrop.height(), 0, 'f', 4));
    return false;
  }

  const QRect mappedPixelRegion = regionToPixels(mappedRegion, thumbnailImage.size());
  if (mappedPixelRegion.width() < kMinRegionPixelSize ||
      mappedPixelRegion.height() < kMinRegionPixelSize) {
    if (outCaptureMethod) {
      *outCaptureMethod = QStringLiteral("thumbnail_mapped_region_too_small");
    }
    writeDebugLog(
        QStringLiteral("Rule %1 fallback mapped region too small: x=%2 y=%3 w=%4 h=%5")
            .arg(ruleKey)
            .arg(mappedPixelRegion.x())
            .arg(mappedPixelRegion.y())
            .arg(mappedPixelRegion.width())
            .arg(mappedPixelRegion.height()));
    return false;
  }

  *outImage = thumbnailImage.copy(mappedPixelRegion);
  if (outCaptureMethod) {
    *outCaptureMethod =
        QStringLiteral("thumbnail_hwnd_capture:%1").arg(thumbnailCaptureMethod);
  }

  writeDebugLog(QStringLiteral("Rule %1 fallback mapped region: sourceClient=%2x%3 "
                               "thumb=%4x%5 nx=%6 ny=%7 nw=%8 nh=%9 px=%10 py=%11 "
                               "pw=%12 ph=%13")
                    .arg(ruleKey)
                    .arg(mappingSourceSize.width())
                    .arg(mappingSourceSize.height())
                    .arg(thumbnailImage.width())
                    .arg(thumbnailImage.height())
                    .arg(mappedRegion.x(), 0, 'f', 4)
                    .arg(mappedRegion.y(), 0, 'f', 4)
                    .arg(mappedRegion.width(), 0, 'f', 4)
                    .arg(mappedRegion.height(), 0, 'f', 4)
                    .arg(mappedPixelRegion.x())
                    .arg(mappedPixelRegion.y())
                    .arg(mappedPixelRegion.width())
                    .arg(mappedPixelRegion.height()));
  return true;
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

  QSet<QString> activeCharacterKeys;
  for (const RegionAlertRule &rule : m_rules) {
    if (!rule.enabled) {
      continue;
    }

    const QString characterKey = normalizeCharacterKey(rule.characterName);
    if (!characterKey.isEmpty()) {
      activeCharacterKeys.insert(characterKey);
    }
  }
  pruneStaleInternalCaptureSurfaces(activeCharacterKeys);

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
    HWND sourceHwnd = m_characterWindows.value(characterName, nullptr);
    if (!sourceHwnd) {
      for (auto it = m_characterWindows.constBegin();
           it != m_characterWindows.constEnd(); ++it) {
        if (it.key().compare(characterName, Qt::CaseInsensitive) == 0) {
          sourceHwnd = it.value();
          break;
        }
      }
    }
    QSize sourceClientSize;
    if (sourceHwnd && IsWindow(sourceHwnd)) {
      RECT sourceClientRect {};
      if (GetClientRect(sourceHwnd, &sourceClientRect)) {
        sourceClientSize = QSize(qMax(0, sourceClientRect.right - sourceClientRect.left),
                                 qMax(0, sourceClientRect.bottom - sourceClientRect.top));
      }
    }

    writeDebugLog(
        QStringLiteral("Poll rule: key=%1 char='%2' enabled=%3")
            .arg(ruleKey, characterName,
                 rule.enabled ? QStringLiteral("true") : QStringLiteral("false")));

    QImage clientImage;
    QString captureMethod;
    bool capturedFromThumbnail = false;
    QString internalCaptureMethod;
    if (captureFromInternalCroppedThumbnail(ruleKey, characterName, sourceHwnd,
                                            rule.regionNormalized,
                                            sourceClientSize, &clientImage,
                                            &internalCaptureMethod)) {
      captureMethod = internalCaptureMethod;
      capturedFromThumbnail = true;
    } else {
      writeDebugLog(QStringLiteral("Rule %1 internal cropped thumbnail capture failed: %2")
                        .arg(ruleKey, internalCaptureMethod));
    }

    if (clientImage.isNull()) {
      QString fallbackCaptureMethod;
      if (captureFromVisibleThumbnailFallback(ruleKey, characterName,
                                              rule.regionNormalized,
                                              sourceClientSize, &clientImage,
                                              &fallbackCaptureMethod)) {
        captureMethod = fallbackCaptureMethod;
        capturedFromThumbnail = true;
      } else {
        writeDebugLog(
            QStringLiteral("Rule %1 fallback visible thumbnail capture failed: %2")
                .arg(ruleKey, fallbackCaptureMethod));
        noteCaptureFailure(ruleKey);
        continue;
      }
    }

    writeDebugLog(
        QStringLiteral("Rule %1 capture succeeded via %2 (%3x%4, thumbnail=%5)")
            .arg(ruleKey, captureMethod)
            .arg(clientImage.width())
            .arg(clientImage.height())
            .arg(capturedFromThumbnail ? QStringLiteral("true")
                                       : QStringLiteral("false")));

    const QString capturePipelineKey = captureMethod.trimmed();
    if (state.capturePipelineKey != capturePipelineKey) {
      writeDebugLog(QStringLiteral("Rule %1 capture pipeline changed: '%2' -> '%3' "
                                   "(baseline reset)")
                        .arg(ruleKey, state.capturePipelineKey,
                             capturePipelineKey));
      state.capturePipelineKey = capturePipelineKey;
      state.baselineFrame = QImage();
      state.consecutiveFramesAboveThreshold = 0;
    }

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
    bool triggered = false;

    if (isAboveThreshold) {
      state.consecutiveFramesAboveThreshold++;
      triggered = now >= state.cooldownUntilMs &&
                  state.consecutiveFramesAboveThreshold >=
                      kConsecutiveFramesRequired;
    }

    if (score > 0.0) {
      writeComparisonDebugImage(ruleKey, characterName, state.baselineFrame,
                                currentFrame, score, threshold, isAboveThreshold,
                                inCooldown, triggered);
    }
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
      if (triggered) {
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
    clearInternalCaptureSurfaces();
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
  state.capturePipelineKey.clear();
}

void RegionAlertMonitor::writeDebugLog(const QString &message) {
  if (!m_debugOutputEnabled) {
    return;
  }

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
    bool inCooldown, bool triggered) {
  if (!m_debugOutputEnabled) {
    return;
  }

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
  const QString fileName =
      QStringLiteral("%1%2_%3_%4.png")
          .arg(triggered ? QStringLiteral("triggered_") : QStringLiteral(""))
          .arg(timestamp)
          .arg(QString::number(m_debugComparisonSequence))
          .arg(sanitizeForFileName(ruleKey));
  const QString fullPath = debugDir.filePath(fileName);
  if (!canvas.save(fullPath)) {
    writeDebugLog(QStringLiteral("Failed to save debug image: %1").arg(fullPath));
    return;
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
    const QSize &sourceClientSize, const QSize &thumbnailSize) {
  if (sourceClientSize.width() <= 0 || sourceClientSize.height() <= 0 ||
      thumbnailSize.width() <= 0 || thumbnailSize.height() <= 0) {
    return QRectF();
  }

  const QRect sourcePixelRegion = regionToPixels(sourceRegion, sourceClientSize);
  if (sourcePixelRegion.width() <= 0 || sourcePixelRegion.height() <= 0) {
    return QRectF();
  }

  const QRectF normalizedCrop = thumbnailCrop.normalized();
  const qreal cropLeftNorm = qBound(0.0, normalizedCrop.left(), 1.0);
  const qreal cropTopNorm = qBound(0.0, normalizedCrop.top(), 1.0);
  const qreal cropRightNorm = qBound(0.0, normalizedCrop.right(), 1.0);
  const qreal cropBottomNorm = qBound(0.0, normalizedCrop.bottom(), 1.0);

  const int sourceWidth = sourceClientSize.width();
  const int sourceHeight = sourceClientSize.height();

  int cropLeft = qBound(0, static_cast<int>(std::floor(cropLeftNorm * sourceWidth)),
                        sourceWidth - 1);
  int cropTop = qBound(0, static_cast<int>(std::floor(cropTopNorm * sourceHeight)),
                       sourceHeight - 1);
  int cropRight = qBound(cropLeft + 1,
                         static_cast<int>(std::ceil(cropRightNorm * sourceWidth)),
                         sourceWidth);
  int cropBottom = qBound(cropTop + 1,
                          static_cast<int>(std::ceil(cropBottomNorm * sourceHeight)),
                          sourceHeight);

  int cropWidth = qMax(1, cropRight - cropLeft);
  int cropHeight = qMax(1, cropBottom - cropTop);

  const qreal sourceAspect =
      static_cast<qreal>(cropWidth) / static_cast<qreal>(cropHeight);
  const qreal destinationAspect =
      static_cast<qreal>(thumbnailSize.width()) /
      static_cast<qreal>(thumbnailSize.height());

  if (sourceAspect > destinationAspect) {
    int targetWidth =
        qMax(1, static_cast<int>(std::lround(cropHeight * destinationAspect)));
    targetWidth = qMin(targetWidth, cropWidth);
    int trimX = (cropWidth - targetWidth) / 2;
    cropLeft += trimX;
    cropRight = cropLeft + targetWidth;
  } else if (sourceAspect < destinationAspect) {
    int targetHeight =
        qMax(1, static_cast<int>(std::lround(cropWidth / destinationAspect)));
    targetHeight = qMin(targetHeight, cropHeight);
    int trimY = (cropHeight - targetHeight) / 2;
    cropTop += trimY;
    cropBottom = cropTop + targetHeight;
  }

  const int effectiveWidth = qMax(1, cropRight - cropLeft);
  const int effectiveHeight = qMax(1, cropBottom - cropTop);

  const int sourceLeft = sourcePixelRegion.x();
  const int sourceTop = sourcePixelRegion.y();
  const int sourceRight = sourcePixelRegion.x() + sourcePixelRegion.width();
  const int sourceBottom = sourcePixelRegion.y() + sourcePixelRegion.height();

  const int overlapLeft = qMax(sourceLeft, cropLeft);
  const int overlapTop = qMax(sourceTop, cropTop);
  const int overlapRight = qMin(sourceRight, cropRight);
  const int overlapBottom = qMin(sourceBottom, cropBottom);

  if (overlapRight <= overlapLeft || overlapBottom <= overlapTop) {
    return QRectF();
  }

  const qreal mappedLeft = qBound(
      0.0, static_cast<qreal>(overlapLeft - cropLeft) / effectiveWidth, 1.0);
  const qreal mappedTop = qBound(
      0.0, static_cast<qreal>(overlapTop - cropTop) / effectiveHeight, 1.0);
  const qreal mappedRight = qBound(
      0.0, static_cast<qreal>(overlapRight - cropLeft) / effectiveWidth, 1.0);
  const qreal mappedBottom = qBound(
      0.0, static_cast<qreal>(overlapBottom - cropTop) / effectiveHeight, 1.0);

  QRectF mapped(QPointF(mappedLeft, mappedTop), QPointF(mappedRight, mappedBottom));
  mapped = mapped.normalized();
  if (!mapped.isValid() || mapped.width() <= 0.0 || mapped.height() <= 0.0) {
    return QRectF();
  }
  return mapped;
}

bool RegionAlertMonitor::captureClientArea(HWND hwnd, QImage *outImage,
                                           QString *outCaptureMethod,
                                           bool allowSolidBlack,
                                           bool preferScreenCapture,
                                           bool allowPrintWindow,
                                           bool rejectLowContrast,
                                           bool allowClientDc) {
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
    if (rejectLowContrast && isFrameLowContrastDark(candidate)) {
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
    succeeded = tryScreenCapture();
    if (!succeeded && allowClientDc) {
      succeeded = tryClientDcCapture();
    }
    if (!succeeded && allowPrintWindow) {
      succeeded = tryPrintWindowCapture();
    }
  } else {
    if (allowClientDc) {
      succeeded = tryClientDcCapture();
    }
    if (!succeeded) {
      succeeded = tryScreenCapture();
    }
    if (!succeeded && allowPrintWindow) {
      succeeded = tryPrintWindowCapture();
    }
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
