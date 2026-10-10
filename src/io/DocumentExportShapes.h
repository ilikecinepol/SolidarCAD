#pragma once

#include <vector>

#include <QString>

#include "model/ShapeFeature.h"

namespace solidar {
class Document;
}

namespace solidar::io::detail {

// Collects a complete, validated export snapshot. Truly empty Bodies are
// ignored, but a Body with history and no current valid result invalidates the
// whole snapshot so exporters can never silently emit a partial document.
[[nodiscard]] bool collectDocumentExportShapes(
    const Document& document, std::vector<ShapeFeature::ShapePtr>* shapes,
    QString* error = nullptr);

}  // namespace solidar::io::detail

namespace solidar::io {

// Read-only preflight shared by export commands and UI enablement. It applies
// the same complete-document policy as the collectors without allocating or
// retaining a shape snapshot.
[[nodiscard]] bool hasExportableDocumentShapes(
    const Document& document, QString* error = nullptr);

}  // namespace solidar::io
