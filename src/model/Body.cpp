#include "model/Body.h"
#include "model/Document.h"

#include <atomic>
#include <stdexcept>
#include <utility>

namespace solidar {
namespace {
std::atomic<BodyId> g_nextBodyId{1};
}

Body::Body(std::string name) : Body(nextId(), std::move(name)) {}

Body::Body(BodyId id, std::string name)
    : id_(id == kInvalidBodyId ? nextId() : id), name_(std::move(name)) {
  BodyId expected = g_nextBodyId.load(std::memory_order_relaxed);
  while (expected <= id_ &&
         !g_nextBodyId.compare_exchange_weak(
             expected, id_ + 1, std::memory_order_relaxed)) {
  }
}

Body::Body(const Body& other) : Body(other.id_, other.name_) {
  features_.reserve(other.features_.size());
  for (const auto& feature : other.features_) {
    auto copy = feature->clone();
    auto* shapeCopy = dynamic_cast<ShapeFeature*>(copy.get());
    if (!shapeCopy)
      throw std::logic_error("ShapeFeature::clone returned incompatible type");
    copy.release();
    features_.emplace_back(shapeCopy);
  }
}

Body& Body::operator=(const Body& other) {
  if (this == &other) return *this;
  Body copy(other);
  *this = std::move(copy);
  return *this;
}

BodyId Body::id() const noexcept { return id_; }
const std::string& Body::name() const noexcept { return name_; }
void Body::setName(std::string name) { name_ = std::move(name); }

ShapeFeature& Body::addFeature(std::unique_ptr<ShapeFeature> feature) {
  if (!feature) throw std::invalid_argument("Body feature cannot be null");
  features_.push_back(std::move(feature));
  return *features_.back();
}

const std::vector<std::unique_ptr<ShapeFeature>>& Body::features() const noexcept {
  return features_;
}

ShapeFeature* Body::activeFeature() noexcept {
  return features_.empty() ? nullptr : features_.back().get();
}

const ShapeFeature* Body::activeFeature() const noexcept {
  return features_.empty() ? nullptr : features_.back().get();
}

ShapeFeature::ShapePtr Body::resultShape() const noexcept {
  const auto* active = activeFeature();
  return active && active->isValid() ? active->shape() : ShapeFeature::ShapePtr{};
}

bool Body::rebuild(const RebuildContext& context) {
  ShapeFeature::ShapePtr previousShape;
  bool upstreamDirty = false;
  for (std::size_t index = 0; index < features_.size(); ++index) {
    auto& feature = features_[index];
    // Retry failed features as well. Otherwise an Error feature restored
    // without its diagnostic is rejected below before its builder can provide
    // a useful current error.
    upstreamDirty = upstreamDirty || feature->isDirty() || feature->isFailed();
    if (upstreamDirty) feature->setDirty();
    const RebuildContext featureContext{context.document, this,
                                        previousShape.get()};
    if (feature->isDirty() && !feature->rebuild(featureContext)) {
      markDirtyFrom(index + 1);
      return false;
    }
    if (!feature->isValid()) {
      markDirtyFrom(index + 1);
      return false;
    }
    previousShape = feature->shape();
    // Attachments to this freshly rebuilt feature must move before the next
    // feature consumes their sketches (Extrude -> face Sketch -> Pocket).
    context.document.updateSketchPlacements();
  }
  return true;
}

void Body::markDirtyFrom(std::size_t index) noexcept {
  for (; index < features_.size(); ++index) features_[index]->setDirty();
}

BodyId Body::nextId() noexcept {
  return g_nextBodyId.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace solidar
