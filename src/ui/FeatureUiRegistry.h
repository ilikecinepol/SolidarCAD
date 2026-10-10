#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>
#include <span>

#include "model/Feature.h"
#include "model/PartDesignToolFramework.h"
#include "ui/ToolIcon.h"

namespace solidar {

class ShapeFeature;

enum class HistoryStepType : std::uint8_t {
  Unknown,
  Sketch,
  ImportedShape,
  Extrude,
  Pocket,
  Revolve,
  Fillet,
  Chamfer,
  JoinBodies,
  Move,
  Mirror,
  LinearPattern,
  CircularPattern,
  Shell,
  Draft,
};

enum class FeatureEditorRoute : std::uint8_t {
  None,
  Extrude,
  Revolve,
  Pocket,
  Fillet,
  Chamfer,
  Mirror,
  Move,
  LinearPattern,
  CircularPattern,
  JoinBodies,
  Shell,
  Draft,
};

using FeatureParameterFormatter = QStringList (*)(const ShapeFeature&);
using FeatureSketchConsumption = bool (*)(const ShapeFeature&, SketchId) noexcept;

struct FeatureUiDescriptor {
  FeatureKind kind{FeatureKind::Unknown};
  HistoryStepType historyType{HistoryStepType::Unknown};
  PartDesignToolKind toolKind{PartDesignToolKind::None};
  ToolIconKind iconKind{ToolIconKind::ImportedShape};
  const char* titleUtf8{};
  bool editable{false};
  FeatureEditorRoute editorRoute{FeatureEditorRoute::None};
  FeatureParameterFormatter formatParameters{};
  FeatureSketchConsumption consumesSketch{};

  [[nodiscard]] QString title() const;
};

[[nodiscard]] std::span<const FeatureUiDescriptor> featureUiDescriptors()
    noexcept;
[[nodiscard]] const FeatureUiDescriptor* featureUiDescriptor(
    FeatureKind kind) noexcept;
[[nodiscard]] QStringList formatFeatureParameters(
    const FeatureUiDescriptor& descriptor, const ShapeFeature& feature);
[[nodiscard]] bool featureConsumesSketch(
    const FeatureUiDescriptor& descriptor, const ShapeFeature& feature,
    SketchId sketchId) noexcept;

}  // namespace solidar
