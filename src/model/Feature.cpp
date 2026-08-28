#include "model/Feature.h"

#include <atomic>
#include <utility>

namespace solidar {
namespace {
std::atomic<FeatureId> g_nextFeatureId{1};
}

Feature::Feature(std::string name) : Feature(nextId(), std::move(name)) {}

Feature::Feature(FeatureId id, std::string name)
    : id_(id == kInvalidFeatureId ? nextId() : id), name_(std::move(name)) {
  FeatureId expected = g_nextFeatureId.load(std::memory_order_relaxed);
  while (expected <= id_ &&
         !g_nextFeatureId.compare_exchange_weak(
             expected, id_ + 1, std::memory_order_relaxed)) {
  }
}

FeatureId Feature::id() const noexcept { return id_; }
const std::string& Feature::name() const noexcept { return name_; }
void Feature::setName(std::string name) { name_ = std::move(name); }
FeatureState Feature::state() const noexcept { return state_; }
bool Feature::isDirty() const noexcept { return state_ == FeatureState::Dirty; }
bool Feature::isValid() const noexcept { return state_ == FeatureState::Valid; }
bool Feature::isFailed() const noexcept { return state_ == FeatureState::Error; }
const std::string& Feature::error() const noexcept { return error_; }

bool Feature::dependsOnSketch(SketchId) const noexcept { return false; }

void Feature::setDirty(bool dirty) noexcept {
  if (dirty) {
    state_ = FeatureState::Dirty;
    error_.clear();
  } else if (state_ == FeatureState::Dirty) {
    state_ = FeatureState::Valid;
  }
}

void Feature::markValid() noexcept {
  state_ = FeatureState::Valid;
  error_.clear();
}

void Feature::markError(std::string message) {
  state_ = FeatureState::Error;
  error_ = std::move(message);
}

FeatureId Feature::nextId() noexcept {
  return g_nextFeatureId.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace solidar
