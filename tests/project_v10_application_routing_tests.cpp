#include "TestAssertions.h"

#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <TopoDS_Shape.hxx>

#include <array>
#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

#include "model/ExtrudeFeature.h"
#include "model/MirrorFeature.h"
#include "model/internal/ComponentIdentityTransitionTestAccess.h"
#include "project/ProductEnvelopeV10Decoder.h"
#include "project/ProductEnvelopeV11Decoder.h"
#include "project/ProductEnvelopeV11Encoder.h"
#include "project/ProductEnvelopeV12Decoder.h"
#include "project/ProjectFile.h"
#include "ui/application/ProjectApplicationService.h"

namespace {

using namespace solidar;

constexpr ProjectId kProject{
    {0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x10}};
constexpr ProjectId kForkProject{
    {0x22, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x11}};
constexpr ProjectId kThirdProject{
    {0x32, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x12}};

ProductDocumentIdentity identity(ProjectId id = kProject) {
  return {id, {"component fixture", "2026-10-31T00:00:00Z"}};
}

void writeBytes(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  CHECK(file.write(bytes) == bytes.size());
  file.close();
}

QByteArray readBytes(const QString& path) {
  QFile file(path);
  CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

QByteArray handAuthoredMinimalV10() {
  return QByteArray(R"JSON({
    "format":"solidar-project","version":10,
    "capabilities":["component-definitions-v1","nested-components-v1","product-envelope-v1"],
    "projectId":"123e4567-e89b-42d3-a456-426614174010",
    "rootDefinitionId":1,
    "metadata":{"name":"component fixture","createdAt":"2026-10-31T00:00:00Z"},
    "definitions":[{
      "definitionId":1,"definitionKind":"component",
      "payload":{"payloadKind":"embedded-component-v4","data":{
        "format":"solidar-project","version":4,
        "capabilities":["persistent-topology-v2"],
        "constraintTypeEncoding":"stable-name-v1",
        "name":"component fixture","createdAt":"2026-10-31T00:00:00Z",
        "document":{"widthMm":60,"heightMm":40,"extrusionMm":25},
        "sketches":[],"extrusion":{"enabled":false},
        "model":{"sketches":[],"bodies":[],"referenceImages":[]}
      }},
      "occurrences":[]
    }]
  })JSON");
}

std::unique_ptr<Document> authoredDocument(std::uint64_t base,
                                           bool topologyAndImage) {
  auto document = std::make_unique<Document>();
  auto& sketch = document->addSketch(SketchId{base + 1}, "profile");
  sketch.geometry.addRectangle({0, 0}, {12, 8});
  auto& body = document->addBody(BodyId{base + 2}, "body");
  auto extrude = std::make_unique<ExtrudeFeature>(
      FeatureId{base + 3}, sketch.id, 5.0, "extrude");
  auto* extrudePointer = extrude.get();
  body.addFeature(std::move(extrude));
  CHECK(document->recomputeDetailed().success);
  if (topologyAndImage) {
    const auto face = extrudePointer->createFaceReferenceV2(body.id(), 0);
    CHECK(face);
    CHECK(face.reference.isV2());
    body.addFeature(std::make_unique<MirrorFeature>(
        FeatureId{base + 4}, extrudePointer->id(), face.reference, "mirror"));
    ReferenceImage image;
    image.id = ReferenceImageId{base + 5};
    image.name = "reference";
    image.sourcePath = "image.png";
    image.supportName = "XY";
    image.placement = SketchPlacement::xy();
    image.pixelWidth = 640;
    image.pixelHeight = 480;
    document->addReferenceImage(std::move(image));
    CHECK(document->recomputeDetailed().success);
  }
  return document;
}

std::unique_ptr<ProductDocument> componentProduct(
    std::optional<ProductDocumentIdentity> productIdentity = identity()) {
  std::vector<ComponentDefinition> definitions;
  definitions.emplace_back(DefinitionId{1}, authoredDocument(1000, false));
  definitions.emplace_back(DefinitionId{2}, authoredDocument(2000, true));
  CHECK(definitions[1].insert(ComponentOccurrence(
            OccurrenceId{7}, DefinitionId{1}, RigidTransform3d::identity(),
            {true, false, true}, "child")) ==
        ComponentOccurrenceInsertStatus::Inserted);
  auto created = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{2}, std::move(productIdentity));
  CHECK(created);
  return std::move(created.product);
}

RigidTransform3d translated(double x, double y, double z) {
  auto created = RigidTransform3d::create({x, y, z}, {1.0, 0.0, 0.0, 0.0});
  CHECK(created);
  return *created.transform;
}

std::unique_ptr<ProductDocument> componentProductWithIndependentHome() {
  std::vector<ComponentDefinition> definitions;
  definitions.emplace_back(DefinitionId{1}, authoredDocument(1000, false));
  definitions.emplace_back(DefinitionId{2}, authoredDocument(2000, true));
  CHECK(definitions[1].insert(ComponentOccurrence(
            OccurrenceId{7}, DefinitionId{1}, translated(12.0, 5.0, -3.0),
            translated(2.0, 1.0, 4.0), OccurrenceFlags{}, "child")) ==
        ComponentOccurrenceInsertStatus::Inserted);
  auto created = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{2}, identity());
  CHECK(created);
  return std::move(created.product);
}

std::unique_ptr<ProductDocument> componentProductWithUnreachableDrawing(
    std::optional<ProductDocumentIdentity> productIdentity = identity()) {
  std::vector<ComponentDefinition> definitions;
  definitions.emplace_back(DefinitionId{1}, std::make_unique<Document>());
  std::vector<ComponentDrawing> drawings;
  drawings.emplace_back(
      DrawingId{71}, "unsupported drawing", ComponentDrawingPaper::A4,
      ComponentDrawingOrientation::Portrait, 1.0,
      std::vector<BodyId>{BodyId{7002}});
  definitions.emplace_back(DefinitionId{2}, authoredDocument(7000, false),
                           std::move(drawings));
  auto created = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{1}, std::move(productIdentity));
  CHECK(created);
  return std::move(created.product);
}

std::unique_ptr<ProductDocument> partProduct() {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{1}, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(std::move(definitions),
                                         DefinitionId{1}, identity());
  CHECK(created);
  return std::move(created.product);
}

class AcceptSuccessor final : public ProductDocumentSuccessor {
 public:
  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&,
      ProductDocumentSuccessorImpact) noexcept override {
    return ProductDocumentSuccessorStatus::Prepared;
  }
};

}  // namespace

int main() {
  QTemporaryDir directory(
      QDir::current().filePath(QStringLiteral("project-v10-routing-XXXXXX")));
  CHECK(directory.isValid());
  ProjectApplicationService service;
  CHECK(project::isWritableComponentPersistenceFormat(
      project::ProjectPersistenceFormat::V10));
  CHECK(project::isWritableComponentPersistenceFormat(
      project::ProjectPersistenceFormat::V11));
  CHECK(project::isWritableComponentPersistenceFormat(
      project::ProjectPersistenceFormat::V12));
  CHECK(!project::isWritableComponentPersistenceFormat(
      project::ProjectPersistenceFormat::V9));
  CHECK(!project::isWritableAssemblyPersistenceFormat(
      project::ProjectPersistenceFormat::V10));
  CHECK(!project::isWritableAssemblyPersistenceFormat(
      project::ProjectPersistenceFormat::V11));
  CHECK(!project::isWritableAssemblyPersistenceFormat(
      project::ProjectPersistenceFormat::V12));

  const QString partV11Path =
      directory.filePath(QStringLiteral("part-v11-rejected.solidar"));
  const QByteArray partV11Sentinel("part-v11-sentinel");
  writeBytes(partV11Path, partV11Sentinel);
  QString partV11Error;
  Document standalonePart;
  CHECK(!project::ProjectFile::saveDocument(
      partV11Path, standalonePart,
      {project::ProjectPersistenceFormat::V11}, &partV11Error));
  CHECK(readBytes(partV11Path) == partV11Sentinel);

  // The hand-authored envelope is routed to the strict V10 scanner and a
  // typed Component-root staged product. The legacy part-only ProjectFile
  // facade fails closed rather than pretending V10 is a part document.
  const QString handPath = directory.filePath(QStringLiteral("hand.solidar"));
  writeBytes(handPath, handAuthoredMinimalV10());
  const auto hand = service.stageOpenProduct(handPath);
  CHECK(hand.succeeded());
  CHECK(hand.staged->rootDefinitionKind() == DefinitionKind::Component);
  CHECK(hand.staged->persistenceState ==
        project::ProjectPersistenceState{project::ProjectPersistenceFormat::V10});
  const auto partOnly = project::ProjectFile::stageLoad(handPath);
  CHECK(!partOnly.succeeded());
  CHECK(partOnly.status == project::ProjectLoadStatus::Unsupported);

  // Root and nested Documents, image metadata and authored topology survive
  // the complete application save/open route at a byte-stable fixed point.
  auto product = componentProduct();
  const QString firstPath = directory.filePath(QStringLiteral("first.solidar"));
  CHECK(service.saveComponent(firstPath, *product).succeeded());
  const QByteArray firstBytes = readBytes(firstPath);
  CHECK(project::scanProductEnvelopeV10(QByteArrayView(firstBytes)).hasCandidate());
  auto opened = service.stageOpenProduct(firstPath);
  CHECK(opened.succeeded());
  CHECK(opened.staged->persistenceState ==
        project::ProjectPersistenceState{project::ProjectPersistenceFormat::V10});
  CHECK(opened.staged->productDocument().definitions().size() == 2);
  const auto* root = opened.staged->productDocument().rootComponentDefinition();
  CHECK(root);
  CHECK(root->document().bodies().size() == 1);
  CHECK(root->document().referenceImages().size() == 1);
  CHECK(root->document().bodies()[0].features().size() == 2);

  ProductDocument reopened =
      std::move(*opened.staged).releaseProductDocument();
  const QString fixedPath = directory.filePath(QStringLiteral("fixed.solidar"));
  CHECK(service.saveComponent(fixedPath, reopened).succeeded());
  CHECK(readBytes(fixedPath) == firstBytes);

  // Once opened as V11, an empty-drawings component graph remains V11.
  auto emptyV11Source = componentProduct();
  const auto emptyV11 = project::encodeProductEnvelopeV11Root(
      *emptyV11Source, kProject,
      {QStringLiteral("component fixture"),
       QStringLiteral("2026-10-31T00:00:00Z")});
  CHECK(emptyV11.succeeded());
  const QString emptyV11Path =
      directory.filePath(QStringLiteral("empty-v11.solidar"));
  writeBytes(emptyV11Path, emptyV11.payload->bytes);
  const auto directEmptyV11 = project::ProjectFile::stageLoad(emptyV11Path);
  CHECK(!directEmptyV11.succeeded());
  CHECK(directEmptyV11.status == project::ProjectLoadStatus::Unsupported);
  const auto partOnlyEmptyV11 = service.stageOpen(emptyV11Path);
  CHECK(!partOnlyEmptyV11.succeeded());
  CHECK(partOnlyEmptyV11.status.code ==
        ProjectApplicationErrorCode::UnsupportedProject);
  auto openedEmptyV11 = service.stageOpenProduct(emptyV11Path);
  CHECK(openedEmptyV11.succeeded());
  CHECK(openedEmptyV11.staged->persistenceState ==
        project::ProjectPersistenceState{
            project::ProjectPersistenceFormat::V11});
  ProductDocument retainedEmptyV11 =
      std::move(*openedEmptyV11.staged).releaseProductDocument();
  const QString retainedEmptyV11Path =
      directory.filePath(QStringLiteral("empty-v11-retained.solidar"));
  CHECK(service
            .saveComponent(
                retainedEmptyV11Path, retainedEmptyV11, std::nullopt,
                {project::ProjectPersistenceFormat::V11})
            .succeeded());
  CHECK(project::scanProductEnvelopeV11(
            QByteArrayView(readBytes(retainedEmptyV11Path)))
            .hasCandidate());

  // An independent home placement selects V12. Open, ordinary Save and Save
  // As all keep both parent-local transforms and never downgrade the carrier.
  auto homeSource = componentProductWithIndependentHome();
  const QString homePath =
      directory.filePath(QStringLiteral("home-v12.solidar"));
  CHECK(service.saveComponent(homePath, *homeSource).succeeded());
  CHECK(project::scanProductEnvelopeV12(QByteArrayView(readBytes(homePath)))
            .hasCandidate());
  auto openedHome = service.stageOpenProduct(homePath);
  CHECK(openedHome.succeeded());
  CHECK(openedHome.staged->persistenceState ==
        project::ProjectPersistenceState{
            project::ProjectPersistenceFormat::V12});
  const auto* openedHomeOccurrence =
      openedHome.staged->productDocument()
          .rootComponentDefinition()
          ->find(OccurrenceId{7});
  CHECK(openedHomeOccurrence);
  CHECK(openedHomeOccurrence->placement() == translated(12.0, 5.0, -3.0));
  CHECK(openedHomeOccurrence->homePlacement() == translated(2.0, 1.0, 4.0));
  ProductDocument retainedHome =
      std::move(*openedHome.staged).releaseProductDocument();
  const QString retainedHomePath =
      directory.filePath(QStringLiteral("home-v12-retained.solidar"));
  CHECK(service
            .saveComponent(
                retainedHomePath, retainedHome, std::nullopt,
                {project::ProjectPersistenceFormat::V12})
            .succeeded());
  CHECK(project::scanProductEnvelopeV12(
            QByteArrayView(readBytes(retainedHomePath)))
            .hasCandidate());

  auto stickyV12Source = componentProduct();
  const QString stickyV12Path =
      directory.filePath(QStringLiteral("home-v12-sticky.solidar"));
  CHECK(service
            .saveComponent(stickyV12Path, *stickyV12Source, std::nullopt,
                           {project::ProjectPersistenceFormat::V12})
            .succeeded());
  CHECK(project::scanProductEnvelopeV12(
            QByteArrayView(readBytes(stickyV12Path)))
            .hasCandidate());

  auto homeSaveAsSource = componentProductWithIndependentHome();
  const QString homeSaveAsPath =
      directory.filePath(QStringLiteral("home-v12-save-as.solidar"));
  CHECK(service
            .saveComponentAs(
                homeSaveAsPath, *homeSaveAsSource,
                ComponentSaveAsRequest{identity(kForkProject), homePath})
            .succeeded());
  CHECK(project::scanProductEnvelopeV12(
            QByteArrayView(readBytes(homeSaveAsPath)))
            .hasCandidate());
  CHECK(homeSaveAsSource->projectId() ==
        std::optional<ProjectId>{kForkProject});

  // The application save policy upgrades a component graph with drawings to
  // V11, including a drawing owned only by an unreachable definition. The
  // strict V10 encoder itself remains fail-closed in its codec tests.
  const QByteArray drawingSentinel("drawing-sentinel");
  auto drawingSource = componentProductWithUnreachableDrawing();
  const auto drawingIdentityBefore = drawingSource->identity();
  const QString drawingSavePath =
      directory.filePath(QStringLiteral("drawing-save.solidar"));
  writeBytes(drawingSavePath, drawingSentinel);
  CHECK(service.saveComponent(drawingSavePath, *drawingSource).succeeded());
  CHECK(project::scanProductEnvelopeV11(
            QByteArrayView(readBytes(drawingSavePath)))
            .hasCandidate());
  const auto reopenedDrawing = service.stageOpenProduct(drawingSavePath);
  CHECK(reopenedDrawing.succeeded());
  CHECK(reopenedDrawing.staged->persistenceState ==
        project::ProjectPersistenceState{
            project::ProjectPersistenceFormat::V11});
  CHECK(reopenedDrawing.staged->productDocument()
            .definitions()
            .findComponent(DefinitionId{2})
            ->drawings()
            .size() == 1);
  CHECK(drawingSource->identity() == drawingIdentityBefore);

  const QByteArray sourceBytesBeforeDrawingSaveAs = readBytes(firstPath);
  auto drawingSaveAsSource = componentProductWithUnreachableDrawing();
  const QString drawingSaveAsPath =
      directory.filePath(QStringLiteral("drawing-save-as.solidar"));
  writeBytes(drawingSaveAsPath, drawingSentinel);
  CHECK(service
            .saveComponentAs(
                drawingSaveAsPath, *drawingSaveAsSource,
                ComponentSaveAsRequest{identity(kForkProject), firstPath})
            .succeeded());
  CHECK(project::scanProductEnvelopeV11(
            QByteArrayView(readBytes(drawingSaveAsPath)))
            .hasCandidate());
  CHECK(readBytes(firstPath) == sourceBytesBeforeDrawingSaveAs);
  CHECK(drawingSaveAsSource->identity() == identity(kForkProject));

  auto unidentifiedDrawingSource =
      componentProductWithUnreachableDrawing(std::nullopt);
  const QString drawingFirstSavePath =
      directory.filePath(QStringLiteral("drawing-first-save.solidar"));
  writeBytes(drawingFirstSavePath, drawingSentinel);
  CHECK(service
            .saveComponent(drawingFirstSavePath,
                           *unidentifiedDrawingSource, identity())
            .succeeded());
  CHECK(project::scanProductEnvelopeV11(
            QByteArrayView(readBytes(drawingFirstSavePath)))
            .hasCandidate());
  CHECK(unidentifiedDrawingSource->identity() == identity());

  // Identity-changing persistence is sealed to one exact live carrier.
  // Invalid targets and invalid mode/source combinations are rejected before
  // any writer can touch the destination.
  auto sealSource = componentProduct();
  auto noIdentitySource = componentProduct(std::nullopt);
  auto invalidTarget = ComponentIdentityTransition::prepareFirstSave(
      *noIdentitySource, ProductDocumentIdentity{});
  CHECK(!invalidTarget);
  CHECK(invalidTarget.status ==
        ComponentIdentityTransitionStatus::InvalidTargetIdentity);
  CHECK(!ComponentIdentityTransition::prepareFirstSave(
      *sealSource, identity(kThirdProject)));
  CHECK(!ComponentIdentityTransition::prepareSaveAs(
      *noIdentitySource, identity(kThirdProject)));
  const auto sameTarget = ComponentIdentityTransition::prepareSaveAs(
      *sealSource, identity(kProject));
  CHECK(!sameTarget);
  CHECK(sameTarget.status ==
        ComponentIdentityTransitionStatus::SameProjectIdentity);

  auto validSeal = ComponentIdentityTransition::prepareSaveAs(
      *sealSource, identity(kThirdProject));
  CHECK(validSeal);
  auto foreignCarrier = componentProduct();
  const QString foreignPath =
      directory.filePath(QStringLiteral("foreign-seal.solidar"));
  CHECK(!service
             .saveComponentAsIdentityTransition(
                 foreignPath, *foreignCarrier, firstPath,
                 *validSeal.transition)
             .succeeded());
  CHECK(!QFile::exists(foreignPath));
  CHECK(foreignCarrier->projectId() == std::optional<ProjectId>{kProject});

  const auto rejectForgedSeal = [&](ComponentIdentityTransition forged,
                                    const QString& name) {
    const QString path = directory.filePath(name);
    CHECK(!service
               .saveComponentAsIdentityTransition(path, *sealSource,
                                                  firstPath, forged)
               .succeeded());
    CHECK(!QFile::exists(path));
    CHECK(sealSource->projectId() == std::optional<ProjectId>{kProject});
  };
  auto wrongIncarnation = *validSeal.transition;
  ComponentIdentityTransitionTestAccess::replaceSourceIncarnation(
      wrongIncarnation,
      ProductIncarnationId{sealSource->incarnation().value() + 1});
  rejectForgedSeal(std::move(wrongIncarnation),
                   QStringLiteral("wrong-incarnation.solidar"));
  auto wrongRoot = *validSeal.transition;
  ComponentIdentityTransitionTestAccess::replaceSourceRoot(
      wrongRoot, DefinitionId{sealSource->rootDefinitionId().value() + 1});
  rejectForgedSeal(std::move(wrongRoot),
                   QStringLiteral("wrong-root.solidar"));
  auto wrongState = *validSeal.transition;
  ComponentIdentityTransitionTestAccess::replaceFirstDefinitionState(
      wrongState, DefinitionStateId{999});
  rejectForgedSeal(std::move(wrongState),
                   QStringLiteral("wrong-state.solidar"));

  // A source mutation after planning invalidates the seal. Here the identity
  // bundle changes through the authoritative no-throw fork commit.
  auto mutatedSource = componentProduct();
  auto staleSeal = ComponentIdentityTransition::prepareSaveAs(
      *mutatedSource, identity(kForkProject));
  CHECK(staleSeal);
  CHECK(mutatedSource->commitForkIdentity(identity(kProject),
                                          identity(kThirdProject)) ==
        ProductDocumentForkIdentityStatus::Committed);
  const QString stalePath =
      directory.filePath(QStringLiteral("stale-seal.solidar"));
  CHECK(!service
             .saveComponentAsIdentityTransition(
                 stalePath, *mutatedSource, firstPath,
                 *staleSeal.transition)
             .succeeded());
  CHECK(!QFile::exists(stalePath));
  CHECK(mutatedSource->projectId() ==
        std::optional<ProjectId>{kThirdProject});

  // Definition-state changes are independently sealed. An authoritative
  // extraction after planning advances the root state and invalidates the
  // transition before the destination writer runs.
  auto stateMutatedSource = componentProduct();
  auto staleStateSeal = ComponentIdentityTransition::prepareSaveAs(
      *stateMutatedSource, identity(kForkProject));
  CHECK(staleStateSeal);
  const DefinitionId stateRoot = stateMutatedSource->rootDefinitionId();
  auto extraction = stateMutatedSource->prepareComponentExtraction(
      {stateMutatedSource->incarnation(), stateRoot, stateRoot,
       stateMutatedSource->definitionStateId(stateRoot), BodyId{2002},
       DefinitionId{9}, OccurrenceId{9}});
  CHECK(extraction);
  AcceptSuccessor accept;
  CHECK(stateMutatedSource->applyComponentExtraction(
            *extraction.transaction, accept) ==
        ComponentExtractionStatus::Committed);
  CHECK(!staleStateSeal.transition->isCurrentSource(*stateMutatedSource));
  const QString staleStatePath =
      directory.filePath(QStringLiteral("stale-state-seal.solidar"));
  CHECK(!service
             .saveComponentAsIdentityTransition(
                 staleStatePath, *stateMutatedSource, firstPath,
                 *staleStateSeal.transition)
             .succeeded());
  CHECK(!QFile::exists(staleStatePath));

  // The sealed first-save route commits exactly the target retained by the
  // seal, rather than accepting a second independently generated identity.
  auto sealedFirstSource = componentProduct(std::nullopt);
  auto firstSeal = ComponentIdentityTransition::prepareFirstSave(
      *sealedFirstSource, identity(kThirdProject));
  CHECK(firstSeal);
  const QString sealedFirstPath =
      directory.filePath(QStringLiteral("sealed-first.solidar"));
  CHECK(service
            .saveComponentIdentityTransition(
                sealedFirstPath, *sealedFirstSource,
                *firstSeal.transition)
            .succeeded());
  CHECK(firstSeal.transition->matchesCommittedProduct(*sealedFirstSource));
  CHECK(sealedFirstSource->projectId() ==
        std::optional<ProjectId>{kThirdProject});

  // Save As publishes a new exact project identity only after successful
  // atomic replacement. The reopened carrier remains explicitly V10.
  const QString forkPath = directory.filePath(QStringLiteral("fork.solidar"));
  const ComponentSaveAsRequest fork{identity(kForkProject), firstPath};
  CHECK(service.saveComponentAs(forkPath, *product, fork).succeeded());
  CHECK(product->projectId() == std::optional<ProjectId>{kForkProject});
  const auto forked = service.stageOpenProduct(forkPath);
  CHECK(forked.succeeded());
  CHECK(forked.staged->productDocument().projectId() ==
        std::optional<ProjectId>{kForkProject});

  // First save may publish a prepared identity. A malformed V10 and a Part
  // presented to the component writer both fail without replacing sentinels.
  auto unidentified = componentProduct(std::nullopt);
  const QString newPath = directory.filePath(QStringLiteral("new.solidar"));
  CHECK(service.saveComponent(newPath, *unidentified, identity()).succeeded());
  CHECK(unidentified->projectId() == std::optional<ProjectId>{kProject});

  QJsonObject malformed =
      QJsonDocument::fromJson(firstBytes).object();
  malformed[QStringLiteral("unexpected")] = true;
  const QString malformedPath =
      directory.filePath(QStringLiteral("malformed.solidar"));
  writeBytes(malformedPath,
             QJsonDocument(malformed).toJson(QJsonDocument::Compact));
  const auto rejected = service.stageOpenProduct(malformedPath);
  CHECK(!rejected.succeeded());
  CHECK(rejected.status.code == ProjectApplicationErrorCode::InvalidProject);
  CHECK(product->rootDefinitionKind() == DefinitionKind::Component);

  auto part = partProduct();
  const QString sentinelPath =
      directory.filePath(QStringLiteral("sentinel.solidar"));
  const QByteArray sentinel("sentinel");
  writeBytes(sentinelPath, sentinel);
  CHECK(!service.saveComponent(sentinelPath, *part).succeeded());
  CHECK(readBytes(sentinelPath) == sentinel);
  CHECK(!service.save(
                    sentinelPath, part->rootDocument(),
                    {project::ProjectPersistenceFormat::V10})
             .succeeded());
  CHECK(readBytes(sentinelPath) == sentinel);

  return EXIT_SUCCESS;
}
