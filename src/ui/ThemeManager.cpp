#include "ui/ThemeManager.h"

#include <QApplication>
#include <QGuiApplication>
#include <QHash>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace solidar {
namespace {

QString substituteColors(QString qss, const ThemeColors& c) {
  const QHash<QString, QString> tokens{
      {"$window", c.window.name(QColor::HexRgb)},
      {"$surface", c.surface.name(QColor::HexRgb)},
      {"$surfaceAlt", c.surfaceAlt.name(QColor::HexRgb)},
      {"$elevated", c.elevated.name(QColor::HexRgb)},
      {"$textPrimary", c.textPrimary.name(QColor::HexRgb)},
      {"$textSecondary", c.textSecondary.name(QColor::HexRgb)},
      {"$textDisabled", c.textDisabled.name(QColor::HexRgb)},
      {"$border", c.border.name(QColor::HexRgb)},
      {"$borderStrong", c.borderStrong.name(QColor::HexRgb)},
      {"$accent", c.accent.name(QColor::HexRgb)},
      {"$accentHover", c.accentHover.name(QColor::HexRgb)},
      {"$accentSoft", c.accentSoft.name(QColor::HexRgb)},
      {"$selection", c.selection.name(QColor::HexRgb)},
      {"$selectionText", c.selectionText.name(QColor::HexRgb)},
      {"$danger", c.danger.name(QColor::HexRgb)},
      {"$warning", c.warning.name(QColor::HexRgb)},
  };
  for (auto it = tokens.cbegin(); it != tokens.cend(); ++it)
    qss.replace(it.key(), it.value());
  return qss;
}

}  // namespace

ResolvedTheme resolveTheme(AppTheme preference, Qt::ColorScheme systemScheme) {
  switch (preference) {
    case AppTheme::Light: return ResolvedTheme::Light;
    case AppTheme::Dark: return ResolvedTheme::Dark;
    case AppTheme::System:
      return systemScheme == Qt::ColorScheme::Dark ? ResolvedTheme::Dark
                                                   : ResolvedTheme::Light;
  }
  return ResolvedTheme::Light;
}

ThemeManager& ThemeManager::instance() {
  static ThemeManager manager;
  return manager;
}

void ThemeManager::apply(ResolvedTheme theme) {
  resolved_ = theme;
  const ThemeColors& palette = colors(theme);

  // Fusion gives a predictable, controllable base so Windows native dark
  // widgets can never leak into the SolidarCAD chrome.
  if (QApplication* app = qobject_cast<QApplication*>(QApplication::instance())) {
    static bool styleSet = false;
    if (!styleSet) {
      if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        app->setStyle(fusion);
      styleSet = true;
    }
    applyPalette(palette);
    app->setStyleSheet(buildStylesheet(palette));
  }
}

ResolvedTheme ThemeManager::resolved() const noexcept { return resolved_; }

const ThemeColors& ThemeManager::colors() const noexcept {
  return colors(resolved_);
}

const ThemeColors& ThemeManager::colors(ResolvedTheme theme) const noexcept {
  static const ThemeColors light = lightThemeColors();
  static const ThemeColors dark = darkThemeColors();
  return theme == ResolvedTheme::Dark ? dark : light;
}

Qt::ColorScheme ThemeManager::systemColorScheme() {
  if (QGuiApplication* app = qobject_cast<QGuiApplication*>(QGuiApplication::instance())) {
    if (app->styleHints())
      return app->styleHints()->colorScheme();
  }
  return Qt::ColorScheme::Light;
}

void ThemeManager::applyPalette(const ThemeColors& c) {
  QPalette palette;
  palette.setColor(QPalette::Window, c.window);
  palette.setColor(QPalette::WindowText, c.textPrimary);
  palette.setColor(QPalette::Base, c.surface);
  palette.setColor(QPalette::AlternateBase, c.surfaceAlt);
  palette.setColor(QPalette::ToolTipBase, c.surface);
  palette.setColor(QPalette::ToolTipText, c.textPrimary);
  palette.setColor(QPalette::Text, c.textPrimary);
  palette.setColor(QPalette::Button, c.surfaceAlt);
  palette.setColor(QPalette::ButtonText, c.textPrimary);
  palette.setColor(QPalette::BrightText, c.textPrimary);
  palette.setColor(QPalette::Highlight, c.accent);
  palette.setColor(QPalette::HighlightedText, c.selectionText);
  palette.setColor(QPalette::PlaceholderText, c.textSecondary);
  palette.setColor(QPalette::Light, c.border);
  palette.setColor(QPalette::Midlight, c.surface);
  palette.setColor(QPalette::Mid, c.borderStrong);
  palette.setColor(QPalette::Dark, c.borderStrong);
  palette.setColor(QPalette::Shadow, Qt::black);
  palette.setColor(QPalette::Disabled, QPalette::Text, c.textDisabled);
  palette.setColor(QPalette::Disabled, QPalette::ButtonText, c.textDisabled);
  palette.setColor(QPalette::Disabled, QPalette::WindowText, c.textDisabled);
  palette.setColor(QPalette::Disabled, QPalette::Highlight, c.surfaceAlt);
  QApplication::setPalette(palette);
}

QString ThemeManager::buildStylesheet(const ThemeColors& c) {
  return substituteColors(QStringLiteral(R"(
    QMainWindow { background: $window; }
    QWidget { color: $textPrimary; }
    QFrame { background: transparent; }
    QLabel { color: $textPrimary; background: transparent; }
    QLabel[uiRole="secondaryText"] { color: $textSecondary; }
    QLabel[uiRole="danger"] { color: $danger; }
    QLabel[uiRole="success"] { color: $accent; }

    QMenuBar { background: $surface; color: $textPrimary; border-bottom: 1px solid $border; }
    QMenuBar::item { background: transparent; padding: 6px 12px; }
    QMenuBar::item:selected { background: $accentSoft; border-radius: 5px; }
    QMenu { background: $surface; color: $textPrimary; border: 1px solid $borderStrong; padding: 5px; }
    QMenu::item { padding: 7px 24px 7px 10px; border-radius: 4px; }
    QMenu::item:selected { background: $accentSoft; color: $textPrimary; }
    QMenu::item:disabled { color: $textDisabled; }
    QMenu::separator { height: 1px; background: $border; margin: 4px 8px; }

    QPushButton { background: $surfaceAlt; color: $textPrimary; border: 1px solid $border;
      border-radius: 7px; padding: 5px 12px; }
    QPushButton:hover { background: $accentSoft; }
    QPushButton:pressed { background: $accentSoft; }
    QPushButton:disabled { color: $textDisabled; }
    QPushButton[uiRole="primaryAction"] { background: $accent; color: #ffffff;
      border: none; border-radius: 6px; padding: 8px 14px; font-weight: 600; }
    QPushButton[uiRole="primaryAction"]:hover { background: $accentHover; }

    QToolButton { background: transparent; color: $textPrimary; border: 1px solid transparent;
      border-radius: 7px; padding: 4px; }
    QToolButton:hover { background: $accentSoft; }
    QToolButton:pressed { background: $accentSoft; }
    QToolButton:checked { background: $accentSoft; color: $accent; border: 1px solid $accent; }
    QToolButton:disabled { color: $textDisabled; }
    QToolButton[stateRole="error"] { border: 2px solid $danger; }
    QToolButton[stateRole="dirty"] { border: 2px solid $warning; }

    QFrame[uiRole="sectionPanel"] { border-top: 1px solid $border; margin-top: 8px;
      padding-top: 10px; }

    QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
      background: $surface; color: $textPrimary; border: 1px solid $borderStrong;
      border-radius: 5px; padding: 3px 6px; }
    QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
      border: 1px solid $accent; }
    QComboBox QAbstractItemView { background: $surface; color: $textPrimary;
      selection-background-color: $accentSoft; selection-color: $textPrimary;
      border: 1px solid $borderStrong; }
    QComboBox::drop-down { border: none; width: 20px; }
    QComboBox::down-arrow { border-left: 4px solid transparent;
      border-right: 4px solid transparent; border-top: 5px solid $textSecondary; }
    QComboBox:disabled { color: $textDisabled; }
    QCheckBox, QRadioButton { color: $textPrimary; background: transparent; }
    QCheckBox:disabled, QRadioButton:disabled { color: $textDisabled; }

    QTreeWidget, QTreeView, QListWidget { background: $surface; color: $textPrimary;
      border: none; outline: 0; }
    QTreeWidget::item, QTreeView::item, QListWidget::item { height: 26px; border-radius: 5px; }
    QTreeWidget::item:selected, QTreeView::item:selected, QListWidget::item:selected {
      background: $accentSoft; color: $selectionText; }
    QHeaderView::section { background: $surfaceAlt; color: $textSecondary;
      border: none; border-bottom: 1px solid $border; padding: 4px 8px; }

    QDockWidget { color: $textPrimary; }
    QDockWidget::title { background: $surface; padding: 8px 10px;
      border-bottom: 1px solid $border; }

    QTabWidget::pane { border: 1px solid $border; }
    QTabBar::tab { background: $surfaceAlt; color: $textSecondary;
      padding: 6px 14px; border: 1px solid $border; border-bottom: none;
      border-top-left-radius: 6px; border-top-right-radius: 6px; }
    QTabBar::tab:selected { background: $surface; color: $textPrimary; }
    QTabBar::tab:hover { background: $accentSoft; }

    QScrollArea { background: transparent; border: none; }
    QScrollBar:vertical { background: $surface; width: 12px; margin: 0; }
    QScrollBar::handle:vertical { background: $borderStrong; border-radius: 5px; min-height: 24px; }
    QScrollBar:horizontal { background: $surface; height: 12px; margin: 0; }
    QScrollBar::handle:horizontal { background: $borderStrong; border-radius: 5px; min-width: 24px; }
    QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
    QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

    QStatusBar { background: $surface; color: $textSecondary; border-top: 1px solid $border; }
    QToolTip { background: $surface; color: $textPrimary; border: 1px solid $borderStrong; }
    QDialog { background: $window; }
    QDialogButtonBox QPushButton { min-width: 72px; }

    /* Home screen */
    QWidget#root, QWidget#content { background: $window; }
    QFrame#sidebar { background: $surface; border-right: 1px solid $border; }
    QPushButton#navActive { background: $accent; color: #ffffff; border: none;
      border-radius: 10px; text-align: left; padding-left: 22px; font-size: 15px; }
    QPushButton#navButton { background: transparent; color: $textPrimary; border: none;
      border-radius: 10px; text-align: left; padding-left: 22px; font-size: 15px; }
    QPushButton#navButton:hover { background: $accentSoft; }
    QLabel#versionLabel { color: $textSecondary; font-size: 12px; }
    QLabel#heading { color: $textPrimary; font-size: 34px; font-weight: 700; }
    QLabel#subtitle { color: $textSecondary; font-size: 15px; }
    QLabel#sectionTitle { color: $textPrimary; font-size: 20px; font-weight: 600; }
    QFrame#actionCard { background: $surface; border: 1px solid $border; border-radius: 14px; }
    QFrame#actionCard:hover { border: 1px solid $accent; }
    QLabel#cardIcon { background: $accentSoft; color: $accent; border-radius: 29px;
      font-size: 30px; }
    QLabel#cardTitle { color: $textPrimary; font-size: 22px; font-weight: 700; }
    QLabel#cardDescription { color: $textSecondary; font-size: 14px; }
    QPushButton#primaryButton { background: $accent; color: #ffffff; border: none;
      border-radius: 9px; font-size: 15px; font-weight: 600; }
    QPushButton#primaryButton:hover { background: $accentHover; }
    QPushButton#secondaryButton { background: $surface; color: $accent;
      border: 1px solid $accent; border-radius: 9px; font-size: 15px; font-weight: 600; }
    QPushButton#secondaryButton:hover { background: $accentSoft; }

    /* Model / Sketch ribbons */
    QWidget#modelRibbon, QWidget#sketchRibbon { background: $surface;
      border-bottom: 1px solid $border; }
    QToolButton#modelCommand, QPushButton#toolButton { background: transparent;
      color: $textPrimary; border: 1px solid transparent; border-radius: 8px;
      font-size: 13px; padding: 5px 10px; }
    QToolButton#modelCommand:hover, QPushButton#toolButton:hover { background: $accentSoft; }
    QToolButton#modelCommand:checked, QPushButton#toolButton:checked {
      background: $accentSoft; color: $accent; border: 2px solid $accent; font-weight: 600; }
    QPushButton#toolButton:disabled { color: $textDisabled; background: transparent; }
    QToolButton#modelGroupMenuButton, QToolButton#groupMenuButton {
      color: $textSecondary; background: transparent; border: none; font-size: 10px;
      font-weight: 700; padding: 2px 10px; }
    QToolButton#modelGroupMenuButton:hover, QToolButton#groupMenuButton:hover {
      color: $accent; background: $accentSoft; border-radius: 5px; }
    QLabel#groupCaption { color: $textSecondary; font-size: 10px; font-weight: 600; }
    QFrame#modelSeparator, QFrame#separator { color: $border; margin: 4px 7px; }
    QLabel#constraintReady { color: $accent; font-size: 13px; }
    QLabel#constraintMuted { color: $textSecondary; font-size: 12px; }
    QPushButton#finishButton { background: $accent; color: #ffffff; border: none;
      border-radius: 9px; font-size: 14px; font-weight: 600; padding: 0 18px; }
    QPushButton#finishButton:hover { background: $accentHover; }
  )"), c);
}

}  // namespace solidar
