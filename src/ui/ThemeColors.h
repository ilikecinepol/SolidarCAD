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
  QColor onAccent;

  QColor selection;
  QColor selectionText;
  QColor danger;
  QColor warning;
  QColor shadow;

  QColor viewportBackground;
  QColor viewportSurface;
  QColor viewportSurfaceLight;
  QColor viewportSurfaceDark;
  QColor viewportEdge;
  QColor viewportHandle;
  QColor viewportHover;
  QColor viewportSelection;
  QColor viewportAngular;
  QColor previewPositive;
  QColor previewNegative;
  QColor timelineTrack;

  QColor gridMinor;
  QColor gridMajor;
  QColor axisX;
  QColor axisY;
  QColor axisZ;

  // Sketcher semantic paint roles. Keeping these in the global theme contract
  // gives Light, Dark and System-resolved rendering the same ownership model
  // as the rest of the application.
  QColor sketchBackground;
  QColor sketchRulerBackground;
  QColor sketchDatum;
  QColor sketchCommitted;
  QColor sketchCommittedLocked;
  QColor sketchSelected;
  QColor sketchSelectedLocked;
  QColor sketchEndpointFill;
  QColor sketchTrim;
  QColor sketchProjection;
  QColor sketchConstraint;
  QColor sketchDimension;
  QColor sketchDimensionSelected;
  QColor sketchDimensionText;
  QColor sketchDimensionSelectedText;
  QColor sketchTransient;
  QColor sketchHud;

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
