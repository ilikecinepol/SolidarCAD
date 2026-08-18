#pragma once

#include <QString>

#include "sketch/Sketch.h"

namespace solidar {

struct SolidFeature {
  sketch::Sketch geometry;
  QString supportName;
  double startMm{0.0};
  double lengthMm{0.0};
};

}  // namespace solidar
