#include "ui/PartDesignHistory.h"

#include <QHash>
#include <QToolButton>

#include "model/ChamferFeature.h"
#include "model/CircularPatternFeature.h"
#include "model/DraftFeature.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/MirrorFeature.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"
#include "model/ShellFeature.h"
#include "ui/PartDesignToolHelp.h"

namespace solidar {
namespace {

QString stateText(FeatureState state) {
  if (state == FeatureState::Valid) return QStringLiteral("Valid");
  if (state == FeatureState::Dirty) return QStringLiteral("Dirty");
  return QStringLiteral("Error");
}

QString operationText(ExtrudeOperation operation) {
  if (operation == ExtrudeOperation::NewBody) return QString::fromUtf8("Новое тело");
  if (operation == ExtrudeOperation::Join) return QString::fromUtf8("Объединение");
  return QString::fromUtf8("Вырез");
}

QString planeText(MirrorPlane plane) {
  if (plane == MirrorPlane::XY) return QStringLiteral("XY");
  if (plane == MirrorPlane::XZ) return QStringLiteral("XZ");
  return QStringLiteral("YZ");
}

HistoryStep featureStep(const Body& body, const ShapeFeature& feature,
                        QHash<int, int>& counts) {
  HistoryStep step;
  step.bodyId = body.id(); step.featureId = feature.id();
  step.state = feature.state(); step.shape = feature.shape();
  PartDesignToolKind kind = PartDesignToolKind::None;
  QStringList parameters;
  if (const auto* v = dynamic_cast<const ExtrudeFeature*>(&feature)) {
    step.type = HistoryStepType::Extrude; kind = PartDesignToolKind::Extrude;
    parameters << QString::fromUtf8("Длина: %1 мм").arg(v->lengthMm(), 0, 'f', 2)
               << QString::fromUtf8("Операция: %1").arg(operationText(v->operation()));
  } else if (const auto* v = dynamic_cast<const PocketFeature*>(&feature)) {
    step.type = HistoryStepType::Pocket; kind = PartDesignToolKind::Pocket;
    parameters << QString::fromUtf8("Глубина: %1 мм").arg(v->depthMm(), 0, 'f', 2);
  } else if (const auto* v = dynamic_cast<const RevolveFeature*>(&feature)) {
    step.type = HistoryStepType::Revolve; kind = PartDesignToolKind::Revolve;
    parameters << QString::fromUtf8("Угол: %1°").arg(v->angleDeg(), 0, 'f', 2);
  } else if (const auto* v = dynamic_cast<const FilletFeature*>(&feature)) {
    step.type = HistoryStepType::Fillet; kind = PartDesignToolKind::Fillet;
    parameters << QString::fromUtf8("Радиус: %1 мм").arg(v->radiusMm(), 0, 'f', 2)
               << QString::fromUtf8("Рёбер: %1").arg(v->edges().size());
  } else if (const auto* v = dynamic_cast<const ChamferFeature*>(&feature)) {
    step.type = HistoryStepType::Chamfer; kind = PartDesignToolKind::Chamfer;
    parameters << QString::fromUtf8("Размер: %1 мм").arg(v->distanceMm(), 0, 'f', 2)
               << QString::fromUtf8("Рёбер: %1").arg(v->edges().size());
  } else if (const auto* v = dynamic_cast<const MirrorFeature*>(&feature)) {
    step.type = HistoryStepType::Mirror; kind = PartDesignToolKind::Mirror;
    parameters << QString::fromUtf8("Плоскость: %1").arg(planeText(v->plane()));
  } else if (const auto* v = dynamic_cast<const LinearPatternFeature*>(&feature)) {
    step.type = HistoryStepType::LinearPattern; kind = PartDesignToolKind::LinearPattern;
    parameters << QString::fromUtf8("Количество: %1").arg(v->count())
               << QString::fromUtf8("Шаг: %1 мм").arg(v->spacingMm(), 0, 'f', 2);
  } else if (const auto* v = dynamic_cast<const CircularPatternFeature*>(&feature)) {
    step.type = HistoryStepType::CircularPattern; kind = PartDesignToolKind::CircularPattern;
    parameters << QString::fromUtf8("Количество: %1").arg(v->count())
               << QString::fromUtf8("Угол: %1°").arg(v->angleDeg(), 0, 'f', 2);
  } else if (const auto* v = dynamic_cast<const ShellFeature*>(&feature)) {
    step.type = HistoryStepType::Shell; kind = PartDesignToolKind::Shell;
    parameters << QString::fromUtf8("Толщина: %1 мм").arg(v->thicknessMm(), 0, 'f', 2)
               << QString::fromUtf8("Удаляемых граней: %1").arg(v->removedFaces().size())
               << (v->outside() ? QString::fromUtf8("Направление: наружу")
                                : QString::fromUtf8("Направление: внутрь"));
  } else if (const auto* v = dynamic_cast<const DraftFeature*>(&feature)) {
    step.type = HistoryStepType::Draft; kind = PartDesignToolKind::Draft;
    parameters << QString::fromUtf8("Угол: %1°").arg(v->angleDeg(), 0, 'f', 2)
               << QString::fromUtf8("Граней: %1").arg(v->draftedFaces().size());
  }
  const auto* help = partDesignToolHelp(kind);
  const int number = ++counts[static_cast<int>(step.type)];
  step.title = help ? help->title + QStringLiteral(" %1").arg(number)
                    : QString::fromStdString(feature.typeName());
  step.icon = partDesignToolIcon(kind);
  step.tooltip = step.title;
  if (!parameters.isEmpty()) step.tooltip += QLatin1Char('\n') + parameters.join(QLatin1Char('\n'));
  step.tooltip += QString::fromUtf8("\nСостояние: %1").arg(stateText(step.state));
  if (!feature.error().empty())
    step.tooltip += QString::fromUtf8("\nДиагностика: ") + QString::fromStdString(feature.error());
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
    for (const auto& sketch : document.sketches())
      if (feature->dependsOnSketch(sketch.id)) addSketch(sketch);
    result.push_back(featureStep(*body, *feature, counts));
    for (const auto& sketch : document.sketches())
      if (sketch.support.type == SketchSupportType::Face &&
          sketch.support.face.featureId == feature->id()) {
        bool consumed = false;
        for (const auto& candidate : body->features())
          consumed = consumed || candidate->dependsOnSketch(sketch.id);
        if (!consumed) addSketch(sketch);
      }
  }
  for (const auto& sketch : document.sketches()) {
    bool usedByAnotherBody = false;
    for (const auto& candidateBody : document.bodies()) {
      if (candidateBody.id() == body->id()) continue;
      for (const auto& feature : candidateBody.features())
        usedByAnotherBody = usedByAnotherBody || feature->dependsOnSketch(sketch.id);
    }
    if (!usedByAnotherBody) addSketch(sketch);
  }
  return result;
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
  const QString stateRule = step.state == FeatureState::Error
      ? QStringLiteral("border:2px solid #d32f2f;")
      : step.state == FeatureState::Dirty
            ? QStringLiteral("border:2px solid #e6a100;")
            : QStringLiteral("border:1px solid #cfdaea;");
  button.setStyleSheet(
      QStringLiteral("QToolButton{background:#fff;border-radius:6px;padding:3px;") +
      stateRule + QStringLiteral("}"
      "QToolButton:hover{background:#e8f2ff;border:2px solid #6fa8f7;}"
      "QToolButton:checked{background:#d7e9ff;border:2px solid #1671e8;}"));
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
