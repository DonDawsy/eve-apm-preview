#ifndef CROPPICKERDIALOG_H
#define CROPPICKERDIALOG_H

#include <QDialog>
#include <QRectF>
#include <QSize>
#include <Windows.h>

class QLabel;
class QPushButton;
class CropPreviewWidget;

class CropPickerDialog : public QDialog {
  Q_OBJECT

public:
  explicit CropPickerDialog(HWND sourceWindow, const QRectF &initialCrop,
                            QWidget *parent = nullptr);

  QRectF selectedCropNormalized() const;
  QSize selectedCropPixelSize() const;
  QSize sourceSizePixels() const;

private slots:
  void onResetClicked();
  void updateSelectionSummary(const QRectF &selection);

private:
  CropPreviewWidget *m_previewWidget;
  QLabel *m_summaryLabel;
  QRectF m_selectedCrop;
};

#endif
