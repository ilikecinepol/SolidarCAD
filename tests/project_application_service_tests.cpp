#include "TestAssertions.h"

#include "ui/application/ProjectApplicationService.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdlib>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "model/IdGeneration.h"
#include "model/ImportedShapeFeature.h"
#include "model/JoinBodiesFeature.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/ProductEnvelopeV6Encoder.h"
#include "project/ProductEnvelopeV5Encoder.h"
#include "project/ProductEnvelopeV7Decoder.h"
#include "project/ProductEnvelopeV7Encoder.h"
#include "project/ProductEnvelopeV8Decoder.h"
#include "project/ProductEnvelopeV8Encoder.h"
#include "project/ProductEnvelopeV9Decoder.h"
#include "project/ProductEnvelopeV9Encoder.h"
#include "project/LinkedPartSourceResolver.h"
#include "project/ProjectFile.h"
#include "ui/application/AssemblyCommandHistory.h"
#include "ui/application/internal/AssemblyLinkedOpenPolicy.h"
#include "ui/application/internal/AssemblySaveAsPathMetadata.h"
#include "ui/application/LinkedPartLifecycleService.h"

namespace {

using PathRelation =
    solidar::application::detail::AssemblySaveAsPathRelation;
using MissingPathResolution =
    solidar::application::detail::MissingPathResolution;
using MissingPathResolutionStatus =
    solidar::application::detail::MissingPathResolutionStatus;
using PathNameComparisonPolicy =
    solidar::application::detail::PathNameComparisonPolicy;

class FakeAssemblySaveAsPathMetadataReader final
    : public solidar::application::detail::
          AssemblySaveAsPathMetadataReader {
 public:
  struct Rule final {
    QString left;
    QString right;
    PathRelation relation{PathRelation::Missing};
  };

  struct MissingRule final {
    QString path;
    MissingPathResolution resolution;
  };

  void addFileIdentity(QString left, QString right, PathRelation relation) {
    fileIdentity_.push_back(
        {QDir::cleanPath(std::move(left)), QDir::cleanPath(std::move(right)),
         relation});
  }

  void addCanonical(QString left, QString right, PathRelation relation) {
    canonical_.push_back(
        {QDir::cleanPath(std::move(left)), QDir::cleanPath(std::move(right)),
         relation});
  }

  void addMissingPathAlias(QString left, QString right,
                           PathRelation relation) {
    const QString parent =
        QStringLiteral("C:/fake-physical-parent-%1").arg(++fakeParentId_);
    switch (relation) {
      case PathRelation::Same:
        addMissingPathResolution(
            std::move(left), MissingPathResolutionStatus::Resolved, parent,
            {QStringLiteral("shared")},
            PathNameComparisonPolicy::CaseSensitiveExact);
        addMissingPathResolution(
            std::move(right), MissingPathResolutionStatus::Resolved, parent,
            {QStringLiteral("shared")},
            PathNameComparisonPolicy::CaseSensitiveExact);
        break;
      case PathRelation::Different:
        addMissingPathResolution(
            std::move(left), MissingPathResolutionStatus::Resolved, parent,
            {QStringLiteral("left")},
            PathNameComparisonPolicy::CaseSensitiveExact);
        addMissingPathResolution(
            std::move(right), MissingPathResolutionStatus::Resolved, parent,
            {QStringLiteral("right")},
            PathNameComparisonPolicy::CaseSensitiveExact);
        break;
      case PathRelation::Missing:
        addMissingPathResolution(
            std::move(left), MissingPathResolutionStatus::Missing, {}, {},
            PathNameComparisonPolicy::Unknown);
        break;
      case PathRelation::Error:
        addMissingPathResolution(
            std::move(left), MissingPathResolutionStatus::Error, {}, {},
            PathNameComparisonPolicy::Error);
        break;
    }
  }

  void addMissingPathResolution(
      QString path, MissingPathResolutionStatus status, QString parent,
      std::vector<QString> suffix, PathNameComparisonPolicy policy) {
    missingPaths_.push_back(
        {QDir::cleanPath(std::move(path)),
         {status, std::move(parent), std::move(suffix), policy}});
  }

  PathRelation compareFileIdentity(
      const QString& left, const QString& right) const noexcept override {
    ++metadataCalls_;
    if (QDir::cleanPath(left) == QDir::cleanPath(right))
      return PathRelation::Same;
    return lookup(fileIdentity_, left, right);
  }

  PathRelation compareCanonicalPaths(
      const QString& left, const QString& right) const noexcept override {
    ++metadataCalls_;
    return lookup(canonical_, left, right);
  }

  MissingPathResolution resolveMissingPath(
      const QString& path) const noexcept override {
    ++metadataCalls_;
    const QString clean = QDir::cleanPath(path);
    for (const auto& rule : missingPaths_) {
      if (rule.path == clean) return rule.resolution;
    }
    return {};
  }

  [[nodiscard]] std::size_t metadataCalls() const noexcept {
    return metadataCalls_;
  }

 private:
  static PathRelation lookup(const std::vector<Rule>& rules,
                             const QString& left,
                             const QString& right,
                             PathRelation fallback =
                                 PathRelation::Missing) noexcept {
    const QString cleanLeft = QDir::cleanPath(left);
    const QString cleanRight = QDir::cleanPath(right);
    for (const auto& rule : rules) {
      if ((rule.left == cleanLeft && rule.right == cleanRight) ||
          (rule.left == cleanRight && rule.right == cleanLeft))
        return rule.relation;
    }
    return fallback;
  }

  std::vector<Rule> fileIdentity_;
  std::vector<Rule> canonical_;
  std::vector<MissingRule> missingPaths_;
  std::size_t fakeParentId_{};
  mutable std::size_t metadataCalls_{};
};

class AcceptLinkedReloadSuccessor final
    : public solidar::ProductDocumentSuccessor {
 public:
  solidar::ProductDocumentSuccessorStatus prepare(
      const solidar::ProductDocument&,
      solidar::ProductDocumentSuccessorImpact) noexcept override {
    return solidar::ProductDocumentSuccessorStatus::Prepared;
  }
};

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

bool linkCreationUnavailableByPrivilege(const std::error_code& error) {
#if defined(Q_OS_WIN)
  // ERROR_ACCESS_DENIED and ERROR_PRIVILEGE_NOT_HELD are the two outcomes
  // observed for sandboxed/non-developer Windows accounts.
  return error.value() == 5 || error.value() == 1314;
#else
  return error == std::errc::permission_denied ||
         error == std::errc::operation_not_permitted;
#endif
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
constexpr solidar::ProjectId kAssemblyProjectId{
    {0x22, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x01}};

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

QByteArray v6FixtureBytes() {
  QFile file(QString::fromUtf8(SOLIDAR_MINIMAL_V6_FIXTURE_PATH));
  CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

QJsonObject v6FixtureRoot() {
  const auto parsed = QJsonDocument::fromJson(v6FixtureBytes());
  CHECK(parsed.isObject());
  return parsed.object();
}

std::unique_ptr<solidar::ProductDocument> assemblyProduct(
    solidar::Document first, solidar::Document second) {
  solidar::detail::ScopedExplicitIdReservationPause pause;
  solidar::DefinitionRegistry definitions;
  CHECK(definitions.insert(solidar::PartDefinition(
            solidar::DefinitionId{1},
            std::make_unique<solidar::Document>(std::move(first)))) ==
        solidar::DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(solidar::PartDefinition(
            solidar::DefinitionId{2},
            std::make_unique<solidar::Document>(std::move(second)))) ==
        solidar::DefinitionInsertStatus::Inserted);
  solidar::AssemblyDefinition assembly(solidar::DefinitionId{100});
  CHECK(assembly.insert(solidar::ComponentOccurrence(
            solidar::OccurrenceId{11}, solidar::DefinitionId{1},
            solidar::RigidTransform3d::identity(), {true, false, true})) ==
        solidar::AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(assembly)) ==
        solidar::DefinitionInsertStatus::Inserted);
  auto created = solidar::ProductDocument::create(
      std::move(definitions), solidar::DefinitionId{100});
  CHECK(created);
  return std::move(created.product);
}

std::unique_ptr<solidar::ProductDocument> linkedAssemblyProduct(
    solidar::Document cache, const solidar::LinkedPartBinding& binding,
    std::optional<solidar::ProductDocumentIdentity> identity =
        solidar::ProductDocumentIdentity{kAssemblyProjectId,
                                         {"Assembly", "2026-10-27T00:00:00Z"}}) {
  solidar::detail::ScopedExplicitIdReservationPause pause;
  solidar::DefinitionRegistry definitions;
  CHECK(definitions.insert(solidar::PartDefinition(
            solidar::DefinitionId{2},
            std::make_unique<solidar::Document>(std::move(cache)),
            binding)) == solidar::DefinitionInsertStatus::Inserted);
  solidar::AssemblyDefinition assembly(solidar::DefinitionId{1});
  CHECK(assembly.insert(solidar::ComponentOccurrence(
            solidar::OccurrenceId{3}, solidar::DefinitionId{2},
            solidar::RigidTransform3d::identity(), {}, "linked")) ==
        solidar::AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(assembly)) ==
        solidar::DefinitionInsertStatus::Inserted);
  auto created = solidar::ProductDocument::create(
      std::move(definitions), solidar::DefinitionId{1}, std::move(identity));
  CHECK(created);
  return std::move(created.product);
}

std::unique_ptr<solidar::ProductDocument> nestedLinkedAssemblyProduct(
    solidar::Document cache, const solidar::LinkedPartBinding& binding,
    std::optional<solidar::ProductDocumentIdentity> identity =
        solidar::ProductDocumentIdentity{kAssemblyProjectId,
                                         {"Nested assembly",
                                          "2026-10-29T00:00:00Z"}},
    std::optional<solidar::LinkedPartBinding> orphanBinding = std::nullopt) {
  solidar::detail::ScopedExplicitIdReservationPause pause;
  std::vector<solidar::PartDefinition> parts;
  parts.emplace_back(solidar::DefinitionId{2},
                     std::make_unique<solidar::Document>(std::move(cache)),
                     binding);
  if (orphanBinding) {
    auto orphan = std::make_unique<solidar::Document>();
    orphan->setBox({37.0, 20.0, 10.0});
    parts.emplace_back(solidar::DefinitionId{30}, std::move(orphan),
                       *orphanBinding);
  }
  std::vector<solidar::AssemblyDefinition> assemblies;
  solidar::AssemblyDefinition shared(solidar::DefinitionId{20});
  CHECK(shared.insert(solidar::ComponentOccurrence(
            solidar::OccurrenceId{7}, solidar::DefinitionId{2},
            solidar::RigidTransform3d::identity(), {}, "nested linked")) ==
        solidar::AssemblyOccurrenceInsertStatus::Inserted);
  solidar::AssemblyDefinition root(solidar::DefinitionId{1});
  CHECK(root.insert(solidar::ComponentOccurrence(
            solidar::OccurrenceId{1}, solidar::DefinitionId{20},
            solidar::RigidTransform3d::identity(), {}, "left")) ==
        solidar::AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(solidar::ComponentOccurrence(
            solidar::OccurrenceId{2}, solidar::DefinitionId{20},
            solidar::RigidTransform3d::identity(),
            {.visible = true, .suppressed = true, .grounded = false},
            "right")) ==
        solidar::AssemblyOccurrenceInsertStatus::Inserted);
  assemblies.push_back(std::move(root));
  assemblies.push_back(std::move(shared));
  auto created = solidar::ProductDocument::createNested(
      std::move(parts), std::move(assemblies), solidar::DefinitionId{1},
      std::move(identity));
  CHECK(created);
  return std::move(created.product);
}

solidar::Document invalidRecomputeDocument() {
  solidar::detail::ScopedExplicitIdReservationPause pause;
  solidar::Document document;
  auto& first = document.addBody(900001, "first");
  first.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
      900002,
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape()),
      "first archive"));
  auto& second = document.addBody(900003, "second");
  second.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
      900004,
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape()),
      "second archive"));
  document.findBody(900001)->addFeature(
      std::make_unique<solidar::JoinBodiesFeature>(
          900005, 900001, 900002, 900003, 900004, "disconnected join"));
  return document;
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
  static_assert(noexcept(
      std::declval<const solidar::StagedApplicationDocument&>()
          .rootDefinitionKind()));
  static_assert(noexcept(
      std::declval<const solidar::StagedApplicationDocument&>()
          .rootPartDocument()));
  static_assert(!noexcept(
      std::declval<const solidar::StagedApplicationDocument&>()
          .rootDocument()));
  static_assert(noexcept(service.stageOpen(QString{})));
  static_assert(noexcept(service.stageOpenProduct(QString{})));
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
  CHECK(directlyStaged.rootDefinitionKind() == solidar::DefinitionKind::Part);
  CHECK(directlyStaged.rootPartDocument() == readyDocument);
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

  // The assembly-aware boundary uses the hand-authored V6 fixture directly.
  // It retains sharing, the orphan definition, flags and placements, while
  // speculative materialization advances none of the process-global edit IDs.
  const QString v6Path =
      QString::fromUtf8(SOLIDAR_MINIMAL_V6_FIXTURE_PATH);
  const AutoIds beforeV6Open = consumeAutoIds("before-v6-open");
  auto openedV6 = service.stageOpenProduct(v6Path);
  const AutoIds afterV6Open = consumeAutoIds("after-v6-open");
  checkOnlyOneAutoAdvance(beforeV6Open, afterV6Open);
  CHECK(openedV6.succeeded());
  CHECK(openedV6.staged->rootDefinitionKind() ==
        solidar::DefinitionKind::Assembly);
  CHECK(openedV6.staged->rootPartDocument() == nullptr);
  CHECK(openedV6.staged->persistenceState ==
        solidar::project::ProjectPersistenceState{
            solidar::project::ProjectPersistenceFormat::V6});
  const auto& v6Product = openedV6.staged->productDocument();
  CHECK(v6Product.rootDefinitionId() == solidar::DefinitionId{100});
  CHECK(v6Product.definitions().definitionIds() ==
        std::vector<solidar::DefinitionId>(
            {solidar::DefinitionId{1}, solidar::DefinitionId{2},
             solidar::DefinitionId{100}}));
  CHECK(v6Product.definitions().findPart(solidar::DefinitionId{2}) !=
        nullptr);
  const auto* v6Assembly = v6Product.rootAssemblyDefinition();
  CHECK(v6Assembly);
  const auto* v6First = v6Assembly->find(solidar::OccurrenceId{11});
  const auto* v6Second = v6Assembly->find(solidar::OccurrenceId{12});
  CHECK(v6First && v6Second);
  CHECK(v6First->targetDefinitionId() == v6Second->targetDefinitionId());
  CHECK(v6First->flags() ==
        solidar::OccurrenceFlags({true, false, true}));
  CHECK(v6Second->flags() ==
        solidar::OccurrenceFlags({false, true, false}));
  const std::array<double, 3> expectedV6Translation{10.0, 20.0, 30.0};
  CHECK(v6Second->placement().translationMm() == expectedV6Translation);

  const auto partOnlyV6 = service.stageOpen(
      v6Path, solidar::ProductV5OpenPolicy::Allow);
  CHECK(!partOnlyV6.succeeded());
  CHECK(partOnlyV6.status.code ==
        solidar::ProjectApplicationErrorCode::UnsupportedProject);
  CHECK(!partOnlyV6.staged);

  // A V6 part carrying actual parametric/B-Rep content is fully rebuilt by
  // stageOpenProduct, not merely schema-checked.
  auto shapeAssembly = assemblyProduct(boxDocument("V6 B-Rep"),
                                       solidar::Document{});
  const auto encodedShapeAssembly =
      solidar::project::encodeProductEnvelopeV6Root(
          *shapeAssembly, kProductProjectId,
          {QStringLiteral("assembly"),
           QStringLiteral("2026-10-23T00:00:00Z")});
  CHECK(encodedShapeAssembly.succeeded());
  const QString shapeAssemblyPath =
      directory.filePath(QStringLiteral("materialized-v6.solidar"));
  CHECK(writeBytes(shapeAssemblyPath, encodedShapeAssembly.payload->bytes));
  const auto stagedShapeAssembly =
      service.stageOpenProduct(shapeAssemblyPath);
  CHECK(stagedShapeAssembly.succeeded());
  const auto* stagedPart = stagedShapeAssembly.staged->productDocument()
                               .definitions()
                               .findPart(solidar::DefinitionId{1});
  CHECK(stagedPart);
  CHECK(stagedPart->document().bodies().size() == 1);
  CHECK(stagedPart->document().bodies().front().activeFeature());
  CHECK(stagedPart->document()
            .bodies()
            .front()
            .activeFeature()
            ->lastValidShape());

  auto danglingV6Root = v6FixtureRoot();
  auto danglingDefinitions =
      danglingV6Root.value(QStringLiteral("definitions")).toArray();
  auto danglingAssembly = danglingDefinitions.last().toObject();
  auto danglingOccurrences =
      danglingAssembly.value(QStringLiteral("occurrences")).toArray();
  auto danglingOccurrence = danglingOccurrences.first().toObject();
  danglingOccurrence[QStringLiteral("targetDefinitionId")] = 999;
  danglingOccurrences[0] = danglingOccurrence;
  danglingAssembly[QStringLiteral("occurrences")] = danglingOccurrences;
  danglingDefinitions[danglingDefinitions.size() - 1] = danglingAssembly;
  danglingV6Root[QStringLiteral("definitions")] = danglingDefinitions;
  const QString danglingV6Path =
      directory.filePath(QStringLiteral("dangling-v6.solidar"));
  CHECK(writeBytes(danglingV6Path,
                   QJsonDocument(danglingV6Root)
                       .toJson(QJsonDocument::Compact)));
  const auto danglingV6 = service.stageOpenProduct(danglingV6Path);
  CHECK(!danglingV6.succeeded());
  CHECK(danglingV6.status.code ==
        solidar::ProjectApplicationErrorCode::InvalidProject);
  CHECK(!danglingV6.staged);

  auto unknownCapabilityRoot = v6FixtureRoot();
  unknownCapabilityRoot[QStringLiteral("capabilities")] =
      QJsonArray{QStringLiteral("flat-assembly-v1"),
                 QStringLiteral("product-envelope-v1"),
                 QStringLiteral("zzz-future-required")};
  const QString unknownCapabilityPath =
      directory.filePath(QStringLiteral("unknown-capability-v6.solidar"));
  CHECK(writeBytes(unknownCapabilityPath,
                   QJsonDocument(unknownCapabilityRoot)
                       .toJson(QJsonDocument::Compact)));
  const auto unknownCapability =
      service.stageOpenProduct(unknownCapabilityPath);
  CHECK(!unknownCapability.succeeded());
  CHECK(unknownCapability.status.code ==
        solidar::ProjectApplicationErrorCode::UnsupportedProject);
  CHECK(!unknownCapability.staged);

  auto futureV6Root = v6FixtureRoot();
  futureV6Root[QStringLiteral("version")] = 9;
  const QString futureV6Path =
      directory.filePath(QStringLiteral("future-v9.solidar"));
  CHECK(writeBytes(futureV6Path,
                   QJsonDocument(futureV6Root)
                       .toJson(QJsonDocument::Compact)));
  const auto futureV6 = service.stageOpenProduct(futureV6Path);
  CHECK(!futureV6.succeeded());
  CHECK(futureV6.status.code ==
        solidar::ProjectApplicationErrorCode::UnsupportedProject);
  CHECK(!futureV6.staged);

  // A late failure in orphan definition 2 rolls back the complete staged
  // product and does not reserve explicit persisted IDs globally.
  auto lateFailureAssembly = assemblyProduct(
      solidar::Document{}, invalidRecomputeDocument());
  const auto encodedLateFailure =
      solidar::project::encodeProductEnvelopeV6Root(
          *lateFailureAssembly, kProductProjectId,
          {QStringLiteral("assembly"),
           QStringLiteral("2026-10-23T00:00:00Z")});
  CHECK(encodedLateFailure.succeeded());
  const QString lateFailureV6Path =
      directory.filePath(QStringLiteral("late-failure-v6.solidar"));
  CHECK(writeBytes(lateFailureV6Path, encodedLateFailure.payload->bytes));
  const AutoIds beforeLateFailure = consumeAutoIds("before-v6-late-failure");
  const auto lateFailureV6 = service.stageOpenProduct(lateFailureV6Path);
  const AutoIds afterLateFailure = consumeAutoIds("after-v6-late-failure");
  checkOnlyOneAutoAdvance(beforeLateFailure, afterLateFailure);
  CHECK(!lateFailureV6.succeeded());
  CHECK(lateFailureV6.status.code ==
        solidar::ProjectApplicationErrorCode::InvalidProject);
  CHECK(!lateFailureV6.staged);

  // V6 is staged-only in A3c. Every save/import/upgrade route fails before
  // dereferencing an assembly root or replacing an existing destination.
  const QString rejectedV6Destination =
      directory.filePath(QStringLiteral("rejected-v6-save.solidar"));
  const QByteArray v6DestinationSentinel("v6-destination-sentinel");
  CHECK(writeBytes(rejectedV6Destination, v6DestinationSentinel));
  const auto rejectedV6Save = service.save(rejectedV6Destination, v6Product);
  CHECK(!rejectedV6Save.succeeded());
  CHECK(rejectedV6Save.code ==
        solidar::ProjectApplicationErrorCode::SaveFailed);
  CHECK(readBytes(rejectedV6Destination) == v6DestinationSentinel);
  const auto rejectedV6Import = service.stageImportStep(
      directory.filePath(QStringLiteral("missing-v6.step")), v6Product,
      QStringLiteral("must not import"));
  CHECK(!rejectedV6Import.succeeded());
  CHECK(rejectedV6Import.status.code ==
        solidar::ProjectApplicationErrorCode::InvalidProject);
  CHECK(!rejectedV6Import.staged);
  QString directV6SaveError;
  CHECK(!solidar::project::ProjectFile::saveDocument(
      rejectedV6Destination, committed,
      {solidar::project::ProjectPersistenceFormat::V6},
      &directV6SaveError));
  CHECK(!directV6SaveError.isEmpty());
  CHECK(readBytes(rejectedV6Destination) == v6DestinationSentinel);
  const auto rejectedV6Upgrade = service.upgradeProjectFormat(
      rejectedV6Destination, committed,
      {solidar::project::ProjectPersistenceFormat::V6});
  CHECK(!rejectedV6Upgrade.succeeded());
  CHECK(readBytes(rejectedV6Destination) == v6DestinationSentinel);

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

  // A6b2: exact V7 dispatch materializes lastValid first, then diagnoses
  // linked sources without silently accepting or reloading a changed hash.
  const QString assemblyDirectory =
      directory.filePath(QStringLiteral("a6b2"));
  CHECK(QDir().mkpath(assemblyDirectory + QStringLiteral("/parts")));
  const QString linkedSourcePath =
      assemblyDirectory + QStringLiteral("/parts/source.solidar");
  solidar::Document linkedSourceDocument;
  linkedSourceDocument.setBox({77.0, 40.0, 25.0});
  auto linkedSourceProduct = product(
      std::move(linkedSourceDocument), productIdentity(),
      solidar::DefinitionId{17});
  const auto linkedSourceEncoded = solidar::project::encodeProductEnvelopeV5Root(
      *linkedSourceProduct, kProductProjectId,
      {QStringLiteral("linked"),
       QStringLiteral("2026-10-27T00:00:00Z")});
  CHECK(linkedSourceEncoded.succeeded());
  CHECK(writeBytes(linkedSourcePath, linkedSourceEncoded.payload->bytes));
  constexpr solidar::ProjectId kOrphanSourceProjectId{
      {0x82, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
       0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x18}};
  solidar::Document orphanSourceDocument;
  orphanSourceDocument.setBox({37.0, 20.0, 10.0});
  auto orphanSource = product(
      std::move(orphanSourceDocument),
      solidar::ProductDocumentIdentity{
          kOrphanSourceProjectId,
          {"orphan linked", "2026-10-29T00:00:00Z"}},
      solidar::DefinitionId{31});
  const auto orphanSourceEncoded =
      solidar::project::encodeProductEnvelopeV5Root(
          *orphanSource, kOrphanSourceProjectId,
          {QStringLiteral("orphan linked"),
           QStringLiteral("2026-10-29T00:00:00Z")});
  CHECK(orphanSourceEncoded.succeeded());
  const QString orphanSourcePath =
      assemblyDirectory + QStringLiteral("/parts/orphan.solidar");
  CHECK(writeBytes(orphanSourcePath, orphanSourceEncoded.payload->bytes));
  const solidar::LinkedPartBinding orphanBinding{
      {kOrphanSourceProjectId, solidar::DefinitionId{31}},
      "parts/orphan.solidar",
      solidar::project::linkedSourceSha256(
          QByteArrayView(orphanSourceEncoded.payload->bytes))};
  const solidar::LinkedPartBinding linkedBinding{
      {kProductProjectId, solidar::DefinitionId{17}},
      "parts/source.solidar",
      solidar::project::linkedSourceSha256(
          QByteArrayView(linkedSourceEncoded.payload->bytes))};
  solidar::Document cache;
  cache.setBox({60.0, 40.0, 25.0});
  auto linkedAssembly = linkedAssemblyProduct(cache, linkedBinding);
  const auto v7Encoded = solidar::project::encodeProductEnvelopeV7Root(
      *linkedAssembly, kAssemblyProjectId,
      {QStringLiteral("assembly"),
       QStringLiteral("2026-10-27T00:00:00Z")});
  CHECK(v7Encoded.succeeded());
  const QString assemblyPath =
      assemblyDirectory + QStringLiteral("/assembly.solidar");
  CHECK(writeBytes(assemblyPath, v7Encoded.payload->bytes));

  auto exactV7 = service.stageOpenProduct(assemblyPath);
  CHECK(exactV7.succeeded());
  CHECK(exactV7.staged->persistenceState->format ==
        solidar::project::ProjectPersistenceFormat::V7);
  CHECK(exactV7.staged->linkedPartRuntimeStates.at(
            solidar::DefinitionId{2}).status ==
        solidar::LinkedPartRuntimeStatus::Exact);
  CHECK(exactV7.staged->productDocument()
            .partDocument(solidar::DefinitionId{2})
            ->box()
            .widthMm == 60.0);

  solidar::Document revisedSourceDocument;
  revisedSourceDocument.setBox({88.0, 40.0, 25.0});
  auto revisedSource = product(
      std::move(revisedSourceDocument), productIdentity(),
      solidar::DefinitionId{17});
  const auto revisedEncoded = solidar::project::encodeProductEnvelopeV5Root(
      *revisedSource, kProductProjectId,
      {QStringLiteral("linked"),
       QStringLiteral("2026-10-27T00:00:00Z")});
  CHECK(revisedEncoded.succeeded());
  CHECK(writeBytes(linkedSourcePath, revisedEncoded.payload->bytes));
  auto outdatedV7 = service.stageOpenProduct(assemblyPath);
  CHECK(outdatedV7.succeeded());
  CHECK(outdatedV7.staged->linkedPartRuntimeStates.at(
            solidar::DefinitionId{2}).status ==
        solidar::LinkedPartRuntimeStatus::Outdated);
  CHECK(outdatedV7.staged->productDocument()
            .partDocument(solidar::DefinitionId{2})
            ->box()
            .widthMm == 60.0);
  CHECK(QFile::remove(linkedSourcePath));
  auto missingV7 = service.stageOpenProduct(assemblyPath);
  CHECK(missingV7.succeeded());
  CHECK(missingV7.staged->linkedPartRuntimeStates.at(
            solidar::DefinitionId{2}).status ==
        solidar::LinkedPartRuntimeStatus::Missing);
  CHECK(missingV7.staged->productDocument()
            .partDocument(solidar::DefinitionId{2})
            ->box()
            .widthMm == 60.0);

  // A6c1b: exact V8 dispatch keeps the recursive definition graph and uses
  // the same application-owned diagnostic policy for every linked definition.
  CHECK(writeBytes(linkedSourcePath, linkedSourceEncoded.payload->bytes));
  solidar::Document nestedCache;
  nestedCache.setBox({61.0, 40.0, 25.0});
  auto nestedLinkedAssembly = nestedLinkedAssemblyProduct(
      nestedCache, linkedBinding,
      solidar::ProductDocumentIdentity{
          kAssemblyProjectId,
          {"Nested assembly", "2026-10-29T00:00:00Z"}},
      orphanBinding);
  const auto v8Encoded = solidar::project::encodeProductEnvelopeV8Root(
      *nestedLinkedAssembly, kAssemblyProjectId,
      {QStringLiteral("nested assembly"),
       QStringLiteral("2026-10-29T00:00:00Z")});
  CHECK(v8Encoded.succeeded());
  const QString nestedAssemblyPath =
      assemblyDirectory + QStringLiteral("/nested-v8.solidar");
  CHECK(writeBytes(nestedAssemblyPath, v8Encoded.payload->bytes));
  const auto partOnlyV8 = service.stageOpen(nestedAssemblyPath);
  CHECK(!partOnlyV8.succeeded());
  CHECK(partOnlyV8.status.code ==
        solidar::ProjectApplicationErrorCode::UnsupportedProject);
  auto exactV8 = service.stageOpenProduct(nestedAssemblyPath);
  CHECK(exactV8.succeeded());
  CHECK(exactV8.staged->persistenceState->format ==
        solidar::project::ProjectPersistenceFormat::V8);
  CHECK(exactV8.staged->productDocument().definitions().size() == 4);
  CHECK(static_cast<bool>(exactV8.staged->productDocument().resolveOccurrence(
      solidar::OccurrencePath::create(
          {solidar::OccurrenceId{1}, solidar::OccurrenceId{7}})
          .path.value())));
  CHECK(static_cast<bool>(exactV8.staged->productDocument().resolveOccurrence(
      solidar::OccurrencePath::create(
          {solidar::OccurrenceId{2}, solidar::OccurrenceId{7}})
          .path.value())));
  CHECK(exactV8.staged->linkedPartRuntimeStates.at(
            solidar::DefinitionId{2}).status ==
        solidar::LinkedPartRuntimeStatus::Exact);
  CHECK(exactV8.staged->linkedPartRuntimeStates.at(
            solidar::DefinitionId{30}).status ==
        solidar::LinkedPartRuntimeStatus::Exact);
  CHECK(exactV8.staged->productDocument()
            .partDocument(solidar::DefinitionId{2})
            ->box()
            .widthMm == 61.0);

  // The A6b2 policy is shared by V7/V8: resolver outcomes are diagnostics on
  // lastValid except for an unsafe descriptor. The batch stops at a bounded
  // first failure, so every other linked definition remains Unchecked rather
  // than being falsely labelled Exact.
  const auto diagnosticNestedProduct = [&] {
    solidar::Document diagnosticCache;
    diagnosticCache.setBox({61.0, 40.0, 25.0});
    return nestedLinkedAssemblyProduct(
        std::move(diagnosticCache), linkedBinding,
        solidar::ProductDocumentIdentity{
            kAssemblyProjectId,
            {"Nested diagnostic", "2026-10-29T00:30:00Z"}},
        orphanBinding);
  };
  using ResolveStatus = solidar::project::LinkedPartSourceResolveStatus;
  const std::array<std::pair<ResolveStatus, solidar::LinkedPartRuntimeStatus>,
                   6>
      diagnosticStatuses{{
          {ResolveStatus::IdentityMismatch,
           solidar::LinkedPartRuntimeStatus::IdentityMismatch},
          {ResolveStatus::OutsideAllowedRoot,
           solidar::LinkedPartRuntimeStatus::SecurityRejected},
          {ResolveStatus::InvalidSource,
           solidar::LinkedPartRuntimeStatus::InvalidSource},
          {ResolveStatus::ResourceFailure,
           solidar::LinkedPartRuntimeStatus::ResourceFailure},
          {ResolveStatus::SourceChanged,
           solidar::LinkedPartRuntimeStatus::ResourceFailure},
          {ResolveStatus::Outdated,
           solidar::LinkedPartRuntimeStatus::Outdated}}};
  for (const auto& [resolverStatus, runtime] : diagnosticStatuses) {
    for (std::size_t failureIndex = 0; failureIndex < 2; ++failureIndex) {
      auto staged = solidar::application::detail::
          stageAssemblyOpenWithResolutionForTesting(
              diagnosticNestedProduct(),
              solidar::project::ProjectPersistenceFormat::V8,
              resolverStatus, failureIndex,
              QStringLiteral("injected linked diagnostic"));
      CHECK(staged.succeeded());
      const auto& states = staged.staged->linkedPartRuntimeStates;
      const solidar::DefinitionId failed =
          failureIndex == 0 ? solidar::DefinitionId{2}
                            : solidar::DefinitionId{30};
      const solidar::DefinitionId untouched =
          failureIndex == 0 ? solidar::DefinitionId{30}
                            : solidar::DefinitionId{2};
      CHECK(states.at(failed).status == runtime);
      CHECK(states.at(untouched).status ==
            solidar::LinkedPartRuntimeStatus::Unchecked);
      CHECK(states.at(failed).status !=
            solidar::LinkedPartRuntimeStatus::Exact);
      CHECK(states.at(untouched).status !=
            solidar::LinkedPartRuntimeStatus::Exact);
      CHECK(staged.staged->productDocument()
                .partDocument(solidar::DefinitionId{2})
                ->box()
                .widthMm == 61.0);
    }
  }
  const auto unsafeDescriptorDiagnostic = solidar::application::detail::
      stageAssemblyOpenWithResolutionForTesting(
          diagnosticNestedProduct(),
          solidar::project::ProjectPersistenceFormat::V8,
          ResolveStatus::InvalidDescriptor, 0,
          QStringLiteral("injected unsafe descriptor"));
  CHECK(!unsafeDescriptorDiagnostic.succeeded());
  CHECK(!unsafeDescriptorDiagnostic.staged);

  const QString nestedExportOutput =
      assemblyDirectory + QStringLiteral("/must-not-export.step");
  CHECK(!QFile::exists(nestedExportOutput));
  CHECK(service.preflightFullProductExport(*nestedLinkedAssembly,
                                            nestedAssemblyPath)
            .succeeded());
  CHECK(!QFile::exists(nestedExportOutput));

  // The orphan linked definition participates even though it has no
  // occurrence. The shared linked definition also has a suppressed recursive
  // occurrence, and the preflight deliberately reasons over definitions
  // rather than the presentation subset.
  CHECK(QFile::remove(orphanSourcePath));
  CHECK(!service.preflightFullProductExport(*nestedLinkedAssembly,
                                             nestedAssemblyPath)
             .succeeded());
  CHECK(!QFile::exists(nestedExportOutput));
  CHECK(writeBytes(orphanSourcePath, orphanSourceEncoded.payload->bytes));
  CHECK(service.preflightFullProductExport(*nestedLinkedAssembly,
                                            nestedAssemblyPath)
            .succeeded());
  for (const ResolveStatus blocked :
       {ResolveStatus::OutsideAllowedRoot, ResolveStatus::ResourceFailure}) {
    CHECK(!solidar::application::detail::
               preflightFullProductExportWithResolutionForTesting(
                   *nestedLinkedAssembly, blocked, 1,
                   QStringLiteral("injected export diagnostic"))
               .succeeded());
    CHECK(!QFile::exists(nestedExportOutput));
  }

  // Exercise the production lifecycle rather than synthesizing a new hash:
  // decode the actual nested V8, Reload its shared linked definition, verify
  // the newly accepted revision, then mutate that source again. Export must
  // fail before creating an output after the post-Reload source change.
  auto reloadedV8 = service.stageOpenProduct(nestedAssemblyPath);
  CHECK(reloadedV8.succeeded());
  auto reloadedNestedProduct =
      std::move(*reloadedV8.staged).releaseProductDocument();
  CHECK(writeBytes(linkedSourcePath, revisedEncoded.payload->bytes));
  solidar::AssemblyCommandHistory reloadHistory;
  AcceptLinkedReloadSuccessor reloadSuccessor;
  solidar::project::LinkedPartSourcePolicy reloadPolicy;
  reloadPolicy.projectRoot = assemblyDirectory;
#if defined(Q_OS_WIN)
  reloadPolicy.pathSemantics = solidar::project::LinkedPathSemantics::Windows;
#else
  reloadPolicy.pathSemantics = solidar::project::LinkedPathSemantics::Posix;
#endif
  const auto nestedReload = solidar::LinkedPartLifecycleService::reload(
      reloadedNestedProduct, solidar::DefinitionId{2}, reloadPolicy,
      reloadHistory, reloadSuccessor);
  CHECK(nestedReload.status ==
        solidar::LinkedPartLifecycleStatus::Committed);
  CHECK(reloadedNestedProduct.partDocument(solidar::DefinitionId{2})
            ->box()
            .widthMm == 88.0);
  CHECK(service.preflightFullProductExport(reloadedNestedProduct,
                                            nestedAssemblyPath)
            .succeeded());
  solidar::Document changedAfterReloadDocument;
  changedAfterReloadDocument.setBox({99.0, 40.0, 25.0});
  auto changedAfterReloadSource = product(
      std::move(changedAfterReloadDocument), productIdentity(),
      solidar::DefinitionId{17});
  const auto changedAfterReloadEncoded =
      solidar::project::encodeProductEnvelopeV5Root(
          *changedAfterReloadSource, kProductProjectId,
          {QStringLiteral("linked"),
           QStringLiteral("2026-10-27T00:00:00Z")});
  CHECK(changedAfterReloadEncoded.succeeded());
  CHECK(writeBytes(linkedSourcePath,
                   changedAfterReloadEncoded.payload->bytes));
  CHECK(!service.preflightFullProductExport(reloadedNestedProduct,
                                             nestedAssemblyPath)
             .succeeded());
  CHECK(!QFile::exists(nestedExportOutput));
  CHECK(writeBytes(linkedSourcePath, linkedSourceEncoded.payload->bytes));

  CHECK(writeBytes(linkedSourcePath, revisedEncoded.payload->bytes));
  auto outdatedV8 = service.stageOpenProduct(nestedAssemblyPath);
  CHECK(outdatedV8.succeeded());
  CHECK(outdatedV8.staged->linkedPartRuntimeStates.at(
            solidar::DefinitionId{2}).status ==
        solidar::LinkedPartRuntimeStatus::Outdated);
  CHECK(outdatedV8.staged->productDocument()
            .partDocument(solidar::DefinitionId{2})
            ->box()
            .widthMm == 61.0);
  CHECK(!service.preflightFullProductExport(*nestedLinkedAssembly,
                                             nestedAssemblyPath)
             .succeeded());
  CHECK(!QFile::exists(nestedExportOutput));
  CHECK(QFile::remove(linkedSourcePath));
  auto missingV8 = service.stageOpenProduct(nestedAssemblyPath);
  CHECK(missingV8.succeeded());
  CHECK(missingV8.staged->linkedPartRuntimeStates.at(
            solidar::DefinitionId{2}).status ==
        solidar::LinkedPartRuntimeStatus::Missing);
  CHECK(!service.preflightFullProductExport(*nestedLinkedAssembly,
                                             nestedAssemblyPath)
             .succeeded());
  CHECK(!QFile::exists(nestedExportOutput));

  QJsonObject wrongV8SourceRoot =
      QJsonDocument::fromJson(linkedSourceEncoded.payload->bytes).object();
  wrongV8SourceRoot[QStringLiteral("projectId")] =
      QStringLiteral("223e4567-e89b-42d3-a456-426614174099");
  CHECK(writeBytes(
      linkedSourcePath,
      QJsonDocument(wrongV8SourceRoot).toJson(QJsonDocument::Compact)));
  auto identityMismatchV8 = service.stageOpenProduct(nestedAssemblyPath);
  CHECK(identityMismatchV8.succeeded());
  CHECK(identityMismatchV8.staged->linkedPartRuntimeStates.at(
            solidar::DefinitionId{2}).status ==
        solidar::LinkedPartRuntimeStatus::IdentityMismatch);
  CHECK(identityMismatchV8.staged->productDocument()
            .partDocument(solidar::DefinitionId{2})
            ->box()
            .widthMm == 61.0);
  CHECK(!service.preflightFullProductExport(*nestedLinkedAssembly,
                                             nestedAssemblyPath)
             .succeeded());
  CHECK(!QFile::exists(nestedExportOutput));

  CHECK(writeBytes(linkedSourcePath, QByteArrayLiteral("not a project")));
  auto invalidSourceV8 = service.stageOpenProduct(nestedAssemblyPath);
  CHECK(invalidSourceV8.succeeded());
  CHECK(invalidSourceV8.staged->linkedPartRuntimeStates.at(
            solidar::DefinitionId{2}).status ==
        solidar::LinkedPartRuntimeStatus::InvalidSource);
  CHECK(invalidSourceV8.staged->productDocument()
            .partDocument(solidar::DefinitionId{2})
            ->box()
            .widthMm == 61.0);
  CHECK(!service.preflightFullProductExport(*nestedLinkedAssembly,
                                             nestedAssemblyPath)
             .succeeded());
  CHECK(!QFile::exists(nestedExportOutput));

  // V8 schema and lastValid cache validation are not resolver diagnostics:
  // malformed envelopes fail before a staged application document exists.
  auto invalidV8SchemaRoot = v8Encoded.payload->root;
  invalidV8SchemaRoot.remove(QStringLiteral("definitions"));
  const QString invalidV8SchemaPath =
      assemblyDirectory + QStringLiteral("/invalid-v8-schema.solidar");
  CHECK(writeBytes(invalidV8SchemaPath,
                   QJsonDocument(invalidV8SchemaRoot)
                       .toJson(QJsonDocument::Compact)));
  const auto invalidV8Schema = service.stageOpenProduct(invalidV8SchemaPath);
  CHECK(!invalidV8Schema.succeeded());
  CHECK(!invalidV8Schema.staged);
  CHECK(invalidV8Schema.status.code ==
        solidar::ProjectApplicationErrorCode::InvalidProject);

  auto invalidV8CacheRoot = v8Encoded.payload->root;
  auto invalidV8CacheDefinitions =
      invalidV8CacheRoot.value(QStringLiteral("definitions")).toArray();
  for (qsizetype index = 0; index < invalidV8CacheDefinitions.size();
       ++index) {
    auto definition = invalidV8CacheDefinitions.at(index).toObject();
    if (definition.value(QStringLiteral("definitionId")).toInt() != 2)
      continue;
    auto payload = definition.value(QStringLiteral("payload")).toObject();
    auto lastValid = payload.value(QStringLiteral("lastValid")).toObject();
    lastValid[QStringLiteral("unexpected-cache-field")] = true;
    payload[QStringLiteral("lastValid")] = lastValid;
    definition[QStringLiteral("payload")] = payload;
    invalidV8CacheDefinitions[index] = definition;
  }
  invalidV8CacheRoot[QStringLiteral("definitions")] =
      invalidV8CacheDefinitions;
  const QString invalidV8CachePath =
      assemblyDirectory + QStringLiteral("/invalid-v8-cache.solidar");
  CHECK(writeBytes(invalidV8CachePath,
                   QJsonDocument(invalidV8CacheRoot)
                       .toJson(QJsonDocument::Compact)));
  const auto invalidV8Cache = service.stageOpenProduct(invalidV8CachePath);
  CHECK(!invalidV8Cache.succeeded());
  CHECK(!invalidV8Cache.staged);
  CHECK(invalidV8Cache.status.code ==
        solidar::ProjectApplicationErrorCode::InvalidProject);

  auto unsafeV8Root = v8Encoded.payload->root;
  auto unsafeDefinitions =
      unsafeV8Root.value(QStringLiteral("definitions")).toArray();
  for (qsizetype index = 0; index < unsafeDefinitions.size(); ++index) {
    auto definition = unsafeDefinitions.at(index).toObject();
    if (definition.value(QStringLiteral("definitionId")).toInt() != 2)
      continue;
    auto payload = definition.value(QStringLiteral("payload")).toObject();
    auto binding = payload.value(QStringLiteral("binding")).toObject();
    binding[QStringLiteral("relativePathHint")] =
        QStringLiteral("../escape.solidar");
    payload[QStringLiteral("binding")] = binding;
    definition[QStringLiteral("payload")] = payload;
    unsafeDefinitions[index] = definition;
  }
  unsafeV8Root[QStringLiteral("definitions")] = unsafeDefinitions;
  const QString unsafeV8Path =
      assemblyDirectory + QStringLiteral("/unsafe-v8.solidar");
  CHECK(writeBytes(unsafeV8Path,
                   QJsonDocument(unsafeV8Root).toJson(QJsonDocument::Compact)));
  const auto unsafeV8 = service.stageOpenProduct(unsafeV8Path);
  CHECK(!unsafeV8.succeeded());
  CHECK(unsafeV8.status.code ==
        solidar::ProjectApplicationErrorCode::InvalidProject);

  CHECK(writeBytes(linkedSourcePath, linkedSourceEncoded.payload->bytes));
  const QString savedV8Path =
      assemblyDirectory + QStringLiteral("/saved-v8.solidar");
  CHECK(service
            .saveAssembly(savedV8Path, *nestedLinkedAssembly,
                          {solidar::project::ProjectPersistenceFormat::V8})
            .succeeded());
  const auto savedV8 = service.stageOpenProduct(savedV8Path);
  CHECK(savedV8.succeeded());
  CHECK(savedV8.staged->persistenceState->format ==
        solidar::project::ProjectPersistenceFormat::V8);
  const solidar::ProductDocumentIdentity v8ForkIdentity{
      solidar::ProjectId{{0x72, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                          0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x08}},
      {"Nested assembly copy", "2026-10-29T01:00:00Z"}};
  const QString copiedV8Path =
      assemblyDirectory + QStringLiteral("/copy-v8.solidar");
  CHECK(service
            .saveAssemblyAs(
                copiedV8Path, *nestedLinkedAssembly,
                {solidar::project::ProjectPersistenceFormat::V8},
                {v8ForkIdentity, savedV8Path})
            .succeeded());
  const auto copiedV8 = service.stageOpenProduct(copiedV8Path);
  CHECK(copiedV8.succeeded());
  CHECK(copiedV8.staged->persistenceState->format ==
        solidar::project::ProjectPersistenceFormat::V8);
  CHECK(copiedV8.staged->productDocument().identity() == v8ForkIdentity);
  CHECK(QFile::remove(linkedSourcePath));

  // Existing V7 and first-save V7 both use the common atomic writer. Same-dir
  // Save As is allowed while Missing; a cross-directory fork fails closed.
  const QString savedV7Path =
      assemblyDirectory + QStringLiteral("/saved-v7.solidar");
  CHECK(service.saveAssemblyV7(savedV7Path, *linkedAssembly).succeeded());
  const auto savedV7 = service.stageOpenProduct(savedV7Path);
  CHECK(savedV7.succeeded());
  const solidar::ProductDocumentIdentity forkIdentity{
      solidar::ProjectId{{0x32, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                          0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x02}},
      {"Assembly copy", "2026-10-27T01:00:00Z"}};
  const QString sameDirectoryCopy =
      assemblyDirectory + QStringLiteral("/copy-v7.solidar");
  CHECK(service.saveAssemblyV7As(
            sameDirectoryCopy, *linkedAssembly,
            {forkIdentity, savedV7Path})
            .succeeded());
  CHECK(linkedAssembly->identity() == forkIdentity);
  CHECK(QFile::exists(sameDirectoryCopy));

  const auto identityBeforeMissingLocatorCollision =
      *linkedAssembly->identity();
  const auto missingLocatorCollision = service.saveAssemblyV7As(
      linkedSourcePath, *linkedAssembly,
      {{solidar::ProjectId{{0x37, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                            0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x07}},
        {"Missing locator collision", "2026-10-27T01:30:00Z"}},
       sameDirectoryCopy});
  CHECK(!missingLocatorCollision.succeeded());
  CHECK(!QFile::exists(linkedSourcePath));
  CHECK(linkedAssembly->identity() ==
        identityBeforeMissingLocatorCollision);

  const QString otherDirectory =
      directory.filePath(QStringLiteral("a6b2-other"));
  CHECK(QDir().mkpath(otherDirectory));
  const auto identityBeforeCrossDirectory = *linkedAssembly->identity();
  const auto failedCrossDirectory = service.saveAssemblyV7As(
      otherDirectory + QStringLiteral("/copy.solidar"), *linkedAssembly,
      {{solidar::ProjectId{{0x42, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                            0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x03}},
        {"Cross copy", "2026-10-27T02:00:00Z"}},
       sameDirectoryCopy});
  CHECK(!failedCrossDirectory.succeeded());
  CHECK(linkedAssembly->identity() == identityBeforeCrossDirectory);

  // A genuinely different authored directory may succeed only when its
  // unchanged locator resolves to the exact same canonical source. A
  // directory symlink supplies that condition without duplicating content;
  // Windows hosts without symlink privilege skip only this platform seam.
  const QString aliasRoot =
      directory.filePath(QStringLiteral("a6b2-canonical-alias"));
  std::error_code directorySymlinkError;
  std::filesystem::create_directory_symlink(
      std::filesystem::path(assemblyDirectory.toStdWString()),
      std::filesystem::path(aliasRoot.toStdWString()),
      directorySymlinkError);
  CHECK(!directorySymlinkError ||
        linkCreationUnavailableByPrivilege(directorySymlinkError));
  if (!directorySymlinkError) {
    CHECK(writeBytes(linkedSourcePath,
                     linkedSourceEncoded.payload->bytes));
    const QString canonicalCrossTarget =
        aliasRoot + QStringLiteral("/canonical-cross-copy.solidar");
    const auto canonicalCrossSave = service.saveAssemblyV7As(
        canonicalCrossTarget, *linkedAssembly,
        {{solidar::ProjectId{{0x43, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                              0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x13}},
          {"Canonical cross copy", "2026-10-27T02:30:00Z"}},
         sameDirectoryCopy});
    CHECK(canonicalCrossSave.succeeded());
    CHECK(QFile::exists(canonicalCrossTarget));
    CHECK(service.stageOpenProduct(canonicalCrossTarget).succeeded());
    CHECK(QFile::remove(linkedSourcePath));
  }

  auto newAssembly = service.stageNewAssembly();
  CHECK(newAssembly.succeeded());
  auto newAssemblyProduct =
      std::move(*newAssembly.staged).releaseProductDocument();
  CHECK(!newAssemblyProduct.identity());
  const solidar::ProductDocumentIdentity firstIdentity{
      solidar::ProjectId{{0x52, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                          0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x04}},
      {"New assembly", "2026-10-27T03:00:00Z"}};
  const QString firstSavePath =
      assemblyDirectory + QStringLiteral("/first-save.solidar");
  CHECK(service.saveAssemblyV7(firstSavePath, newAssemblyProduct,
                               firstIdentity)
            .succeeded());
  CHECK(newAssemblyProduct.identity() == firstIdentity);

  auto failedFirstSaveStage = service.stageNewAssembly();
  CHECK(failedFirstSaveStage.succeeded());
  auto failedFirstSaveProduct =
      std::move(*failedFirstSaveStage.staged).releaseProductDocument();
  const QString failedFirstSavePath =
      directory.filePath(QStringLiteral("missing-parent/assembly.solidar"));
  CHECK(!service.saveAssemblyV7(failedFirstSavePath,
                                failedFirstSaveProduct, firstIdentity)
             .succeeded());
  CHECK(!failedFirstSaveProduct.identity());
  CHECK(!QFile::exists(failedFirstSavePath));

  CHECK(!service.preflightFullProductExport(*linkedAssembly,
                                             sameDirectoryCopy)
             .succeeded());
  CHECK(writeBytes(linkedSourcePath, revisedEncoded.payload->bytes));
  CHECK(!service.preflightFullProductExport(*linkedAssembly,
                                             sameDirectoryCopy)
             .succeeded());
  CHECK(writeBytes(linkedSourcePath, linkedSourceEncoded.payload->bytes));
  CHECK(service.preflightFullProductExport(*linkedAssembly,
                                            sameDirectoryCopy)
            .succeeded());
  const QByteArray linkedSourceBeforeSave = readBytes(linkedSourcePath);
  const QDateTime linkedSourceMtimeBeforeSave =
      QFileInfo(linkedSourcePath).lastModified();
  const QStringList linkedSourceEntriesBeforeSave =
      QDir(assemblyDirectory + QStringLiteral("/parts"))
          .entryList(QDir::AllEntries | QDir::Hidden | QDir::System,
                     QDir::Name);
  CHECK(service.saveAssemblyV7(sameDirectoryCopy, *linkedAssembly)
            .succeeded());
  CHECK(readBytes(linkedSourcePath) == linkedSourceBeforeSave);
  CHECK(QFileInfo(linkedSourcePath).lastModified() ==
        linkedSourceMtimeBeforeSave);
  CHECK(QDir(assemblyDirectory + QStringLiteral("/parts"))
            .entryList(QDir::AllEntries | QDir::Hidden | QDir::System,
                       QDir::Name) == linkedSourceEntriesBeforeSave);

  // Deterministic metadata matrix: a file-identity alias (hardlink model), a
  // canonical-path alias (symlink/reparse model), and a metadata failure all
  // reject before publication or identity/history commit. These checks do not
  // depend on host link privileges.
  const QString fakeHardlinkTarget =
      assemblyDirectory + QStringLiteral("/fake-hardlink-alias.solidar");
  const QByteArray fakeHardlinkSentinel("fake-hardlink-target");
  CHECK(writeBytes(fakeHardlinkTarget, fakeHardlinkSentinel));
  FakeAssemblySaveAsPathMetadataReader fakeHardlinkReader;
  fakeHardlinkReader.addFileIdentity(
      fakeHardlinkTarget, sameDirectoryCopy, PathRelation::Different);
  fakeHardlinkReader.addCanonical(
      fakeHardlinkTarget, sameDirectoryCopy, PathRelation::Different);
  fakeHardlinkReader.addFileIdentity(
      fakeHardlinkTarget, linkedSourcePath, PathRelation::Same);
  const auto fakeHardlinkIdentityBefore = *linkedAssembly->identity();
  const QByteArray fakeHardlinkSourceBefore = readBytes(linkedSourcePath);
  const auto fakeHardlinkRejected =
      solidar::application::detail::saveAssemblyV7As(
          fakeHardlinkTarget, *linkedAssembly,
          {{solidar::ProjectId{{0x74, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42,
                                0xd3, 0xa4, 0x56, 0x42, 0x66, 0x14, 0x17,
                                0x40, 0x07}},
            {"Fake hardlink alias", "2026-10-27T05:15:00Z"}},
           sameDirectoryCopy},
          fakeHardlinkReader);
  CHECK(!fakeHardlinkRejected.succeeded());
  CHECK(readBytes(fakeHardlinkTarget) == fakeHardlinkSentinel);
  CHECK(readBytes(linkedSourcePath) == fakeHardlinkSourceBefore);
  CHECK(linkedAssembly->identity() == fakeHardlinkIdentityBefore);

  const QString fakeSymlinkTarget =
      assemblyDirectory + QStringLiteral("/fake-symlink-alias.solidar");
  const QByteArray fakeSymlinkSentinel("fake-symlink-target");
  CHECK(writeBytes(fakeSymlinkTarget, fakeSymlinkSentinel));
  FakeAssemblySaveAsPathMetadataReader fakeSymlinkReader;
  fakeSymlinkReader.addFileIdentity(
      fakeSymlinkTarget, sameDirectoryCopy, PathRelation::Different);
  fakeSymlinkReader.addCanonical(
      fakeSymlinkTarget, sameDirectoryCopy, PathRelation::Different);
  fakeSymlinkReader.addFileIdentity(
      fakeSymlinkTarget, linkedSourcePath, PathRelation::Different);
  fakeSymlinkReader.addCanonical(
      fakeSymlinkTarget, linkedSourcePath, PathRelation::Same);
  const auto fakeSymlinkIdentityBefore = *linkedAssembly->identity();
  const auto fakeSymlinkRejected =
      solidar::application::detail::saveAssemblyV7As(
          fakeSymlinkTarget, *linkedAssembly,
          {{solidar::ProjectId{{0x75, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42,
                                0xd3, 0xa4, 0x56, 0x42, 0x66, 0x14, 0x17,
                                0x40, 0x07}},
            {"Fake symlink alias", "2026-10-27T05:20:00Z"}},
           sameDirectoryCopy},
          fakeSymlinkReader);
  CHECK(!fakeSymlinkRejected.succeeded());
  CHECK(readBytes(fakeSymlinkTarget) == fakeSymlinkSentinel);
  CHECK(readBytes(linkedSourcePath) == fakeHardlinkSourceBefore);
  CHECK(linkedAssembly->identity() == fakeSymlinkIdentityBefore);

  const QString metadataErrorTarget =
      assemblyDirectory + QStringLiteral("/metadata-error.solidar");
  const QByteArray metadataErrorSentinel("metadata-error-target");
  CHECK(writeBytes(metadataErrorTarget, metadataErrorSentinel));
  FakeAssemblySaveAsPathMetadataReader metadataErrorReader;
  metadataErrorReader.addFileIdentity(
      metadataErrorTarget, sameDirectoryCopy, PathRelation::Different);
  metadataErrorReader.addCanonical(
      metadataErrorTarget, sameDirectoryCopy, PathRelation::Different);
  metadataErrorReader.addFileIdentity(
      metadataErrorTarget, linkedSourcePath, PathRelation::Error);
  const auto metadataErrorIdentityBefore = *linkedAssembly->identity();
  const QByteArray metadataErrorAssemblyBefore = readBytes(sameDirectoryCopy);
  const QByteArray metadataErrorSourceBefore = readBytes(linkedSourcePath);
  const QStringList metadataErrorEntriesBefore =
      QDir(assemblyDirectory + QStringLiteral("/parts"))
          .entryList(QDir::AllEntries | QDir::Hidden | QDir::System,
                     QDir::Name);
  solidar::AssemblyCommandHistory untouchedHistory;
  untouchedHistory.markSavepoint();
  const auto untouchedHistorySize = untouchedHistory.size();
  const auto untouchedHistoryCursor = untouchedHistory.cursor();
  const auto untouchedHistoryBytes = untouchedHistory.retainedBytes();
  const auto untouchedHistoryRevision = untouchedHistory.currentRevision();
  const auto untouchedHistorySavepoint =
      untouchedHistory.savepointRevision();
  const auto metadataErrorRejected =
      solidar::application::detail::saveAssemblyV7As(
          metadataErrorTarget, *linkedAssembly,
          {{solidar::ProjectId{{0x76, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42,
                                0xd3, 0xa4, 0x56, 0x42, 0x66, 0x14, 0x17,
                                0x40, 0x07}},
            {"Metadata error", "2026-10-27T05:25:00Z"}},
           sameDirectoryCopy},
          metadataErrorReader);
  CHECK(!metadataErrorRejected.succeeded());
  CHECK(metadataErrorRejected.code ==
        solidar::ProjectApplicationErrorCode::SaveFailed);
  CHECK(readBytes(metadataErrorTarget) == metadataErrorSentinel);
  CHECK(readBytes(sameDirectoryCopy) == metadataErrorAssemblyBefore);
  CHECK(readBytes(linkedSourcePath) == metadataErrorSourceBefore);
  CHECK(QDir(assemblyDirectory + QStringLiteral("/parts"))
            .entryList(QDir::AllEntries | QDir::Hidden | QDir::System,
                       QDir::Name) == metadataErrorEntriesBefore);
  CHECK(linkedAssembly->identity() == metadataErrorIdentityBefore);
  CHECK(untouchedHistory.size() == untouchedHistorySize);
  CHECK(untouchedHistory.cursor() == untouchedHistoryCursor);
  CHECK(untouchedHistory.retainedBytes() == untouchedHistoryBytes);
  CHECK(untouchedHistory.currentRevision() == untouchedHistoryRevision);
  CHECK(untouchedHistory.savepointRevision() == untouchedHistorySavepoint);

  // Missing leaves are compared through their longest existing physical
  // parents plus the complete normalized suffix. This deterministic seam
  // models a symlink/junction parent even on hosts where creating one is not
  // permitted. Same rejects for both common V7/V8 policy routes; Missing and
  // Error are fail-closed, while a proven Different relation may publish.
  const auto makeMissingAliasProduct = [&] {
    solidar::Document localCache;
    localCache.setBox({60.0, 40.0, 25.0});
    return linkedAssemblyProduct(std::move(localCache), linkedBinding);
  };
  const QString missingAuthoredTarget = linkedSourcePath;
  const QString fakeParentAliasTarget =
      assemblyDirectory + QStringLiteral("/alias-parts/source.solidar");
  const QString fakeMultiLevelAliasTarget =
      assemblyDirectory +
      QStringLiteral("/alias-parts/deep/one/two/source.solidar");
  for (const auto [target, format, replacementProject] :
       std::array<std::tuple<QString,
                             solidar::project::ProjectPersistenceFormat,
                             solidar::ProjectId>, 2>{
           std::tuple{fakeParentAliasTarget,
                      solidar::project::ProjectPersistenceFormat::V7,
                      solidar::ProjectId{{0x83, 0x3e, 0x45, 0x67, 0xe8, 0x9b,
                                          0x42, 0xd3, 0xa4, 0x56, 0x42, 0x66,
                                          0x14, 0x17, 0x40, 0x18}}},
           std::tuple{fakeMultiLevelAliasTarget,
                      solidar::project::ProjectPersistenceFormat::V8,
                      solidar::ProjectId{{0x84, 0x3e, 0x45, 0x67, 0xe8, 0x9b,
                                          0x42, 0xd3, 0xa4, 0x56, 0x42, 0x66,
                                          0x14, 0x17, 0x40, 0x18}}}}) {
    auto candidate = makeMissingAliasProduct();
    const auto identityBefore = candidate->identity();
    FakeAssemblySaveAsPathMetadataReader reader;
    reader.addFileIdentity(target, sameDirectoryCopy,
                           PathRelation::Different);
    reader.addMissingPathAlias(target, missingAuthoredTarget,
                               PathRelation::Same);
    const auto rejected = solidar::application::detail::saveAssemblyAs(
        target, *candidate, {format},
        {{replacementProject,
          {"Missing parent alias", "2026-10-29T06:00:00Z"}},
         sameDirectoryCopy},
        reader);
    CHECK(!rejected.succeeded());
    CHECK(!QFile::exists(target));
    CHECK(candidate->identity() == identityBefore);
  }

  const QString missingRelationTarget =
      assemblyDirectory + QStringLiteral("/missing-parent-proof.solidar");
  for (const PathRelation relation :
       {PathRelation::Missing, PathRelation::Error}) {
    auto candidate = makeMissingAliasProduct();
    const auto identityBefore = candidate->identity();
    const QByteArray assemblyBefore = readBytes(sameDirectoryCopy);
    const QByteArray sourceBefore = readBytes(linkedSourcePath);
    FakeAssemblySaveAsPathMetadataReader reader;
    reader.addFileIdentity(missingRelationTarget, sameDirectoryCopy,
                           PathRelation::Different);
    reader.addMissingPathAlias(missingRelationTarget, missingAuthoredTarget,
                               relation);
    const auto rejected = solidar::application::detail::saveAssemblyAs(
        missingRelationTarget, *candidate,
        {solidar::project::ProjectPersistenceFormat::V8},
        {{solidar::ProjectId{{0x85, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42,
                              0xd3, 0xa4, 0x56, 0x42, 0x66, 0x14, 0x17,
                              static_cast<std::uint8_t>(0x40 +
                                  (relation == PathRelation::Error))}},
          {"Missing parent proof", "2026-10-29T06:05:00Z"}},
         sameDirectoryCopy},
        reader);
    CHECK(!rejected.succeeded());
    CHECK(!QFile::exists(missingRelationTarget));
    CHECK(candidate->identity() == identityBefore);
    CHECK(readBytes(sameDirectoryCopy) == assemblyBefore);
    CHECK(readBytes(linkedSourcePath) == sourceBefore);
  }

  // Destination spelling is validated before *any* lexical cleaning,
  // metadata resolution or writer preparation. Mixed separators, drive paths
  // and UNC forms cannot smuggle an explicit dot segment through a symlink or
  // junction parent. Existing bytes, identity and cold history remain exact.
  const QString dotParent =
      assemblyDirectory + QStringLiteral("/dot-parent");
  CHECK(QDir().mkpath(dotParent));
  const QString dotSentinelPath =
      assemblyDirectory + QStringLiteral("/dot-target.solidar");
  const QByteArray dotSentinel("dot-target-sentinel");
  CHECK(writeBytes(dotSentinelPath, dotSentinel));
  const QStringList dotDirectoryEntries =
      QDir(assemblyDirectory)
          .entryList(QDir::AllEntries | QDir::Hidden | QDir::System,
                     QDir::Name);
  solidar::AssemblyCommandHistory dotHistory;
  dotHistory.markSavepoint();
  const auto dotHistoryCursor = dotHistory.cursor();
  const auto dotHistoryRevision = dotHistory.currentRevision();
  const auto dotHistorySavepoint = dotHistory.savepointRevision();
  const auto dotHistoryBytes = dotHistory.retainedBytes();
  const std::array<std::pair<QString,
                             solidar::project::ProjectPersistenceFormat>, 10>
      invalidRawTargets{{
          {dotParent + QStringLiteral("/../dot-target.solidar"),
           solidar::project::ProjectPersistenceFormat::V7},
          {dotParent + QStringLiteral("\\..\\dot-target.solidar"),
           solidar::project::ProjectPersistenceFormat::V8},
          {QStringLiteral("C:\\alias-parent\\..\\source.solidar"),
           solidar::project::ProjectPersistenceFormat::V7},
          {QStringLiteral("\\\\server\\share\\.\\source.solidar"),
           solidar::project::ProjectPersistenceFormat::V8},
          {QStringLiteral("C:..\\source.solidar"),
           solidar::project::ProjectPersistenceFormat::V7},
          {QStringLiteral("c:./source.solidar"),
           solidar::project::ProjectPersistenceFormat::V8},
          {QStringLiteral("C:source.solidar"),
           solidar::project::ProjectPersistenceFormat::V7},
          {QStringLiteral("c:folder\\source.solidar"),
           solidar::project::ProjectPersistenceFormat::V8},
          {QStringLiteral("D:folder/source.solidar"),
           solidar::project::ProjectPersistenceFormat::V7},
          {QStringLiteral("e:"),
           solidar::project::ProjectPersistenceFormat::V8}}};
  for (std::size_t index = 0; index < invalidRawTargets.size(); ++index) {
    auto candidate = makeMissingAliasProduct();
    const auto identityBefore = candidate->identity();
    const QByteArray sourceBefore = readBytes(linkedSourcePath);
    FakeAssemblySaveAsPathMetadataReader reader;
    const auto rejected = solidar::application::detail::saveAssemblyAs(
        invalidRawTargets[index].first, *candidate,
        {invalidRawTargets[index].second},
        {{solidar::ProjectId{{
              static_cast<std::uint8_t>(0x90 + index), 0x3e, 0x45, 0x67,
              0xe8, 0x9b, 0x42, 0xd3, 0xa4, 0x56, 0x42, 0x66, 0x14, 0x17,
              0x40, 0x18}},
          {"Explicit dot target", "2026-10-29T07:00:00Z"}},
         sameDirectoryCopy},
        reader);
    CHECK(!rejected.succeeded());
    CHECK(reader.metadataCalls() == 0);
    CHECK(candidate->identity() == identityBefore);
    CHECK(readBytes(dotSentinelPath) == dotSentinel);
    CHECK(readBytes(linkedSourcePath) == sourceBefore);
    CHECK(QDir(assemblyDirectory)
              .entryList(QDir::AllEntries | QDir::Hidden | QDir::System,
                         QDir::Name) == dotDirectoryEntries);
    CHECK(dotHistory.cursor() == dotHistoryCursor);
    CHECK(dotHistory.currentRevision() == dotHistoryRevision);
    CHECK(dotHistory.savepointRevision() == dotHistorySavepoint);
    CHECK(dotHistory.retainedBytes() == dotHistoryBytes);
  }

  // Missing suffix comparison is explicit filesystem policy. A
  // case-insensitive, Unicode-normalizing directory treats ASCII case and
  // NFC/NFD spellings as aliases across every component. Unknown or failed
  // policy queries reject instead of guessing Different.
  struct SuffixPolicyCase final {
    QString targetName;
    std::vector<QString> targetSuffix;
    std::vector<QString> sourceSuffix;
    PathNameComparisonPolicy policy;
  };
  const QString composed = QString::fromUtf8("caf\xC3\xA9");
  const QString decomposed = QStringLiteral("cafe\u0301");
  const std::array<SuffixPolicyCase, 5> failClosedSuffixCases{{
      {QStringLiteral("case-fold-alias.solidar"),
       {QStringLiteral("Deep"), QStringLiteral("SOURCE.SOLIDAR")},
       {QStringLiteral("deep"), QStringLiteral("source.solidar")},
       PathNameComparisonPolicy::UnicodeNormalizedCaseInsensitive},
      {QStringLiteral("unicode-normalized-alias.solidar"),
       {QStringLiteral("nested"), composed},
       {QStringLiteral("NESTED"), decomposed},
       PathNameComparisonPolicy::UnicodeNormalizedCaseInsensitive},
      {QStringLiteral("normalized-sensitive-alias.solidar"),
       {QStringLiteral("nested"), QStringLiteral("level"), composed},
       {QStringLiteral("nested"), QStringLiteral("level"), decomposed},
       PathNameComparisonPolicy::UnicodeNormalizedCaseSensitive},
      {QStringLiteral("unknown-name-policy.solidar"),
       {QStringLiteral("same")}, {QStringLiteral("same")},
       PathNameComparisonPolicy::Unknown},
      {QStringLiteral("failed-name-policy.solidar"),
       {QStringLiteral("same")}, {QStringLiteral("same")},
       PathNameComparisonPolicy::Error}}};
  for (std::size_t index = 0; index < failClosedSuffixCases.size(); ++index) {
    const auto& test = failClosedSuffixCases[index];
    const QString target = assemblyDirectory + QLatin1Char('/') +
                           test.targetName;
    const QByteArray sentinel("suffix-policy-sentinel");
    CHECK(writeBytes(target, sentinel));
    auto candidate = makeMissingAliasProduct();
    const auto identityBefore = candidate->identity();
    const QByteArray sourceBefore = readBytes(linkedSourcePath);
    FakeAssemblySaveAsPathMetadataReader reader;
    reader.addFileIdentity(target, sameDirectoryCopy,
                           PathRelation::Different);
    const QString physicalParent =
        QStringLiteral("C:/same-policy-parent-%1").arg(index);
    reader.addMissingPathResolution(
        target, MissingPathResolutionStatus::Resolved, physicalParent,
        test.targetSuffix, test.policy);
    reader.addMissingPathResolution(
        missingAuthoredTarget, MissingPathResolutionStatus::Resolved,
        physicalParent, test.sourceSuffix, test.policy);
    const auto rejected = solidar::application::detail::saveAssemblyAs(
        target, *candidate,
        {index % 2 == 0
             ? solidar::project::ProjectPersistenceFormat::V7
             : solidar::project::ProjectPersistenceFormat::V8},
        {{solidar::ProjectId{{
              static_cast<std::uint8_t>(0x94 + index), 0x3e, 0x45, 0x67,
              0xe8, 0x9b, 0x42, 0xd3, 0xa4, 0x56, 0x42, 0x66, 0x14, 0x17,
              0x40, 0x18}},
          {"Suffix policy", "2026-10-29T07:05:00Z"}},
         sameDirectoryCopy},
        reader);
    CHECK(!rejected.succeeded());
    CHECK(readBytes(target) == sentinel);
    CHECK(readBytes(linkedSourcePath) == sourceBefore);
    CHECK(candidate->identity() == identityBefore);
    CHECK(dotHistory.cursor() == dotHistoryCursor);
    CHECK(dotHistory.currentRevision() == dotHistoryRevision);
    CHECK(dotHistory.savepointRevision() == dotHistorySavepoint);
    CHECK(dotHistory.retainedBytes() == dotHistoryBytes);
  }

  // macOS-style normalized-but-case-sensitive comparison treats NFC/NFD as
  // equal while retaining case. A case difference in one component is
  // therefore Distinct even when another component differs only by Unicode
  // normalization, and the exact target spelling reaches the V8 writer.
  const QString caseSensitiveTarget =
      assemblyDirectory + QStringLiteral("/case-sensitive-target.solidar");
  auto caseSensitiveProduct = makeMissingAliasProduct();
  FakeAssemblySaveAsPathMetadataReader caseSensitiveReader;
  caseSensitiveReader.addFileIdentity(
      caseSensitiveTarget, sameDirectoryCopy, PathRelation::Different);
  const QString caseSensitiveParent =
      QStringLiteral("C:/case-sensitive-parent");
  caseSensitiveReader.addMissingPathResolution(
      caseSensitiveTarget, MissingPathResolutionStatus::Resolved,
      caseSensitiveParent,
      {QStringLiteral("Deep"), composed},
      PathNameComparisonPolicy::UnicodeNormalizedCaseSensitive);
  caseSensitiveReader.addMissingPathResolution(
      missingAuthoredTarget, MissingPathResolutionStatus::Resolved,
      caseSensitiveParent,
      {QStringLiteral("deep"), decomposed},
      PathNameComparisonPolicy::UnicodeNormalizedCaseSensitive);
  const solidar::ProductDocumentIdentity caseSensitiveIdentity{
      solidar::ProjectId{{0x99, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                          0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x18}},
      {"Case-sensitive V8", "2026-10-29T07:10:00Z"}};
  CHECK(solidar::application::detail::saveAssemblyAs(
            caseSensitiveTarget, *caseSensitiveProduct,
            {solidar::project::ProjectPersistenceFormat::V8},
            {caseSensitiveIdentity, sameDirectoryCopy},
            caseSensitiveReader)
            .succeeded());
  CHECK(QFile::exists(caseSensitiveTarget));
  CHECK(caseSensitiveProduct->identity() == caseSensitiveIdentity);

  // The Linux/POSIX exact policy deliberately does not normalize names. NFC
  // and NFD components under the same physical parent remain Distinct.
  const QString exactUnicodeTarget =
      assemblyDirectory + QStringLiteral("/exact-unicode-target.solidar");
  auto exactUnicodeProduct = makeMissingAliasProduct();
  FakeAssemblySaveAsPathMetadataReader exactUnicodeReader;
  exactUnicodeReader.addFileIdentity(
      exactUnicodeTarget, sameDirectoryCopy, PathRelation::Different);
  const QString exactUnicodeParent =
      QStringLiteral("C:/exact-unicode-parent");
  exactUnicodeReader.addMissingPathResolution(
      exactUnicodeTarget, MissingPathResolutionStatus::Resolved,
      exactUnicodeParent, {QStringLiteral("nested"), composed},
      PathNameComparisonPolicy::CaseSensitiveExact);
  exactUnicodeReader.addMissingPathResolution(
      missingAuthoredTarget, MissingPathResolutionStatus::Resolved,
      exactUnicodeParent, {QStringLiteral("nested"), decomposed},
      PathNameComparisonPolicy::CaseSensitiveExact);
  const solidar::ProductDocumentIdentity exactUnicodeIdentity{
      solidar::ProjectId{{0x9b, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                          0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x18}},
      {"Exact Unicode V7", "2026-10-29T07:12:00Z"}};
  CHECK(solidar::application::detail::saveAssemblyAs(
            exactUnicodeTarget, *exactUnicodeProduct,
            {solidar::project::ProjectPersistenceFormat::V7},
            {exactUnicodeIdentity, sameDirectoryCopy}, exactUnicodeReader)
            .succeeded());
  CHECK(QFile::exists(exactUnicodeTarget));
  CHECK(exactUnicodeProduct->identity() == exactUnicodeIdentity);

  const QString provenDifferentTarget =
      assemblyDirectory + QStringLiteral("/proven-different-v8.solidar");
  auto provenDifferentProduct = makeMissingAliasProduct();
  FakeAssemblySaveAsPathMetadataReader provenDifferentReader;
  provenDifferentReader.addFileIdentity(
      provenDifferentTarget, sameDirectoryCopy, PathRelation::Different);
  provenDifferentReader.addMissingPathAlias(
      provenDifferentTarget, missingAuthoredTarget, PathRelation::Different);
  const solidar::ProductDocumentIdentity provenDifferentIdentity{
      solidar::ProjectId{{0x87, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                          0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x18}},
      {"Proven different V8", "2026-10-29T06:10:00Z"}};
  CHECK(solidar::application::detail::saveAssemblyAs(
            provenDifferentTarget, *provenDifferentProduct,
            {solidar::project::ProjectPersistenceFormat::V8},
            {provenDifferentIdentity, sameDirectoryCopy},
            provenDifferentReader)
            .succeeded());
  CHECK(QFile::exists(provenDifferentTarget));
  CHECK(provenDifferentProduct->identity() == provenDifferentIdentity);

  // Two distinct directory trees carry byte-identical linked sources. The
  // injected read-only identity relation models a common canonical source, so
  // cross-directory Save As succeeds deterministically without native links.
  const QString fakeCrossOldRoot =
      directory.filePath(QStringLiteral("fake-cross-old"));
  const QString fakeCrossNewRoot =
      directory.filePath(QStringLiteral("fake-cross-new"));
  CHECK(QDir().mkpath(fakeCrossOldRoot + QStringLiteral("/parts")));
  CHECK(QDir().mkpath(fakeCrossNewRoot + QStringLiteral("/parts")));
  const QString fakeCrossOldSource =
      fakeCrossOldRoot + QStringLiteral("/parts/source.solidar");
  const QString fakeCrossNewSource =
      fakeCrossNewRoot + QStringLiteral("/parts/source.solidar");
  CHECK(writeBytes(fakeCrossOldSource, linkedSourceEncoded.payload->bytes));
  CHECK(writeBytes(fakeCrossNewSource, linkedSourceEncoded.payload->bytes));
  const QString fakeCrossAssembly =
      fakeCrossOldRoot + QStringLiteral("/assembly.solidar");
  CHECK(writeBytes(fakeCrossAssembly, v7Encoded.payload->bytes));
  const QString fakeCrossTarget =
      fakeCrossNewRoot + QStringLiteral("/assembly-copy.solidar");
  FakeAssemblySaveAsPathMetadataReader fakeCrossReader;
  fakeCrossReader.addFileIdentity(
      fakeCrossTarget, fakeCrossAssembly, PathRelation::Different);
  fakeCrossReader.addCanonical(
      fakeCrossTarget, fakeCrossAssembly, PathRelation::Different);
  fakeCrossReader.addFileIdentity(
      fakeCrossTarget, fakeCrossOldSource, PathRelation::Different);
  fakeCrossReader.addCanonical(
      fakeCrossTarget, fakeCrossOldSource, PathRelation::Different);
  fakeCrossReader.addFileIdentity(
      QFileInfo(fakeCrossOldSource).canonicalFilePath(),
      QFileInfo(fakeCrossNewSource).canonicalFilePath(), PathRelation::Same);
  const auto fakeCrossSave =
      solidar::application::detail::saveAssemblyV7As(
          fakeCrossTarget, *linkedAssembly,
          {{solidar::ProjectId{{0x77, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42,
                                0xd3, 0xa4, 0x56, 0x42, 0x66, 0x14, 0x17,
                                0x40, 0x07}},
            {"Fake canonical cross copy", "2026-10-27T05:30:00Z"}},
           fakeCrossAssembly},
          fakeCrossReader);
  CHECK(fakeCrossSave.succeeded());
  CHECK(QFile::exists(fakeCrossTarget));
  CHECK(service.stageOpenProduct(fakeCrossTarget).succeeded());
  CHECK(readBytes(fakeCrossOldSource) == linkedSourceEncoded.payload->bytes);
  CHECK(readBytes(fakeCrossNewSource) == linkedSourceEncoded.payload->bytes);

  const QByteArray linkedSourceBeforeAlias = readBytes(linkedSourcePath);
  const auto identityBeforeAlias = *linkedAssembly->identity();
  const auto aliasRejected = service.saveAssemblyV7As(
      linkedSourcePath, *linkedAssembly,
      {{solidar::ProjectId{{0x62, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                            0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x05}},
        {"Alias", "2026-10-27T04:00:00Z"}},
       sameDirectoryCopy});
  CHECK(!aliasRejected.succeeded());
  CHECK(readBytes(linkedSourcePath) == linkedSourceBeforeAlias);
  CHECK(linkedAssembly->identity() == identityBeforeAlias);

  std::error_code hardlinkError;
  const QString hardlinkAlias =
      assemblyDirectory + QStringLiteral("/hardlink-source-alias.solidar");
  std::filesystem::create_hard_link(
      std::filesystem::path(linkedSourcePath.toStdWString()),
      std::filesystem::path(hardlinkAlias.toStdWString()), hardlinkError);
  CHECK(!hardlinkError || linkCreationUnavailableByPrivilege(hardlinkError));
  if (!hardlinkError) {
    const auto hardlinkIdentityBefore = *linkedAssembly->identity();
    const QByteArray hardlinkSourceBefore = readBytes(linkedSourcePath);
    const auto hardlinkRejected = service.saveAssemblyV7As(
        hardlinkAlias, *linkedAssembly,
        {{solidar::ProjectId{{0x72, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42,
                              0xd3, 0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40,
                              0x07}},
          {"Hardlink alias", "2026-10-27T05:00:00Z"}},
         sameDirectoryCopy});
    CHECK(!hardlinkRejected.succeeded());
    CHECK(readBytes(linkedSourcePath) == hardlinkSourceBefore);
    CHECK(linkedAssembly->identity() == hardlinkIdentityBefore);
  }

  std::error_code symlinkError;
  const QString symlinkAlias =
      assemblyDirectory + QStringLiteral("/symlink-source-alias.solidar");
  std::filesystem::create_symlink(
      std::filesystem::path(linkedSourcePath.toStdWString()),
      std::filesystem::path(symlinkAlias.toStdWString()), symlinkError);
  CHECK(!symlinkError || linkCreationUnavailableByPrivilege(symlinkError));
  if (!symlinkError) {
    const auto symlinkIdentityBefore = *linkedAssembly->identity();
    const QByteArray symlinkSourceBefore = readBytes(linkedSourcePath);
    const auto symlinkRejected = service.saveAssemblyV7As(
        symlinkAlias, *linkedAssembly,
        {{solidar::ProjectId{{0x73, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                              0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x07}},
          {"Symlink alias", "2026-10-27T05:30:00Z"}},
         sameDirectoryCopy});
    CHECK(!symlinkRejected.succeeded());
    CHECK(readBytes(linkedSourcePath) == symlinkSourceBefore);
    CHECK(linkedAssembly->identity() == symlinkIdentityBefore);
  }

  // Supplemental native proof for a missing multi-component leaf below a
  // directory symlink/junction. The authored and destination leaves do not
  // exist, but their physical parent plus suffix are identical.
  const QString nativeMissingRoot =
      directory.filePath(QStringLiteral("native-missing-parent-alias"));
  const QString nativeRealParent =
      nativeMissingRoot + QStringLiteral("/parts");
  const QString nativeAliasParent =
      nativeMissingRoot + QStringLiteral("/alias-parts");
  CHECK(QDir().mkpath(nativeRealParent));
  std::error_code directoryLinkError;
  std::filesystem::create_directory_symlink(
      std::filesystem::path(nativeRealParent.toStdWString()),
      std::filesystem::path(nativeAliasParent.toStdWString()),
      directoryLinkError);
  CHECK(!directoryLinkError ||
        linkCreationUnavailableByPrivilege(directoryLinkError));
  if (!directoryLinkError) {
    auto nativeBinding = linkedBinding;
    nativeBinding.relativePathHintUtf8 = "parts/deep/one/source.solidar";
    CHECK(QDir().mkpath(nativeRealParent + QStringLiteral("/deep/one")));
    solidar::Document nativeCache;
    nativeCache.setBox({60.0, 40.0, 25.0});
    auto nativeProduct =
        linkedAssemblyProduct(std::move(nativeCache), nativeBinding);
    const auto nativeIdentityBefore = nativeProduct->identity();
    const QString nativeCurrent =
        nativeMissingRoot + QStringLiteral("/assembly.solidar");
    CHECK(writeBytes(nativeCurrent, v7Encoded.payload->bytes));
    const QString nativeAliasLeaf =
        nativeAliasParent + QStringLiteral("/deep/one/source.solidar");
    const auto nativeRejected = service.saveAssemblyV7As(
        nativeAliasLeaf, *nativeProduct,
        {{solidar::ProjectId{{0x88, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42,
                              0xd3, 0xa4, 0x56, 0x42, 0x66, 0x14, 0x17,
                              0x40, 0x18}},
          {"Native missing parent alias", "2026-10-29T06:15:00Z"}},
         nativeCurrent});
    CHECK(!nativeRejected.succeeded());
    CHECK(!QFile::exists(nativeAliasLeaf));
    CHECK(nativeProduct->identity() == nativeIdentityBefore);

    auto nativeDotProduct = linkedAssemblyProduct(
        solidar::Document{}, nativeBinding);
    const auto nativeDotIdentityBefore = nativeDotProduct->identity();
    const QByteArray nativeCurrentBefore = readBytes(nativeCurrent);
    const QStringList nativeEntriesBefore =
        QDir(nativeMissingRoot)
            .entryList(QDir::AllEntries | QDir::Hidden | QDir::System,
                       QDir::Name);
    const QString nativeDotLeaf =
        nativeAliasParent + QStringLiteral("/deep/one/../source.solidar");
    const auto nativeDotRejected = service.saveAssemblyAs(
        nativeDotLeaf, *nativeDotProduct,
        {solidar::project::ProjectPersistenceFormat::V8},
        {{solidar::ProjectId{{0x9a, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42,
                              0xd3, 0xa4, 0x56, 0x42, 0x66, 0x14, 0x17,
                              0x40, 0x18}},
          {"Native dot alias", "2026-10-29T07:15:00Z"}},
         nativeCurrent});
    CHECK(!nativeDotRejected.succeeded());
    CHECK(!QFile::exists(nativeAliasParent +
                         QStringLiteral("/deep/source.solidar")));
    CHECK(readBytes(nativeCurrent) == nativeCurrentBefore);
    CHECK(nativeDotProduct->identity() == nativeDotIdentityBefore);
    CHECK(QDir(nativeMissingRoot)
              .entryList(QDir::AllEntries | QDir::Hidden | QDir::System,
                         QDir::Name) == nativeEntriesBefore);
  }

  // Exact V9 routing, empty diagnostics, exact-format Save and explicit-only
  // promotion. V10 is Future and the compatibility part stage never projects
  // an assembly definition.
  {
    auto v9Product = assemblyProduct(boxDocument("V9 A"),
                                     boxDocument("V9 B"));
    const solidar::project::ProductEnvelopeV5Metadata v9Metadata{
        QStringLiteral("V9 application"),
        QStringLiteral("2026-10-30T00:00:00Z")};
    const auto v9Encoded = solidar::project::encodeProductEnvelopeV9Root(
        *v9Product, kAssemblyProjectId, v9Metadata);
    CHECK(v9Encoded.succeeded());
    const QString v9Path =
        directory.filePath(QStringLiteral("application-v9.solidar"));
    CHECK(writeBytes(v9Path, v9Encoded.payload->bytes));
    CHECK(!service.stageOpen(v9Path).succeeded());
    auto openedV9 = service.stageOpenProduct(v9Path);
    CHECK(openedV9.succeeded());
    CHECK(openedV9.staged->persistenceState ==
          solidar::project::ProjectPersistenceState{
              solidar::project::ProjectPersistenceFormat::V9});
    CHECK(openedV9.staged->mateRuntimeDiagnostics.empty());

    auto loadedV9 =
        std::move(*openedV9.staged).releaseProductDocument();
    const QString rejectedAssemblyV11Path =
        directory.filePath(QStringLiteral("rejected-assembly-v11.solidar"));
    CHECK(!service
               .saveAssembly(
                   rejectedAssemblyV11Path, loadedV9,
                   {solidar::project::ProjectPersistenceFormat::V11})
               .succeeded());
    CHECK(!QFile::exists(rejectedAssemblyV11Path));
    const QString savedV9Path =
        directory.filePath(QStringLiteral("saved-application-v9.solidar"));
    CHECK(service
              .saveAssembly(savedV9Path, loadedV9,
                            {solidar::project::ProjectPersistenceFormat::V9})
              .succeeded());
    const auto savedV9 = service.stageOpenProduct(savedV9Path);
    CHECK(savedV9.succeeded());
    CHECK(savedV9.staged->persistenceState->format ==
          solidar::project::ProjectPersistenceFormat::V9);
    const QString copiedV9Path =
        directory.filePath(QStringLiteral("copied-application-v9.solidar"));
    const solidar::ProductDocumentIdentity copiedV9Identity{
        solidar::ProjectId{{0x92, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                            0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x09}},
        {"V9 copy", "2026-10-30T01:00:00Z"}};
    CHECK(service
              .saveAssemblyAs(
                  copiedV9Path, loadedV9,
                  {solidar::project::ProjectPersistenceFormat::V9},
                  {copiedV9Identity, savedV9Path})
              .succeeded());
    const auto copiedV9 = service.stageOpenProduct(copiedV9Path);
    CHECK(copiedV9.succeeded());
    CHECK(copiedV9.staged->persistenceState->format ==
          solidar::project::ProjectPersistenceFormat::V9);
    CHECK(loadedV9.identity() == copiedV9Identity);

    auto futureRoot = v9Encoded.payload->root;
    futureRoot.insert(QStringLiteral("version"), 13);
    const QString futurePath =
        directory.filePath(QStringLiteral("future-v13.solidar"));
    CHECK(writeBytes(futurePath,
                     QJsonDocument(futureRoot).toJson(QJsonDocument::Compact)));
    const auto future = service.stageOpenProduct(futurePath);
    CHECK(!future.succeeded());
    CHECK(future.status.code ==
          solidar::ProjectApplicationErrorCode::UnsupportedProject);

    auto state = solidar::project::ProjectPersistenceState{
        solidar::project::ProjectPersistenceFormat::V8};
    CHECK(service.validateAssemblyV9Promotion(loadedV9, state).succeeded());
    CHECK(state.format == solidar::project::ProjectPersistenceFormat::V8);
    CHECK(service.promoteAssemblyPersistenceToV9(state, loadedV9).succeeded());
    CHECK(state.format == solidar::project::ProjectPersistenceFormat::V9);

    auto custom = solidar::MateSolverOptions{};
    custom.maximumIterations = 32;
    CHECK(v9Product->restoreAssemblyMateState(
              solidar::DefinitionId{100}, custom, {}) ==
          solidar::AssemblyMateRestoreStatus::Restored);
    CHECK(!solidar::project::encodeProductEnvelopeV7Root(
               *v9Product, kAssemblyProjectId, v9Metadata)
               .succeeded());
    CHECK(!solidar::project::encodeProductEnvelopeV8Root(
               *v9Product, kAssemblyProjectId, v9Metadata)
               .succeeded());
    CHECK(solidar::project::encodeProductEnvelopeV9Root(
              *v9Product, kAssemblyProjectId, v9Metadata)
              .succeeded());
  }

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
