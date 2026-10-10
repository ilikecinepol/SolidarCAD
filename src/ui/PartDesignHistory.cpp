#include "ui/PartDesignHistory.h"

#include <QHash>
#include <QToolButton>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "ui/PartDesignToolHelp.h"
#include "ui/FeatureUiRegistry.h"
#include "ui/ToolIcon.h"

namespace solidar {
namespace {

QString stateText(FeatureState state) {
  if (state == FeatureState::Valid) return QStringLiteral("Valid");
  if (state == FeatureState::Dirty) return QStringLiteral("Dirty");
  return QStringLiteral("Error");
}

HistoryStep featureStep(const Body& body, const ShapeFeature& feature,
                        QHash<int, int>& counts) {
  HistoryStep step;
  step.bodyId = body.id(); step.featureId = feature.id();
  step.state = feature.state();
  step.shape = feature.lastValidShape();
  const auto* descriptor = featureUiDescriptor(feature.kind());
  if (!descriptor) {
    step.type = HistoryStepType::Unknown;
    step.title = QString::fromUtf8("Неизвестная операция");
    step.editable = false;
  } else {
    step.type = descriptor->historyType;
    step.editable = descriptor->editable;
  }
  const QStringList parameters = descriptor
                                     ? formatFeatureParameters(*descriptor,
                                                               feature)
                                     : QStringList{};
  const int number = ++counts[static_cast<int>(step.type)];
  if (descriptor) {
    step.title = descriptor->title() + QStringLiteral(" %1").arg(number);
    step.icon = toolIcon(descriptor->iconKind);
  }
  step.tooltip = step.title;
  if (!parameters.isEmpty()) step.tooltip += QLatin1Char('\n') + parameters.join(QLatin1Char('\n'));
  step.tooltip += QString::fromUtf8("\nСостояние: %1").arg(stateText(step.state));
  if (!feature.error().empty())
    step.tooltip += QString::fromUtf8("\nДиагностика: ") + QString::fromStdString(feature.error());
  if (step.editable)
    step.tooltip += QString::fromUtf8("\nНажмите для редактирования");
  return step;
}

}  // namespace

std::vector<HistoryStep> buildPartDesignHistory(const Document& document,
                                                const Body& body) {
  return buildPartDesignHistory(document, &body);
}

std::vector<HistoryStep> buildPartDesignHistory(const Document& document,
                                                const Body* body) {
  std::vector<HistoryStep> result;
  QHash<int, int> counts;
  QHash<SketchId, bool> emitted;
  std::unordered_map<FeatureId, FeatureDependencies> declaredByFeature;
  std::unordered_map<BodyId, std::unordered_set<SketchId>> sketchesByBody;
  for (const auto& candidateBody : document.bodies())
    for (const auto& feature : candidateBody.features()) {
      auto declared = feature->dependencies();
      auto& bodySketches = sketchesByBody[candidateBody.id()];
      bodySketches.insert(declared.sketchIds.begin(), declared.sketchIds.end());
      declaredByFeature.emplace(feature->id(), std::move(declared));
    }
  auto addSketch = [&](const DocumentSketch& sketch) {
    if (emitted.value(sketch.id, false)) return;
    HistoryStep step;
    step.type = HistoryStepType::Sketch; step.sketchId = sketch.id;
    step.bodyId = body ? body->id() : kInvalidBodyId;
    step.title = QString::fromUtf8("Эскиз %1").arg(
        ++counts[static_cast<int>(HistoryStepType::Sketch)]);
    step.tooltip = step.title + QString::fromUtf8("\nНажмите для редактирования");
    step.icon = modelCommandIcon(QStringLiteral("createSketch"));
    result.push_back(std::move(step)); emitted[sketch.id] = true;
  };
  if (!body) {
    for (const auto& sketch : document.sketches()) addSketch(sketch);
    return result;
  }
  for (const auto& feature : body->features()) {
    const auto& declared = declaredByFeature.at(feature->id());
    for (const auto& sketch : document.sketches())
      if (std::find(declared.sketchIds.begin(), declared.sketchIds.end(),
                    sketch.id) != declared.sketchIds.end())
        addSketch(sketch);
    result.push_back(featureStep(*body, *feature, counts));
    for (const auto& sketch : document.sketches())
      if (sketch.support.type == SketchSupportType::Face &&
          sketch.support.face.featureId == feature->id()) {
        const auto bodySketches = sketchesByBody.find(body->id());
        if (bodySketches == sketchesByBody.end() ||
            !bodySketches->second.contains(sketch.id))
          addSketch(sketch);
      }
  }
  for (const auto& sketch : document.sketches()) {
    bool usedByAnotherBody = false;
    for (const auto& candidateBody : document.bodies()) {
      if (candidateBody.id() == body->id()) continue;
      const auto candidateSketches = sketchesByBody.find(candidateBody.id());
      usedByAnotherBody = candidateSketches != sketchesByBody.end() &&
                          candidateSketches->second.contains(sketch.id);
      if (usedByAnotherBody) break;
    }
    if (!usedByAnotherBody) addSketch(sketch);
  }
  return result;
}

bool isSketchConsumedByPartDesign(const Document& document,
                                  SketchId sketchId) noexcept {
  if (sketchId == kInvalidSketchId) return false;

  for (const Body& body : document.bodies()) {
    for (const auto& feature : body.features()) {
      if (!feature) continue;

      // "Consumed" is intentionally narrower than dependsOnSketch(): e.g. a
      // Revolve axis sketch is a dependency but is not the profile that should
      // be auto-hidden after the solid feature is created.
      const auto* descriptor = featureUiDescriptor(feature->kind());
      if (descriptor && featureConsumesSketch(*descriptor, *feature, sketchId))
        return true;
    }
  }
  return false;
}

void configureHistoryButton(QToolButton& button, const HistoryStep& step,
                            bool selected) {
  button.setObjectName(QStringLiteral("historyStep"));
  button.setIcon(step.icon); button.setIconSize(QSize(18, 18));
  button.setText({}); button.setToolTip(step.tooltip);
  button.setAccessibleName(step.title);
  button.setToolButtonStyle(Qt::ToolButtonIconOnly);
  button.setFixedSize(28, 28); button.setCheckable(true);
  button.setChecked(selected); button.setCursor(Qt::PointingHandCursor);
  button.setProperty("stateRole", step.state == FeatureState::Error
                                      ? QStringLiteral("error")
                                      : step.state == FeatureState::Dirty
                                            ? QStringLiteral("dirty")
                                            : QString());
}

ShapeFeature* findHistoryFeature(Document& document, BodyId bodyId,
                                 FeatureId featureId) noexcept {
  Body* body = document.findBody(bodyId);
  if (!body) return nullptr;
  for (const auto& feature : body->features())
    if (feature->id() == featureId) return feature.get();
  return nullptr;
}

const ShapeFeature* findHistoryFeature(const Document& document, BodyId bodyId,
                                       FeatureId featureId) noexcept {
  const Body* body = document.findBody(bodyId);
  if (!body) return nullptr;
  for (const auto& feature : body->features())
    if (feature->id() == featureId) return feature.get();
  return nullptr;
}

}  // namespace solidar
