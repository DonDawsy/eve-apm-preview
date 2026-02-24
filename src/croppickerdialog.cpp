#include "croppickerdialog.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>
#include <dwmapi.h>

namespace {
QRectF normalizeOrFull(const QRectF &crop) {
  QRectF normalized = crop.normalized();

  qreal left = qBound(0.0, normalized.left(), 1.0);
  qreal top = qBound(0.0, normalized.top(), 1.0);
  qreal right = qBound(0.0, normalized.right(), 1.0);
  qreal bottom = qBound(0.0, normalized.bottom(), 1.0);

  QRectF clamped(QPointF(left, top), QPointF(right, bottom));
  clamped = clamped.normalized();

  static constexpr qreal kMinSize = 0.0001;
  if (!clamped.isValid() || clamped.width() < kMinSize ||
      clamped.height() < kMinSize) {
    return QRectF(0.0, 0.0, 1.0, 1.0);
  }

  return clamped;
}
} // namespace

class CropPreviewWidget : public QWidget {
  Q_OBJECT

public:
  explicit CropPreviewWidget(HWND sourceWindow, QWidget *parent = nullptr)
      : QWidget(parent), m_sourceWindow(sourceWindow) {
    setMouseTracking(true);
    setMinimumSize(720, 405);
  }

  ~CropPreviewWidget() override { cleanupThumbnail(); }

  void setSelectionNormalized(const QRectF &selection) {
    QRectF normalized = normalizeOrFull(selection);
    if (qFuzzyCompare(m_selectionNormalized.x(), normalized.x()) &&
        qFuzzyCompare(m_selectionNormalized.y(), normalized.y()) &&
        qFuzzyCompare(m_selectionNormalized.width(), normalized.width()) &&
        qFuzzyCompare(m_selectionNormalized.height(), normalized.height())) {
      return;
    }

    m_selectionNormalized = normalized;
    emit selectionChanged(m_selectionNormalized);
    update();
  }

  QRectF selectionNormalized() const { return m_selectionNormalized; }

signals:
  void selectionChanged(const QRectF &selection);

protected:
  void showEvent(QShowEvent *event) override {
    QWidget::showEvent(event);
    ensureThumbnailRegistered();
    updateThumbnail();
  }

  void resizeEvent(QResizeEvent *event) override {
    QWidget::resizeEvent(event);
    updateThumbnail();
  }

  void paintEvent(QPaintEvent *event) override {
    QWidget::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QRect preview = previewRect();

    painter.fillRect(rect(), QColor(8, 8, 10));

    if (preview.width() > 0 && preview.height() > 0) {
      QPainterPath outside;
      outside.addRect(rect());
      QPainterPath inside;
      inside.addRect(preview);
      outside = outside.subtracted(inside);
      painter.fillPath(outside, QColor(0, 0, 0, 150));

      painter.setPen(QPen(QColor(255, 255, 255, 120), 1));
      painter.drawRect(preview.adjusted(0, 0, -1, -1));

      QRect selection = selectionRectInWidget();
      if (selection.width() > 0 && selection.height() > 0) {
        painter.fillRect(selection, QColor(253, 204, 18, 38));
        painter.setPen(QPen(QColor(253, 204, 18), 2));
        painter.drawRect(selection.adjusted(0, 0, -1, -1));
      }
    }
  }

  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton) {
      QWidget::mousePressEvent(event);
      return;
    }

    QRect preview = previewRect();
    if (!preview.contains(event->position().toPoint())) {
      QWidget::mousePressEvent(event);
      return;
    }

    m_isSelecting = true;
    m_dragStart = clampPointToPreview(event->position().toPoint());
    m_dragCurrent = m_dragStart;
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    if (!m_isSelecting) {
      QWidget::mouseMoveEvent(event);
      return;
    }

    m_dragCurrent = clampPointToPreview(event->position().toPoint());
    QRect candidate = QRect(m_dragStart, m_dragCurrent).normalized();

    QRectF normalized = selectionRectToNormalized(candidate);
    if (normalized.width() > 0.0 && normalized.height() > 0.0) {
      m_selectionNormalized = normalized;
      emit selectionChanged(m_selectionNormalized);
      update();
    }

    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (!m_isSelecting || event->button() != Qt::LeftButton) {
      QWidget::mouseReleaseEvent(event);
      return;
    }

    m_isSelecting = false;
    m_dragCurrent = clampPointToPreview(event->position().toPoint());

    QRect candidate = QRect(m_dragStart, m_dragCurrent).normalized();
    if (candidate.width() >= 3 && candidate.height() >= 3) {
      QRectF normalized = selectionRectToNormalized(candidate);
      if (normalized.width() > 0.0 && normalized.height() > 0.0) {
        m_selectionNormalized = normalized;
        emit selectionChanged(m_selectionNormalized);
      }
    }

    update();
    event->accept();
  }

private:
  HWND destinationWindowHandle() const {
    QWidget *topLevelWindow = window();
    if (!topLevelWindow) {
      return nullptr;
    }
    return reinterpret_cast<HWND>(topLevelWindow->winId());
  }

  void ensureThumbnailRegistered() {
    if (!m_sourceWindow || !IsWindow(m_sourceWindow)) {
      return;
    }

    HWND destination = destinationWindowHandle();
    if (!destination) {
      return;
    }

    if (m_thumbnail && m_destinationWindow == destination) {
      return;
    }

    if (m_thumbnail && m_destinationWindow != destination) {
      cleanupThumbnail();
    }

    HRESULT hr = DwmRegisterThumbnail(destination, m_sourceWindow, &m_thumbnail);
    if (FAILED(hr)) {
      m_thumbnail = nullptr;
      m_destinationWindow = nullptr;
      return;
    }

    m_destinationWindow = destination;
    DwmQueryThumbnailSourceSize(m_thumbnail, &m_sourceSize);
  }

  void cleanupThumbnail() {
    if (m_thumbnail) {
      DwmUnregisterThumbnail(m_thumbnail);
      m_thumbnail = nullptr;
    }
    m_destinationWindow = nullptr;
  }

  void updateThumbnail() {
    ensureThumbnailRegistered();
    if (!m_thumbnail) {
      return;
    }

    HRESULT hr = DwmQueryThumbnailSourceSize(m_thumbnail, &m_sourceSize);
    if (FAILED(hr) || m_sourceSize.cx <= 0 || m_sourceSize.cy <= 0) {
      return;
    }

    QRect preview = previewRect();
    if (preview.width() <= 0 || preview.height() <= 0) {
      return;
    }

    QWidget *topLevelWindow = window();
    if (!topLevelWindow) {
      return;
    }

    QPoint previewTopLeftInDestination = mapTo(topLevelWindow, preview.topLeft());
    QRect previewInDestination(previewTopLeftInDestination, preview.size());

    qreal dpr = devicePixelRatio();

    DWM_THUMBNAIL_PROPERTIES props = {};
    props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION |
                    DWM_TNP_VISIBLE | DWM_TNP_OPACITY |
                    DWM_TNP_SOURCECLIENTAREAONLY;
    props.fVisible = TRUE;
    props.opacity = 255;
    props.fSourceClientAreaOnly = TRUE;

    props.rcSource.left = 0;
    props.rcSource.top = 0;
    props.rcSource.right = m_sourceSize.cx;
    props.rcSource.bottom = m_sourceSize.cy;

    props.rcDestination.left =
        static_cast<int>(std::floor(previewInDestination.left() * dpr));
    props.rcDestination.top =
        static_cast<int>(std::floor(previewInDestination.top() * dpr));
    props.rcDestination.right = static_cast<int>(
        std::ceil((previewInDestination.left() + previewInDestination.width()) *
                  dpr));
    props.rcDestination.bottom = static_cast<int>(
        std::ceil((previewInDestination.top() + previewInDestination.height()) *
                  dpr));

    DwmUpdateThumbnailProperties(m_thumbnail, &props);
    update();
  }

  QRect previewRect() const {
    if (m_sourceSize.cx <= 0 || m_sourceSize.cy <= 0) {
      return QRect();
    }

    const qreal sourceAspect = static_cast<qreal>(m_sourceSize.cx) / m_sourceSize.cy;
    const qreal widgetAspect = static_cast<qreal>(width()) / qMax(1, height());

    int targetWidth = width();
    int targetHeight = height();

    if (sourceAspect > widgetAspect) {
      targetHeight = static_cast<int>(std::lround(targetWidth / sourceAspect));
    } else {
      targetWidth = static_cast<int>(std::lround(targetHeight * sourceAspect));
    }

    targetWidth = qMax(1, targetWidth);
    targetHeight = qMax(1, targetHeight);

    int x = (width() - targetWidth) / 2;
    int y = (height() - targetHeight) / 2;
    return QRect(x, y, targetWidth, targetHeight);
  }

  QPoint clampPointToPreview(const QPoint &point) const {
    QRect preview = previewRect();
    if (preview.isEmpty()) {
      return QPoint();
    }

    int x = qBound(preview.left(), point.x(), preview.right());
    int y = qBound(preview.top(), point.y(), preview.bottom());
    return QPoint(x, y);
  }

  QRect selectionRectInWidget() const {
    QRect preview = previewRect();
    if (preview.isEmpty()) {
      return QRect();
    }

    qreal leftF = preview.left() + (m_selectionNormalized.left() * preview.width());
    qreal topF = preview.top() + (m_selectionNormalized.top() * preview.height());
    qreal rightF =
        preview.left() + (m_selectionNormalized.right() * preview.width());
    qreal bottomF =
        preview.top() + (m_selectionNormalized.bottom() * preview.height());

    int left = static_cast<int>(std::floor(leftF));
    int top = static_cast<int>(std::floor(topF));
    int right = static_cast<int>(std::ceil(rightF));
    int bottom = static_cast<int>(std::ceil(bottomF));

    QRect selection(left, top, qMax(1, right - left), qMax(1, bottom - top));
    return selection.intersected(preview);
  }

  QRectF selectionRectToNormalized(const QRect &selection) const {
    QRect preview = previewRect();
    if (preview.isEmpty()) {
      return QRectF();
    }

    QRect clamped = selection.intersected(preview).normalized();
    if (clamped.isEmpty()) {
      return QRectF();
    }

    qreal left = static_cast<qreal>(clamped.left() - preview.left()) / preview.width();
    qreal top = static_cast<qreal>(clamped.top() - preview.top()) / preview.height();
    qreal right =
        static_cast<qreal>((clamped.right() + 1) - preview.left()) / preview.width();
    qreal bottom = static_cast<qreal>((clamped.bottom() + 1) - preview.top()) /
                   preview.height();

    QRectF normalized(QPointF(left, top), QPointF(right, bottom));
    normalized = normalized.normalized();

    qreal clampedLeft = qBound(0.0, normalized.left(), 1.0);
    qreal clampedTop = qBound(0.0, normalized.top(), 1.0);
    qreal clampedRight = qBound(0.0, normalized.right(), 1.0);
    qreal clampedBottom = qBound(0.0, normalized.bottom(), 1.0);

    QRectF result(QPointF(clampedLeft, clampedTop),
                  QPointF(clampedRight, clampedBottom));
    result = result.normalized();
    if (result.width() <= 0.0 || result.height() <= 0.0) {
      return QRectF();
    }

    return result;
  }

  HWND m_sourceWindow = nullptr;
  HWND m_destinationWindow = nullptr;
  HTHUMBNAIL m_thumbnail = nullptr;
  SIZE m_sourceSize = {0, 0};
  QRectF m_selectionNormalized = QRectF(0.0, 0.0, 1.0, 1.0);

  bool m_isSelecting = false;
  QPoint m_dragStart;
  QPoint m_dragCurrent;
};

CropPickerDialog::CropPickerDialog(HWND sourceWindow, const QRectF &initialCrop,
                                   QWidget *parent)
    : QDialog(parent), m_previewWidget(new CropPreviewWidget(sourceWindow, this)),
      m_summaryLabel(new QLabel(this)), m_selectedCrop(normalizeOrFull(initialCrop)) {
  setWindowTitle("Pick Thumbnail Crop");
  resize(860, 620);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(14, 14, 14, 14);
  layout->setSpacing(10);

  QLabel *infoLabel = new QLabel(
      "Drag a rectangle to select the thumbnail crop area. The thumbnail will "
      "always fill its frame without distortion.",
      this);
  infoLabel->setWordWrap(true);
  layout->addWidget(infoLabel);

  layout->addWidget(m_previewWidget, 1);

  layout->addWidget(m_summaryLabel);

  QHBoxLayout *buttonLayout = new QHBoxLayout();
  buttonLayout->setSpacing(8);

  QPushButton *resetButton = new QPushButton("Reset to Full", this);
  connect(resetButton, &QPushButton::clicked, this,
          &CropPickerDialog::onResetClicked);
  buttonLayout->addWidget(resetButton);

  buttonLayout->addStretch();

  QPushButton *cancelButton = new QPushButton("Cancel", this);
  connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
  buttonLayout->addWidget(cancelButton);

  QPushButton *saveButton = new QPushButton("Save", this);
  saveButton->setDefault(true);
  connect(saveButton, &QPushButton::clicked, this, &QDialog::accept);
  buttonLayout->addWidget(saveButton);

  layout->addLayout(buttonLayout);

  connect(m_previewWidget, &CropPreviewWidget::selectionChanged, this,
          &CropPickerDialog::updateSelectionSummary);

  m_previewWidget->setSelectionNormalized(m_selectedCrop);
  updateSelectionSummary(m_selectedCrop);
}

QRectF CropPickerDialog::selectedCropNormalized() const {
  return normalizeOrFull(m_selectedCrop);
}

void CropPickerDialog::onResetClicked() {
  QRectF full(0.0, 0.0, 1.0, 1.0);
  m_previewWidget->setSelectionNormalized(full);
}

void CropPickerDialog::updateSelectionSummary(const QRectF &selection) {
  m_selectedCrop = normalizeOrFull(selection);

  const qreal x = m_selectedCrop.x() * 100.0;
  const qreal y = m_selectedCrop.y() * 100.0;
  const qreal w = m_selectedCrop.width() * 100.0;
  const qreal h = m_selectedCrop.height() * 100.0;

  m_summaryLabel->setText(
      QString("Selected crop: x=%1%, y=%2%, w=%3%, h=%4%")
          .arg(QString::number(x, 'f', 1), QString::number(y, 'f', 1),
               QString::number(w, 'f', 1), QString::number(h, 'f', 1)));
}

#include "croppickerdialog.moc"
