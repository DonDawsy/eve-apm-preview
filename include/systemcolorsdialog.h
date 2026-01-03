#ifndef SYSTEMCOLORSDIALOG_H
#define SYSTEMCOLORSDIALOG_H

#include <QColor>
#include <QDialog>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

class SystemColorsDialog : public QDialog {
  Q_OBJECT

public:
  explicit SystemColorsDialog(QWidget *parent = nullptr);
  ~SystemColorsDialog() override = default;

  void loadSystemColors();

  void saveSystemColors();

private slots:
  void onAddSystemColor();
  void onSystemColorButtonClicked();
  void onOkClicked();
  void onCancelClicked();

private:
  QWidget *createSystemColorFormRow(const QString &systemName = "",
                                    const QColor &color = QColor("#00FFFF"));
  void updateScrollHeight();
  void updateColorButton(QPushButton *button, const QColor &color);

  QScrollArea *m_scrollArea;
  QWidget *m_container;
  QVBoxLayout *m_layout;
  QPushButton *m_addButton;
  QPushButton *m_okButton;
  QPushButton *m_cancelButton;

  QHash<QString, QColor> m_initialColors;
};

#endif 
