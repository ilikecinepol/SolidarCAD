#include "model/Document.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace solidar {
namespace {
std::atomic<SketchId> g_nextSketchId{1};

template <typename Id>
void advancePast(std::atomic<Id>& next, Id used) noexcept {
  Id expected = next.load(std::memory_order_relaxed);
  while (expected <= used &&
         !next.compare_exchange_weak(expected, used + 1,
                                     std::memory_order_relaxed)) {
  }
}
}  // namespace

Document::Document() = default;

DocumentSketch& Document::addSketch(std::string name) {
  return addSketch(nextSketchId(), std::move(name));
}

DocumentSketch& Document::addSketch(SketchId id, std::string name,
                                    sketch::Sketch geometry) {
  if (id == kInvalidSketchId) id = nextSketchId();
  if (findSketch(id)) throw std::invalid_argument("Duplicate SketchId");
  advancePast(g_nextSketchId, id);
  if (name.empty()) name = "Sketch " + std::to_string(sketches_.size() + 1);
  sketches_.push_back({id, std::move(name), std::move(geometry)});
  return sketches_.back();
}

std::vector<DocumentSketch>& Document::sketches() noexcept { return sketches_; }
const std::vector<DocumentSketch>& Document::sketches() const noexcept {
  return sketches_;
}

DocumentSketch* Document::findSketch(SketchId id) noexcept {
  const auto found = std::find_if(sketches_.begin(), sketches_.end(),
                                  [id](const auto& item) { return item.id == id; });
  return found == sketches_.end() ? nullptr : &*found;
}

const DocumentSketch* Document::findSketch(SketchId id) const noexcept {
  const auto found = std::find_if(sketches_.begin(), sketches_.end(),
                                  [id](const auto& item) { return item.id == id; });
  return found == sketches_.end() ? nullptr : &*found;
}

Body& Document::addBody(std::string name) {
  if (name.empty()) name = "Body " + std::to_string(bodies_.size() + 1);
  bodies_.emplace_back(std::move(name));
  return bodies_.back();
}

Body& Document::addBody(BodyId id, std::string name) {
  if (id != kInvalidBodyId && findBody(id))
    throw std::invalid_argument("Duplicate BodyId");
  if (name.empty()) name = "Body " + std::to_string(bodies_.size() + 1);
  bodies_.emplace_back(id, std::move(name));
  return bodies_.back();
}

std::vector<Body>& Document::bodies() noexcept { return bodies_; }
const std::vector<Body>& Document::bodies() const noexcept { return bodies_; }
Body* Document::findBody(BodyId id) noexcept {
  const auto found = std::find_if(bodies_.begin(), bodies_.end(),
                                  [id](const auto& body) { return body.id() == id; });
  return found == bodies_.end() ? nullptr : &*found;
}
const Body* Document::findBody(BodyId id) const noexcept {
  const auto found = std::find_if(bodies_.begin(), bodies_.end(),
                                  [id](const auto& body) { return body.id() == id; });
  return found == bodies_.end() ? nullptr : &*found;
}
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

SketchId Document::nextSketchId() noexcept {
  return g_nextSketchId.fetch_add(1, std::memory_order_relaxed);
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
