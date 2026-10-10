#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

#include "model/Body.h"
#include "model/SketchPlacement.h"
#include "sketch/Sketch.h"

namespace solidar {

struct DocumentSketch {
  SketchId id{kInvalidSketchId};
  std::string name;
  sketch::Sketch geometry;
  SketchPlacement placement{SketchPlacement::xy()};
  SketchSupport support{};
  bool supportResolved{true};
  bool placementDirty{true};
  // Monotonic diagnostic revision for deterministic placement scheduling
  // tests. Geometry edits do not advance it; support reconciliation does.
  std::uint64_t placementRevision{};
};

struct BoxParameters {
  double widthMm{60.0};
  double depthMm{40.0};
  double heightMm{25.0};
};

struct BodyFeatureRemovalRange {
  BodyId bodyId{kInvalidBodyId};
  std::size_t firstFeatureIndex{};
  std::vector<FeatureId> featureIds;

  friend bool operator==(const BodyFeatureRemovalRange&,
                         const BodyFeatureRemovalRange&) = default;
};

struct FeatureRemovalPlan {
  // bodyId is retained for callers that identify the selected Feature by its
  // owner. The actual mutation contract is bodyRanges: one validated suffix
  // per affected Body.
  BodyId bodyId{kInvalidBodyId};
  FeatureId selectedFeatureId{kInvalidFeatureId};
  SketchId selectedSketchId{kInvalidSketchId};
  BodyId selectedBodyId{kInvalidBodyId};
  std::vector<BodyFeatureRemovalRange> bodyRanges;
  std::vector<BodyId> bodyIds;
  std::vector<FeatureId> featureIds;
  std::vector<SketchId> sketchIds;
  bool applicable{false};
  std::string diagnostic;
  [[nodiscard]] bool empty() const noexcept {
    return bodyRanges.empty() && bodyIds.empty() && sketchIds.empty();
  }
};

class Document final {
 public:
  Document();

  DocumentSketch& addSketch(std::string name = {});
  DocumentSketch& addSketch(SketchId id, std::string name,
                            sketch::Sketch geometry = {});
  [[nodiscard]] const std::vector<DocumentSketch>& sketches() const noexcept;
  [[nodiscard]] DocumentSketch* sketchAt(std::size_t index) noexcept;
  [[nodiscard]] DocumentSketch* findSketch(SketchId id) noexcept;
  [[nodiscard]] const DocumentSketch* findSketch(SketchId id) const noexcept;
  bool replaceSketchGeometry(SketchId id, sketch::Sketch geometry);
  bool markSketchDirty(SketchId id);

  Body& addBody(std::string name = {});
  Body& addBody(BodyId id, std::string name);
  [[nodiscard]] const std::vector<Body>& bodies() const noexcept;
  [[nodiscard]] Body* findBody(BodyId id) noexcept;
  [[nodiscard]] const Body* findBody(BodyId id) const noexcept;
  [[nodiscard]] ShapeFeature* findFeature(FeatureId id) noexcept;
  [[nodiscard]] const ShapeFeature* findFeature(FeatureId id) const noexcept;
  [[nodiscard]] Body* findBodyForFeature(FeatureId id) noexcept;
  [[nodiscard]] const Body* findBodyForFeature(FeatureId id) const noexcept;
  [[nodiscard]] Body* activeBody() noexcept;
  [[nodiscard]] const Body* activeBody() const noexcept;
  [[nodiscard]] bool rebuild();
  [[nodiscard]] bool recompute();
  [[nodiscard]] bool recomputeFrom(FeatureId featureId);
  [[nodiscard]] std::string rebuildError() const;
  [[nodiscard]] FeatureRemovalPlan planFeatureRemoval(
      BodyId bodyId, FeatureId featureId) const;
  [[nodiscard]] FeatureRemovalPlan planSketchRemoval(SketchId sketchId) const;
  [[nodiscard]] FeatureRemovalPlan planBodyRemoval(BodyId bodyId) const;
  // Apply exactly the plan that was presented to the user. The plan is
  // recomputed and compared before any mutation, so stale confirmations fail
  // without changing the Document.
  bool applyRemovalPlan(const FeatureRemovalPlan& plan,
                        std::string* error = nullptr);
  bool removeFeatureCascade(BodyId bodyId, FeatureId featureId,
                            std::string* error = nullptr);
  bool removeSketchCascade(SketchId sketchId,
                           std::string* error = nullptr);
  bool removeBodyCascade(BodyId bodyId, std::string* error = nullptr);
  bool attachSketchToFace(SketchId sketchId, FaceReference reference);
  void updateSketchPlacements();
  [[nodiscard]] const BoxParameters& box() const noexcept;
  void setBox(BoxParameters parameters);
  // Commit a fully validated/restored document to the editable application
  // state. Staging deliberately leaves process-global generators untouched.
  void reserveIdsForEditing() const noexcept;
  bool applyBodySlice(std::size_t index, std::optional<Body> body);
  bool applyFeatureSlice(BodyId bodyId, std::size_t index,
                         const ShapeFeature* feature);
  bool applySketchSlice(std::size_t index,
                        std::optional<DocumentSketch> sketch);

 private:
  static SketchId nextSketchId() noexcept;
  bool applyValidatedRemovalPlan(const FeatureRemovalPlan& plan,
                                 std::string* error);
  bool updateSketchPlacement(DocumentSketch& sketch);

  std::vector<DocumentSketch> sketches_;
  std::vector<Body> bodies_;
  BoxParameters box_;
  std::string dependencyError_;
};

}  // namespace solidar
