#include "ui/SettingsWidget.h"

#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace solidar {

SettingsWidget::SettingsWidget(AppSettings& settings, QWidget* parent)
    : QWidget(parent), settings_(settings) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(18);

  auto* title = new QLabel(QString::fromUtf8("Интерфейс"), this);
  title->setObjectName("sectionTitle");
  root->addWidget(title);

  auto* card = new QFrame(this);
  card->setObjectName("actionCard");
  auto* cardLayout = new QVBoxLayout(card);
  cardLayout->setContentsMargins(28, 24, 28, 24);
  cardLayout->setSpacing(12);

  auto* themeTitle = new QLabel(QString::fromUtf8("Тема"), card);
  themeTitle->setObjectName("cardTitle");
  cardLayout->addWidget(themeTitle);

  auto* form = new QFormLayout;
  form->setContentsMargins(0, 0, 0, 0);
  form->setSpacing(8);

  themeCombo_ = new QComboBox(card);
  themeCombo_->setObjectName("themeCombo");
  themeCombo_->addItem(QString::fromUtf8("Системная"));
  themeCombo_->addItem(QString::fromUtf8("Светлая"));
  themeCombo_->addItem(QString::fromUtf8("Тёмная"));
  form->addRow(QString::fromUtf8("Оформление приложения:"), themeCombo_);

  cardLayout->addLayout(form);

  themeDescription_ = new QLabel(card);
  themeDescription_->setObjectName("themeDescription");
  themeDescription_->setProperty("uiRole", "secondaryText");
  themeDescription_->setWordWrap(true);
  cardLayout->addWidget(themeDescription_);

  auto* buttons = new QHBoxLayout;
  buttons->addStretch();
  cancelButton_ = new QPushButton(QString::fromUtf8("Отмена"), card);
  cancelButton_->setObjectName("cancelButton");
  applyButton_ = new QPushButton(QString::fromUtf8("Применить"), card);
  applyButton_->setObjectName("applyButton");
  applyButton_->setProperty("uiRole", "primaryAction");
  buttons->addWidget(cancelButton_);
  buttons->addWidget(applyButton_);
  cardLayout->addLayout(buttons);

  root->addWidget(card);
  root->addStretch();

  syncComboToSettings();

  connect(themeCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
    updateDescription(selectedTheme());
  });
  connect(applyButton_, &QPushButton::clicked, this, &SettingsWidget::applyChanges);
  connect(cancelButton_, &QPushButton::clicked, this, &SettingsWidget::cancelChanges);
  connect(&settings_, &AppSettings::themeChanged, this,
          [this](AppTheme) { syncComboToSettings(); });
}

AppTheme SettingsWidget::selectedTheme() const {
  const int index = themeCombo_->currentIndex();
  return index == 1 ? AppTheme::Light : index == 2 ? AppTheme::Dark
                                                   : AppTheme::System;
}

void SettingsWidget::syncComboToSettings() {
  const AppTheme theme = settings_.theme();
  const int index = theme == AppTheme::Light   ? 1
                    : theme == AppTheme::Dark ? 2
                                              : 0;
  const QSignalBlocker blocker(themeCombo_);
  themeCombo_->setCurrentIndex(index);
  updateDescription(theme);
}

void SettingsWidget::applyChanges() {
  settings_.setTheme(selectedTheme());
}

void SettingsWidget::cancelChanges() {
  syncComboToSettings();
  emit cancelRequested();
}

void SettingsWidget::updateDescription(AppTheme theme) {
  switch (theme) {
    case AppTheme::Light:
      themeDescription_->setText(
          QString::fromUtf8("Светлая тема интерфейса."));
      break;
    case AppTheme::Dark:
      themeDescription_->setText(
          QString::fromUtf8("Тёмная тема интерфейса."));
      break;
    case AppTheme::System:
      themeDescription_->setText(
          QString::fromUtf8("Системная тема следует настройке операционной системы."));
      break;
  }
}

}  // namespace solidar
