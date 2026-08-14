#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace solidar {

using FeatureId = std::uint64_t;

enum class FeatureType { Sketch, Extrude };

struct Feature {
  FeatureId id{};
  FeatureType type{FeatureType::Sketch};
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

  [[nodiscard]] const std::vector<Feature>& features() const noexcept;
  [[nodiscard]] const BoxParameters& box() const noexcept;
  void setBox(BoxParameters parameters);

 private:
  std::vector<Feature> features_;
  BoxParameters box_;
};

}  // namespace solidar
