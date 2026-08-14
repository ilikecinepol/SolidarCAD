#include "model/Document.h"

#include <stdexcept>

namespace solidar {

Document::Document()
    : features_{{1, FeatureType::Sketch, "Sketch 1"},
                {2, FeatureType::Extrude, "Extrude 1"}} {}

const std::vector<Feature>& Document::features() const noexcept {
  return features_;
}

const BoxParameters& Document::box() const noexcept { return box_; }

void Document::setBox(BoxParameters parameters) {
  if (parameters.widthMm <= 0.0 || parameters.depthMm <= 0.0 ||
      parameters.heightMm <= 0.0) {
    throw std::invalid_argument("Solid dimensions must be positive");
  }
  box_ = parameters;
}

}  // namespace solidar
