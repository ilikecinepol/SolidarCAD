#pragma once

#include <QColor>

namespace solidar {

// Semantic application colors. No magic values live in widget paint code; the
// ThemeManager owns the light/dark variants and widgets refer to these names.
struct ThemeColors {
  QColor window;
  QColor surface;
  QColor surfaceAlt;
  QColor elevated;

  QColor textPrimary;
  QColor textSecondary;
  QColor textDisabled;

  QColor border;
  QColor borderStrong;

  QColor accent;
  QColor accentHover;
  QColor accentPressed;
  QColor accentSoft;

  QColor selection;
  QColor selectionText;
  QColor danger;

  QColor viewportBackground;

  QColor gridMinor;
  QColor gridMajor;
  QColor axisX;
  QColor axisY;
  QColor axisZ;

  QColor cubeTop;
  QColor cubeFront;
  QColor cubeSide;
  QColor cubeOutline;
  QColor cubeText;
  QColor cubeBevel;
  QColor cubeHover;
  QColor cubePressed;
  QColor cubeActive;
  QColor cubeAccent;
  QColor cubeShadow;
};

ThemeColors lightThemeColors();
ThemeColors darkThemeColors();

}  // namespace solidar
