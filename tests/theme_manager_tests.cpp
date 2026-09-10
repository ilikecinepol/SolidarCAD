#include <QColor>

#include <cstdlib>
#include <iostream>

#include "ui/ThemeColors.h"
#include "ui/ThemeManager.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

int main() {
  using namespace solidar;

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
    CHECK(c.textDisabled != c.textPrimary);
    CHECK(c.viewportBackground.isValid());
    CHECK(c.gridMinor.isValid() && c.gridMajor.isValid());
    CHECK(c.axisX != c.axisY && c.axisY != c.axisZ && c.axisX != c.axisZ);
    CHECK(c.cubeTop != c.cubeSide);
    CHECK(c.cubeFront != c.cubeSide);
  }

  return EXIT_SUCCESS;
}
