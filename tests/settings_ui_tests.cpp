#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QPushButton>
#include <QStackedWidget>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

#include "app/AppSettings.h"
#include "home/HomeWindow.h"
#include "ui/SettingsWidget.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

namespace {
QPushButton* buttonByText(QWidget* root, const QString& text) {
  for (auto* button : root->findChildren<QPushButton*>())
    if (button->text() == text) return button;
  return nullptr;
}
}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
  QTemporaryDir directory;
  CHECK(directory.isValid());

  // SettingsWidget edits and reflects AppSettings without a signal loop.
  {
    solidar::AppSettings settings(directory.filePath("a.ini"));
    solidar::SettingsWidget widget(settings);
    auto* combo = widget.findChild<QComboBox*>("themeCombo");
    CHECK(combo);
    CHECK(combo->count() == 3);
    CHECK(combo->currentIndex() == 0);  // System default

    combo->setCurrentIndex(2);  // Dark
    CHECK(settings.theme() == solidar::AppTheme::Dark);
    CHECK(combo->currentIndex() == 2);

    settings.setTheme(solidar::AppTheme::Light);  // external change reflects back
    CHECK(combo->currentIndex() == 1);
    CHECK(settings.theme() == solidar::AppTheme::Light);
  }

  // HomeWindow navigation updates the page and the active sidebar state.
  {
    solidar::AppSettings settings(directory.filePath("b.ini"));
    solidar::home::HomeWindow home(settings);
    auto* pages = home.findChild<QStackedWidget*>("pages");
    CHECK(pages);
    auto* settingsButton = buttonByText(&home, QString::fromUtf8("Настройки"));
    auto* overviewButton = buttonByText(&home, QString::fromUtf8("Главная"));
    CHECK(settingsButton);
    CHECK(overviewButton);
    CHECK(pages->currentIndex() == 0);
    CHECK(overviewButton->objectName() == QStringLiteral("navActive"));

    settingsButton->click();
    CHECK(pages->currentIndex() == 1);
    CHECK(settingsButton->objectName() == QStringLiteral("navActive"));
    CHECK(overviewButton->objectName() == QStringLiteral("navButton"));

    overviewButton->click();
    CHECK(pages->currentIndex() == 0);
    CHECK(overviewButton->objectName() == QStringLiteral("navActive"));
    CHECK(settingsButton->objectName() == QStringLiteral("navButton"));
  }

  return EXIT_SUCCESS;
}
