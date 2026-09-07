#include <BRepPrimAPI_MakeBox.hxx>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "model/Document.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

namespace {
class CountingFeature final : public solidar::ShapeFeature {
 public:
  CountingFeature(std::string name,
                  solidar::SketchId sketchId = solidar::kInvalidSketchId)
      : ShapeFeature(std::move(name)), sketchId_(sketchId) {}
  std::string typeName() const override { return "Counting"; }
  bool dependsOnSketch(solidar::SketchId id) const noexcept override {
    return sketchId_ == id;
  }
  bool rebuild(const solidar::RebuildContext&) override {
    ++rebuildCount;
    clearShape();
    if (fail_) {
      markError("Intentional upstream history failure");
      return false;
    }
    setShape(std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(10.0 + rebuildCount, 10.0, 10.0).Shape()));
    markValid();
    return true;
  }
  std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<CountingFeature>(*this);
  }
  void setFail(bool fail) { fail_ = fail; setDirty(); }
  int rebuildCount{};
 private:
  solidar::SketchId sketchId_{solidar::kInvalidSketchId};
  bool fail_{};
};
}  // namespace

int main() {
  solidar::Document document;
  auto& rootSketch = document.addSketch("Root sketch");
  const auto rootSketchId = rootSketch.id;
  rootSketch.geometry.addRectangle({0.0, 0.0}, {20.0, 10.0});
  auto& body = document.addBody("Dependent body");
  auto root = std::make_unique<CountingFeature>("Root", rootSketchId);
  auto child = std::make_unique<CountingFeature>("Child");
  auto tail = std::make_unique<CountingFeature>("Tail");
  auto* rootPtr = root.get();
  auto* childPtr = child.get();
  auto* tailPtr = tail.get();
  const auto rootId = rootPtr->id();
  const auto childId = childPtr->id();
  const auto tailId = tailPtr->id();
  body.addFeature(std::move(root));
  body.addFeature(std::move(child));
  body.addFeature(std::move(tail));
  auto& otherBody = document.addBody("Independent body");
  auto other = std::make_unique<CountingFeature>("Independent");
  auto* otherPtr = other.get();
  otherBody.addFeature(std::move(other));

  CHECK(document.recompute());
  CHECK(rootPtr->rebuildCount == 1 && childPtr->rebuildCount == 1);
  CHECK(tailPtr->rebuildCount == 1 && otherPtr->rebuildCount == 1);
  solidar::sketch::Sketch replacement;
  replacement.addRectangle({0.0, 0.0}, {25.0, 12.0});
  CHECK(document.replaceSketchGeometry(rootSketchId, replacement));
  CHECK(rootPtr->isDirty() && childPtr->isDirty() && tailPtr->isDirty());
  CHECK(otherPtr->isValid());
  CHECK(document.recompute());
  CHECK(rootPtr->rebuildCount == 2 && childPtr->rebuildCount == 2);
  CHECK(tailPtr->rebuildCount == 2 && otherPtr->rebuildCount == 1);
  CHECK(rootPtr->id() == rootId && childPtr->id() == childId &&
        tailPtr->id() == tailId);

  rootPtr->setFail(true);
  CHECK(!document.recompute());
  CHECK(rootPtr->isFailed() && !rootPtr->hasShape() && !rootPtr->error().empty());
  CHECK(childPtr->isFailed() && tailPtr->isFailed());
  CHECK(!childPtr->hasShape() && !tailPtr->hasShape());
  CHECK(childPtr->error().find("Blocked by invalid upstream") != std::string::npos);
  CHECK(tailPtr->error().find("Blocked by invalid upstream") != std::string::npos);
  CHECK(otherPtr->isValid() && otherPtr->rebuildCount == 1);

  rootPtr->setFail(false);
  CHECK(document.recompute());
  CHECK(rootPtr->isValid() && childPtr->isValid() && tailPtr->isValid());
  CHECK(rootPtr->id() == rootId && childPtr->id() == childId &&
        tailPtr->id() == tailId);
  CHECK(otherPtr->rebuildCount == 1);
  const int rootCount = rootPtr->rebuildCount;
  const int childCount = childPtr->rebuildCount;
  CHECK(document.recomputeFrom(tailId));
  CHECK(rootPtr->rebuildCount == rootCount && childPtr->rebuildCount == childCount);
  CHECK(tailPtr->rebuildCount == 4);
  return EXIT_SUCCESS;
}
