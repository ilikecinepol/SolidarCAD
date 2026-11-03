#include "TestAssertions.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdlib>
#include <memory>
#include <utility>

#include "model/IdGeneration.h"
#include "model/OccurrenceDelta.h"
#include "project/ProductEnvelopeV6Encoder.h"
#include "project/ProductEnvelopeV6Materialization.h"

namespace {

using namespace solidar;
using namespace solidar::project;

constexpr ProjectId kProject{
    {0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00}};

ProductEnvelopeV5Metadata metadata() {
  return {QStringLiteral("assembly"),
          QStringLiteral("2026-10-23T00:00:00Z")};
}

QJsonObject fixtureRoot() {
  QFile file(QString::fromUtf8(SOLIDAR_MINIMAL_V6_FIXTURE_PATH));
  CHECK(file.open(QIODevice::ReadOnly));
  const auto parsed = QJsonDocument::fromJson(file.readAll());
  CHECK(parsed.isObject());
  return parsed.object();
}

std::unique_ptr<ProductDocument> assemblyProductWithSecondPlacement(
    RigidTransform3d secondPlacement, bool linkedSecond = false) {
  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{1}, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  if (linkedSecond) {
    SourceContentHash hash;
    hash.bytes[0] = 0x42;
    CHECK(definitions.insert(PartDefinition(
              DefinitionId{2}, std::make_unique<Document>(),
              LinkedPartBinding{{kProject, DefinitionId{77}},
                                "library/part.solidar", hash})) ==
          DefinitionInsertStatus::Inserted);
  } else {
    CHECK(definitions.insert(PartDefinition(
              DefinitionId{2}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
  }
  AssemblyDefinition assembly(DefinitionId{100});
  const auto identity = RigidTransform3d::identity();
  CHECK(assembly.insert(ComponentOccurrence(
            OccurrenceId{11}, DefinitionId{1}, identity,
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(assembly.insert(ComponentOccurrence(
            OccurrenceId{12}, DefinitionId{1}, std::move(secondPlacement),
            OccurrenceFlags{false, true, false})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(assembly)) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(std::move(definitions),
                                         DefinitionId{100});
  CHECK(created);
  return std::move(created.product);
}

std::unique_ptr<ProductDocument> assemblyProduct() {
  const auto rotated = RigidTransform3d::create(
      {10.0, 20.0, 30.0},
      {0.7071067811865476, 0.0, 0.0, 0.7071067811865476});
  CHECK(rotated);
  return assemblyProductWithSecondPlacement(*rotated.transform);
}

}  // namespace

int main() {
  auto source = assemblyProduct();
  const auto idsBefore = source->definitions().definitionIds();
  const auto* assemblyBefore = source->rootAssemblyDefinition();
  CHECK(assemblyBefore);
  const auto firstBeforePlacement =
      assemblyBefore->find(OccurrenceId{11})->placement();
  const auto firstBeforeFlags = assemblyBefore->find(OccurrenceId{11})->flags();
  const auto secondBeforePlacement =
      assemblyBefore->find(OccurrenceId{12})->placement();
  const auto secondBeforeFlags =
      assemblyBefore->find(OccurrenceId{12})->flags();

  const auto first = encodeProductEnvelopeV6Root(*source, kProject, metadata());
  const auto second = encodeProductEnvelopeV6Root(*source, kProject, metadata());
  CHECK(first.succeeded());
  CHECK(second.succeeded());
  CHECK(first.payload->bytes == second.payload->bytes);
  CHECK(first.payload->root == fixtureRoot());
  CHECK(source->definitions().definitionIds() == idsBefore);
  CHECK(source->rootAssemblyDefinition()
            ->find(OccurrenceId{11})
            ->placement() == firstBeforePlacement);
  CHECK(source->rootAssemblyDefinition()->find(OccurrenceId{11})->flags() ==
        firstBeforeFlags);
  CHECK(source->rootAssemblyDefinition()
            ->find(OccurrenceId{12})
            ->placement() == secondBeforePlacement);
  CHECK(source->rootAssemblyDefinition()->find(OccurrenceId{12})->flags() ==
        secondBeforeFlags);
  CHECK(source->definitions().findPart(DefinitionId{1})->document().bodies().empty());
  CHECK(source->definitions().findPart(DefinitionId{2})->document().bodies().empty());

  {
    const auto identity = RigidTransform3d::identity();
    auto linkedSource = assemblyProductWithSecondPlacement(identity, true);
    const auto rejected =
        encodeProductEnvelopeV6Root(*linkedSource, kProject, metadata());
    CHECK(rejected.status == ProductEnvelopeV6EncodeStatus::Invalid);
    CHECK(!rejected.payload);
    CHECK(rejected.error.contains(QStringLiteral("linked")));
    CHECK(linkedSource->definitions().findPart(DefinitionId{2})->isLinked());
  }

  const auto definitions =
      first.payload->root[QStringLiteral("definitions")].toArray();
  CHECK(definitions.size() == 3);
  CHECK(definitions.at(0).toObject()[QStringLiteral("definitionId")].toInt() ==
        1);
  CHECK(definitions.at(1).toObject()[QStringLiteral("definitionId")].toInt() ==
        2);
  const auto occurrences = definitions.at(2)
                               .toObject()[QStringLiteral("occurrences")]
                               .toArray();
  CHECK(occurrences.size() == 2);
  for (const auto occurrenceValue : occurrences) {
    const auto occurrence = occurrenceValue.toObject();
    CHECK(!occurrence.contains(QStringLiteral("payload")));
    CHECK(!occurrence.contains(QStringLiteral("brep")));
    CHECK(!occurrence.contains(QStringLiteral("mesh")));
  }
  CHECK(definitions.at(1).toObject().contains(QStringLiteral("payload")));

  // A5c5 Duplicate persists as two stable occurrence IDs targeting one
  // embedded Part definition.  Materialization must therefore restore one
  // authored Document, not clone it per occurrence.
  {
    ::solidar::detail::ScopedExplicitIdReservationPause pause;
    DefinitionRegistry duplicateDefinitions;
    CHECK(duplicateDefinitions.insert(PartDefinition(
              DefinitionId{7}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
    AssemblyDefinition duplicateAssembly(DefinitionId{70});
    CHECK(duplicateAssembly.insert(ComponentOccurrence(
              OccurrenceId{1}, DefinitionId{7},
              RigidTransform3d::identity(), {})) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(duplicateAssembly.insert(ComponentOccurrence(
              OccurrenceId{2}, DefinitionId{7},
              RigidTransform3d::identity(), {})) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(duplicateDefinitions.insert(std::move(duplicateAssembly)) ==
          DefinitionInsertStatus::Inserted);
    auto duplicateProduct = ProductDocument::create(
        std::move(duplicateDefinitions), DefinitionId{70});
    CHECK(duplicateProduct);
    const auto encodedDuplicate = encodeProductEnvelopeV6Root(
        *duplicateProduct.product, kProject, metadata());
    CHECK(encodedDuplicate.succeeded());
    const auto duplicateScan = scanProductEnvelopeV6(
        QByteArrayView(encodedDuplicate.payload->bytes));
    CHECK(duplicateScan.status == ProductEnvelopeV6ScanStatus::Candidate);
    CHECK(duplicateScan.candidate);
    CHECK(duplicateScan.candidate->parts.size() == 1);
    CHECK(duplicateScan.candidate->assembly);
    CHECK(duplicateScan.candidate->assembly->occurrences.size() == 2);
    CHECK(duplicateScan.candidate->assembly->occurrences[0].targetDefinitionId ==
          DefinitionId{7});
    CHECK(duplicateScan.candidate->assembly->occurrences[1].targetDefinitionId ==
          DefinitionId{7});
    auto restoredDuplicate =
        materializeProductEnvelopeV6(*duplicateScan.candidate);
    CHECK(restoredDuplicate.succeeded());
    const std::vector<DefinitionId> expectedDuplicateDefinitionIds{
        DefinitionId{7}, DefinitionId{70}};
    CHECK(restoredDuplicate.product->definitions().definitionIds() ==
          expectedDuplicateDefinitionIds);
    const auto* restoredPart =
        restoredDuplicate.product->definitions().findPart(DefinitionId{7});
    CHECK(restoredPart);
    const auto* restoredAssembly =
        restoredDuplicate.product->rootAssemblyDefinition();
    CHECK(restoredAssembly);
    CHECK(restoredAssembly->find(OccurrenceId{1})->targetDefinitionId() ==
          DefinitionId{7});
    CHECK(restoredAssembly->find(OccurrenceId{2})->targetDefinitionId() ==
          DefinitionId{7});
    CHECK(&restoredDuplicate.product->definitions()
               .findPart(restoredAssembly->find(OccurrenceId{1})
                             ->targetDefinitionId())
               ->document() == &restoredPart->document());
    CHECK(&restoredDuplicate.product->definitions()
               .findPart(restoredAssembly->find(OccurrenceId{2})
                             ->targetDefinitionId())
               ->document() == &restoredPart->document());
  }

  // A non-axis canonical transform survives the complete persistence path
  // exactly, including a defensive rescan and materialization/re-encode.
  {
    const auto canonical = RigidTransform3d::create(
        {4.0, 5.0, 6.0},
        {0.8716010645330563, 0.8716010645330545,
         -0.034945101736085116, 0.045766538112406385});
    CHECK(canonical);
    auto nonAxisSource =
        assemblyProductWithSecondPlacement(*canonical.transform);
    const auto encoded =
        encodeProductEnvelopeV6Root(*nonAxisSource, kProject, metadata());
    CHECK(encoded.succeeded());
    const auto scanned =
        scanProductEnvelopeV6(QByteArrayView(encoded.payload->bytes));
    CHECK(scanned.status == ProductEnvelopeV6ScanStatus::Candidate);
    CHECK(scanned.candidate);
    CHECK(scanned.candidate->assembly->occurrences[1].placement ==
          *canonical.transform);
    auto restored = materializeProductEnvelopeV6(*scanned.candidate);
    CHECK(restored.succeeded());
    const auto reencoded =
        encodeProductEnvelopeV6Root(*restored.product, kProject, metadata());
    CHECK(reencoded.succeeded());
    CHECK(reencoded.payload->root == encoded.payload->root);
    CHECK(restored.product->rootAssemblyDefinition()
              ->find(OccurrenceId{12})
              ->placement() == *canonical.transform);
  }

  // A5c6 Remove does not garbage-collect definitions.  V6 therefore keeps
  // an empty assembly root and every embedded orphan Part definition.
  {
    ::solidar::detail::ScopedExplicitIdReservationPause pause;
    DefinitionRegistry orphanDefinitions;
    CHECK(orphanDefinitions.insert(PartDefinition(
              DefinitionId{9}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
    CHECK(orphanDefinitions.insert(PartDefinition(
              DefinitionId{10}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
    CHECK(orphanDefinitions.insert(AssemblyDefinition(DefinitionId{90})) ==
          DefinitionInsertStatus::Inserted);
    auto emptyAssembly = ProductDocument::create(
        std::move(orphanDefinitions), DefinitionId{90});
    CHECK(emptyAssembly);
    const auto encodedEmpty = encodeProductEnvelopeV6Root(
        *emptyAssembly.product, kProject, metadata());
    CHECK(encodedEmpty.succeeded());
    const auto emptyScan = scanProductEnvelopeV6(
        QByteArrayView(encodedEmpty.payload->bytes));
    CHECK(emptyScan.status == ProductEnvelopeV6ScanStatus::Candidate);
    CHECK(emptyScan.candidate);
    CHECK(emptyScan.candidate->parts.size() == 2);
    CHECK(emptyScan.candidate->assembly);
    CHECK(emptyScan.candidate->assembly->occurrences.empty());
    auto restoredEmpty = materializeProductEnvelopeV6(*emptyScan.candidate);
    CHECK(restoredEmpty.succeeded());
    const std::vector<DefinitionId> expectedEmptyDefinitionIds{
        DefinitionId{9}, DefinitionId{10}, DefinitionId{90}};
    CHECK(restoredEmpty.product->definitions().definitionIds() ==
          expectedEmptyDefinitionIds);
    CHECK(restoredEmpty.product->rootAssemblyDefinition());
    CHECK(restoredEmpty.product->rootAssemblyDefinition()->occurrences().empty());
    CHECK(restoredEmpty.product->definitions().findPart(DefinitionId{9}));
    CHECK(restoredEmpty.product->definitions().findPart(DefinitionId{10}));
  }

  // V5 and standalone part roots are deliberately not silently promoted.
  DefinitionRegistry standaloneDefinitions;
  CHECK(standaloneDefinitions.insert(PartDefinition(
            DefinitionId{7}, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  auto standalone = ProductDocument::create(std::move(standaloneDefinitions),
                                            DefinitionId{7});
  CHECK(standalone);
  const auto rejected =
      encodeProductEnvelopeV6Root(*standalone.product, kProject, metadata());
  CHECK(rejected.status == ProductEnvelopeV6EncodeStatus::Invalid);
  CHECK(!rejected.payload);

  // V6 has no authored occurrence-name field. Refuse to serialize a renamed
  // model rather than silently dropping the A5c1 state.
  auto named = assemblyProduct();
  const auto namedPath = OccurrencePath::create({OccurrenceId{11}});
  CHECK(namedPath);
  auto rename = OccurrenceDelta::rename(
      {named->incarnation(), named->rootDefinitionId(),
       std::move(*namedPath.path), DefinitionId{1}},
      {}, "Корпус");
  CHECK(rename);
  CHECK(named->applyOccurrenceDelta(*rename.delta) ==
        OccurrenceDeltaStatus::Committed);
  const auto namedRejected =
      encodeProductEnvelopeV6Root(*named, kProject, metadata());
  CHECK(namedRejected.status ==
        ProductEnvelopeV6EncodeStatus::UnsupportedAuthoredOccurrenceName);
  CHECK(!namedRejected.payload);

  return EXIT_SUCCESS;
}
