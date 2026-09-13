#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "model/Document.h"
#include "model/ExtrudeFeature.h"

namespace solidar {

enum class ContextActionKind { Extrude };

struct SketchProfileSelectionContext {
  DocumentSketch profile;
  BodyId activeBodyId{kInvalidBodyId};
  FeatureId activeFeatureId{kInvalidFeatureId};
  ShapeFeature::ShapePtr activeBodyShape;
};

struct ContextActionCapability {
  ContextActionKind kind{ContextActionKind::Extrude};
  DocumentSketch profile;
  BodyId bodyId{kInvalidBodyId};
  FeatureId sourceFeatureId{kInvalidFeatureId};
  ShapeFeature::ShapePtr baseShape;
  ExtrudeOperation operation{ExtrudeOperation::NewBody};
  bool operationFollowsDirection{};
};

class ContextActionRegistry final {
 public:
  using Provider = std::function<std::optional<ContextActionCapability>(
      const SketchProfileSelectionContext&)>;
  void addProvider(Provider provider);
  [[nodiscard]] std::optional<ContextActionCapability> resolve(
      const SketchProfileSelectionContext& context) const;
 private:
  std::vector<Provider> providers_;
};

[[nodiscard]] std::optional<ContextActionCapability>
resolveSketchProfileExtrude(const SketchProfileSelectionContext& context);

enum class DirectInteractionState {
  Idle, Hovering, Selected, Ready, Dragging, NumericEditing,
  PreviewInvalid, Committing, Cancelling
};

class DirectInteractionController final {
 public:
  void hover(bool available) noexcept;
  void selected() noexcept;
  void ready(bool previewValid) noexcept;
  void dragging() noexcept;
  void numericEditing() noexcept;
  void previewValidity(bool valid) noexcept;
  void committing() noexcept;
  void cancelling() noexcept;
  void reset() noexcept;
  [[nodiscard]] DirectInteractionState state() const noexcept { return state_; }
 private:
  DirectInteractionState state_{DirectInteractionState::Idle};
};

}  // namespace solidar
