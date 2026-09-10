#include "ui/SettingsWidget.h"

#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
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

  root->addWidget(card);
  root->addStretch();

  auto applyCombo = [this](AppTheme theme) {
    const int index = theme == AppTheme::Light   ? 1
                      : theme == AppTheme::Dark ? 2
                                                : 0;
    const QSignalBlocker blocker(themeCombo_);
    themeCombo_->setCurrentIndex(index);
    updateDescription(theme);
  };
  applyCombo(settings_.theme());

  connect(themeCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
    const AppTheme theme = index == 1   ? AppTheme::Light
                           : index == 2 ? AppTheme::Dark
                                        : AppTheme::System;
    settings_.setTheme(theme);
    updateDescription(theme);
  });
  connect(&settings_, &AppSettings::themeChanged, this, applyCombo);
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
