#include "ui/SketchCanvas.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace solidar;

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

#define CHECK(condition) require((condition), #condition)

void screenAxisToSketchModeAllRotations() {
  for (int q = 0; q < 4; ++q) {
    const bool odd = (q & 1) != 0;
    const QString horizontal =
        SketchCanvas::pointDimensionModeForScreenAxis(true, q);
    const QString vertical =
        SketchCanvas::pointDimensionModeForScreenAxis(false, q);
    CHECK(horizontal ==
          (odd ? QStringLiteral("y") : QStringLiteral("x")));
    CHECK(vertical ==
          (odd ? QStringLiteral("x") : QStringLiteral("y")));
  }

  // Negative and over-rotation values normalize to the same quadrant.
  CHECK(SketchCanvas::pointDimensionModeForScreenAxis(true, 3) ==
        QStringLiteral("y"));
  CHECK(SketchCanvas::pointDimensionModeForScreenAxis(true, 7) ==
        QStringLiteral("y"));
  CHECK(SketchCanvas::pointDimensionModeForScreenAxis(true, -1) ==
        QStringLiteral("y"));
  CHECK(SketchCanvas::pointDimensionModeForScreenAxis(true, 0) ==
        QStringLiteral("x"));
}

void resolveModeUsesSketchAxisSeparation() {
  // A gap with Sketch-X separation only, requested as a vertical dimension
  // line. At q3 the screen-vertical axis is Sketch X, so it resolves to "x";
  // at q0 the screen-vertical axis is Sketch Y, where there is no separation,
  // so it falls back to aligned.
  CHECK(SketchCanvas::resolvePointDimensionMode(false, true, 12.0, 0.0, 3) ==
        QStringLiteral("x"));
  CHECK(SketchCanvas::resolvePointDimensionMode(false, true, 12.0, 0.0, 0) ==
        QStringLiteral("aligned"));

  // The same gap requested as a horizontal dimension line.
  CHECK(SketchCanvas::resolvePointDimensionMode(true, false, 12.0, 0.0, 0) ==
        QStringLiteral("x"));
  CHECK(SketchCanvas::resolvePointDimensionMode(true, false, 12.0, 0.0, 3) ==
        QStringLiteral("aligned"));

  // A point pair with both separations maps to the correct Sketch axis at
  // every rotation.
  CHECK(SketchCanvas::resolvePointDimensionMode(true, false, 12.0, 7.0, 0) ==
        QStringLiteral("x"));
  CHECK(SketchCanvas::resolvePointDimensionMode(true, false, 12.0, 7.0, 2) ==
        QStringLiteral("x"));
  CHECK(SketchCanvas::resolvePointDimensionMode(true, false, 12.0, 7.0, 1) ==
        QStringLiteral("y"));
  CHECK(SketchCanvas::resolvePointDimensionMode(true, false, 12.0, 7.0, 3) ==
        QStringLiteral("y"));

  CHECK(SketchCanvas::resolvePointDimensionMode(false, true, 12.0, 7.0, 0) ==
        QStringLiteral("y"));
  CHECK(SketchCanvas::resolvePointDimensionMode(false, true, 12.0, 7.0, 1) ==
        QStringLiteral("x"));
  CHECK(SketchCanvas::resolvePointDimensionMode(false, true, 12.0, 7.0, 2) ==
        QStringLiteral("y"));
  CHECK(SketchCanvas::resolvePointDimensionMode(false, true, 12.0, 7.0, 3) ==
        QStringLiteral("x"));

  // No cursor orientation -> aligned.
  CHECK(SketchCanvas::resolvePointDimensionMode(false, false, 12.0, 7.0, 3) ==
        QStringLiteral("aligned"));
}

void witnessProjectsInSketchCoordinates() {
  const sketch::Point first{3.0, 4.0};
  const sketch::Point second{10.0, 20.0};

  const auto x = SketchCanvas::pointDimensionWitness(
      first, second, sketch::DimensionKind::PointDistanceX);
  CHECK(std::abs(x.first.xMm - 3.0) <= 1e-12 &&
        std::abs(x.first.yMm - 4.0) <= 1e-12);
  CHECK(std::abs(x.second.xMm - 10.0) <= 1e-12 &&
        std::abs(x.second.yMm - 4.0) <= 1e-12);

  const auto y = SketchCanvas::pointDimensionWitness(
      first, second, sketch::DimensionKind::PointDistanceY);
  CHECK(std::abs(y.first.xMm - 3.0) <= 1e-12 &&
        std::abs(y.first.yMm - 4.0) <= 1e-12);
  CHECK(std::abs(y.second.xMm - 3.0) <= 1e-12 &&
        std::abs(y.second.yMm - 20.0) <= 1e-12);

  const auto aligned = SketchCanvas::pointDimensionWitness(
      first, second, sketch::DimensionKind::PointDistance);
  CHECK(std::abs(aligned.first.xMm - 3.0) <= 1e-12 &&
        std::abs(aligned.first.yMm - 4.0) <= 1e-12);
  CHECK(std::abs(aligned.second.xMm - 10.0) <= 1e-12 &&
        std::abs(aligned.second.yMm - 20.0) <= 1e-12);
}

void angularRadiusStaysReadable() {
  CHECK(std::abs(SketchCanvas::angularDimensionRadiusPx(2.0, 10.0, 800.0) -
                 20.0) <= 1e-12);
  CHECK(std::abs(SketchCanvas::angularDimensionRadiusPx(0.0, 10.0, 800.0) -
                 16.0) <= 1e-12);
  CHECK(std::abs(SketchCanvas::angularDimensionRadiusPx(1000.0, 10.0, 800.0) -
                 176.0) <= 1e-12);
  CHECK(std::abs(SketchCanvas::angularDimensionRadiusPx(1000.0, 10.0, 200.0) -
                 48.0) <= 1e-12);
}

}  // namespace

int main() {
  screenAxisToSketchModeAllRotations();
  resolveModeUsesSketchAxisSeparation();
  witnessProjectsInSketchCoordinates();
  angularRadiusStaysReadable();
  return EXIT_SUCCESS;
}
