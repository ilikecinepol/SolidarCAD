#pragma once

#include <memory>
#include <string>

#include "model/SketchPlacement.h"

class TopoDS_Shape;

namespace solidar {

enum class ToolLifecycle { Inactive, Editing, PreviewValid, PreviewInvalid };

struct LinearToolManipulator {
  Point3d origin{};
  Vector3d direction{0.0, 0.0, 1.0};
  double valueMm{};
};

struct AngularToolManipulator {
  Point3d origin{};
  Vector3d axis{1.0, 0.0, 0.0};
  double radiusMm{20.0};
  double angleDeg{360.0};
};

class ToolSession {
 public:
  virtual ~ToolSession() = default;
  [[nodiscard]] virtual ToolLifecycle lifecycle() const noexcept = 0;
  [[nodiscard]] virtual std::shared_ptr<const TopoDS_Shape> previewShape() const = 0;
  [[nodiscard]] virtual const std::string& error() const noexcept = 0;
  virtual bool updatePreview() = 0;
  virtual void cancel() noexcept = 0;
};

}  // namespace solidar
