#include "model/Feature.h"
#include "model/GeometryOperation.h"
#include "model/IdGeneration.h"

#include <algorithm>
#include <atomic>
#include <utility>

namespace solidar {
namespace {
std::atomic<FeatureId> g_nextFeatureId{1};
}

Feature::Feature(std::string name) : Feature(nextId(), std::move(name)) {}

Feature::Feature(FeatureId id, std::string name)
    : id_(id == kInvalidFeatureId ? nextId() : id), name_(std::move(name)) {
  if (!detail::explicitIdReservationEnabled()) return;
  reserveId(id_);
}

void Feature::reserveId(FeatureId id) noexcept {
  FeatureId expected = g_nextFeatureId.load(std::memory_order_relaxed);
  while (expected <= id &&
         !g_nextFeatureId.compare_exchange_weak(
             expected, id + 1, std::memory_order_relaxed)) {
  }
}

FeatureId Feature::id() const noexcept { return id_; }
const std::string& Feature::name() const noexcept { return name_; }
void Feature::setName(std::string name) { name_ = std::move(name); }
FeatureState Feature::state() const noexcept { return state_; }
bool Feature::isDirty() const noexcept { return state_ == FeatureState::Dirty; }
bool Feature::isValid() const noexcept { return state_ == FeatureState::Valid; }
bool Feature::isFailed() const noexcept { return state_ == FeatureState::Error; }
const std::string& Feature::error() const noexcept { return failure_.detail; }
const OperationFailure& Feature::failure() const noexcept { return failure_; }
OperationFailureCode Feature::errorCode() const noexcept {
  return failure_.code;
}

FeatureDependencies Feature::dependencies() const { return {}; }
bool Feature::dependsOnSketch(SketchId sketchId) const {
  const auto declared = dependencies();
  return std::find(declared.sketchIds.begin(), declared.sketchIds.end(),
                   sketchId) != declared.sketchIds.end();
}
bool Feature::dependsOnFeature(FeatureId featureId) const {
  const auto declared = dependencies();
  return std::find(declared.featureIds.begin(), declared.featureIds.end(),
                   featureId) != declared.featureIds.end();
}

bool Feature::rebuild(const RebuildContext& context) noexcept {
  bool rebuilt = false;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] { rebuilt = rebuildAtBoundary(context); }, &failure);
  if (completed) return rebuilt;

  try {
    switch (failure.kind) {
      case GeometryFailureKind::OcctException:
        markError(OperationFailureCode::GeometryOperationFailed,
                  "OpenCASCADE feature rebuild failed");
        break;
      case GeometryFailureKind::StandardException:
        markError(OperationFailureCode::GeometryOperationFailed,
                  "Feature rebuild failed with a standard exception");
        break;
      default:
        markError(OperationFailureCode::GeometryOperationFailed,
                  "Feature rebuild failed with an unknown exception");
        break;
    }
  } catch (...) {
  }
  return false;
}

void Feature::setDirty(bool dirty) noexcept {
  if (dirty) {
    state_ = FeatureState::Dirty;
    failure_ = {};
  } else if (state_ == FeatureState::Dirty) {
    state_ = FeatureState::Valid;
  }
}

void Feature::markBlocked(std::string message) {
  markError(std::move(message));
}

void Feature::markBlocked(OperationFailureCode code, std::string message) {
  markError(code == OperationFailureCode::None ? OperationFailureCode::Unknown
                                                : code,
            std::move(message));
}

void Feature::markValid() noexcept {
  state_ = FeatureState::Valid;
  failure_ = {};
}

void Feature::markError(std::string message) {
  markError(OperationFailureCode::Unknown, std::move(message));
}

void Feature::markError(OperationFailureCode code, std::string message) {
  state_ = FeatureState::Error;
  failure_ = {code == OperationFailureCode::None ? OperationFailureCode::Unknown
                                                  : code,
              std::move(message)};
}

FeatureId Feature::nextId() noexcept {
  return g_nextFeatureId.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace solidar
