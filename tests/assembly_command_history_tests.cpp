#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QJsonDocument>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ExtrudeFeature.h"
#include "model/OccurrenceDelta.h"
#include "model/OccurrenceDuplicate.h"
#include "model/TopologyReferenceResolver.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"
#include "ui/application/AssemblyCommandHistory.h"
#include "ui/application/internal/AssemblyCommandHistoryTestAccess.h"

using namespace solidar;

namespace {

std::unique_ptr<ProductDocument> emptyAssembly() {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(AssemblyDefinition(DefinitionId{1})) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{1});
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<Document> part(std::weak_ptr<const TopoDS_Shape>* weak = nullptr) {
  auto result = std::make_unique<Document>();
  auto& body = result->addBody(101, "body");
  auto shape = std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(8.0, 7.0, 6.0).Shape());
  if (weak) *weak = shape;
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      102, std::move(shape), "box"));
  CHECK(result->recompute());
  return result;
}

std::vector<std::byte> cold(const Document& document) {
  const auto encoded = project::detail::encodeProjectFormatV4DocumentRoot(
      document, {QStringLiteral("insert"), QString{}});
  CHECK(encoded.succeeded());
  const QByteArray bytes =
      QJsonDocument(*encoded.root).toJson(QJsonDocument::Compact);
  std::vector<std::byte> result(static_cast<std::size_t>(bytes.size()));
  if (!result.empty())
    std::memcpy(result.data(), bytes.constData(), result.size());
  return result;
}

OccurrencePath path(std::initializer_list<std::uint64_t> ids) {
  std::vector<OccurrenceId> entries;
  entries.reserve(ids.size());
  for (const auto id : ids) entries.emplace_back(id);
  auto made = OccurrencePath::create(std::move(entries));
  CHECK(made);
  return std::move(*made.path);
}

RootOccurrenceAddress address(const ProductDocument& product,
                              OccurrenceId occurrence,
                              DefinitionId target = DefinitionId{2}) {
  return {product.incarnation(), product.rootDefinitionId(),
          path({occurrence.value()}), target};
}

std::unique_ptr<ProductDocument> assemblyWithOccurrence() {
  auto product = emptyAssembly();
  CHECK(product->insert(
            PartDefinition(DefinitionId{2}, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  CHECK(product->insertRootOccurrence(ComponentOccurrence(
            OccurrenceId{3}, DefinitionId{2}, RigidTransform3d::identity())) ==
        ProductOccurrenceMutationStatus::Committed);
  return product;
}

LinkedPartBinding linkedBinding(std::uint8_t revision,
                                std::string locator = "parts/source.solidar") {
  ProjectId::Bytes projectBytes{};
  projectBytes[0] = 0x42;
  SourceContentHash hash;
  hash.bytes[0] = revision;
  return {{ProjectId{projectBytes}, DefinitionId{7}}, std::move(locator), hash};
}

std::unique_ptr<ProductDocument> linkedAssemblyWithOccurrences(
    std::unique_ptr<Document> document = {}) {
  if (!document) document = part();
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{2}, std::move(document), linkedBinding(1))) ==
        DefinitionInsertStatus::Inserted);
  AssemblyDefinition root(DefinitionId{1});
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{3}, DefinitionId{2}, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, false}, "first")) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{4}, DefinitionId{2}, RigidTransform3d::identity(),
            OccurrenceFlags{true, true, false}, "second")) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{1});
  CHECK(made);
  return std::move(made.product);
}

struct PersistentFaceFixture final {
  std::unique_ptr<Document> document;
  FaceReference reference;
  SketchId profileId{kInvalidSketchId};
  SketchId supportedId{kInvalidSketchId};
};

PersistentFaceFixture persistentFaceFixture(double profileWidth) {
  constexpr SketchId profileId{7101};
  constexpr BodyId bodyId{7102};
  constexpr FeatureId extrudeId{7103};
  constexpr SketchId supportedId{7104};
  auto document = std::make_unique<Document>();
  sketch::Sketch profileGeometry;
  profileGeometry.addRectangle({0.0, 0.0}, {profileWidth, 8.0});
  document->addSketch(profileId, "persistent profile",
                      std::move(profileGeometry));
  auto& body = document->addBody(bodyId, "persistent owner");
  body.addFeature(std::make_unique<ExtrudeFeature>(
      extrudeId, profileId, 5.0, "persistent source"));
  CHECK(document->recompute());
  std::optional<FaceReference> reference;
  for (std::size_t index = 0; index < 32; ++index) {
    auto created =
        document->createFaceReferenceV2(bodyId, extrudeId, index);
    if (created) {
      reference = std::move(created.reference);
      break;
    }
  }
  CHECK(reference && reference->isV2());
  document->addSketch(supportedId, "persistent consumer");
  CHECK(document->attachSketchToFace(supportedId, *reference));
  CHECK(document->recompute());
  return {std::move(document), std::move(*reference), profileId, supportedId};
}

struct MateHistoryFixture final {
  std::unique_ptr<ProductDocument> product;
  FaceReference face;
};

MateHistoryFixture mateHistoryFixture(bool threeNodeChain = false) {
  auto partFixture = persistentFaceFixture(10.0);
  FaceReference face = partFixture.reference;
  AssemblyDefinition root(DefinitionId{1});
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{3}, DefinitionId{2}, RigidTransform3d::identity(),
            {.visible = true, .suppressed = false, .grounded = true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const auto shifted = RigidTransform3d::create(
      threeNodeChain ? std::array<double, 3>{20.0, 30.0, 40.0}
                     : std::array<double, 3>{0.0, 0.0, 20.0},
      {1.0, 0.0, 0.0, 0.0});
  CHECK(shifted);
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{4}, DefinitionId{2},
                                        *shifted.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  if (threeNodeChain) {
    const auto farther = RigidTransform3d::create(
        {50.0, 60.0, 70.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(farther);
    CHECK(root.insert(ComponentOccurrence(OccurrenceId{5}, DefinitionId{2},
                                          *farther.transform)) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(root.mateRegistry().insert(PlanarCoincidentMate{
              MateId{1}, MateEndpoint{path({3}), face},
              MateEndpoint{path({4}), face}, PlanarMateOrientation::Aligned,
              0.0}) == MateRegistryMutationStatus::Inserted);
  }
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(DefinitionId{2}, std::move(partFixture.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{1});
  CHECK(made);
  return {std::move(made.product), std::move(face)};
}

template <class Callable>
class CallableSuccessor final : public ProductDocumentSuccessor {
 public:
  explicit CallableSuccessor(Callable& callable) noexcept
      : callable_(callable) {}

  ProductDocumentSuccessorStatus prepare(
      const ProductDocument& candidate,
      ProductDocumentSuccessorImpact) noexcept override {
    try {
      return callable_(candidate) ? ProductDocumentSuccessorStatus::Prepared
                                  : ProductDocumentSuccessorStatus::Rejected;
    } catch (const std::bad_alloc&) {
      return ProductDocumentSuccessorStatus::ResourceFailure;
    } catch (...) {
      return ProductDocumentSuccessorStatus::Rejected;
    }
  }

 private:
  Callable& callable_;
};

class ResourceFailureSuccessor final : public ProductDocumentSuccessor {
 public:
  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&,
      ProductDocumentSuccessorImpact) noexcept override {
    ++calls;
    return ProductDocumentSuccessorStatus::ResourceFailure;
  }

  int calls{};
};

class FixedStatusSuccessor final : public ProductDocumentSuccessor {
 public:
  explicit FixedStatusSuccessor(ProductDocumentSuccessorStatus status) noexcept
      : status_(status) {}

  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&,
      ProductDocumentSuccessorImpact impact) noexcept override {
    ++calls;
    lastImpact = impact;
    return status_;
  }

  int calls{};
  ProductDocumentSuccessorImpact lastImpact{
      ProductDocumentSuccessorImpact::FullProjection};

 private:
  ProductDocumentSuccessorStatus status_;
};

class ReentrantDefinitionMutationSuccessor final
    : public ProductDocumentSuccessor {
 public:
  ProductDocumentSuccessorStatus prepare(
      const ProductDocument& candidate,
      ProductDocumentSuccessorImpact impact) noexcept override {
    ++calls;
    lastImpact = impact;
    auto& mutableCandidate = const_cast<ProductDocument&>(candidate);
    try {
      auto orphan = std::make_unique<Document>();
      insertStatus = mutableCandidate.insert(
          PartDefinition(DefinitionId{99}, std::move(orphan)));

      const Document* current = candidate.partDocument(DefinitionId{2});
      if (current) {
        Document nestedCandidate = *current;
        nestedCandidate.setBox({77.0, 66.0, 55.0});
        FixedStatusSuccessor nestedSuccessor(
            ProductDocumentSuccessorStatus::Prepared);
        const DefinitionEditAddress nestedAddress{
            candidate.incarnation(), candidate.rootDefinitionId(),
            DefinitionId{2}, candidate.definitionStateId(DefinitionId{2})};
        nestedStatus = mutableCandidate
                           .applyDefinitionEdit(nestedAddress, nestedCandidate,
                                                nestedSuccessor)
                           .status;
        nestedSuccessorCalls = nestedSuccessor.calls;
      }
    } catch (...) {
      threw = true;
    }
    return ProductDocumentSuccessorStatus::Rejected;
  }

  int calls{};
  int nestedSuccessorCalls{};
  bool threw{};
  DefinitionInsertStatus insertStatus{DefinitionInsertStatus::Inserted};
  DefinitionEditStatus nestedStatus{DefinitionEditStatus::Committed};
  ProductDocumentSuccessorImpact lastImpact{
      ProductDocumentSuccessorImpact::FullProjection};
};

EmbeddedPartInsertColdCommand insertCommand(
    DefinitionId definition, OccurrenceId occurrence,
    std::vector<std::byte> bytes,
    AssemblyCommandSemanticState before = {},
    AssemblyCommandSemanticState after = {}) {
  return {definition, occurrence, RigidTransform3d::identity(), {},
          std::move(bytes), {}, before, after};
}

struct Snapshot {
  std::size_t historySize{};
  std::size_t cursor{};
  std::size_t retained{};
  AssemblyCommandRevision revision;
  AssemblyCommandRevision savepoint;
  bool atSavepoint{};
  bool dirty{};
  bool busy{};
  bool canUndo{};
  bool canRedo{};
  std::optional<OccurrencePath> semanticSelection;
  std::size_t definitions{};
  std::size_t occurrences{};

  friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

Snapshot snapshot(const AssemblyCommandHistory& history,
                  const ProductDocument& product) {
  return {history.size(),
          history.cursor(),
          history.retainedBytes(),
          history.currentRevision(),
          history.savepointRevision(),
          history.isAtSavepoint(),
          !history.isAtSavepoint(),
          history.isBusy(),
          history.canUndo(),
          history.canRedo(),
          history.currentOccurrence(),
          product.definitions().size(),
          product.rootAssemblyDefinition()
              ? product.rootAssemblyDefinition()->size()
              : 0};
}

struct OccurrenceSnapshot final {
  const ComponentOccurrence* address{};
  OccurrenceId id;
  DefinitionId target;
  RigidTransform3d placement{RigidTransform3d::identity()};
  OccurrenceFlags flags{};
  std::string displayNameUtf8;

  friend bool operator==(const OccurrenceSnapshot&,
                         const OccurrenceSnapshot&) = default;
};

struct ProductSnapshot final {
  // Capture every exposed identity/value plus exact node/document
  // addresses and a deterministic authored-document fingerprint.
  DefinitionId rootId;
  ProductIncarnationId incarnation;
  std::optional<ProductDocumentIdentity> identity;
  const AssemblyDefinition* rootAddress{};
  std::vector<DefinitionId> definitionIds;
  std::vector<DefinitionKind> definitionKinds;
  const PartDefinition* trackedDefinitionAddress{};
  const Document* trackedDocumentAddress{};
  DefinitionStateId trackedDefinitionState;
  std::vector<std::byte> trackedDocumentFingerprint;
  std::vector<const TopoDS_Shape*> trackedShapeAddresses;
  std::vector<ShapeRevision> trackedShapeRevisions;
  std::vector<OccurrenceSnapshot> occurrences;

  friend bool operator==(const ProductSnapshot&,
                         const ProductSnapshot&) = default;
};

ProductSnapshot productSnapshot(const ProductDocument& product,
                                DefinitionId trackedDefinition) {
  ProductSnapshot result;
  result.rootId = product.rootDefinitionId();
  result.incarnation = product.incarnation();
  result.identity = product.identity();
  result.rootAddress = product.rootAssemblyDefinition();
  result.definitionIds = product.definitions().definitionIds();
  result.definitionKinds.reserve(result.definitionIds.size());
  for (const auto id : result.definitionIds) {
    const auto kind = product.definitions().kind(id);
    CHECK(kind.has_value());
    result.definitionKinds.push_back(*kind);
  }
  const auto* tracked = product.definitions().findPart(trackedDefinition);
  if (tracked) {
    result.trackedDefinitionAddress = tracked;
    result.trackedDocumentAddress = &tracked->document();
    result.trackedDefinitionState = tracked->stateId();
    result.trackedDocumentFingerprint = cold(tracked->document());
    for (const auto& body : tracked->document().bodies()) {
      for (const auto& feature : body.features()) {
        result.trackedShapeAddresses.push_back(
            feature->lastValidShape().get());
        result.trackedShapeRevisions.push_back(feature->shapeRevision());
      }
    }
  }
  if (const auto* root = product.rootAssemblyDefinition()) {
    result.occurrences.reserve(root->occurrences().size());
    for (const auto& [id, occurrence] : root->occurrences()) {
      result.occurrences.push_back(
          {&occurrence, id, occurrence.targetDefinitionId(),
           occurrence.placement(), occurrence.flags(),
           occurrence.displayNameUtf8()});
    }
  }
  return result;
}

bool sameProductSemantics(ProductSnapshot left, ProductSnapshot right) {
  for (auto& occurrence : left.occurrences) occurrence.address = nullptr;
  for (auto& occurrence : right.occurrences) occurrence.address = nullptr;
  return left == right;
}

struct PayloadSnapshot final {
  DefinitionId definitionId;
  OccurrenceId occurrenceId;
  RigidTransform3d placement{RigidTransform3d::identity()};
  OccurrenceFlags flags{};
  std::string displayNameUtf8;
  const Document* documentAddress{};
  std::vector<std::byte> documentFingerprint;

  friend bool operator==(const PayloadSnapshot&,
                         const PayloadSnapshot&) = default;
};

PayloadSnapshot payloadSnapshot(const EmbeddedPartInsertPayload& payload) {
  return {payload.definitionId,
          payload.occurrenceId,
          payload.placement,
          payload.flags,
          payload.displayNameUtf8,
          payload.document.get(),
          payload.document ? cold(*payload.document) : std::vector<std::byte>{}};
}

DuplicateOccurrenceColdPayload duplicatePayload(
    const ProductDocument& product, OccurrenceId sourceId,
    OccurrenceId duplicateId) {
  const auto* occurrence = product.rootAssemblyDefinition()->find(sourceId);
  CHECK(occurrence);
  return {{product.incarnation(), product.rootDefinitionId(),
           path({sourceId.value()}), occurrence->targetDefinitionId()},
          duplicateId,
          occurrence->targetDefinitionId(),
          occurrence->placement(),
          occurrence->homePlacement(),
          occurrence->flags(),
          occurrence->displayNameUtf8()};
}

RemoveOccurrenceColdPayload removePayload(
    const ProductDocument& product, OccurrenceId occurrenceId) {
  const auto* occurrence =
      product.rootAssemblyDefinition()->find(occurrenceId);
  CHECK(occurrence);
  return {{product.incarnation(), product.rootDefinitionId(),
           path({occurrenceId.value()}), occurrence->targetDefinitionId()},
          occurrence->placement(), occurrence->flags(),
          occurrence->displayNameUtf8(), occurrence->homePlacement()};
}

}  // namespace

int main() {
  using ConstPartDocumentSignature =
      const Document* (ProductDocument::*)(DefinitionId) const noexcept;
  static_assert(std::is_same_v<decltype(&ProductDocument::partDocument),
                               ConstPartDocumentSignature>);
  static_assert(std::variant_size_v<AssemblyCommand> == 8);
  static_assert(std::is_same_v<std::variant_alternative_t<0, AssemblyCommand>,
                               EmbeddedPartInsertColdCommand>);
  static_assert(std::is_same_v<std::variant_alternative_t<1, AssemblyCommand>,
                               OccurrenceDeltaCommand>);
  static_assert(std::is_same_v<std::variant_alternative_t<2, AssemblyCommand>,
                               DuplicateOccurrenceColdCommand>);
  static_assert(std::is_same_v<std::variant_alternative_t<3, AssemblyCommand>,
                               RemoveOccurrenceColdCommand>);
  static_assert(std::is_same_v<std::variant_alternative_t<4, AssemblyCommand>,
                               DefinitionEditColdCommand>);
  static_assert(std::is_same_v<std::variant_alternative_t<5, AssemblyCommand>,
                               LinkedPartReloadColdCommand>);
  static_assert(std::is_same_v<std::variant_alternative_t<7, AssemblyCommand>,
                               PlanarMateColdCommand>);
  // A8a is one append-only cold command. Apply/undo/redo preserve the exact
  // authored registry revisions/fingerprint and all solved placements.
  {
    auto fixture = mateHistoryFixture();
    auto& product = *fixture.product;
    auto* root = product.rootAssemblyDefinition();
    CHECK(root);
    const MateRegistry beforeRegistry = root->mateRegistry();
    const auto beforeMoving = root->find(OccurrenceId{4})->placement();
    auto prepared = preparePlanarMateAuthoring(
        product, DefinitionId{1},
        PlanarCoincidentMate{
            *beforeRegistry.lowestFreeId(),
            MateEndpoint{path({3}), fixture.face},
            MateEndpoint{path({4}), fixture.face},
            PlanarMateOrientation::Aligned, 0.0});
    CHECK(prepared.transaction);
    const MateRegistry afterRegistry = prepared.transaction->afterRegistry();
    const auto afterMoving =
        prepared.transaction->candidate().changes().back().afterPlacement;
    AssemblyCommandHistory history;
    FixedStatusSuccessor successor(ProductDocumentSuccessorStatus::Prepared);
    PlanarMateColdCommand command{
        std::make_unique<PlanarMateAuthoringTransaction>(
            std::move(*prepared.transaction)),
        {}, {}};
    CHECK(history.executePlanarMate(product, std::move(command), successor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(successor.calls == 1);
    CHECK(successor.lastImpact ==
          ProductDocumentSuccessorImpact::MateProjection);
    CHECK(history.size() == 1);
    CHECK(history.cursor() == 1);
    CHECK(root->mateRegistry() == afterRegistry);
    CHECK(root->find(OccurrenceId{4})->placement() == afterMoving);

    FixedStatusSuccessor undoSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.undo(product, undoSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(root->mateRegistry() == beforeRegistry);
    CHECK(root->find(OccurrenceId{4})->placement() == beforeMoving);
    CHECK(history.cursor() == 0);

    FixedStatusSuccessor redoSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.redo(product, redoSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(root->mateRegistry() == afterRegistry);
    CHECK(root->find(OccurrenceId{4})->placement() == afterMoving);
    CHECK(history.cursor() == 1);
  }
  // A real three-node connected component is one cold history command. The
  // frozen placement oracle below is independent of undo/redo itself.
  {
    auto fixture = mateHistoryFixture(true);
    auto* root = fixture.product->rootAssemblyDefinition();
    CHECK(root && root->size() == 3 && root->mateRegistry().size() == 1);
    auto prepared = preparePlanarMateAuthoring(
        *fixture.product, DefinitionId{1},
        PlanarCoincidentMate{
            MateId{2}, MateEndpoint{path({4}), fixture.face},
            MateEndpoint{path({5}), fixture.face},
            PlanarMateOrientation::Aligned, 0.0});
    CHECK(prepared.transaction);
    CHECK(prepared.transaction->candidate().changes().size() == 3);
    const auto expected3 = RigidTransform3d::identity();
    const auto expected4 = RigidTransform3d::create(
        {20.0, 0.0, 40.0}, {1.0, 0.0, 0.0, 0.0});
    const auto expected5 = RigidTransform3d::create(
        {50.0, 0.0, 70.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(expected4 && expected5);
    const MateRegistry after = prepared.transaction->afterRegistry();
    const MateRegistry before = prepared.transaction->beforeRegistry();
    const auto before3 = root->find(OccurrenceId{3})->placement();
    const auto before4 = root->find(OccurrenceId{4})->placement();
    const auto before5 = root->find(OccurrenceId{5})->placement();
    AssemblyCommandHistory history;
    FixedStatusSuccessor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.executePlanarMate(
              *fixture.product,
              {std::make_unique<PlanarMateAuthoringTransaction>(
                   std::move(*prepared.transaction)),
               {}, {}},
              successor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(successor.calls == 1);
    CHECK(successor.lastImpact ==
          ProductDocumentSuccessorImpact::MateProjection);
    CHECK(root->mateRegistry() == after);
    CHECK(root->mateRegistry().revision() == after.revision());
    CHECK(root->mateRegistry().fingerprint() == after.fingerprint());
    CHECK(root->find(OccurrenceId{3})->placement() == expected3);
    CHECK(root->find(OccurrenceId{4})->placement() == *expected4.transform);
    CHECK(root->find(OccurrenceId{5})->placement() == *expected5.transform);
    CHECK(history.size() == 1 && history.cursor() == 1);
    history.markSavepoint();
    CHECK(history.isAtSavepoint());
    FixedStatusSuccessor undo(ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.undo(*fixture.product, undo).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(undo.calls == 1 && undo.lastImpact ==
                                ProductDocumentSuccessorImpact::MateProjection);
    CHECK(root->mateRegistry() == before);
    CHECK(root->mateRegistry().revision() == before.revision());
    CHECK(root->mateRegistry().fingerprint() == before.fingerprint());
    CHECK(root->find(OccurrenceId{3})->placement() == before3);
    CHECK(root->find(OccurrenceId{4})->placement() == before4);
    CHECK(root->find(OccurrenceId{5})->placement() == before5);
    CHECK(history.size() == 1 && history.cursor() == 0 && history.canRedo());
    CHECK(!history.isAtSavepoint());
    FixedStatusSuccessor redo(ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.redo(*fixture.product, redo).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(redo.calls == 1 && redo.lastImpact ==
                                ProductDocumentSuccessorImpact::MateProjection);
    CHECK(root->mateRegistry() == after);
    CHECK(root->mateRegistry().revision() == after.revision());
    CHECK(root->mateRegistry().fingerprint() == after.fingerprint());
    CHECK(root->find(OccurrenceId{3})->placement() == expected3);
    CHECK(root->find(OccurrenceId{4})->placement() == *expected4.transform);
    CHECK(root->find(OccurrenceId{5})->placement() == *expected5.transform);
    CHECK(history.size() == 1 && history.cursor() == 1 &&
          history.isAtSavepoint());
  }
  // Successor rejection/resource failure are exact pre-command rollbacks.
  for (const auto& failure : std::array{
           std::pair{ProductDocumentSuccessorStatus::Rejected,
                     AssemblyCommandHistoryStatus::SuccessorRejected},
           std::pair{ProductDocumentSuccessorStatus::ResourceFailure,
                     AssemblyCommandHistoryStatus::ResourceLimit}}) {
    auto fixture = mateHistoryFixture(true);
    auto* root = fixture.product->rootAssemblyDefinition();
    auto prepared = preparePlanarMateAuthoring(
        *fixture.product, DefinitionId{1},
        PlanarCoincidentMate{
            MateId{2}, MateEndpoint{path({4}), fixture.face},
            MateEndpoint{path({5}), fixture.face},
            PlanarMateOrientation::Aligned, 0.0});
    CHECK(prepared.transaction);
    const MateRegistry registryBefore = root->mateRegistry();
    const auto placement3 = root->find(OccurrenceId{3})->placement();
    const auto placement4 = root->find(OccurrenceId{4})->placement();
    const auto placement5 = root->find(OccurrenceId{5})->placement();
    AssemblyCommandHistory history;
    const auto historyBefore = snapshot(history, *fixture.product);
    FixedStatusSuccessor successor(failure.first);
    CHECK(history.executePlanarMate(
              *fixture.product,
              {std::make_unique<PlanarMateAuthoringTransaction>(
                   std::move(*prepared.transaction)),
               {}, {}},
              successor)
              .status == failure.second);
    CHECK(successor.calls == 1);
    CHECK(successor.lastImpact ==
          ProductDocumentSuccessorImpact::MateProjection);
    CHECK(root->mateRegistry() == registryBefore);
    CHECK(root->mateRegistry().revision() == registryBefore.revision());
    CHECK(root->mateRegistry().fingerprint() == registryBefore.fingerprint());
    CHECK(root->find(OccurrenceId{3})->placement() == placement3);
    CHECK(root->find(OccurrenceId{4})->placement() == placement4);
    CHECK(root->find(OccurrenceId{5})->placement() == placement5);
    CHECK(snapshot(history, *fixture.product) == historyBefore);
  }
  // A successor is allowed to probe reentry, but the ProductDocument guard
  // must report Busy and the outer command still publishes exactly once.
  {
    auto fixture = mateHistoryFixture(true);
    auto* root = fixture.product->rootAssemblyDefinition();
    auto prepared = preparePlanarMateAuthoring(
        *fixture.product, DefinitionId{1},
        PlanarCoincidentMate{
            MateId{2}, MateEndpoint{path({4}), fixture.face},
            MateEndpoint{path({5}), fixture.face},
            PlanarMateOrientation::Aligned, 0.0});
    CHECK(prepared.transaction);
    ProductOccurrenceMutationStatus reentrantStatus =
        ProductOccurrenceMutationStatus::Committed;
    int calls = 0;
    auto probe = [&](const ProductDocument&) {
      ++calls;
      reentrantStatus = fixture.product->setRootOccurrencePlacement(
          path({5}), RigidTransform3d::identity());
      return reentrantStatus == ProductOccurrenceMutationStatus::Busy;
    };
    CallableSuccessor successor(probe);
    AssemblyCommandHistory history;
    CHECK(history.executePlanarMate(
              *fixture.product,
              {std::make_unique<PlanarMateAuthoringTransaction>(
                   std::move(*prepared.transaction)),
               {}, {}},
              successor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(calls == 1);
    CHECK(reentrantStatus == ProductOccurrenceMutationStatus::Busy);
    CHECK(history.size() == 1 && history.cursor() == 1);
  }
  // The same three-node command proves stale Apply/Undo/Redo and redo-tail
  // atomicity. Every stale path fails before successor publication.
  {
    auto fixture = mateHistoryFixture(true);
    auto* root = fixture.product->rootAssemblyDefinition();
    const auto makePrepared = [&]() {
      return preparePlanarMateAuthoring(
          *fixture.product, DefinitionId{1},
          PlanarCoincidentMate{
              MateId{2}, MateEndpoint{path({4}), fixture.face},
              MateEndpoint{path({5}), fixture.face},
              PlanarMateOrientation::Aligned, 0.0});
    };
    auto staleApply = makePrepared();
    CHECK(staleApply.transaction);
    CHECK(fixture.product->setRootOccurrencePlacement(
              path({5}), RigidTransform3d::identity()) ==
          ProductOccurrenceMutationStatus::Committed);
    const MateRegistry staleApplyRegistry = root->mateRegistry();
    const auto staleApply3 = root->find(OccurrenceId{3})->placement();
    const auto staleApply4 = root->find(OccurrenceId{4})->placement();
    const auto staleApply5 = root->find(OccurrenceId{5})->placement();
    AssemblyCommandHistory staleHistory;
    const auto staleHistoryBefore = snapshot(staleHistory, *fixture.product);
    FixedStatusSuccessor staleApplySuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(staleHistory.executePlanarMate(
              *fixture.product,
              {std::make_unique<PlanarMateAuthoringTransaction>(
                   std::move(*staleApply.transaction)),
               {}, {}},
              staleApplySuccessor)
              .status == AssemblyCommandHistoryStatus::ModelRejected);
    CHECK(staleApplySuccessor.calls == 0);
    CHECK(root->mateRegistry() == staleApplyRegistry);
    CHECK(root->mateRegistry().revision() == staleApplyRegistry.revision());
    CHECK(root->mateRegistry().fingerprint() ==
          staleApplyRegistry.fingerprint());
    CHECK(root->find(OccurrenceId{3})->placement() == staleApply3);
    CHECK(root->find(OccurrenceId{4})->placement() == staleApply4);
    CHECK(root->find(OccurrenceId{5})->placement() == staleApply5);
    CHECK(snapshot(staleHistory, *fixture.product) == staleHistoryBefore);
  }
  {
    auto fixture = mateHistoryFixture(true);
    auto* root = fixture.product->rootAssemblyDefinition();
    const auto initial3 = RigidTransform3d::identity();
    const auto initial4 = RigidTransform3d::create(
        {20.0, 30.0, 40.0}, {1.0, 0.0, 0.0, 0.0});
    const auto initial5 = RigidTransform3d::create(
        {50.0, 60.0, 70.0}, {1.0, 0.0, 0.0, 0.0});
    const auto solved4 = RigidTransform3d::create(
        {20.0, 0.0, 40.0}, {1.0, 0.0, 0.0, 0.0});
    const auto solved5 = RigidTransform3d::create(
        {50.0, 0.0, 70.0}, {1.0, 0.0, 0.0, 0.0});
    const auto stale = RigidTransform3d::create(
        {91.0, 92.0, 93.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(initial4 && initial5 && solved4 && solved5 && stale);
    const auto prepare = [&]() {
      return preparePlanarMateAuthoring(
          *fixture.product, DefinitionId{1},
          PlanarCoincidentMate{
              MateId{2}, MateEndpoint{path({4}), fixture.face},
              MateEndpoint{path({5}), fixture.face},
              PlanarMateOrientation::Aligned, 0.0});
    };
    auto seed = prepare();
    CHECK(seed.transaction);
    const MateRegistry beforeRegistry = seed.transaction->beforeRegistry();
    const MateRegistry afterRegistry = seed.transaction->afterRegistry();
    AssemblyCommandHistory history;
    FixedStatusSuccessor apply(ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.executePlanarMate(
              *fixture.product,
              {std::make_unique<PlanarMateAuthoringTransaction>(
                   std::move(*seed.transaction)),
               {}, {}},
              apply)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(apply.calls == 1 && apply.lastImpact ==
                                ProductDocumentSuccessorImpact::MateProjection);
    history.markSavepoint();
    CHECK(root->mateRegistry() == afterRegistry);
    CHECK(root->mateRegistry().revision() == afterRegistry.revision());
    CHECK(root->mateRegistry().fingerprint() == afterRegistry.fingerprint());
    CHECK(root->find(OccurrenceId{3})->placement() == initial3);
    CHECK(root->find(OccurrenceId{4})->placement() == *solved4.transform);
    CHECK(root->find(OccurrenceId{5})->placement() == *solved5.transform);

    CHECK(fixture.product->setRootOccurrencePlacement(
              path({5}), *stale.transform) ==
          ProductOccurrenceMutationStatus::Committed);
    const auto staleUndoSnapshot = snapshot(history, *fixture.product);
    FixedStatusSuccessor staleUndo(ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.undo(*fixture.product, staleUndo).status ==
          AssemblyCommandHistoryStatus::ModelRejected);
    CHECK(staleUndo.calls == 0);
    CHECK(snapshot(history, *fixture.product) == staleUndoSnapshot);
    CHECK(root->mateRegistry() == afterRegistry);
    CHECK(root->find(OccurrenceId{5})->placement() == *stale.transform);

    CHECK(fixture.product->setRootOccurrencePlacement(
              path({5}), *solved5.transform) ==
          ProductOccurrenceMutationStatus::Committed);
    FixedStatusSuccessor undo(ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.undo(*fixture.product, undo).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(history.cursor() == 0 && history.size() == 1 && history.canRedo());
    CHECK(root->mateRegistry() == beforeRegistry);
    CHECK(root->mateRegistry().revision() == beforeRegistry.revision());
    CHECK(root->mateRegistry().fingerprint() == beforeRegistry.fingerprint());
    CHECK(root->find(OccurrenceId{3})->placement() == initial3);
    CHECK(root->find(OccurrenceId{4})->placement() == *initial4.transform);
    CHECK(root->find(OccurrenceId{5})->placement() == *initial5.transform);

    CHECK(fixture.product->setRootOccurrencePlacement(
              path({5}), *stale.transform) ==
          ProductOccurrenceMutationStatus::Committed);
    const auto staleRedoSnapshot = snapshot(history, *fixture.product);
    FixedStatusSuccessor staleRedo(ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.redo(*fixture.product, staleRedo).status ==
          AssemblyCommandHistoryStatus::ModelRejected);
    CHECK(staleRedo.calls == 0);
    CHECK(snapshot(history, *fixture.product) == staleRedoSnapshot);
    CHECK(root->mateRegistry() == beforeRegistry);
    CHECK(root->mateRegistry().revision() == beforeRegistry.revision());
    CHECK(root->mateRegistry().fingerprint() == beforeRegistry.fingerprint());
    CHECK(root->find(OccurrenceId{5})->placement() == *stale.transform);

    CHECK(fixture.product->setRootOccurrencePlacement(
              path({5}), *initial5.transform) ==
          ProductOccurrenceMutationStatus::Committed);
    auto failedNew = prepare();
    CHECK(failedNew.transaction);
    const auto tailBefore = snapshot(history, *fixture.product);
    FixedStatusSuccessor rejected(ProductDocumentSuccessorStatus::Rejected);
    CHECK(history.executePlanarMate(
              *fixture.product,
              {std::make_unique<PlanarMateAuthoringTransaction>(
                   std::move(*failedNew.transaction)),
               {}, {}},
              rejected)
              .status == AssemblyCommandHistoryStatus::SuccessorRejected);
    CHECK(rejected.calls == 1 && rejected.lastImpact ==
                                   ProductDocumentSuccessorImpact::MateProjection);
    CHECK(snapshot(history, *fixture.product) == tailBefore);
    CHECK(history.canRedo());
    CHECK(root->mateRegistry() == beforeRegistry);
    CHECK(root->mateRegistry().revision() == beforeRegistry.revision());
    CHECK(root->mateRegistry().fingerprint() == beforeRegistry.fingerprint());
    CHECK(root->find(OccurrenceId{3})->placement() == initial3);
    CHECK(root->find(OccurrenceId{4})->placement() == *initial4.transform);
    CHECK(root->find(OccurrenceId{5})->placement() == *initial5.transform);

    auto replacement = prepare();
    CHECK(replacement.transaction);
    FixedStatusSuccessor replacementSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.executePlanarMate(
              *fixture.product,
              {std::make_unique<PlanarMateAuthoringTransaction>(
                   std::move(*replacement.transaction)),
               {}, {}},
              replacementSuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(replacementSuccessor.calls == 1);
    CHECK(history.size() == 1 && history.cursor() == 1 &&
          !history.canRedo());
    CHECK(root->mateRegistry() == afterRegistry);
    CHECK(root->mateRegistry().revision() == afterRegistry.revision());
    CHECK(root->mateRegistry().fingerprint() == afterRegistry.fingerprint());
    CHECK(root->find(OccurrenceId{3})->placement() == initial3);
    CHECK(root->find(OccurrenceId{4})->placement() == *solved4.transform);
    CHECK(root->find(OccurrenceId{5})->placement() == *solved5.transform);
  }
  // The mate command owns a heap transaction, two complete registry maps and
  // all candidate dependency storage. Its checked retained-byte accounting
  // accepts exactly 64 MiB and rejects limit+1 before model/successor/history.
  {
    // Independent lower bound: every std::map owns a sentinel plus one fully
    // aligned node per relation. Exercise mixed datum-only and
    // provenance-owning reference families without relying on string
    // over-counting to cover node storage.
    {
      auto fixture = mateHistoryFixture();
      MateRegistry mixed;
      CHECK(mixed.insert(AxisConcentricMate{
                MateId{1}, {path({3}), LocalDatumAxis::X},
                {path({4}), LocalDatumAxis::Y}}) ==
            MateRegistryMutationStatus::Inserted);
      CHECK(mixed.insert(PointDistanceMate{
                MateId{2}, {path({3}), LocalDatumPoint::Origin},
                {path({4}), LocalDatumPoint::Origin}, 7.0}) ==
            MateRegistryMutationStatus::Inserted);
      CHECK(mixed.insert(PlanarParallelMate{
                MateId{3}, {path({3}), fixture.face},
                {path({4}), fixture.face}, PlanarMateOrientation::Aligned}) ==
            MateRegistryMutationStatus::Inserted);
      const auto bytes =
          detail::AssemblyCommandHistoryTestAccess::mateRegistryDynamicBytes(
              mixed);
      constexpr std::size_t kIndependentNodeBytes =
          sizeof(std::pair<const MateId, AssemblyMate>) + 5 * sizeof(void*);
      CHECK(bytes);
      CHECK(*bytes >= (mixed.size() + 1) * kIndependentNodeBytes);
      CHECK(*bytes > (mixed.size() + 1) * kIndependentNodeBytes);
    }
    const auto makeCommand = [](MateHistoryFixture& fixture) {
      auto* root = fixture.product->rootAssemblyDefinition();
      CHECK(root);
      auto prepared = preparePlanarMateAuthoring(
          *fixture.product, DefinitionId{1},
          PlanarCoincidentMate{
              *root->mateRegistry().lowestFreeId(),
              MateEndpoint{path({3}), fixture.face},
              MateEndpoint{path({4}), fixture.face},
              PlanarMateOrientation::Aligned, 0.0});
      CHECK(prepared.transaction);
      return PlanarMateColdCommand{
          std::make_unique<PlanarMateAuthoringTransaction>(
              std::move(*prepared.transaction)),
          {}, {}};
    };
    {
      auto fixture = mateHistoryFixture();
      auto command = makeCommand(fixture);
      AssemblyCommand measured(std::move(command));
      const auto bytes =
          detail::AssemblyCommandHistoryTestAccess::dynamicBytes(measured);
      CHECK(bytes && *bytes > sizeof(PlanarMateAuthoringTransaction) &&
            *bytes < AssemblyCommandHistory::maximumRetainedBytes());
      command = std::move(std::get<PlanarMateColdCommand>(measured));
      AssemblyCommandHistory history;
      std::vector<std::byte> filler(
          AssemblyCommandHistory::maximumRetainedBytes() - *bytes);
      CHECK(history.recordApplied(insertCommand(
                DefinitionId{9201}, OccurrenceId{9202}, std::move(filler))) ==
            AssemblyCommandHistoryStatus::Committed);
      FixedStatusSuccessor successor(ProductDocumentSuccessorStatus::Prepared);
      CHECK(history.executePlanarMate(*fixture.product, std::move(command),
                                      successor)
                .status == AssemblyCommandHistoryStatus::Committed);
      CHECK(history.retainedBytes() ==
            AssemblyCommandHistory::maximumRetainedBytes());
    }
    {
      auto fixture = mateHistoryFixture();
      auto command = makeCommand(fixture);
      AssemblyCommand measured(std::move(command));
      const auto bytes =
          detail::AssemblyCommandHistoryTestAccess::dynamicBytes(measured);
      CHECK(bytes && *bytes > 1 &&
            *bytes < AssemblyCommandHistory::maximumRetainedBytes());
      command = std::move(std::get<PlanarMateColdCommand>(measured));
      AssemblyCommandHistory history;
      std::vector<std::byte> filler(
          AssemblyCommandHistory::maximumRetainedBytes() - *bytes + 1);
      CHECK(history.recordApplied(insertCommand(
                DefinitionId{9211}, OccurrenceId{9212}, std::move(filler))) ==
            AssemblyCommandHistoryStatus::Committed);
      const auto before = snapshot(history, *fixture.product);
      const MateRegistry registryBefore =
          fixture.product->rootAssemblyDefinition()->mateRegistry();
      const auto placementBefore = fixture.product->rootAssemblyDefinition()
                                       ->find(OccurrenceId{4})
                                       ->placement();
      FixedStatusSuccessor successor(ProductDocumentSuccessorStatus::Prepared);
      CHECK(history.executePlanarMate(*fixture.product, std::move(command),
                                      successor)
                .status == AssemblyCommandHistoryStatus::ResourceLimit);
      CHECK(successor.calls == 0);
      CHECK(snapshot(history, *fixture.product) == before);
      CHECK(fixture.product->rootAssemblyDefinition()->mateRegistry() ==
            registryBefore);
      CHECK(fixture.product->rootAssemblyDefinition()
                ->find(OccurrenceId{4})
                ->placement() == placementBefore);
    }
  }
  static_assert(std::is_same_v<std::variant_alternative_t<6, AssemblyCommand>,
                               LinkedPartRepairColdCommand>);
  static_assert(!std::is_constructible_v<EmbeddedPartInsertColdCommand,
                                         std::unique_ptr<Document>>);
  static_assert(!std::is_constructible_v<OccurrenceDeltaCommand, Document>);
  static_assert(!std::is_constructible_v<DuplicateOccurrenceColdCommand,
                                         Document>);
  static_assert(!std::is_constructible_v<RemoveOccurrenceColdCommand,
                                         Document>);

  // The address represents a full stable path, while the flat A5c0 boundary
  // validates and rejects depth other than one.
  {
    auto product = assemblyWithOccurrence();
    const auto empty = OccurrenceDelta::empty(address(*product, OccurrenceId{3}));
    const auto beforeDefinitions = product->definitions().size();
    const auto beforeOccurrences = product->rootAssemblyDefinition()->size();
    CHECK(product->applyOccurrenceDelta(empty) ==
          OccurrenceDeltaStatus::NoChange);
    CHECK(product->revertOccurrenceDelta(empty) ==
          OccurrenceDeltaStatus::NoChange);
    CHECK(product->definitions().size() == beforeDefinitions);
    CHECK(product->rootAssemblyDefinition()->size() == beforeOccurrences);

    AssemblyCommandHistory history;
    const auto before = snapshot(history, *product);
    auto accept = [](const ProductDocument&) { return true; };
    CallableSuccessor acceptEmpty(accept);
    const auto noChange =
        history.executeOccurrenceDelta(*product, {empty}, acceptEmpty);
    CHECK(noChange.status == AssemblyCommandHistoryStatus::NoChange);
    CHECK(noChange.deltaStatus == OccurrenceDeltaStatus::NoChange);
    CHECK(snapshot(history, *product) == before);

    auto staleIncarnation = address(*product, OccurrenceId{3});
    staleIncarnation.productIncarnation = ProductIncarnationId{999999};
    const auto staleDelta =
        OccurrenceDelta::empty(std::move(staleIncarnation));
    CHECK(product->applyOccurrenceDelta(staleDelta) ==
          OccurrenceDeltaStatus::ProductIncarnationMismatch);
    CHECK(product->revertOccurrenceDelta(staleDelta) ==
          OccurrenceDeltaStatus::ProductIncarnationMismatch);

    auto wrongRoot = address(*product, OccurrenceId{3});
    wrongRoot.rootDefinitionId = DefinitionId{99};
    const auto wrongRootDelta = OccurrenceDelta::empty(std::move(wrongRoot));
    CHECK(product->applyOccurrenceDelta(wrongRootDelta) ==
          OccurrenceDeltaStatus::RootDefinitionMismatch);
    CHECK(product->revertOccurrenceDelta(wrongRootDelta) ==
          OccurrenceDeltaStatus::RootDefinitionMismatch);

    auto missing = address(*product, OccurrenceId{99});
    const auto missingDelta = OccurrenceDelta::empty(std::move(missing));
    CHECK(product->applyOccurrenceDelta(missingDelta) ==
          OccurrenceDeltaStatus::OccurrenceMissing);
    CHECK(product->revertOccurrenceDelta(missingDelta) ==
          OccurrenceDeltaStatus::OccurrenceMissing);

    auto wrongTarget = address(*product, OccurrenceId{3}, DefinitionId{99});
    const auto wrongTargetDelta =
        OccurrenceDelta::empty(std::move(wrongTarget));
    CHECK(product->applyOccurrenceDelta(wrongTargetDelta) ==
          OccurrenceDeltaStatus::TargetDefinitionMismatch);
    CHECK(product->revertOccurrenceDelta(wrongTargetDelta) ==
          OccurrenceDeltaStatus::TargetDefinitionMismatch);

    RootOccurrenceAddress nested{product->incarnation(),
                                 product->rootDefinitionId(), path({3, 4}),
                                 DefinitionId{2}};
    const auto nestedDelta = OccurrenceDelta::empty(std::move(nested));
    CHECK(product->applyOccurrenceDelta(nestedDelta) ==
          OccurrenceDeltaStatus::UnsupportedPathDepth);
    CHECK(product->revertOccurrenceDelta(nestedDelta) ==
          OccurrenceDeltaStatus::UnsupportedPathDepth);

    DefinitionRegistry partDefinitions;
    CHECK(partDefinitions.insert(
              PartDefinition(DefinitionId{1}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
    auto partProduct = ProductDocument::create(std::move(partDefinitions),
                                               DefinitionId{1});
    CHECK(partProduct);
    RootOccurrenceAddress partAddress{partProduct.product->incarnation(),
                                      DefinitionId{1}, path({3}),
                                      DefinitionId{2}};
    CHECK(partProduct.product->applyOccurrenceDelta(
              OccurrenceDelta::empty(std::move(partAddress))) ==
          OccurrenceDeltaStatus::RootIsNotAssembly);

    auto other = assemblyWithOccurrence();
    CHECK(other->applyOccurrenceDelta(empty) ==
          OccurrenceDeltaStatus::ProductIncarnationMismatch);
  }

  // A typed resource result crosses the noexcept A5b Apply boundary without a
  // temporary callback allocation. The model rolls back both nodes and
  // returns the exact authored owner to the caller.
  {
    auto resourceProduct = emptyAssembly();
    EmbeddedPartInsertPayload resourcePayload{
        DefinitionId{2}, OccurrenceId{3}, RigidTransform3d::identity(), {},
        part()};
    AssemblyCommandHistory resourceHistory;
    const auto beforeProduct =
        productSnapshot(*resourceProduct, resourcePayload.definitionId);
    const auto beforePayload = payloadSnapshot(resourcePayload);
    const auto beforeHistory = snapshot(resourceHistory, *resourceProduct);
    CHECK(resourceHistory.prepareRecord(insertCommand(
              resourcePayload.definitionId, resourcePayload.occurrenceId,
              beforePayload.documentFingerprint, {},
              {path({resourcePayload.occurrenceId.value()})})) ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(resourceHistory.isBusy());
    ResourceFailureSuccessor resourceFailure;
    static_assert(noexcept(resourceProduct->applyEmbeddedPartInsert(
        resourcePayload, resourceFailure)));
    CHECK(resourceProduct->applyEmbeddedPartInsert(resourcePayload,
                                                    resourceFailure) ==
          EmbeddedPartInsertStatus::SuccessorResourceFailure);
    resourceHistory.discardPrepared();
    CHECK(resourceFailure.calls == 1);
    CHECK(payloadSnapshot(resourcePayload) == beforePayload);
    CHECK(productSnapshot(*resourceProduct, resourcePayload.definitionId) ==
          beforeProduct);
    CHECK(snapshot(resourceHistory, *resourceProduct) == beforeHistory);
  }

  // The maximum valid authored name must be copied before the noexcept
  // Undo/Redo model boundary. Injected allocation failure at payload
  // preparation is an exact no-op and never reaches successor preparation.
  {
    auto namedProduct = emptyAssembly();
    auto namedDocument = part();
    const auto namedBytes = cold(*namedDocument);
    const std::string longName(16 * 1024, 'n');
    EmbeddedPartInsertPayload namedPayload{
        DefinitionId{20}, OccurrenceId{30}, RigidTransform3d::identity(), {},
        std::move(namedDocument), longName};
    AssemblyCommandHistory namedHistory;
    CHECK(namedHistory.prepareRecord(EmbeddedPartInsertColdCommand{
              DefinitionId{20}, OccurrenceId{30},
              RigidTransform3d::identity(), {}, namedBytes, longName, {},
              {path({30})}}) == AssemblyCommandHistoryStatus::Committed);
    FixedStatusSuccessor namedSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(namedProduct->applyEmbeddedPartInsert(namedPayload,
                                                namedSuccessor) ==
          EmbeddedPartInsertStatus::Committed);
    CHECK(namedHistory.commitPrepared() ==
          AssemblyCommandHistoryStatus::Committed);

    const auto undoProductBefore =
        productSnapshot(*namedProduct, DefinitionId{20});
    const auto undoHistoryBefore = snapshot(namedHistory, *namedProduct);
    FixedStatusSuccessor undoSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    detail::AssemblyCommandHistoryTestAccess::inject(
        namedHistory,
        detail::AssemblyCommandHistoryFailurePoint::PayloadPreparation,
        detail::AssemblyCommandHistoryFailureKind::BadAlloc);
    CHECK(namedHistory.undo(*namedProduct, undoSuccessor).status ==
          AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(undoSuccessor.calls == 0);
    CHECK(productSnapshot(*namedProduct, DefinitionId{20}) ==
          undoProductBefore);
    CHECK(snapshot(namedHistory, *namedProduct) == undoHistoryBefore);
    detail::AssemblyCommandHistoryTestAccess::clearInjection(namedHistory);

    CHECK(namedHistory.undo(*namedProduct, namedSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    const auto redoProductBefore =
        productSnapshot(*namedProduct, DefinitionId{20});
    const auto redoHistoryBefore = snapshot(namedHistory, *namedProduct);
    FixedStatusSuccessor redoSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    detail::AssemblyCommandHistoryTestAccess::inject(
        namedHistory,
        detail::AssemblyCommandHistoryFailurePoint::PayloadPreparation,
        detail::AssemblyCommandHistoryFailureKind::BadAlloc);
    CHECK(namedHistory.redo(*namedProduct, redoSuccessor).status ==
          AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(redoSuccessor.calls == 0);
    CHECK(productSnapshot(*namedProduct, DefinitionId{20}) ==
          redoProductBefore);
    CHECK(snapshot(namedHistory, *namedProduct) == redoHistoryBefore);
    detail::AssemblyCommandHistoryTestAccess::clearInjection(namedHistory);
    CHECK(namedHistory.redo(*namedProduct, namedSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(namedProduct->rootAssemblyDefinition()
              ->find(OccurrenceId{30})
              ->displayNameUtf8() == longName);
  }

  // Existing A5b insert is the other alternative in the one history. Runtime
  // geometry expires on Undo; Redo reconstructs solely from cold bytes.
  auto product = emptyAssembly();
  std::weak_ptr<const TopoDS_Shape> runtimeShape;
  auto document = part(&runtimeShape);
  const auto authored = cold(*document);
  EmbeddedPartInsertPayload payload{DefinitionId{2}, OccurrenceId{3},
                                    RigidTransform3d::identity(), {},
                                    std::move(document)};
  AssemblyCommandHistory history;
  const auto baselineRevision = history.currentRevision();
  CHECK(history.isAtSavepoint());
  CHECK(history.prepareRecord(insertCommand(
            DefinitionId{2}, OccurrenceId{3}, authored, {},
            {path({3})})) ==
        AssemblyCommandHistoryStatus::Committed);
  CHECK(history.isBusy());
  CHECK(product->applyEmbeddedPartInsert(payload) ==
        EmbeddedPartInsertStatus::Committed);
  CHECK(history.commitPrepared() == AssemblyCommandHistoryStatus::Committed);
  CHECK(!history.isBusy());
  CHECK(history.currentRevision() != baselineRevision);
  CHECK(!history.isAtSavepoint());
  CHECK(history.retainedBytes() == authored.size() + sizeof(OccurrenceId));

  auto rejectSuccessor = [](const ProductDocument&) { return false; };
  CallableSuccessor rejectPreparation(rejectSuccessor);
  const auto beforeRejectedUndo = snapshot(history, *product);
  CHECK(history.undo(*product, rejectPreparation).status ==
        AssemblyCommandHistoryStatus::SuccessorRejected);
  CHECK(snapshot(history, *product) == beforeRejectedUndo);

  // The typed reference is fully constructed before the noexcept model call:
  // a deterministic resource report cannot allocate/throw at the call site
  // and the paired mutation rolls back exactly. currentOccurrence() in the
  // history snapshot is this model-layer test's semantic-selection proxy;
  // publication/epoch rollback is covered by project_switch_ui_tests.
  ResourceFailureSuccessor resourceFailure;
  static_assert(noexcept(product->extractEmbeddedPartInsert(
      payload, resourceFailure)));
  const auto exactProductBeforeResourceUndo =
      productSnapshot(*product, DefinitionId{2});
  const auto exactPayloadBeforeResourceUndo = payloadSnapshot(payload);
  CHECK(history.undo(*product, resourceFailure).status ==
        AssemblyCommandHistoryStatus::ResourceLimit);
  CHECK(resourceFailure.calls == 1);
  CHECK(snapshot(history, *product) == beforeRejectedUndo);
  CHECK(productSnapshot(*product, DefinitionId{2}) ==
        exactProductBeforeResourceUndo);
  CHECK(payloadSnapshot(payload) == exactPayloadBeforeResourceUndo);

  auto acceptSuccessor = [](const ProductDocument&) { return true; };
  CallableSuccessor acceptPreparation(acceptSuccessor);
  auto result = history.undo(*product, acceptPreparation);
  CHECK(result.status ==
        AssemblyCommandHistoryStatus::Committed);
  CHECK(result.restoredState == AssemblyCommandSemanticState{});
  CHECK(history.isAtSavepoint());
  CHECK(runtimeShape.expired());
  CHECK(history.canRedo());
  result = history.redo(*product, acceptPreparation);
  CHECK(result.status ==
        AssemblyCommandHistoryStatus::Committed);
  CHECK(result.restoredState ==
        (AssemblyCommandSemanticState{path({3})}));
  CHECK(!history.isAtSavepoint());
  CHECK(product->resolveRootOccurrence(path({3})));
  CHECK(history.currentOccurrence() == path({3}));

  history.markSavepoint();
  CHECK(history.isAtSavepoint());
  CHECK(history.undo(*product, acceptPreparation).status ==
        AssemblyCommandHistoryStatus::Committed);
  CHECK(!history.isAtSavepoint());
  CHECK(history.redo(*product, acceptPreparation).status ==
        AssemblyCommandHistoryStatus::Committed);
  CHECK(history.isAtSavepoint());

  // A valid empty command after Undo is an exact no-op and retains the redo
  // tail, revision, savepoint and retained-byte accounting.
  CHECK(history.undo(*product, acceptPreparation).status ==
        AssemblyCommandHistoryStatus::Committed);
  CHECK(product->insert(
            PartDefinition(DefinitionId{8}, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  CHECK(product->insertRootOccurrence(ComponentOccurrence(
            OccurrenceId{9}, DefinitionId{8}, RigidTransform3d::identity())) ==
        ProductOccurrenceMutationStatus::Committed);
  const auto noOpBefore = snapshot(history, *product);
  const auto validNoOp = OccurrenceDelta::empty(
      address(*product, OccurrenceId{9}, DefinitionId{8}));
  CHECK(history.executeOccurrenceDelta(*product, {validNoOp},
                                       acceptPreparation).status ==
        AssemblyCommandHistoryStatus::NoChange);
  CHECK(snapshot(history, *product) == noOpBefore);

  for (const auto kind :
       {detail::AssemblyCommandHistoryFailureKind::BadAlloc,
        detail::AssemblyCommandHistoryFailureKind::Runtime,
        detail::AssemblyCommandHistoryFailureKind::Unknown}) {
    detail::AssemblyCommandHistoryTestAccess::inject(
        history, detail::AssemblyCommandHistoryFailurePoint::BeforeOperation,
        kind);
    const auto failed = history.executeOccurrenceDelta(
        *product, {validNoOp}, acceptPreparation);
    CHECK(failed.status ==
          (kind == detail::AssemblyCommandHistoryFailureKind::BadAlloc
               ? AssemblyCommandHistoryStatus::ResourceLimit
               : AssemblyCommandHistoryStatus::ModelRejected));
    CHECK(!failed.deltaStatus.has_value());
    CHECK(snapshot(history, *product) == noOpBefore);
  }
  detail::AssemblyCommandHistoryTestAccess::clearInjection(history);

  // Busy fails closed under a successor callback and finalization failures
  // use the paired model adapter's allocation-free exact rollback.
  CHECK(history.redo(*product, acceptPreparation).status ==
        AssemblyCommandHistoryStatus::Committed);
  const auto beforeReentrant = snapshot(history, *product);
  bool nestedBusy = false;
  auto reentrant = [&](const ProductDocument&) {
    nestedBusy = history.executeOccurrenceDelta(
                     *product, {validNoOp}, acceptPreparation).status ==
                 AssemblyCommandHistoryStatus::Busy;
    return false;
  };
  CallableSuccessor reentrantPreparation(reentrant);
  CHECK(history.undo(*product, reentrantPreparation).status ==
        AssemblyCommandHistoryStatus::SuccessorRejected);
  CHECK(nestedBusy);
  CHECK(snapshot(history, *product) == beforeReentrant);

  for (const auto kind :
       {detail::AssemblyCommandHistoryFailureKind::BadAlloc,
        detail::AssemblyCommandHistoryFailureKind::Runtime,
        detail::AssemblyCommandHistoryFailureKind::Unknown}) {
    detail::AssemblyCommandHistoryTestAccess::inject(
        history, detail::AssemblyCommandHistoryFailurePoint::Finalization,
        kind);
    const auto beforeFailure = snapshot(history, *product);
    const auto expected =
        kind == detail::AssemblyCommandHistoryFailureKind::BadAlloc
            ? AssemblyCommandHistoryStatus::ResourceLimit
            : AssemblyCommandHistoryStatus::SuccessorRejected;
    CHECK(history.undo(*product, acceptPreparation).status == expected);
    CHECK(snapshot(history, *product) == beforeFailure);
    CHECK(product->resolveRootOccurrence(path({3})));
  }
  detail::AssemblyCommandHistoryTestAccess::clearInjection(history);

  const auto revisionBeforeClear = history.currentRevision();
  history.clear();
  CHECK(history.currentRevision().isValid());
  CHECK(history.currentRevision() != revisionBeforeClear);
  CHECK(history.isAtSavepoint());
  CHECK(history.size() == 0 && history.cursor() == 0 &&
        history.retainedBytes() == 0);

  // Vector-backed semantic paths are copied before every live-model or
  // cursor mutation. A deterministic allocation failure at that boundary is
  // therefore an exact no-op and never reaches successor preparation.
  {
    auto semanticProduct = assemblyWithOccurrence();
    AssemblyCommandHistory semanticHistory;
    const auto makeCommand = [&] {
      return OccurrenceDeltaCommand{
          OccurrenceDelta::visibility(
              address(*semanticProduct, OccurrenceId{3}, DefinitionId{2}),
              true, false),
          {path({1, 7})},
          {path({2, 7})}};
    };

    const auto executeProductBefore =
        productSnapshot(*semanticProduct, DefinitionId{2});
    const auto executeHistoryBefore =
        snapshot(semanticHistory, *semanticProduct);
    FixedStatusSuccessor executeSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    detail::AssemblyCommandHistoryTestAccess::inject(
        semanticHistory,
        detail::AssemblyCommandHistoryFailurePoint::SemanticStatePreparation,
        detail::AssemblyCommandHistoryFailureKind::BadAlloc);
    CHECK(semanticHistory
              .executeOccurrenceDelta(*semanticProduct, makeCommand(),
                                      executeSuccessor)
              .status == AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(executeSuccessor.calls == 0);
    CHECK(productSnapshot(*semanticProduct, DefinitionId{2}) ==
          executeProductBefore);
    CHECK(snapshot(semanticHistory, *semanticProduct) ==
          executeHistoryBefore);
    detail::AssemblyCommandHistoryTestAccess::clearInjection(semanticHistory);

    FixedStatusSuccessor seedSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(semanticHistory
              .executeOccurrenceDelta(*semanticProduct, makeCommand(),
                                      seedSuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    semanticHistory.markSavepoint();

    const auto undoProductBefore =
        productSnapshot(*semanticProduct, DefinitionId{2});
    const auto undoHistoryBefore = snapshot(semanticHistory, *semanticProduct);
    FixedStatusSuccessor undoSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    detail::AssemblyCommandHistoryTestAccess::inject(
        semanticHistory,
        detail::AssemblyCommandHistoryFailurePoint::SemanticStatePreparation,
        detail::AssemblyCommandHistoryFailureKind::BadAlloc);
    CHECK(semanticHistory.undo(*semanticProduct, undoSuccessor).status ==
          AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(undoSuccessor.calls == 0);
    CHECK(productSnapshot(*semanticProduct, DefinitionId{2}) ==
          undoProductBefore);
    CHECK(snapshot(semanticHistory, *semanticProduct) == undoHistoryBefore);
    detail::AssemblyCommandHistoryTestAccess::clearInjection(semanticHistory);

    CHECK(semanticHistory.undo(*semanticProduct, seedSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    semanticHistory.markSavepoint();
    const auto redoProductBefore =
        productSnapshot(*semanticProduct, DefinitionId{2});
    const auto redoHistoryBefore = snapshot(semanticHistory, *semanticProduct);
    FixedStatusSuccessor redoSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    detail::AssemblyCommandHistoryTestAccess::inject(
        semanticHistory,
        detail::AssemblyCommandHistoryFailurePoint::SemanticStatePreparation,
        detail::AssemblyCommandHistoryFailureKind::BadAlloc);
    CHECK(semanticHistory.redo(*semanticProduct, redoSuccessor).status ==
          AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(redoSuccessor.calls == 0);
    CHECK(productSnapshot(*semanticProduct, DefinitionId{2}) ==
          redoProductBefore);
    CHECK(snapshot(semanticHistory, *semanticProduct) == redoHistoryBefore);
    detail::AssemblyCommandHistoryTestAccess::clearInjection(semanticHistory);
  }

  // The private two-step path proves forward apply, reverse rollback and a
  // single closed-variant history entry without exposing A5c1 fields.
  {
    auto syntheticProduct = assemblyWithOccurrence();
    const RootOccurrenceAddress syntheticAddress =
        address(*syntheticProduct, OccurrenceId{3});
    auto syntheticCommand = [&] {
      return OccurrenceDeltaCommand{
          detail::AssemblyCommandHistoryTestAccess::syntheticDelta(
              syntheticAddress)};
    };

    for (const auto point :
         {detail::AssemblyCommandHistoryFailurePoint::Allocation,
          detail::AssemblyCommandHistoryFailurePoint::Reserve,
          detail::AssemblyCommandHistoryFailurePoint::AfterFirstSyntheticOperation,
          detail::AssemblyCommandHistoryFailurePoint::Finalization}) {
      for (const auto kind :
           {detail::AssemblyCommandHistoryFailureKind::BadAlloc,
            detail::AssemblyCommandHistoryFailureKind::Runtime,
            detail::AssemblyCommandHistoryFailureKind::Unknown}) {
        AssemblyCommandHistory syntheticHistory;
        detail::AssemblyCommandHistoryTestAccess::inject(
            syntheticHistory, point, kind);
        int first = 0;
        int second = 0;
        std::array<int, 4> trace{};
        std::size_t traceSize = 0;
        const auto before = snapshot(syntheticHistory, *syntheticProduct);
        const auto status =
            detail::AssemblyCommandHistoryTestAccess::executeSyntheticTwoStep(
                syntheticHistory, syntheticCommand(), first, second, trace,
                traceSize);
        const auto expectedStatus =
            point == detail::AssemblyCommandHistoryFailurePoint::Allocation ||
                    point == detail::AssemblyCommandHistoryFailurePoint::Reserve ||
                    kind == detail::AssemblyCommandHistoryFailureKind::BadAlloc
                ? AssemblyCommandHistoryStatus::ResourceLimit
                : AssemblyCommandHistoryStatus::ModelRejected;
        CHECK(status == expectedStatus);
        CHECK(first == 0);
        CHECK(second == 0);
        CHECK(snapshot(syntheticHistory, *syntheticProduct) == before);
        if (point == detail::AssemblyCommandHistoryFailurePoint::
                         AfterFirstSyntheticOperation) {
          CHECK(traceSize == 2 && trace[0] == 1 && trace[1] == -1);
        } else if (point ==
                   detail::AssemblyCommandHistoryFailurePoint::Finalization) {
          CHECK(traceSize == 4 && trace[0] == 1 && trace[1] == 2 &&
                trace[2] == -2 && trace[3] == -1);
        }
      }
    }

    AssemblyCommandHistory success;
    int first = 0;
    int second = 0;
    std::array<int, 4> trace{};
    std::size_t traceSize = 0;
    CHECK(detail::AssemblyCommandHistoryTestAccess::executeSyntheticTwoStep(
              success, syntheticCommand(), first, second, trace, traceSize) ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(first == 1 && second == 1);
    CHECK(traceSize == 2 && trace[0] == 1 && trace[1] == 2);
    CHECK(success.size() == 1 && success.cursor() == 1);
    CHECK(success.retainedBytes() == sizeof(OccurrenceId));

    AssemblyCommandHistory duplicate;
    int same = 0;
    traceSize = 0;
    CHECK(detail::AssemblyCommandHistoryTestAccess::executeSyntheticTwoStep(
              duplicate, syntheticCommand(), same, same, trace, traceSize) ==
          AssemblyCommandHistoryStatus::InvalidCommand);
    CHECK(same == 0 && duplicate.size() == 0);

    AssemblyCommandHistory contradictory;
    int contradictoryFirst = 0;
    int contradictorySecond = 0;
    traceSize = 0;
    CHECK(detail::AssemblyCommandHistoryTestAccess::executeSyntheticTwoStep(
              contradictory, syntheticCommand(), contradictoryFirst,
              contradictorySecond, trace, traceSize, true) ==
          AssemblyCommandHistoryStatus::InvalidCommand);
    CHECK(contradictoryFirst == 0 && contradictorySecond == 0 &&
          contradictory.size() == 0);
  }

  // Exact/over budget, redo-tail replacement, command count and revision
  // overflow all fail before visible history mutation.
  {
    // Full-path semantic state is retained memory too: two depth-2 vectors
    // move the cold-byte exact/+1 boundary by exactly four OccurrenceIds.
    constexpr std::size_t pathBytes = 4 * sizeof(OccurrenceId);
    const auto exactColdBytes =
        AssemblyCommandHistory::maximumRetainedBytes() - pathBytes;
    AssemblyCommandHistory exactPathBudget;
    CHECK(exactPathBudget.recordApplied(insertCommand(
              DefinitionId{2}, OccurrenceId{2},
              std::vector<std::byte>(exactColdBytes),
              {path({1, 7})}, {path({2, 7})})) ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(exactPathBudget.retainedBytes() ==
          AssemblyCommandHistory::maximumRetainedBytes());
  }
  {
    constexpr std::size_t pathBytes = 4 * sizeof(OccurrenceId);
    const auto overColdBytes =
        AssemblyCommandHistory::maximumRetainedBytes() - pathBytes + 1;
    AssemblyCommandHistory overPathBudget;
    CHECK(overPathBudget.recordApplied(insertCommand(
              DefinitionId{2}, OccurrenceId{2},
              std::vector<std::byte>(overColdBytes),
              {path({1, 7})}, {path({2, 7})})) ==
          AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(overPathBudget.size() == 0);
    CHECK(overPathBudget.retainedBytes() == 0);
  }
  {
    AssemblyCommandHistory budget;
    auto budgetProduct = emptyAssembly();
    auto applyAndRecord = [&](DefinitionId definition, OccurrenceId occurrence,
                              std::vector<std::byte> bytes) {
      EmbeddedPartInsertPayload item{definition, occurrence,
                                     RigidTransform3d::identity(), {},
                                     std::make_unique<Document>()};
      CHECK(budgetProduct->applyEmbeddedPartInsert(item) ==
            EmbeddedPartInsertStatus::Committed);
      CHECK(budget.recordApplied(insertCommand(
                definition, occurrence, std::move(bytes))) ==
            AssemblyCommandHistoryStatus::Committed);
    };
    applyAndRecord(DefinitionId{2}, OccurrenceId{2},
                   std::vector<std::byte>(1));
    const auto largeSize =
        AssemblyCommandHistory::maximumRetainedBytes() - 1;
    applyAndRecord(DefinitionId{3}, OccurrenceId{3},
                   std::vector<std::byte>(largeSize));
    CHECK(budget.retainedBytes() ==
          AssemblyCommandHistory::maximumRetainedBytes());
    CHECK(budget.undo(*budgetProduct, acceptPreparation).status ==
          AssemblyCommandHistoryStatus::Committed);
    const auto tailBefore = snapshot(budget, *budgetProduct);
    CHECK(budget.prepareRecord(insertCommand(
              DefinitionId{4}, OccurrenceId{4},
              std::vector<std::byte>(largeSize))) ==
          AssemblyCommandHistoryStatus::Committed);
    budget.discardPrepared();
    CHECK(snapshot(budget, *budgetProduct) == tailBefore);
    CHECK(budget.prepareRecord(insertCommand(
              DefinitionId{4}, OccurrenceId{4},
              std::vector<std::byte>(
                  AssemblyCommandHistory::maximumRetainedBytes()))) ==
          AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(snapshot(budget, *budgetProduct) == tailBefore);

    CHECK(budget.prepareRecord(insertCommand(
              DefinitionId{4}, OccurrenceId{4},
              std::vector<std::byte>(largeSize))) ==
          AssemblyCommandHistoryStatus::Committed);
    EmbeddedPartInsertPayload replacement{
        DefinitionId{4}, OccurrenceId{4}, RigidTransform3d::identity(), {},
        std::make_unique<Document>()};
    CHECK(budgetProduct->applyEmbeddedPartInsert(replacement) ==
          EmbeddedPartInsertStatus::Committed);
    CHECK(budget.commitPrepared() ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(!budget.canRedo());
    CHECK(budget.size() == 2 && budget.cursor() == 2);
    CHECK(budget.retainedBytes() ==
          AssemblyCommandHistory::maximumRetainedBytes());
  }

  {
    AssemblyCommandHistory count;
    for (std::size_t i = 0; i < AssemblyCommandHistory::maximumCommands(); ++i) {
      CHECK(count.recordApplied(insertCommand(
                DefinitionId{i + 1}, OccurrenceId{i + 1},
                std::vector<std::byte>(1))) ==
            AssemblyCommandHistoryStatus::Committed);
    }
    const auto sizeBefore = count.size();
    CHECK(count.recordApplied(insertCommand(
              DefinitionId{999}, OccurrenceId{999},
              std::vector<std::byte>(1))) ==
          AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(count.size() == sizeBefore);

    AssemblyCommandHistory revisions;
    detail::AssemblyCommandHistoryTestAccess::setNextRevision(revisions, 0);
    CHECK(revisions.recordApplied(insertCommand(
              DefinitionId{1}, OccurrenceId{1},
              std::vector<std::byte>(1))) ==
          AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(revisions.size() == 0 && revisions.isAtSavepoint());
  }

  // Destroying the authoritative product while history is retained cannot
  // keep runtime OCCT shapes alive.
  {
    AssemblyCommandHistory coldHistory;
    std::weak_ptr<const TopoDS_Shape> weak;
    auto coldProduct = emptyAssembly();
    auto owned = part(&weak);
    const auto bytes = cold(*owned);
    EmbeddedPartInsertPayload item{DefinitionId{2}, OccurrenceId{2},
                                   RigidTransform3d::identity(), {},
                                   std::move(owned)};
    CHECK(coldProduct->applyEmbeddedPartInsert(item) ==
          EmbeddedPartInsertStatus::Committed);
    CHECK(coldHistory.recordApplied(insertCommand(
              DefinitionId{2}, OccurrenceId{2}, bytes)) ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(!weak.expired());
    coldProduct.reset();
    CHECK(weak.expired());
    CHECK(coldHistory.retainedBytes() == bytes.size());
  }

  // A5c1 rename changes only authored bytes, is one bounded command and uses
  // the same exact address and before/after guards for Apply/Undo/Redo.
  {
    CHECK(validateOccurrenceDisplayNameUtf8("Кронштейн 🔩") ==
          OccurrenceDisplayNameStatus::Valid);
    CHECK(validateOccurrenceDisplayNameUtf8("   ") ==
          OccurrenceDisplayNameStatus::AllWhitespace);
    CHECK(validateOccurrenceDisplayNameUtf8(std::string("bad\0name", 8)) ==
          OccurrenceDisplayNameStatus::EmbeddedNull);
    CHECK(validateOccurrenceDisplayNameUtf8("line\nbreak") ==
          OccurrenceDisplayNameStatus::ForbiddenControl);
    CHECK(validateOccurrenceDisplayNameUtf8(std::string(16 * 1024 + 1, 'x')) ==
          OccurrenceDisplayNameStatus::TooLong);
    CHECK(validateOccurrenceDisplayNameUtf8(std::string(16 * 1024, 'x')) ==
          OccurrenceDisplayNameStatus::Valid);
    CHECK(validateOccurrenceDisplayNameUtf8("e\xcc\x81") ==
          OccurrenceDisplayNameStatus::Valid);
    CHECK(validateOccurrenceDisplayNameUtf8("\xe2\x80\xa8") ==
          OccurrenceDisplayNameStatus::ForbiddenControl);
    CHECK(validateOccurrenceDisplayNameUtf8(
              std::string{"\xc0\xaf", 2}) ==
          OccurrenceDisplayNameStatus::InvalidUtf8);

    auto renamedProduct = assemblyWithOccurrence();
    auto invalidBefore = OccurrenceDelta::rename(
        address(*renamedProduct, OccurrenceId{3}), "bad\nname", "Valid");
    CHECK(!invalidBefore &&
          invalidBefore.status ==
              OccurrenceDeltaCreateStatus::InvalidBeforeName);
    auto invalidAfter = OccurrenceDelta::rename(
        address(*renamedProduct, OccurrenceId{3}), {}, "   ");
    CHECK(!invalidAfter &&
          invalidAfter.status ==
              OccurrenceDeltaCreateStatus::InvalidAfterName);
    CHECK(renamedProduct->insertRootOccurrence(ComponentOccurrence(
              OccurrenceId{4}, DefinitionId{2},
              RigidTransform3d::identity(),
              {.visible = false, .suppressed = true, .grounded = true},
              "Сосед")) == ProductOccurrenceMutationStatus::Committed);
    const auto siblingBefore = *renamedProduct->rootAssemblyDefinition()->find(
        OccurrenceId{4});
    const auto* occurrence =
        renamedProduct->rootAssemblyDefinition()->find(OccurrenceId{3});
    CHECK(occurrence);
    const auto placement = occurrence->placement();
    const auto flags = occurrence->flags();
    const auto* document =
        &renamedProduct->definitions().findPart(DefinitionId{2})->document();
    auto created = OccurrenceDelta::rename(
        address(*renamedProduct, OccurrenceId{3}), {}, "Кронштейн 🔩");
    CHECK(created);
    AssemblyCommandHistory renameHistory;
    const auto before = snapshot(renameHistory, *renamedProduct);
    OccurrenceDeltaCommand command{
        std::move(*created.delta), {}, {path({3})}};
    const auto applied = renameHistory.executeOccurrenceDelta(
        *renamedProduct, std::move(command), acceptPreparation);
    CHECK(applied.status == AssemblyCommandHistoryStatus::Committed);
    CHECK(applied.deltaStatus == OccurrenceDeltaStatus::Committed);
    CHECK(applied.restoredState ==
          (AssemblyCommandSemanticState{path({3})}));
    CHECK(renameHistory.size() == 1 && renameHistory.cursor() == 1);
    CHECK(!renameHistory.isAtSavepoint());
    occurrence =
        renamedProduct->rootAssemblyDefinition()->find(OccurrenceId{3});
    CHECK(occurrence->displayNameUtf8() == "Кронштейн 🔩");
    CHECK(occurrence->placement() == placement && occurrence->flags() == flags);
    CHECK(&renamedProduct->definitions()
               .findPart(DefinitionId{2})
               ->document() == document);
    const auto* siblingAfter =
        renamedProduct->rootAssemblyDefinition()->find(OccurrenceId{4});
    CHECK(siblingAfter && siblingAfter->id() == siblingBefore.id() &&
          siblingAfter->targetDefinitionId() ==
              siblingBefore.targetDefinitionId() &&
          siblingAfter->placement() == siblingBefore.placement() &&
          siblingAfter->flags() == siblingBefore.flags() &&
          siblingAfter->displayNameUtf8() ==
              siblingBefore.displayNameUtf8());

    const auto renameUndone =
        renameHistory.undo(*renamedProduct, acceptPreparation);
    CHECK(renameUndone.status == AssemblyCommandHistoryStatus::Committed);
    CHECK(renameUndone.restoredState == AssemblyCommandSemanticState{});
    CHECK(renameHistory.isAtSavepoint());
    CHECK(renamedProduct->rootAssemblyDefinition()
              ->find(OccurrenceId{3})
              ->displayNameUtf8()
              .empty());
    const auto renameRedone =
        renameHistory.redo(*renamedProduct, acceptPreparation);
    CHECK(renameRedone.status == AssemblyCommandHistoryStatus::Committed);
    CHECK(renameRedone.restoredState ==
          (AssemblyCommandSemanticState{path({3})}));
    CHECK(!renameHistory.isAtSavepoint());
    CHECK(renamedProduct->rootAssemblyDefinition()
              ->find(OccurrenceId{3})
              ->displayNameUtf8() == "Кронштейн 🔩");

    ResourceFailureSuccessor undoResourceFailure;
    const auto beforeUndoResourceFailure =
        snapshot(renameHistory, *renamedProduct);
    const auto productBeforeUndoResourceFailure =
        productSnapshot(*renamedProduct, DefinitionId{2});
    const auto failedUndo =
        renameHistory.undo(*renamedProduct, undoResourceFailure);
    CHECK(failedUndo.status == AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(snapshot(renameHistory, *renamedProduct) ==
          beforeUndoResourceFailure);
    CHECK(productSnapshot(*renamedProduct, DefinitionId{2}) ==
          productBeforeUndoResourceFailure);
    CHECK(renamedProduct->rootAssemblyDefinition()
              ->find(OccurrenceId{3})
              ->displayNameUtf8() == "Кронштейн 🔩");
    CHECK(undoResourceFailure.calls == 1);

    auto rejected = OccurrenceDelta::rename(
        address(*renamedProduct, OccurrenceId{3}), "Кронштейн 🔩", "Другая");
    CHECK(rejected);
    ResourceFailureSuccessor resourceFailure;
    const auto beforeFailure = snapshot(renameHistory, *renamedProduct);
    const auto productBeforeFailure =
        productSnapshot(*renamedProduct, DefinitionId{2});
    const auto failed = renameHistory.executeOccurrenceDelta(
        *renamedProduct,
        {std::move(*rejected.delta), {path({3})},
         {path({3})}},
        resourceFailure);
    CHECK(failed.status == AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(snapshot(renameHistory, *renamedProduct) == beforeFailure);
    CHECK(productSnapshot(*renamedProduct, DefinitionId{2}) ==
          productBeforeFailure);
    CHECK(renamedProduct->rootAssemblyDefinition()
              ->find(OccurrenceId{3})
              ->displayNameUtf8() == "Кронштейн 🔩");
    CHECK(resourceFailure.calls == 1);
    CHECK(before.historySize == 0);

    // Current-value mismatch is an exact model no-op in both directions.
    auto stale = OccurrenceDelta::rename(
        address(*renamedProduct, OccurrenceId{3}), "Устаревшее", "Будущее");
    CHECK(stale);
    const auto staleBefore = productSnapshot(*renamedProduct, DefinitionId{2});
    CHECK(renamedProduct->applyOccurrenceDelta(*stale.delta) ==
          OccurrenceDeltaStatus::CurrentValueMismatch);
    CHECK(renamedProduct->revertOccurrenceDelta(*stale.delta) ==
          OccurrenceDeltaStatus::CurrentValueMismatch);
    CHECK(productSnapshot(*renamedProduct, DefinitionId{2}) == staleBefore);

    // A second Rename may be undone, then an equal before/after command must
    // remain a true no-op and preserve that redo tail byte-for-byte.
    auto second = OccurrenceDelta::rename(
        address(*renamedProduct, OccurrenceId{3}), "Кронштейн 🔩",
        std::string(128, 'N'));
    CHECK(second);
    const std::size_t expectedSecondBytes =
        second.delta->address().path.entries().capacity() *
            sizeof(OccurrenceId) +
        second.delta->rename()->beforeUtf8.capacity() +
        second.delta->rename()->afterUtf8.capacity();
    CHECK(renameHistory.executeOccurrenceDelta(
              *renamedProduct,
              {std::move(*second.delta), {path({3})},
               {path({3})}},
              acceptPreparation)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(renameHistory.retainedBytes() >= expectedSecondBytes);
    CHECK(renameHistory.undo(*renamedProduct, acceptPreparation).status ==
          AssemblyCommandHistoryStatus::Committed);
    const auto redoTailBefore = snapshot(renameHistory, *renamedProduct);
    auto equal = OccurrenceDelta::rename(
        address(*renamedProduct, OccurrenceId{3}), "Кронштейн 🔩",
        "Кронштейн 🔩");
    CHECK(equal);
    int noChangeSuccessorCalls = 0;
    auto countUnexpectedSuccessor = [&](const ProductDocument&) {
      ++noChangeSuccessorCalls;
      return true;
    };
    CallableSuccessor noChangeSuccessor(countUnexpectedSuccessor);
    const auto noChange = renameHistory.executeOccurrenceDelta(
        *renamedProduct,
        {std::move(*equal.delta), {path({3})},
         {path({3})}},
        noChangeSuccessor);
    CHECK(noChange.status == AssemblyCommandHistoryStatus::NoChange);
    CHECK(noChange.deltaStatus == OccurrenceDeltaStatus::NoChange);
    CHECK(noChangeSuccessorCalls == 0);
    CHECK(snapshot(renameHistory, *renamedProduct) == redoTailBefore);
    CHECK(renameHistory.redo(*renamedProduct, acceptPreparation).status ==
          AssemblyCommandHistoryStatus::Committed);

    // Hidden/suppressed/grounded is not an eligibility or model guard for
    // Rename; all flags remain exact through the direct Apply/Revert pair.
    auto hiddenRename = OccurrenceDelta::rename(
        address(*renamedProduct, OccurrenceId{4}), "Сосед", "Скрытый сосед");
    CHECK(hiddenRename);
    const auto hiddenBefore = *renamedProduct->rootAssemblyDefinition()->find(
        OccurrenceId{4});
    CHECK(renamedProduct->applyOccurrenceDelta(*hiddenRename.delta) ==
          OccurrenceDeltaStatus::Committed);
    const auto* hiddenAfter =
        renamedProduct->rootAssemblyDefinition()->find(OccurrenceId{4});
    CHECK(hiddenAfter && hiddenAfter->flags() == hiddenBefore.flags() &&
          hiddenAfter->placement() == hiddenBefore.placement() &&
          hiddenAfter->displayNameUtf8() == "Скрытый сосед");
    CHECK(renamedProduct->revertOccurrenceDelta(*hiddenRename.delta) ==
          OccurrenceDeltaStatus::Committed);
    const auto* hiddenRestored =
        renamedProduct->rootAssemblyDefinition()->find(OccurrenceId{4});
    CHECK(hiddenRestored && hiddenRestored->id() == hiddenBefore.id() &&
          hiddenRestored->targetDefinitionId() ==
              hiddenBefore.targetDefinitionId() &&
          hiddenRestored->placement() == hiddenBefore.placement() &&
          hiddenRestored->flags() == hiddenBefore.flags() &&
          hiddenRestored->displayNameUtf8() ==
              hiddenBefore.displayNameUtf8());
  }

  // A later Rename makes the older Insert cold guard stale until Rename is
  // reverted; the paired extractor must never erase a differently named
  // occurrence/definition pair.
  {
    auto guarded = emptyAssembly();
    EmbeddedPartInsertPayload inserted{
        DefinitionId{2}, OccurrenceId{2}, RigidTransform3d::identity(), {},
        std::make_unique<Document>()};
    CHECK(guarded->applyEmbeddedPartInsert(inserted) ==
          EmbeddedPartInsertStatus::Committed);
    auto renamed = OccurrenceDelta::rename(
        address(*guarded, OccurrenceId{2}), {}, "Имя");
    CHECK(renamed);
    CHECK(guarded->applyOccurrenceDelta(*renamed.delta) ==
          OccurrenceDeltaStatus::Committed);
    CHECK(guarded->extractEmbeddedPartInsert(inserted) ==
          EmbeddedPartInsertStatus::GuardMismatch);
    CHECK(guarded->revertOccurrenceDelta(*renamed.delta) ==
          OccurrenceDeltaStatus::Committed);
    CHECK(guarded->extractEmbeddedPartInsert(inserted) ==
          EmbeddedPartInsertStatus::Extracted);
  }

  // A5c2 Visibility is a narrow authored-bit delta.  It neither overwrites
  // neighbouring flags nor asks for a full/tree-only successor.
  {
    auto visibleProduct = assemblyWithOccurrence();
    const auto occurrencePath = path({3});
    OccurrenceFlags initialFlags;
    initialFlags.visible = true;
    initialFlags.suppressed = true;
    initialFlags.grounded = true;
    CHECK(visibleProduct->setRootOccurrenceFlags(occurrencePath,
                                                 initialFlags) ==
          ProductOccurrenceMutationStatus::Committed);
    const auto before =
        productSnapshot(*visibleProduct, DefinitionId{2});
    auto hide = OccurrenceDelta::visibility(
        address(*visibleProduct, OccurrenceId{3}), true, false);
    ProductDocumentSuccessorImpact observedImpact =
        ProductDocumentSuccessorImpact::FullProjection;
    auto acceptVisibility = [&](const ProductDocument& candidate) {
      const auto* current = candidate.rootAssemblyDefinition()->find(
          OccurrenceId{3});
      return current && !current->flags().visible &&
             current->flags().suppressed && current->flags().grounded;
    };
    class VisibilitySuccessor final : public ProductDocumentSuccessor {
     public:
      VisibilitySuccessor(decltype(acceptVisibility)& accept,
                          ProductDocumentSuccessorImpact& impact) noexcept
          : accept_(accept), impact_(impact) {}
      ProductDocumentSuccessorStatus prepare(
          const ProductDocument& candidate,
          ProductDocumentSuccessorImpact impact) noexcept override {
        impact_ = impact;
        return accept_(candidate) ? ProductDocumentSuccessorStatus::Prepared
                                  : ProductDocumentSuccessorStatus::Rejected;
      }
     private:
      decltype(acceptVisibility)& accept_;
      ProductDocumentSuccessorImpact& impact_;
    } visibilitySuccessor(acceptVisibility, observedImpact);
    CHECK(visibleProduct->applyOccurrenceDelta(hide, visibilitySuccessor) ==
          OccurrenceDeltaStatus::Committed);
    CHECK(observedImpact ==
          ProductDocumentSuccessorImpact::VisibilityProjection);
    const auto* hidden = visibleProduct->rootAssemblyDefinition()->find(
        OccurrenceId{3});
    CHECK(hidden && !hidden->flags().visible && hidden->flags().suppressed &&
          hidden->flags().grounded &&
          hidden->placement() == before.occurrences.front().placement &&
          hidden->displayNameUtf8() ==
              before.occurrences.front().displayNameUtf8);
    CHECK(visibleProduct->revertOccurrenceDelta(hide) ==
          OccurrenceDeltaStatus::Committed);
    CHECK(productSnapshot(*visibleProduct, DefinitionId{2}) == before);

    ResourceFailureSuccessor resourceFailure;
    CHECK(visibleProduct->applyOccurrenceDelta(hide, resourceFailure) ==
          OccurrenceDeltaStatus::ResourceFailure);
    CHECK(resourceFailure.calls == 1);
    CHECK(productSnapshot(*visibleProduct, DefinitionId{2}) == before);

    auto stale = OccurrenceDelta::visibility(
        address(*visibleProduct, OccurrenceId{3}), false, true);
    CHECK(visibleProduct->applyOccurrenceDelta(stale) ==
          OccurrenceDeltaStatus::CurrentValueMismatch);
    CHECK(productSnapshot(*visibleProduct, DefinitionId{2}) == before);

    AssemblyCommandHistory history;
    auto accept = [](const ProductDocument&) { return true; };
    CallableSuccessor historySuccessor(accept);
    CHECK(history.executeOccurrenceDelta(
              *visibleProduct,
              {hide, {path({3})}, {path({3})}},
              historySuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(history.size() == 1 && history.cursor() == 1 &&
          !history.isAtSavepoint());
    CHECK(history.retainedBytes() ==
          (hide.address().path.entries().capacity() +
           path({3}).entries().capacity() * 2) * sizeof(OccurrenceId));
    CHECK(history.undo(*visibleProduct, historySuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(productSnapshot(*visibleProduct, DefinitionId{2}) == before);
    CHECK(history.redo(*visibleProduct, historySuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(!visibleProduct->rootAssemblyDefinition()
               ->find(OccurrenceId{3})
               ->flags()
               .visible);

    CHECK(history.undo(*visibleProduct, historySuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    const auto redoTail = snapshot(history, *visibleProduct);
    auto equal = OccurrenceDelta::visibility(
        address(*visibleProduct, OccurrenceId{3}), true, true);
    int calls = 0;
    auto count = [&](const ProductDocument&) {
      ++calls;
      return true;
    };
    CallableSuccessor noChangeSuccessor(count);
    CHECK(history.executeOccurrenceDelta(
              *visibleProduct,
              {std::move(equal), {path({3})}, {path({3})}},
              noChangeSuccessor)
              .status == AssemblyCommandHistoryStatus::NoChange);
    CHECK(calls == 0);
    CHECK(snapshot(history, *visibleProduct) == redoTail);
  }

  // A5c3 Grounding is an exact bit-only delta.  While its transaction is
  // active even a successor attempting the formerly-public placement path is
  // rejected as Busy, and a failed successor restores every authored value.
  {
    auto groundedProduct = assemblyWithOccurrence();
    CHECK(groundedProduct->setRootOccurrenceFlags(
              path({3}),
              {.visible = false, .suppressed = true, .grounded = false}) ==
          ProductOccurrenceMutationStatus::Committed);
    const auto before = productSnapshot(*groundedProduct, DefinitionId{2});
    const auto attemptedPlacementResult = RigidTransform3d::create(
        {12.0, -3.0, 7.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(attemptedPlacementResult);
    const RigidTransform3d attemptedPlacement =
        *attemptedPlacementResult.transform;
    auto ground = OccurrenceDelta::grounded(
        address(*groundedProduct, OccurrenceId{3}), false, true);
    ProductDocumentSuccessorImpact observedImpact =
        ProductDocumentSuccessorImpact::FullProjection;
    ProductOccurrenceMutationStatus reentrantPlacement =
        ProductOccurrenceMutationStatus::Committed;
    class GroundingSuccessor final : public ProductDocumentSuccessor {
     public:
      GroundingSuccessor(ProductDocument& product,
                         ProductDocumentSuccessorImpact& impact,
                         ProductOccurrenceMutationStatus& placement,
                         const RigidTransform3d& attempted) noexcept
          : product_(product), impact_(impact), placement_(placement),
            attempted_(attempted) {}

      ProductDocumentSuccessorStatus prepare(
          const ProductDocument& candidate,
          ProductDocumentSuccessorImpact impact) noexcept override {
        impact_ = impact;
        const auto* occurrence = candidate.rootAssemblyDefinition()->find(
            OccurrenceId{3});
        if (!occurrence || !occurrence->flags().grounded)
          return ProductDocumentSuccessorStatus::Rejected;
        placement_ = product_.setRootOccurrencePlacement(
            path({3}), attempted_);
        return placement_ == ProductOccurrenceMutationStatus::Busy
                   ? ProductDocumentSuccessorStatus::Prepared
                   : ProductDocumentSuccessorStatus::Rejected;
      }

     private:
      ProductDocument& product_;
      ProductDocumentSuccessorImpact& impact_;
      ProductOccurrenceMutationStatus& placement_;
      const RigidTransform3d& attempted_;
    } groundingSuccessor(*groundedProduct, observedImpact,
                         reentrantPlacement, attemptedPlacement);

    CHECK(groundedProduct->applyOccurrenceDelta(ground,
                                                groundingSuccessor) ==
          OccurrenceDeltaStatus::Committed);
    CHECK(observedImpact ==
          ProductDocumentSuccessorImpact::GroundingProjection);
    CHECK(reentrantPlacement == ProductOccurrenceMutationStatus::Busy);
    const auto* grounded = groundedProduct->rootAssemblyDefinition()->find(
        OccurrenceId{3});
    CHECK(grounded && grounded->flags().grounded);
    CHECK(grounded->placement() == before.occurrences.front().placement);
    CHECK(grounded->flags().visible == before.occurrences.front().flags.visible);
    CHECK(grounded->flags().suppressed ==
          before.occurrences.front().flags.suppressed);
    CHECK(grounded->displayNameUtf8() ==
          before.occurrences.front().displayNameUtf8);
    CHECK(groundedProduct->setRootOccurrencePlacement(
              path({3}), attemptedPlacement) ==
          ProductOccurrenceMutationStatus::OccurrenceGrounded);
    CHECK(groundedProduct->revertOccurrenceDelta(ground) ==
          OccurrenceDeltaStatus::Committed);
    CHECK(productSnapshot(*groundedProduct, DefinitionId{2}) == before);
    CHECK(groundedProduct->setRootOccurrencePlacement(
              path({3}), attemptedPlacement) ==
          ProductOccurrenceMutationStatus::Committed);
    CHECK(groundedProduct->setRootOccurrencePlacement(
              path({3}), RigidTransform3d::identity()) ==
          ProductOccurrenceMutationStatus::Committed);
    CHECK(productSnapshot(*groundedProduct, DefinitionId{2}) == before);

    ResourceFailureSuccessor resourceFailure;
    CHECK(groundedProduct->applyOccurrenceDelta(ground, resourceFailure) ==
          OccurrenceDeltaStatus::ResourceFailure);
    CHECK(resourceFailure.calls == 1);
    CHECK(productSnapshot(*groundedProduct, DefinitionId{2}) == before);

    auto stale = OccurrenceDelta::grounded(
        address(*groundedProduct, OccurrenceId{3}), true, false);
    CHECK(groundedProduct->applyOccurrenceDelta(stale) ==
          OccurrenceDeltaStatus::CurrentValueMismatch);
    CHECK(productSnapshot(*groundedProduct, DefinitionId{2}) == before);

    AssemblyCommandHistory history;
    auto accept = [](const ProductDocument&) { return true; };
    CallableSuccessor historySuccessor(accept);
    CHECK(history.executeOccurrenceDelta(
              *groundedProduct,
              {ground, {path({3})}, {path({3})}},
              historySuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(history.size() == 1 && history.cursor() == 1 &&
          !history.isAtSavepoint());
    CHECK(groundedProduct->rootAssemblyDefinition()
              ->find(OccurrenceId{3})
              ->flags()
              .grounded);
    CHECK(history.undo(*groundedProduct, historySuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(productSnapshot(*groundedProduct, DefinitionId{2}) == before);
    CHECK(history.redo(*groundedProduct, historySuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(groundedProduct->rootAssemblyDefinition()
              ->find(OccurrenceId{3})
              ->flags()
              .grounded);

    CHECK(history.undo(*groundedProduct, historySuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    const auto redoTail = snapshot(history, *groundedProduct);
    auto equal = OccurrenceDelta::grounded(
        address(*groundedProduct, OccurrenceId{3}), false, false);
    int calls = 0;
    auto count = [&](const ProductDocument&) {
      ++calls;
      return true;
    };
    CallableSuccessor noChangeSuccessor(count);
    CHECK(history.executeOccurrenceDelta(
              *groundedProduct,
              {std::move(equal), {path({3})}, {path({3})}},
              noChangeSuccessor)
              .status == AssemblyCommandHistoryStatus::NoChange);
    CHECK(calls == 0);
    CHECK(snapshot(history, *groundedProduct) == redoTail);
  }

  // A5c4 Suppression changes only the suppression bit.  The legacy whole-
  // flags mutator is closed while an occurrence transaction is active, so a
  // captured mutable alias cannot overwrite the candidate or its rollback.
  {
    auto product = assemblyWithOccurrence();
    const OccurrenceFlags initial{false, false, true};
    CHECK(product->setRootOccurrenceFlags(path({3}), initial) ==
          ProductOccurrenceMutationStatus::Committed);
    const auto before = productSnapshot(*product, DefinitionId{2});
    auto suppress = OccurrenceDelta::suppressed(
        address(*product, OccurrenceId{3}), false, true);
    ProductDocumentSuccessorImpact observedImpact =
        ProductDocumentSuccessorImpact::FullProjection;
    ProductOccurrenceMutationStatus reentrantFlags =
        ProductOccurrenceMutationStatus::Committed;
    class SuppressionSuccessor final : public ProductDocumentSuccessor {
     public:
      SuppressionSuccessor(ProductDocument& product,
                           ProductDocumentSuccessorImpact& impact,
                           ProductOccurrenceMutationStatus& flags) noexcept
          : product_(product), impact_(impact), flags_(flags) {}

      ProductDocumentSuccessorStatus prepare(
          const ProductDocument& candidate,
          ProductDocumentSuccessorImpact impact) noexcept override {
        impact_ = impact;
        const auto* occurrence = candidate.rootAssemblyDefinition()->find(
            OccurrenceId{3});
        if (!occurrence || !occurrence->flags().suppressed)
          return ProductDocumentSuccessorStatus::Rejected;
        flags_ = product_.setRootOccurrenceFlags(
            path({3}), {.visible = true, .suppressed = false,
                        .grounded = false});
        return flags_ == ProductOccurrenceMutationStatus::Busy
                   ? ProductDocumentSuccessorStatus::Prepared
                   : ProductDocumentSuccessorStatus::Rejected;
      }

     private:
      ProductDocument& product_;
      ProductDocumentSuccessorImpact& impact_;
      ProductOccurrenceMutationStatus& flags_;
    } successor(*product, observedImpact, reentrantFlags);

    CHECK(product->applyOccurrenceDelta(suppress, successor) ==
          OccurrenceDeltaStatus::Committed);
    CHECK(observedImpact ==
          ProductDocumentSuccessorImpact::SuppressionProjection);
    CHECK(reentrantFlags == ProductOccurrenceMutationStatus::Busy);
    const auto* suppressed =
        product->rootAssemblyDefinition()->find(OccurrenceId{3});
    CHECK(suppressed && suppressed->flags().suppressed);
    CHECK(!suppressed->flags().visible && suppressed->flags().grounded);
    CHECK(suppressed->placement() == before.occurrences.front().placement);
    CHECK(suppressed->displayNameUtf8() ==
          before.occurrences.front().displayNameUtf8);
    CHECK(product->revertOccurrenceDelta(suppress) ==
          OccurrenceDeltaStatus::Committed);
    CHECK(productSnapshot(*product, DefinitionId{2}) == before);

    ResourceFailureSuccessor resourceFailure;
    CHECK(product->applyOccurrenceDelta(suppress, resourceFailure) ==
          OccurrenceDeltaStatus::ResourceFailure);
    CHECK(resourceFailure.calls == 1);
    CHECK(productSnapshot(*product, DefinitionId{2}) == before);

    auto stale = OccurrenceDelta::suppressed(
        address(*product, OccurrenceId{3}), true, false);
    CHECK(product->applyOccurrenceDelta(stale) ==
          OccurrenceDeltaStatus::CurrentValueMismatch);
    CHECK(productSnapshot(*product, DefinitionId{2}) == before);

    AssemblyCommandHistory history;
    auto accept = [](const ProductDocument&) { return true; };
    CallableSuccessor historySuccessor(accept);
    CHECK(history.executeOccurrenceDelta(
              *product,
              {suppress, {path({3})}, {path({3})}},
              historySuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(history.size() == 1 && history.cursor() == 1 &&
          !history.isAtSavepoint());
    CHECK(product->rootAssemblyDefinition()
              ->find(OccurrenceId{3})
              ->flags()
              .suppressed);
    CHECK(history.undo(*product, historySuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(productSnapshot(*product, DefinitionId{2}) == before);
    CHECK(history.redo(*product, historySuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(product->rootAssemblyDefinition()
              ->find(OccurrenceId{3})
              ->flags()
              .suppressed);

    CHECK(history.undo(*product, historySuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    const auto redoTail = snapshot(history, *product);
    auto equal = OccurrenceDelta::suppressed(
        address(*product, OccurrenceId{3}), false, false);
    int calls = 0;
    auto count = [&](const ProductDocument&) {
      ++calls;
      return true;
    };
    CallableSuccessor noChangeSuccessor(count);
    CHECK(history.executeOccurrenceDelta(
              *product,
              {std::move(equal), {path({3})}, {path({3})}},
              noChangeSuccessor)
              .status == AssemblyCommandHistoryStatus::NoChange);
    CHECK(calls == 0);
    CHECK(snapshot(history, *product) == redoTail);
  }

  // All authored A5c occurrence edits share one ordered history.  Exercise a
  // mixed chain so adding Suppression cannot accidentally introduce a second
  // cursor or overwrite any of Rename/Visibility/Grounding state.
  {
    auto product = assemblyWithOccurrence();
    const auto before = productSnapshot(*product, DefinitionId{2});
    AssemblyCommandHistory history;
    auto accept = [](const ProductDocument&) { return true; };
    CallableSuccessor successor(accept);
    const AssemblyCommandSemanticState selected{path({3})};

    auto rename = OccurrenceDelta::rename(
        address(*product, OccurrenceId{3}), {}, "Смешанный 🔩");
    CHECK(rename);
    CHECK(history.executeOccurrenceDelta(
              *product, {std::move(*rename.delta), selected, selected},
              successor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(history.executeOccurrenceDelta(
              *product,
              {OccurrenceDelta::visibility(
                   address(*product, OccurrenceId{3}), true, false),
               selected, selected},
              successor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(history.executeOccurrenceDelta(
              *product,
              {OccurrenceDelta::grounded(
                   address(*product, OccurrenceId{3}), false, true),
               selected, selected},
              successor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(history.executeOccurrenceDelta(
              *product,
              {OccurrenceDelta::suppressed(
                   address(*product, OccurrenceId{3}), false, true),
               selected, selected},
              successor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(history.size() == 4 && history.cursor() == 4 &&
          !history.isAtSavepoint());
    const auto* edited =
        product->rootAssemblyDefinition()->find(OccurrenceId{3});
    CHECK(edited && edited->displayNameUtf8() == "Смешанный 🔩" &&
          !edited->flags().visible && edited->flags().grounded &&
          edited->flags().suppressed);

    for (int index = 0; index < 4; ++index) {
      CHECK(history.undo(*product, successor).status ==
            AssemblyCommandHistoryStatus::Committed);
    }
    CHECK(history.cursor() == 0);
    CHECK(productSnapshot(*product, DefinitionId{2}) == before);

    for (int index = 0; index < 4; ++index) {
      CHECK(history.redo(*product, successor).status ==
            AssemblyCommandHistoryStatus::Committed);
    }
    CHECK(history.cursor() == 4);
    edited = product->rootAssemblyDefinition()->find(OccurrenceId{3});
    CHECK(edited && edited->displayNameUtf8() == "Смешанный 🔩" &&
          !edited->flags().visible && edited->flags().grounded &&
          edited->flags().suppressed);
  }

  // A5c5 Duplicate is a structural cold command: it inserts/removes only one
  // occurrence node, reuses the exact Part definition/Document/B-Rep, and
  // retains one stable proposed ID across Undo/Redo.
  {
    const auto placed = RigidTransform3d::create(
        {17.0, -4.0, 9.0}, {0.9238795325, 0.0, 0.3826834323, 0.0});
    CHECK(placed);
    const auto home = RigidTransform3d::create(
        {-3.0, 8.0, 2.0}, {0.9238795325, 0.0, -0.3826834323, 0.0});
    CHECK(home);
    std::weak_ptr<const TopoDS_Shape> sourceShape;
    DefinitionRegistry definitions;
    CHECK(definitions.insert(
              PartDefinition(DefinitionId{2}, part(&sourceShape))) ==
          DefinitionInsertStatus::Inserted);
    AssemblyDefinition root(DefinitionId{1});
    CHECK(root.insert(ComponentOccurrence(
              OccurrenceId{1}, DefinitionId{2}, RigidTransform3d::identity())) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(root.insert(ComponentOccurrence(
              OccurrenceId{3}, DefinitionId{2}, *placed.transform,
              *home.transform,
              {.visible = false, .suppressed = true, .grounded = true},
              "Копируемый 🔩")) == AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(definitions.insert(std::move(root)) ==
          DefinitionInsertStatus::Inserted);
    auto made = ProductDocument::create(std::move(definitions), DefinitionId{1});
    CHECK(made);
    auto product = std::move(made.product);
    CHECK(product->lowestFreeRootOccurrenceId() == OccurrenceId{2});
    const auto before = productSnapshot(*product, DefinitionId{2});
    const auto* partBefore = product->definitions().findPart(DefinitionId{2});
    CHECK(partBefore);
    const Document* documentBefore = &partBefore->document();
    const auto shapeBefore = sourceShape.lock();
    CHECK(shapeBefore);

    auto duplicate = duplicatePayload(*product, OccurrenceId{3},
                                      OccurrenceId{2});
    ProductDocumentSuccessorImpact impact =
        ProductDocumentSuccessorImpact::FullProjection;
    ProductOccurrenceMutationStatus reentrantInsert =
        ProductOccurrenceMutationStatus::Committed;
    ProductOccurrenceMutationStatus reentrantRemove =
        ProductOccurrenceMutationStatus::Committed;
    ProductOccurrenceMutationStatus reentrantPlacement =
        ProductOccurrenceMutationStatus::Committed;
    ProductOccurrenceMutationStatus reentrantFlags =
        ProductOccurrenceMutationStatus::Committed;
    OccurrenceDeltaStatus reentrantDelta = OccurrenceDeltaStatus::Committed;
    DuplicateOccurrenceStatus reentrantDuplicate =
        DuplicateOccurrenceStatus::Committed;
    EmbeddedPartInsertStatus reentrantEmbedded =
        EmbeddedPartInsertStatus::Committed;
    auto reentrantVisibility = OccurrenceDelta::visibility(
        duplicate.source, false, true);
    EmbeddedPartInsertPayload reentrantEmbeddedPayload{
        DefinitionId{9}, OccurrenceId{9}, RigidTransform3d::identity(), {},
        std::make_unique<Document>(), {}};
    class DuplicateSuccessor final : public ProductDocumentSuccessor {
     public:
      DuplicateSuccessor(ProductDocument& product,
                         ProductDocumentSuccessorImpact& impact,
                         ProductOccurrenceMutationStatus& insert,
                         ProductOccurrenceMutationStatus& remove,
                         ProductOccurrenceMutationStatus& placement,
                         ProductOccurrenceMutationStatus& flags,
                         const OccurrenceDelta& delta,
                         OccurrenceDeltaStatus& deltaStatus,
                         const DuplicateOccurrenceColdPayload& duplicate,
                         DuplicateOccurrenceStatus& duplicateStatus,
                         EmbeddedPartInsertPayload& embedded,
                         EmbeddedPartInsertStatus& embeddedStatus) noexcept
          : product_(product), impact_(impact), insert_(insert),
            remove_(remove), placement_(placement), flags_(flags),
            delta_(delta), deltaStatus_(deltaStatus), duplicate_(duplicate),
            duplicateStatus_(duplicateStatus), embedded_(embedded),
            embeddedStatus_(embeddedStatus) {}

      ProductDocumentSuccessorStatus prepare(
          const ProductDocument& candidate,
          ProductDocumentSuccessorImpact impact) noexcept override {
        impact_ = impact;
        const auto* inserted =
            candidate.rootAssemblyDefinition()->find(OccurrenceId{2});
        if (!inserted || inserted->displayNameUtf8() != "Копируемый 🔩")
          return ProductDocumentSuccessorStatus::Rejected;
        insert_ = product_.insertRootOccurrence(ComponentOccurrence(
            OccurrenceId{9}, DefinitionId{2}, RigidTransform3d::identity()));
        remove_ = product_.removeRootOccurrence(path({3}));
        placement_ = product_.setRootOccurrencePlacement(
            path({3}), RigidTransform3d::identity());
        flags_ = product_.setRootOccurrenceFlags(path({3}), {});
        deltaStatus_ = product_.applyOccurrenceDelta(delta_, *this);
        duplicateStatus_ = product_.applyDuplicateOccurrence(
            duplicate_, *this, DuplicateOccurrenceFailurePoint::None);
        embeddedStatus_ = product_.applyEmbeddedPartInsert(embedded_, *this);
        return insert_ == ProductOccurrenceMutationStatus::Busy &&
                       remove_ == ProductOccurrenceMutationStatus::Busy &&
                       placement_ == ProductOccurrenceMutationStatus::Busy &&
                       flags_ == ProductOccurrenceMutationStatus::Busy &&
                       deltaStatus_ == OccurrenceDeltaStatus::Busy &&
                       duplicateStatus_ == DuplicateOccurrenceStatus::Busy &&
                       embeddedStatus_ == EmbeddedPartInsertStatus::Busy &&
                       embedded_.detached()
                   ? ProductDocumentSuccessorStatus::Prepared
                   : ProductDocumentSuccessorStatus::Rejected;
      }

     private:
      ProductDocument& product_;
      ProductDocumentSuccessorImpact& impact_;
      ProductOccurrenceMutationStatus& insert_;
      ProductOccurrenceMutationStatus& remove_;
      ProductOccurrenceMutationStatus& placement_;
      ProductOccurrenceMutationStatus& flags_;
      const OccurrenceDelta& delta_;
      OccurrenceDeltaStatus& deltaStatus_;
      const DuplicateOccurrenceColdPayload& duplicate_;
      DuplicateOccurrenceStatus& duplicateStatus_;
      EmbeddedPartInsertPayload& embedded_;
      EmbeddedPartInsertStatus& embeddedStatus_;
    } duplicateSuccessor(*product, impact, reentrantInsert, reentrantRemove,
                         reentrantPlacement, reentrantFlags,
                         reentrantVisibility, reentrantDelta, duplicate,
                         reentrantDuplicate, reentrantEmbeddedPayload,
                         reentrantEmbedded);

    CHECK(product->applyDuplicateOccurrence(
              duplicate, duplicateSuccessor,
              DuplicateOccurrenceFailurePoint::None) ==
          DuplicateOccurrenceStatus::Committed);
    CHECK(impact == ProductDocumentSuccessorImpact::DuplicateProjection);
    CHECK(reentrantInsert == ProductOccurrenceMutationStatus::Busy);
    CHECK(reentrantRemove == ProductOccurrenceMutationStatus::Busy);
    CHECK(reentrantPlacement == ProductOccurrenceMutationStatus::Busy);
    CHECK(reentrantFlags == ProductOccurrenceMutationStatus::Busy);
    CHECK(reentrantDelta == OccurrenceDeltaStatus::Busy);
    CHECK(reentrantDuplicate == DuplicateOccurrenceStatus::Busy);
    CHECK(reentrantEmbedded == EmbeddedPartInsertStatus::Busy);
    CHECK(reentrantEmbeddedPayload.detached());
    const auto* source =
        product->rootAssemblyDefinition()->find(OccurrenceId{3});
    const auto* copy =
        product->rootAssemblyDefinition()->find(OccurrenceId{2});
    CHECK(source && copy && source != copy);
    CHECK(copy->targetDefinitionId() == source->targetDefinitionId());
    CHECK(copy->placement() == source->placement());
    CHECK(copy->homePlacement() == source->homePlacement());
    CHECK(copy->flags() == source->flags());
    CHECK(copy->displayNameUtf8() == source->displayNameUtf8());
    CHECK(product->definitions().findPart(DefinitionId{2}) == partBefore);
    CHECK(&product->definitions().findPart(DefinitionId{2})->document() ==
          documentBefore);
    CHECK(sourceShape.lock() == shapeBefore);
    const auto applied = productSnapshot(*product, DefinitionId{2});

    auto accept = [](const ProductDocument&) { return true; };
    CallableSuccessor acceptSuccessor(accept);
    CHECK(product->extractDuplicateOccurrence(
              duplicate, acceptSuccessor,
              DuplicateOccurrenceFailurePoint::NodeExtract) ==
          DuplicateOccurrenceStatus::NodeFailure);
    CHECK(productSnapshot(*product, DefinitionId{2}) == applied);
    CHECK(product->extractDuplicateOccurrence(
              duplicate, acceptSuccessor,
              DuplicateOccurrenceFailurePoint::None) ==
          DuplicateOccurrenceStatus::Extracted);
    CHECK(productSnapshot(*product, DefinitionId{2}) == before);

    for (const auto failure : {
             DuplicateOccurrenceFailurePoint::OccurrenceConstruction,
             DuplicateOccurrenceFailurePoint::NodeAllocation,
             DuplicateOccurrenceFailurePoint::NodeCommit}) {
      const auto status = product->applyDuplicateOccurrence(
          duplicate, acceptSuccessor, failure);
      CHECK(status ==
            (failure == DuplicateOccurrenceFailurePoint::NodeCommit
                 ? DuplicateOccurrenceStatus::NodeFailure
                 : DuplicateOccurrenceStatus::AllocationFailure));
      CHECK(productSnapshot(*product, DefinitionId{2}) == before);
    }
    ResourceFailureSuccessor resourceFailure;
    CHECK(product->applyDuplicateOccurrence(
              duplicate, resourceFailure,
              DuplicateOccurrenceFailurePoint::None) ==
          DuplicateOccurrenceStatus::SuccessorResourceFailure);
    CHECK(productSnapshot(*product, DefinitionId{2}) == before);

    auto invalidId = duplicate;
    invalidId.duplicateId = OccurrenceId{};
    CHECK(product->applyDuplicateOccurrence(
              invalidId, acceptSuccessor,
              DuplicateOccurrenceFailurePoint::None) ==
          DuplicateOccurrenceStatus::InvalidPayload);
    invalidId.duplicateId = OccurrenceId{kMaximumRootOccurrences + 1};
    CHECK(product->applyDuplicateOccurrence(
              invalidId, acceptSuccessor,
              DuplicateOccurrenceFailurePoint::None) ==
          DuplicateOccurrenceStatus::InvalidPayload);
    invalidId = duplicate;
    invalidId.duplicateId = OccurrenceId{3};
    CHECK(product->applyDuplicateOccurrence(
              invalidId, acceptSuccessor,
              DuplicateOccurrenceFailurePoint::None) ==
          DuplicateOccurrenceStatus::InvalidPayload);
    CHECK(product->extractDuplicateOccurrence(
              invalidId, acceptSuccessor,
              DuplicateOccurrenceFailurePoint::None) ==
          DuplicateOccurrenceStatus::InvalidPayload);
    CHECK(productSnapshot(*product, DefinitionId{2}) == before);
    auto collision = duplicate;
    collision.duplicateId = OccurrenceId{1};
    CHECK(product->applyDuplicateOccurrence(
              collision, acceptSuccessor,
              DuplicateOccurrenceFailurePoint::None) ==
          DuplicateOccurrenceStatus::DuplicateIdCollision);
    auto stale = duplicate;
    stale.source.productIncarnation = ProductIncarnationId{999999};
    CHECK(product->applyDuplicateOccurrence(
              stale, acceptSuccessor,
              DuplicateOccurrenceFailurePoint::None) ==
          DuplicateOccurrenceStatus::ProductIncarnationMismatch);
    auto wrongTarget = duplicate;
    wrongTarget.targetDefinitionId = DefinitionId{99};
    CHECK(product->applyDuplicateOccurrence(
              wrongTarget, acceptSuccessor,
              DuplicateOccurrenceFailurePoint::None) ==
          DuplicateOccurrenceStatus::TargetDefinitionMismatch);
    auto wrongSnapshot = duplicate;
    wrongSnapshot.flags.visible = true;
    CHECK(product->applyDuplicateOccurrence(
              wrongSnapshot, acceptSuccessor,
              DuplicateOccurrenceFailurePoint::None) ==
          DuplicateOccurrenceStatus::SourceSnapshotMismatch);
    CHECK(productSnapshot(*product, DefinitionId{2}) == before);

    AssemblyCommandHistory history;
    const auto duplicateSemanticProductBefore =
        productSnapshot(*product, DefinitionId{2});
    const auto duplicateSemanticHistoryBefore = snapshot(history, *product);
    FixedStatusSuccessor duplicateSemanticSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    detail::AssemblyCommandHistoryTestAccess::inject(
        history,
        detail::AssemblyCommandHistoryFailurePoint::SemanticStatePreparation,
        detail::AssemblyCommandHistoryFailureKind::BadAlloc);
    CHECK(history.executeDuplicate(
              *product, {duplicate, {path({3})}, {path({2})}},
              duplicateSemanticSuccessor)
              .status == AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(duplicateSemanticSuccessor.calls == 0);
    CHECK(productSnapshot(*product, DefinitionId{2}) ==
          duplicateSemanticProductBefore);
    CHECK(snapshot(history, *product) == duplicateSemanticHistoryBefore);
    detail::AssemblyCommandHistoryTestAccess::clearInjection(history);
    DuplicateOccurrenceColdCommand command{
        duplicate, {path({3})}, {path({2})}};
    const auto executed = history.executeDuplicate(
        *product, std::move(command), acceptSuccessor);
    CHECK(executed.status == AssemblyCommandHistoryStatus::Committed);
    CHECK(executed.restoredState ==
          AssemblyCommandSemanticState{path({2})});
    CHECK(history.size() == 1 && history.cursor() == 1 &&
          history.currentOccurrence() == path({2}));
    CHECK(history.retainedBytes() ==
          (duplicate.source.path.entries().capacity() +
           path({3}).entries().capacity() + path({2}).entries().capacity()) *
                  sizeof(OccurrenceId) +
              duplicate.displayNameUtf8.capacity());
    const auto historyApplied = productSnapshot(*product, DefinitionId{2});

    ResourceFailureSuccessor undoResourceFailure;
    const auto beforeFailedUndo = snapshot(history, *product);
    CHECK(history.undo(*product, undoResourceFailure).status ==
          AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(snapshot(history, *product) == beforeFailedUndo);
    CHECK(productSnapshot(*product, DefinitionId{2}) == historyApplied);

    CHECK(history.undo(*product, acceptSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(history.currentOccurrence() == std::nullopt);
    CHECK(productSnapshot(*product, DefinitionId{2}) == before);
    ResourceFailureSuccessor redoResourceFailure;
    const auto beforeFailedRedo = snapshot(history, *product);
    CHECK(history.redo(*product, redoResourceFailure).status ==
          AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(snapshot(history, *product) == beforeFailedRedo);
    CHECK(productSnapshot(*product, DefinitionId{2}) == before);
    CHECK(history.redo(*product, acceptSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(sameProductSemantics(
        productSnapshot(*product, DefinitionId{2}), historyApplied));

    // Duplicate-of-duplicate remains a reference to the same exact Part.
    const auto secondId = product->lowestFreeRootOccurrenceId();
    CHECK(secondId == OccurrenceId{4});
    auto secondPayload =
        duplicatePayload(*product, OccurrenceId{2}, *secondId);
    CHECK(history.executeDuplicate(
              *product,
              {secondPayload, {path({2})}, {path({secondId->value()})}},
              acceptSuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    const auto* second = product->rootAssemblyDefinition()->find(*secondId);
    CHECK(second && second->targetDefinitionId() == DefinitionId{2});
    CHECK(&product->definitions().findPart(DefinitionId{2})->document() ==
          documentBefore);
    auto renamedSecond = OccurrenceDelta::rename(
        address(*product, *secondId), "Копируемый 🔩", "Вторая копия");
    CHECK(renamedSecond);
    CHECK(history.executeOccurrenceDelta(
              *product,
              {std::move(*renamedSecond.delta), {path({secondId->value()})},
               {path({secondId->value()})}},
              acceptSuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(history.undo(*product, acceptSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(history.undo(*product, acceptSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(!product->rootAssemblyDefinition()->find(*secondId));

    // Finalization failure happens after successor preparation but still
    // rolls the structural node back and preserves the redo tail/cursor.
    CHECK(history.undo(*product, acceptSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    const auto beforeFinalization = productSnapshot(*product, DefinitionId{2});
    const auto historyBeforeFinalization = snapshot(history, *product);
    detail::AssemblyCommandHistoryTestAccess::inject(
        history, detail::AssemblyCommandHistoryFailurePoint::Finalization,
        detail::AssemblyCommandHistoryFailureKind::BadAlloc);
    CHECK(history.executeDuplicate(
              *product,
              {duplicate, {path({3})}, {path({2})}},
              acceptSuccessor)
              .status == AssemblyCommandHistoryStatus::ResourceLimit);
    detail::AssemblyCommandHistoryTestAccess::clearInjection(history);
    CHECK(productSnapshot(*product, DefinitionId{2}) == beforeFinalization);
    CHECK(snapshot(history, *product) == historyBeforeFinalization);
  }

  // A5c6 Remove owns no live node: definitions/Documents/B-Rep remain exact,
  // failures reinsert the extracted map node allocation-free, and Undo stages
  // a fresh exact node before touching the live root.
  {
    const auto placed = RigidTransform3d::create(
        {-8.0, 13.0, 21.0}, {0.9238795325, 0.3826834323, 0.0, 0.0});
    CHECK(placed);
    std::weak_ptr<const TopoDS_Shape> sourceShape;
    DefinitionRegistry definitions;
    CHECK(definitions.insert(
              PartDefinition(DefinitionId{2}, part(&sourceShape))) ==
          DefinitionInsertStatus::Inserted);
    CHECK(definitions.insert(
              PartDefinition(DefinitionId{4}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
    AssemblyDefinition root(DefinitionId{1});
    CHECK(root.insert(ComponentOccurrence(
              OccurrenceId{1}, DefinitionId{2},
              RigidTransform3d::identity())) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(root.insert(ComponentOccurrence(
              OccurrenceId{3}, DefinitionId{2}, *placed.transform,
              {.visible = false, .suppressed = true, .grounded = true},
              "Удаляемый 🔩")) == AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(definitions.insert(std::move(root)) ==
          DefinitionInsertStatus::Inserted);
    auto made = ProductDocument::create(std::move(definitions), DefinitionId{1});
    CHECK(made);
    auto product = std::move(made.product);
    const auto payload = removePayload(*product, OccurrenceId{3});
    const auto before = productSnapshot(*product, DefinitionId{2});
    const auto* partBefore = product->definitions().findPart(DefinitionId{2});
    CHECK(partBefore);
    const Document* documentBefore = &partBefore->document();
    const auto shapeBefore = sourceShape.lock();
    CHECK(shapeBefore);

    auto accept = [](const ProductDocument&) { return true; };
    CallableSuccessor acceptSuccessor(accept);

    // Every constructible public rejection is table-driven against an exact
    // product/history snapshot. The history deliberately owns a redo tail so
    // these direct model failures also prove cursor/revision/savepoint/redo
    // preservation. Pre-validation failures must not invoke the successor.
    AssemblyCommandHistory guardHistory;
    auto seedRename = OccurrenceDelta::rename(
        address(*product, OccurrenceId{3}), "Удаляемый 🔩", "guard");
    CHECK(seedRename);
    CHECK(guardHistory.executeOccurrenceDelta(
              *product,
              {std::move(*seedRename.delta), {path({3})},
               {path({3})}},
              acceptSuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(guardHistory.undo(*product, acceptSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(guardHistory.canRedo());

    const auto checkApplyFailure =
        [&](const RemoveOccurrenceColdPayload& candidate,
            RemoveOccurrenceStatus expected,
            RemoveOccurrenceFailurePoint failurePoint,
            ProductDocumentSuccessorStatus successorStatus,
            int expectedSuccessorCalls) {
          FixedStatusSuccessor successor(successorStatus);
          const auto productBefore = productSnapshot(*product, DefinitionId{2});
          const auto historyBefore = snapshot(guardHistory, *product);
          CHECK(product->applyRemoveOccurrence(candidate, successor,
                                               failurePoint) == expected);
          CHECK(successor.calls == expectedSuccessorCalls);
          CHECK(productSnapshot(*product, DefinitionId{2}) == productBefore);
          CHECK(snapshot(guardHistory, *product) == historyBefore);
        };

    struct ApplyGuardCase final {
      RemoveOccurrenceColdPayload payload;
      RemoveOccurrenceStatus expected;
    };
    std::vector<ApplyGuardCase> applyGuards;
    auto invalidPayload = payload;
    invalidPayload.displayNameUtf8 = "bad\nname";
    applyGuards.push_back(
        {std::move(invalidPayload), RemoveOccurrenceStatus::InvalidPayload});
    auto staleIncarnationGuard = payload;
    staleIncarnationGuard.occurrence.productIncarnation =
        ProductIncarnationId{999999};
    applyGuards.push_back({std::move(staleIncarnationGuard),
                           RemoveOccurrenceStatus::ProductIncarnationMismatch});
    auto wrongRootGuard = payload;
    wrongRootGuard.occurrence.rootDefinitionId = DefinitionId{77};
    applyGuards.push_back({std::move(wrongRootGuard),
                           RemoveOccurrenceStatus::RootDefinitionMismatch});
    auto nestedGuard = payload;
    nestedGuard.occurrence.path = path({3, 4});
    applyGuards.push_back({std::move(nestedGuard),
                           RemoveOccurrenceStatus::UnsupportedPathDepth});
    auto missingGuard = payload;
    missingGuard.occurrence.path = path({99});
    applyGuards.push_back({std::move(missingGuard),
                           RemoveOccurrenceStatus::OccurrenceMissing});
    auto invalidTargetGuard = payload;
    invalidTargetGuard.occurrence.expectedTargetDefinitionId = DefinitionId{};
    applyGuards.push_back({std::move(invalidTargetGuard),
                           RemoveOccurrenceStatus::TargetDefinitionMismatch});
    auto wrongExistingTargetGuard = payload;
    wrongExistingTargetGuard.occurrence.expectedTargetDefinitionId =
        DefinitionId{4};
    applyGuards.push_back({std::move(wrongExistingTargetGuard),
                           RemoveOccurrenceStatus::TargetDefinitionMismatch});
    auto missingTargetGuard = payload;
    missingTargetGuard.occurrence.expectedTargetDefinitionId = DefinitionId{99};
    applyGuards.push_back({std::move(missingTargetGuard),
                           RemoveOccurrenceStatus::TargetPartMissing});
    auto snapshotGuard = payload;
    snapshotGuard.flags.visible = true;
    applyGuards.push_back({std::move(snapshotGuard),
                           RemoveOccurrenceStatus::SnapshotMismatch});
    for (const auto& guard : applyGuards) {
      checkApplyFailure(guard.payload, guard.expected,
                        RemoveOccurrenceFailurePoint::None,
                        ProductDocumentSuccessorStatus::Prepared, 0);
    }
    checkApplyFailure(payload, RemoveOccurrenceStatus::NodeFailure,
                      RemoveOccurrenceFailurePoint::NodeExtract,
                      ProductDocumentSuccessorStatus::Prepared, 0);
    checkApplyFailure(payload, RemoveOccurrenceStatus::SuccessorRejected,
                      RemoveOccurrenceFailurePoint::None,
                      ProductDocumentSuccessorStatus::Rejected, 1);
    checkApplyFailure(payload,
                      RemoveOccurrenceStatus::SuccessorResourceFailure,
                      RemoveOccurrenceFailurePoint::None,
                      ProductDocumentSuccessorStatus::ResourceFailure, 1);

    class BusyProbeSuccessor final : public ProductDocumentSuccessor {
     public:
      BusyProbeSuccessor(ProductDocument& product,
                         const RemoveOccurrenceColdPayload& payload) noexcept
          : product_(product), payload_(payload) {}

      ProductDocumentSuccessorStatus prepare(
          const ProductDocument&,
          ProductDocumentSuccessorImpact) noexcept override {
        ++calls;
        nestedApply = product_.applyRemoveOccurrence(
            payload_, *this, RemoveOccurrenceFailurePoint::None);
        nestedRestore = product_.restoreRemovedOccurrence(
            payload_, *this, RemoveOccurrenceFailurePoint::None);
        return ProductDocumentSuccessorStatus::Rejected;
      }

      int calls{};
      RemoveOccurrenceStatus nestedApply{RemoveOccurrenceStatus::Removed};
      RemoveOccurrenceStatus nestedRestore{RemoveOccurrenceStatus::Restored};

     private:
      ProductDocument& product_;
      const RemoveOccurrenceColdPayload& payload_;
    } busyProbe(*product, payload);
    const auto busyProductBefore = productSnapshot(*product, DefinitionId{2});
    const auto busyHistoryBefore = snapshot(guardHistory, *product);
    CHECK(product->applyRemoveOccurrence(
              payload, busyProbe, RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::SuccessorRejected);
    CHECK(busyProbe.calls == 1);
    CHECK(busyProbe.nestedApply == RemoveOccurrenceStatus::Busy);
    CHECK(busyProbe.nestedRestore == RemoveOccurrenceStatus::Busy);
    CHECK(productSnapshot(*product, DefinitionId{2}) == busyProductBefore);
    CHECK(snapshot(guardHistory, *product) == busyHistoryBefore);

    // A Part root reaches RootIsNotAssembly before target lookup and leaves
    // both exact runtime ownership and the untouched history invariant.
    DefinitionRegistry partRootDefinitions;
    CHECK(partRootDefinitions.insert(
              PartDefinition(DefinitionId{1}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
    auto partRoot = ProductDocument::create(std::move(partRootDefinitions),
                                            DefinitionId{1});
    CHECK(partRoot);
    RemoveOccurrenceColdPayload partRootPayload{
        {partRoot.product->incarnation(), DefinitionId{1}, path({3}),
         DefinitionId{2}},
        RigidTransform3d::identity(), {}, {}};
    AssemblyCommandHistory partRootHistory;
    FixedStatusSuccessor partRootSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    const auto partRootBefore = productSnapshot(*partRoot.product, DefinitionId{1});
    const auto partRootHistoryBefore =
        snapshot(partRootHistory, *partRoot.product);
    CHECK(partRoot.product->applyRemoveOccurrence(
              partRootPayload, partRootSuccessor,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::RootIsNotAssembly);
    CHECK(partRoot.product->restoreRemovedOccurrence(
              partRootPayload, partRootSuccessor,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::RootIsNotAssembly);
    CHECK(partRootSuccessor.calls == 0);
    CHECK(productSnapshot(*partRoot.product, DefinitionId{1}) ==
          partRootBefore);
    CHECK(snapshot(partRootHistory, *partRoot.product) ==
          partRootHistoryBefore);
    ResourceFailureSuccessor resourceFailure;
    CHECK(product->applyRemoveOccurrence(
              payload, acceptSuccessor,
              RemoveOccurrenceFailurePoint::NodeExtract) ==
          RemoveOccurrenceStatus::NodeFailure);
    CHECK(productSnapshot(*product, DefinitionId{2}) == before);
    CHECK(product->applyRemoveOccurrence(
              payload, resourceFailure,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::SuccessorResourceFailure);
    CHECK(productSnapshot(*product, DefinitionId{2}) == before);

    ProductDocumentSuccessorImpact impact =
        ProductDocumentSuccessorImpact::FullProjection;
    RemoveOccurrenceStatus reentrantRestore =
        RemoveOccurrenceStatus::Restored;
    RemoveOccurrenceStatus reentrantRemove =
        RemoveOccurrenceStatus::Removed;
    ProductOccurrenceMutationStatus reentrantInsert =
        ProductOccurrenceMutationStatus::Committed;
    class RemoveSuccessor final : public ProductDocumentSuccessor {
     public:
      RemoveSuccessor(ProductDocument& product,
                      const RemoveOccurrenceColdPayload& payload,
                      ProductDocumentSuccessorImpact& impact,
                      RemoveOccurrenceStatus& remove,
                      RemoveOccurrenceStatus& restore,
                      ProductOccurrenceMutationStatus& insert) noexcept
          : product_(product), payload_(payload), impact_(impact),
            remove_(remove), restore_(restore), insert_(insert) {}

      ProductDocumentSuccessorStatus prepare(
          const ProductDocument& candidate,
          ProductDocumentSuccessorImpact impact) noexcept override {
        ++calls;
        impact_ = impact;
        if (candidate.rootAssemblyDefinition()->find(OccurrenceId{3}) ||
            !candidate.definitions().findPart(DefinitionId{2}))
          return ProductDocumentSuccessorStatus::Rejected;
        restore_ = product_.restoreRemovedOccurrence(
            payload_, *this, RemoveOccurrenceFailurePoint::None);
        remove_ = product_.applyRemoveOccurrence(
            payload_, *this, RemoveOccurrenceFailurePoint::None);
        insert_ = product_.insertRootOccurrence(ComponentOccurrence(
            OccurrenceId{9}, DefinitionId{2}, RigidTransform3d::identity()));
        return remove_ == RemoveOccurrenceStatus::Busy &&
                       restore_ == RemoveOccurrenceStatus::Busy &&
                       insert_ == ProductOccurrenceMutationStatus::Busy
                   ? ProductDocumentSuccessorStatus::Prepared
                   : ProductDocumentSuccessorStatus::Rejected;
      }

     private:
      ProductDocument& product_;
      const RemoveOccurrenceColdPayload& payload_;
      ProductDocumentSuccessorImpact& impact_;
      RemoveOccurrenceStatus& remove_;
      RemoveOccurrenceStatus& restore_;
      ProductOccurrenceMutationStatus& insert_;

     public:
      int calls{};
    } removeSuccessor(*product, payload, impact, reentrantRemove,
                      reentrantRestore,
                      reentrantInsert);
    const auto guardBeforeSuccessfulRemove = snapshot(guardHistory, *product);
    CHECK(product->applyRemoveOccurrence(
              payload, removeSuccessor,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::Removed);
    CHECK(removeSuccessor.calls == 1);
    CHECK(impact == ProductDocumentSuccessorImpact::RemoveProjection);
    CHECK(reentrantRemove == RemoveOccurrenceStatus::Busy);
    CHECK(reentrantRestore == RemoveOccurrenceStatus::Busy);
    CHECK(reentrantInsert == ProductOccurrenceMutationStatus::Busy);
    CHECK(!product->rootAssemblyDefinition()->find(OccurrenceId{3}));
    CHECK(product->rootAssemblyDefinition()->size() == 1);
    CHECK(product->definitions().findPart(DefinitionId{2}) == partBefore);
    CHECK(&partBefore->document() == documentBefore);
    CHECK(sourceShape.lock() == shapeBefore);
    const auto removed = productSnapshot(*product, DefinitionId{2});
    auto expectedRemoved = before;
    expectedRemoved.occurrences.erase(
        std::remove_if(expectedRemoved.occurrences.begin(),
                       expectedRemoved.occurrences.end(),
                       [](const auto& occurrence) {
                         return occurrence.id == OccurrenceId{3};
                       }),
        expectedRemoved.occurrences.end());
    CHECK(removed == expectedRemoved);
    auto expectedGuardAfterRemove = guardBeforeSuccessfulRemove;
    expectedGuardAfterRemove.occurrences = 1;
    CHECK(snapshot(guardHistory, *product) == expectedGuardAfterRemove);

    const auto checkRestoreFailure =
        [&](const RemoveOccurrenceColdPayload& candidate,
            RemoveOccurrenceStatus expected,
            RemoveOccurrenceFailurePoint failurePoint,
            ProductDocumentSuccessorStatus successorStatus,
            int expectedSuccessorCalls) {
          FixedStatusSuccessor successor(successorStatus);
          const auto productBefore = productSnapshot(*product, DefinitionId{2});
          const auto historyBefore = snapshot(guardHistory, *product);
          CHECK(product->restoreRemovedOccurrence(candidate, successor,
                                                  failurePoint) == expected);
          CHECK(successor.calls == expectedSuccessorCalls);
          CHECK(productSnapshot(*product, DefinitionId{2}) == productBefore);
          CHECK(snapshot(guardHistory, *product) == historyBefore);
        };
    // OccurrenceMissing and SnapshotMismatch are Apply-only: Restore has no
    // live node to inspect. Conversely collision and limit are Restore-only;
    // Apply starts from the exact live node captured by its cold payload.
    struct RestoreGuardCase final {
      RemoveOccurrenceColdPayload payload;
      RemoveOccurrenceStatus expected;
    };
    std::vector<RestoreGuardCase> restoreGuards;
    auto restoreInvalidPayload = payload;
    restoreInvalidPayload.displayNameUtf8 = "bad\rname";
    restoreGuards.push_back({std::move(restoreInvalidPayload),
                             RemoveOccurrenceStatus::InvalidPayload});
    auto restoreStaleIncarnation = payload;
    restoreStaleIncarnation.occurrence.productIncarnation =
        ProductIncarnationId{999999};
    restoreGuards.push_back(
        {std::move(restoreStaleIncarnation),
         RemoveOccurrenceStatus::ProductIncarnationMismatch});
    auto restoreWrongRoot = payload;
    restoreWrongRoot.occurrence.rootDefinitionId = DefinitionId{77};
    restoreGuards.push_back({std::move(restoreWrongRoot),
                             RemoveOccurrenceStatus::RootDefinitionMismatch});
    auto restoreNested = payload;
    restoreNested.occurrence.path = path({3, 4});
    restoreGuards.push_back({std::move(restoreNested),
                             RemoveOccurrenceStatus::UnsupportedPathDepth});
    auto restoreInvalidTarget = payload;
    restoreInvalidTarget.occurrence.expectedTargetDefinitionId = DefinitionId{};
    restoreGuards.push_back({std::move(restoreInvalidTarget),
                             RemoveOccurrenceStatus::TargetDefinitionMismatch});
    auto restoreMissingTarget = payload;
    restoreMissingTarget.occurrence.expectedTargetDefinitionId =
        DefinitionId{99};
    restoreGuards.push_back({std::move(restoreMissingTarget),
                             RemoveOccurrenceStatus::TargetPartMissing});
    for (const auto& guard : restoreGuards) {
      checkRestoreFailure(guard.payload, guard.expected,
                          RemoveOccurrenceFailurePoint::None,
                          ProductDocumentSuccessorStatus::Prepared, 0);
    }
    checkRestoreFailure(payload, RemoveOccurrenceStatus::AllocationFailure,
                        RemoveOccurrenceFailurePoint::OccurrenceConstruction,
                        ProductDocumentSuccessorStatus::Prepared, 0);
    checkRestoreFailure(payload, RemoveOccurrenceStatus::AllocationFailure,
                        RemoveOccurrenceFailurePoint::NodeAllocation,
                        ProductDocumentSuccessorStatus::Prepared, 0);
    checkRestoreFailure(payload, RemoveOccurrenceStatus::NodeFailure,
                        RemoveOccurrenceFailurePoint::NodeCommit,
                        ProductDocumentSuccessorStatus::Prepared, 0);
    checkRestoreFailure(payload, RemoveOccurrenceStatus::SuccessorRejected,
                        RemoveOccurrenceFailurePoint::None,
                        ProductDocumentSuccessorStatus::Rejected, 1);
    checkRestoreFailure(payload,
                        RemoveOccurrenceStatus::SuccessorResourceFailure,
                        RemoveOccurrenceFailurePoint::None,
                        ProductDocumentSuccessorStatus::ResourceFailure, 1);

    for (const auto failure : {
             RemoveOccurrenceFailurePoint::OccurrenceConstruction,
             RemoveOccurrenceFailurePoint::NodeAllocation,
             RemoveOccurrenceFailurePoint::NodeCommit}) {
      const auto status = product->restoreRemovedOccurrence(
          payload, acceptSuccessor, failure);
      CHECK(status ==
            (failure == RemoveOccurrenceFailurePoint::NodeCommit
                 ? RemoveOccurrenceStatus::NodeFailure
                 : RemoveOccurrenceStatus::AllocationFailure));
      CHECK(productSnapshot(*product, DefinitionId{2}) == removed);
    }
    CHECK(product->restoreRemovedOccurrence(
              payload, resourceFailure,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::SuccessorResourceFailure);
    CHECK(productSnapshot(*product, DefinitionId{2}) == removed);
    FixedStatusSuccessor restoreSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    const auto guardBeforeSuccessfulRestore = snapshot(guardHistory, *product);
    CHECK(product->restoreRemovedOccurrence(
              payload, restoreSuccessor,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::Restored);
    CHECK(restoreSuccessor.calls == 1);
    CHECK(restoreSuccessor.lastImpact ==
          ProductDocumentSuccessorImpact::RemoveProjection);
    const auto restoredSnapshot = productSnapshot(*product, DefinitionId{2});
    auto expectedRestored = before;
    const auto actualRestoredNode = std::find_if(
        restoredSnapshot.occurrences.begin(), restoredSnapshot.occurrences.end(),
        [](const auto& occurrence) {
          return occurrence.id == OccurrenceId{3};
        });
    const auto expectedRestoredNode = std::find_if(
        expectedRestored.occurrences.begin(), expectedRestored.occurrences.end(),
        [](const auto& occurrence) {
          return occurrence.id == OccurrenceId{3};
        });
    CHECK(actualRestoredNode != restoredSnapshot.occurrences.end());
    CHECK(expectedRestoredNode != expectedRestored.occurrences.end());
    expectedRestoredNode->address = actualRestoredNode->address;
    CHECK(restoredSnapshot == expectedRestored);
    auto expectedGuardAfterRestore = guardBeforeSuccessfulRestore;
    expectedGuardAfterRestore.occurrences = 2;
    CHECK(snapshot(guardHistory, *product) == expectedGuardAfterRestore);
    FixedStatusSuccessor collisionSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    const auto collisionBefore = productSnapshot(*product, DefinitionId{2});
    const auto collisionHistoryBefore = snapshot(guardHistory, *product);
    CHECK(product->restoreRemovedOccurrence(
              payload, collisionSuccessor,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::OccurrenceIdCollision);
    CHECK(collisionSuccessor.calls == 0);
    CHECK(productSnapshot(*product, DefinitionId{2}) == collisionBefore);
    CHECK(snapshot(guardHistory, *product) == collisionHistoryBefore);

    auto stale = payload;
    stale.occurrence.productIncarnation = ProductIncarnationId{999999};
    CHECK(product->applyRemoveOccurrence(
              stale, acceptSuccessor,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::ProductIncarnationMismatch);
    auto wrongTarget = payload;
    wrongTarget.occurrence.expectedTargetDefinitionId = DefinitionId{99};
    CHECK(product->applyRemoveOccurrence(
              wrongTarget, acceptSuccessor,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::TargetPartMissing);
    auto wrongSnapshot = payload;
    wrongSnapshot.flags.visible = true;
    CHECK(product->applyRemoveOccurrence(
              wrongSnapshot, acceptSuccessor,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::SnapshotMismatch);
    CHECK(sameProductSemantics(productSnapshot(*product, DefinitionId{2}),
                               before));

    AssemblyCommandHistory history;
    const auto removeSemanticProductBefore =
        productSnapshot(*product, DefinitionId{2});
    const auto removeSemanticHistoryBefore = snapshot(history, *product);
    FixedStatusSuccessor removeSemanticSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    detail::AssemblyCommandHistoryTestAccess::inject(
        history,
        detail::AssemblyCommandHistoryFailurePoint::SemanticStatePreparation,
        detail::AssemblyCommandHistoryFailureKind::BadAlloc);
    CHECK(history.executeRemove(*product, {payload, {path({3})}, {}},
                                removeSemanticSuccessor)
              .status == AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(removeSemanticSuccessor.calls == 0);
    CHECK(productSnapshot(*product, DefinitionId{2}) ==
          removeSemanticProductBefore);
    CHECK(snapshot(history, *product) == removeSemanticHistoryBefore);
    detail::AssemblyCommandHistoryTestAccess::clearInjection(history);
    RemoveOccurrenceColdCommand command{
        payload, {path({3})}, {}};
    FixedStatusSuccessor executeRemoveSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    const auto executed = history.executeRemove(
        *product, std::move(command), executeRemoveSuccessor);
    CHECK(executed.status == AssemblyCommandHistoryStatus::Committed);
    CHECK(executeRemoveSuccessor.calls == 1);
    CHECK(executeRemoveSuccessor.lastImpact ==
          ProductDocumentSuccessorImpact::RemoveProjection);
    CHECK(executed.restoredState == AssemblyCommandSemanticState{});
    CHECK(history.size() == 1 && history.cursor() == 1 &&
          !history.currentOccurrence());
    CHECK(history.retainedBytes() ==
          (payload.occurrence.path.entries().capacity() +
           path({3}).entries().capacity()) * sizeof(OccurrenceId) +
              payload.displayNameUtf8.capacity());
    CHECK(sameProductSemantics(productSnapshot(*product, DefinitionId{2}),
                               removed));

    const auto historyRemoved = productSnapshot(*product, DefinitionId{2});
    const auto beforeFailedUndo = snapshot(history, *product);
    CHECK(history.undo(*product, resourceFailure).status ==
          AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(snapshot(history, *product) == beforeFailedUndo);
    CHECK(productSnapshot(*product, DefinitionId{2}) == historyRemoved);

    // Reusing the freed ID blocks Undo exactly; history/cursor remain at the
    // remove command until the collision is removed.
    CHECK(product->insertRootOccurrence(ComponentOccurrence(
              OccurrenceId{3}, DefinitionId{2},
              RigidTransform3d::identity())) ==
          ProductOccurrenceMutationStatus::Committed);
    const auto collisionHistory = snapshot(history, *product);
    const auto collisionProduct = productSnapshot(*product, DefinitionId{2});
    CHECK(history.undo(*product, acceptSuccessor).status ==
          AssemblyCommandHistoryStatus::ModelRejected);
    CHECK(snapshot(history, *product) == collisionHistory);
    CHECK(productSnapshot(*product, DefinitionId{2}) == collisionProduct);
    CHECK(product->removeRootOccurrence(path({3})) ==
          ProductOccurrenceMutationStatus::Committed);

    FixedStatusSuccessor undoRemoveSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    const auto restoredResult = history.undo(*product, undoRemoveSuccessor);
    CHECK(restoredResult.status == AssemblyCommandHistoryStatus::Committed);
    CHECK(undoRemoveSuccessor.calls == 1);
    CHECK(undoRemoveSuccessor.lastImpact ==
          ProductDocumentSuccessorImpact::RemoveProjection);
    CHECK(restoredResult.restoredState ==
          AssemblyCommandSemanticState{path({3})});
    const auto* restored =
        product->rootAssemblyDefinition()->find(OccurrenceId{3});
    CHECK(restored && restored->targetDefinitionId() == DefinitionId{2} &&
          restored->placement() == payload.placement &&
          restored->flags() == payload.flags &&
          restored->displayNameUtf8() == payload.displayNameUtf8);
    CHECK(&product->definitions().findPart(DefinitionId{2})->document() ==
          documentBefore);
    CHECK(sourceShape.lock() == shapeBefore);

    const auto beforeFailedRedo = snapshot(history, *product);
    CHECK(history.redo(*product, resourceFailure).status ==
          AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(snapshot(history, *product) == beforeFailedRedo);
    FixedStatusSuccessor redoRemoveSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.redo(*product, redoRemoveSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(redoRemoveSuccessor.calls == 1);
    CHECK(redoRemoveSuccessor.lastImpact ==
          ProductDocumentSuccessorImpact::RemoveProjection);
    CHECK(!product->rootAssemblyDefinition()->find(OccurrenceId{3}));

    // Removing the final authored occurrence leaves a valid empty root and
    // the exact orphan Part/Document/B-Rep allocation intact.
    const auto finalPayload = removePayload(*product, OccurrenceId{1});
    CHECK(product->applyRemoveOccurrence(
              finalPayload, acceptSuccessor,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::Removed);
    CHECK(product->rootAssemblyDefinition()->size() == 0);
    CHECK(product->rootDefinitionKind() == DefinitionKind::Assembly);
    CHECK(product->definitions().size() == 3);
    CHECK(product->definitions().findPart(DefinitionId{2}) == partBefore);
    CHECK(&partBefore->document() == documentBefore);
    CHECK(sourceShape.lock() == shapeBefore);
    CHECK(product->restoreRemovedOccurrence(
              finalPayload, acceptSuccessor,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::Restored);
  }

  // One real unified command sequence crosses all A5b/A5c1-6 boundaries:
  // Insert -> Duplicate -> Rename -> Visibility -> Ground -> Suppress ->
  // Remove. Full Undo/Redo proves stable IDs, exact field restoration,
  // semantic selection, and that the shared definition is orphaned only when
  // the Insert itself is undone.
  {
    auto chainProduct = emptyAssembly();
    auto chainDocument = part();
    const auto chainBytes = cold(*chainDocument);
    EmbeddedPartInsertPayload chainInsert{
        DefinitionId{2}, OccurrenceId{1}, RigidTransform3d::identity(), {},
        std::move(chainDocument)};
    AssemblyCommandHistory chainHistory;
    std::vector<AssemblyCommandRevision> chainRevisions;
    auto acceptChain = [](const ProductDocument&) { return true; };
    CallableSuccessor chainSuccessor(acceptChain);
    CHECK(chainHistory.prepareRecord(insertCommand(
              DefinitionId{2}, OccurrenceId{1}, chainBytes, {},
              {path({1})})) ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(chainProduct->applyEmbeddedPartInsert(chainInsert, chainSuccessor) ==
          EmbeddedPartInsertStatus::Committed);
    CHECK(chainHistory.commitPrepared() ==
          AssemblyCommandHistoryStatus::Committed);
    chainRevisions.push_back(chainHistory.currentRevision());

    const auto chainDuplicate =
        duplicatePayload(*chainProduct, OccurrenceId{1}, OccurrenceId{2});
    CHECK(chainHistory.executeDuplicate(
              *chainProduct,
              {chainDuplicate, {path({1})}, {path({2})}},
              chainSuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    chainRevisions.push_back(chainHistory.currentRevision());
    const AssemblyCommandSemanticState duplicateSelected{path({2})};
    auto chainRename = OccurrenceDelta::rename(
        address(*chainProduct, OccurrenceId{2}), {}, "Цепочка 🔩");
    CHECK(chainRename);
    CHECK(chainHistory.executeOccurrenceDelta(
              *chainProduct,
              {std::move(*chainRename.delta), duplicateSelected,
               duplicateSelected},
              chainSuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    chainRevisions.push_back(chainHistory.currentRevision());
    CHECK(chainHistory.executeOccurrenceDelta(
              *chainProduct,
              {OccurrenceDelta::visibility(
                   address(*chainProduct, OccurrenceId{2}), true, false),
               duplicateSelected, duplicateSelected},
              chainSuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    chainRevisions.push_back(chainHistory.currentRevision());
    CHECK(chainHistory.executeOccurrenceDelta(
              *chainProduct,
              {OccurrenceDelta::grounded(
                   address(*chainProduct, OccurrenceId{2}), false, true),
               duplicateSelected, duplicateSelected},
              chainSuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    chainRevisions.push_back(chainHistory.currentRevision());
    CHECK(chainHistory.executeOccurrenceDelta(
              *chainProduct,
              {OccurrenceDelta::suppressed(
                   address(*chainProduct, OccurrenceId{2}), false, true),
               duplicateSelected, duplicateSelected},
              chainSuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    chainRevisions.push_back(chainHistory.currentRevision());
    const auto chainRemove = removePayload(*chainProduct, OccurrenceId{2});
    CHECK(chainHistory.executeRemove(
              *chainProduct,
              {chainRemove, duplicateSelected, {}}, chainSuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    chainRevisions.push_back(chainHistory.currentRevision());
    CHECK(chainHistory.size() == 7 && chainHistory.cursor() == 7);
    CHECK(chainRevisions.size() == 7);
    CHECK(chainProduct->rootAssemblyDefinition()->size() == 1);
    CHECK(chainProduct->rootAssemblyDefinition()->find(OccurrenceId{1}));
    CHECK(!chainProduct->rootAssemblyDefinition()->find(OccurrenceId{2}));
    CHECK(chainProduct->definitions().findPart(DefinitionId{2}));
    CHECK(!chainHistory.currentOccurrence());

    CHECK(chainHistory.undo(*chainProduct, chainSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    const auto exactEdited = [&](bool visible, bool grounded, bool suppressed,
                                 std::string_view name) {
      const auto* occurrence =
          chainProduct->rootAssemblyDefinition()->find(OccurrenceId{2});
      return occurrence && occurrence->targetDefinitionId() == DefinitionId{2} &&
             occurrence->placement() == RigidTransform3d::identity() &&
             occurrence->flags().visible == visible &&
             occurrence->flags().grounded == grounded &&
             occurrence->flags().suppressed == suppressed &&
             occurrence->displayNameUtf8() == name &&
             chainHistory.currentOccurrence() == path({2});
    };
    CHECK(exactEdited(false, true, true, "Цепочка 🔩"));
    CHECK(chainHistory.undo(*chainProduct, chainSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(exactEdited(false, true, false, "Цепочка 🔩"));
    CHECK(chainHistory.undo(*chainProduct, chainSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(exactEdited(false, false, false, "Цепочка 🔩"));
    CHECK(chainHistory.undo(*chainProduct, chainSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(exactEdited(true, false, false, "Цепочка 🔩"));
    CHECK(chainHistory.undo(*chainProduct, chainSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(exactEdited(true, false, false, {}));
    CHECK(chainHistory.undo(*chainProduct, chainSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(!chainProduct->rootAssemblyDefinition()->find(OccurrenceId{2}));
    CHECK(chainHistory.currentOccurrence() == path({1}));
    CHECK(chainHistory.undo(*chainProduct, chainSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(chainProduct->rootAssemblyDefinition()->occurrences().empty());
    CHECK(!chainProduct->definitions().findPart(DefinitionId{2}));
    CHECK(!chainHistory.currentOccurrence());

    for (int index = 0; index < 7; ++index) {
      CHECK(chainHistory.redo(*chainProduct, chainSuccessor).status ==
            AssemblyCommandHistoryStatus::Committed);
      CHECK(chainHistory.cursor() == static_cast<std::size_t>(index + 1));
      CHECK(chainHistory.currentRevision() ==
            chainRevisions[static_cast<std::size_t>(index)]);
      CHECK(!chainHistory.isAtSavepoint());
      const auto* root = chainProduct->rootAssemblyDefinition();
      CHECK(root);
      const auto* source = root->find(OccurrenceId{1});
      CHECK(source && source->targetDefinitionId() == DefinitionId{2} &&
            source->placement() == RigidTransform3d::identity() &&
            source->flags() == OccurrenceFlags{} &&
            source->displayNameUtf8().empty());
      CHECK(chainProduct->definitions().findPart(DefinitionId{2}));
      const auto referenceCount = static_cast<std::size_t>(std::count_if(
          root->occurrences().begin(), root->occurrences().end(),
          [](const auto& entry) {
            return entry.second.targetDefinitionId() == DefinitionId{2};
          }));
      CHECK(referenceCount >= 1);

      const auto* duplicate = root->find(OccurrenceId{2});
      if (index == 0) {
        CHECK(root->size() == 1 && !duplicate);
        CHECK(chainHistory.currentOccurrence() == path({1}));
      } else if (index == 6) {
        CHECK(root->size() == 1 && !duplicate);
        CHECK(!chainHistory.currentOccurrence());
      } else {
        CHECK(root->size() == 2 && duplicate);
        CHECK(duplicate->targetDefinitionId() == DefinitionId{2});
        CHECK(duplicate->placement() == RigidTransform3d::identity());
        const std::string_view expectedName =
            index >= 2 ? std::string_view{"Цепочка 🔩"} : std::string_view{};
        CHECK(duplicate->displayNameUtf8() == expectedName);
        CHECK(duplicate->flags().visible == (index < 3));
        CHECK(duplicate->flags().grounded == (index >= 4));
        CHECK(duplicate->flags().suppressed == (index >= 5));
        CHECK(chainHistory.currentOccurrence() == path({2}));
      }
    }
    CHECK(chainHistory.cursor() == 7 &&
          chainProduct->rootAssemblyDefinition()->size() == 1 &&
          chainProduct->rootAssemblyDefinition()->find(OccurrenceId{1}) &&
          !chainProduct->rootAssemblyDefinition()->find(OccurrenceId{2}) &&
          chainProduct->definitions().findPart(DefinitionId{2}) &&
          !chainHistory.currentOccurrence());
  }

  // The centralized allocator fills holes, never wraps past 4096, and does
  // not consume an ID merely by querying it.
  {
    DefinitionRegistry definitions;
    CHECK(definitions.insert(
              PartDefinition(DefinitionId{2}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
    AssemblyDefinition root(DefinitionId{1});
    CHECK(root.insert(ComponentOccurrence(
              OccurrenceId{4096}, DefinitionId{2},
              RigidTransform3d::identity())) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(definitions.insert(std::move(root)) ==
          DefinitionInsertStatus::Inserted);
    auto made = ProductDocument::create(std::move(definitions), DefinitionId{1});
    CHECK(made);
    CHECK(made.product->lowestFreeRootOccurrenceId() == OccurrenceId{1});
    CHECK(made.product->lowestFreeRootOccurrenceId() == OccurrenceId{1});

    DefinitionRegistry fullDefinitions;
    CHECK(fullDefinitions.insert(
              PartDefinition(DefinitionId{2}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
    AssemblyDefinition fullRoot(DefinitionId{1});
    for (std::uint64_t id = 1; id <= kMaximumRootOccurrences; ++id) {
      CHECK(fullRoot.insert(ComponentOccurrence(
                OccurrenceId{id}, DefinitionId{2},
                RigidTransform3d::identity())) ==
            AssemblyOccurrenceInsertStatus::Inserted);
    }
    CHECK(fullDefinitions.insert(std::move(fullRoot)) ==
          DefinitionInsertStatus::Inserted);
    auto full = ProductDocument::create(std::move(fullDefinitions),
                                        DefinitionId{1});
    CHECK(full);
    CHECK(!full.product->lowestFreeRootOccurrenceId());
    auto limitPayload =
        duplicatePayload(*full.product, OccurrenceId{1}, OccurrenceId{2});
    auto acceptLimit = [](const ProductDocument&) { return true; };
    CallableSuccessor limitSuccessor(acceptLimit);
    CHECK(full.product->applyDuplicateOccurrence(
              limitPayload, limitSuccessor,
              DuplicateOccurrenceFailurePoint::None) ==
          DuplicateOccurrenceStatus::OccurrenceLimit);

    RemoveOccurrenceColdPayload restoreLimit{
        {full.product->incarnation(), full.product->rootDefinitionId(),
         path({4097}), DefinitionId{2}},
        RigidTransform3d::identity(), {}, {}};
    AssemblyCommandHistory limitHistory;
    FixedStatusSuccessor removeLimitSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    const auto limitProductBefore =
        productSnapshot(*full.product, DefinitionId{2});
    const auto limitHistoryBefore = snapshot(limitHistory, *full.product);
    CHECK(full.product->restoreRemovedOccurrence(
              restoreLimit, removeLimitSuccessor,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::OccurrenceLimit);
    CHECK(removeLimitSuccessor.calls == 0);
    CHECK(productSnapshot(*full.product, DefinitionId{2}) ==
          limitProductBefore);
    CHECK(snapshot(limitHistory, *full.product) == limitHistoryBefore);
  }

  // A5d definition edits remain cold in the unified assembly history.  The
  // authoritative embedded Document allocation and every occurrence field
  // stay stable while the definition state token/publication advance.
  {
    auto product = assemblyWithOccurrence();
    auto* authoritative = product->partDocument(DefinitionId{2});
    CHECK(authoritative);
    const Document* authoritativeAddress = authoritative;
    const auto productBefore = productSnapshot(*product, DefinitionId{2});
    const auto stateBefore = product->definitionStateId(DefinitionId{2});
    Document edited = *authoritative;
    edited.setBox({31.0, 29.0, 17.0});
    const DocumentDelta delta = DocumentDelta::create(*authoritative, edited);
    CHECK(delta && !delta.empty());

    AssemblyCommandHistory history;
    FixedStatusSuccessor successor(ProductDocumentSuccessorStatus::Prepared);
    DefinitionEditColdCommand command{
        {product->incarnation(), product->rootDefinitionId(), DefinitionId{2},
         stateBefore},
        delta,
        {{}, DefinitionId{2}},
        {{}, DefinitionId{2}}};
    const auto applied = history.executeDefinitionEdit(
        *product, std::move(command), successor);
    CHECK(applied.status == AssemblyCommandHistoryStatus::Committed);
    CHECK(successor.calls == 1 &&
          successor.lastImpact ==
              ProductDocumentSuccessorImpact::DefinitionProjection);
    CHECK(product->partDocument(DefinitionId{2}) == authoritativeAddress);
    CHECK(product->partDocument(DefinitionId{2})->box().widthMm == 31.0);
    CHECK(product->definitionStateId(DefinitionId{2}) != stateBefore);
    CHECK(product->rootAssemblyDefinition()->find(OccurrenceId{3})->placement() ==
          RigidTransform3d::identity());
    CHECK(history.size() == 1 && history.cursor() == 1 &&
          history.currentRevision().isValid() && !history.isAtSavepoint());

    FixedStatusSuccessor undoSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.undo(*product, undoSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(undoSuccessor.calls == 1 &&
          undoSuccessor.lastImpact ==
              ProductDocumentSuccessorImpact::DefinitionProjection);
    CHECK(product->partDocument(DefinitionId{2}) == authoritativeAddress);
    const auto productAfterSuccessfulUndo =
        productSnapshot(*product, DefinitionId{2});
    CHECK(productAfterSuccessfulUndo.rootId == productBefore.rootId);
    CHECK(productAfterSuccessfulUndo.incarnation == productBefore.incarnation);
    CHECK(productAfterSuccessfulUndo.identity == productBefore.identity);
    CHECK(productAfterSuccessfulUndo.rootAddress == productBefore.rootAddress);
    CHECK(productAfterSuccessfulUndo.definitionIds ==
          productBefore.definitionIds);
    CHECK(productAfterSuccessfulUndo.definitionKinds ==
          productBefore.definitionKinds);
    CHECK(productAfterSuccessfulUndo.trackedDefinitionAddress ==
          productBefore.trackedDefinitionAddress);
    CHECK(productAfterSuccessfulUndo.trackedDocumentAddress ==
          productBefore.trackedDocumentAddress);
    CHECK(productAfterSuccessfulUndo.trackedDocumentFingerprint ==
          productBefore.trackedDocumentFingerprint);
    CHECK(productAfterSuccessfulUndo.occurrences == productBefore.occurrences);
    // A successful Undo is itself a new definition state and recompute, so
    // unlike a failed transaction it intentionally advances the runtime token
    // and may replace B-Rep handles. Exact failure checks below still compare
    // the complete snapshot including both fields.
    CHECK(productAfterSuccessfulUndo.trackedDefinitionState !=
          productBefore.trackedDefinitionState);
    CHECK(history.cursor() == 0 && history.canRedo());

    FixedStatusSuccessor redoSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.redo(*product, redoSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(redoSuccessor.calls == 1 &&
          redoSuccessor.lastImpact ==
              ProductDocumentSuccessorImpact::DefinitionProjection);
    CHECK(product->partDocument(DefinitionId{2}) == authoritativeAddress);
    CHECK(product->partDocument(DefinitionId{2})->box().widthMm == 31.0);
    CHECK(history.cursor() == 1);

    CHECK(history.undo(*product, undoSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    const auto exactBeforeFailure = productSnapshot(*product, DefinitionId{2});
    const auto historyBeforeFailure = snapshot(history, *product);
    ResourceFailureSuccessor rejected;
    CHECK(history.redo(*product, rejected).status ==
          AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(rejected.calls == 1);
    CHECK(productSnapshot(*product, DefinitionId{2}) == exactBeforeFailure);
    CHECK(snapshot(history, *product) == historyBeforeFailure);

    // Invalid recompute never reaches successor and cannot replace the live
    // embedded Document or advance its runtime state token.
    Document invalid;
    auto& invalidBody = invalid.addBody(501, "invalid");
    invalidBody.addFeature(std::make_unique<ImportedShapeFeature>(
        502, std::make_shared<const TopoDS_Shape>(), "null"));
    const auto invalidBefore = productSnapshot(*product, DefinitionId{2});
    const auto invalidState = product->definitionStateId(DefinitionId{2});
    FixedStatusSuccessor invalidSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    const DefinitionEditAddress invalidAddress{
        product->incarnation(), product->rootDefinitionId(), DefinitionId{2},
        invalidState};
    const auto invalidResult = product->applyDefinitionEdit(
        invalidAddress, invalid, invalidSuccessor);
    CHECK(invalidResult.status == DefinitionEditStatus::RecomputeFailed);
    CHECK(invalidSuccessor.calls == 0);
    CHECK(productSnapshot(*product, DefinitionId{2}) == invalidBefore);
    CHECK(product->definitionStateId(DefinitionId{2}) == invalidState);

    // The same invalid cold delta through the unified history boundary must
    // fail during the actual model recompute, before successor preparation or
    // any history revision/cursor mutation.
    auto invalidHistoryProduct = assemblyWithOccurrence();
    const Document* invalidHistoryCurrent =
        invalidHistoryProduct->partDocument(DefinitionId{2});
    CHECK(invalidHistoryCurrent);
    const auto invalidHistoryProductBefore =
        productSnapshot(*invalidHistoryProduct, DefinitionId{2});
    AssemblyCommandHistory invalidHistory;
    const auto invalidHistoryBefore =
        snapshot(invalidHistory, *invalidHistoryProduct);
    FixedStatusSuccessor invalidHistorySuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    DefinitionEditColdCommand invalidHistoryCommand{
        {invalidHistoryProduct->incarnation(),
         invalidHistoryProduct->rootDefinitionId(), DefinitionId{2},
         invalidHistoryProduct->definitionStateId(DefinitionId{2})},
        DocumentDelta::create(*invalidHistoryCurrent, invalid),
        {{}, DefinitionId{2}},
        {{}, DefinitionId{2}}};
    CHECK(invalidHistory
              .executeDefinitionEdit(*invalidHistoryProduct,
                                     std::move(invalidHistoryCommand),
                                     invalidHistorySuccessor)
              .status == AssemblyCommandHistoryStatus::ModelRejected);
    CHECK(invalidHistorySuccessor.calls == 0);
    CHECK(productSnapshot(*invalidHistoryProduct, DefinitionId{2}) ==
          invalidHistoryProductBefore);
    CHECK(snapshot(invalidHistory, *invalidHistoryProduct) ==
          invalidHistoryBefore);

    // Successor preparation runs while the product mutation guard is held.
    // Neither registry insertion nor a nested embedded-document edit can
    // escape through a second public mutation boundary.
    CHECK(history.redo(*product, redoSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    const auto reentrantProductBefore =
        productSnapshot(*product, DefinitionId{2});
    const auto reentrantHistoryBefore = snapshot(history, *product);
    CHECK(history.undo(*product, undoSuccessor).status ==
          AssemblyCommandHistoryStatus::Committed);
    const auto beforeRejectedRedo = productSnapshot(*product, DefinitionId{2});
    const auto historyBeforeRejectedRedo = snapshot(history, *product);
    ReentrantDefinitionMutationSuccessor reentrant;
    CHECK(history.redo(*product, reentrant).status ==
          AssemblyCommandHistoryStatus::SuccessorRejected);
    CHECK(reentrant.calls == 1 && !reentrant.threw);
    CHECK(reentrant.lastImpact ==
          ProductDocumentSuccessorImpact::DefinitionProjection);
    CHECK(reentrant.insertStatus == DefinitionInsertStatus::Busy);
    CHECK(reentrant.nestedStatus == DefinitionEditStatus::Busy);
    CHECK(reentrant.nestedSuccessorCalls == 0);
    CHECK(!product->definitions().contains(DefinitionId{99}));
    CHECK(productSnapshot(*product, DefinitionId{2}) == beforeRejectedRedo);
    CHECK(snapshot(history, *product) == historyBeforeRejectedRedo);
    CHECK(reentrantProductBefore != beforeRejectedRedo);
    CHECK(reentrantHistoryBefore != historyBeforeRejectedRedo);

    struct DefinitionFailureCase final {
      DefinitionEditFailurePoint modelPoint;
      ProductDocumentSuccessorStatus successorStatus;
      detail::AssemblyCommandHistoryFailurePoint historyPoint;
      detail::AssemblyCommandHistoryFailureKind historyKind;
      AssemblyCommandHistoryStatus expected;
      int expectedSuccessorCalls;
    };
    const std::array definitionFailures{
        DefinitionFailureCase{
            DefinitionEditFailurePoint::None,
            ProductDocumentSuccessorStatus::Prepared,
            detail::AssemblyCommandHistoryFailurePoint::SemanticStatePreparation,
            detail::AssemblyCommandHistoryFailureKind::BadAlloc,
            AssemblyCommandHistoryStatus::ResourceLimit, 0},
        DefinitionFailureCase{
            DefinitionEditFailurePoint::None,
            ProductDocumentSuccessorStatus::Prepared,
            detail::AssemblyCommandHistoryFailurePoint::PayloadPreparation,
            detail::AssemblyCommandHistoryFailureKind::BadAlloc,
            AssemblyCommandHistoryStatus::ResourceLimit, 0},
        DefinitionFailureCase{
            DefinitionEditFailurePoint::BeforeRecompute,
            ProductDocumentSuccessorStatus::Prepared,
            detail::AssemblyCommandHistoryFailurePoint::None,
            detail::AssemblyCommandHistoryFailureKind::BadAlloc,
            AssemblyCommandHistoryStatus::ResourceLimit, 0},
        DefinitionFailureCase{
            DefinitionEditFailurePoint::BeforeCommit,
            ProductDocumentSuccessorStatus::Prepared,
            detail::AssemblyCommandHistoryFailurePoint::None,
            detail::AssemblyCommandHistoryFailureKind::BadAlloc,
            AssemblyCommandHistoryStatus::ResourceLimit, 0},
        DefinitionFailureCase{
            DefinitionEditFailurePoint::Finalization,
            ProductDocumentSuccessorStatus::Prepared,
            detail::AssemblyCommandHistoryFailurePoint::None,
            detail::AssemblyCommandHistoryFailureKind::BadAlloc,
            AssemblyCommandHistoryStatus::ResourceLimit, 1},
        DefinitionFailureCase{
            DefinitionEditFailurePoint::None,
            ProductDocumentSuccessorStatus::Rejected,
            detail::AssemblyCommandHistoryFailurePoint::None,
            detail::AssemblyCommandHistoryFailureKind::BadAlloc,
            AssemblyCommandHistoryStatus::SuccessorRejected, 1},
        DefinitionFailureCase{
            DefinitionEditFailurePoint::None,
            ProductDocumentSuccessorStatus::ResourceFailure,
            detail::AssemblyCommandHistoryFailurePoint::None,
            detail::AssemblyCommandHistoryFailureKind::BadAlloc,
            AssemblyCommandHistoryStatus::ResourceLimit, 1},
        DefinitionFailureCase{
            DefinitionEditFailurePoint::None,
            ProductDocumentSuccessorStatus::Prepared,
            detail::AssemblyCommandHistoryFailurePoint::Finalization,
            detail::AssemblyCommandHistoryFailureKind::BadAlloc,
            AssemblyCommandHistoryStatus::ResourceLimit, 1}};

    const auto makeEdit = [](ProductDocument& candidateProduct) {
      const Document* current =
          candidateProduct.partDocument(DefinitionId{2});
      CHECK(current);
      Document changed = *current;
      changed.setBox({41.0, 23.0, 19.0});
      return DefinitionEditColdCommand{
          {candidateProduct.incarnation(),
           candidateProduct.rootDefinitionId(), DefinitionId{2},
           candidateProduct.definitionStateId(DefinitionId{2})},
          DocumentDelta::create(*current, changed),
           {path({3}), DefinitionId{2}},
           {path({3}), DefinitionId{2}}};
    };

    // Execute, Undo and Redo all share the same exact failure contract.
    for (const auto& failure : definitionFailures) {
      // PayloadPreparation is an Undo/Redo-only boundary: Execute already owns
      // the caller-prepared candidate and has no cold reconstruction step.
      if (failure.historyPoint !=
          detail::AssemblyCommandHistoryFailurePoint::PayloadPreparation) {
        auto executeProduct = assemblyWithOccurrence();
        AssemblyCommandHistory executeHistory;
        detail::AssemblyCommandHistoryTestAccess::injectDefinitionEdit(
            executeHistory, failure.modelPoint);
        if (failure.historyPoint !=
            detail::AssemblyCommandHistoryFailurePoint::None) {
          detail::AssemblyCommandHistoryTestAccess::inject(
              executeHistory, failure.historyPoint, failure.historyKind);
        }
        const auto executeProductBefore =
            productSnapshot(*executeProduct, DefinitionId{2});
        const auto executeHistoryBefore =
            snapshot(executeHistory, *executeProduct);
        FixedStatusSuccessor executeSuccessor(failure.successorStatus);
        CHECK(executeHistory
                  .executeDefinitionEdit(*executeProduct,
                                         makeEdit(*executeProduct),
                                         executeSuccessor)
                  .status == failure.expected);
        CHECK(executeSuccessor.calls == failure.expectedSuccessorCalls);
        CHECK(productSnapshot(*executeProduct, DefinitionId{2}) ==
              executeProductBefore);
        CHECK(snapshot(executeHistory, *executeProduct) ==
              executeHistoryBefore);
      }

      auto undoProduct = assemblyWithOccurrence();
      AssemblyCommandHistory undoHistory;
      FixedStatusSuccessor seedSuccessor(
          ProductDocumentSuccessorStatus::Prepared);
      CHECK(undoHistory
                .executeDefinitionEdit(*undoProduct, makeEdit(*undoProduct),
                                       seedSuccessor)
                .status == AssemblyCommandHistoryStatus::Committed);
      detail::AssemblyCommandHistoryTestAccess::injectDefinitionEdit(
          undoHistory, failure.modelPoint);
      if (failure.historyPoint !=
          detail::AssemblyCommandHistoryFailurePoint::None) {
        detail::AssemblyCommandHistoryTestAccess::inject(
            undoHistory, failure.historyPoint, failure.historyKind);
      }
      const auto undoProductBefore =
          productSnapshot(*undoProduct, DefinitionId{2});
      const auto undoHistoryBefore = snapshot(undoHistory, *undoProduct);
      FixedStatusSuccessor undoFailureSuccessor(failure.successorStatus);
      const auto undoFailureResult =
          undoHistory.undo(*undoProduct, undoFailureSuccessor);
      CHECK(undoFailureResult.status == failure.expected);
      CHECK(!undoFailureResult.restoredState.has_value());
      CHECK(undoFailureSuccessor.calls == failure.expectedSuccessorCalls);
      CHECK(productSnapshot(*undoProduct, DefinitionId{2}) ==
            undoProductBefore);
      CHECK(snapshot(undoHistory, *undoProduct) == undoHistoryBefore);

      auto redoProduct = assemblyWithOccurrence();
      AssemblyCommandHistory redoHistory;
      CHECK(redoHistory
                .executeDefinitionEdit(*redoProduct, makeEdit(*redoProduct),
                                       seedSuccessor)
                .status == AssemblyCommandHistoryStatus::Committed);
      CHECK(redoHistory.undo(*redoProduct, seedSuccessor).status ==
            AssemblyCommandHistoryStatus::Committed);
      detail::AssemblyCommandHistoryTestAccess::injectDefinitionEdit(
          redoHistory, failure.modelPoint);
      if (failure.historyPoint !=
          detail::AssemblyCommandHistoryFailurePoint::None) {
        detail::AssemblyCommandHistoryTestAccess::inject(
            redoHistory, failure.historyPoint, failure.historyKind);
      }
      const auto redoProductBefore =
          productSnapshot(*redoProduct, DefinitionId{2});
      const auto redoHistoryBefore = snapshot(redoHistory, *redoProduct);
      FixedStatusSuccessor redoFailureSuccessor(failure.successorStatus);
      const auto redoFailureResult =
          redoHistory.redo(*redoProduct, redoFailureSuccessor);
      CHECK(redoFailureResult.status == failure.expected);
      CHECK(!redoFailureResult.restoredState.has_value());
      CHECK(redoFailureSuccessor.calls == failure.expectedSuccessorCalls);
      CHECK(productSnapshot(*redoProduct, DefinitionId{2}) ==
            redoProductBefore);
      CHECK(snapshot(redoHistory, *redoProduct) == redoHistoryBefore);
    }
  }

  // A6b1 linked lifecycle commands retain only cold authored deltas/bindings.
  // Reload updates the single shared definition for both occurrences; Repair
  // changes only the relocatable locator. Undo/Redo never need a resolver.
  {
    auto product = linkedAssemblyWithOccurrences();
    const Document* authoritative = product->partDocument(DefinitionId{2});
    CHECK(authoritative);
    const auto* initialBinding = product->partLinkedBinding(DefinitionId{2});
    CHECK(initialBinding);
    const LinkedPartBinding beforeBinding = *initialBinding;
    const auto occurrence3 =
        *product->rootAssemblyDefinition()->find(OccurrenceId{3});
    const auto occurrence4 =
        *product->rootAssemblyDefinition()->find(OccurrenceId{4});

    Document forbiddenEdit = *authoritative;
    forbiddenEdit.setBox({19.0, 18.0, 17.0});
    FixedStatusSuccessor unused(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product
              ->applyDefinitionEdit(
                  {product->incarnation(), product->rootDefinitionId(),
                   DefinitionId{2},
                   product->definitionStateId(DefinitionId{2})},
                  forbiddenEdit, unused)
              .status ==
          DefinitionEditStatus::LinkedDefinitionRequiresLifecycle);
    CHECK(unused.calls == 0);

    Document invalidReload;
    auto& invalidBody = invalidReload.addBody(801, "invalid linked reload");
    invalidBody.addFeature(std::make_unique<ImportedShapeFeature>(
        802, std::make_shared<const TopoDS_Shape>(), "null"));
    LinkedPartBinding invalidRevision = beforeBinding;
    invalidRevision.acceptedHash.bytes[0] = 9;
    const auto beforeInvalidReload =
        productSnapshot(*product, DefinitionId{2});
    FixedStatusSuccessor invalidReloadSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(product
              ->applyLinkedPartReload(
                  {product->incarnation(), product->rootDefinitionId(),
                   DefinitionId{2},
                   product->definitionStateId(DefinitionId{2})},
                  beforeBinding, invalidRevision, invalidReload,
                  invalidReloadSuccessor)
              .status == LinkedPartMutationStatus::RecomputeFailed);
    CHECK(invalidReloadSuccessor.calls == 0);
    CHECK(productSnapshot(*product, DefinitionId{2}) == beforeInvalidReload);
    CHECK(*product->partLinkedBinding(DefinitionId{2}) == beforeBinding);

    Document reloaded = *authoritative;
    reloaded.setBox({31.0, 29.0, 17.0});
    LinkedPartBinding afterBinding = beforeBinding;
    afterBinding.acceptedHash.bytes[0] = 2;
    LinkedPartReloadColdCommand reload{
        {product->incarnation(), product->rootDefinitionId(), DefinitionId{2},
         product->definitionStateId(DefinitionId{2})},
        beforeBinding,
        afterBinding,
        DocumentDelta::create(*authoritative, reloaded),
        {{}, DefinitionId{2}},
        {{}, DefinitionId{2}}};
    AssemblyCommandHistory history;
    FixedStatusSuccessor reloadSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.executeLinkedPartReload(*product, std::move(reload),
                                         reloadSuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(reloadSuccessor.calls == 1 &&
          reloadSuccessor.lastImpact ==
              ProductDocumentSuccessorImpact::DefinitionProjection);
    CHECK(product->partDocument(DefinitionId{2}) == authoritative);
    CHECK(product->partDocument(DefinitionId{2})->box().widthMm == 31.0);
    CHECK(*product->partLinkedBinding(DefinitionId{2}) == afterBinding);
    const auto* unchanged3 =
        product->rootAssemblyDefinition()->find(OccurrenceId{3});
    const auto* unchanged4 =
        product->rootAssemblyDefinition()->find(OccurrenceId{4});
    CHECK(unchanged3 && unchanged3->id() == occurrence3.id() &&
          unchanged3->targetDefinitionId() ==
              occurrence3.targetDefinitionId() &&
          unchanged3->placement() == occurrence3.placement() &&
          unchanged3->flags() == occurrence3.flags() &&
          unchanged3->displayNameUtf8() == occurrence3.displayNameUtf8());
    CHECK(unchanged4 && unchanged4->id() == occurrence4.id() &&
          unchanged4->targetDefinitionId() ==
              occurrence4.targetDefinitionId() &&
          unchanged4->placement() == occurrence4.placement() &&
          unchanged4->flags() == occurrence4.flags() &&
          unchanged4->displayNameUtf8() == occurrence4.displayNameUtf8());
    CHECK(history.retainedBytes() >=
          beforeBinding.relativePathHintUtf8.capacity() +
              afterBinding.relativePathHintUtf8.capacity());

    FixedStatusSuccessor coldUndo(ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.undo(*product, coldUndo).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(product->partDocument(DefinitionId{2})->box().widthMm != 31.0);
    CHECK(*product->partLinkedBinding(DefinitionId{2}) == beforeBinding);
    CHECK(history.redo(*product, coldUndo).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(product->partDocument(DefinitionId{2})->box().widthMm == 31.0);
    CHECK(*product->partLinkedBinding(DefinitionId{2}) == afterBinding);

    const Document* beforeRepairDocument =
        product->partDocument(DefinitionId{2});
    LinkedPartBinding repairedBinding = afterBinding;
    repairedBinding.relativePathHintUtf8 = "relocated/source.solidar";
    FixedStatusSuccessor repairSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(history
              .executeLinkedPartRepair(
                  *product,
                  {{product->incarnation(), product->rootDefinitionId(),
                    DefinitionId{2},
                    product->definitionStateId(DefinitionId{2})},
                   afterBinding, repairedBinding, {{}, DefinitionId{2}},
                   {{}, DefinitionId{2}}},
                  repairSuccessor)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(repairSuccessor.calls == 1 &&
          repairSuccessor.lastImpact == ProductDocumentSuccessorImpact::TreeOnly);
    CHECK(product->partDocument(DefinitionId{2}) == beforeRepairDocument);
    CHECK(*product->partLinkedBinding(DefinitionId{2}) == repairedBinding);
    CHECK(product->partLinkedBinding(DefinitionId{2})->acceptedHash ==
          afterBinding.acceptedHash);
    CHECK(history.undo(*product, coldUndo).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(*product->partLinkedBinding(DefinitionId{2}) == afterBinding);
    CHECK(history.redo(*product, coldUndo).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(*product->partLinkedBinding(DefinitionId{2}) == repairedBinding);

    Document rejectedReload = *product->partDocument(DefinitionId{2});
    rejectedReload.setBox({47.0, 29.0, 17.0});
    LinkedPartBinding rejectedBinding = repairedBinding;
    rejectedBinding.acceptedHash.bytes[0] = 3;
    const auto exactProductBefore =
        productSnapshot(*product, DefinitionId{2});
    const auto exactHistoryBefore = snapshot(history, *product);
    const LinkedPartBinding exactBindingBefore =
        *product->partLinkedBinding(DefinitionId{2});
    detail::AssemblyCommandHistoryTestAccess::inject(
        history, detail::AssemblyCommandHistoryFailurePoint::Finalization,
        detail::AssemblyCommandHistoryFailureKind::BadAlloc);
    FixedStatusSuccessor finalizationSuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(history
              .executeLinkedPartReload(
                  *product,
                  {{product->incarnation(), product->rootDefinitionId(),
                    DefinitionId{2},
                    product->definitionStateId(DefinitionId{2})},
                   repairedBinding, rejectedBinding,
                   DocumentDelta::create(
                       *product->partDocument(DefinitionId{2}), rejectedReload),
                   {}, {}},
                  finalizationSuccessor)
              .status == AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(finalizationSuccessor.calls == 1);
    CHECK(productSnapshot(*product, DefinitionId{2}) == exactProductBefore);
    CHECK(snapshot(history, *product) == exactHistoryBefore);
    CHECK(*product->partLinkedBinding(DefinitionId{2}) == exactBindingBefore);

    ResourceFailureSuccessor reject;
    CHECK(history
              .executeLinkedPartReload(
                  *product,
                  {{product->incarnation(), product->rootDefinitionId(),
                    DefinitionId{2},
                    product->definitionStateId(DefinitionId{2})},
                   repairedBinding, rejectedBinding,
                   DocumentDelta::create(
                       *product->partDocument(DefinitionId{2}), rejectedReload),
                   {}, {}},
                  reject)
              .status == AssemblyCommandHistoryStatus::ResourceLimit);
    CHECK(productSnapshot(*product, DefinitionId{2}) == exactProductBefore);
    CHECK(snapshot(history, *product) == exactHistoryBefore);
    CHECK(*product->partLinkedBinding(DefinitionId{2}) == exactBindingBefore);

    LinkedPartBinding wrongIdentity = repairedBinding;
    wrongIdentity.key.definitionId = DefinitionId{8};
    FixedStatusSuccessor identitySuccessor(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(product
              ->applyLinkedPartRepair(
                  {product->incarnation(), product->rootDefinitionId(),
                   DefinitionId{2},
                   product->definitionStateId(DefinitionId{2})},
                  repairedBinding, wrongIdentity, identitySuccessor)
              .status == LinkedPartMutationStatus::ForbiddenIdentityChange);
    CHECK(identitySuccessor.calls == 0);
  }

  // A6b1 persistent-topology reload uses the authored V2 face identity. The
  // upstream profile revision changes the OCCT shape, while the downstream
  // sketch support resolves again without falling back to a raw face ordinal.
  // A broken provenance revision is rejected before projection and preserves
  // an existing redo tail byte-for-byte.
  {
    auto fixture = persistentFaceFixture(10.0);
    const FaceReference authoredReference = fixture.reference;
    const SketchId profileId = fixture.profileId;
    const SketchId supportedId = fixture.supportedId;
    auto product = linkedAssemblyWithOccurrences(std::move(fixture.document));
    const Document* current = product->partDocument(DefinitionId{2});
    CHECK(current);
    const auto* supportedBefore = current->findSketch(supportedId);
    CHECK(supportedBefore && supportedBefore->supportResolved &&
          supportedBefore->support.face == authoredReference &&
          supportedBefore->support.face.isV2());
    const ShapeFeature* featureBefore = current->findFeature(
        authoredReference.bodyId(), authoredReference.featureId());
    CHECK(featureBefore && featureBefore->lastValidShape());
    const auto shapeBefore = featureBefore->lastValidShape();
    const LinkedPartBinding beforeBinding =
        *product->partLinkedBinding(DefinitionId{2});

    Document revised = *current;
    sketch::Sketch widerProfile;
    widerProfile.addRectangle({0.0, 0.0}, {14.0, 8.0});
    CHECK(revised.replaceSketchGeometry(profileId, std::move(widerProfile)));
    LinkedPartBinding revisedBinding = beforeBinding;
    revisedBinding.acceptedHash.bytes[0] = 4;
    AssemblyCommandHistory history;
    FixedStatusSuccessor projection(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(history
              .executeLinkedPartReload(
                  *product,
                  {{product->incarnation(), product->rootDefinitionId(),
                    DefinitionId{2},
                    product->definitionStateId(DefinitionId{2})},
                   beforeBinding, revisedBinding,
                   DocumentDelta::create(*current, revised), {}, {}},
                  projection)
              .status == AssemblyCommandHistoryStatus::Committed);
    CHECK(projection.calls == 1 &&
          projection.lastImpact ==
              ProductDocumentSuccessorImpact::DefinitionProjection);
    const auto* resolved =
        product->partDocument(DefinitionId{2})->findSketch(supportedId);
    CHECK(resolved);
    CHECK(resolved->supportResolved);
    CHECK(resolved->support.face == authoredReference);
    CHECK(resolved->support.face.isV2());
    const ShapeFeature* featureAfter =
        product->partDocument(DefinitionId{2})->findFeature(
            authoredReference.bodyId(), authoredReference.featureId());
    CHECK(featureAfter && featureAfter->lastValidShape() &&
          featureAfter->lastValidShape().get() != shapeBefore.get());
    const auto topology = featureAfter->topologyIndex();
    CHECK(topology);
    CHECK(topology->resolveFace(authoredReference));
    CHECK(*product->partLinkedBinding(DefinitionId{2}) == revisedBinding);

    CHECK(history.undo(*product, projection).status ==
          AssemblyCommandHistoryStatus::Committed);
    CHECK(history.canRedo());
    current = product->partDocument(DefinitionId{2});
    Document broken = *current;
    auto brokenReference = *authoredReference.v2();
    brokenReference.provenance.localName = "missing-linked-face";
    auto* brokenSupport = broken.findSketch(supportedId);
    CHECK(brokenSupport);
    brokenSupport->support = {
        SketchSupportType::Face,
        FaceReference::fromV2(std::move(brokenReference))};
    brokenSupport->placementDirty = true;
    LinkedPartBinding brokenBinding = beforeBinding;
    brokenBinding.acceptedHash.bytes[0] = 5;
    const auto exactProductBefore =
        productSnapshot(*product, DefinitionId{2});
    const auto exactHistoryBefore = snapshot(history, *product);
    const LinkedPartBinding exactBindingBefore =
        *product->partLinkedBinding(DefinitionId{2});
    FixedStatusSuccessor untouchedProjection(
        ProductDocumentSuccessorStatus::Prepared);
    CHECK(history
              .executeLinkedPartReload(
                  *product,
                  {{product->incarnation(), product->rootDefinitionId(),
                    DefinitionId{2},
                    product->definitionStateId(DefinitionId{2})},
                   beforeBinding, brokenBinding,
                   DocumentDelta::create(*current, broken), {}, {}},
                  untouchedProjection)
              .status == AssemblyCommandHistoryStatus::ModelRejected);
    CHECK(untouchedProjection.calls == 0);
    CHECK(productSnapshot(*product, DefinitionId{2}) == exactProductBefore);
    CHECK(*product->partLinkedBinding(DefinitionId{2}) == exactBindingBefore);
    CHECK(snapshot(history, *product) == exactHistoryBefore);
    CHECK(history.canRedo());
  }

  // Linked Reload and Repair use the same atomic rollback contract on Undo
  // and Redo as on the initial application.  Exercise model resource seams,
  // successor rejection/resource failure, and history finalization failure.
  {
    struct LinkedFailureCase final {
      DefinitionEditFailurePoint modelPoint;
      ProductDocumentSuccessorStatus successorStatus;
      detail::AssemblyCommandHistoryFailurePoint historyPoint;
      AssemblyCommandHistoryStatus expected;
      int expectedSuccessorCalls;
    };
    const std::array failures{
        LinkedFailureCase{
            DefinitionEditFailurePoint::None,
            ProductDocumentSuccessorStatus::Prepared,
            detail::AssemblyCommandHistoryFailurePoint::SemanticStatePreparation,
            AssemblyCommandHistoryStatus::ResourceLimit, 0},
        LinkedFailureCase{
            DefinitionEditFailurePoint::None,
            ProductDocumentSuccessorStatus::Prepared,
            detail::AssemblyCommandHistoryFailurePoint::PayloadPreparation,
            AssemblyCommandHistoryStatus::ResourceLimit, 0},
        LinkedFailureCase{
            DefinitionEditFailurePoint::BeforeRecompute,
            ProductDocumentSuccessorStatus::Prepared,
            detail::AssemblyCommandHistoryFailurePoint::None,
            AssemblyCommandHistoryStatus::ResourceLimit, 0},
        LinkedFailureCase{
            DefinitionEditFailurePoint::BeforeCommit,
            ProductDocumentSuccessorStatus::Prepared,
            detail::AssemblyCommandHistoryFailurePoint::None,
            AssemblyCommandHistoryStatus::ResourceLimit, 0},
        LinkedFailureCase{
            DefinitionEditFailurePoint::Finalization,
            ProductDocumentSuccessorStatus::Prepared,
            detail::AssemblyCommandHistoryFailurePoint::None,
            AssemblyCommandHistoryStatus::ResourceLimit, 1},
        LinkedFailureCase{
            DefinitionEditFailurePoint::None,
            ProductDocumentSuccessorStatus::Rejected,
            detail::AssemblyCommandHistoryFailurePoint::None,
            AssemblyCommandHistoryStatus::SuccessorRejected, 1},
        LinkedFailureCase{
            DefinitionEditFailurePoint::None,
            ProductDocumentSuccessorStatus::ResourceFailure,
            detail::AssemblyCommandHistoryFailurePoint::None,
            AssemblyCommandHistoryStatus::ResourceLimit, 1},
        LinkedFailureCase{
            DefinitionEditFailurePoint::None,
            ProductDocumentSuccessorStatus::Prepared,
            detail::AssemblyCommandHistoryFailurePoint::Finalization,
            AssemblyCommandHistoryStatus::ResourceLimit, 1}};

    const auto applySeed = [](bool reload, ProductDocument& candidateProduct,
                              AssemblyCommandHistory& candidateHistory,
                              ProductDocumentSuccessor& successor) {
      const Document* current =
          candidateProduct.partDocument(DefinitionId{2});
      CHECK(current);
      const LinkedPartBinding before =
          *candidateProduct.partLinkedBinding(DefinitionId{2});
      if (reload) {
        Document changed = *current;
        changed.setBox({43.0, 29.0, 17.0});
        LinkedPartBinding after = before;
        after.acceptedHash.bytes[0] =
            static_cast<std::uint8_t>(before.acceptedHash.bytes[0] + 1);
        return candidateHistory.executeLinkedPartReload(
            candidateProduct,
            {{candidateProduct.incarnation(),
              candidateProduct.rootDefinitionId(), DefinitionId{2},
              candidateProduct.definitionStateId(DefinitionId{2})},
             before, after, DocumentDelta::create(*current, changed),
              {path({3}), DefinitionId{2}},
              {path({3}), DefinitionId{2}}},
            successor);
      }
      LinkedPartBinding after = before;
      after.relativePathHintUtf8 = "matrix/relocated-source.solidar";
      return candidateHistory.executeLinkedPartRepair(
          candidateProduct,
          {{candidateProduct.incarnation(),
            candidateProduct.rootDefinitionId(), DefinitionId{2},
            candidateProduct.definitionStateId(DefinitionId{2})},
           before, after, {path({3}), DefinitionId{2}},
           {path({3}), DefinitionId{2}}},
          successor);
    };

    for (const bool reload : {true, false}) {
      for (const auto& failure : failures) {
        if (!reload &&
            failure.historyPoint ==
                detail::AssemblyCommandHistoryFailurePoint::PayloadPreparation)
          continue;
        auto undoProduct = linkedAssemblyWithOccurrences();
        AssemblyCommandHistory undoHistory;
        FixedStatusSuccessor seedSuccessor(
            ProductDocumentSuccessorStatus::Prepared);
        CHECK(applySeed(reload, *undoProduct, undoHistory, seedSuccessor)
                  .status == AssemblyCommandHistoryStatus::Committed);
        undoHistory.markSavepoint();
        detail::AssemblyCommandHistoryTestAccess::injectDefinitionEdit(
            undoHistory, failure.modelPoint);
        if (failure.historyPoint !=
            detail::AssemblyCommandHistoryFailurePoint::None) {
          detail::AssemblyCommandHistoryTestAccess::inject(
              undoHistory, failure.historyPoint,
              detail::AssemblyCommandHistoryFailureKind::BadAlloc);
        }
        const auto undoProductBefore =
            productSnapshot(*undoProduct, DefinitionId{2});
        const auto undoHistoryBefore = snapshot(undoHistory, *undoProduct);
        const LinkedPartBinding undoBindingBefore =
            *undoProduct->partLinkedBinding(DefinitionId{2});
        FixedStatusSuccessor undoFailure(failure.successorStatus);
        const auto undoFailureResult =
            undoHistory.undo(*undoProduct, undoFailure);
        CHECK(undoFailureResult.status == failure.expected);
        CHECK(!undoFailureResult.restoredState.has_value());
        CHECK(undoFailure.calls == failure.expectedSuccessorCalls);
        CHECK(productSnapshot(*undoProduct, DefinitionId{2}) ==
              undoProductBefore);
        CHECK(*undoProduct->partLinkedBinding(DefinitionId{2}) ==
              undoBindingBefore);
        CHECK(snapshot(undoHistory, *undoProduct) == undoHistoryBefore);

        auto redoProduct = linkedAssemblyWithOccurrences();
        AssemblyCommandHistory redoHistory;
        CHECK(applySeed(reload, *redoProduct, redoHistory, seedSuccessor)
                  .status == AssemblyCommandHistoryStatus::Committed);
        CHECK(redoHistory.undo(*redoProduct, seedSuccessor).status ==
              AssemblyCommandHistoryStatus::Committed);
        redoHistory.markSavepoint();
        detail::AssemblyCommandHistoryTestAccess::injectDefinitionEdit(
            redoHistory, failure.modelPoint);
        if (failure.historyPoint !=
            detail::AssemblyCommandHistoryFailurePoint::None) {
          detail::AssemblyCommandHistoryTestAccess::inject(
              redoHistory, failure.historyPoint,
              detail::AssemblyCommandHistoryFailureKind::BadAlloc);
        }
        const auto redoProductBefore =
            productSnapshot(*redoProduct, DefinitionId{2});
        const auto redoHistoryBefore = snapshot(redoHistory, *redoProduct);
        const LinkedPartBinding redoBindingBefore =
            *redoProduct->partLinkedBinding(DefinitionId{2});
        FixedStatusSuccessor redoFailure(failure.successorStatus);
        const auto redoFailureResult =
            redoHistory.redo(*redoProduct, redoFailure);
        CHECK(redoFailureResult.status == failure.expected);
        CHECK(!redoFailureResult.restoredState.has_value());
        CHECK(redoFailure.calls == failure.expectedSuccessorCalls);
        CHECK(productSnapshot(*redoProduct, DefinitionId{2}) ==
              redoProductBefore);
        CHECK(*redoProduct->partLinkedBinding(DefinitionId{2}) ==
              redoBindingBefore);
        CHECK(snapshot(redoHistory, *redoProduct) == redoHistoryBefore);
      }
    }
  }

  // The aggregate linked-command retained-byte gate accepts exactly 64 MiB.
  // At limit+1 it rejects before touching the model or truncating a redo tail.
  {
    auto makeRepair = [](ProductDocument& product, std::string locator) {
      const LinkedPartBinding before =
          *product.partLinkedBinding(DefinitionId{2});
      LinkedPartBinding after = before;
      after.relativePathHintUtf8 = std::move(locator);
      return LinkedPartRepairColdCommand{
          {product.incarnation(), product.rootDefinitionId(), DefinitionId{2},
           product.definitionStateId(DefinitionId{2})},
          before, after, {path({1, 7}), DefinitionId{2}},
          {path({2, 7}), DefinitionId{2}}};
    };
    const std::string largeLocator(4096, 'a');

    {
      auto product = linkedAssemblyWithOccurrences();
      AssemblyCommandHistory history;
      auto command = makeRepair(*product, largeLocator);
      const auto linkedBytes =
          detail::AssemblyCommandHistoryTestAccess::dynamicBytes(
              AssemblyCommand(command));
      CHECK(linkedBytes &&
            *linkedBytes < AssemblyCommandHistory::maximumRetainedBytes());
      std::vector<std::byte> filler(
          AssemblyCommandHistory::maximumRetainedBytes() - *linkedBytes);
      CHECK(history.recordApplied(insertCommand(
                DefinitionId{9001}, OccurrenceId{9002}, std::move(filler))) ==
            AssemblyCommandHistoryStatus::Committed);
      FixedStatusSuccessor successor(
          ProductDocumentSuccessorStatus::Prepared);
      CHECK(history.executeLinkedPartRepair(*product, std::move(command),
                                            successor)
                .status == AssemblyCommandHistoryStatus::Committed);
      CHECK(history.retainedBytes() ==
            AssemblyCommandHistory::maximumRetainedBytes());
    }

    {
      auto product = linkedAssemblyWithOccurrences();
      AssemblyCommandHistory history;
      auto rejected = makeRepair(*product, largeLocator);
      const auto linkedBytes =
          detail::AssemblyCommandHistoryTestAccess::dynamicBytes(
              AssemblyCommand(rejected));
      CHECK(linkedBytes && *linkedBytes > 1 &&
            *linkedBytes < AssemblyCommandHistory::maximumRetainedBytes());
      std::vector<std::byte> filler(
          AssemblyCommandHistory::maximumRetainedBytes() - *linkedBytes + 1);
      CHECK(history.recordApplied(insertCommand(
                DefinitionId{9011}, OccurrenceId{9012}, std::move(filler))) ==
            AssemblyCommandHistoryStatus::Committed);
      FixedStatusSuccessor successor(
          ProductDocumentSuccessorStatus::Prepared);
      auto tail = makeRepair(*product, "tail/source.solidar");
      CHECK(history.executeLinkedPartRepair(*product, std::move(tail),
                                            successor)
                .status == AssemblyCommandHistoryStatus::Committed);
      CHECK(history.undo(*product, successor).status ==
            AssemblyCommandHistoryStatus::Committed);
      history.markSavepoint();
      CHECK(history.canRedo());
      const auto productBefore = productSnapshot(*product, DefinitionId{2});
      const auto historyBefore = snapshot(history, *product);
      const LinkedPartBinding bindingBefore =
          *product->partLinkedBinding(DefinitionId{2});
      CHECK(history.executeLinkedPartRepair(*product, std::move(rejected),
                                            successor)
                .status == AssemblyCommandHistoryStatus::ResourceLimit);
      CHECK(productSnapshot(*product, DefinitionId{2}) == productBefore);
      CHECK(*product->partLinkedBinding(DefinitionId{2}) == bindingBefore);
      CHECK(snapshot(history, *product) == historyBefore);
      CHECK(history.canRedo());
    }
  }

  return EXIT_SUCCESS;
}
