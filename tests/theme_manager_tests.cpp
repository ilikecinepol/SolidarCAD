#include "TestAssertions.h"

#include <QColor>
#include <QApplication>
#include <QPalette>
#include <QtGlobal>

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <iostream>

#include "ui/ThemeColors.h"
#include "ui/ThemeManager.h"

namespace {

double relativeLuminance(const QColor& color) {
  const auto linear = [](double value) {
    value /= 255.0;
    return value <= 0.04045 ? value / 12.92
                            : std::pow((value + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * linear(color.red()) + 0.7152 * linear(color.green()) +
         0.0722 * linear(color.blue());
}

double contrastRatio(const QColor& first, const QColor& second) {
  const double firstLuminance = relativeLuminance(first);
  const double secondLuminance = relativeLuminance(second);
  const double lighter = std::max(firstLuminance, secondLuminance);
  const double darker = std::min(firstLuminance, secondLuminance);
  return (lighter + 0.05) / (darker + 0.05);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace solidar;
  QApplication app(argc, argv);

  // resolveTheme maps the preference onto a concrete Light/Dark.
  CHECK(resolveTheme(AppTheme::Light, Qt::ColorScheme::Dark) ==
        ResolvedTheme::Light);
  CHECK(resolveTheme(AppTheme::Dark, Qt::ColorScheme::Light) ==
        ResolvedTheme::Dark);
  CHECK(resolveTheme(AppTheme::System, Qt::ColorScheme::Light) ==
        ResolvedTheme::Light);
  CHECK(resolveTheme(AppTheme::System, Qt::ColorScheme::Dark) ==
        ResolvedTheme::Dark);
  CHECK(resolveTheme(AppTheme::System, Qt::ColorScheme::Unknown) ==
        ResolvedTheme::Light);

  // Semantic separation must hold for both themes (no text-on-text or
  // accent-on-surface collisions).
  for (const ThemeColors& c : {lightThemeColors(), darkThemeColors()}) {
    CHECK(c.window != c.textPrimary);
    CHECK(c.surface != c.textPrimary);
    CHECK(c.accent != c.surface);
    CHECK(c.onAccent != c.accent);
    CHECK(std::abs(qGray(c.onAccent.rgb()) - qGray(c.accent.rgb())) >= 80);
    CHECK(c.textDisabled != c.textPrimary);
    CHECK(c.viewportBackground.isValid());
    CHECK(c.viewportSurface.isValid());
    CHECK(c.viewportSurfaceLight.isValid());
    CHECK(c.viewportSurfaceDark.isValid());
    CHECK(c.viewportEdge.isValid());
    CHECK(c.viewportHandle.isValid());
    CHECK(c.viewportHover.isValid());
    CHECK(c.viewportSelection.isValid());
    CHECK(c.viewportAngular.isValid());
    CHECK(c.previewPositive.isValid());
    CHECK(c.previewNegative.isValid());
    CHECK(c.timelineTrack.isValid());
    CHECK(contrastRatio(c.timelineTrack, c.surface) >= 3.0);
    CHECK(contrastRatio(c.timelineTrack, c.window) >= 3.0);
    CHECK(contrastRatio(c.viewportEdge, c.viewportBackground) >= 3.0);
    CHECK(c.shadow.isValid());
    CHECK(c.gridMinor.isValid() && c.gridMajor.isValid());
    CHECK(c.axisX != c.axisY && c.axisY != c.axisZ && c.axisX != c.axisZ);
    CHECK(c.cubeTop != c.cubeSide);
    CHECK(c.cubeFront != c.cubeSide);
  }

  // Smoke all three preferences through the actual QApplication palette.
  // System is resolved first, so widgets never inherit an arbitrary native
  // light background while the rest of the application uses a dark palette.
  ThemeManager& manager = ThemeManager::instance();
  for (const ResolvedTheme resolved : {ResolvedTheme::Light,
                                       ResolvedTheme::Dark,
                                       resolveTheme(AppTheme::System,
                                                    manager.systemColorScheme())}) {
    manager.apply(resolved);
    const ThemeColors& c = manager.colors();
    CHECK(manager.resolved() == resolved);
    CHECK(app.palette().color(QPalette::Window) == c.window);
    CHECK(app.palette().color(QPalette::Base) == c.surface);
    CHECK(app.palette().color(QPalette::Highlight) == c.accent);
    CHECK(app.palette().color(QPalette::HighlightedText) == c.onAccent);
    if (resolved == ResolvedTheme::Dark) {
      CHECK(!app.styleSheet().contains(QStringLiteral("background: #ffffff"),
                                       Qt::CaseInsensitive));
    }
    CHECK(!app.styleSheet().contains(QLatin1Char('$')));
  }

  return EXIT_SUCCESS;
}
