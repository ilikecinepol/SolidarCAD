#pragma once

#include <QIcon>
#include <QString>
#include <vector>

#include "model/Document.h"

class QToolButton;

namespace solidar {

enum class HistoryStepType {
  Sketch, Extrude, Pocket, Revolve, Fillet, Chamfer, Mirror,
  LinearPattern, CircularPattern, Shell, Draft
};

struct HistoryStep {
  HistoryStepType type{HistoryStepType::Sketch};
  SketchId sketchId{kInvalidSketchId};
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  QString title;
  QString tooltip;
  QIcon icon;
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
