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

bool Document::replaceSketchGeometry(SketchId id, sketch::Sketch geometry) {
  auto* target = findSketch(id);
  if (!target) return false;
  target->geometry = std::move(geometry);
  markSketchDirty(id);
  return true;
}

bool Document::markSketchDirty(SketchId id) noexcept {
  if (!findSketch(id)) return false;
  bool affected = false;
  for (auto& body : bodies_) {
    const auto& features = body.features();
    for (std::size_t index = 0; index < features.size(); ++index) {
      if (!features[index]->dependsOnSketch(id)) continue;
      body.markDirtyFrom(index);
      affected = true;
      break;
    }
  }
  return affected;
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
  const RebuildContext context{*this};
  for (auto& body : bodies_)
    if (!body.rebuild(context)) return false;
  return true;
}

bool Document::recompute() { return rebuild(); }

bool Document::recomputeFrom(FeatureId featureId) {
  if (featureId == kInvalidFeatureId) return false;
  bool found = false;
  for (auto& body : bodies_) {
    const auto& features = body.features();
    for (std::size_t index = 0; index < features.size(); ++index) {
      if (features[index]->id() != featureId) continue;
      body.markDirtyFrom(index);
      found = true;
      break;
    }
  }
  return found && recompute();
}

bool Document::attachSketchToFace(SketchId sketchId, FaceReference reference) {
  auto* sketch = findSketch(sketchId);
  if (!sketch) return false;
  sketch->support = {SketchSupportType::Face, reference};
  updateSketchPlacements();
  return sketch->supportResolved;
}

void Document::updateSketchPlacements() {
  for (auto& sketch : sketches_) {
    if (sketch.support.type != SketchSupportType::Face) continue;
    const auto& reference = sketch.support.face;
    const Body* body = findBody(reference.bodyId);
    const ShapeFeature* feature = nullptr;
    if (body) {
      for (const auto& candidate : body->features())
        if (candidate->id() == reference.featureId) {
          feature = candidate.get();
          break;
        }
    }
    if (!feature || !feature->isValid() || !feature->shape()) {
      sketch.supportResolved = false;
      continue;
    }
    const auto resolved =
        resolveFacePlacement(*feature->shape(), reference.topology());
    sketch.supportResolved = resolved.planar;
    if (resolved.planar) sketch.placement = resolved.placement;
  }
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
