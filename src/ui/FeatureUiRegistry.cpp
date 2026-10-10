#include "ui/FeatureUiRegistry.h"

#include <array>

#include "model/ChamferFeature.h"
#include "model/CircularPatternFeature.h"
#include "model/DraftFeature.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/MirrorFeature.h"
#include "model/MoveFeature.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"
#include "model/ShellFeature.h"

namespace solidar {
namespace {

QString operationText(ExtrudeOperation operation) {
  if (operation == ExtrudeOperation::NewBody)
    return QString::fromUtf8("Новое тело");
  if (operation == ExtrudeOperation::Join)
    return QString::fromUtf8("Объединение");
  return QString::fromUtf8("Вырез");
}

QString operationText(PatternOperation operation) {
  return operation == PatternOperation::NewBody
             ? QString::fromUtf8("Новое тело")
             : QString::fromUtf8("Добавление");
}

QString planeText(MirrorPlane plane) {
  if (plane == MirrorPlane::XY) return QStringLiteral("XY");
  if (plane == MirrorPlane::XZ) return QStringLiteral("XZ");
  return QStringLiteral("YZ");
}

template <class Concrete>
const Concrete* descriptorFeature(const ShapeFeature& feature) noexcept {
  return dynamic_cast<const Concrete*>(&feature);
}

QStringList noParameters(const ShapeFeature&) { return {}; }

QStringList extrudeParameters(const ShapeFeature& feature) {
  const auto* value = descriptorFeature<ExtrudeFeature>(feature);
  if (!value) return {};
  return {QString::fromUtf8("Длина: %1 мм").arg(value->lengthMm(), 0, 'f', 2),
          QString::fromUtf8("Операция: %1")
              .arg(operationText(value->operation()))};
}

QStringList pocketParameters(const ShapeFeature& feature) {
  const auto* value = descriptorFeature<PocketFeature>(feature);
  return value ? QStringList{QString::fromUtf8("Глубина: %1 мм")
                                 .arg(value->depthMm(), 0, 'f', 2)}
               : QStringList{};
}

QStringList revolveParameters(const ShapeFeature& feature) {
  const auto* value = descriptorFeature<RevolveFeature>(feature);
  return value ? QStringList{QString::fromUtf8("Угол: %1°")
                                 .arg(value->angleDeg(), 0, 'f', 2)}
               : QStringList{};
}

QStringList filletParameters(const ShapeFeature& feature) {
  const auto* value = descriptorFeature<FilletFeature>(feature);
  if (!value) return {};
  return {QString::fromUtf8("Радиус: %1 мм")
              .arg(value->radiusMm(), 0, 'f', 2),
          QString::fromUtf8("Рёбер: %1").arg(value->edges().size())};
}

QStringList chamferParameters(const ShapeFeature& feature) {
  const auto* value = descriptorFeature<ChamferFeature>(feature);
  if (!value) return {};
  return {QString::fromUtf8("Размер: %1 мм")
              .arg(value->distanceMm(), 0, 'f', 2),
          QString::fromUtf8("Рёбер: %1").arg(value->edges().size())};
}

QStringList joinBodiesParameters(const ShapeFeature& feature) {
  return descriptorFeature<JoinBodiesFeature>(feature)
             ? QStringList{QString::fromUtf8("Исходных тел: 2")}
             : QStringList{};
}

QStringList moveParameters(const ShapeFeature& feature) {
  const auto* value = descriptorFeature<MoveFeature>(feature);
  if (!value) return {};
  const auto offset = value->offsetMm();
  return {QString::fromUtf8("X: %1 мм").arg(offset.x, 0, 'f', 2),
          QString::fromUtf8("Y: %1 мм").arg(offset.y, 0, 'f', 2),
          QString::fromUtf8("Z: %1 мм").arg(offset.z, 0, 'f', 2)};
}

QStringList mirrorParameters(const ShapeFeature& feature) {
  const auto* value = descriptorFeature<MirrorFeature>(feature);
  return value ? QStringList{QString::fromUtf8("Плоскость: %1")
                                 .arg(planeText(value->plane()))}
               : QStringList{};
}

QStringList linearPatternParameters(const ShapeFeature& feature) {
  const auto* value = descriptorFeature<LinearPatternFeature>(feature);
  if (!value) return {};
  return {QString::fromUtf8("Количество: %1").arg(value->count()),
          QString::fromUtf8("Шаг: %1 мм")
              .arg(value->spacingMm(), 0, 'f', 2),
          QString::fromUtf8("Операция: %1")
              .arg(operationText(value->operation()))};
}

QStringList circularPatternParameters(const ShapeFeature& feature) {
  const auto* value = descriptorFeature<CircularPatternFeature>(feature);
  if (!value) return {};
  return {QString::fromUtf8("Количество: %1").arg(value->count()),
          QString::fromUtf8("Угол: %1°")
              .arg(value->angleDeg(), 0, 'f', 2),
          QString::fromUtf8("Операция: %1")
              .arg(operationText(value->operation()))};
}

QStringList shellParameters(const ShapeFeature& feature) {
  const auto* value = descriptorFeature<ShellFeature>(feature);
  if (!value) return {};
  return {QString::fromUtf8("Толщина: %1 мм")
              .arg(value->thicknessMm(), 0, 'f', 2),
          QString::fromUtf8("Удаляемых граней: %1")
              .arg(value->removedFaces().size()),
          value->outside() ? QString::fromUtf8("Направление: наружу")
                           : QString::fromUtf8("Направление: внутрь")};
}

QStringList draftParameters(const ShapeFeature& feature) {
  const auto* value = descriptorFeature<DraftFeature>(feature);
  if (!value) return {};
  const double signedAngle =
      value->reversed() ? -value->angleDeg() : value->angleDeg();
  return {QString::fromUtf8("Угол: %1°").arg(signedAngle, 0, 'f', 2),
          QString::fromUtf8("Граней: %1")
              .arg(value->draftedFaces().size())};
}

bool doesNotConsumeSketch(const ShapeFeature&, SketchId) noexcept {
  return false;
}

bool extrudeConsumesSketch(const ShapeFeature& feature,
                           SketchId sketchId) noexcept {
  const auto* extrude = descriptorFeature<ExtrudeFeature>(feature);
  return extrude && !extrude->isFaceSource() &&
         extrude->profileSketchId() == sketchId;
}

bool pocketConsumesSketch(const ShapeFeature& feature,
                          SketchId sketchId) noexcept {
  const auto* pocket = descriptorFeature<PocketFeature>(feature);
  return pocket && pocket->profileSketchId() == sketchId;
}

bool revolveConsumesSketch(const ShapeFeature& feature,
                           SketchId sketchId) noexcept {
  const auto* revolve = descriptorFeature<RevolveFeature>(feature);
  return revolve && revolve->profileSketchId() == sketchId;
}

const std::array<FeatureUiDescriptor, kPersistedFeatureKindCount>& registry() {
  static const std::array<FeatureUiDescriptor, kPersistedFeatureKindCount>
      entries{{
          {FeatureKind::ImportedShape, HistoryStepType::ImportedShape,
           PartDesignToolKind::None, ToolIconKind::ImportedShape,
           "Импортированная модель", false, FeatureEditorRoute::None,
           noParameters, doesNotConsumeSketch},
          {FeatureKind::Extrude, HistoryStepType::Extrude,
           PartDesignToolKind::Extrude, ToolIconKind::Extrude, "Выдавливание",
           true, FeatureEditorRoute::Extrude, extrudeParameters,
           extrudeConsumesSketch},
          {FeatureKind::Revolve, HistoryStepType::Revolve,
           PartDesignToolKind::Revolve, ToolIconKind::Revolve, "Вращение", true,
           FeatureEditorRoute::Revolve, revolveParameters,
           revolveConsumesSketch},
          {FeatureKind::Pocket, HistoryStepType::Pocket,
           PartDesignToolKind::Pocket, ToolIconKind::Pocket, "Вырез", true,
           FeatureEditorRoute::Pocket, pocketParameters, pocketConsumesSketch},
          {FeatureKind::Fillet, HistoryStepType::Fillet,
           PartDesignToolKind::Fillet, ToolIconKind::Fillet, "Скругление", true,
           FeatureEditorRoute::Fillet, filletParameters,
           doesNotConsumeSketch},
          {FeatureKind::Chamfer, HistoryStepType::Chamfer,
           PartDesignToolKind::Chamfer, ToolIconKind::Chamfer, "Фаска", true,
           FeatureEditorRoute::Chamfer, chamferParameters,
           doesNotConsumeSketch},
          {FeatureKind::Mirror, HistoryStepType::Mirror,
           PartDesignToolKind::Mirror, ToolIconKind::Mirror, "Зеркало", true,
           FeatureEditorRoute::Mirror, mirrorParameters, doesNotConsumeSketch},
          {FeatureKind::Move, HistoryStepType::Move, PartDesignToolKind::Move,
           ToolIconKind::Move, "Перемещение", true, FeatureEditorRoute::Move,
           moveParameters, doesNotConsumeSketch},
          {FeatureKind::LinearPattern, HistoryStepType::LinearPattern,
           PartDesignToolKind::LinearPattern, ToolIconKind::LinearPattern,
           "Линейный массив", true, FeatureEditorRoute::LinearPattern,
           linearPatternParameters, doesNotConsumeSketch},
          {FeatureKind::CircularPattern, HistoryStepType::CircularPattern,
           PartDesignToolKind::CircularPattern, ToolIconKind::CircularPattern,
           "Круговой массив", true, FeatureEditorRoute::CircularPattern,
           circularPatternParameters, doesNotConsumeSketch},
          {FeatureKind::JoinBodies, HistoryStepType::JoinBodies,
           PartDesignToolKind::JoinBodies, ToolIconKind::JoinBodies,
           "Соединить тела", true, FeatureEditorRoute::JoinBodies,
           joinBodiesParameters, doesNotConsumeSketch},
          {FeatureKind::Shell, HistoryStepType::Shell,
           PartDesignToolKind::Shell, ToolIconKind::Shell, "Оболочка", true,
           FeatureEditorRoute::Shell, shellParameters, doesNotConsumeSketch},
          {FeatureKind::Draft, HistoryStepType::Draft,
           PartDesignToolKind::Draft, ToolIconKind::Draft, "Уклон", true,
           FeatureEditorRoute::Draft, draftParameters, doesNotConsumeSketch},
      }};
  return entries;
}

}  // namespace

QString FeatureUiDescriptor::title() const {
  return titleUtf8 ? QString::fromUtf8(titleUtf8) : QString{};
}

std::span<const FeatureUiDescriptor> featureUiDescriptors() noexcept {
  return registry();
}

const FeatureUiDescriptor* featureUiDescriptor(FeatureKind kind) noexcept {
  for (const auto& descriptor : registry())
    if (descriptor.kind == kind) return &descriptor;
  return nullptr;
}

QStringList formatFeatureParameters(const FeatureUiDescriptor& descriptor,
                                    const ShapeFeature& feature) {
  return descriptor.formatParameters ? descriptor.formatParameters(feature)
                                     : QStringList{};
}

bool featureConsumesSketch(const FeatureUiDescriptor& descriptor,
                           const ShapeFeature& feature,
                           SketchId sketchId) noexcept {
  return descriptor.consumesSketch &&
         descriptor.consumesSketch(feature, sketchId);
}

}  // namespace solidar
