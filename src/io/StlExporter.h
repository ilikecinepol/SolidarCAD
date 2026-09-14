#pragma once

#include <QPointF>
#include <QString>
#include <vector>

#include "model/Document.h"
#include "model/SolidFeature.h"
#include "sketch/Sketch.h"

namespace solidar::io {

// Export the authoritative final B-Rep of every Body in the Document.
// This is the production STL path. It preserves fillets, chamfers, cuts,
// revolves, shells, drafts, mirrors and patterns exactly as rebuilt by OCCT.
bool exportDocumentAsciiStl(const QString& path, const Document& document,
                            QString* error = nullptr);

bool exportAsciiStl(const QString& path, const sketch::Sketch& profile,
                    const QString& support, const BoxParameters& box,
                    QPointF bodyPosition,
                    const std::vector<SolidFeature>& features = {},
                    QString* error = nullptr);

}  // namespace solidar::io
