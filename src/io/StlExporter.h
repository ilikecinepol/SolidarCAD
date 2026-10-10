#pragma once

#include <QString>

#include "model/Document.h"

namespace solidar::io {

namespace detail {

// BRepMesh_IncrementalMesh reports a bit mask independently from IsDone().
// Keep the acceptance policy testable without exposing OCCT types here.
[[nodiscard]] bool isAcceptableStlMeshingStatus(int statusFlags) noexcept;

}  // namespace detail

// Export the authoritative final B-Rep of every Body in the Document.
// This is the production STL path. It preserves fillets, chamfers, cuts,
// revolves, shells, drafts, mirrors and patterns exactly as rebuilt by OCCT.
bool exportDocumentAsciiStl(const QString& path, const Document& document,
                            QString* error = nullptr);

}  // namespace solidar::io
