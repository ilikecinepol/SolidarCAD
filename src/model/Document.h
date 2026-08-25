#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model/Body.h"

namespace solidar {

enum class DocumentFeatureType { Sketch, Extrude };

struct DocumentFeature {
  FeatureId id{};
  DocumentFeatureType type{DocumentFeatureType::Sketch};
  std::string name;
};

struct BoxParameters {
  double widthMm{60.0};
  double depthMm{40.0};
  double heightMm{25.0};
};

class Document final {
 public:
  Document();

  [[nodiscard]] const std::vector<DocumentFeature>& features() const noexcept;
  Body& addBody(std::string name = {});
  [[nodiscard]] std::vector<Body>& bodies() noexcept;
  [[nodiscard]] const std::vector<Body>& bodies() const noexcept;
  [[nodiscard]] Body* activeBody() noexcept;
  [[nodiscard]] const Body* activeBody() const noexcept;
  [[nodiscard]] bool rebuild();
  [[nodiscard]] const BoxParameters& box() const noexcept;
  void setBox(BoxParameters parameters);

 private:
  std::vector<DocumentFeature> features_;
  std::vector<Body> bodies_;
  BoxParameters box_;
};

}  // namespace solidar
