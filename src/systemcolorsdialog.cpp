#include "systemcolorsdialog.h"
#include "config.h"
#include "stylesheet.h"
#include <QColorDialog>
#include <QScrollBar>
#include <QTimer>

SystemColorsDialog::SystemColorsDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle("System Name Colors");
  resize(550, 500);

  QVBoxLayout *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(16, 16, 16, 16);
  mainLayout->setSpacing(12);

  QLabel *infoLabel = new QLabel(
      "Define custom colors for specific solar systems. These colors will "
      "override the default or unique generated colors.");
  infoLabel->setWordWrap(true);
  infoLabel->setStyleSheet(StyleSheet::getDialogInfoLabelStyleSheet());
  mainLayout->addWidget(infoLabel);

  m_scrollArea = new QScrollArea();
  m_scrollArea->setWidgetResizable(true);
  m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  m_scrollArea->setStyleSheet(
      "QScrollArea { border: 1px solid #3a3a3a; background-color: #1e1e1e; "
      "border-radius: 4px; }");

  m_container = new QWidget();
  m_layout = new QVBoxLayout(m_container);
  m_layout->setContentsMargins(4, 4, 4, 4);
  m_layout->setSpacing(6);
  m_layout->addStretch();

  m_scrollArea->setWidget(m_container);
  mainLayout->addWidget(m_scrollArea, 1);

  QHBoxLayout *addButtonLayout = new QHBoxLayout();
  m_addButton = new QPushButton("Add System");
  m_addButton->setStyleSheet(StyleSheet::getDialogButtonStyleSheet());
  connect(m_addButton, &QPushButton::clicked, this,
          &SystemColorsDialog::onAddSystemColor);
  addButtonLayout->addWidget(m_addButton);
  addButtonLayout->addStretch();
  mainLayout->addLayout(addButtonLayout);

  QHBoxLayout *dialogButtonLayout = new QHBoxLayout();
  dialogButtonLayout->addStretch();

  m_okButton = new QPushButton("OK");
  m_cancelButton = new QPushButton("Cancel");

  QString buttonStyle = StyleSheet::getDialogButtonStyleSheet();
  m_okButton->setStyleSheet(buttonStyle);
  m_cancelButton->setStyleSheet(buttonStyle);

  connect(m_okButton, &QPushButton::clicked, this,
          &SystemColorsDialog::onOkClicked);
  connect(m_cancelButton, &QPushButton::clicked, this,
          &SystemColorsDialog::onCancelClicked);

  dialogButtonLayout->addWidget(m_okButton);
  dialogButtonLayout->addWidget(m_cancelButton);

  mainLayout->addLayout(dialogButtonLayout);

  setStyleSheet(StyleSheet::getDialogStyleSheetForWidget());
}

void SystemColorsDialog::loadSystemColors() {
  while (m_layout->count() > 1) {
    QLayoutItem *item = m_layout->takeAt(0);
    if (item->widget()) {
      item->widget()->deleteLater();
    }
    delete item;
  }

  Config &config = Config::instance();
  QHash<QString, QColor> systemColors = config.getAllSystemNameColors();
  m_initialColors = systemColors;

  for (auto it = systemColors.constBegin(); it != systemColors.constEnd();
       ++it) {
    QString systemName = it.key();
    QColor color = it.value();

    QWidget *formRow = createSystemColorFormRow(systemName, color);
    int count = m_layout->count();
    m_layout->insertWidget(count - 1, formRow);
  }

  if (systemColors.isEmpty()) {
    QWidget *formRow = createSystemColorFormRow();
    int count = m_layout->count();
    m_layout->insertWidget(count - 1, formRow);
  }

  updateScrollHeight();
}

void SystemColorsDialog::saveSystemColors() {
  Config &config = Config::instance();

  QHash<QString, QColor> allColors = config.getAllSystemNameColors();
  for (auto it = allColors.constBegin(); it != allColors.constEnd(); ++it) {
    config.removeSystemNameColor(it.key());
  }

  for (int i = 0; i < m_layout->count() - 1; ++i) {
    QWidget *rowWidget = qobject_cast<QWidget *>(m_layout->itemAt(i)->widget());
    if (!rowWidget)
      continue;

    QLineEdit *nameEdit = rowWidget->findChild<QLineEdit *>();
    if (!nameEdit)
      continue;

    QString systemName = nameEdit->text().trimmed();
    if (systemName.isEmpty())
      continue;

    QPushButton *colorButton = nullptr;
    QList<QPushButton *> buttons = rowWidget->findChildren<QPushButton *>();
    for (QPushButton *btn : buttons) {
      if (btn->property("color").isValid()) {
        colorButton = btn;
        break;
      }
    }

    if (colorButton) {
      QColor color = colorButton->property("color").value<QColor>();
      if (color.isValid()) {
        config.setSystemNameColor(systemName, color);
      }
    }
  }

  config.save();
}

QWidget *SystemColorsDialog::createSystemColorFormRow(const QString &systemName,
                                                      const QColor &color) {
  QWidget *rowWidget = new QWidget();
  rowWidget->setStyleSheet(
      "QWidget { background-color: #2a2a2a; border: 1px solid #3a3a3a; "
      "border-radius: 4px; padding: 4px; }");

  QHBoxLayout *rowLayout = new QHBoxLayout(rowWidget);
  rowLayout->setContentsMargins(8, 4, 8, 4);
  rowLayout->setSpacing(8);

  QLineEdit *nameEdit = new QLineEdit();
  nameEdit->setText(systemName);
  nameEdit->setPlaceholderText("System Name (e.g., Jita)");
  nameEdit->setStyleSheet(StyleSheet::getTableCellEditorStyleSheet());
  rowLayout->addWidget(nameEdit, 1);

  QPushButton *colorButton = new QPushButton();
  colorButton->setFixedSize(150, 32);
  colorButton->setCursor(Qt::PointingHandCursor);
  colorButton->setProperty("color", color);
  updateColorButton(colorButton, color);
  connect(colorButton, &QPushButton::clicked, this,
          &SystemColorsDialog::onSystemColorButtonClicked);
  rowLayout->addWidget(colorButton);

  QPushButton *deleteButton = new QPushButton("×");
  deleteButton->setFixedSize(32, 32);
  deleteButton->setStyleSheet("QPushButton {"
                              "    background-color: #3a3a3a;"
                              "    color: #ffffff;"
                              "    border: 1px solid #555555;"
                              "    border-radius: 4px;"
                              "    font-size: 18px;"
                              "    font-weight: bold;"
                              "    padding: 0px;"
                              "}"
                              "QPushButton:hover {"
                              "    background-color: #e74c3c;"
                              "    border: 1px solid #c0392b;"
                              "}"
                              "QPushButton:pressed {"
                              "    background-color: #c0392b;"
                              "}");
  deleteButton->setCursor(Qt::PointingHandCursor);

  connect(deleteButton, &QPushButton::clicked, this, [this, rowWidget]() {
    m_layout->removeWidget(rowWidget);
    rowWidget->deleteLater();
    QTimer::singleShot(0, this, &SystemColorsDialog::updateScrollHeight);
  });

  rowLayout->addWidget(deleteButton);

  return rowWidget;
}

void SystemColorsDialog::updateScrollHeight() {
  if (m_scrollArea && m_layout) {
    int rowCount = m_layout->count() - 1;

    if (rowCount <= 0) {
      m_scrollArea->setMinimumHeight(60);
    } else {
      int calculatedHeight = (rowCount * 48) + 10;
      int finalHeight = qMin(300, qMax(60, calculatedHeight));
      m_scrollArea->setMinimumHeight(finalHeight);
    }
  }
}

void SystemColorsDialog::updateColorButton(QPushButton *button,
                                           const QColor &color) {
  if (!button || !color.isValid()) {
    return;
  }

  QString textColor =
      (color.red() * 0.299 + color.green() * 0.587 + color.blue() * 0.114) > 128
          ? "#000000"
          : "#FFFFFF";

  button->setStyleSheet(QString("QPushButton {"
                                "    background-color: %1;"
                                "    color: %2;"
                                "    border: 1px solid #555555;"
                                "    border-radius: 4px;"
                                "    font-weight: bold;"
                                "    padding: 4px;"
                                "}"
                                "QPushButton:hover {"
                                "    border: 2px solid #ffffff;"
                                "}")
                            .arg(color.name())
                            .arg(textColor));

  button->setText(color.name().toUpper());
}

void SystemColorsDialog::onAddSystemColor() {
  QWidget *formRow = createSystemColorFormRow();

  int count = m_layout->count();
  m_layout->insertWidget(count - 1, formRow);

  m_container->updateGeometry();
  m_layout->activate();

  QLineEdit *nameEdit = formRow->findChild<QLineEdit *>();
  if (nameEdit) {
    nameEdit->setFocus();
  }

  updateScrollHeight();

  QTimer::singleShot(0, [this]() {
    m_scrollArea->verticalScrollBar()->setValue(
        m_scrollArea->verticalScrollBar()->maximum());
  });
}

void SystemColorsDialog::onSystemColorButtonClicked() {
  QPushButton *button = qobject_cast<QPushButton *>(sender());
  if (!button)
    return;

  QColor currentColor = button->property("color").value<QColor>();

  QColor newColor =
      QColorDialog::getColor(currentColor, this, "Select System Name Color");

  if (newColor.isValid()) {
    button->setProperty("color", newColor);
    updateColorButton(button, newColor);
  }
}

void SystemColorsDialog::onOkClicked() {
  saveSystemColors();
  accept();
}

void SystemColorsDialog::onCancelClicked() { reject(); }
