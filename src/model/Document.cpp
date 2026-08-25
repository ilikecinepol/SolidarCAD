#include "model/Document.h"

#include <stdexcept>
#include <utility>

namespace solidar {

Document::Document()
    : features_{{1, DocumentFeatureType::Sketch, "Sketch 1"},
                {2, DocumentFeatureType::Extrude, "Extrude 1"}} {}

const std::vector<DocumentFeature>& Document::features() const noexcept {
  return features_;
}

Body& Document::addBody(std::string name) {
  if (name.empty()) name = "Body " + std::to_string(bodies_.size() + 1);
  bodies_.emplace_back(std::move(name));
  return bodies_.back();
}

std::vector<Body>& Document::bodies() noexcept { return bodies_; }
const std::vector<Body>& Document::bodies() const noexcept { return bodies_; }
Body* Document::activeBody() noexcept {
  return bodies_.empty() ? nullptr : &bodies_.back();
}
const Body* Document::activeBody() const noexcept {
  return bodies_.empty() ? nullptr : &bodies_.back();
}

bool Document::rebuild() {
  for (auto& body : bodies_)
    if (!body.rebuild()) return false;
  return true;
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
