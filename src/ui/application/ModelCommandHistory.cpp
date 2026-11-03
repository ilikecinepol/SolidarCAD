#include "ui/application/ModelCommandHistory.h"

#include <algorithm>
#include <array>
#include <memory>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "model/ChamferFeature.h"
#include "model/CircularPatternFeature.h"
#include "model/ComponentGraphAnalysis.h"
#include "model/DraftFeature.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/MirrorFeature.h"
#include "model/MoveFeature.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"
#include "model/ShellFeature.h"
#include "ui/application/internal/ModelCommandHistoryTestAccess.h"

namespace solidar {

class DocumentDelta::Storage {
 public:
  virtual ~Storage() = default;
  [[nodiscard]] virtual bool empty() const noexcept = 0;
  [[nodiscard]] virtual std::size_t sliceCount() const noexcept = 0;
  [[nodiscard]] virtual std::size_t retainedBytes() const noexcept = 0;
  virtual void apply(Document& document, bool forward,
                     bool recompute) const = 0;
};

namespace {

std::size_t topologyReferenceBytes(const EdgeReference& reference) {
  if (const auto* legacy = reference.v1())
    return legacy->persistentTag.capacity();
  const auto* persistent = reference.v2();
  if (!persistent) return 0;
  std::size_t bytes = persistent->provenance.localName.capacity() +
                      persistent->provenance.origins.capacity() *
                          sizeof(TopologyProvenanceOrigin);
  for (const auto& origin : persistent->provenance.origins)
    bytes += origin.localName.capacity();
  return bytes;
}

std::size_t topologyReferenceBytes(const FaceReference& reference) {
  if (const auto* legacy = reference.v1())
    return legacy->persistentTag.capacity();
  const auto* persistent = reference.v2();
  if (!persistent) return 0;
  std::size_t bytes = persistent->provenance.localName.capacity() +
                      persistent->provenance.origins.capacity() *
                          sizeof(TopologyProvenanceOrigin);
  for (const auto& origin : persistent->provenance.origins)
    bytes += origin.localName.capacity();
  return bytes;
}

bool sameSketchDefinition(const std::optional<sketch::Sketch>& left,
                          const std::optional<sketch::Sketch>& right) {
  return left.has_value() == right.has_value() &&
         (!left || (left->semanticFingerprint() ==
                        right->semanticFingerprint() &&
                    left->semanticallyEqual(*right)));
}

bool samePlacement(const SketchPlacement& left,
                   const SketchPlacement& right) {
  return left.origin.x == right.origin.x &&
         left.origin.y == right.origin.y &&
         left.origin.z == right.origin.z &&
         left.xDirection.x == right.xDirection.x &&
         left.xDirection.y == right.xDirection.y &&
         left.xDirection.z == right.xDirection.z &&
         left.yDirection.x == right.yDirection.x &&
         left.yDirection.y == right.yDirection.y &&
         left.yDirection.z == right.yDirection.z;
}

bool sameDocumentSketchDefinition(const DocumentSketch& left,
                                  const DocumentSketch& right) {
  return left.id == right.id && left.name == right.name &&
         left.geometry.semanticFingerprint() ==
             right.geometry.semanticFingerprint() &&
         left.geometry.semanticallyEqual(right.geometry) &&
         samePlacement(left.placement, right.placement) &&
         left.support.type == right.support.type &&
         (left.support.type != SketchSupportType::Face ||
          left.support.face == right.support.face);
}

template <class Concrete, class Predicate>
bool compareAs(const ShapeFeature& left, const ShapeFeature& right,
               Predicate&& predicate) {
  const auto* a = dynamic_cast<const Concrete*>(&left);
  const auto* b = dynamic_cast<const Concrete*>(&right);
  return a && b && predicate(*a, *b);
}

template <class Concrete>
std::size_t baseFeatureBytes(const ShapeFeature& feature) {
  return sizeof(Concrete) + feature.name().capacity() +
         feature.error().capacity();
}

using FeatureEqual = bool (*)(const ShapeFeature&, const ShapeFeature&);
using FeatureBytes = std::size_t (*)(const ShapeFeature&);

struct HistoryFeatureDescriptor {
  FeatureKind kind{FeatureKind::Unknown};
  FeatureEqual equal{};
  FeatureBytes bytes{};
};

bool importedEqual(const ShapeFeature& left, const ShapeFeature& right) {
  const auto* a = dynamic_cast<const ImportedShapeFeature*>(&left);
  const auto* b = dynamic_cast<const ImportedShapeFeature*>(&right);
  if (!a || !b) return false;
  auto coldArchive = [](const ImportedShapeFeature& feature) {
    auto copy = feature;
    copy.prepareForHistory();
    return copy.historyArchive();
  };
  return coldArchive(*a) == coldArchive(*b);
}
std::size_t importedBytes(const ShapeFeature& feature) {
  const auto* imported = dynamic_cast<const ImportedShapeFeature*>(&feature);
  return baseFeatureBytes<ImportedShapeFeature>(feature) +
         (imported ? imported->historyArchive().capacity() : 0U);
}

bool extrudeEqual(const ShapeFeature& left, const ShapeFeature& right) {
  return compareAs<ExtrudeFeature>(left, right, [](const auto& a, const auto& b) {
    return a.isFaceSource() == b.isFaceSource() &&
           (a.isFaceSource() ? a.faceReference() == b.faceReference()
                             : a.profileSketchId() == b.profileSketchId()) &&
           sameSketchDefinition(a.profileOverride(), b.profileOverride()) &&
           a.lengthMm() == b.lengthMm() && a.operation() == b.operation() &&
           a.reversed() == b.reversed();
  });
}
std::size_t extrudeBytes(const ShapeFeature& feature) {
  const auto* value = dynamic_cast<const ExtrudeFeature*>(&feature);
  std::size_t bytes = baseFeatureBytes<ExtrudeFeature>(feature);
  if (value && value->profileOverride())
    bytes += value->profileOverride()->ownedBytes();
  if (value && value->faceReference())
    bytes += topologyReferenceBytes(*value->faceReference());
  return bytes;
}

bool revolveEqual(const ShapeFeature& left, const ShapeFeature& right) {
  return compareAs<RevolveFeature>(left, right, [](const auto& a, const auto& b) {
    return a.profileSketchId() == b.profileSketchId() &&
           a.axis() == b.axis() && a.angleDeg() == b.angleDeg() &&
           a.operation() == b.operation() && a.reversed() == b.reversed() &&
           sameSketchDefinition(a.profileOverride(), b.profileOverride());
  });
}
std::size_t revolveBytes(const ShapeFeature& feature) {
  const auto* value = dynamic_cast<const RevolveFeature*>(&feature);
  std::size_t bytes = baseFeatureBytes<RevolveFeature>(feature);
  if (value && value->profileOverride())
    bytes += value->profileOverride()->ownedBytes();
  return bytes;
}

bool pocketEqual(const ShapeFeature& left, const ShapeFeature& right) {
  return compareAs<PocketFeature>(left, right, [](const auto& a, const auto& b) {
    return a.profileSketchId() == b.profileSketchId() &&
           a.depthMm() == b.depthMm();
  });
}

bool filletEqual(const ShapeFeature& left, const ShapeFeature& right) {
  return compareAs<FilletFeature>(left, right, [](const auto& a, const auto& b) {
    return a.edges() == b.edges() && a.radiusMm() == b.radiusMm();
  });
}
std::size_t filletBytes(const ShapeFeature& feature) {
  const auto* value = dynamic_cast<const FilletFeature*>(&feature);
  std::size_t bytes = baseFeatureBytes<FilletFeature>(feature);
  if (!value) return bytes;
  bytes += value->edges().capacity() * sizeof(EdgeReference);
  for (const auto& reference : value->edges())
    bytes += topologyReferenceBytes(reference);
  return bytes;
}

bool chamferEqual(const ShapeFeature& left, const ShapeFeature& right) {
  return compareAs<ChamferFeature>(left, right, [](const auto& a, const auto& b) {
    return a.edges() == b.edges() && a.distanceMm() == b.distanceMm();
  });
}
std::size_t chamferBytes(const ShapeFeature& feature) {
  const auto* value = dynamic_cast<const ChamferFeature*>(&feature);
  std::size_t bytes = baseFeatureBytes<ChamferFeature>(feature);
  if (!value) return bytes;
  bytes += value->edges().capacity() * sizeof(EdgeReference);
  for (const auto& reference : value->edges())
    bytes += topologyReferenceBytes(reference);
  return bytes;
}

bool mirrorEqual(const ShapeFeature& left, const ShapeFeature& right) {
  return compareAs<MirrorFeature>(left, right, [](const auto& a, const auto& b) {
    return a.sourceFeatureId() == b.sourceFeatureId() &&
           a.plane() == b.plane() && a.planeFace() == b.planeFace();
  });
}
std::size_t mirrorBytes(const ShapeFeature& feature) {
  const auto* value = dynamic_cast<const MirrorFeature*>(&feature);
  std::size_t bytes = baseFeatureBytes<MirrorFeature>(feature);
  if (value && value->planeFace())
    bytes += topologyReferenceBytes(*value->planeFace());
  return bytes;
}
bool moveEqual(const ShapeFeature& left, const ShapeFeature& right) {
  return compareAs<MoveFeature>(left, right, [](const auto& a, const auto& b) {
    const auto av = a.offsetMm();
    const auto bv = b.offsetMm();
    return a.sourceFeatureId() == b.sourceFeatureId() && av.x == bv.x &&
           av.y == bv.y && av.z == bv.z;
  });
}
bool linearPatternEqual(const ShapeFeature& left, const ShapeFeature& right) {
  return compareAs<LinearPatternFeature>(
      left, right, [](const auto& a, const auto& b) {
        return a.sourceBodyId() == b.sourceBodyId() &&
               a.sourceFeatureId() == b.sourceFeatureId() &&
               a.direction() == b.direction() && a.count() == b.count() &&
               a.spacingMm() == b.spacingMm() &&
               a.operation() == b.operation();
      });
}
bool circularPatternEqual(const ShapeFeature& left, const ShapeFeature& right) {
  return compareAs<CircularPatternFeature>(
      left, right, [](const auto& a, const auto& b) {
        return a.sourceBodyId() == b.sourceBodyId() &&
               a.sourceFeatureId() == b.sourceFeatureId() &&
               a.axis() == b.axis() && a.count() == b.count() &&
               a.angleDeg() == b.angleDeg() &&
               a.operation() == b.operation();
      });
}
bool joinEqual(const ShapeFeature& left, const ShapeFeature& right) {
  return compareAs<JoinBodiesFeature>(left, right, [](const auto& a, const auto& b) {
    return a.firstBodyId() == b.firstBodyId() &&
           a.firstFeatureId() == b.firstFeatureId() &&
           a.secondBodyId() == b.secondBodyId() &&
           a.secondFeatureId() == b.secondFeatureId();
  });
}
bool shellEqual(const ShapeFeature& left, const ShapeFeature& right) {
  return compareAs<ShellFeature>(left, right, [](const auto& a, const auto& b) {
    return a.sourceFeatureId() == b.sourceFeatureId() &&
           a.removedFaces() == b.removedFaces() &&
           a.thicknessMm() == b.thicknessMm() && a.outside() == b.outside();
  });
}
std::size_t shellBytes(const ShapeFeature& feature) {
  const auto* value = dynamic_cast<const ShellFeature*>(&feature);
  std::size_t bytes = baseFeatureBytes<ShellFeature>(feature);
  if (!value) return bytes;
  bytes += value->removedFaces().capacity() * sizeof(FaceReference);
  for (const auto& reference : value->removedFaces())
    bytes += topologyReferenceBytes(reference);
  return bytes;
}
bool draftEqual(const ShapeFeature& left, const ShapeFeature& right) {
  return compareAs<DraftFeature>(left, right, [](const auto& a, const auto& b) {
    return a.sourceFeatureId() == b.sourceFeatureId() &&
           a.draftedFaces() == b.draftedFaces() &&
           a.neutralPlane() == b.neutralPlane() &&
           a.pullDirection() == b.pullDirection() &&
           a.rotationEdge() == b.rotationEdge() &&
           a.angleDeg() == b.angleDeg() && a.reversed() == b.reversed();
  });
}
std::size_t draftBytes(const ShapeFeature& feature) {
  const auto* value = dynamic_cast<const DraftFeature*>(&feature);
  std::size_t bytes = baseFeatureBytes<DraftFeature>(feature);
  if (!value) return bytes;
  bytes += value->draftedFaces().capacity() * sizeof(FaceReference);
  for (const auto& reference : value->draftedFaces())
    bytes += topologyReferenceBytes(reference);
  if (value->neutralPlane().face)
    bytes += topologyReferenceBytes(*value->neutralPlane().face);
  if (value->rotationEdge())
    bytes += topologyReferenceBytes(*value->rotationEdge());
  return bytes;
}

const std::array<HistoryFeatureDescriptor, kPersistedFeatureKindCount>&
historyFeatureRegistry() {
  static const std::array<HistoryFeatureDescriptor,
                          kPersistedFeatureKindCount>
      entries{{
          {FeatureKind::ImportedShape, importedEqual, importedBytes},
          {FeatureKind::Extrude, extrudeEqual, extrudeBytes},
          {FeatureKind::Revolve, revolveEqual, revolveBytes},
          {FeatureKind::Pocket, pocketEqual, baseFeatureBytes<PocketFeature>},
          {FeatureKind::Fillet, filletEqual, filletBytes},
          {FeatureKind::Chamfer, chamferEqual, chamferBytes},
          {FeatureKind::Mirror, mirrorEqual, mirrorBytes},
          {FeatureKind::Move, moveEqual, baseFeatureBytes<MoveFeature>},
          {FeatureKind::LinearPattern, linearPatternEqual,
           baseFeatureBytes<LinearPatternFeature>},
          {FeatureKind::CircularPattern, circularPatternEqual,
           baseFeatureBytes<CircularPatternFeature>},
          {FeatureKind::JoinBodies, joinEqual,
           baseFeatureBytes<JoinBodiesFeature>},
          {FeatureKind::Shell, shellEqual, shellBytes},
          {FeatureKind::Draft, draftEqual, draftBytes},
      }};
  return entries;
}

const HistoryFeatureDescriptor* historyFeatureDescriptor(
    FeatureKind kind) noexcept {
  for (const auto& descriptor : historyFeatureRegistry())
    if (descriptor.kind == kind) return &descriptor;
  return nullptr;
}

bool sameFeatureDefinition(const ShapeFeature& left,
                           const ShapeFeature& right) {
  if (left.id() != right.id() || left.name() != right.name() ||
      left.kind() != right.kind())
    return false;
  const auto* descriptor = historyFeatureDescriptor(left.kind());
  return descriptor && descriptor->equal && descriptor->equal(left, right);
}

std::size_t featureOwnedBytes(const ShapeFeature& feature) {
  const auto* descriptor = historyFeatureDescriptor(feature.kind());
  return descriptor && descriptor->bytes
             ? descriptor->bytes(feature)
             : sizeof(ShapeFeature) + feature.name().capacity() +
                   feature.error().capacity();
}

std::size_t bodyOwnedBytes(const Body& body) {
  std::size_t bytes = sizeof(Body) + body.name().capacity() +
                      body.features().capacity() *
                          sizeof(std::unique_ptr<ShapeFeature>);
  for (const auto& feature : body.features())
    if (feature) bytes += featureOwnedBytes(*feature);
  return bytes;
}

std::size_t documentSketchOwnedBytes(const DocumentSketch& item) {
  std::size_t bytes = sizeof(DocumentSketch) + item.name.capacity() +
                      item.geometry.ownedBytes();
  if (item.support.type == SketchSupportType::Face)
    bytes += topologyReferenceBytes(item.support.face);
  if (item.effectiveSupportAuthoredFace)
    bytes += topologyReferenceBytes(*item.effectiveSupportAuthoredFace);
  if (item.effectiveSupportFace)
    bytes += topologyReferenceBytes(*item.effectiveSupportFace);
  bytes += item.supportFailure.detail.capacity();
  return bytes;
}

bool sameReferenceImage(const ReferenceImage& left,
                        const ReferenceImage& right) {
  return left.id == right.id && left.name == right.name &&
         left.sourcePath == right.sourcePath &&
         left.supportName == right.supportName &&
         samePlacement(left.placement, right.placement) &&
         left.offsetXMm == right.offsetXMm &&
         left.offsetYMm == right.offsetYMm && left.scale == right.scale &&
         left.pixelWidth == right.pixelWidth &&
         left.pixelHeight == right.pixelHeight && left.visible == right.visible;
}

std::size_t referenceImageOwnedBytes(const ReferenceImage& image) {
  return sizeof(ReferenceImage) + image.name.capacity() +
         image.sourcePath.capacity() + image.supportName.capacity();
}

std::size_t editorStateOwnedBytes(const EditorCommittedState& state) {
  std::size_t bytes = sizeof(EditorCommittedState) +
                      state.presentation.sketchVisibilities.capacity() *
                          sizeof(std::uint8_t) +
                      state.sketchViewIds.capacity() * sizeof(SketchId) +
                      state.bodies.capacity() * sizeof(BodyId) +
                      state.edges.capacity() * sizeof(EdgeReference) +
                      state.faces.capacity() * sizeof(FaceReference);
  for (const auto& edge : state.edges)
    bytes += topologyReferenceBytes(edge);
  for (const auto& face : state.faces)
    bytes += topologyReferenceBytes(face);
  return bytes;
}

std::unique_ptr<ShapeFeature> cloneHistoryFeature(
    const ShapeFeature& feature) {
  auto cloned = feature.clone();
  auto* shape = dynamic_cast<ShapeFeature*>(cloned.get());
  if (!shape)
    throw std::logic_error("history feature clone is not a ShapeFeature");
  shape->prepareForHistory();
  cloned.release();
  return std::unique_ptr<ShapeFeature>(shape);
}

Body cloneHistoryBody(const Body& body) {
  Body result(body);
  for (const auto& feature : result.features())
    if (feature) feature->prepareForHistory();
  return result;
}

struct BodyDeltaSlice {
  std::size_t beforeIndex{};
  std::size_t afterIndex{};
  std::optional<Body> beforeBody;
  std::optional<Body> afterBody;
};

struct BodyStateDelta {
  BodyId id{kInvalidBodyId};
  std::string beforeName;
  std::string afterName;
  bool beforeVisible{true};
  bool afterVisible{true};
};

struct FeatureDeltaSlice {
  BodyId bodyId{kInvalidBodyId};
  std::size_t beforeIndex{};
  std::size_t afterIndex{};
  std::unique_ptr<ShapeFeature> beforeFeature;
  std::unique_ptr<ShapeFeature> afterFeature;
};

struct SketchDeltaSlice {
  std::size_t beforeIndex{};
  std::size_t afterIndex{};
  std::optional<DocumentSketch> beforeSketch;
  std::optional<DocumentSketch> afterSketch;
};

struct ReferenceImageDeltaSlice {
  std::size_t beforeIndex{};
  std::size_t afterIndex{};
  std::optional<ReferenceImage> beforeImage;
  std::optional<ReferenceImage> afterImage;
};

struct DocumentDeltaStorage final : DocumentDelta::Storage {
  std::vector<BodyDeltaSlice> bodies;
  std::vector<BodyStateDelta> bodyStates;
  std::vector<FeatureDeltaSlice> features;
  std::vector<SketchDeltaSlice> sketches;
  std::vector<ReferenceImageDeltaSlice> referenceImages;
  BoxParameters beforeBox;
  BoxParameters afterBox;
  std::size_t retainedByteCount{};

  [[nodiscard]] bool empty() const noexcept override {
    return bodies.empty() && bodyStates.empty() && features.empty() &&
           sketches.empty() && referenceImages.empty() &&
           beforeBox.widthMm == afterBox.widthMm &&
           beforeBox.depthMm == afterBox.depthMm &&
           beforeBox.heightMm == afterBox.heightMm;
  }
  [[nodiscard]] std::size_t retainedBytes() const noexcept override {
    return retainedByteCount;
  }
  [[nodiscard]] std::size_t sliceCount() const noexcept override {
    return bodies.size() + bodyStates.size() + features.size() +
           sketches.size() + referenceImages.size();
  }
  void apply(Document& document, bool forward, bool recompute) const override;
};

std::shared_ptr<const DocumentDeltaStorage> makeDocumentDeltaStorage(
    const Document& before, const Document& after) {
  auto storage = std::make_shared<DocumentDeltaStorage>();
  auto& delta = *storage;
  delta.beforeBox = before.box();
  delta.afterBox = after.box();
  for (std::size_t i = 0; i < before.bodies().size(); ++i) {
    const Body& oldBody = before.bodies()[i];
    const auto found = std::find_if(
        after.bodies().begin(), after.bodies().end(),
        [&oldBody](const Body& body) { return body.id() == oldBody.id(); });
    const std::size_t newBodyIndex =
        found == after.bodies().end()
            ? after.bodies().size()
            : static_cast<std::size_t>(
                  std::distance(after.bodies().begin(), found));
    if (found == after.bodies().end()) {
      BodyDeltaSlice slice;
      slice.beforeIndex = i;
      slice.afterIndex = newBodyIndex;
      slice.beforeBody = cloneHistoryBody(oldBody);
      delta.retainedByteCount += bodyOwnedBytes(*slice.beforeBody);
      delta.bodies.push_back(std::move(slice));
      continue;
    }
    if (oldBody.name() != found->name() ||
        oldBody.visible() != found->visible()) {
      delta.bodyStates.push_back({oldBody.id(), oldBody.name(), found->name(),
                                  oldBody.visible(), found->visible()});
      delta.retainedByteCount += sizeof(BodyStateDelta) +
                             oldBody.name().capacity() +
                             found->name().capacity();
    }
    for (std::size_t oldIndex = 0; oldIndex < oldBody.features().size();
         ++oldIndex) {
      const auto& oldFeature = oldBody.features()[oldIndex];
      const auto current = std::find_if(
          found->features().begin(), found->features().end(),
          [&oldFeature](const auto& feature) {
            return feature && oldFeature &&
                   feature->id() == oldFeature->id();
          });
      const std::size_t newIndex =
          current == found->features().end()
              ? found->features().size()
              : static_cast<std::size_t>(
                    std::distance(found->features().begin(), current));
      if (current != found->features().end() &&
          sameFeatureDefinition(*oldFeature, **current))
        continue;
      FeatureDeltaSlice slice;
      slice.bodyId = oldBody.id();
      slice.beforeIndex = oldIndex;
      slice.afterIndex = newIndex;
      slice.beforeFeature = cloneHistoryFeature(*oldFeature);
      if (current != found->features().end())
        slice.afterFeature = cloneHistoryFeature(**current);
      delta.retainedByteCount += featureOwnedBytes(*slice.beforeFeature);
      if (slice.afterFeature)
        delta.retainedByteCount += featureOwnedBytes(*slice.afterFeature);
      delta.features.push_back(std::move(slice));
    }
    for (std::size_t newIndex = 0; newIndex < found->features().size();
         ++newIndex) {
      const auto& feature = found->features()[newIndex];
      const auto old = std::find_if(
          oldBody.features().begin(), oldBody.features().end(),
          [&feature](const auto& item) {
            return item && feature && item->id() == feature->id();
          });
      if (old != oldBody.features().end()) continue;
      FeatureDeltaSlice slice;
      slice.bodyId = oldBody.id();
      slice.beforeIndex = oldBody.features().size();
      slice.afterIndex = newIndex;
      slice.afterFeature = cloneHistoryFeature(*feature);
      delta.retainedByteCount += featureOwnedBytes(*slice.afterFeature);
      delta.features.push_back(std::move(slice));
    }
  }
  for (std::size_t index = 0; index < after.bodies().size(); ++index) {
    const Body& body = after.bodies()[index];
    if (before.findBody(body.id())) continue;
    BodyDeltaSlice slice;
    slice.beforeIndex = before.bodies().size();
    slice.afterIndex = index;
    slice.afterBody = cloneHistoryBody(body);
    delta.retainedByteCount += bodyOwnedBytes(*slice.afterBody);
    delta.bodies.push_back(std::move(slice));
  }
  for (std::size_t index = 0; index < before.sketches().size(); ++index) {
    const auto& old = before.sketches()[index];
    const auto* current = after.findSketch(old.id);
    if (current && sameDocumentSketchDefinition(old, *current)) continue;
    SketchDeltaSlice slice;
    slice.beforeIndex = index;
    slice.beforeSketch = old;
    if (current) {
      slice.afterIndex = static_cast<std::size_t>(
          current - after.sketches().data());
      slice.afterSketch = *current;
    } else {
      slice.afterIndex = after.sketches().size();
    }
    delta.retainedByteCount += documentSketchOwnedBytes(old);
    if (slice.afterSketch)
      delta.retainedByteCount += documentSketchOwnedBytes(*slice.afterSketch);
    delta.sketches.push_back(std::move(slice));
  }
  for (std::size_t index = 0; index < after.sketches().size(); ++index) {
    const auto& sketch = after.sketches()[index];
    if (before.findSketch(sketch.id)) continue;
    SketchDeltaSlice slice;
    slice.beforeIndex = before.sketches().size();
    slice.afterIndex = index;
    slice.afterSketch = sketch;
    delta.retainedByteCount += documentSketchOwnedBytes(sketch);
    delta.sketches.push_back(std::move(slice));
  }
  for (std::size_t index = 0; index < before.referenceImages().size(); ++index) {
    const auto& old = before.referenceImages()[index];
    const auto* current = after.findReferenceImage(old.id);
    if (current && sameReferenceImage(old, *current)) continue;
    ReferenceImageDeltaSlice slice;
    slice.beforeIndex = index;
    slice.beforeImage = old;
    if (current) {
      slice.afterIndex = static_cast<std::size_t>(
          current - after.referenceImages().data());
      slice.afterImage = *current;
    } else {
      slice.afterIndex = after.referenceImages().size();
    }
    delta.retainedByteCount += referenceImageOwnedBytes(old);
    if (slice.afterImage)
      delta.retainedByteCount += referenceImageOwnedBytes(*slice.afterImage);
    delta.referenceImages.push_back(std::move(slice));
  }
  for (std::size_t index = 0; index < after.referenceImages().size(); ++index) {
    const auto& image = after.referenceImages()[index];
    if (before.findReferenceImage(image.id)) continue;
    ReferenceImageDeltaSlice slice;
    slice.beforeIndex = before.referenceImages().size();
    slice.afterIndex = index;
    slice.afterImage = image;
    delta.retainedByteCount += referenceImageOwnedBytes(image);
    delta.referenceImages.push_back(std::move(slice));
  }
  delta.retainedByteCount += delta.bodies.capacity() * sizeof(BodyDeltaSlice) +
                         delta.bodyStates.capacity() * sizeof(BodyStateDelta) +
                         delta.features.capacity() *
                             sizeof(FeatureDeltaSlice) +
                         delta.sketches.capacity() * sizeof(SketchDeltaSlice) +
                         delta.referenceImages.capacity() *
                             sizeof(ReferenceImageDeltaSlice);
  return storage;
}

void applyDocumentDelta(Document& document, const DocumentDeltaStorage& delta,
                        bool forward, bool recompute) {
  const auto desiredBody = [forward](const BodyDeltaSlice& slice)
      -> const std::optional<Body>& {
    return forward ? slice.afterBody : slice.beforeBody;
  };
  const auto sourceBody = [forward](const BodyDeltaSlice& slice)
      -> const std::optional<Body>& {
    return forward ? slice.beforeBody : slice.afterBody;
  };
  std::vector<std::size_t> removals;
  for (const auto& slice : delta.bodies) {
    if (!sourceBody(slice) || desiredBody(slice)) continue;
    for (std::size_t index = 0; index < document.bodies().size(); ++index)
      if (document.bodies()[index].id() == sourceBody(slice)->id())
        removals.push_back(index);
  }
  std::sort(removals.rbegin(), removals.rend());
  for (const auto index : removals)
    if (!document.applyBodySlice(index, std::nullopt))
      throw std::runtime_error("history body removal failed");
  for (const auto& slice : delta.bodies) {
    if (!desiredBody(slice)) continue;
    std::optional<std::size_t> currentIndex;
    for (std::size_t index = 0; index < document.bodies().size(); ++index)
      if (document.bodies()[index].id() == desiredBody(slice)->id())
        currentIndex = index;
    const std::size_t target = forward ? slice.afterIndex : slice.beforeIndex;
    if (!document.applyBodySlice(
            currentIndex.value_or(std::min(target, document.bodies().size())),
            *desiredBody(slice)))
      throw std::runtime_error("history body apply failed");
  }
  for (const auto& state : delta.bodyStates) {
    Body* body = document.findBody(state.id);
    if (!body) throw std::runtime_error("history body state target missing");
    body->setName(forward ? state.afterName : state.beforeName);
    body->setVisible(forward ? state.afterVisible : state.beforeVisible);
  }

  const auto desiredFeature = [forward](const FeatureDeltaSlice& slice) {
    return forward ? slice.afterFeature.get() : slice.beforeFeature.get();
  };
  const auto sourceFeature = [forward](const FeatureDeltaSlice& slice) {
    return forward ? slice.beforeFeature.get() : slice.afterFeature.get();
  };
  struct FeatureRemoval {
    BodyId bodyId{kInvalidBodyId};
    std::size_t index{};
  };
  std::vector<FeatureRemoval> featureRemovals;
  for (const auto& slice : delta.features) {
    if (!sourceFeature(slice) || desiredFeature(slice)) continue;
    const Body* body = document.findBody(slice.bodyId);
    if (!body) throw std::runtime_error("history feature body missing");
    const auto found = body->featureIndex(sourceFeature(slice)->id());
    if (!found) throw std::runtime_error("history feature removal missing");
    featureRemovals.push_back({slice.bodyId, *found});
  }
  std::sort(featureRemovals.begin(), featureRemovals.end(),
            [](const auto& left, const auto& right) {
              return left.bodyId == right.bodyId
                         ? left.index > right.index
                         : left.bodyId < right.bodyId;
            });
  for (const auto& removal : featureRemovals)
    if (!document.applyFeatureSlice(removal.bodyId, removal.index, nullptr))
      throw std::runtime_error("history feature removal failed");
  for (const auto& slice : delta.features) {
    const auto* desired = desiredFeature(slice);
    if (!desired) continue;
    const Body* body = document.findBody(slice.bodyId);
    if (!body) throw std::runtime_error("history feature body missing");
    const auto existing = body->featureIndex(desired->id());
    const std::size_t target = forward ? slice.afterIndex : slice.beforeIndex;
    if (!document.applyFeatureSlice(
            slice.bodyId,
            existing.value_or(std::min(target, body->features().size())),
            desired))
      throw std::runtime_error("history feature apply failed");
  }

  for (const auto& slice : delta.sketches) {
    const auto& source = forward ? slice.beforeSketch : slice.afterSketch;
    const auto& desired = forward ? slice.afterSketch : slice.beforeSketch;
    if (!source || desired) continue;
    for (std::size_t index = document.sketches().size(); index-- > 0;)
      if (document.sketches()[index].id == source->id &&
          !document.applySketchSlice(index, std::nullopt))
        throw std::runtime_error("history sketch removal failed");
  }
  for (const auto& slice : delta.sketches) {
    const auto& desired = forward ? slice.afterSketch : slice.beforeSketch;
    if (!desired) continue;
    std::optional<std::size_t> currentIndex;
    for (std::size_t index = 0; index < document.sketches().size(); ++index)
      if (document.sketches()[index].id == desired->id)
        currentIndex = index;
    const std::size_t target = forward ? slice.afterIndex : slice.beforeIndex;
    if (!document.applySketchSlice(
            currentIndex.value_or(std::min(target, document.sketches().size())),
            *desired))
      throw std::runtime_error("history sketch apply failed");
  }
  for (const auto& slice : delta.referenceImages) {
    const auto& source = forward ? slice.beforeImage : slice.afterImage;
    const auto& desired = forward ? slice.afterImage : slice.beforeImage;
    if (!source || desired) continue;
    for (std::size_t index = document.referenceImages().size(); index-- > 0;)
      if (document.referenceImages()[index].id == source->id &&
          !document.applyReferenceImageSlice(index, std::nullopt))
        throw std::runtime_error("history reference image removal failed");
  }
  for (const auto& slice : delta.referenceImages) {
    const auto& desired = forward ? slice.afterImage : slice.beforeImage;
    if (!desired) continue;
    std::optional<std::size_t> currentIndex;
    for (std::size_t index = 0; index < document.referenceImages().size();
         ++index)
      if (document.referenceImages()[index].id == desired->id)
        currentIndex = index;
    const std::size_t target = forward ? slice.afterIndex : slice.beforeIndex;
    if (!document.applyReferenceImageSlice(
            currentIndex.value_or(
                std::min(target, document.referenceImages().size())),
            *desired))
      throw std::runtime_error("history reference image apply failed");
  }
  document.setBox(forward ? delta.afterBox : delta.beforeBox);
  if (recompute && !document.recompute())
    throw std::runtime_error(document.rebuildError());
}

void DocumentDeltaStorage::apply(Document& document, bool forward,
                                 bool recompute) const {
  applyDocumentDelta(document, *this, forward, recompute);
}

std::string exceptionText(const char* fallback) noexcept {
  try {
    throw;
  } catch (const std::exception& error) {
    try {
      return error.what();
    } catch (...) {
      return fallback;
    }
  } catch (...) {
    return fallback;
  }
}

}  // namespace

DocumentDelta DocumentDelta::create(const Document& before,
                                    const Document& after) {
  return DocumentDelta(makeDocumentDeltaStorage(before, after));
}

void DocumentDelta::applyForward(Document& document) const {
  if (!storage_) throw std::logic_error("empty DocumentDelta storage");
  storage_->apply(document, true, true);
}

void DocumentDelta::applyBackward(Document& document) const {
  if (!storage_) throw std::logic_error("empty DocumentDelta storage");
  storage_->apply(document, false, true);
}

void DocumentDelta::applyForwardUnrecomputed(Document& document) const {
  if (!storage_) throw std::logic_error("empty DocumentDelta storage");
  storage_->apply(document, true, false);
}

void DocumentDelta::applyBackwardUnrecomputed(Document& document) const {
  if (!storage_) throw std::logic_error("empty DocumentDelta storage");
  storage_->apply(document, false, false);
}

bool DocumentDelta::empty() const noexcept {
  return !storage_ || storage_->empty();
}

std::size_t DocumentDelta::retainedBytes() const noexcept {
  return storage_ ? storage_->retainedBytes() : 0;
}

std::size_t DocumentDelta::sliceCount() const noexcept {
  return storage_ ? storage_->sliceCount() : 0;
}

bool componentWorkspaceInstanceKeyLess(
    const ComponentWorkspaceInstanceKey& left,
    const ComponentWorkspaceInstanceKey& right) noexcept {
  if (left.path.has_value() != right.path.has_value())
    return !left.path.has_value();
  if (left.path) {
    const auto& a = left.path->entries();
    const auto& b = right.path->entries();
    const auto mismatch = std::mismatch(a.begin(), a.end(), b.begin(), b.end());
    if (mismatch.first != a.end() && mismatch.second != b.end())
      return mismatch.first->value() < mismatch.second->value();
    if (a.size() != b.size()) return a.size() < b.size();
  }
  return left.definitionId < right.definitionId;
}

bool componentWorkspaceBodyKeyLess(
    const ComponentWorkspaceBodyKey& left,
    const ComponentWorkspaceBodyKey& right) noexcept {
  if (componentWorkspaceInstanceKeyLess(left.instance, right.instance))
    return true;
  if (componentWorkspaceInstanceKeyLess(right.instance, left.instance))
    return false;
  return left.bodyId < right.bodyId;
}

bool componentWorkspaceDrawingKeyLess(
    const ComponentWorkspaceDrawingKey& left,
    const ComponentWorkspaceDrawingKey& right) noexcept {
  if (componentWorkspaceInstanceKeyLess(left.instance, right.instance))
    return true;
  if (componentWorkspaceInstanceKeyLess(right.instance, left.instance))
    return false;
  return left.drawingId < right.drawingId;
}

class ModelCommandHistory::Impl final {
 public:
  struct DocumentCommand {
    DocumentDelta delta;
    EditorCommittedState before;
    EditorCommittedState after;
  };

  using Payload =
      std::variant<DocumentCommand, ComponentExtractionColdCommand,
                   ComponentPlacementColdCommand,
                   ComponentHomePlacementColdCommand>;

  struct Command {
    Payload payload;
    std::size_t retainedBytes{};
    std::uint64_t beforeRevision{};
    std::uint64_t afterRevision{};
  };

  struct Pending {
    Command command;
    std::size_t retainedAfterCommit{};
    std::size_t dropFrontCount{};
  };

  explicit Impl(std::size_t budget) : byteBudget(budget) {}

  [[nodiscard]] static ModelCommandKind kind(const Command& command) noexcept {
    if (std::holds_alternative<DocumentCommand>(command.payload))
      return ModelCommandKind::DocumentTransition;
    if (std::holds_alternative<ComponentExtractionColdCommand>(
            command.payload))
      return ModelCommandKind::ComponentExtraction;
    if (std::holds_alternative<ComponentPlacementColdCommand>(
            command.payload))
      return ModelCommandKind::ComponentPlacement;
    return ModelCommandKind::ComponentHomePlacement;
  }

  void enforceBudget() noexcept {
    pending.reset();
    while (!commands.empty() &&
           (commands.size() > ModelCommandHistory::kMaxCommands ||
            retained > byteBudget)) {
      if (cursor != 0) {
        retained -= std::min(retained, commands.front().retainedBytes);
        commands.erase(commands.begin());
        --cursor;
      } else {
        retained -= std::min(retained, commands.back().retainedBytes);
        commands.pop_back();
      }
    }
  }

  [[nodiscard]] bool prepare(Command command) {
    if (pending || applying) return false;
    if (command.afterRevision == 0 ||
        command.afterRevision == std::numeric_limits<std::uint64_t>::max())
      return false;
    if (command.retainedBytes > byteBudget) return false;

    std::size_t prospective{};
    for (std::size_t index = 0; index < cursor; ++index) {
      if (commands[index].retainedBytes >
          std::numeric_limits<std::size_t>::max() - prospective)
        return false;
      prospective += commands[index].retainedBytes;
    }
    if (command.retainedBytes >
        std::numeric_limits<std::size_t>::max() - prospective)
      return false;
    prospective += command.retainedBytes;

    std::size_t drop{};
    const std::size_t prospectiveCount = cursor + 1;
    while (drop < cursor &&
           (prospectiveCount - drop > ModelCommandHistory::kMaxCommands ||
            prospective > byteBudget)) {
      prospective -= commands[drop].retainedBytes;
      ++drop;
    }
    if (prospective > byteBudget) return false;

    commands.reserve(cursor + 1);
    pending.emplace(Pending{std::move(command), prospective, drop});
    return true;
  }

  std::vector<Command> commands;
  std::size_t cursor{};
  std::optional<Pending> pending;
  std::size_t retained{};
  std::size_t byteBudget{ModelCommandHistory::kDefaultByteBudget};
  bool applying{};
  std::uint64_t currentRevision{1};
  std::optional<std::uint64_t> savepointRevision{1};
  std::uint64_t nextRevision{2};
  bool revisionExhausted{};
};

namespace {

std::optional<std::size_t> checkedAdd(std::size_t left,
                                      std::size_t right) noexcept {
  if (right > std::numeric_limits<std::size_t>::max() - left)
    return std::nullopt;
  return left + right;
}

std::optional<std::size_t> checkedMultiply(std::size_t left,
                                           std::size_t right) noexcept {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left)
    return std::nullopt;
  return left * right;
}

std::optional<std::size_t> instanceKeyDynamicBytes(
    const ComponentWorkspaceInstanceKey& key) noexcept {
  if (!key.path) return std::size_t{0};
  return checkedMultiply(key.path->entries().capacity(), sizeof(OccurrenceId));
}

std::optional<std::size_t> componentSemanticStateDynamicBytes(
    const ComponentWorkspaceSemanticState& state) noexcept {
  std::size_t bytes = state.diagnostic.capacity();
  const auto addKey = [&bytes](const ComponentWorkspaceInstanceKey& key) {
    const auto dynamic = instanceKeyDynamicBytes(key);
    if (!dynamic) return false;
    const auto sum = checkedAdd(bytes, *dynamic);
    if (!sum) return false;
    bytes = *sum;
    return true;
  };
  if (!addKey(state.activeInstance) ||
      (state.selectedInstance && !addKey(*state.selectedInstance)) ||
      (state.selectedBody && !addKey(state.selectedBody->instance)))
    return std::nullopt;
  return bytes;
}

template <class Key>
std::optional<std::size_t> workspaceProjectionDynamicBytes(
    const std::vector<Key>& keys) noexcept {
  const auto storage = checkedMultiply(keys.capacity(), sizeof(Key));
  if (!storage) return std::nullopt;
  std::size_t bytes = *storage;
  for (const auto& key : keys) {
    const ComponentWorkspaceInstanceKey& instance = [&]() -> const auto& {
      if constexpr (std::is_same_v<Key, ComponentWorkspaceInstanceKey>)
        return key;
      else
        return key.instance;
    }();
    const auto dynamic = instanceKeyDynamicBytes(instance);
    if (!dynamic) return std::nullopt;
    const auto sum = checkedAdd(bytes, *dynamic);
    if (!sum) return std::nullopt;
    bytes = *sum;
  }
  return bytes;
}

std::optional<std::size_t> componentDrawingsDynamicBytes(
    const std::vector<ComponentDrawing>& drawings) noexcept {
  const auto storage =
      checkedMultiply(drawings.capacity(), sizeof(ComponentDrawing));
  if (!storage) return std::nullopt;
  std::size_t bytes = *storage;
  for (const auto& drawing : drawings) {
    const auto sources = checkedMultiply(drawing.sourceBodyIds().capacity(),
                                         sizeof(BodyId));
    if (!sources) return std::nullopt;
    const auto withName = checkedAdd(bytes, drawing.nameUtf8().capacity());
    if (!withName) return std::nullopt;
    const auto withSources = checkedAdd(*withName, *sources);
    if (!withSources) return std::nullopt;
    bytes = *withSources;
  }
  return bytes;
}

std::optional<std::size_t> componentExtractionOwnedBytes(
    const ComponentExtractionColdCommand& command) noexcept {
  std::size_t bytes = sizeof(ComponentExtractionColdCommand);
  const auto featureBytes = checkedMultiply(
      command.transaction.plan.featureIds.capacity(), sizeof(FeatureId));
  const auto sketchBytes = checkedMultiply(
      command.transaction.plan.sketches.capacity(),
      sizeof(ComponentExtractionSketchLocation));
  const auto frozenSupportBytes = checkedMultiply(
      command.transaction.plan.frozenSketchSupports.capacity(),
      sizeof(ComponentExtractionFrozenSketchSupport));
  std::size_t frozenSupportReferenceBytes = 0;
  for (const auto& frozen :
       command.transaction.plan.frozenSketchSupports) {
    const auto sum = checkedAdd(
        frozenSupportReferenceBytes,
        topologyReferenceBytes(frozen.authoredFace));
    if (!sum) return std::nullopt;
    frozenSupportReferenceBytes = *sum;
  }
  const auto beforeBytes =
      componentSemanticStateDynamicBytes(command.beforeState);
  const auto afterBytes =
      componentSemanticStateDynamicBytes(command.afterState);
  const auto beforeInstanceBytes =
      workspaceProjectionDynamicBytes(command.beforeInstances);
  const auto afterInstanceBytes =
      workspaceProjectionDynamicBytes(command.afterInstances);
  const auto beforeBodyBytes =
      workspaceProjectionDynamicBytes(command.beforeBodies);
  const auto afterBodyBytes =
      workspaceProjectionDynamicBytes(command.afterBodies);
  const auto beforeDrawingBytes =
      workspaceProjectionDynamicBytes(command.beforeDrawings);
  const auto afterDrawingBytes =
      workspaceProjectionDynamicBytes(command.afterDrawings);
  const auto beforeOwnerDrawingBytes =
      componentDrawingsDynamicBytes(command.transaction.beforeOwnerDrawings);
  const auto afterOwnerDrawingBytes =
      componentDrawingsDynamicBytes(command.transaction.afterOwnerDrawings);
  const auto childDrawingBytes = componentDrawingsDynamicBytes(
      command.transaction.createdChildDrawings);
  if (!featureBytes || !sketchBytes || !frozenSupportBytes || !beforeBytes ||
      !afterBytes ||
      !beforeInstanceBytes || !afterInstanceBytes || !beforeBodyBytes ||
      !afterBodyBytes || !beforeDrawingBytes || !afterDrawingBytes ||
      !beforeOwnerDrawingBytes || !afterOwnerDrawingBytes ||
      !childDrawingBytes)
    return std::nullopt;
  for (const std::size_t value :
       {*featureBytes, *sketchBytes, *frozenSupportBytes,
        frozenSupportReferenceBytes,
        command.transaction.plan.diagnostic.capacity(), *beforeBytes,
        *afterBytes, *beforeInstanceBytes, *afterInstanceBytes,
        *beforeBodyBytes, *afterBodyBytes, *beforeDrawingBytes,
        *afterDrawingBytes, *beforeOwnerDrawingBytes,
        *afterOwnerDrawingBytes, *childDrawingBytes}) {
    const auto sum = checkedAdd(bytes, value);
    if (!sum) return std::nullopt;
    bytes = *sum;
  }
  return bytes;
}

std::optional<std::size_t> componentPlacementOwnedBytes(
    const ComponentPlacementColdCommand& command) noexcept {
  std::size_t bytes = sizeof(ComponentPlacementColdCommand);
  const auto requestPathBytes = command.transaction.request.path
                                    ? instanceKeyDynamicBytes(
                                          {command.transaction.request
                                               .expectedTargetDefinitionId,
                                           command.transaction.request.path})
                                    : std::optional<std::size_t>{0};
  const auto beforeBytes =
      componentSemanticStateDynamicBytes(command.beforeState);
  const auto afterBytes =
      componentSemanticStateDynamicBytes(command.afterState);
  const auto beforeInstanceBytes =
      workspaceProjectionDynamicBytes(command.beforeInstances);
  const auto afterInstanceBytes =
      workspaceProjectionDynamicBytes(command.afterInstances);
  const auto beforeBodyBytes =
      workspaceProjectionDynamicBytes(command.beforeBodies);
  const auto afterBodyBytes =
      workspaceProjectionDynamicBytes(command.afterBodies);
  if (!requestPathBytes || !beforeBytes || !afterBytes ||
      !beforeInstanceBytes || !afterInstanceBytes || !beforeBodyBytes ||
      !afterBodyBytes)
    return std::nullopt;
  for (const std::size_t value :
       {*requestPathBytes, *beforeBytes, *afterBytes, *beforeInstanceBytes,
        *afterInstanceBytes, *beforeBodyBytes, *afterBodyBytes}) {
    const auto sum = checkedAdd(bytes, value);
    if (!sum) return std::nullopt;
    bytes = *sum;
  }
  return bytes;
}

std::optional<std::size_t> componentHomePlacementOwnedBytes(
    const ComponentHomePlacementColdCommand& command) noexcept {
  std::size_t bytes = sizeof(ComponentHomePlacementColdCommand);
  const auto requestPathBytes = instanceKeyDynamicBytes(
      {command.transaction.request.expectedTargetDefinitionId,
       command.transaction.request.path});
  const auto beforeBytes =
      componentSemanticStateDynamicBytes(command.beforeState);
  const auto afterBytes = componentSemanticStateDynamicBytes(command.afterState);
  const auto beforeInstanceBytes =
      workspaceProjectionDynamicBytes(command.beforeInstances);
  const auto afterInstanceBytes =
      workspaceProjectionDynamicBytes(command.afterInstances);
  const auto beforeBodyBytes =
      workspaceProjectionDynamicBytes(command.beforeBodies);
  const auto afterBodyBytes =
      workspaceProjectionDynamicBytes(command.afterBodies);
  if (!requestPathBytes || !beforeBytes || !afterBytes ||
      !beforeInstanceBytes || !afterInstanceBytes || !beforeBodyBytes ||
      !afterBodyBytes)
    return std::nullopt;
  for (const std::size_t value :
       {*requestPathBytes, *beforeBytes, *afterBytes, *beforeInstanceBytes,
        *afterInstanceBytes, *beforeBodyBytes, *afterBodyBytes}) {
    const auto sum = checkedAdd(bytes, value);
    if (!sum) return std::nullopt;
    bytes = *sum;
  }
  const auto transactionBytes = checkedMultiply(
      command.additionalTransactions.capacity(),
      sizeof(ComponentHomePlacementTransaction));
  if (!transactionBytes) return std::nullopt;
  if (const auto sum = checkedAdd(bytes, *transactionBytes))
    bytes = *sum;
  else
    return std::nullopt;
  for (const auto& transaction : command.additionalTransactions) {
    const auto pathBytes = instanceKeyDynamicBytes(
        {transaction.request.expectedTargetDefinitionId,
         transaction.request.path});
    if (!pathBytes) return std::nullopt;
    if (const auto sum = checkedAdd(bytes, *pathBytes))
      bytes = *sum;
    else
      return std::nullopt;
  }
  return bytes;
}

constexpr std::size_t kMaximumWorkspaceInstances = 65537;
constexpr std::size_t kMaximumWorkspaceBodies = 65536;
constexpr std::size_t kMaximumWorkspaceDrawings = 65536;

bool validInstanceKey(const ComponentWorkspaceInstanceKey& key) noexcept {
  if (!key.definitionId.isValid()) return false;
  if (!key.path) return true;
  if (key.path->entries().empty() || key.path->entries().size() > 32)
    return false;
  return std::all_of(key.path->entries().begin(), key.path->entries().end(),
                     [](OccurrenceId id) { return id.isValid(); });
}

bool sameInstancePath(const ComponentWorkspaceInstanceKey& left,
                      const ComponentWorkspaceInstanceKey& right) noexcept {
  return left.path == right.path;
}

class ProjectionWorkLimitExceeded final {};

struct ProjectionWorkBudget final {
  std::size_t limit{};
  std::size_t count{};

  bool instanceLess(const ComponentWorkspaceInstanceKey& left,
                    const ComponentWorkspaceInstanceKey& right) {
    consume();
    return componentWorkspaceInstanceKeyLess(left, right);
  }
  bool bodyLess(const ComponentWorkspaceBodyKey& left,
                const ComponentWorkspaceBodyKey& right) {
    consume();
    return componentWorkspaceBodyKeyLess(left, right);
  }
  bool drawingLess(const ComponentWorkspaceDrawingKey& left,
                   const ComponentWorkspaceDrawingKey& right) {
    consume();
    return componentWorkspaceDrawingKeyLess(left, right);
  }

 private:
  void consume() {
    if (count >= limit) throw ProjectionWorkLimitExceeded{};
    ++count;
  }
};

bool containsInstance(
    const std::vector<ComponentWorkspaceInstanceKey>& instances,
    const ComponentWorkspaceInstanceKey& expected,
    ProjectionWorkBudget& work) {
  const auto found = std::lower_bound(
      instances.begin(), instances.end(), expected,
      [&work](const auto& left, const auto& right) {
        return work.instanceLess(left, right);
      });
  return found != instances.end() && *found == expected;
}

bool containsBody(const std::vector<ComponentWorkspaceBodyKey>& bodies,
                  const ComponentWorkspaceBodyKey& expected,
                  ProjectionWorkBudget& work) {
  const auto found = std::lower_bound(
      bodies.begin(), bodies.end(), expected,
      [&work](const auto& left, const auto& right) {
        return work.bodyLess(left, right);
      });
  return found != bodies.end() && *found == expected;
}

bool containsDrawing(
    const std::vector<ComponentWorkspaceDrawingKey>& drawings,
    const ComponentWorkspaceDrawingKey& expected,
    ProjectionWorkBudget& work) {
  const auto found = std::lower_bound(
      drawings.begin(), drawings.end(), expected,
      [&work](const auto& left, const auto& right) {
        return work.drawingLess(left, right);
      });
  return found != drawings.end() && *found == expected;
}

bool validWorkspaceProjection(
    const std::vector<ComponentWorkspaceInstanceKey>& instances,
    const std::vector<ComponentWorkspaceBodyKey>& bodies,
    const std::vector<ComponentWorkspaceDrawingKey>& drawings,
    DefinitionId rootDefinitionId, ProjectionWorkBudget& work) {
  if (instances.empty() || instances.size() > kMaximumWorkspaceInstances ||
      bodies.size() > kMaximumWorkspaceBodies ||
      drawings.size() > kMaximumWorkspaceDrawings)
    return false;
  std::size_t roots{};
  for (std::size_t index = 0; index < instances.size(); ++index) {
    const auto& instance = instances[index];
    if (!validInstanceKey(instance)) return false;
    if (!instance.path) {
      ++roots;
      if (instance.definitionId != rootDefinitionId) return false;
    }
    if (index != 0 &&
        (!work.instanceLess(instances[index - 1], instance) ||
         sameInstancePath(instances[index - 1], instance)))
      return false;
  }
  if (roots != 1) return false;
  for (std::size_t index = 0; index < bodies.size(); ++index) {
    const auto& body = bodies[index];
    if (body.bodyId == kInvalidBodyId ||
        !containsInstance(instances, body.instance, work))
      return false;
    if (index != 0 && !work.bodyLess(bodies[index - 1], body)) return false;
  }
  for (std::size_t index = 0; index < drawings.size(); ++index) {
    const auto& drawing = drawings[index];
    if (!drawing.drawingId.isValid() ||
        !containsInstance(instances, drawing.instance, work))
      return false;
    if (index != 0 &&
        !work.drawingLess(drawings[index - 1], drawing))
      return false;
  }
  return true;
}

bool validWorkspaceProjection(
    const std::vector<ComponentWorkspaceInstanceKey>& instances,
    const std::vector<ComponentWorkspaceBodyKey>& bodies,
    DefinitionId rootDefinitionId, ProjectionWorkBudget& work) {
  return validWorkspaceProjection(instances, bodies, {}, rootDefinitionId,
                                  work);
}

bool validSemanticState(
    const ComponentWorkspaceSemanticState& state,
    const std::vector<ComponentWorkspaceInstanceKey>& instances,
    const std::vector<ComponentWorkspaceBodyKey>& bodies,
    ProjectionWorkBudget& work) {
  return containsInstance(instances, state.activeInstance, work) &&
         (!state.selectedInstance ||
          containsInstance(instances, *state.selectedInstance, work)) &&
         (!state.selectedBody ||
          containsBody(bodies, *state.selectedBody, work));
}

bool isPartPersistenceFormat(
    project::ProjectPersistenceFormat format) noexcept {
  return format == project::ProjectPersistenceFormat::V3 ||
         format == project::ProjectPersistenceFormat::V4 ||
         format == project::ProjectPersistenceFormat::V5;
}

bool isComponentPersistenceFormat(
    project::ProjectPersistenceFormat format) noexcept {
  return project::isWritableComponentPersistenceFormat(format);
}

bool productHasComponentDrawings(const ProductDocument& product) {
  for (const DefinitionId id : product.definitions().definitionIds()) {
    const auto* component = product.definitions().findComponent(id);
    if (component && !component->drawings().empty()) return true;
  }
  return false;
}

bool validComponentPersistenceTransition(
    const ProductDocument& product,
    project::ProjectPersistenceState before,
    project::ProjectPersistenceState after, bool convertedPartRoot) {
  if (convertedPartRoot)
    return isPartPersistenceFormat(before.format) &&
           after.format == project::ProjectPersistenceFormat::V10;
  if (!isComponentPersistenceFormat(before.format) ||
      !isComponentPersistenceFormat(after.format))
    return false;
  if (before.format == project::ProjectPersistenceFormat::V12)
    return after.format == project::ProjectPersistenceFormat::V12;
  if (after.format == project::ProjectPersistenceFormat::V12) return true;
  const auto expected =
      before.format == project::ProjectPersistenceFormat::V11 ||
              productHasComponentDrawings(product)
          ? project::ProjectPersistenceFormat::V11
          : project::ProjectPersistenceFormat::V10;
  return before.format == expected && after.format == expected;
}

std::optional<ComponentWorkspaceInstanceKey> nestedComponentInstance(
    const ComponentWorkspaceInstanceKey& owner, DefinitionId childDefinition,
    OccurrenceId childOccurrence) {
  std::vector<OccurrenceId> entries;
  if (owner.path) entries = owner.path->entries();
  entries.push_back(childOccurrence);
  auto path = OccurrencePath::create(std::move(entries));
  if (!path) return std::nullopt;
  return ComponentWorkspaceInstanceKey{childDefinition,
                                       std::move(*path.path)};
}

bool validWorkspaceTransition(
    const ComponentExtractionColdCommand& command,
    ProjectionWorkBudget& work) {
  const auto& transaction = command.transaction;
  const auto root = transaction.address.rootDefinitionId;
  const auto ownerDefinition = transaction.address.ownerDefinitionId;
  const auto child = transaction.address.childDefinitionId;
  const auto occurrence = transaction.address.childOccurrenceId;
  if (!validWorkspaceProjection(command.beforeInstances, command.beforeBodies,
                                command.beforeDrawings, root, work) ||
      !validWorkspaceProjection(command.afterInstances, command.afterBodies,
                                command.afterDrawings, root, work) ||
      !validSemanticState(command.beforeState, command.beforeInstances,
                          command.beforeBodies, work) ||
      !validSemanticState(command.afterState, command.afterInstances,
                          command.afterBodies, work))
    return false;
  if (transaction.convertedPartRoot) {
    if (!isPartPersistenceFormat(command.beforePersistence.format) ||
        command.beforeInstances.size() != 1 ||
        command.beforeInstances.front().definitionId != root ||
        command.beforeInstances.front().path)
      return false;
  } else if (!isComponentPersistenceFormat(
                 command.beforePersistence.format)) {
    return false;
  }
  if (!isComponentPersistenceFormat(command.afterPersistence.format))
    return false;
  std::vector<std::pair<ComponentWorkspaceInstanceKey,
                        ComponentWorkspaceInstanceKey>>
      createdInstances;
  for (const auto& before : command.beforeInstances) {
    if (before.definitionId != ownerDefinition) continue;
    const auto created = nestedComponentInstance(before, child, occurrence);
    if (!created) return false;
    createdInstances.emplace_back(before, *created);
  }
  if (createdInstances.empty() ||
      command.afterInstances.size() !=
          command.beforeInstances.size() + createdInstances.size() ||
      command.afterBodies.size() != command.beforeBodies.size() ||
      command.afterDrawings.size() != command.beforeDrawings.size())
    return false;
  for (const auto& before : command.beforeInstances) {
    if (!containsInstance(command.afterInstances, before, work)) return false;
    if (before.definitionId == child) return false;
  }
  for (const auto& after : command.afterInstances) {
    if (containsInstance(command.beforeInstances, after, work)) continue;
    const auto created = std::find_if(
        createdInstances.begin(), createdInstances.end(),
        [&after](const auto& pair) { return pair.second == after; });
    if (created == createdInstances.end()) return false;
  }
  for (const auto& pair : createdInstances)
    if (!containsInstance(command.afterInstances, pair.second, work))
      return false;

  for (const auto& before : command.beforeBodies) {
    ComponentWorkspaceBodyKey expected = before;
    if (before.bodyId == transaction.address.bodyId) {
      const auto created = std::find_if(
          createdInstances.begin(), createdInstances.end(),
          [&before](const auto& pair) {
            return pair.first == before.instance;
          });
      if (created != createdInstances.end()) expected.instance = created->second;
    }
    if (!containsBody(command.afterBodies, expected, work)) return false;
  }
  for (const auto& after : command.afterBodies) {
    ComponentWorkspaceBodyKey expected = after;
    if (after.bodyId == transaction.address.bodyId) {
      const auto created = std::find_if(
          createdInstances.begin(), createdInstances.end(),
          [&after](const auto& pair) {
            return pair.second == after.instance;
          });
      if (created != createdInstances.end()) expected.instance = created->first;
    }
    if (!containsBody(command.beforeBodies, expected, work)) return false;
  }
  const auto drawingMovesToChild = [&transaction](DrawingId id) {
    return std::any_of(
        transaction.createdChildDrawings.begin(),
        transaction.createdChildDrawings.end(),
        [id](const ComponentDrawing& drawing) { return drawing.id() == id; });
  };
  for (const auto& before : command.beforeDrawings) {
    ComponentWorkspaceDrawingKey expected = before;
    if (drawingMovesToChild(before.drawingId)) {
      const auto created = std::find_if(
          createdInstances.begin(), createdInstances.end(),
          [&before](const auto& pair) {
            return pair.first == before.instance;
          });
      if (created != createdInstances.end()) expected.instance = created->second;
    }
    if (!containsDrawing(command.afterDrawings, expected, work)) return false;
  }
  for (const auto& after : command.afterDrawings) {
    ComponentWorkspaceDrawingKey expected = after;
    if (drawingMovesToChild(after.drawingId)) {
      const auto created = std::find_if(
          createdInstances.begin(), createdInstances.end(),
          [&after](const auto& pair) {
            return pair.second == after.instance;
          });
      if (created != createdInstances.end()) expected.instance = created->first;
    }
    if (!containsDrawing(command.beforeDrawings, expected, work)) return false;
  }
  return true;
}

enum class ProjectionValidationStatus { Valid, Invalid, ResourceFailure };

ProjectionValidationStatus workspaceProjectionMatchesProduct(
    const ProductDocument& product,
    const ComponentExtractionColdCommand& command,
    ProjectionWorkBudget& work) noexcept {
  try {
    const auto& transaction = command.transaction;
    if (product.incarnation() != transaction.address.productIncarnation ||
        product.rootDefinitionId() != transaction.address.rootDefinitionId ||
        product.definitionStateId(transaction.address.ownerDefinitionId) !=
            transaction.beforeOwnerStateId)
      return ProjectionValidationStatus::Invalid;
    std::vector<ComponentWorkspaceInstanceKey> instances;
    std::vector<ComponentWorkspaceBodyKey> bodies;
    std::vector<ComponentWorkspaceDrawingKey> drawings;
    if (const auto* part = product.rootPartDefinition()) {
      if (!transaction.convertedPartRoot)
        return ProjectionValidationStatus::Invalid;
      ComponentWorkspaceInstanceKey root{product.rootDefinitionId(),
                                         std::nullopt};
      instances.push_back(root);
      bodies.reserve(part->document().bodies().size());
      for (const auto& body : part->document().bodies())
        bodies.push_back({root, body.id()});
    } else if (product.rootComponentDefinition()) {
      if (transaction.convertedPartRoot)
        return ProjectionValidationStatus::Invalid;
      const auto traversal =
          expandComponentGraph(product.definitions(), product.rootDefinitionId());
      if (!traversal) return ProjectionValidationStatus::ResourceFailure;
      instances.reserve(traversal.visits.size());
      for (const auto& visit : traversal.visits) {
        ComponentWorkspaceInstanceKey instance{visit.definitionId, visit.path};
        instances.push_back(instance);
        const auto* definition =
            product.definitions().findComponent(visit.definitionId);
        const auto* document =
            definition ? &definition->document() : nullptr;
        if (!document) return ProjectionValidationStatus::Invalid;
        if (document->bodies().size() >
            kMaximumWorkspaceBodies - bodies.size())
          return ProjectionValidationStatus::ResourceFailure;
        for (const auto& body : document->bodies())
          bodies.push_back({instance, body.id()});
        if (definition->drawings().size() >
            kMaximumWorkspaceDrawings - drawings.size())
          return ProjectionValidationStatus::ResourceFailure;
        for (const auto& drawing : definition->drawings())
          drawings.push_back({instance, drawing.id()});
      }
    } else {
      return ProjectionValidationStatus::Invalid;
    }
    std::sort(instances.begin(), instances.end(),
              [&work](const auto& left, const auto& right) {
                return work.instanceLess(left, right);
              });
    std::sort(bodies.begin(), bodies.end(),
              [&work](const auto& left, const auto& right) {
                return work.bodyLess(left, right);
              });
    std::sort(drawings.begin(), drawings.end(),
              [&work](const auto& left, const auto& right) {
                return work.drawingLess(left, right);
              });
    if (instances.size() != command.beforeInstances.size() ||
        bodies.size() != command.beforeBodies.size() ||
        drawings.size() != command.beforeDrawings.size())
      return ProjectionValidationStatus::Invalid;
    return instances == command.beforeInstances &&
                   bodies == command.beforeBodies &&
                   drawings == command.beforeDrawings
               ? ProjectionValidationStatus::Valid
               : ProjectionValidationStatus::Invalid;
  } catch (const ProjectionWorkLimitExceeded&) {
    return ProjectionValidationStatus::ResourceFailure;
  } catch (...) {
    return ProjectionValidationStatus::ResourceFailure;
  }
}

bool validComponentExtractionCommand(
    const ComponentExtractionColdCommand& command,
    ProjectionWorkBudget& work) {
  const auto& transaction = command.transaction;
  const auto& address = transaction.address;
  std::vector<ComponentDrawing> expectedParentDrawings;
  std::vector<ComponentDrawing> expectedChildDrawings;
  expectedParentDrawings.reserve(transaction.beforeOwnerDrawings.size());
  expectedChildDrawings.reserve(transaction.beforeOwnerDrawings.size());
  for (const auto& drawing : transaction.beforeOwnerDrawings) {
    const bool moved = std::binary_search(drawing.sourceBodyIds().begin(),
                                          drawing.sourceBodyIds().end(),
                                          address.bodyId);
    if (moved) {
      if (drawing.sourceBodyIds().size() != 1) return false;
      expectedChildDrawings.push_back(drawing);
    } else {
      expectedParentDrawings.push_back(drawing);
    }
  }
  if (!address.productIncarnation.isValid() ||
      !address.rootDefinitionId.isValid() ||
      !address.ownerDefinitionId.isValid() ||
      !address.expectedOwnerStateId.isValid() ||
      address.bodyId == kInvalidBodyId || !address.childDefinitionId.isValid() ||
      address.childDefinitionId == address.ownerDefinitionId ||
      !address.childOccurrenceId.isValid() ||
      transaction.plan.status != ComponentExtractionPlanStatus::Planned ||
      transaction.plan.bodyId != address.bodyId ||
      !transaction.beforeOwnerStateId.isValid() ||
      !transaction.afterOwnerStateId.isValid() ||
      !transaction.createdChildStateId.isValid() ||
      transaction.beforeNextStateIdValue == 0 ||
      transaction.afterNextStateIdValue == 0 ||
      transaction.createdChildNextStateIdValue == 0 ||
      address.expectedOwnerStateId != transaction.beforeOwnerStateId ||
      transaction.afterOwnerStateId.value() !=
          transaction.beforeNextStateIdValue ||
      transaction.beforeNextStateIdValue ==
          std::numeric_limits<std::uint64_t>::max() ||
      transaction.afterNextStateIdValue !=
          transaction.beforeNextStateIdValue + 1 ||
      transaction.createdChildStateId != DefinitionStateId{1} ||
      transaction.createdChildNextStateIdValue != 2 ||
      (transaction.convertedPartRoot &&
       address.ownerDefinitionId != address.rootDefinitionId) ||
      (transaction.convertedPartRoot &&
       (!transaction.beforeOwnerDrawings.empty() ||
        !transaction.afterOwnerDrawings.empty() ||
        !transaction.createdChildDrawings.empty())) ||
      (!transaction.convertedPartRoot &&
       (transaction.afterOwnerDrawings != expectedParentDrawings ||
        transaction.createdChildDrawings != expectedChildDrawings)) ||
      !validWorkspaceTransition(command, work))
    return false;
  return true;
}

bool validComponentPlacementCommand(
    const ComponentPlacementColdCommand& command,
    ProjectionWorkBudget& work) {
  const auto& transaction = command.transaction;
  const auto& request = transaction.request;
  if (!request.productIncarnation.isValid() ||
      !request.rootDefinitionId.isValid() || !request.path ||
      !request.expectedTargetDefinitionId.isValid() ||
      !request.expectedOwnerStateId.isValid() ||
      !transaction.ownerDefinitionId.isValid() ||
      !transaction.beforeOwnerStateId.isValid() ||
      !transaction.afterOwnerStateId.isValid() ||
      request.expectedOwnerStateId != transaction.beforeOwnerStateId ||
      request.expectedPlacement != transaction.beforePlacement ||
      transaction.beforePlacement == transaction.afterPlacement ||
      transaction.afterOwnerStateId.value() !=
          transaction.beforeNextStateIdValue ||
      transaction.beforeNextStateIdValue == 0 ||
      transaction.beforeNextStateIdValue ==
          std::numeric_limits<std::uint64_t>::max() ||
      transaction.afterNextStateIdValue !=
          transaction.beforeNextStateIdValue + 1 ||
      !isComponentPersistenceFormat(command.beforePersistence.format) ||
      !isComponentPersistenceFormat(command.afterPersistence.format) ||
      !validWorkspaceProjection(command.beforeInstances, command.beforeBodies,
                                request.rootDefinitionId, work) ||
      !validWorkspaceProjection(command.afterInstances, command.afterBodies,
                                request.rootDefinitionId, work) ||
      command.beforeInstances != command.afterInstances ||
      command.beforeBodies != command.afterBodies ||
      !validSemanticState(command.beforeState, command.beforeInstances,
                          command.beforeBodies, work) ||
      !validSemanticState(command.afterState, command.afterInstances,
                          command.afterBodies, work))
    return false;
  return true;
}

bool validComponentHomePlacementCommand(
    const ComponentHomePlacementColdCommand& command,
    ProjectionWorkBudget& work) {
  const auto& transaction = command.transaction;
  const auto& request = transaction.request;
  const auto validTransaction = [](const ComponentHomePlacementTransaction& item) {
    const auto& itemRequest = item.request;
    if (!itemRequest.productIncarnation.isValid() ||
        !itemRequest.rootDefinitionId.isValid() ||
        itemRequest.path.entries().empty() ||
        !itemRequest.expectedTargetDefinitionId.isValid() ||
        !itemRequest.expectedOwnerDefinitionId.isValid() ||
        !itemRequest.expectedOwnerStateId.isValid() ||
        !item.ownerDefinitionId.isValid() ||
        item.ownerDefinitionId != itemRequest.expectedOwnerDefinitionId ||
        !item.beforeOwnerStateId.isValid() ||
        !item.afterOwnerStateId.isValid() ||
        itemRequest.expectedOwnerStateId != item.beforeOwnerStateId ||
        itemRequest.expectedPlacement != item.beforePlacement ||
        itemRequest.expectedHomePlacement != item.beforeHomePlacement ||
        item.beforePlacement == item.beforeHomePlacement ||
        item.afterOwnerStateId.value() != item.beforeNextStateIdValue ||
        item.beforeNextStateIdValue == 0 ||
        item.beforeNextStateIdValue ==
            std::numeric_limits<std::uint64_t>::max() ||
        item.afterNextStateIdValue != item.beforeNextStateIdValue + 1)
      return false;
    if (itemRequest.operation == ComponentHomePlacementOperation::AdoptCurrent)
      return item.afterPlacement == item.beforePlacement &&
             item.afterHomePlacement == item.beforePlacement;
    return item.afterPlacement == item.beforeHomePlacement &&
           item.afterHomePlacement == item.beforeHomePlacement;
  };
  if (!validTransaction(transaction) ||
      !request.rootDefinitionId.isValid() ||
      !isComponentPersistenceFormat(command.beforePersistence.format) ||
      !isComponentPersistenceFormat(command.afterPersistence.format) ||
      !validWorkspaceProjection(command.beforeInstances, command.beforeBodies,
                                request.rootDefinitionId, work) ||
      !validWorkspaceProjection(command.afterInstances, command.afterBodies,
                                request.rootDefinitionId, work) ||
      command.beforeInstances != command.afterInstances ||
      command.beforeBodies != command.afterBodies ||
      !validSemanticState(command.beforeState, command.beforeInstances,
                          command.beforeBodies, work) ||
      !validSemanticState(command.afterState, command.afterInstances,
                          command.afterBodies, work))
    return false;
  for (const auto& additional : command.additionalTransactions) {
    if (!validTransaction(additional) ||
        request.operation != ComponentHomePlacementOperation::Restore ||
        additional.request.operation != ComponentHomePlacementOperation::Restore ||
        additional.request.productIncarnation != request.productIncarnation ||
        additional.request.rootDefinitionId != request.rootDefinitionId)
      return false;
  }
  return true;
}

ProjectionValidationStatus workspaceProjectionMatchesProduct(
    const ProductDocument& product,
    const ComponentPlacementColdCommand& command,
    ProjectionWorkBudget& work) noexcept {
  try {
    const auto& transaction = command.transaction;
    const auto prepared =
        product.prepareComponentPlacement(transaction.request);
    if (!prepared || *prepared.transaction != transaction)
      return ProjectionValidationStatus::Invalid;
    if (product.incarnation() !=
            transaction.request.productIncarnation ||
        product.rootDefinitionId() !=
            transaction.request.rootDefinitionId ||
        product.rootComponentDefinition() == nullptr ||
        product.definitionStateId(transaction.ownerDefinitionId) !=
            transaction.beforeOwnerStateId)
      return ProjectionValidationStatus::Invalid;
    const auto traversal = expandComponentGraph(
        product.definitions(), product.rootDefinitionId());
    if (!traversal) return ProjectionValidationStatus::ResourceFailure;
    std::vector<ComponentWorkspaceInstanceKey> instances;
    std::vector<ComponentWorkspaceBodyKey> bodies;
    instances.reserve(traversal.visits.size());
    for (const auto& visit : traversal.visits) {
      ComponentWorkspaceInstanceKey instance{visit.definitionId, visit.path};
      instances.push_back(instance);
      const auto* document = product.componentDocument(visit.definitionId);
      if (!document) return ProjectionValidationStatus::Invalid;
      if (document->bodies().size() >
          kMaximumWorkspaceBodies - bodies.size())
        return ProjectionValidationStatus::ResourceFailure;
      for (const auto& body : document->bodies())
        bodies.push_back({instance, body.id()});
    }
    std::sort(instances.begin(), instances.end(),
              [&work](const auto& left, const auto& right) {
                return work.instanceLess(left, right);
              });
    std::sort(bodies.begin(), bodies.end(),
              [&work](const auto& left, const auto& right) {
                return work.bodyLess(left, right);
              });
    return instances == command.beforeInstances &&
                   bodies == command.beforeBodies
               ? ProjectionValidationStatus::Valid
               : ProjectionValidationStatus::Invalid;
  } catch (const ProjectionWorkLimitExceeded&) {
    return ProjectionValidationStatus::ResourceFailure;
  } catch (...) {
    return ProjectionValidationStatus::ResourceFailure;
  }
}

ProjectionValidationStatus workspaceProjectionMatchesProduct(
    const ProductDocument& product,
    const ComponentHomePlacementColdCommand& command,
    ProjectionWorkBudget& work) noexcept {
  try {
    const auto& transaction = command.transaction;
    if (command.additionalTransactions.empty()) {
      const auto prepared =
          product.prepareComponentHomePlacement(transaction.request);
      if (!prepared || *prepared.transaction != transaction)
        return ProjectionValidationStatus::Invalid;
    } else {
      const auto prepared = product.prepareAllComponentHomePlacements();
      if (!prepared || prepared.transactions.size() !=
                           command.additionalTransactions.size() + 1 ||
          prepared.transactions.front() != transaction ||
          !std::equal(command.additionalTransactions.begin(),
                      command.additionalTransactions.end(),
                      prepared.transactions.begin() + 1))
        return ProjectionValidationStatus::Invalid;
    }
    if (product.incarnation() != transaction.request.productIncarnation ||
        product.rootDefinitionId() !=
            transaction.request.rootDefinitionId ||
        product.rootComponentDefinition() == nullptr ||
        product.definitionStateId(transaction.ownerDefinitionId) !=
            transaction.beforeOwnerStateId)
      return ProjectionValidationStatus::Invalid;
    const auto traversal = expandComponentGraph(
        product.definitions(), product.rootDefinitionId());
    if (!traversal) return ProjectionValidationStatus::ResourceFailure;
    std::vector<ComponentWorkspaceInstanceKey> instances;
    std::vector<ComponentWorkspaceBodyKey> bodies;
    instances.reserve(traversal.visits.size());
    for (const auto& visit : traversal.visits) {
      ComponentWorkspaceInstanceKey instance{visit.definitionId, visit.path};
      instances.push_back(instance);
      const auto* document = product.componentDocument(visit.definitionId);
      if (!document) return ProjectionValidationStatus::Invalid;
      if (document->bodies().size() >
          kMaximumWorkspaceBodies - bodies.size())
        return ProjectionValidationStatus::ResourceFailure;
      for (const auto& body : document->bodies())
        bodies.push_back({instance, body.id()});
    }
    std::sort(instances.begin(), instances.end(),
              [&work](const auto& left, const auto& right) {
                return work.instanceLess(left, right);
              });
    std::sort(bodies.begin(), bodies.end(),
              [&work](const auto& left, const auto& right) {
                return work.bodyLess(left, right);
              });
    return instances == command.beforeInstances &&
                   bodies == command.beforeBodies
               ? ProjectionValidationStatus::Valid
               : ProjectionValidationStatus::Invalid;
  } catch (const ProjectionWorkLimitExceeded&) {
    return ProjectionValidationStatus::ResourceFailure;
  } catch (...) {
    return ProjectionValidationStatus::ResourceFailure;
  }
}

ModelCommandHistoryStatus extractionFailureStatus(
    ComponentExtractionStatus status) noexcept {
  switch (status) {
    case ComponentExtractionStatus::SuccessorRejected:
      return ModelCommandHistoryStatus::SuccessorRejected;
    case ComponentExtractionStatus::SuccessorResourceFailure:
      return ModelCommandHistoryStatus::SuccessorResourceFailure;
    case ComponentExtractionStatus::ResourceFailure:
      return ModelCommandHistoryStatus::ResourceLimit;
    case ComponentExtractionStatus::Committed:
    case ComponentExtractionStatus::Reverted:
      return ModelCommandHistoryStatus::Committed;
    default:
      return ModelCommandHistoryStatus::ModelRejected;
  }
}

ModelCommandHistoryStatus placementFailureStatus(
    ComponentPlacementStatus status) noexcept {
  switch (status) {
    case ComponentPlacementStatus::SuccessorRejected:
      return ModelCommandHistoryStatus::SuccessorRejected;
    case ComponentPlacementStatus::SuccessorResourceFailure:
      return ModelCommandHistoryStatus::SuccessorResourceFailure;
    case ComponentPlacementStatus::ResourceFailure:
      return ModelCommandHistoryStatus::ResourceLimit;
    case ComponentPlacementStatus::Committed:
    case ComponentPlacementStatus::Reverted:
      return ModelCommandHistoryStatus::Committed;
    case ComponentPlacementStatus::NoChange:
      return ModelCommandHistoryStatus::NoChange;
    default:
      return ModelCommandHistoryStatus::ModelRejected;
  }
}

ModelCommandHistoryStatus homePlacementFailureStatus(
    ComponentHomePlacementStatus status) noexcept {
  switch (status) {
    case ComponentHomePlacementStatus::SuccessorRejected:
      return ModelCommandHistoryStatus::SuccessorRejected;
    case ComponentHomePlacementStatus::SuccessorResourceFailure:
      return ModelCommandHistoryStatus::SuccessorResourceFailure;
    case ComponentHomePlacementStatus::ResourceFailure:
      return ModelCommandHistoryStatus::ResourceLimit;
    case ComponentHomePlacementStatus::Committed:
    case ComponentHomePlacementStatus::Reverted:
      return ModelCommandHistoryStatus::Committed;
    case ComponentHomePlacementStatus::NoChange:
      return ModelCommandHistoryStatus::NoChange;
    default:
      return ModelCommandHistoryStatus::ModelRejected;
  }
}

class ApplyingGuard final {
 public:
  explicit ApplyingGuard(bool& value) : value_(value) { value_ = true; }
  ~ApplyingGuard() { value_ = false; }

 private:
  bool& value_;
};

}  // namespace

ModelCommandHistory::ModelCommandHistory(std::size_t byteBudget)
    : impl_(std::make_unique<Impl>(byteBudget)) {}

ModelCommandHistory::~ModelCommandHistory() = default;
ModelCommandHistory::ModelCommandHistory(ModelCommandHistory&&) noexcept =
    default;
ModelCommandHistory& ModelCommandHistory::operator=(
    ModelCommandHistory&&) noexcept = default;

void ModelCommandHistory::throwInjectedFailure(std::uint8_t point) {
  if (testFailurePoint_ != point) return;
  switch (static_cast<detail::ModelCommandHistoryFailureKind>(
      testFailureKind_)) {
    case detail::ModelCommandHistoryFailureKind::BadAlloc:
      throw std::bad_alloc();
    case detail::ModelCommandHistoryFailureKind::Runtime:
      throw std::runtime_error("model command history injected failure");
    case detail::ModelCommandHistoryFailureKind::Unknown:
      throw 1;
    default:
      return;
  }
}

void ModelCommandHistory::setNextRevisionForTesting(
    std::uint64_t value) noexcept {
  impl_->nextRevision = value;
  impl_->revisionExhausted = false;
}

void ModelCommandHistory::setProjectionCompareLimitForTesting(
    std::size_t value) noexcept {
  testProjectionCompareLimit_ = value;
}

std::size_t ModelCommandHistory::projectionCompareCountForTesting() const
    noexcept {
  return testProjectionCompareCount_;
}

HistoryCommitResult ModelCommandHistory::recordTransition(
    Document& current, Document previous, EditorCommittedState before,
    EditorCommittedState after) noexcept {
  HistoryCommitResult result;
  if (impl_->applying) {
    result.status = HistoryCommitStatus::NoChange;
    result.state = std::move(after);
    return result;
  }
  try {
    auto delta = DocumentDelta::create(previous, current);
    if (delta.empty() && before.presentation == after.presentation) {
      result.status = HistoryCommitStatus::NoChange;
      result.state = std::move(before);
      return result;
    }
    auto retained = checkedAdd(delta.retainedBytes(), sizeof(Impl::Command));
    if (retained)
      retained = checkedAdd(*retained, editorStateOwnedBytes(before));
    if (retained)
      retained = checkedAdd(*retained, editorStateOwnedBytes(after));
    if (!retained || *retained > impl_->byteBudget) {
      static_assert(std::is_nothrow_move_assignable_v<Document>);
      current = std::move(previous);
      result.status = HistoryCommitStatus::Rejected;
      result.state = std::move(before);
      result.error = "history byte budget exceeded";
      return result;
    }
    result.state = after;
    const std::uint64_t beforeRevision = impl_->currentRevision;
    const std::uint64_t afterRevision = impl_->nextRevision;
    // Keep the caller-owned rollback values intact until reserve/preparation
    // succeeds. A failure must restore both Document and editor state.
    Impl::DocumentCommand documentCommand{delta, before, after};
    Impl::Command command{std::move(documentCommand), *retained,
                          beforeRevision, afterRevision};
    if (!impl_->prepare(std::move(command))) {
      current = std::move(previous);
      result.status = HistoryCommitStatus::Rejected;
      result.state = std::move(before);
      result.error = "history byte budget exceeded";
      return result;
    }
    (void)commitPrepared();
    impl_->currentRevision = afterRevision;
    result.status = HistoryCommitStatus::Accepted;
    return result;
  } catch (...) {
    static_assert(std::is_nothrow_move_assignable_v<Document>);
    current = std::move(previous);
    result.status = HistoryCommitStatus::Rejected;
    result.state = std::move(before);
    result.error = exceptionText("history transition creation failed");
    return result;
  }
}

HistoryApplyResult ModelCommandHistory::undo(
    Document& current, const EditorCommittedState& currentState) noexcept {
  HistoryApplyResult result;
  try {
    result.state = currentState;
  } catch (...) {
    result.error = exceptionText("history undo state capture failed");
    return result;
  }
  if (impl_->cursor == 0 || impl_->applying || impl_->pending) return result;
  if (impl_->kind(impl_->commands[impl_->cursor - 1]) !=
      ModelCommandKind::DocumentTransition) {
    result.error = "history command kind mismatch";
    return result;
  }
  try {
    Document rollback = current;
    auto& command = impl_->commands[impl_->cursor - 1];
    const auto& document = std::get<Impl::DocumentCommand>(command.payload);
    result.state = document.before;
    ApplyingGuard guard(impl_->applying);
    try {
      document.delta.applyBackward(current);
    } catch (...) {
      current = std::move(rollback);
      result.state = currentState;
      result.error = exceptionText("history undo failed");
      return result;
    }
    --impl_->cursor;
    impl_->currentRevision = command.beforeRevision;
    result.changed = true;
    return result;
  } catch (...) {
    result.state = {};
    result.error = exceptionText("history undo preparation failed");
    return result;
  }
}

HistoryApplyResult ModelCommandHistory::redo(
    Document& current, const EditorCommittedState& currentState) noexcept {
  HistoryApplyResult result;
  try {
    result.state = currentState;
  } catch (...) {
    result.error = exceptionText("history redo state capture failed");
    return result;
  }
  if (impl_->cursor >= impl_->commands.size() || impl_->applying ||
      impl_->pending)
    return result;
  if (impl_->kind(impl_->commands[impl_->cursor]) !=
      ModelCommandKind::DocumentTransition) {
    result.error = "history command kind mismatch";
    return result;
  }
  try {
    Document rollback = current;
    auto& command = impl_->commands[impl_->cursor];
    const auto& document = std::get<Impl::DocumentCommand>(command.payload);
    result.state = document.after;
    ApplyingGuard guard(impl_->applying);
    try {
      document.delta.applyForward(current);
    } catch (...) {
      current = std::move(rollback);
      result.state = currentState;
      result.error = exceptionText("history redo failed");
      return result;
    }
    ++impl_->cursor;
    impl_->currentRevision = command.afterRevision;
    result.changed = true;
    return result;
  } catch (...) {
    result.state = {};
    result.error = exceptionText("history redo preparation failed");
    return result;
  }
}

ModelCommandHistoryStatus ModelCommandHistory::prepareComponentExtraction(
    const ProductDocument& product,
    const ComponentExtractionColdCommand& command) noexcept {
  if (impl_->applying || impl_->pending)
    return ModelCommandHistoryStatus::Busy;
  ProjectionWorkBudget work{testProjectionCompareLimit_, 0};
  testProjectionCompareCount_ = 0;
  try {
    if (!validComponentExtractionCommand(command, work)) {
      testProjectionCompareCount_ = work.count;
      return ModelCommandHistoryStatus::InvalidCommand;
    }
    if (!validComponentPersistenceTransition(
            product, command.beforePersistence, command.afterPersistence,
            command.transaction.convertedPartRoot)) {
      testProjectionCompareCount_ = work.count;
      return ModelCommandHistoryStatus::InvalidCommand;
    }
    const auto projection =
        workspaceProjectionMatchesProduct(product, command, work);
    testProjectionCompareCount_ = work.count;
    if (projection == ProjectionValidationStatus::ResourceFailure)
      return ModelCommandHistoryStatus::ResourceLimit;
    if (projection != ProjectionValidationStatus::Valid)
      return ModelCommandHistoryStatus::InvalidCommand;
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::ModelCommandHistoryFailurePoint::PrepareAccounting));
    ComponentExtractionColdCommand retainedCommand = command;
    const auto retained = componentExtractionOwnedBytes(retainedCommand);
    if (!retained || *retained > impl_->byteBudget)
      return ModelCommandHistoryStatus::ResourceLimit;
    Impl::Command entry{std::move(retainedCommand), *retained,
                        impl_->currentRevision, impl_->nextRevision};
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::ModelCommandHistoryFailurePoint::PrepareReserve));
    if (!impl_->prepare(std::move(entry)))
      return ModelCommandHistoryStatus::ResourceLimit;
    return ModelCommandHistoryStatus::Prepared;
  } catch (const ProjectionWorkLimitExceeded&) {
    testProjectionCompareCount_ = work.count;
    return ModelCommandHistoryStatus::ResourceLimit;
  } catch (...) {
    testProjectionCompareCount_ = work.count;
    return ModelCommandHistoryStatus::ResourceLimit;
  }
}

ModelCommandHistoryStatus ModelCommandHistory::commitPrepared() noexcept {
  if (!impl_->pending) return ModelCommandHistoryStatus::NoChange;
  static_assert(std::is_nothrow_move_constructible_v<Impl::Command>);
  static_assert(std::is_nothrow_move_assignable_v<Impl::Command>);
  auto pending = std::move(*impl_->pending);
  impl_->pending.reset();
  impl_->commands.erase(impl_->commands.begin() +
                            static_cast<std::ptrdiff_t>(impl_->cursor),
                        impl_->commands.end());
  impl_->commands.push_back(std::move(pending.command));
  if (pending.dropFrontCount != 0) {
    impl_->commands.erase(
        impl_->commands.begin(),
        impl_->commands.begin() +
            static_cast<std::ptrdiff_t>(pending.dropFrontCount));
  }
  impl_->cursor = impl_->commands.size();
  impl_->retained = pending.retainedAfterCommit;
  impl_->currentRevision = impl_->commands.back().afterRevision;
  ++impl_->nextRevision;
  return ModelCommandHistoryStatus::Committed;
}

void ModelCommandHistory::discardPrepared() noexcept { impl_->pending.reset(); }

ComponentExtractionHistoryResult
ModelCommandHistory::undoComponentExtraction(
    ProductDocument& product,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  ComponentExtractionHistoryResult result;
  if (impl_->applying || impl_->pending) {
    result.status = ModelCommandHistoryStatus::Busy;
    return result;
  }
  if (impl_->cursor == 0) {
    result.status = ModelCommandHistoryStatus::NothingToUndo;
    return result;
  }
  auto& entry = impl_->commands[impl_->cursor - 1];
  if (impl_->kind(entry) != ModelCommandKind::ComponentExtraction) {
    result.status = ModelCommandHistoryStatus::WrongCommandKind;
    return result;
  }
  const auto& command =
      std::get<ComponentExtractionColdCommand>(entry.payload);
  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::ModelCommandHistoryFailurePoint::UndoStatePreparation));
    auto state = command.beforeState;
    auto persistence = command.beforePersistence;
    ApplyingGuard guard(impl_->applying);
    const auto model = product.revertComponentExtraction(
        command.transaction, prepareSuccessor);
    result.modelStatus = model;
    result.status = extractionFailureStatus(model);
    if (model != ComponentExtractionStatus::Reverted) return result;
    --impl_->cursor;
    impl_->currentRevision = entry.beforeRevision;
    result.restoredState = std::move(state);
    result.restoredPersistence = persistence;
    return result;
  } catch (...) {
    result.status = ModelCommandHistoryStatus::ResourceLimit;
    return result;
  }
}

ComponentExtractionHistoryResult
ModelCommandHistory::redoComponentExtraction(
    ProductDocument& product,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  ComponentExtractionHistoryResult result;
  if (impl_->applying || impl_->pending) {
    result.status = ModelCommandHistoryStatus::Busy;
    return result;
  }
  if (impl_->cursor >= impl_->commands.size()) {
    result.status = ModelCommandHistoryStatus::NothingToRedo;
    return result;
  }
  auto& entry = impl_->commands[impl_->cursor];
  if (impl_->kind(entry) != ModelCommandKind::ComponentExtraction) {
    result.status = ModelCommandHistoryStatus::WrongCommandKind;
    return result;
  }
  const auto& command =
      std::get<ComponentExtractionColdCommand>(entry.payload);
  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::ModelCommandHistoryFailurePoint::RedoStatePreparation));
    auto state = command.afterState;
    auto persistence = command.afterPersistence;
    ApplyingGuard guard(impl_->applying);
    const auto model = product.applyComponentExtraction(
        command.transaction, prepareSuccessor);
    result.modelStatus = model;
    result.status = extractionFailureStatus(model);
    if (model != ComponentExtractionStatus::Committed) return result;
    ++impl_->cursor;
    impl_->currentRevision = entry.afterRevision;
    result.restoredState = std::move(state);
    result.restoredPersistence = persistence;
    return result;
  } catch (...) {
    result.status = ModelCommandHistoryStatus::ResourceLimit;
    return result;
  }
}

ModelCommandHistoryStatus ModelCommandHistory::prepareComponentPlacement(
    const ProductDocument& product,
    const ComponentPlacementColdCommand& command) noexcept {
  if (impl_->applying || impl_->pending)
    return ModelCommandHistoryStatus::Busy;
  ProjectionWorkBudget work{testProjectionCompareLimit_, 0};
  testProjectionCompareCount_ = 0;
  try {
    if (!validComponentPlacementCommand(command, work)) {
      testProjectionCompareCount_ = work.count;
      return ModelCommandHistoryStatus::InvalidCommand;
    }
    if (!validComponentPersistenceTransition(
            product, command.beforePersistence, command.afterPersistence,
            false)) {
      testProjectionCompareCount_ = work.count;
      return ModelCommandHistoryStatus::InvalidCommand;
    }
    const auto projection =
        workspaceProjectionMatchesProduct(product, command, work);
    testProjectionCompareCount_ = work.count;
    if (projection == ProjectionValidationStatus::ResourceFailure)
      return ModelCommandHistoryStatus::ResourceLimit;
    if (projection != ProjectionValidationStatus::Valid)
      return ModelCommandHistoryStatus::InvalidCommand;
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::ModelCommandHistoryFailurePoint::PrepareAccounting));
    ComponentPlacementColdCommand retainedCommand = command;
    const auto retained = componentPlacementOwnedBytes(retainedCommand);
    if (!retained || *retained > impl_->byteBudget)
      return ModelCommandHistoryStatus::ResourceLimit;
    Impl::Command entry{std::move(retainedCommand), *retained,
                        impl_->currentRevision, impl_->nextRevision};
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::ModelCommandHistoryFailurePoint::PrepareReserve));
    if (!impl_->prepare(std::move(entry)))
      return ModelCommandHistoryStatus::ResourceLimit;
    return ModelCommandHistoryStatus::Prepared;
  } catch (const ProjectionWorkLimitExceeded&) {
    testProjectionCompareCount_ = work.count;
    return ModelCommandHistoryStatus::ResourceLimit;
  } catch (...) {
    testProjectionCompareCount_ = work.count;
    return ModelCommandHistoryStatus::ResourceLimit;
  }
}

ComponentPlacementHistoryResult ModelCommandHistory::undoComponentPlacement(
    ProductDocument& product,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  ComponentPlacementHistoryResult result;
  if (impl_->applying || impl_->pending) {
    result.status = ModelCommandHistoryStatus::Busy;
    return result;
  }
  if (impl_->cursor == 0) {
    result.status = ModelCommandHistoryStatus::NothingToUndo;
    return result;
  }
  auto& entry = impl_->commands[impl_->cursor - 1];
  if (impl_->kind(entry) != ModelCommandKind::ComponentPlacement) {
    result.status = ModelCommandHistoryStatus::WrongCommandKind;
    return result;
  }
  const auto& command =
      std::get<ComponentPlacementColdCommand>(entry.payload);
  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::ModelCommandHistoryFailurePoint::UndoStatePreparation));
    auto state = command.beforeState;
    auto persistence = command.beforePersistence;
    ApplyingGuard guard(impl_->applying);
    const auto model = product.revertComponentPlacement(
        command.transaction, prepareSuccessor);
    result.modelStatus = model;
    result.status = placementFailureStatus(model);
    if (model != ComponentPlacementStatus::Reverted) return result;
    --impl_->cursor;
    impl_->currentRevision = entry.beforeRevision;
    result.restoredState = std::move(state);
    result.restoredPersistence = persistence;
    return result;
  } catch (...) {
    result.status = ModelCommandHistoryStatus::ResourceLimit;
    return result;
  }
}

ComponentPlacementHistoryResult ModelCommandHistory::redoComponentPlacement(
    ProductDocument& product,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  ComponentPlacementHistoryResult result;
  if (impl_->applying || impl_->pending) {
    result.status = ModelCommandHistoryStatus::Busy;
    return result;
  }
  if (impl_->cursor >= impl_->commands.size()) {
    result.status = ModelCommandHistoryStatus::NothingToRedo;
    return result;
  }
  auto& entry = impl_->commands[impl_->cursor];
  if (impl_->kind(entry) != ModelCommandKind::ComponentPlacement) {
    result.status = ModelCommandHistoryStatus::WrongCommandKind;
    return result;
  }
  const auto& command =
      std::get<ComponentPlacementColdCommand>(entry.payload);
  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::ModelCommandHistoryFailurePoint::RedoStatePreparation));
    auto state = command.afterState;
    auto persistence = command.afterPersistence;
    ApplyingGuard guard(impl_->applying);
    const auto model = product.applyComponentPlacement(
        command.transaction, prepareSuccessor);
    result.modelStatus = model;
    result.status = placementFailureStatus(model);
    if (model != ComponentPlacementStatus::Committed) return result;
    ++impl_->cursor;
    impl_->currentRevision = entry.afterRevision;
    result.restoredState = std::move(state);
    result.restoredPersistence = persistence;
    return result;
  } catch (...) {
    result.status = ModelCommandHistoryStatus::ResourceLimit;
    return result;
  }
}

ModelCommandHistoryStatus ModelCommandHistory::prepareComponentHomePlacement(
    const ProductDocument& product,
    const ComponentHomePlacementColdCommand& command) noexcept {
  if (impl_->applying || impl_->pending)
    return ModelCommandHistoryStatus::Busy;
  ProjectionWorkBudget work{testProjectionCompareLimit_, 0};
  testProjectionCompareCount_ = 0;
  try {
    if (!validComponentHomePlacementCommand(command, work)) {
      testProjectionCompareCount_ = work.count;
      return ModelCommandHistoryStatus::InvalidCommand;
    }
    if (!validComponentPersistenceTransition(
            product, command.beforePersistence, command.afterPersistence,
            false)) {
      testProjectionCompareCount_ = work.count;
      return ModelCommandHistoryStatus::InvalidCommand;
    }
    const auto projection =
        workspaceProjectionMatchesProduct(product, command, work);
    testProjectionCompareCount_ = work.count;
    if (projection == ProjectionValidationStatus::ResourceFailure)
      return ModelCommandHistoryStatus::ResourceLimit;
    if (projection != ProjectionValidationStatus::Valid)
      return ModelCommandHistoryStatus::InvalidCommand;
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::ModelCommandHistoryFailurePoint::PrepareAccounting));
    ComponentHomePlacementColdCommand retainedCommand = command;
    const auto retained = componentHomePlacementOwnedBytes(retainedCommand);
    if (!retained || *retained > impl_->byteBudget)
      return ModelCommandHistoryStatus::ResourceLimit;
    Impl::Command entry{std::move(retainedCommand), *retained,
                        impl_->currentRevision, impl_->nextRevision};
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::ModelCommandHistoryFailurePoint::PrepareReserve));
    if (!impl_->prepare(std::move(entry)))
      return ModelCommandHistoryStatus::ResourceLimit;
    return ModelCommandHistoryStatus::Prepared;
  } catch (const ProjectionWorkLimitExceeded&) {
    testProjectionCompareCount_ = work.count;
    return ModelCommandHistoryStatus::ResourceLimit;
  } catch (...) {
    testProjectionCompareCount_ = work.count;
    return ModelCommandHistoryStatus::ResourceLimit;
  }
}

ComponentHomePlacementHistoryResult
ModelCommandHistory::undoComponentHomePlacement(
    ProductDocument& product,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  ComponentHomePlacementHistoryResult result;
  if (impl_->applying || impl_->pending) {
    result.status = ModelCommandHistoryStatus::Busy;
    return result;
  }
  if (impl_->cursor == 0) {
    result.status = ModelCommandHistoryStatus::NothingToUndo;
    return result;
  }
  auto& entry = impl_->commands[impl_->cursor - 1];
  if (impl_->kind(entry) != ModelCommandKind::ComponentHomePlacement) {
    result.status = ModelCommandHistoryStatus::WrongCommandKind;
    return result;
  }
  const auto& command =
      std::get<ComponentHomePlacementColdCommand>(entry.payload);
  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::ModelCommandHistoryFailurePoint::UndoStatePreparation));
    auto state = command.beforeState;
    auto persistence = command.beforePersistence;
    ApplyingGuard guard(impl_->applying);
    ComponentHomePlacementStatus model{};
    if (command.additionalTransactions.empty()) {
      model = product.revertComponentHomePlacement(command.transaction,
                                                    prepareSuccessor);
    } else {
      std::vector<ComponentHomePlacementTransaction> transactions;
      transactions.reserve(command.additionalTransactions.size() + 1);
      transactions.push_back(command.transaction);
      transactions.insert(transactions.end(),
                          command.additionalTransactions.begin(),
                          command.additionalTransactions.end());
      model = product.revertAllComponentHomePlacements(transactions,
                                                       prepareSuccessor);
    }
    result.modelStatus = model;
    result.status = homePlacementFailureStatus(model);
    if (model != ComponentHomePlacementStatus::Reverted) return result;
    --impl_->cursor;
    impl_->currentRevision = entry.beforeRevision;
    result.restoredState = std::move(state);
    result.restoredPersistence = persistence;
    return result;
  } catch (...) {
    result.status = ModelCommandHistoryStatus::ResourceLimit;
    return result;
  }
}

ComponentHomePlacementHistoryResult
ModelCommandHistory::redoComponentHomePlacement(
    ProductDocument& product,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  ComponentHomePlacementHistoryResult result;
  if (impl_->applying || impl_->pending) {
    result.status = ModelCommandHistoryStatus::Busy;
    return result;
  }
  if (impl_->cursor >= impl_->commands.size()) {
    result.status = ModelCommandHistoryStatus::NothingToRedo;
    return result;
  }
  auto& entry = impl_->commands[impl_->cursor];
  if (impl_->kind(entry) != ModelCommandKind::ComponentHomePlacement) {
    result.status = ModelCommandHistoryStatus::WrongCommandKind;
    return result;
  }
  const auto& command =
      std::get<ComponentHomePlacementColdCommand>(entry.payload);
  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::ModelCommandHistoryFailurePoint::RedoStatePreparation));
    auto state = command.afterState;
    auto persistence = command.afterPersistence;
    ApplyingGuard guard(impl_->applying);
    ComponentHomePlacementStatus model{};
    if (command.additionalTransactions.empty()) {
      model = product.applyComponentHomePlacement(command.transaction,
                                                   prepareSuccessor);
    } else {
      std::vector<ComponentHomePlacementTransaction> transactions;
      transactions.reserve(command.additionalTransactions.size() + 1);
      transactions.push_back(command.transaction);
      transactions.insert(transactions.end(),
                          command.additionalTransactions.begin(),
                          command.additionalTransactions.end());
      model = product.applyAllComponentHomePlacements(transactions,
                                                      prepareSuccessor);
    }
    result.modelStatus = model;
    result.status = homePlacementFailureStatus(model);
    if (model != ComponentHomePlacementStatus::Committed) return result;
    ++impl_->cursor;
    impl_->currentRevision = entry.afterRevision;
    result.restoredState = std::move(state);
    result.restoredPersistence = persistence;
    return result;
  } catch (...) {
    result.status = ModelCommandHistoryStatus::ResourceLimit;
    return result;
  }
}

void ModelCommandHistory::clear(bool currentIsSavepoint) noexcept {
  if (impl_->revisionExhausted || impl_->nextRevision == 0) return;
  const std::uint64_t replacementRevision = impl_->nextRevision;
  impl_->commands.clear();
  impl_->cursor = 0;
  impl_->pending.reset();
  impl_->retained = 0;
  impl_->applying = false;
  impl_->currentRevision = replacementRevision;
  if (replacementRevision == std::numeric_limits<std::uint64_t>::max())
    impl_->revisionExhausted = true;
  else
    ++impl_->nextRevision;
  if (currentIsSavepoint)
    impl_->savepointRevision = impl_->currentRevision;
  else
    impl_->savepointRevision.reset();
}

void ModelCommandHistory::markSavepoint() noexcept {
  impl_->savepointRevision = impl_->currentRevision;
}

void ModelCommandHistory::setByteBudget(std::size_t bytes) noexcept {
  impl_->byteBudget = bytes;
  impl_->enforceBudget();
}

bool ModelCommandHistory::canUndo() const noexcept {
  return impl_->cursor != 0;
}
bool ModelCommandHistory::canRedo() const noexcept {
  return impl_->cursor < impl_->commands.size();
}
bool ModelCommandHistory::isApplying() const noexcept {
  return impl_->applying;
}
std::size_t ModelCommandHistory::undoCount() const noexcept {
  return impl_->cursor;
}
std::size_t ModelCommandHistory::redoCount() const noexcept {
  return impl_->commands.size() - impl_->cursor;
}
std::size_t ModelCommandHistory::retainedBytes() const noexcept {
  return impl_->retained;
}
std::size_t ModelCommandHistory::byteBudget() const noexcept {
  return impl_->byteBudget;
}
bool ModelCommandHistory::isAtSavepoint() const noexcept {
  return impl_->savepointRevision &&
         *impl_->savepointRevision == impl_->currentRevision;
}
std::uint64_t ModelCommandHistory::currentRevision() const noexcept {
  return impl_->currentRevision;
}

std::optional<ModelCommandKind> ModelCommandHistory::nextUndoKind() const
    noexcept {
  if (impl_->cursor == 0) return std::nullopt;
  return Impl::kind(impl_->commands[impl_->cursor - 1]);
}

std::optional<ModelCommandKind> ModelCommandHistory::nextRedoKind() const
    noexcept {
  if (impl_->cursor >= impl_->commands.size()) return std::nullopt;
  return Impl::kind(impl_->commands[impl_->cursor]);
}

std::optional<std::reference_wrapper<
    const ComponentWorkspaceSemanticState>>
ModelCommandHistory::nextUndoComponentWorkspaceState() const noexcept {
  if (impl_->cursor == 0) return std::nullopt;
  const auto& command = impl_->commands[impl_->cursor - 1];
  const auto* extraction =
      std::get_if<ComponentExtractionColdCommand>(&command.payload);
  if (extraction) return std::cref(extraction->beforeState);
  const auto* placement =
      std::get_if<ComponentPlacementColdCommand>(&command.payload);
  if (placement) return std::cref(placement->beforeState);
  const auto* homePlacement =
      std::get_if<ComponentHomePlacementColdCommand>(&command.payload);
  if (!homePlacement) return std::nullopt;
  return std::cref(homePlacement->beforeState);
}

std::optional<std::reference_wrapper<
    const ComponentWorkspaceSemanticState>>
ModelCommandHistory::nextRedoComponentWorkspaceState() const noexcept {
  if (impl_->cursor >= impl_->commands.size()) return std::nullopt;
  const auto& command = impl_->commands[impl_->cursor];
  const auto* extraction =
      std::get_if<ComponentExtractionColdCommand>(&command.payload);
  if (extraction) return std::cref(extraction->afterState);
  const auto* placement =
      std::get_if<ComponentPlacementColdCommand>(&command.payload);
  if (placement) return std::cref(placement->afterState);
  const auto* homePlacement =
      std::get_if<ComponentHomePlacementColdCommand>(&command.payload);
  if (!homePlacement) return std::nullopt;
  return std::cref(homePlacement->afterState);
}

DocumentDeltaMetrics ModelCommandHistory::inspectDelta(
    const Document& before, const Document& after) {
  const auto delta = DocumentDelta::create(before, after);
  return {delta.sliceCount(), delta.retainedBytes()};
}

}  // namespace solidar
