#pragma once

#include <cstddef>
#include <memory>

class TopoDS_Shape;

namespace solidar {
class ComponentDrawing;
class Document;

namespace drawing {

enum class DrawingSourceStatus {
  Collected,
  InvalidDrawing,
  MissingSourceBody,
};

struct DrawingSource final {
  DrawingSourceStatus status{DrawingSourceStatus::Collected};
  std::shared_ptr<TopoDS_Shape> shape;
  std::size_t bodyCount{0};
  std::size_t solidCount{0};

  [[nodiscard]] bool succeeded() const noexcept {
    return status == DrawingSourceStatus::Collected;
  }
};

// A drawing is a document-level view. Keep every body in the source compound;
// choosing a body for a sketch must not silently remove its neighbours.
[[nodiscard]] DrawingSource collectDrawingSource(const Document& document);

// Empty sourceBodyIds means every body in the owning Document. Explicit IDs
// are consumed in canonical authored order. Missing/noncanonical references
// are rejected before a compound is built; the result retains no model
// pointers or lazy references.
[[nodiscard]] DrawingSource collectDrawingSource(
    const ComponentDrawing& drawing, const Document& owningDocument);

}  // namespace drawing
}  // namespace solidar
