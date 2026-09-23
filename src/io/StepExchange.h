#pragma once

#include <memory>

#include <QString>

class TopoDS_Shape;

namespace solidar {
class Document;
}

namespace solidar::io {

// Read an exact OCCT B-Rep from STEP. The stream-based implementation keeps
// Windows Unicode paths out of the narrow filename API used by OCCT.
[[nodiscard]] std::shared_ptr<const TopoDS_Shape> readStepFile(
    const QString& path, QString* error = nullptr);

// Append one imported Body atomically. On failure the supplied document is
// unchanged.
bool importDocumentStep(const QString& path, Document* document,
                        const QString& featureName = {},
                        QString* error = nullptr);

// Export the authoritative resultShape() of every Body as AP242 B-Rep.
bool exportDocumentStep(const QString& path, const Document& document,
                        QString* error = nullptr);

}  // namespace solidar::io
