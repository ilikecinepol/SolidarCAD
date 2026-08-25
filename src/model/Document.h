#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model/Body.h"
#include "sketch/Sketch.h"

namespace solidar {

using SketchId = std::uint64_t;
inline constexpr SketchId kInvalidSketchId = 0;

struct DocumentSketch {
  SketchId id{kInvalidSketchId};
  std::string name;
  sketch::Sketch geometry;
};

struct BoxParameters {
  double widthMm{60.0};
  double depthMm{40.0};
  double heightMm{25.0};
};

class Document final {
 public:
  Document();

  DocumentSketch& addSketch(std::string name = {});
  DocumentSketch& addSketch(SketchId id, std::string name,
                            sketch::Sketch geometry = {});
  [[nodiscard]] std::vector<DocumentSketch>& sketches() noexcept;
  [[nodiscard]] const std::vector<DocumentSketch>& sketches() const noexcept;
  [[nodiscard]] DocumentSketch* findSketch(SketchId id) noexcept;
  [[nodiscard]] const DocumentSketch* findSketch(SketchId id) const noexcept;

  Body& addBody(std::string name = {});
  Body& addBody(BodyId id, std::string name);
  [[nodiscard]] std::vector<Body>& bodies() noexcept;
  [[nodiscard]] const std::vector<Body>& bodies() const noexcept;
  [[nodiscard]] Body* findBody(BodyId id) noexcept;
  [[nodiscard]] const Body* findBody(BodyId id) const noexcept;
  [[nodiscard]] Body* activeBody() noexcept;
  [[nodiscard]] const Body* activeBody() const noexcept;
  [[nodiscard]] bool rebuild();
  [[nodiscard]] const BoxParameters& box() const noexcept;
  void setBox(BoxParameters parameters);

 private:
  static SketchId nextSketchId() noexcept;

  std::vector<DocumentSketch> sketches_;
  std::vector<Body> bodies_;
  BoxParameters box_;
};

}  // namespace solidar
