#include "model/ImportedShapeFeature.h"

#include <TopoDS_Shape.hxx>

#include <memory>
#include <utility>

namespace solidar {

ImportedShapeFeature::ImportedShapeFeature(ShapePtr shape, std::string name)
    : ShapeFeature(name.empty() ? "Imported STEP" : std::move(name)),
      importedShape_(std::move(shape)) {}

ImportedShapeFeature::ImportedShapeFeature(FeatureId id, ShapePtr shape,
                                           std::string name)
    : ShapeFeature(id, name.empty() ? "Imported STEP" : std::move(name)),
      importedShape_(std::move(shape)) {}

const ImportedShapeFeature::ShapePtr&
ImportedShapeFeature::importedShape() const noexcept {
  return importedShape_;
}

std::string ImportedShapeFeature::typeName() const { return "ImportedShape"; }

bool ImportedShapeFeature::rebuild(const RebuildContext&) {
  if (!importedShape_ || importedShape_->IsNull()) {
    clearShape();
    markError("Imported B-Rep shape is empty");
    return false;
  }
  setShape(importedShape_);
  markValid();
  return true;
}

std::unique_ptr<Feature> ImportedShapeFeature::clone() const {
  return std::make_unique<ImportedShapeFeature>(*this);
}

}  // namespace solidar
