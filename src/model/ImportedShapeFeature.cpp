#include "model/ImportedShapeFeature.h"

#include <TopoDS_Shape.hxx>
#include <BRep_Builder.hxx>
#include <BRepTools.hxx>

#include <memory>
#include <sstream>
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

const std::string& ImportedShapeFeature::historyArchive() const noexcept {
  return historyArchive_;
}

void ImportedShapeFeature::prepareForHistory() {
  if (importedShape_ && !importedShape_->IsNull()) {
    std::ostringstream stream(std::ios::binary);
    BRepTools::Write(*importedShape_, stream);
    historyArchive_ = stream.str();
  }
  importedShape_.reset();
  ShapeFeature::prepareForHistory();
}


bool ImportedShapeFeature::rebuildImpl(const RebuildContext&) {
  if ((!importedShape_ || importedShape_->IsNull()) &&
      !historyArchive_.empty()) {
    std::istringstream stream(historyArchive_, std::ios::binary);
    TopoDS_Shape restored;
    BRep_Builder builder;
    BRepTools::Read(restored, stream, builder);
    if (!restored.IsNull()) {
      importedShape_ = std::make_shared<const TopoDS_Shape>(restored);
      historyArchive_.clear();
      historyArchive_.shrink_to_fit();
    }
  }
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
