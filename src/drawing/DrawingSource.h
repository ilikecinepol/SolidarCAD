#pragma once

#include <cstddef>
#include <memory>

class TopoDS_Shape;

namespace solidar {
class Document;

namespace drawing {

struct DrawingSource final {
  std::shared_ptr<TopoDS_Shape> shape;
  std::size_t bodyCount{0};
  std::size_t solidCount{0};
};

// A drawing is a document-level view. Keep every body in the source compound;
// choosing a body for a sketch must not silently remove its neighbours.
[[nodiscard]] DrawingSource collectDrawingSource(const Document& document);

}  // namespace drawing
}  // namespace solidar
