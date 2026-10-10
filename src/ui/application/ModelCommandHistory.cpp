#include "ui/application/ModelCommandHistory.h"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "model/ChamferFeature.h"
#include "model/CircularPatternFeature.h"
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

namespace solidar {
namespace {

std::size_t topologyReferenceBytes(const EdgeReference& reference) {
  return reference.persistentTag.capacity();
}

std::size_t topologyReferenceBytes(const FaceReference& reference) {
  return reference.persistentTag.capacity();
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
           a.plane() == b.plane();
  });
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
          {FeatureKind::Mirror, mirrorEqual, baseFeatureBytes<MirrorFeature>},
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
  return sizeof(DocumentSketch) + item.name.capacity() +
         item.geometry.ownedBytes() + item.support.face.persistentTag.capacity();
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

struct DocumentDelta {
  std::vector<BodyDeltaSlice> bodies;
  std::vector<BodyStateDelta> bodyStates;
  std::vector<FeatureDeltaSlice> features;
  std::vector<SketchDeltaSlice> sketches;
  std::vector<ReferenceImageDeltaSlice> referenceImages;
  BoxParameters beforeBox;
  BoxParameters afterBox;
  std::size_t retainedBytes{};

  [[nodiscard]] bool empty() const noexcept {
    return bodies.empty() && bodyStates.empty() && features.empty() &&
           sketches.empty() && referenceImages.empty() &&
           beforeBox.widthMm == afterBox.widthMm &&
           beforeBox.depthMm == afterBox.depthMm &&
           beforeBox.heightMm == afterBox.heightMm;
  }
};

DocumentDelta makeDocumentDelta(const Document& before,
                                const Document& after) {
  DocumentDelta delta;
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
      delta.retainedBytes += bodyOwnedBytes(*slice.beforeBody);
      delta.bodies.push_back(std::move(slice));
      continue;
    }
    if (oldBody.name() != found->name() ||
        oldBody.visible() != found->visible()) {
      delta.bodyStates.push_back({oldBody.id(), oldBody.name(), found->name(),
                                  oldBody.visible(), found->visible()});
      delta.retainedBytes += sizeof(BodyStateDelta) +
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
      delta.retainedBytes += featureOwnedBytes(*slice.beforeFeature);
      if (slice.afterFeature)
        delta.retainedBytes += featureOwnedBytes(*slice.afterFeature);
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
      delta.retainedBytes += featureOwnedBytes(*slice.afterFeature);
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
    delta.retainedBytes += bodyOwnedBytes(*slice.afterBody);
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
    delta.retainedBytes += documentSketchOwnedBytes(old);
    if (slice.afterSketch)
      delta.retainedBytes += documentSketchOwnedBytes(*slice.afterSketch);
    delta.sketches.push_back(std::move(slice));
  }
  for (std::size_t index = 0; index < after.sketches().size(); ++index) {
    const auto& sketch = after.sketches()[index];
    if (before.findSketch(sketch.id)) continue;
    SketchDeltaSlice slice;
    slice.beforeIndex = before.sketches().size();
    slice.afterIndex = index;
    slice.afterSketch = sketch;
    delta.retainedBytes += documentSketchOwnedBytes(sketch);
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
    delta.retainedBytes += referenceImageOwnedBytes(old);
    if (slice.afterImage)
      delta.retainedBytes += referenceImageOwnedBytes(*slice.afterImage);
    delta.referenceImages.push_back(std::move(slice));
  }
  for (std::size_t index = 0; index < after.referenceImages().size(); ++index) {
    const auto& image = after.referenceImages()[index];
    if (before.findReferenceImage(image.id)) continue;
    ReferenceImageDeltaSlice slice;
    slice.beforeIndex = before.referenceImages().size();
    slice.afterIndex = index;
    slice.afterImage = image;
    delta.retainedBytes += referenceImageOwnedBytes(image);
    delta.referenceImages.push_back(std::move(slice));
  }
  delta.retainedBytes += delta.bodies.capacity() * sizeof(BodyDeltaSlice) +
                         delta.bodyStates.capacity() * sizeof(BodyStateDelta) +
                         delta.features.capacity() *
                             sizeof(FeatureDeltaSlice) +
                         delta.sketches.capacity() * sizeof(SketchDeltaSlice) +
                         delta.referenceImages.capacity() *
                             sizeof(ReferenceImageDeltaSlice);
  return delta;
}

void applyDocumentDelta(Document& document, const DocumentDelta& delta,
                        bool forward) {
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
  if (!document.recompute())
    throw std::runtime_error(document.rebuildError());
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

class ModelCommandHistory::Impl final {
 public:
  struct Command {
    std::shared_ptr<const DocumentDelta> delta;
    EditorCommittedState before;
    EditorCommittedState after;
    std::size_t retainedBytes{};
    std::uint64_t beforeRevision{};
    std::uint64_t afterRevision{};
  };

  explicit Impl(std::size_t budget) : byteBudget(budget) {}

  void eraseRedo() noexcept {
    for (const auto& command : redo)
      retained -= std::min(retained, command.retainedBytes);
    redo.clear();
  }

  void enforceBudget() noexcept {
    while (!undo.empty() &&
           (undo.size() > ModelCommandHistory::kMaxCommands ||
            retained > byteBudget)) {
      retained -= std::min(retained, undo.front().retainedBytes);
      undo.erase(undo.begin());
    }
    while (!redo.empty() && retained > byteBudget) {
      retained -= std::min(retained, redo.front().retainedBytes);
      redo.erase(redo.begin());
    }
  }

  std::vector<Command> undo;
  std::vector<Command> redo;
  std::size_t retained{};
  std::size_t byteBudget{ModelCommandHistory::kDefaultByteBudget};
  bool applying{};
  std::uint64_t currentRevision{1};
  std::optional<std::uint64_t> savepointRevision{1};
  std::uint64_t nextRevision{2};
};

ModelCommandHistory::ModelCommandHistory(std::size_t byteBudget)
    : impl_(std::make_unique<Impl>(byteBudget)) {}

ModelCommandHistory::~ModelCommandHistory() = default;
ModelCommandHistory::ModelCommandHistory(ModelCommandHistory&&) noexcept =
    default;
ModelCommandHistory& ModelCommandHistory::operator=(
    ModelCommandHistory&&) noexcept = default;

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
    auto delta = makeDocumentDelta(previous, current);
    if (delta.empty() && before.presentation == after.presentation) {
      result.status = HistoryCommitStatus::NoChange;
      result.state = std::move(before);
      return result;
    }
    const std::size_t retained =
        delta.retainedBytes + sizeof(Impl::Command) +
        editorStateOwnedBytes(before) + editorStateOwnedBytes(after);
    if (retained > impl_->byteBudget) {
      static_assert(std::is_nothrow_move_assignable_v<Document>);
      current = std::move(previous);
      result.status = HistoryCommitStatus::Rejected;
      result.state = std::move(before);
      result.error = "history byte budget exceeded";
      return result;
    }
    impl_->undo.reserve(impl_->undo.size() + 1);
    auto sharedDelta = std::make_shared<const DocumentDelta>(std::move(delta));
    result.state = after;
    const std::uint64_t beforeRevision = impl_->currentRevision;
    const std::uint64_t afterRevision = impl_->nextRevision++;
    Impl::Command command{std::move(sharedDelta), std::move(before),
                          std::move(after), retained, beforeRevision,
                          afterRevision};
    impl_->eraseRedo();
    impl_->undo.push_back(std::move(command));
    impl_->retained += retained;
    impl_->enforceBudget();
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

namespace {
class ApplyingGuard final {
 public:
  explicit ApplyingGuard(bool& value) : value_(value) { value_ = true; }
  ~ApplyingGuard() { value_ = false; }

 private:
  bool& value_;
};
}  // namespace

HistoryApplyResult ModelCommandHistory::undo(
    Document& current, const EditorCommittedState& currentState) noexcept {
  HistoryApplyResult result;
  try {
    result.state = currentState;
  } catch (...) {
    result.error = exceptionText("history undo state capture failed");
    return result;
  }
  if (impl_->undo.empty() || impl_->applying) return result;
  try {
    impl_->redo.reserve(impl_->redo.size() + 1);
    Document rollback = current;
    result.state = impl_->undo.back().before;
    ApplyingGuard guard(impl_->applying);
    try {
      applyDocumentDelta(current, *impl_->undo.back().delta, false);
    } catch (...) {
      current = std::move(rollback);
      result.state = currentState;
      result.error = exceptionText("history undo failed");
      return result;
    }
    impl_->redo.push_back(std::move(impl_->undo.back()));
    impl_->currentRevision = impl_->redo.back().beforeRevision;
    impl_->undo.pop_back();
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
  if (impl_->redo.empty() || impl_->applying) return result;
  try {
    impl_->undo.reserve(impl_->undo.size() + 1);
    Document rollback = current;
    result.state = impl_->redo.back().after;
    ApplyingGuard guard(impl_->applying);
    try {
      applyDocumentDelta(current, *impl_->redo.back().delta, true);
    } catch (...) {
      current = std::move(rollback);
      result.state = currentState;
      result.error = exceptionText("history redo failed");
      return result;
    }
    impl_->undo.push_back(std::move(impl_->redo.back()));
    impl_->currentRevision = impl_->undo.back().afterRevision;
    impl_->redo.pop_back();
    result.changed = true;
    return result;
  } catch (...) {
    result.state = {};
    result.error = exceptionText("history redo preparation failed");
    return result;
  }
}

void ModelCommandHistory::clear(bool currentIsSavepoint) noexcept {
  impl_->undo.clear();
  impl_->redo.clear();
  impl_->retained = 0;
  impl_->applying = false;
  impl_->currentRevision = impl_->nextRevision++;
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
  return !impl_->undo.empty();
}
bool ModelCommandHistory::canRedo() const noexcept {
  return !impl_->redo.empty();
}
bool ModelCommandHistory::isApplying() const noexcept {
  return impl_->applying;
}
std::size_t ModelCommandHistory::undoCount() const noexcept {
  return impl_->undo.size();
}
std::size_t ModelCommandHistory::redoCount() const noexcept {
  return impl_->redo.size();
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

DocumentDeltaMetrics ModelCommandHistory::inspectDelta(
    const Document& before, const Document& after) {
  const auto delta = makeDocumentDelta(before, after);
  return {delta.bodies.size() + delta.bodyStates.size() +
              delta.features.size() + delta.sketches.size() +
              delta.referenceImages.size(),
          delta.retainedBytes};
}

}  // namespace solidar
