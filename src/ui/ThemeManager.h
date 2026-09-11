#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

#include "app/AppTheme.h"
#include "ui/ThemeColors.h"

namespace solidar {

// A concrete theme, after resolving the System preference against the OS
// color scheme. The application always renders through one of these two
// palettes, never through raw native widget styling.
enum class ResolvedTheme { Light, Dark };

// Pure resolution of the user preference against the system scheme.
ResolvedTheme resolveTheme(AppTheme preference, Qt::ColorScheme systemScheme);

// Application-wide theme controller. Owns the resolved palette, the Fusion
// base style and the global stylesheet so every widget reads the same theme
// and no single widget is patched locally. This is presentation only: it has
// no persistence (see AppSettings).
class ThemeManager final {
 public:
  static ThemeManager& instance();

  // Applies the resolved theme to QApplication (style, palette, stylesheet).
  void apply(ResolvedTheme theme);

  [[nodiscard]] ResolvedTheme resolved() const noexcept;
  [[nodiscard]] const ThemeColors& colors() const noexcept;
  [[nodiscard]] const ThemeColors& colors(ResolvedTheme theme) const noexcept;

  // Reads the OS color scheme; returns Light when Qt cannot determine it.
  [[nodiscard]] static Qt::ColorScheme systemColorScheme();

 private:
  ThemeManager() = default;
  static void applyPalette(const ThemeColors& colors);
  static QString buildStylesheet(const ThemeColors& colors);

  ResolvedTheme resolved_{ResolvedTheme::Light};
};

}  // namespace solidar
