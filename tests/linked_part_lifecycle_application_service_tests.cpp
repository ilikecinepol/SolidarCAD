#include "TestAssertions.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdlib>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "project/internal/LinkedPartSourceFileSystem.h"
#include "project/ProductEnvelopeV7Decoder.h"
#include "project/ProductEnvelopeV7Encoder.h"
#include "project/ProductEnvelopeV7Materialization.h"
#include "ui/application/LinkedPartLifecycleService.h"

using namespace solidar;
using namespace solidar::project;

namespace {

constexpr ProjectId kProject{
    {0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00}};
constexpr ExternalSourceKey kKey{kProject, DefinitionId{17}};

QByteArray sourceBytes(double width, QString projectId =
                                         QStringLiteral(
                                             "123e4567-e89b-42d3-a456-426614174000")) {
  const QJsonObject v4{
      {"format", "solidar-project"},
      {"version", 4},
      {"capabilities", QJsonArray{"persistent-topology-v2"}},
      {"constraintTypeEncoding", "stable-name-v1"},
      {"name", "linked"},
      {"createdAt", "2026-10-27T00:00:00Z"},
      {"document", QJsonObject{{"widthMm", width},
                                 {"heightMm", 40},
                                 {"extrusionMm", 25}}},
      {"sketches", QJsonArray{}},
      {"extrusion", QJsonObject{{"enabled", false}}},
      {"model", QJsonObject{{"sketches", QJsonArray{}},
                              {"bodies", QJsonArray{}},
                              {"referenceImages", QJsonArray{}}}}};
  const QJsonObject definition{
      {"definitionId", 17},
      {"definitionKind", "part"},
      {"payload", QJsonObject{{"payloadKind", "embedded-part-v4"},
                                {"data", v4}}}};
  return QJsonDocument(QJsonObject{
                           {"format", "solidar-project"},
                           {"version", 5},
                           {"capabilities", QJsonArray{"product-envelope-v1"}},
                           {"projectId", std::move(projectId)},
                           {"rootDefinitionId", 17},
                           {"metadata", QJsonObject{{"name", "linked"},
                                                     {"createdAt", "2026-10-27T00:00:00Z"}}},
                           {"definitions", QJsonArray{definition}}})
      .toJson(QJsonDocument::Compact);
}

class ReadHandle final : public project::detail::LinkedSourceReadHandle {
 public:
  ReadHandle(QByteArray bytes, std::uint64_t& reads)
      : bytes_(std::move(bytes)), reads_(reads) {}
  project::detail::LinkedSourceReadResult readBounded(
      std::uint64_t maximumBytes, std::uint64_t maximumWorkUnits) override {
    ++reads_;
    if (maximumWorkUnits == 0 ||
        static_cast<std::uint64_t>(bytes_.size()) > maximumBytes)
      return {project::detail::LinkedSourceOpenStatus::BudgetExceeded, {}, 1,
              QStringLiteral("budget")};
    return {project::detail::LinkedSourceOpenStatus::Opened, bytes_, 1, {}};
  }

 private:
  QByteArray bytes_;
  std::uint64_t& reads_;
};

class ExactReadFileSystem final
    : public project::detail::LinkedPartSourceFileSystem {
 public:
  project::detail::LinkedSourceCanonicalRoot canonicalizeRoot(
      const QString& path, std::uint32_t, std::uint64_t) override {
    return {project::detail::LinkedSourceOpenStatus::Opened, path, 1, 0, {}};
  }
  project::detail::LinkedSourceOpenedFile openExact(
      const QString& path, std::uint32_t, std::uint64_t) override {
    ++opens;
    const auto found = files.find(path);
    if (found == files.end()) {
      project::detail::LinkedSourceOpenedFile missing;
      missing.status = project::detail::LinkedSourceOpenStatus::Missing;
      missing.workUnits = 1;
      missing.error = QStringLiteral("missing");
      return missing;
    }
    project::detail::LinkedSourceOpenedFile opened;
    opened.status = project::detail::LinkedSourceOpenStatus::Opened;
    opened.canonicalPath = path;
    opened.fileIdentity = path;
    opened.objectVersion = QStringLiteral("v1");
    opened.sizeBytes = static_cast<std::uint64_t>(found->second.size());
    opened.workUnits = 1;
    opened.handle = std::make_unique<ReadHandle>(found->second, reads);
    return opened;
  }

  std::map<QString, QByteArray> files;
  std::uint64_t opens{};
  std::uint64_t reads{};
};

class AcceptSuccessor final : public ProductDocumentSuccessor {
 public:
  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&, ProductDocumentSuccessorImpact value) noexcept override {
    ++calls;
    impact = value;
    return ProductDocumentSuccessorStatus::Prepared;
  }
  int calls{};
  ProductDocumentSuccessorImpact impact{
      ProductDocumentSuccessorImpact::FullProjection};
};

class StatusSuccessor final : public ProductDocumentSuccessor {
 public:
  explicit StatusSuccessor(ProductDocumentSuccessorStatus status) noexcept
      : status_(status) {}
  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&,
      ProductDocumentSuccessorImpact) noexcept override {
    ++calls;
    return status_;
  }
  int calls{};

 private:
  ProductDocumentSuccessorStatus status_;
};

std::unique_ptr<ProductDocument> linkedAssembly(const QByteArray& accepted) {
  LinkedPartBinding binding{kKey, "parts/source.solidar",
                            linkedSourceSha256(QByteArrayView(accepted))};
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{2}, std::make_unique<Document>(), binding)) ==
        DefinitionInsertStatus::Inserted);
  AssemblyDefinition root(DefinitionId{1});
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{3}, DefinitionId{2}, RigidTransform3d::identity(),
            {}, "first")) == AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{4}, DefinitionId{2}, RigidTransform3d::identity(),
            {}, "second")) == AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{1});
  CHECK(made);
  return std::move(made.product);
}

OccurrencePath path(std::initializer_list<std::uint64_t> ids) {
  std::vector<OccurrenceId> entries;
  entries.reserve(ids.size());
  for (const auto id : ids) entries.emplace_back(id);
  auto made = OccurrencePath::create(std::move(entries));
  CHECK(made);
  return std::move(*made.path);
}

std::unique_ptr<ProductDocument> nestedLinkedAssembly(
    const QByteArray& accepted) {
  LinkedPartBinding binding{kKey, "parts/source.solidar",
                            linkedSourceSha256(QByteArrayView(accepted))};
  std::vector<PartDefinition> parts;
  parts.emplace_back(DefinitionId{2}, std::make_unique<Document>(), binding);
  std::vector<AssemblyDefinition> assemblies;
  AssemblyDefinition child(DefinitionId{10});
  CHECK(child.insert(ComponentOccurrence(
            OccurrenceId{7}, DefinitionId{2}, RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  AssemblyDefinition root(DefinitionId{1});
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{3}, DefinitionId{10},
            RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{4}, DefinitionId{10},
            RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  assemblies.push_back(std::move(child));
  assemblies.push_back(std::move(root));
  auto made = ProductDocument::createNested(
      std::move(parts), std::move(assemblies), DefinitionId{1});
  CHECK(made);
  return std::move(made.product);
}

LinkedPartSourcePolicy policy() {
  LinkedPartSourcePolicy result;
  result.projectRoot = QStringLiteral("C:/Project");
  result.pathSemantics = LinkedPathSemantics::Windows;
  return result;
}

}  // namespace

int main() {
  const QByteArray accepted = sourceBytes(60.0);
  const QByteArray revision = sourceBytes(77.0);
  {
    LinkedPartBinding binding{kKey, "one/source.solidar",
                              linkedSourceSha256(QByteArrayView(accepted))};
    DefinitionRegistry definitions;
    CHECK(definitions.insert(PartDefinition(
              DefinitionId{2}, std::make_unique<Document>(), binding)) ==
          DefinitionInsertStatus::Inserted);
    binding.relativePathHintUtf8 = "two/source.solidar";
    CHECK(definitions.insert(PartDefinition(
              DefinitionId{3}, std::make_unique<Document>(), binding)) ==
          DefinitionInsertStatus::DuplicateExternalSourceKey);
    CHECK(definitions.size() == 1);
  }
  auto product = linkedAssembly(accepted);
  ExactReadFileSystem fileSystem;
  fileSystem.files.emplace(QStringLiteral("C:/Project/parts/source.solidar"),
                           revision);
  AssemblyCommandHistory history;
  AcceptSuccessor successor;

  const auto reload = LinkedPartLifecycleService::reload(
      *product, DefinitionId{2}, policy(), fileSystem, history, successor,
      {{}, DefinitionId{2}}, {{}, DefinitionId{2}});
  CHECK(reload.status == LinkedPartLifecycleStatus::Committed);
  CHECK(reload.resolverStatus == LinkedPartSourceResolveStatus::Resolved);
  CHECK(reload.historyStatus == AssemblyCommandHistoryStatus::Committed);
  CHECK(successor.calls == 1 &&
        successor.impact == ProductDocumentSuccessorImpact::DefinitionProjection);
  CHECK(product->partDocument(DefinitionId{2})->box().widthMm == 77.0);
  CHECK(product->partLinkedBinding(DefinitionId{2})->acceptedHash ==
        linkedSourceSha256(QByteArrayView(revision)));
  CHECK(product->rootAssemblyDefinition()->find(OccurrenceId{3}) &&
        product->rootAssemblyDefinition()->find(OccurrenceId{4}));

  const auto readsAfterReload = fileSystem.reads;
  fileSystem.files.clear();
  const auto reloadBindingBeforeFailedUndo =
      *product->partLinkedBinding(DefinitionId{2});
  const auto reloadStateBeforeFailedUndo =
      product->definitionStateId(DefinitionId{2});
  const auto reloadCursorBeforeFailedUndo = history.cursor();
  StatusSuccessor rejectedReloadUndo(
      ProductDocumentSuccessorStatus::Rejected);
  CHECK(history.undo(*product, rejectedReloadUndo).status ==
        AssemblyCommandHistoryStatus::SuccessorRejected);
  CHECK(rejectedReloadUndo.calls == 1);
  CHECK(fileSystem.reads == readsAfterReload);
  CHECK(history.cursor() == reloadCursorBeforeFailedUndo);
  CHECK(product->definitionStateId(DefinitionId{2}) ==
        reloadStateBeforeFailedUndo);
  CHECK(*product->partLinkedBinding(DefinitionId{2}) ==
        reloadBindingBeforeFailedUndo);
  CHECK(history.undo(*product, successor).status ==
        AssemblyCommandHistoryStatus::Committed);
  const auto redoTailSize = history.size();
  const auto redoTailCursor = history.cursor();
  const auto redoTailRevision = history.currentRevision();
  const auto redoTailRetained = history.retainedBytes();
  CHECK(history.canRedo());
  const auto failedWithRedoTail = LinkedPartLifecycleService::repair(
      *product, DefinitionId{2}, QStringLiteral("missing/source.solidar"),
      policy(), fileSystem, history, successor);
  CHECK(failedWithRedoTail.status ==
        LinkedPartLifecycleStatus::ResolverRejected);
  CHECK(history.size() == redoTailSize &&
        history.cursor() == redoTailCursor && history.canRedo() &&
        history.currentRevision() == redoTailRevision &&
        history.retainedBytes() == redoTailRetained);
  const auto reloadBindingBeforeFailedRedo =
      *product->partLinkedBinding(DefinitionId{2});
  const auto reloadStateBeforeFailedRedo =
      product->definitionStateId(DefinitionId{2});
  StatusSuccessor resourceReloadRedo(
      ProductDocumentSuccessorStatus::ResourceFailure);
  CHECK(history.redo(*product, resourceReloadRedo).status ==
        AssemblyCommandHistoryStatus::ResourceLimit);
  CHECK(resourceReloadRedo.calls == 1);
  CHECK(fileSystem.reads == readsAfterReload);
  CHECK(history.size() == redoTailSize &&
        history.cursor() == redoTailCursor && history.canRedo() &&
        history.currentRevision() == redoTailRevision &&
        history.retainedBytes() == redoTailRetained);
  CHECK(product->definitionStateId(DefinitionId{2}) ==
        reloadStateBeforeFailedRedo);
  CHECK(*product->partLinkedBinding(DefinitionId{2}) ==
        reloadBindingBeforeFailedRedo);
  CHECK(history.redo(*product, successor).status ==
        AssemblyCommandHistoryStatus::Committed);
  CHECK(fileSystem.reads == readsAfterReload);

  const QByteArray newer = sourceBytes(88.0);
  fileSystem.files.emplace(
      QStringLiteral("C:/Project/relocated/source.solidar"), newer);
  const SourceContentHash acceptedHash =
      product->partLinkedBinding(DefinitionId{2})->acceptedHash;
  const Document* documentAddress = product->partDocument(DefinitionId{2});
  bool resolvedOutdatedBeforeCommit = false;
  const auto repair = LinkedPartLifecycleService::repair(
      *product, DefinitionId{2}, QStringLiteral("relocated/source.solidar"),
      policy(), fileSystem, history, successor, {}, {},
      &resolvedOutdatedBeforeCommit);
  CHECK(repair.status == LinkedPartLifecycleStatus::Committed);
  CHECK(repair.outdated);
  CHECK(resolvedOutdatedBeforeCommit);
  CHECK(product->partLinkedBinding(DefinitionId{2})->relativePathHintUtf8 ==
        "relocated/source.solidar");
  CHECK(product->partLinkedBinding(DefinitionId{2})->acceptedHash ==
        acceptedHash);
  CHECK(product->partDocument(DefinitionId{2}) == documentAddress);
  CHECK(product->partDocument(DefinitionId{2})->box().widthMm == 77.0);
  CHECK(successor.impact == ProductDocumentSuccessorImpact::TreeOnly);

  const auto encoded = encodeProductEnvelopeV7Root(
      *product, kProject,
      {QStringLiteral("assembly"),
       QStringLiteral("2026-10-27T00:00:00Z")});
  CHECK(encoded.succeeded());
  const auto scanned =
      scanProductEnvelopeV7(QByteArrayView(encoded.payload->bytes));
  CHECK(scanned.hasCandidate());
  auto roundTrip = materializeProductEnvelopeV7(*scanned.candidate);
  CHECK(roundTrip.succeeded());
  CHECK(*roundTrip.product->partLinkedBinding(DefinitionId{2}) ==
        *product->partLinkedBinding(DefinitionId{2}));
  CHECK(roundTrip.product->partDocument(DefinitionId{2})->box().widthMm ==
        77.0);

  const auto readsAfterRepair = fileSystem.reads;
  fileSystem.files.clear();
  const auto repairBindingBeforeFailedUndo =
      *product->partLinkedBinding(DefinitionId{2});
  const auto repairStateBeforeFailedUndo =
      product->definitionStateId(DefinitionId{2});
  const auto repairCursorBeforeFailedUndo = history.cursor();
  StatusSuccessor rejectedUndo(ProductDocumentSuccessorStatus::Rejected);
  CHECK(history.undo(*product, rejectedUndo).status ==
        AssemblyCommandHistoryStatus::SuccessorRejected);
  CHECK(rejectedUndo.calls == 1);
  CHECK(fileSystem.reads == readsAfterRepair);
  CHECK(history.cursor() == repairCursorBeforeFailedUndo);
  CHECK(product->definitionStateId(DefinitionId{2}) ==
        repairStateBeforeFailedUndo);
  CHECK(*product->partLinkedBinding(DefinitionId{2}) ==
        repairBindingBeforeFailedUndo);
  CHECK(history.undo(*product, successor).status ==
        AssemblyCommandHistoryStatus::Committed);
  const auto repairBindingBeforeFailedRedo =
      *product->partLinkedBinding(DefinitionId{2});
  const auto repairStateBeforeFailedRedo =
      product->definitionStateId(DefinitionId{2});
  const auto repairCursorBeforeFailedRedo = history.cursor();
  StatusSuccessor resourceRedo(
      ProductDocumentSuccessorStatus::ResourceFailure);
  CHECK(history.redo(*product, resourceRedo).status ==
        AssemblyCommandHistoryStatus::ResourceLimit);
  CHECK(resourceRedo.calls == 1);
  CHECK(fileSystem.reads == readsAfterRepair);
  CHECK(history.cursor() == repairCursorBeforeFailedRedo);
  CHECK(product->definitionStateId(DefinitionId{2}) ==
        repairStateBeforeFailedRedo);
  CHECK(*product->partLinkedBinding(DefinitionId{2}) ==
        repairBindingBeforeFailedRedo);
  CHECK(history.redo(*product, successor).status ==
        AssemblyCommandHistoryStatus::Committed);
  CHECK(fileSystem.reads == readsAfterRepair);

  const auto stateBeforeFailure = product->definitionStateId(DefinitionId{2});
  const auto bindingBeforeFailure =
      *product->partLinkedBinding(DefinitionId{2});
  const auto sizeBeforeFailure = history.size();
  const auto cursorBeforeFailure = history.cursor();
  const auto missing = LinkedPartLifecycleService::repair(
      *product, DefinitionId{2}, QStringLiteral("missing/source.solidar"),
      policy(), fileSystem, history, successor);
  CHECK(missing.status == LinkedPartLifecycleStatus::ResolverRejected);
  CHECK(missing.resolverStatus == LinkedPartSourceResolveStatus::Missing);
  CHECK(product->definitionStateId(DefinitionId{2}) == stateBeforeFailure);
  CHECK(*product->partLinkedBinding(DefinitionId{2}) == bindingBeforeFailure);
  CHECK(history.size() == sizeBeforeFailure &&
        history.cursor() == cursorBeforeFailure);

  fileSystem.files.emplace(
      QStringLiteral("C:/Project/wrong/source.solidar"),
      sourceBytes(99.0,
                  QStringLiteral("223e4567-e89b-42d3-a456-426614174001")));
  const auto wrongIdentity = LinkedPartLifecycleService::repair(
      *product, DefinitionId{2}, QStringLiteral("wrong/source.solidar"),
      policy(), fileSystem, history, successor);
  CHECK(wrongIdentity.status == LinkedPartLifecycleStatus::ResolverRejected);
  CHECK(wrongIdentity.resolverStatus ==
        LinkedPartSourceResolveStatus::IdentityMismatch);
  CHECK(*product->partLinkedBinding(DefinitionId{2}) == bindingBeforeFailure);

  // A linked definition reached through two nested prefixes reloads once for
  // the definition. Cold Undo/Redo restores the full selected leaf path and
  // never reopens the source file.
  auto nested = nestedLinkedAssembly(accepted);
  ExactReadFileSystem nestedFileSystem;
  nestedFileSystem.files.emplace(
      QStringLiteral("C:/Project/parts/source.solidar"), revision);
  AssemblyCommandHistory nestedHistory;
  AcceptSuccessor nestedSuccessor;
  const AssemblyCommandSemanticState nestedSelection{path({3, 7}),
                                                      DefinitionId{2}};
  const auto nestedReload = LinkedPartLifecycleService::reload(
      *nested, DefinitionId{2}, policy(), nestedFileSystem, nestedHistory,
      nestedSuccessor, nestedSelection, nestedSelection);
  CHECK(nestedReload.status == LinkedPartLifecycleStatus::Committed);
  CHECK(nested->partDocument(DefinitionId{2})->box().widthMm == 77.0);
  CHECK(nested->resolveOccurrence(path({3, 7})).terminalPart());
  CHECK(nested->resolveOccurrence(path({4, 7})).terminalPart());
  const auto nestedReads = nestedFileSystem.reads;
  nestedFileSystem.files.clear();
  const auto nestedUndo = nestedHistory.undo(*nested, nestedSuccessor);
  CHECK(nestedUndo.status == AssemblyCommandHistoryStatus::Committed);
  CHECK(nestedUndo.restoredState == nestedSelection);
  CHECK(nested->partDocument(DefinitionId{2})->box().widthMm != 77.0);
  CHECK(nestedFileSystem.reads == nestedReads);
  const auto nestedRedo = nestedHistory.redo(*nested, nestedSuccessor);
  CHECK(nestedRedo.status == AssemblyCommandHistoryStatus::Committed);
  CHECK(nestedRedo.restoredState == nestedSelection);
  CHECK(nested->partDocument(DefinitionId{2})->box().widthMm == 77.0);
  CHECK(nestedFileSystem.reads == nestedReads);

  // Repair is path-only even when the same external key now resolves to a
  // newer hash. It preserves the accepted cache/hash, restores a full nested
  // leaf selection, and its cold Undo/Redo perform no filesystem reads.
  nestedFileSystem.files.emplace(
      QStringLiteral("C:/Project/relocated/nested-source.solidar"), newer);
  const auto nestedAcceptedHash =
      nested->partLinkedBinding(DefinitionId{2})->acceptedHash;
  const Document* nestedDocumentAddress =
      nested->partDocument(DefinitionId{2});
  const AssemblyCommandSemanticState nestedRepairSelection{
      path({4, 7}), {}};
  bool nestedRepairOutdated = false;
  const auto nestedRepair = LinkedPartLifecycleService::repair(
      *nested, DefinitionId{2},
      QStringLiteral("relocated/nested-source.solidar"), policy(),
      nestedFileSystem, nestedHistory, nestedSuccessor,
      nestedRepairSelection, nestedRepairSelection,
      &nestedRepairOutdated);
  CHECK(nestedRepair.status == LinkedPartLifecycleStatus::Committed);
  CHECK(nestedRepair.outdated && nestedRepairOutdated);
  CHECK(nested->partLinkedBinding(DefinitionId{2})->relativePathHintUtf8 ==
        "relocated/nested-source.solidar");
  CHECK(nested->partLinkedBinding(DefinitionId{2})->acceptedHash ==
        nestedAcceptedHash);
  CHECK(nested->partDocument(DefinitionId{2}) == nestedDocumentAddress);
  CHECK(nested->partDocument(DefinitionId{2})->box().widthMm == 77.0);
  const auto nestedReadsAfterRepair = nestedFileSystem.reads;
  nestedFileSystem.files.clear();

  const auto nestedRepairBindingBeforeFailure =
      *nested->partLinkedBinding(DefinitionId{2});
  const auto nestedRepairStateBeforeFailure =
      nested->definitionStateId(DefinitionId{2});
  const auto nestedRepairCursorBeforeFailure = nestedHistory.cursor();
  StatusSuccessor nestedRejectedUndo(
      ProductDocumentSuccessorStatus::Rejected);
  CHECK(nestedHistory.undo(*nested, nestedRejectedUndo).status ==
        AssemblyCommandHistoryStatus::SuccessorRejected);
  CHECK(nestedRejectedUndo.calls == 1);
  CHECK(nestedHistory.cursor() == nestedRepairCursorBeforeFailure);
  CHECK(nested->definitionStateId(DefinitionId{2}) ==
        nestedRepairStateBeforeFailure);
  CHECK(*nested->partLinkedBinding(DefinitionId{2}) ==
        nestedRepairBindingBeforeFailure);
  CHECK(nestedFileSystem.reads == nestedReadsAfterRepair);

  const auto nestedRepairUndo = nestedHistory.undo(*nested, nestedSuccessor);
  CHECK(nestedRepairUndo.status == AssemblyCommandHistoryStatus::Committed);
  CHECK(nestedRepairUndo.restoredState == nestedRepairSelection);
  CHECK(nested->partLinkedBinding(DefinitionId{2})->relativePathHintUtf8 ==
        "parts/source.solidar");
  const auto nestedRepairBindingBeforeRedoFailure =
      *nested->partLinkedBinding(DefinitionId{2});
  const auto nestedRepairRedoCursor = nestedHistory.cursor();
  StatusSuccessor nestedResourceRedo(
      ProductDocumentSuccessorStatus::ResourceFailure);
  CHECK(nestedHistory.redo(*nested, nestedResourceRedo).status ==
        AssemblyCommandHistoryStatus::ResourceLimit);
  CHECK(nestedResourceRedo.calls == 1);
  CHECK(nestedHistory.cursor() == nestedRepairRedoCursor);
  CHECK(*nested->partLinkedBinding(DefinitionId{2}) ==
        nestedRepairBindingBeforeRedoFailure);
  CHECK(nestedFileSystem.reads == nestedReadsAfterRepair);
  const auto nestedRepairRedo = nestedHistory.redo(*nested, nestedSuccessor);
  CHECK(nestedRepairRedo.status == AssemblyCommandHistoryStatus::Committed);
  CHECK(nestedRepairRedo.restoredState == nestedRepairSelection);
  CHECK(nested->partLinkedBinding(DefinitionId{2})->relativePathHintUtf8 ==
        "relocated/nested-source.solidar");
  CHECK(nested->partLinkedBinding(DefinitionId{2})->acceptedHash ==
        nestedAcceptedHash);
  CHECK(nestedFileSystem.reads == nestedReadsAfterRepair);

  return EXIT_SUCCESS;
}
