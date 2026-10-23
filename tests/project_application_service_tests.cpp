#include "TestAssertions.h"

#include "ui/application/ProjectApplicationService.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "model/IdGeneration.h"
#include "model/ImportedShapeFeature.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/ProjectFile.h"

namespace {

solidar::Document boxDocument(const char* name = "Box") {
  solidar::Document document;
  auto& body = document.addBody(name);
  body.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape()),
      name));
  static_cast<void>(document.recompute());
  return document;
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
         file.write(bytes) == bytes.size() && file.flush();
}

QByteArray readBytes(const QString& path) {
  QFile file(path);
  CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

constexpr solidar::ProjectId kProductProjectId{
    {0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00}};
constexpr solidar::DefinitionId kProductRoot{4242};

solidar::ProductDocumentIdentity productIdentity() {
  return {kProductProjectId,
          {"Product \xCE\xA9/\xE9\x9B\xAA",
           "2026-10-23T12:34:56Z/\xD1\x82\xD0\xBE\xD1\x87\xD0\xBD\xD0\xBE"}};
}

std::unique_ptr<solidar::ProductDocument> product(
    solidar::Document document,
    std::optional<solidar::ProductDocumentIdentity> identity,
    solidar::DefinitionId root = kProductRoot) {
  solidar::DefinitionRegistry definitions;
  CHECK(definitions.insert(solidar::PartDefinition(
            root, std::make_unique<solidar::Document>(std::move(document)))) ==
        solidar::DefinitionInsertStatus::Inserted);
  auto created = solidar::ProductDocument::create(
      std::move(definitions), root, std::move(identity));
  CHECK(created);
  return std::move(created.product);
}

struct AutoIds {
  solidar::SketchId sketch{};
  solidar::ReferenceImageId image{};
  solidar::BodyId body{};
  solidar::FeatureId feature{};
};

AutoIds consumeAutoIds(const char* suffix) {
  solidar::Document document;
  AutoIds ids;
  ids.sketch = document.addSketch(std::string("sketch-") + suffix).id;
  ids.image = document
                  .addReferenceImage(std::string("image-") + suffix,
                                     "image.png", "XY",
                                     solidar::SketchPlacement::xy(), 10, 10)
                  .id;
  auto& body = document.addBody(std::string("body-") + suffix);
  ids.body = body.id();
  auto feature = std::make_unique<solidar::ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(2.0, 3.0, 4.0).Shape()),
      "auto-feature");
  ids.feature = feature->id();
  body.addFeature(std::move(feature));
  return ids;
}

void checkOnlyOneAutoAdvance(const AutoIds& before, const AutoIds& after) {
  CHECK(after.sketch == before.sketch + 1);
  CHECK(after.image == before.image + 1);
  CHECK(after.body == before.body + 1);
  CHECK(after.feature == before.feature + 1);
}

template <typename T>
concept HasLvalueProductRelease = requires(T& value) {
  value.releaseProductDocument();
};

template <typename T>
concept HasRvalueProductRelease = requires(T&& value) {
  std::move(value).releaseProductDocument();
};

template <typename T>
concept HasStagedRootRelease = requires(T&& value) {
  std::move(value).releaseRootDocument();
};

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("project-application-service-tests-XXXXXX")));
  CHECK(directory.isValid());

  const solidar::ProjectApplicationService service;
  static_assert(!std::is_default_constructible_v<
                solidar::StagedApplicationDocument>);
  static_assert(!std::is_copy_constructible_v<
                solidar::StagedApplicationDocument>);
  static_assert(!std::is_copy_assignable_v<
                solidar::StagedApplicationDocument>);
  static_assert(std::is_nothrow_move_constructible_v<
                solidar::StagedApplicationDocument>);
  static_assert(std::is_nothrow_move_assignable_v<
                solidar::StagedApplicationDocument>);
  static_assert(!HasLvalueProductRelease<solidar::StagedApplicationDocument>);
  static_assert(HasRvalueProductRelease<solidar::StagedApplicationDocument>);
  static_assert(!HasStagedRootRelease<solidar::StagedApplicationDocument>);
  static_assert(std::is_same_v<
                decltype(std::declval<solidar::StagedApplicationDocument&&>()
                             .releaseProductDocument()),
                solidar::ProductDocument>);
  static_assert(noexcept(std::declval<solidar::StagedApplicationDocument&&>()
                             .releaseProductDocument()));
  static_assert(noexcept(service.stageOpen(QString{})));
  static_assert(noexcept(service.save(QString{},
                                      std::declval<const solidar::Document&>())));
  static_assert(noexcept(service.save(
      QString{}, std::declval<const solidar::Document&>(),
      solidar::project::ProjectPersistenceState{})));
  static_assert(noexcept(service.save(
      QString{}, std::declval<const solidar::ProductDocument&>())));
  static_assert(noexcept(service.upgradeProjectFormat(
      QString{}, std::declval<const solidar::Document&>(),
      solidar::project::ProjectPersistenceState{})));
  static_assert(noexcept(service.stageImportStep(
      QString{}, std::declval<const solidar::Document&>(), QString{})));
  static_assert(noexcept(service.stageImportStep(
      QString{}, std::declval<const solidar::ProductDocument&>(),
      QString{})));

  // A ready carrier is moved into staging without rebuilding or shadowing its
  // authoritative identity/root ownership.
  auto readyCarrier =
      product(boxDocument("Ready carrier"), productIdentity());
  const auto readyIdentity = *readyCarrier->identity();
  const auto* const readyDocument = &readyCarrier->rootDocument();
  const auto* const readyBody = &readyDocument->bodies().front();
  const auto* const readyFeature = readyBody->features().front().get();
  solidar::StagedApplicationDocument directlyStaged(
      std::move(*readyCarrier), std::nullopt,
      solidar::project::ProjectPersistenceState{
          solidar::project::ProjectPersistenceFormat::V5});
  CHECK(directlyStaged.productDocument().identity() == readyIdentity);
  CHECK(directlyStaged.productDocument().rootDefinitionId() == kProductRoot);
  CHECK(&directlyStaged.rootDocument() == readyDocument);
  CHECK(&directlyStaged.rootDocument().bodies().front() == readyBody);
  CHECK(directlyStaged.rootDocument()
            .bodies()
            .front()
            .features()
            .front()
            .get() == readyFeature);

  // Rejected staging cannot mutate caller-owned committed state.
  auto committed = boxDocument("Committed");
  const auto committedBodyId = committed.bodies().front().id();
  const QString corruptPath = directory.filePath(QStringLiteral("corrupt.solidar"));
  CHECK(writeBytes(corruptPath, QByteArrayLiteral("{broken")));
  const auto corrupt = service.stageOpen(corruptPath);
  CHECK(!corrupt.succeeded());
  CHECK(corrupt.status.code ==
        solidar::ProjectApplicationErrorCode::InvalidProject);
  CHECK(!corrupt.staged);
  CHECK(committed.bodies().size() == 1);
  CHECK(committed.bodies().front().id() == committedBodyId);

  const QString unsupportedPath =
      directory.filePath(QStringLiteral("unsupported.solidar"));
  CHECK(writeBytes(
      unsupportedPath,
      QByteArrayLiteral(
          R"({"format":"solidar-project","version":99})")));
  const auto unsupported = service.stageOpen(unsupportedPath);
  CHECK(!unsupported.succeeded());
  CHECK(unsupported.status.code ==
        solidar::ProjectApplicationErrorCode::UnsupportedProject);
  CHECK(!unsupported.staged);

  const QString malformedV3Path =
      directory.filePath(QStringLiteral("malformed-v3.solidar"));
  CHECK(writeBytes(
      malformedV3Path,
      QByteArrayLiteral(
          R"({"format":"solidar-project","version":3,"constraintTypeEncoding":"stable-name-v1"})")));
  const auto malformedV3 = service.stageOpen(malformedV3Path);
  CHECK(!malformedV3.succeeded());
  CHECK(malformedV3.status.code ==
        solidar::ProjectApplicationErrorCode::InvalidProject);
  CHECK(!malformedV3.staged);

  const QString unknownSchemaV3Path =
      directory.filePath(QStringLiteral("unknown-schema-v3.solidar"));
  CHECK(writeBytes(
      unknownSchemaV3Path,
      QByteArrayLiteral(
          R"({"format":"solidar-project","version":3,"constraintTypeEncoding":"stable-name-v1","sketches":[{"schema":"future-sketch-v9"}]})")));
  const auto unknownSchemaV3 = service.stageOpen(unknownSchemaV3Path);
  CHECK(!unknownSchemaV3.succeeded());
  CHECK(unknownSchemaV3.status.code ==
        solidar::ProjectApplicationErrorCode::UnsupportedProject);
  CHECK(!unknownSchemaV3.staged);
  CHECK(committed.bodies().size() == 1);
  CHECK(committed.bodies().front().id() == committedBodyId);

  // Historical v1 becomes a complete staged Document and carries the legacy
  // extrusion presentation reference without exposing ProjectFile DTOs to UI.
  solidar::project::ProjectData legacy;
  legacy.box = {40.0, 30.0, 20.0};
  solidar::sketch::Sketch legacySketch;
  legacySketch.addRectangle({0.0, 0.0}, {12.0, 8.0});
  legacy.sketches.push_back(
      {std::move(legacySketch), QString::fromUtf8("Верхняя")});
  legacy.hasExtrusion = true;
  legacy.extrusionSourceSketch = 0;
  const QString v1Path = directory.filePath(QStringLiteral("historical-v1.solidar"));
  QString fixtureError;
  CHECK(solidar::project::ProjectFile::save(v1Path, legacy, &fixtureError));
  auto openedV1 = service.stageOpen(v1Path);
  CHECK(openedV1.succeeded());
  CHECK(openedV1.staged->rootDocument().sketches().size() == 1);
  CHECK(openedV1.staged->historicalLegacyExtrusionSourceSketchId ==
        openedV1.staged->rootDocument().sketches().front().id);
  CHECK(openedV1.staged->rootDocument()
            .sketches()
            .front()
            .placement.origin.z == 20.0);
  CHECK(openedV1.staged->persistenceState ==
        solidar::project::ProjectPersistenceState{
            solidar::project::ProjectPersistenceFormat::V3});

  // Historical v2 is still accepted through the service boundary and staged
  // into the same transient root carrier as every other supported version.
  const QString v2Path =
      directory.filePath(QStringLiteral("hand-authored-v2.solidar"));
  const QJsonObject v2Root{
      {"format", QStringLiteral("solidar-project")},
      {"version", 2},
      {"name", QStringLiteral("hand-authored-v2")},
      {"createdAt", QStringLiteral("2026-10-18T00:00:00Z")},
      {"document", QJsonObject{{"widthMm", 60.0},
                                {"heightMm", 40.0},
                                {"extrusionMm", 25.0}}},
      {"sketches", QJsonArray{}},
      {"extrusion", QJsonObject{{"enabled", false}}},
      {"model", QJsonObject{{"sketches", QJsonArray{}},
                              {"bodies", QJsonArray{}},
                              {"referenceImages", QJsonArray{}}}}};
  CHECK(writeBytes(v2Path,
                   QJsonDocument(v2Root).toJson(QJsonDocument::Compact)));
  auto openedV2 = service.stageOpen(v2Path);
  CHECK(openedV2.succeeded());
  CHECK(openedV2.staged->rootDocument().bodies().empty());
  CHECK(openedV2.staged->persistenceState ==
        solidar::project::ProjectPersistenceState{
            solidar::project::ProjectPersistenceFormat::V3});

  // Canonical v3 save/open round-trip keeps parametric document ownership in
  // the staged result until the caller explicitly commits it.
  const QString v3Path = directory.filePath(QStringLiteral("canonical-v3.solidar"));
  const auto saved = service.save(v3Path, committed);
  CHECK(saved.succeeded());
  auto openedV3 = service.stageOpen(v3Path);
  CHECK(openedV3.succeeded());
  CHECK(openedV3.staged->rootDocument().bodies().size() == 1);
  CHECK(openedV3.staged->rootDocument()
            .bodies()
            .front()
            .features()
            .size() == 1);
  CHECK(!openedV3.staged->historicalLegacyExtrusionSourceSketchId);
  CHECK(openedV3.staged->persistenceState ==
        solidar::project::ProjectPersistenceState{
            solidar::project::ProjectPersistenceFormat::V3});
  const auto* const stagedDocument = &openedV3.staged->rootDocument();
  const auto* const stagedBody = &stagedDocument->bodies().front();
  const auto* const stagedFeature = stagedBody->features().front().get();
  const auto* const stagedShapeFeature =
      dynamic_cast<const solidar::ShapeFeature*>(stagedFeature);
  CHECK(stagedShapeFeature);
  const auto* const stagedShape = stagedShapeFeature->lastValidShape().get();
  std::string topologyError;
  const auto stagedTopology =
      stagedShapeFeature->lastValidTopologyIndex(&topologyError);
  CHECK(stagedTopology);
  CHECK(topologyError.empty());
  const auto* const stagedTopologyAddress = stagedTopology.get();
  const auto stagedRevision = stagedShapeFeature->shapeRevision();

  auto releasedV3 =
      std::move(*openedV3.staged).releaseProductDocument();
  CHECK(&releasedV3.rootDocument() == stagedDocument);
  CHECK(&releasedV3.rootDocument().bodies().front() == stagedBody);
  CHECK(releasedV3.rootDocument().bodies().front().features().front().get() ==
        stagedFeature);
  const auto* const releasedShapeFeature =
      dynamic_cast<const solidar::ShapeFeature*>(stagedFeature);
  CHECK(releasedShapeFeature);
  CHECK(releasedShapeFeature->lastValidShape().get() == stagedShape);
  CHECK(releasedShapeFeature->shapeRevision() == stagedRevision);
  CHECK(releasedShapeFeature->lastValidTopologyIndex().get() ==
        stagedTopologyAddress);

  const QString v4Path =
      directory.filePath(QStringLiteral("canonical-v4.solidar"));
  const solidar::project::ProjectPersistenceState v4{
      solidar::project::ProjectPersistenceFormat::V4};
  CHECK(service.save(v4Path, committed, v4).succeeded());
  auto openedV4 = service.stageOpen(v4Path);
  CHECK(openedV4.succeeded());
  CHECK(openedV4.staged->persistenceState == v4);

  const QString standaloneV5Path =
      directory.filePath(QStringLiteral("standalone-v5-rejected.solidar"));
  const QByteArray standaloneSentinel("standalone-sentinel");
  CHECK(writeBytes(standaloneV5Path, standaloneSentinel));
  const auto standaloneV5 = service.save(
      standaloneV5Path, committed,
      {solidar::project::ProjectPersistenceFormat::V5});
  CHECK(!standaloneV5.succeeded());
  CHECK(standaloneV5.code ==
        solidar::ProjectApplicationErrorCode::SaveFailed);
  CHECK(readBytes(standaloneV5Path) == standaloneSentinel);

  // Identified one-definition products use the v5 path. The service preserves
  // the authoritative carrier exactly, and speculative open leaves every
  // process-global edit-ID cursor untouched.
  const QString v5Path =
      directory.filePath(QStringLiteral("canonical-v5.solidar"));
  auto identified = product(boxDocument("Product root"), productIdentity());
  const auto sourceIdentity = *identified->identity();
  const auto* const sourceDocument = &identified->rootDocument();
  const auto* const sourceBody = &sourceDocument->bodies().front();
  const auto* const sourceFeature = sourceBody->features().front().get();
  const auto* const sourceShape = sourceFeature->lastValidShape().get();
  CHECK(service.save(v5Path, *identified).succeeded());
  CHECK(identified->identity() == sourceIdentity);
  CHECK(identified->rootDefinitionId() == kProductRoot);
  CHECK(&identified->rootDocument() == sourceDocument);
  CHECK(sourceBody->features().front().get() == sourceFeature);
  CHECK(sourceFeature->lastValidShape().get() == sourceShape);

  const QByteArray v5Bytes = readBytes(v5Path);
  const auto scannedV5 = solidar::project::scanProductEnvelopeV5(
      QByteArrayView(v5Bytes));
  CHECK(scannedV5.status ==
        solidar::project::ProductEnvelopeV5ScanStatus::Candidate);
  CHECK(scannedV5.candidate);
  CHECK(scannedV5.candidate->projectId == kProductProjectId);
  CHECK(scannedV5.candidate->rootDefinitionId == kProductRoot);
  CHECK(scannedV5.candidate->definitionId == kProductRoot);
  CHECK(scannedV5.candidate->metadata.name ==
        QString::fromUtf8("Product Ω/雪"));
  CHECK(scannedV5.candidate->metadata.createdAt ==
        QString::fromUtf8("2026-10-23T12:34:56Z/точно"));

  const AutoIds beforeV5Open = consumeAutoIds("before-v5-open");
  const auto defaultV5 = service.stageOpen(v5Path);
  CHECK(!defaultV5.succeeded());
  CHECK(defaultV5.status.code ==
        solidar::ProjectApplicationErrorCode::UnsupportedProject);
  CHECK(!defaultV5.staged);
  auto openedV5 = service.stageOpen(
      v5Path, solidar::ProductV5OpenPolicy::Allow);
  const AutoIds afterV5Open = consumeAutoIds("after-v5-open");
  checkOnlyOneAutoAdvance(beforeV5Open, afterV5Open);
  CHECK(openedV5.succeeded());
  CHECK(openedV5.staged->persistenceState ==
        solidar::project::ProjectPersistenceState{
            solidar::project::ProjectPersistenceFormat::V5});
  CHECK(openedV5.staged->productDocument().identity() == sourceIdentity);
  CHECK(openedV5.staged->productDocument().rootDefinitionId() ==
        kProductRoot);
  CHECK(openedV5.staged->productDocument().definitions().size() == 1);
  CHECK(openedV5.staged->rootDocument().bodies().size() == 1);

  // A v5-shaped malformed envelope is Invalid. A structurally valid envelope
  // with an unsupported required capability is Unsupported.
  QJsonObject malformedV5Root =
      QJsonDocument::fromJson(v5Bytes).object();
  malformedV5Root.insert(QStringLiteral("rootDefinitionId"), 4243);
  const QString malformedV5Path =
      directory.filePath(QStringLiteral("malformed-v5.solidar"));
  CHECK(writeBytes(malformedV5Path,
                   QJsonDocument(malformedV5Root)
                       .toJson(QJsonDocument::Compact)));
  const auto malformedV5 = service.stageOpen(
      malformedV5Path, solidar::ProductV5OpenPolicy::Allow);
  CHECK(!malformedV5.succeeded());
  CHECK(malformedV5.status.code ==
        solidar::ProjectApplicationErrorCode::InvalidProject);
  CHECK(!malformedV5.staged);

  QJsonObject unsupportedV5Root =
      QJsonDocument::fromJson(v5Bytes).object();
  unsupportedV5Root.insert(
      QStringLiteral("capabilities"),
      QJsonArray{QStringLiteral("product-envelope-v1"),
                 QStringLiteral("zzz-required-capability")});
  const QString unsupportedV5Path =
      directory.filePath(QStringLiteral("unsupported-v5.solidar"));
  CHECK(writeBytes(unsupportedV5Path,
                   QJsonDocument(unsupportedV5Root)
                       .toJson(QJsonDocument::Compact)));
  const auto unsupportedV5 = service.stageOpen(
      unsupportedV5Path, solidar::ProductV5OpenPolicy::Allow);
  CHECK(!unsupportedV5.succeeded());
  CHECK(unsupportedV5.status.code ==
        solidar::ProjectApplicationErrorCode::UnsupportedProject);
  CHECK(!unsupportedV5.staged);

  // Product save preconditions and publication failures preserve both the
  // existing destination and every source carrier address/value.
  const QString rejectedProductPath =
      directory.filePath(QStringLiteral("rejected-product.solidar"));
  const QByteArray destinationSentinel("existing-destination");
  CHECK(writeBytes(rejectedProductPath, destinationSentinel));
  auto identityless = product(boxDocument("Identityless"), std::nullopt);
  const auto* const identitylessDocument = &identityless->rootDocument();
  const auto* const identitylessFeature =
      identitylessDocument->bodies().front().features().front().get();
  const auto rejectedIdentityless =
      service.save(rejectedProductPath, *identityless);
  CHECK(!rejectedIdentityless.succeeded());
  CHECK(rejectedIdentityless.code ==
        solidar::ProjectApplicationErrorCode::SaveFailed);
  CHECK(readBytes(rejectedProductPath) == destinationSentinel);
  CHECK(!identityless->identity());
  CHECK(&identityless->rootDocument() == identitylessDocument);
  CHECK(identitylessDocument->bodies().front().features().front().get() ==
        identitylessFeature);

  auto multiple = product(boxDocument("Multiple"), productIdentity());
  CHECK(multiple->insert(solidar::PartDefinition(
            solidar::DefinitionId{4243},
            std::make_unique<solidar::Document>())) ==
        solidar::DefinitionInsertStatus::Inserted);
  const auto multipleIdentity = *multiple->identity();
  const auto* const multipleRoot = &multiple->rootDocument();
  const auto rejectedMultiple =
      service.save(rejectedProductPath, *multiple);
  CHECK(!rejectedMultiple.succeeded());
  CHECK(rejectedMultiple.code ==
        solidar::ProjectApplicationErrorCode::SaveFailed);
  CHECK(readBytes(rejectedProductPath) == destinationSentinel);
  CHECK(multiple->identity() == multipleIdentity);
  CHECK(multiple->definitions().size() == 2);
  CHECK(&multiple->rootDocument() == multipleRoot);

  const QString failedProductPath =
      directory.filePath(QStringLiteral("missing/product-v5.solidar"));
  const auto failedProductSave = service.save(failedProductPath, *identified);
  CHECK(!failedProductSave.succeeded());
  CHECK(failedProductSave.code ==
        solidar::ProjectApplicationErrorCode::SaveFailed);
  CHECK(!QFile::exists(failedProductPath));
  CHECK(identified->identity() == sourceIdentity);
  CHECK(&identified->rootDocument() == sourceDocument);
  CHECK(sourceBody->features().front().get() == sourceFeature);
  CHECK(sourceFeature->lastValidShape().get() == sourceShape);

  // Explicit upgrade uses the existing atomic v4 writer. A V4 call is an
  // exact no-op, while a failed V3 upgrade never manufactures a destination.
  const QString upgradedPath =
      directory.filePath(QStringLiteral("explicit-upgrade.solidar"));
  CHECK(service.save(upgradedPath, committed).succeeded());
  CHECK(service.upgradeProjectFormat(
                    upgradedPath, committed,
                    {solidar::project::ProjectPersistenceFormat::V3})
            .succeeded());
  QFile upgradedFile(upgradedPath);
  CHECK(upgradedFile.open(QIODevice::ReadOnly));
  const auto upgradedRoot =
      QJsonDocument::fromJson(upgradedFile.readAll()).object();
  CHECK(upgradedRoot.value(QStringLiteral("version")).toInt() == 4);

  const QString noOpPath =
      directory.filePath(QStringLiteral("already-v4-no-op.solidar"));
  CHECK(service.upgradeProjectFormat(noOpPath, solidar::Document{}, v4)
            .succeeded());
  CHECK(!QFile::exists(noOpPath));

  const QString v5NoOpPath =
      directory.filePath(QStringLiteral("already-v5-no-op.solidar"));
  CHECK(service.upgradeProjectFormat(
                    v5NoOpPath, solidar::Document{},
                    {solidar::project::ProjectPersistenceFormat::V5})
            .succeeded());
  CHECK(!QFile::exists(v5NoOpPath));

  const QString missingParent =
      directory.filePath(QStringLiteral("missing/upgrade.solidar"));
  const auto failedUpgrade = service.upgradeProjectFormat(
      missingParent, committed,
      {solidar::project::ProjectPersistenceFormat::V3});
  CHECK(!failedUpgrade.succeeded());
  CHECK(failedUpgrade.code == solidar::ProjectApplicationErrorCode::SaveFailed);
  CHECK(!QFile::exists(missingParent));

  // STEP import is also staged against a copy. Both successful and failed
  // translations leave the supplied current Document unchanged.
  const QString stepPath = directory.filePath(QStringLiteral("source.step"));
  CHECK(service.exportStep(stepPath, committed).succeeded());
  solidar::Document importBase;
  importBase.addBody("Existing");
  const auto existingId = importBase.bodies().front().id();
  auto imported =
      service.stageImportStep(stepPath, importBase, QStringLiteral("Imported"));
  CHECK(imported.succeeded());
  CHECK(importBase.bodies().size() == 1);
  CHECK(importBase.bodies().front().id() == existingId);
  CHECK(imported.staged->rootDocument().bodies().size() == 2);
  CHECK(!imported.staged->persistenceState);

  const auto failedImport = service.stageImportStep(
      directory.filePath(QStringLiteral("missing.step")), importBase,
      QStringLiteral("Missing"));
  CHECK(!failedImport.succeeded());
  CHECK(failedImport.status.code ==
        solidar::ProjectApplicationErrorCode::StepImportFailed);
  CHECK(!failedImport.staged);
  CHECK(importBase.bodies().size() == 1);
  CHECK(importBase.bodies().front().id() == existingId);

  // Product-aware STEP staging retains authoritative identity/root ownership
  // while importing into a detached Document copy.
  auto productImportSource =
      product(boxDocument("Product import source"), productIdentity());
  const auto productImportIdentity = *productImportSource->identity();
  const auto* const productImportDocument =
      &productImportSource->rootDocument();
  const auto* const productImportFeature =
      productImportDocument->bodies().front().features().front().get();
  const auto* const productImportShape =
      productImportFeature->lastValidShape().get();
  auto productImported = service.stageImportStep(
      stepPath, *productImportSource, QStringLiteral("Product imported"));
  CHECK(productImported.succeeded());
  CHECK(productImported.staged->productDocument().identity() ==
        productImportIdentity);
  CHECK(productImported.staged->productDocument().rootDefinitionId() ==
        kProductRoot);
  CHECK(productImported.staged->productDocument().definitions().size() == 1);
  CHECK(productImported.staged->rootDocument().bodies().size() == 2);
  CHECK(productImportSource->identity() == productImportIdentity);
  CHECK(productImportSource->rootDefinitionId() == kProductRoot);
  CHECK(&productImportSource->rootDocument() == productImportDocument);
  CHECK(productImportDocument->bodies().size() == 1);
  CHECK(productImportDocument->bodies().front().features().front().get() ==
        productImportFeature);
  CHECK(productImportFeature->lastValidShape().get() == productImportShape);

  const auto failedProductImport = service.stageImportStep(
      directory.filePath(QStringLiteral("missing-product.step")),
      *productImportSource, QStringLiteral("Missing product"));
  CHECK(!failedProductImport.succeeded());
  CHECK(failedProductImport.status.code ==
        solidar::ProjectApplicationErrorCode::StepImportFailed);
  CHECK(!failedProductImport.staged);
  CHECK(productImportSource->identity() == productImportIdentity);
  CHECK(&productImportSource->rootDocument() == productImportDocument);
  CHECK(productImportDocument->bodies().size() == 1);
  CHECK(productImportDocument->bodies().front().features().front().get() ==
        productImportFeature);

  // Copying a product with high persisted explicit IDs for a failed
  // speculative STEP import must not reserve the process-global edit cursor.
  solidar::Document highIdDocument;
  constexpr solidar::BodyId kHighBodyId =
      static_cast<solidar::BodyId>(
          solidar::project::ProjectFile::kMaximumPersistedId - 10);
  {
    solidar::detail::ScopedExplicitIdReservationPause reservationPause;
    highIdDocument.addBody(kHighBodyId, "high persisted body");
  }
  auto highIdProduct =
      product(std::move(highIdDocument), productIdentity());
  const AutoIds beforeFailedHighIdImport =
      consumeAutoIds("before-failed-high-id-import");
  const auto failedHighIdImport = service.stageImportStep(
      directory.filePath(QStringLiteral("missing-high-id.step")),
      *highIdProduct, QStringLiteral("Missing high ID"));
  const AutoIds afterFailedHighIdImport =
      consumeAutoIds("after-failed-high-id-import");
  CHECK(!failedHighIdImport.succeeded());
  CHECK(failedHighIdImport.status.code ==
        solidar::ProjectApplicationErrorCode::StepImportFailed);
  CHECK(!failedHighIdImport.staged);
  checkOnlyOneAutoAdvance(beforeFailedHighIdImport,
                          afterFailedHighIdImport);
  CHECK(highIdProduct->rootDocument().bodies().size() == 1);
  CHECK(highIdProduct->rootDocument().bodies().front().id() == kHighBodyId);

  const auto preflight = service.preflightStlExport(committed);
  CHECK(preflight.succeeded());
  const QString stlPath = directory.filePath(QStringLiteral("source.stl"));
  CHECK(service.exportStl(stlPath, committed).succeeded());
  CHECK(QFile::exists(stlPath));

  const auto emptyPreflight =
      service.preflightStlExport(solidar::Document{});
  CHECK(!emptyPreflight.succeeded());
  CHECK(emptyPreflight.code ==
        solidar::ProjectApplicationErrorCode::NoExportableShapes);

  return EXIT_SUCCESS;
}
