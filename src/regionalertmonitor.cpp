#include "regionalertmonitor.h"
#include <QDateTime>
#include <QDebug>
#include <QSet>
#include <cmath>

namespace {
constexpr int kConsecutiveFramesRequired = 2;
constexpr int kCaptureFailureResetThreshold = 3;
constexpr int kPreprocessSize = 96;
constexpr int kPixelDeltaThreshold = 20;
constexpr int kMinRegionPixelSize = 8;
} // namespace

RegionAlertMonitor::RegionAlertMonitor(QObject *parent) : QObject(parent) {
  m_pollTimer.setSingleShot(false);
  connect(&m_pollTimer, &QTimer::timeout, this, &RegionAlertMonitor::pollRules);
}

void RegionAlertMonitor::reloadFromConfig() {
  const Config &cfg = Config::instance();

  m_enabled = cfg.regionAlertsEnabled();
  m_pollIntervalMs = qBound(100, cfg.regionAlertsPollIntervalMs(), 10000);
  m_cooldownMs = qBound(0, cfg.regionAlertsCooldownMs(), 60000);
  m_rules = cfg.regionAlertRules();

  QSet<QString> activeRuleKeys;
  for (const RegionAlertRule &rule : m_rules) {
    activeRuleKeys.insert(effectiveRuleKey(rule));
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
}

void RegionAlertMonitor::pollRules() {
  if (!m_enabled || m_rules.isEmpty()) {
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
      noteCaptureFailure(ruleKey);
      continue;
    }

    QImage clientImage;
    if (!captureClientArea(hwnd, &clientImage)) {
      noteCaptureFailure(ruleKey);
      continue;
    }

    state.consecutiveCaptureFailures = 0;

    const QRect pixelRegion =
        regionToPixels(rule.regionNormalized, clientImage.size());
    if (pixelRegion.width() < kMinRegionPixelSize ||
        pixelRegion.height() < kMinRegionPixelSize) {
      resetRuleState(ruleKey);
      continue;
    }

    const QImage currentFrame = preprocessForDiff(clientImage.copy(pixelRegion));
    if (currentFrame.isNull()) {
      noteCaptureFailure(ruleKey);
      continue;
    }

    if (state.baselineFrame.isNull() ||
        state.baselineFrame.size() != currentFrame.size()) {
      state.baselineFrame = currentFrame;
      state.consecutiveFramesAboveThreshold = 0;
      continue;
    }

    const double score = changedPercent(state.baselineFrame, currentFrame);
    const int threshold = qBound(1, rule.thresholdPercent, 100);
    const bool isAboveThreshold = score >= static_cast<double>(threshold);

    if (isAboveThreshold) {
      state.consecutiveFramesAboveThreshold++;
    } else {
      state.consecutiveFramesAboveThreshold = 0;
    }

    if (state.consecutiveFramesAboveThreshold >= kConsecutiveFramesRequired &&
        now >= state.cooldownUntilMs) {
      emit regionAlertTriggered(characterName, rule.id, rule.label, score);
      state.cooldownUntilMs = now + m_cooldownMs;
      state.consecutiveFramesAboveThreshold = 0;
    }

    // Keep baseline moving so stable post-change frames don't retrigger.
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
  if (state.consecutiveCaptureFailures >= kCaptureFailureResetThreshold) {
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

bool RegionAlertMonitor::captureClientArea(HWND hwnd, QImage *outImage) {
  if (!outImage || !hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) {
    return false;
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

  bool captured = (PrintWindow(hwnd, memoryDc, PW_CLIENTONLY) == TRUE);
  if (!captured) {
    HDC clientDc = GetDC(hwnd);
    if (clientDc) {
      captured = (BitBlt(memoryDc, 0, 0, width, height, clientDc, 0, 0,
                         SRCCOPY | CAPTUREBLT) == TRUE);
      ReleaseDC(hwnd, clientDc);
    }
  }

  if (captured) {
    QImage wrapped(static_cast<const uchar *>(pixels), width, height,
                   QImage::Format_ARGB32);
    *outImage = wrapped.copy();
  }

  SelectObject(memoryDc, oldBitmap);
  DeleteObject(dib);
  DeleteDC(memoryDc);
  ReleaseDC(nullptr, screenDc);

  return captured && !outImage->isNull();
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
