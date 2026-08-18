#pragma once

#include <QPointF>
#include <QString>
#include <vector>

#include "model/Document.h"
#include "model/SolidFeature.h"
#include "sketch/Sketch.h"

namespace solidar::io {

bool exportAsciiStl(const QString& path, const sketch::Sketch& profile,
                    const QString& support, const BoxParameters& box,
                    QPointF bodyPosition,
                    const std::vector<SolidFeature>& features = {},
                    QString* error = nullptr);

}  // namespace solidar::io
