#include "ui/ThemeColors.h"

namespace solidar {

ThemeColors lightThemeColors() {
  ThemeColors c;
  c.window = QColor("#f7f9fc");
  c.surface = QColor("#ffffff");
  c.surfaceAlt = QColor("#f2f5fa");
  c.elevated = QColor("#ffffff");

  c.textPrimary = QColor("#17346f");
  c.textSecondary = QColor("#607493");
  c.textDisabled = QColor("#9ca9bd");

  c.border = QColor("#d8e1ef");
  c.borderStrong = QColor("#b9c8dd");

  c.accent = QColor("#086cff");
  c.accentHover = QColor("#005fdf");
  c.accentPressed = QColor("#0050bd");
  c.accentSoft = QColor("#e5f0ff");
  c.onAccent = QColor("#ffffff");

  c.selection = QColor("#e5f0ff");
  c.selectionText = QColor("#075fdd");
  c.danger = QColor("#c62828");
  c.warning = QColor("#b97a00");
  c.shadow = QColor("#23344c");

  c.viewportBackground = QColor("#f6f9fc");
  c.viewportSurface = QColor("#89939d");
  c.viewportSurfaceLight = QColor("#c4cbd2");
  c.viewportSurfaceDark = QColor("#66717c");
  c.viewportEdge = QColor("#4d5863");
  c.viewportHandle = c.onAccent;
  c.viewportHover = QColor("#00a6ff");
  c.viewportSelection = QColor("#ff8a24");
  c.viewportAngular = QColor("#ef7d00");
  c.previewPositive = QColor("#197df5");
  c.previewNegative = QColor("#e0424c");
  c.timelineTrack = c.textSecondary;

  c.gridMinor = QColor("#e4e9f1");
  c.gridMajor = QColor("#ccd4e0");
  c.axisX = QColor("#c4706c");
  c.axisY = QColor("#6ca26c");
  c.axisZ = QColor("#6a84c8");

  c.sketchBackground = QColor("#fbfcff");
  c.sketchRulerBackground = QColor("#f3f6fb");
  c.sketchDatum = QColor("#596570");
  c.sketchCommitted = QColor("#1469d7");
  c.sketchCommittedLocked = QColor("#8b5cf6");
  c.sketchSelected = QColor("#ff8a24");
  c.sketchSelectedLocked = QColor("#a78bfa");
  c.sketchEndpointFill = QColor("#ffffff");
  c.sketchTrim = QColor("#ff4d4f");
  c.sketchProjection = QColor("#0a72ff");
  c.sketchConstraint = QColor("#1469d7");
  c.sketchDimension = QColor("#315e9d");
  c.sketchDimensionSelected = QColor("#ff8a24");
  c.sketchDimensionText = QColor("#244a82");
  c.sketchDimensionSelectedText = QColor("#d76400");
  c.sketchTransient = QColor("#0872f9");
  c.sketchHud = QColor("#536985");

  c.cubeTop = QColor("#f5f8fc");
  c.cubeFront = QColor("#dbe3ee");
  c.cubeSide = QColor("#bfccdd");
  c.cubeOutline = QColor("#586a82");
  c.cubeText = QColor("#26394f");
  c.cubeBevel = QColor("#f8fbff");
  c.cubeHover = QColor("#c4e0ff");
  c.cubePressed = QColor("#93c4f9");
  c.cubeActive = QColor("#ddebfb");
  c.cubeAccent = QColor("#2874c9");
  c.cubeShadow = QColor("#23344c");
  return c;
}

ThemeColors darkThemeColors() {
  ThemeColors c;
  c.window = QColor("#1d2127");
  c.surface = QColor("#252a31");
  c.surfaceAlt = QColor("#2d333b");
  c.elevated = QColor("#323943");

  c.textPrimary = QColor("#e8ecf2");
  c.textSecondary = QColor("#aeb7c4");
  c.textDisabled = QColor("#707986");

  c.border = QColor("#3b434e");
  c.borderStrong = QColor("#505a68");

  c.accent = QColor("#3d8dff");
  c.accentHover = QColor("#60a2ff");
  c.accentPressed = QColor("#2577e8");
  c.accentSoft = QColor("#25344a");
  c.onAccent = QColor("#ffffff");

  c.selection = QColor("#2b3c55");
  c.selectionText = QColor("#cfe4ff");
  c.danger = QColor("#ef7a70");
  c.warning = QColor("#e0a53d");
  c.shadow = QColor("#000000");

  c.viewportBackground = QColor("#1b2026");
  c.viewportSurface = QColor("#697583");
  c.viewportSurfaceLight = QColor("#8b98a6");
  c.viewportSurfaceDark = QColor("#454f5b");
  c.viewportEdge = QColor("#a1acb8");
  c.viewportHandle = c.onAccent;
  c.viewportHover = QColor("#52c3ff");
  c.viewportSelection = QColor("#ffad66");
  c.viewportAngular = QColor("#ffad66");
  c.previewPositive = c.accent;
  c.previewNegative = c.danger;
  c.timelineTrack = c.textSecondary;

  c.gridMinor = QColor("#272d35");
  c.gridMajor = QColor("#333b46");
  c.axisX = QColor("#b06b67");
  c.axisY = QColor("#6ba06b");
  c.axisZ = QColor("#7a90c8");

  c.sketchBackground = c.viewportBackground;
  c.sketchRulerBackground = c.surface;
  c.sketchDatum = c.textSecondary;
  c.sketchCommitted = c.accentHover;
  c.sketchCommittedLocked = QColor("#b6a0ff");
  c.sketchSelected = QColor("#ffad66");
  c.sketchSelectedLocked = QColor("#d0c4ff");
  c.sketchEndpointFill = c.surface;
  c.sketchTrim = c.danger;
  c.sketchProjection = c.accent;
  c.sketchConstraint = c.accentHover;
  c.sketchDimension = QColor("#8fbbff");
  c.sketchDimensionSelected = c.warning;
  c.sketchDimensionText = c.textPrimary;
  c.sketchDimensionSelectedText = QColor("#ffc56a");
  c.sketchTransient = c.accent;
  c.sketchHud = c.textSecondary;

  c.cubeTop = QColor("#2c333c");
  c.cubeFront = QColor("#3b434e");
  c.cubeSide = QColor("#232930");
  c.cubeOutline = QColor("#9aa6b4");
  c.cubeText = QColor("#e8ecf2");
  c.cubeBevel = QColor("#46505b");
  c.cubeHover = QColor("#33507a");
  c.cubePressed = QColor("#3d6db0");
  c.cubeActive = QColor("#2f4057");
  c.cubeAccent = QColor("#5ea2ff");
  c.cubeShadow = QColor("#000000");
  return c;
}

}  // namespace solidar
