#pragma once

#include <QIcon>
#include <QString>
#include <vector>

#include "model/Document.h"
#include "ui/FeatureUiRegistry.h"

class QToolButton;

namespace solidar {

struct HistoryStep {
  HistoryStepType type{HistoryStepType::Sketch};
  SketchId sketchId{kInvalidSketchId};
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  QString title;
  QString tooltip;
  QIcon icon;
  bool editable{true};
  FeatureState state{FeatureState::Valid};
  ShapeFeature::ShapePtr shape;
};

[[nodiscard]] std::vector<HistoryStep> buildPartDesignHistory(
    const Document& document, const Body& body);
[[nodiscard]] std::vector<HistoryStep> buildPartDesignHistory(
    const Document& document, const Body* body);
// Presentation policy: profile sketches consumed by solid-creating Part Design
// features are hidden by default, but remain in Document/history and may be
// shown explicitly by the user.
[[nodiscard]] bool isSketchConsumedByPartDesign(
    const Document& document, SketchId sketchId) noexcept;
void configureHistoryButton(QToolButton& button, const HistoryStep& step,
                            bool selected);
[[nodiscard]] ShapeFeature* findHistoryFeature(Document& document,
                                               BodyId bodyId,
                                               FeatureId featureId) noexcept;
[[nodiscard]] const ShapeFeature* findHistoryFeature(
    const Document& document, BodyId bodyId, FeatureId featureId) noexcept;

}  // namespace solidar
