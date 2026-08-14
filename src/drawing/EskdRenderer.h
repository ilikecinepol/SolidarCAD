#pragma once

#include <QRectF>
#include <QString>

#include "sketch/Sketch.h"

class QPainter;

namespace solidar::drawing {

struct TitleBlockData {
  QString designation{"ОФКД.000001.001"};
  QString productName{"Деталь"};
  QString material{"Материал"};
  QString organization{QString::fromUtf8("Солидарность CAD")};
  QString scale{"1:1"};
};

class EskdRenderer final {
 public:
  // Page coordinates are millimetres; the painter transform maps them to output.
  static void renderA4(QPainter& painter, const QRectF& target,
                       const sketch::Sketch& sketch,
                       const TitleBlockData& title = {});
};

}  // namespace solidar::drawing
