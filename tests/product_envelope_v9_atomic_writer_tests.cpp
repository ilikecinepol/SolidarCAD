#include "TestAssertions.h"

#include <QByteArrayView>
#include <QStringList>

#include <TopoDS_Shape.hxx>

#include <array>
#include <cmath>
#include <memory>

#include "model/AssemblyMate.h"
#include "model/ExtrudeFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/PlanarMateSolver.h"
#include "project/ProductEnvelopeV9Decoder.h"
#include "project/ProductEnvelopeV9Encoder.h"
#include "project/internal/ProductEnvelopeV9AtomicWriting.h"
#include "sketch/Sketch.h"

namespace {

using namespace solidar;
using namespace solidar::project;
using namespace solidar::project::detail;

constexpr ProjectId kProject{{0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                              0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x09}};

ProductEnvelopeV5Metadata metadata() {
  return {QStringLiteral("v9 atomic"),
          QStringLiteral("2026-10-30T00:00:00Z")};
}

std::unique_ptr<ProductDocument> product() {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(AssemblyDefinition(DefinitionId{1})) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{1});
  CHECK(made);
  return std::move(made.product);
}

OccurrencePath path(OccurrenceId id) {
  auto made = OccurrencePath::create({id});
  CHECK(made);
  return std::move(*made.path);
}

std::unique_ptr<ProductDocument> authoredMateProduct(
    int mateKind = 0) {
  constexpr SketchId sketchId{8101};
  constexpr BodyId bodyId{8102};
  constexpr FeatureId featureId{8103};
  auto document = std::make_unique<Document>();
  sketch::Sketch sketch;
  sketch.addRectangle({0.0, 0.0}, {10.0, 8.0});
  document->addSketch(sketchId, "atomic profile", std::move(sketch));
  auto& body = document->addBody(bodyId, "atomic body");
  body.addFeature(std::make_unique<ExtrudeFeature>(
      featureId, sketchId, 5.0, "atomic extrusion"));
  CHECK(document->recompute());
  std::optional<FaceReference> face;
  for (std::size_t index = 0; index < 32 && !face; ++index) {
    auto made = document->createFaceReferenceV2(bodyId, featureId, index);
    if (made) face = std::move(made.reference);
  }
  CHECK(face);
  const double half = 0.25 * 3.14159265358979323846;
  const auto moving = RigidTransform3d::create(
      {12.0, 6.0, 3.0},
      mateKind == 2
          ? std::array<double, 4>{std::cos(half), std::sin(half), 0.0, 0.0}
          : std::array<double, 4>{1.0, 0.0, 0.0, 0.0});
  CHECK(moving);
  AssemblyDefinition assembly(DefinitionId{2});
  CHECK(assembly.insert(ComponentOccurrence(
            OccurrenceId{10}, DefinitionId{1}, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true}, "fixed")) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(assembly.insert(ComponentOccurrence(OccurrenceId{20}, DefinitionId{1},
                                            *moving.transform, {}, "moving")) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  if (mateKind == 1) {
    CHECK(assembly.mateRegistry().insert(PointDistanceMate{
              MateId{1},
              MatePointEndpoint{path(OccurrenceId{10}),
                                LocalDatumPoint::Origin},
              MatePointEndpoint{path(OccurrenceId{20}),
                                LocalDatumPoint::Origin},
              5.0}) == MateRegistryMutationStatus::Inserted);
  } else if (mateKind == 2) {
    CHECK(assembly.mateRegistry().insert(PlanarAngleMate{
              MateId{1}, MateEndpoint{path(OccurrenceId{10}), *face},
              MateEndpoint{path(OccurrenceId{20}), *face},
              PlanarMateOrientation::Aligned,
              0.52359877559829887308}) ==
          MateRegistryMutationStatus::Inserted);
  } else {
    CHECK(assembly.mateRegistry().insert(PlanarCoincidentMate(
              MateId{1}, MateEndpoint{path(OccurrenceId{10}), *face},
              MateEndpoint{path(OccurrenceId{20}), *face},
              PlanarMateOrientation::Aligned, 0.0)) ==
          MateRegistryMutationStatus::Inserted);
  }
  std::vector<PartDefinition> parts;
  parts.emplace_back(DefinitionId{1}, std::move(document));
  std::vector<AssemblyDefinition> assemblies;
  assemblies.push_back(std::move(assembly));
  auto made = ProductDocument::createNested(
      std::move(parts), std::move(assemblies), DefinitionId{2});
  CHECK(made);
  return std::move(made.product);
}

class AcceptSuccessor final : public ProductDocumentSuccessor {
 public:
  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&,
      ProductDocumentSuccessorImpact) noexcept override {
    return ProductDocumentSuccessorStatus::Prepared;
  }
};

void solveAuthoredMate(ProductDocument& product) {
  auto solved = solvePlanarMates(
      product, DefinitionId{2}, MateSolveRequest{MateId{1}, std::nullopt});
  CHECK(solved.status == MateSolveStatus::Solved ||
        solved.status == MateSolveStatus::Underconstrained ||
        solved.status == MateSolveStatus::Redundant);
  CHECK(solved.candidate);
  CHECK(!solved.candidate->changes().empty());
  AcceptSuccessor successor;
  CHECK(product.applyMatePlacementCandidate(*solved.candidate, successor) ==
        MatePlacementApplyStatus::Applied);
}

struct Cursors final {
  BodyId body;
  SketchId sketch;
  FeatureId feature;
  ProductIncarnationId product;
};

Cursors consumeCursors() {
  Document document;
  const auto sketch = document.addSketch("cursor").id;
  auto& body = document.addBody("cursor");
  auto feature = std::make_unique<ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(), "cursor");
  const auto featureId = feature->id();
  body.addFeature(std::move(feature));
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{1}, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{1});
  CHECK(made);
  return {body.id(), sketch, featureId, made.product->incarnation()};
}

class Transaction final : public ProjectAtomicWriteTransaction {
 public:
  bool open(QString*) override {
    calls << "open";
    return fail != ProjectAtomicWriteFailure::OpenTemporary;
  }
  qint64 write(QByteArrayView bytes, QString*) override {
    calls << "write";
    staged = QByteArray(bytes.data(), bytes.size());
    return fail == ProjectAtomicWriteFailure::WriteTemporary ? bytes.size() - 1
                                                              : bytes.size();
  }
  bool flush(QString*) override {
    calls << "flush";
    return fail != ProjectAtomicWriteFailure::FlushTemporary;
  }
  bool sync(QString*) override {
    calls << "sync";
    return fail != ProjectAtomicWriteFailure::SyncTemporary;
  }
  bool commit(QString*) override {
    calls << "commit";
    if (fail == ProjectAtomicWriteFailure::ReplaceDestination) return false;
    destination = staged;
    return true;
  }
  void cancel() noexcept override { calls << "cancel"; }

  ProjectAtomicWriteFailure fail{ProjectAtomicWriteFailure::None};
  QStringList calls;
  QByteArray staged;
  QByteArray destination{"sentinel"};
};

void checkCursorOnlySamplingAdvanced(const Cursors& before,
                                     const Cursors& after) {
  CHECK(after.body == before.body + 1);
  CHECK(after.sketch == before.sketch + 1);
  CHECK(after.feature == before.feature + 1);
  CHECK(after.product.value() == before.product.value() + 1);
}

}  // namespace

int main() {
  auto source = product();
  const auto sourceBefore =
      encodeProductEnvelopeV9Root(*source, kProject, metadata());
  CHECK(sourceBefore.succeeded());

  {
    const auto cursorsBefore = consumeCursors();
    Transaction transaction;
    const auto result = writeProductEnvelopeV9Atomically(
        *source, kProject, metadata(), transaction);
    const auto cursorsAfter = consumeCursors();
    CHECK(result.succeeded());
    CHECK(transaction.destination == sourceBefore.payload->bytes);
    CHECK((transaction.calls ==
           QStringList{"open", "write", "flush", "sync", "commit"}));
    checkCursorOnlySamplingAdvanced(cursorsBefore, cursorsAfter);
  }

  constexpr std::array preparationPoints{
      ProductEnvelopeV9AtomicWritePreparationFailurePoint::EncodeInitial,
      ProductEnvelopeV9AtomicWritePreparationFailurePoint::ScanInitial,
      ProductEnvelopeV9AtomicWritePreparationFailurePoint::MaterializeInitial,
      ProductEnvelopeV9AtomicWritePreparationFailurePoint::EncodeFixedPoint,
      ProductEnvelopeV9AtomicWritePreparationFailurePoint::FixedPointMismatch,
      ProductEnvelopeV9AtomicWritePreparationFailurePoint::ScanFixedPoint,
      ProductEnvelopeV9AtomicWritePreparationFailurePoint::MaterializeFixedPoint,
      ProductEnvelopeV9AtomicWritePreparationFailurePoint::EncodeStable,
      ProductEnvelopeV9AtomicWritePreparationFailurePoint::StableMismatch};
  for (const auto point : preparationPoints) {
    const auto cursorsBefore = consumeCursors();
    Transaction transaction;
    const auto result = writeProductEnvelopeV9Atomically(
        *source, kProject, metadata(), transaction, point);
    const auto cursorsAfter = consumeCursors();
    CHECK(!result.succeeded());
    CHECK(transaction.calls.isEmpty());
    CHECK(transaction.destination == QByteArray("sentinel"));
    checkCursorOnlySamplingAdvanced(cursorsBefore, cursorsAfter);
  }

  constexpr std::array ioFailures{
      ProjectAtomicWriteFailure::OpenTemporary,
      ProjectAtomicWriteFailure::WriteTemporary,
      ProjectAtomicWriteFailure::FlushTemporary,
      ProjectAtomicWriteFailure::SyncTemporary,
      ProjectAtomicWriteFailure::ReplaceDestination};
  for (const auto failure : ioFailures) {
    const auto cursorsBefore = consumeCursors();
    Transaction transaction;
    transaction.fail = failure;
    const auto result = writeProductEnvelopeV9Atomically(
        *source, kProject, metadata(), transaction);
    const auto cursorsAfter = consumeCursors();
    CHECK(result.failure == failure);
    CHECK(transaction.calls.contains("cancel"));
    CHECK(transaction.destination == QByteArray("sentinel"));
    checkCursorOnlySamplingAdvanced(cursorsBefore, cursorsAfter);
  }

  const auto sourceAfter =
      encodeProductEnvelopeV9Root(*source, kProject, metadata());
  CHECK(sourceAfter.succeeded());
  CHECK(sourceAfter.payload->bytes == sourceBefore.payload->bytes);
  CHECK(scanProductEnvelopeV9(QByteArrayView(sourceAfter.payload->bytes))
            .hasCandidate());

  // A real mate-bearing source whose authored placements already contain the
  // solver result is a genuine fixed point and publishes normally.
  for (const int mateKind : {0, 1, 2}) {
    auto solvedSource = authoredMateProduct(mateKind);
    solveAuthoredMate(*solvedSource);
    const auto solvedBefore =
        encodeProductEnvelopeV9Root(*solvedSource, kProject, metadata());
    CHECK(solvedBefore.succeeded());
    CHECK(solvedBefore.payload->root.value("definitions")
              .toArray()[1]
              .toObject()
              .value("mates")
              .toArray()
              .size() == 1);
    const auto cursorsBefore = consumeCursors();
    Transaction transaction;
    const auto written = writeProductEnvelopeV9Atomically(
        *solvedSource, kProject, metadata(), transaction);
    const auto cursorsAfter = consumeCursors();
    CHECK(written.succeeded());
    CHECK(transaction.destination == solvedBefore.payload->bytes);
    CHECK((transaction.calls ==
           QStringList{"open", "write", "flush", "sync", "commit"}));
    checkCursorOnlySamplingAdvanced(cursorsBefore, cursorsAfter);
    const auto solvedAfter =
        encodeProductEnvelopeV9Root(*solvedSource, kProject, metadata());
    CHECK(solvedAfter.succeeded());
    CHECK(solvedAfter.payload->bytes == solvedBefore.payload->bytes);
  }

  // The same authored mate with an intentionally unsolved placement is an
  // organic B1 != B0 mismatch. It is rejected before transaction open and
  // neither the live source, destination nor any global ID cursor changes.
  for (const int mateKind : {0, 1, 2}) {
    auto unsolvedSource = authoredMateProduct(mateKind);
    const auto unsolvedBefore =
        encodeProductEnvelopeV9Root(*unsolvedSource, kProject, metadata());
    CHECK(unsolvedBefore.succeeded());
    const auto cursorsBefore = consumeCursors();
    Transaction transaction;
    const auto rejected = writeProductEnvelopeV9Atomically(
        *unsolvedSource, kProject, metadata(), transaction);
    const auto cursorsAfter = consumeCursors();
    CHECK(rejected.failure ==
          ProjectAtomicWriteFailure::SerializedPayloadValidation);
    CHECK(transaction.calls.isEmpty());
    CHECK(transaction.destination == QByteArray("sentinel"));
    checkCursorOnlySamplingAdvanced(cursorsBefore, cursorsAfter);
    const auto unsolvedAfter =
        encodeProductEnvelopeV9Root(*unsolvedSource, kProject, metadata());
    CHECK(unsolvedAfter.succeeded());
    CHECK(unsolvedAfter.payload->bytes == unsolvedBefore.payload->bytes);
  }
  return EXIT_SUCCESS;
}
