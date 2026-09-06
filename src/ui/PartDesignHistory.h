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
void configureHistoryButton(QToolButton& button, const HistoryStep& step,
                            bool selected);
[[nodiscard]] ShapeFeature* findHistoryFeature(Document& document,
                                               BodyId bodyId,
                                               FeatureId featureId) noexcept;
[[nodiscard]] const ShapeFeature* findHistoryFeature(
    const Document& document, BodyId bodyId, FeatureId featureId) noexcept;

}  // namespace solidar
