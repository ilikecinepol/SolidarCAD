#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Pnt.hxx>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>

#include <map>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "model/IdGeneration.h"
#include "model/ImportedShapeFeature.h"
#include "model/JoinBodiesFeature.h"
#include "project/LinkedPartSourceResolver.h"
#include "project/ProductEnvelopeV5Encoder.h"
#include "project/internal/LinkedPartSourceFileSystem.h"

namespace {

using namespace solidar;
using namespace solidar::project;

constexpr ProjectId kProject{
    {0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00}};
constexpr ProjectId kOtherProject{
    {0x22, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x01}};
constexpr DefinitionId kDefinition{17};

QJsonObject embeddedPartV4(const QString& name = QStringLiteral("linked")) {
  return QJsonObject{
      {"format", "solidar-project"},
      {"version", 4},
      {"capabilities", QJsonArray{"persistent-topology-v2"}},
      {"constraintTypeEncoding", "stable-name-v1"},
      {"name", name},
      {"createdAt", "2026-10-26T00:00:00Z"},
      {"document", QJsonObject{{"widthMm", 60},
                                 {"heightMm", 40},
                                 {"extrusionMm", 25}}},
      {"sketches", QJsonArray{}},
      {"extrusion", QJsonObject{{"enabled", false}}},
      {"model", QJsonObject{{"sketches", QJsonArray{}},
                              {"bodies", QJsonArray{}},
                              {"referenceImages", QJsonArray{}}}}};
}

QByteArray v5Bytes(QString name = QStringLiteral("linked")) {
  const QJsonObject payload{{"payloadKind", "embedded-part-v4"},
                            {"data", embeddedPartV4(name)}};
  const QJsonObject definition{{"definitionId", 17},
                               {"definitionKind", "part"},
                               {"payload", payload}};
  const QJsonObject root{
      {"format", "solidar-project"},
      {"version", 5},
      {"capabilities", QJsonArray{"product-envelope-v1"}},
      {"projectId", "123e4567-e89b-42d3-a456-426614174000"},
      {"rootDefinitionId", 17},
      {"metadata", QJsonObject{{"name", std::move(name)},
                                {"createdAt", "2026-10-26T00:00:00Z"}}},
      {"definitions", QJsonArray{definition}}};
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray versionBytes(int version) {
  return QJsonDocument(QJsonObject{{"format", "solidar-project"},
                                   {"version", version}})
      .toJson(QJsonDocument::Compact);
}

QByteArray floatingVersionBytes(double version) {
  return QJsonDocument(QJsonObject{{"format", "solidar-project"},
                                   {"version", version}})
      .toJson(QJsonDocument::Compact);
}

QByteArray futureV5Bytes(const QByteArray& valid) {
  auto root = QJsonDocument::fromJson(valid).object();
  auto capabilities = root.value(QStringLiteral("capabilities")).toArray();
  capabilities.prepend(QStringLiteral("future-linked-source-v9"));
  root[QStringLiteral("capabilities")] = capabilities;
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray lateMaterializationFailureBytes() {
  solidar::detail::ScopedExplicitIdReservationPause pause;
  constexpr BodyId firstBody{910001};
  constexpr BodyId secondBody{910002};
  constexpr FeatureId firstImport{910003};
  constexpr FeatureId secondImport{910004};
  constexpr FeatureId join{910005};
  Document document;
  auto& first = document.addBody(firstBody, "first");
  first.addFeature(std::make_unique<ImportedShapeFeature>(
      firstImport,
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape()),
      "first archive"));
  auto& second = document.addBody(secondBody, "second");
  second.addFeature(std::make_unique<ImportedShapeFeature>(
      secondImport,
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(gp_Pnt{100.0, 0.0, 0.0}, 20.0, 10.0, 5.0)
              .Shape()),
      "second archive"));
  document.findBody(firstBody)->addFeature(std::make_unique<JoinBodiesFeature>(
      join, firstBody, firstImport, secondBody, secondImport,
      "disconnected join"));
  DefinitionRegistry registry;
  CHECK(registry.insert(PartDefinition(
            kDefinition, std::make_unique<Document>(std::move(document)))) ==
        DefinitionInsertStatus::Inserted);
  auto product = ProductDocument::create(std::move(registry), kDefinition);
  CHECK(product);
  const auto encoded = encodeProductEnvelopeV5Root(
      *product.product, kOtherProject,
      ProductEnvelopeV5Metadata{QStringLiteral("late failure"),
                                QStringLiteral("2026-10-26T00:00:00Z")});
  CHECK(encoded.succeeded());
  return encoded.payload->bytes;
}

QByteArray nonzeroRebuildBytes() {
  solidar::detail::ScopedExplicitIdReservationPause pause;
  constexpr BodyId body{920001};
  constexpr FeatureId imported{920002};
  Document document;
  auto& target = document.addBody(body, "root");
  target.addFeature(std::make_unique<ImportedShapeFeature>(
      imported,
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(2.0, 3.0, 4.0).Shape()),
      "archive"));
  DefinitionRegistry registry;
  CHECK(registry.insert(PartDefinition(
            kDefinition, std::make_unique<Document>(std::move(document)))) ==
        DefinitionInsertStatus::Inserted);
  auto product = ProductDocument::create(std::move(registry), kDefinition);
  CHECK(product);
  const auto encoded = encodeProductEnvelopeV5Root(
      *product.product, kProject,
      ProductEnvelopeV5Metadata{QStringLiteral("rebuild"),
                                QStringLiteral("2026-10-26T00:00:00Z")});
  CHECK(encoded.succeeded());
  return encoded.payload->bytes;
}

struct FakeEntry {
  solidar::project::detail::LinkedSourceOpenStatus status{
      solidar::project::detail::LinkedSourceOpenStatus::Opened};
  QString canonicalPath;
  QString fileIdentity;
  QString objectVersion{QStringLiteral("v1")};
  QByteArray bytes;
  std::uint64_t workUnits{1};
  std::uint32_t aliasDepth{};
  QString error;
  solidar::project::detail::LinkedSourceOpenStatus readStatus{
      solidar::project::detail::LinkedSourceOpenStatus::Opened};
};

class FakeReadHandle final
    : public solidar::project::detail::LinkedSourceReadHandle {
 public:
  FakeReadHandle(QByteArray bytes,
                 solidar::project::detail::LinkedSourceOpenStatus status,
                 std::uint64_t* reads)
      : bytes_(std::move(bytes)), status_(status), reads_(reads) {}

  solidar::project::detail::LinkedSourceReadResult readBounded(
      std::uint64_t maximumBytes,
      std::uint64_t maximumWorkUnits) override {
    if (reads_) ++*reads_;
    if (maximumWorkUnits == 0)
      return {solidar::project::detail::LinkedSourceOpenStatus::BudgetExceeded,
              {}, 0, QStringLiteral("work")};
    if (status_ != solidar::project::detail::LinkedSourceOpenStatus::Opened)
      return {status_, {}, 1, QStringLiteral("read failure")};
    if (static_cast<std::uint64_t>(bytes_.size()) > maximumBytes)
      return {solidar::project::detail::LinkedSourceOpenStatus::BudgetExceeded,
              {}, 1, QStringLiteral("bytes")};
    return {solidar::project::detail::LinkedSourceOpenStatus::Opened, bytes_, 1,
            {}};
  }

 private:
  QByteArray bytes_;
  solidar::project::detail::LinkedSourceOpenStatus status_;
  std::uint64_t* reads_{};
};

class FakeFileSystem final
    : public solidar::project::detail::LinkedPartSourceFileSystem {
 public:
  std::map<QString, FakeEntry> files;
  std::vector<QString> attempts;
  std::uint64_t reads{};
  std::uint64_t rootCalls{};
  std::uint64_t rootWork{1};
  std::uint32_t rootAliasDepth{};
  solidar::project::detail::LinkedSourceOpenStatus rootStatus{
      solidar::project::detail::LinkedSourceOpenStatus::Opened};

  solidar::project::detail::LinkedSourceCanonicalRoot canonicalizeRoot(
      const QString& absolutePath, std::uint32_t maximumAliasDepth,
      std::uint64_t maximumWorkUnits) override {
    ++rootCalls;
    solidar::project::detail::LinkedSourceCanonicalRoot result;
    result.status = rootStatus;
    result.canonicalPath = absolutePath;
    result.workUnits = rootWork;
    result.aliasDepth = rootAliasDepth;
    if (rootWork > maximumWorkUnits || rootAliasDepth > maximumAliasDepth) {
      result.status =
          solidar::project::detail::LinkedSourceOpenStatus::BudgetExceeded;
      result.error = QStringLiteral("root budget");
    } else if (result.status !=
               solidar::project::detail::LinkedSourceOpenStatus::Opened) {
      result.error = QStringLiteral("root failure");
    }
    return result;
  }

  solidar::project::detail::LinkedSourceOpenedFile openExact(
      const QString& absolutePath, std::uint32_t maximumAliasDepth,
      std::uint64_t maximumWorkUnits) override {
    attempts.push_back(absolutePath);
    if (maximumWorkUnits == 0) {
      solidar::project::detail::LinkedSourceOpenedFile result;
      result.status =
          solidar::project::detail::LinkedSourceOpenStatus::BudgetExceeded;
      result.error = QStringLiteral("work");
      return result;
    }
    const auto found = files.find(absolutePath);
    if (found == files.end()) {
      solidar::project::detail::LinkedSourceOpenedFile result;
      result.status = solidar::project::detail::LinkedSourceOpenStatus::Missing;
      result.workUnits = 1;
      result.error = QStringLiteral("missing");
      return result;
    }
    const auto& entry = found->second;
    solidar::project::detail::LinkedSourceOpenedFile result;
    result.status = entry.status;
    result.canonicalPath = entry.canonicalPath;
    result.fileIdentity = entry.fileIdentity;
    result.objectVersion = entry.objectVersion;
    result.sizeBytes = static_cast<std::uint64_t>(entry.bytes.size());
    result.workUnits = entry.workUnits;
    result.aliasDepth = entry.aliasDepth;
    result.error = entry.error;
    if (entry.aliasDepth > maximumAliasDepth) {
      result.status =
          solidar::project::detail::LinkedSourceOpenStatus::BudgetExceeded;
      result.error = QStringLiteral("depth");
    }
    if (result.status == solidar::project::detail::LinkedSourceOpenStatus::Opened)
      result.handle = std::make_unique<FakeReadHandle>(entry.bytes,
                                                       entry.readStatus, &reads);
    return result;
  }
};

#if defined(Q_OS_WIN)
class FakeWindowsWalkerBackend final
    : public solidar::project::detail::WindowsWalkerBackend {
 public:
  struct Object {
    solidar::project::detail::WindowsWalkerObjectInfo info;
    solidar::project::detail::WindowsWalkerReparseTarget target;
    QString canonicalPath;
    bool caseSensitive{};
  };

  std::map<QString, Object> objects;
  std::map<HandleToken, QString> handles;
  std::vector<HandleToken> openedTokens;
  std::map<HandleToken, std::uint64_t> closeCounts;
  HandleToken nextHandle{1};
  std::uint64_t openCalls{};
  std::uint64_t informationCalls{};
  std::uint64_t caseCalls{};
  std::uint64_t reparseCalls{};
  std::uint64_t canonicalCalls{};
  std::uint64_t networkOpenAttempts{};
  bool throwOnCanonical{};

  FakeWindowsWalkerBackend() {
    addDirectory(QStringLiteral("C:/"), QStringLiteral("drive"));
  }

  solidar::project::detail::LinkedSourceOpenStatus queryRemoteDrive(
      const QString&, bool* remote) override {
    if (!remote)
      return solidar::project::detail::LinkedSourceOpenStatus::ResourceFailure;
    *remote = false;
    return solidar::project::detail::LinkedSourceOpenStatus::Opened;
  }

  void addDirectory(QString path, QString identity, bool caseSensitive = false) {
    Object object;
    object.info.identity = std::move(identity);
    object.info.directory = true;
    object.info.diskFile = false;
    object.canonicalPath = path;
    object.caseSensitive = caseSensitive;
    objects.emplace(std::move(path), std::move(object));
  }

  void addFile(QString path, QString identity) {
    Object object;
    object.info.identity = std::move(identity);
    object.info.version = QStringLiteral("v1");
    object.info.diskFile = true;
    object.info.sizeBytes = 1;
    object.canonicalPath = path;
    objects.emplace(std::move(path), std::move(object));
  }

  void addLink(QString path, QString identity, QString target,
               bool relative = false) {
    Object object;
    object.info.identity = std::move(identity);
    object.info.reparsePoint = true;
    object.info.diskFile = true;
    object.target = {std::move(target), relative};
    object.canonicalPath = path;
    objects.emplace(std::move(path), std::move(object));
  }

  solidar::project::detail::LinkedSourceOpenStatus openDriveRoot(
      const QString& driveRoot, HandleToken* handle) override {
    ++openCalls;
    return openObject(driveRoot, handle);
  }

  solidar::project::detail::LinkedSourceOpenStatus openRelative(
      HandleToken parent, const QString& component, bool,
      HandleToken* handle) override {
    ++openCalls;
    const auto found = handles.find(parent);
    if (found == handles.end())
      return solidar::project::detail::LinkedSourceOpenStatus::ResourceFailure;
    const QString path = QDir::cleanPath(found->second + QLatin1Char('/') +
                                         component);
    if (path.startsWith(QStringLiteral("//"))) ++networkOpenAttempts;
    return openObject(path, handle);
  }

  solidar::project::detail::LinkedSourceOpenStatus queryInformation(
      HandleToken handle,
      solidar::project::detail::WindowsWalkerObjectInfo* info) override {
    ++informationCalls;
    const auto* object = objectFor(handle);
    if (!object || !info)
      return solidar::project::detail::LinkedSourceOpenStatus::ResourceFailure;
    *info = object->info;
    return solidar::project::detail::LinkedSourceOpenStatus::Opened;
  }

  solidar::project::detail::LinkedSourceOpenStatus queryCaseSensitive(
      HandleToken handle, bool* caseSensitive) override {
    ++caseCalls;
    const auto* object = objectFor(handle);
    if (!object || !caseSensitive)
      return solidar::project::detail::LinkedSourceOpenStatus::ResourceFailure;
    *caseSensitive = object->caseSensitive;
    return solidar::project::detail::LinkedSourceOpenStatus::Opened;
  }

  solidar::project::detail::LinkedSourceOpenStatus queryReparseTarget(
      HandleToken handle,
      solidar::project::detail::WindowsWalkerReparseTarget* target) override {
    ++reparseCalls;
    const auto* object = objectFor(handle);
    if (!object || !target || !object->info.reparsePoint)
      return solidar::project::detail::LinkedSourceOpenStatus::ResourceFailure;
    *target = object->target;
    return solidar::project::detail::LinkedSourceOpenStatus::Opened;
  }

  solidar::project::detail::LinkedSourceOpenStatus canonicalPath(
      HandleToken handle, QString* path) override {
    ++canonicalCalls;
    if (throwOnCanonical) throw std::bad_alloc{};
    const auto* object = objectFor(handle);
    if (!object || !path)
      return solidar::project::detail::LinkedSourceOpenStatus::ResourceFailure;
    *path = object->canonicalPath;
    return solidar::project::detail::LinkedSourceOpenStatus::Opened;
  }

  void close(HandleToken handle) noexcept override {
    ++closeCounts[handle];
    handles.erase(handle);
  }

 private:
  solidar::project::detail::LinkedSourceOpenStatus openObject(
      const QString& path, HandleToken* handle) {
    const auto found = objects.find(path);
    if (found == objects.end() || !handle)
      return solidar::project::detail::LinkedSourceOpenStatus::Missing;
    const HandleToken token = nextHandle++;
    handles.emplace(token, path);
    openedTokens.push_back(token);
    *handle = token;
    return solidar::project::detail::LinkedSourceOpenStatus::Opened;
  }

  const Object* objectFor(HandleToken handle) const {
    const auto path = handles.find(handle);
    if (path == handles.end()) return nullptr;
    const auto object = objects.find(path->second);
    return object == objects.end() ? nullptr : &object->second;
  }
};

class ThrowingWindowsWalkerFileSystem final
    : public solidar::project::detail::LinkedPartSourceFileSystem {
 public:
  explicit ThrowingWindowsWalkerFileSystem(bool throwOnCanonical) {
    backend.addDirectory(QStringLiteral("C:/Root"), QStringLiteral("root"));
    backend.addFile(QStringLiteral("C:/Root/file"), QStringLiteral("file"));
    backend.throwOnCanonical = throwOnCanonical;
  }

  solidar::project::detail::LinkedSourceCanonicalRoot canonicalizeRoot(
      const QString&, std::uint32_t, std::uint64_t) override {
    solidar::project::detail::LinkedSourceCanonicalRoot result;
    result.status = solidar::project::detail::LinkedSourceOpenStatus::Opened;
    result.canonicalPath = QStringLiteral("C:/Root");
    return result;
  }

  solidar::project::detail::LinkedSourceOpenedFile openExact(
      const QString& absolutePath, std::uint32_t maximumAliasDepth,
      std::uint64_t maximumWorkUnits) override {
    auto walked = solidar::project::detail::walkWindowsLinkedPath(
        absolutePath, maximumAliasDepth, maximumWorkUnits, false, backend);
    if (walked.status !=
        solidar::project::detail::LinkedSourceOpenStatus::Opened) {
      solidar::project::detail::LinkedSourceOpenedFile failure;
      failure.status = walked.status;
      failure.error = walked.error;
      return failure;
    }
    // Simulates any allocating staging operation after canonicalization but
    // before ownership is transferred into the final read handle.
    throw std::bad_alloc{};
  }

  FakeWindowsWalkerBackend backend;
};
#endif

FakeEntry opened(
    QString canonical, QByteArray bytes, QString identity = QStringLiteral("id"),
    std::uint32_t depth = 0, std::uint64_t work = 1) {
  FakeEntry result;
  result.canonicalPath = std::move(canonical);
  result.fileIdentity = std::move(identity);
  result.bytes = std::move(bytes);
  result.workUnits = work;
  result.aliasDepth = depth;
  return result;
}

LinkedPartSourcePolicy windowsPolicy() {
  LinkedPartSourcePolicy policy;
  policy.projectRoot = QStringLiteral("C:/Root");
  policy.libraryRoots = {QStringLiteral("C:/Library")};
  policy.pathSemantics = LinkedPathSemantics::Windows;
  return policy;
}

LinkedPartSourceDescriptor descriptor(QString path, const QByteArray& bytes,
                                      ExternalSourceKey key =
                                          ExternalSourceKey{kProject,
                                                            kDefinition}) {
  return {key, std::move(path), linkedSourceSha256(QByteArrayView(bytes))};
}

void checkFailure(const LinkedPartSourceResolveResult& result,
                  LinkedPartSourceResolveStatus status) {
  if (result.status != status) {
    std::cerr << "expected status " << static_cast<int>(status)
              << ", actual " << static_cast<int>(result.status) << ": "
              << result.error.toStdString() << '\n';
  }
  CHECK(result.status == status);
  CHECK(!result.succeeded());
  CHECK(result.sources.empty());
  CHECK(!result.error.isEmpty());
}

}  // namespace

int main() {
  const QByteArray valid = v5Bytes();

  for (const auto semantics : {LinkedPathSemantics::Windows,
                               LinkedPathSemantics::Posix}) {
    FakeFileSystem fs;
    auto policy = windowsPolicy();
    policy.pathSemantics = semantics;
    policy.libraryRoots.clear();
    policy.projectRoot += QChar::Null;
    const auto result = resolveLinkedPartSources(
        {descriptor(QStringLiteral("a"), valid)}, policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::InvalidDescriptor);
    CHECK(fs.rootCalls == 0);
    CHECK(fs.attempts.empty());

    FakeFileSystem libraryFs;
    auto libraryPolicy = windowsPolicy();
    libraryPolicy.pathSemantics = semantics;
    libraryPolicy.libraryRoots = {
        QStringLiteral("C:/Library") + QChar::Null};
    const auto libraryResult = resolveLinkedPartSources(
        {descriptor(QStringLiteral("a"), valid)}, libraryPolicy, libraryFs);
    checkFailure(libraryResult,
                 LinkedPartSourceResolveStatus::InvalidDescriptor);
    CHECK(libraryFs.rootCalls == 0);
    CHECK(libraryFs.attempts.empty());
  }

#if defined(Q_OS_WIN)
  {
    FakeWindowsWalkerBackend exact;
    exact.addDirectory(QStringLiteral("C:/Root"), QStringLiteral("root"));
    exact.addFile(QStringLiteral("C:/Root/file"), QStringLiteral("file"));
    auto walked = solidar::project::detail::walkWindowsLinkedPath(
        QStringLiteral("C:/Root/file"), 0, 10, false, exact);
    CHECK(walked.status ==
          solidar::project::detail::LinkedSourceOpenStatus::Opened);
    CHECK(walked.workUnits == 10);
    CHECK(exact.informationCalls == 2);
    CHECK(exact.caseCalls == 2);
    CHECK(exact.canonicalCalls == 1);

    FakeWindowsWalkerBackend overflow;
    overflow.addDirectory(QStringLiteral("C:/Root"), QStringLiteral("root"));
    overflow.addFile(QStringLiteral("C:/Root/file"), QStringLiteral("file"));
    walked = solidar::project::detail::walkWindowsLinkedPath(
        QStringLiteral("C:/Root/file"), 0, 9, false, overflow);
    CHECK(walked.status ==
          solidar::project::detail::LinkedSourceOpenStatus::BudgetExceeded);
    CHECK(walked.workUnits == 8);
    CHECK(overflow.canonicalCalls == 0);
  }
  {
    FakeWindowsWalkerBackend backend;
    backend.addDirectory(QStringLiteral("C:/Root"), QStringLiteral("root"));
    backend.addLink(QStringLiteral("C:/Root/link"), QStringLiteral("link"),
                    QStringLiteral("\\??\\UNC\\server\\share\\file"));
    const auto walked = solidar::project::detail::walkWindowsLinkedPath(
        QStringLiteral("C:/Root/link"), 2, 64, false, backend);
    CHECK(walked.status ==
          solidar::project::detail::LinkedSourceOpenStatus::NetworkDisabled);
    CHECK(backend.networkOpenAttempts == 0);
    CHECK(backend.reparseCalls == 1);
    CHECK(backend.canonicalCalls == 0);
  }
  {
    FakeWindowsWalkerBackend backend;
    backend.addDirectory(QStringLiteral("C:/Root"), QStringLiteral("root"));
    backend.addLink(QStringLiteral("C:/Root/loop"), QStringLiteral("same-link"),
                    QStringLiteral("C:/Root/loop"));
    const auto walked = solidar::project::detail::walkWindowsLinkedPath(
        QStringLiteral("C:/Root/loop"), 4, 128, false, backend);
    CHECK(walked.status ==
          solidar::project::detail::LinkedSourceOpenStatus::TraversalLoop);
    CHECK(walked.aliasDepth == 2);
  }
  {
    auto makeChain = [] {
      FakeWindowsWalkerBackend backend;
      backend.addDirectory(QStringLiteral("C:/Root"), QStringLiteral("root"));
      backend.addLink(QStringLiteral("C:/Root/a"), QStringLiteral("a"),
                      QStringLiteral("C:/Root/b"));
      backend.addLink(QStringLiteral("C:/Root/b"), QStringLiteral("b"),
                      QStringLiteral("C:/Root/file"));
      backend.addFile(QStringLiteral("C:/Root/file"), QStringLiteral("file"));
      return backend;
    };
    auto exact = makeChain();
    auto walked = solidar::project::detail::walkWindowsLinkedPath(
        QStringLiteral("C:/Root/a"), 2, 128, false, exact);
    CHECK(walked.status ==
          solidar::project::detail::LinkedSourceOpenStatus::Opened);
    CHECK(walked.aliasDepth == 2);
    auto overflow = makeChain();
    walked = solidar::project::detail::walkWindowsLinkedPath(
        QStringLiteral("C:/Root/a"), 1, 128, false, overflow);
    CHECK(walked.status ==
          solidar::project::detail::LinkedSourceOpenStatus::BudgetExceeded);
    CHECK(walked.aliasDepth == 2);
  }
  {
    FakeWindowsWalkerBackend backend;
    backend.addDirectory(QStringLiteral("C:/Root"), QStringLiteral("root"),
                         true);
    backend.addFile(QStringLiteral("C:/Root/file"), QStringLiteral("file"));
    const auto walked = solidar::project::detail::walkWindowsLinkedPath(
        QStringLiteral("C:/Root/file"), 0, 64, false, backend);
    CHECK(walked.status ==
          solidar::project::detail::LinkedSourceOpenStatus::ResourceFailure);
    CHECK(backend.openCalls == 2);
  }
  {
    for (const bool throwOnCanonical : {true, false}) {
      ThrowingWindowsWalkerFileSystem fs(throwOnCanonical);
      auto policy = windowsPolicy();
      policy.libraryRoots.clear();
      const auto result = resolveLinkedPartSources(
          {descriptor(QStringLiteral("file"), valid)}, policy, fs);
      checkFailure(result, LinkedPartSourceResolveStatus::ResourceFailure);
      CHECK(fs.backend.handles.empty());
      CHECK(!fs.backend.openedTokens.empty());
      for (const auto token : fs.backend.openedTokens)
        CHECK(fs.backend.closeCounts[token] == 1);
    }
  }
#endif

  // Native entry performs the complete pure descriptor/policy preflight
  // before invoking even the injectable root walker.
  for (const auto& [descriptors, mutatePolicy, expected] :
       std::vector<std::tuple<
           std::vector<LinkedPartSourceDescriptor>,
           std::function<void(LinkedPartSourcePolicy&)>,
           LinkedPartSourceResolveStatus>>{
           {{descriptor("../escape", valid)}, [](auto&) {},
            LinkedPartSourceResolveStatus::PathEscape},
           {{LinkedPartSourceDescriptor{
                 ExternalSourceKey{}, QStringLiteral("a"),
                 linkedSourceSha256(QByteArrayView(valid))}},
            [](auto&) {}, LinkedPartSourceResolveStatus::InvalidDescriptor},
           {{descriptor("a", valid)},
            [](auto& policy) { policy.budgets.maximumDescriptors = 0; },
            LinkedPartSourceResolveStatus::BudgetExceeded}}) {
    FakeFileSystem fs;
    auto policy = windowsPolicy();
    policy.libraryRoots.clear();
    mutatePolicy(policy);
    const auto result =
        solidar::project::detail::resolveLinkedPartSourcesNativeWithFileSystem(
            descriptors, policy, fs);
    checkFailure(result, expected);
    CHECK(fs.rootCalls == 0);
    CHECK(fs.attempts.empty());
  }

  for (const bool nulInProjectRoot : {true, false}) {
    FakeFileSystem fs;
    auto policy = windowsPolicy();
    if (nulInProjectRoot) {
      policy.projectRoot += QChar::Null;
    } else {
      policy.libraryRoots = {
          QStringLiteral("C:/Library") + QChar::Null};
    }
    const auto result =
        solidar::project::detail::resolveLinkedPartSourcesNativeWithFileSystem(
            {descriptor("a", valid)}, policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::InvalidDescriptor);
    CHECK(fs.rootCalls == 0);
    CHECK(fs.attempts.empty());
  }

  // Deterministic native-walker contract coverage independent of OS link
  // privileges: root traversal failures occur before target inspection.
  for (const auto& [rootStatus, expected] :
       std::vector<std::pair<
           solidar::project::detail::LinkedSourceOpenStatus,
           LinkedPartSourceResolveStatus>>{
           {solidar::project::detail::LinkedSourceOpenStatus::NetworkDisabled,
            LinkedPartSourceResolveStatus::NetworkDisabled},
           {solidar::project::detail::LinkedSourceOpenStatus::TraversalLoop,
            LinkedPartSourceResolveStatus::TraversalLoop},
           {solidar::project::detail::LinkedSourceOpenStatus::ResourceFailure,
            LinkedPartSourceResolveStatus::ResourceFailure}}) {
    FakeFileSystem fs;
    fs.rootStatus = rootStatus;
    auto policy = windowsPolicy();
    policy.libraryRoots.clear();
    const auto result =
        solidar::project::detail::resolveLinkedPartSourcesNativeWithFileSystem(
            {descriptor("a", valid)}, policy, fs);
    checkFailure(result, expected);
    CHECK(fs.rootCalls == 1);
    CHECK(fs.attempts.empty());
  }

  {
    // Alias depth and filesystem work are one aggregate across root traversal,
    // target inspection and held-handle read.
    FakeFileSystem fs;
    fs.rootAliasDepth = 1;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid, "id", 1));
    auto policy = windowsPolicy();
    policy.libraryRoots.clear();
    policy.budgets.maximumAliasDepth = 2;
    policy.budgets.maximumFileSystemWork = 3;
    CHECK(solidar::project::detail::resolveLinkedPartSourcesNativeWithFileSystem(
              {descriptor("a", valid)}, policy, fs)
              .succeeded());

    FakeFileSystem depthOverflow;
    depthOverflow.rootAliasDepth = 1;
    depthOverflow.files.emplace(
        QStringLiteral("C:/Root/a"),
        opened(QStringLiteral("C:/Root/a"), valid, "id", 1));
    policy.budgets.maximumAliasDepth = 1;
    checkFailure(
        solidar::project::detail::resolveLinkedPartSourcesNativeWithFileSystem(
            {descriptor("a", valid)}, policy, depthOverflow),
        LinkedPartSourceResolveStatus::BudgetExceeded);
    CHECK(depthOverflow.reads == 0);

    FakeFileSystem workOverflow;
    workOverflow.rootAliasDepth = 1;
    workOverflow.files.emplace(
        QStringLiteral("C:/Root/a"),
        opened(QStringLiteral("C:/Root/a"), valid, "id", 1));
    policy.budgets.maximumAliasDepth = 2;
    policy.budgets.maximumFileSystemWork = 2;
    checkFailure(
        solidar::project::detail::resolveLinkedPartSourcesNativeWithFileSystem(
            {descriptor("a", valid)}, policy, workOverflow),
        LinkedPartSourceResolveStatus::BudgetExceeded);
    CHECK(workOverflow.reads == 0);
  }

  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/components/a.solidar"),
                     opened(QStringLiteral("c:/ROOT/components/a.solidar"),
                            valid));
    const auto result = resolveLinkedPartSources(
        {descriptor(QStringLiteral("components\\.\\a.solidar"), valid)},
        windowsPolicy(), fs);
    CHECK(result.succeeded());
    CHECK(result.sources.size() == 1);
    CHECK((result.sources[0].key ==
           ExternalSourceKey{kProject, kDefinition}));
    CHECK(result.sources[0].descriptorIndexes == std::vector<std::size_t>{0});
    CHECK(result.sources[0].product != nullptr);
    CHECK(result.sources[0].product->projectId() == kProject);
    CHECK(result.sources[0].product->rootDefinitionId() == kDefinition);
    CHECK(fs.attempts ==
          std::vector<QString>{QStringLiteral("C:/Root/components/a.solidar")});
  }

  // Lexical rejection is pure: no filesystem call occurs.
  for (const QString& escaped :
       {QStringLiteral("../a.solidar"), QStringLiteral("a/../../b"),
        QStringLiteral("C:relative"), QStringLiteral("C:/absolute"),
        QStringLiteral("/absolute"), QStringLiteral("folder/file:stream"),
         QStringLiteral("CON/file"), QStringLiteral("\\\\?\\C:\\device"),
         QStringLiteral("\\\\.\\C:\\device")}) {
    FakeFileSystem fs;
    const auto result = resolveLinkedPartSources(
        {descriptor(escaped, valid)}, windowsPolicy(), fs);
    checkFailure(result, LinkedPartSourceResolveStatus::PathEscape);
    CHECK(fs.attempts.empty());
  }
  {
    FakeFileSystem fs;
    const QString nulPath = QStringLiteral("safe/a") + QChar::Null +
                            QStringLiteral(".solidar");
    const auto result = resolveLinkedPartSources(
        {descriptor(nulPath, valid)}, windowsPolicy(), fs);
    checkFailure(result, LinkedPartSourceResolveStatus::InvalidDescriptor);
    CHECK(fs.attempts.empty());
  }
  {
    FakeFileSystem fs;
    const auto result = resolveLinkedPartSources(
        {descriptor(QStringLiteral("//server/share/a"), valid)},
        windowsPolicy(), fs);
    checkFailure(result, LinkedPartSourceResolveStatus::NetworkDisabled);
    CHECK(fs.attempts.empty());
  }
  {
    FakeFileSystem fs;
    const auto result = resolveLinkedPartSources(
        {descriptor(QStringLiteral("safe/missing.solidar"), valid)},
        windowsPolicy(), fs);
    checkFailure(result, LinkedPartSourceResolveStatus::Missing);
    CHECK(fs.attempts.size() == 2);  // project root, then explicit library root
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Library/a.solidar"),
                     opened(QStringLiteral("C:/Library/a.solidar"), valid));
    const auto result = resolveLinkedPartSources(
        {descriptor(QStringLiteral("a.solidar"), valid)}, windowsPolicy(), fs);
    CHECK(result.succeeded());
    CHECK(fs.attempts.size() == 2);
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a.solidar"),
                     opened(QStringLiteral("C:/Root2/a.solidar"), valid));
    const auto result = resolveLinkedPartSources(
        {descriptor(QStringLiteral("a.solidar"), valid)}, windowsPolicy(), fs);
    checkFailure(result, LinkedPartSourceResolveStatus::OutsideAllowedRoot);
    CHECK(fs.reads == 0);
  }
  {
    auto policy = windowsPolicy();
    policy.pathSemantics = LinkedPathSemantics::Posix;
    policy.projectRoot = QStringLiteral("/Root");
    policy.libraryRoots.clear();
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("/Root/a"),
                     opened(QStringLiteral("/root/a"), valid));
    const auto result = resolveLinkedPartSources(
        {descriptor(QStringLiteral("a"), valid)}, policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::OutsideAllowedRoot);
  }
  {
    auto policy = windowsPolicy();
    policy.pathSemantics = LinkedPathSemantics::Posix;
    policy.projectRoot = QStringLiteral("/root");
    policy.libraryRoots.clear();
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("/root/a:b"),
                     opened(QStringLiteral("/root/a:b"), valid));
    const auto result = resolveLinkedPartSources(
        {descriptor(QStringLiteral("a:b"), valid)}, policy, fs);
    CHECK(result.succeeded());
  }
  {
    // Windows ordinal-ignore-case does not perform Unicode full case folding:
    // German sharp-s is not the two-character sequence "SS".
    auto policy = windowsPolicy();
    policy.projectRoot = QStringLiteral("C:/Root/Straße");
    policy.libraryRoots.clear();
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/Straße/a"),
                     opened(QStringLiteral("C:/ROOT/STRASSE/a"), valid));
    const auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                                 policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::OutsideAllowedRoot);
    CHECK(fs.reads == 0);
  }
  {
    auto policy = windowsPolicy();
    policy.libraryRoots.clear();
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("D:/Root/a"), valid));
    const auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                                 policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::OutsideAllowedRoot);
    CHECK(fs.reads == 0);
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid, "id", 33));
    auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                           windowsPolicy(), fs);
    checkFailure(result, LinkedPartSourceResolveStatus::BudgetExceeded);
  }
  {
    FakeFileSystem fs;
    auto changed = opened(QStringLiteral("C:/Root/a"), valid);
    changed.status =
        solidar::project::detail::LinkedSourceOpenStatus::SourceChanged;
    changed.error = QStringLiteral("changed");
    fs.files.emplace(QStringLiteral("C:/Root/a"), std::move(changed));
    const auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                                 windowsPolicy(), fs);
    checkFailure(result, LinkedPartSourceResolveStatus::SourceChanged);
  }
  {
    FakeFileSystem fs;
    auto changed = opened(QStringLiteral("C:/Root/a"), valid);
    changed.readStatus =
        solidar::project::detail::LinkedSourceOpenStatus::SourceChanged;
    fs.files.emplace(QStringLiteral("C:/Root/a"), std::move(changed));
    const auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                                 windowsPolicy(), fs);
    checkFailure(result, LinkedPartSourceResolveStatus::SourceChanged);
    CHECK(fs.reads == 1);
  }
  for (const auto& [openStatus, expected] :
       std::vector<std::pair<
           solidar::project::detail::LinkedSourceOpenStatus,
           LinkedPartSourceResolveStatus>>{
           {solidar::project::detail::LinkedSourceOpenStatus::SourceNotRegular,
            LinkedPartSourceResolveStatus::SourceNotRegular},
           {solidar::project::detail::LinkedSourceOpenStatus::TraversalLoop,
            LinkedPartSourceResolveStatus::TraversalLoop},
           {solidar::project::detail::LinkedSourceOpenStatus::ResourceFailure,
            LinkedPartSourceResolveStatus::ResourceFailure}}) {
    FakeFileSystem fs;
    FakeEntry entry;
    entry.status = openStatus;
    entry.error = QStringLiteral("open failure");
    fs.files.emplace(QStringLiteral("C:/Root/a"), std::move(entry));
    const auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                                 windowsPolicy(), fs);
    checkFailure(result, expected);
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("//server/share/a"), valid));
    const auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                                 windowsPolicy(), fs);
    checkFailure(result, LinkedPartSourceResolveStatus::NetworkDisabled);
    CHECK(fs.reads == 0);
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid));
    auto expected = descriptor("a", valid);
    expected.key.projectId = kOtherProject;
    const auto result =
        resolveLinkedPartSources({expected}, windowsPolicy(), fs);
    checkFailure(result, LinkedPartSourceResolveStatus::IdentityMismatch);
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid));
    auto expected = descriptor("a", valid);
    expected.key.definitionId = DefinitionId{18};
    const auto result =
        resolveLinkedPartSources({expected}, windowsPolicy(), fs);
    checkFailure(result, LinkedPartSourceResolveStatus::DefinitionMissing);
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid));
    auto expected = descriptor("a", valid);
    expected.expectedHash.bytes[0] ^= 1;
    const auto result =
        resolveLinkedPartSources({expected}, windowsPolicy(), fs);
    checkFailure(result, LinkedPartSourceResolveStatus::Outdated);
    const auto reload = resolveLinkedPartSourcesForReload(
        {expected}, windowsPolicy(), fs);
    CHECK(reload.succeeded());
    CHECK(reload.sources.size() == 1 && reload.sources.front().product);
    CHECK(reload.sources.front().contentHash ==
          linkedSourceSha256(QByteArrayView(valid)));
    const auto repair = probeLinkedPartSourcesForRepair(
        {expected}, windowsPolicy(), fs);
    CHECK(repair.succeeded());
    CHECK(repair.sources.size() == 1 && !repair.sources.front().product);
    CHECK(repair.sources.front().contentHash ==
          linkedSourceSha256(QByteArrayView(valid)));
    CHECK(repair.sourcesDecoded == 1 && repair.filesRead == 1);
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid));
    auto wrongIdentity = descriptor("a", valid);
    wrongIdentity.key.projectId = kOtherProject;
    checkFailure(resolveLinkedPartSourcesForReload(
                     {wrongIdentity}, windowsPolicy(), fs),
                 LinkedPartSourceResolveStatus::IdentityMismatch);
    checkFailure(probeLinkedPartSourcesForRepair(
                     {wrongIdentity}, windowsPolicy(), fs),
                 LinkedPartSourceResolveStatus::IdentityMismatch);
  }
  {
    const QByteArray changed = v5Bytes(QStringLiteral("changed"));
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid, "a"));
    fs.files.emplace(QStringLiteral("C:/Root/b"),
                     opened(QStringLiteral("C:/Root/b"), changed, "b"));
    const auto result = resolveLinkedPartSources(
        {descriptor("a", valid), descriptor("b", changed)}, windowsPolicy(),
        fs);
    checkFailure(result, LinkedPartSourceResolveStatus::IdentityConflict);
    const auto reversed = resolveLinkedPartSources(
        {descriptor("b", changed), descriptor("a", valid)}, windowsPolicy(),
        fs);
    checkFailure(reversed, LinkedPartSourceResolveStatus::IdentityConflict);
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid));
    const auto result = resolveLinkedPartSources(
        {descriptor("a", valid), descriptor("a", valid)}, windowsPolicy(),
        fs);
    CHECK(result.succeeded());
    CHECK(result.sources.size() == 1);
    CHECK(result.sources[0].descriptorIndexes ==
          (std::vector<std::size_t>{0, 1}));
    CHECK(result.filesInspected == 2);
    CHECK(result.filesRead == 1);
    CHECK(result.sourcesDecoded == 1);
    CHECK(fs.reads == 1);
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid, "file-a"));
    fs.files.emplace(QStringLiteral("C:/Root/b"),
                     opened(QStringLiteral("C:/Root/b"), valid, "file-b"));
    const auto result = resolveLinkedPartSources(
        {descriptor("a", valid), descriptor("b", valid)}, windowsPolicy(),
        fs);
    CHECK(result.succeeded());
    CHECK(result.sources.size() == 1);
    CHECK(result.sources[0].descriptorIndexes ==
          (std::vector<std::size_t>{0, 1}));
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/z"),
                     opened(QStringLiteral("C:/Root/z"), valid, "same-file"));
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid, "same-file"));
    auto policy = windowsPolicy();
    policy.budgets.maximumTotalBytes =
        static_cast<std::uint64_t>(valid.size());
    const auto result = resolveLinkedPartSources(
        {descriptor("z", valid), descriptor("a", valid)}, policy, fs);
    CHECK(result.succeeded());
    CHECK(result.sources.size() == 1);
    CHECK(result.sources[0].canonicalPath == QStringLiteral("C:/Root/a"));
  }
  {
    // Physical replicas with the same persisted identity/content are read to
    // prove their hash, but the V5 decoder and materializer run once.
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid, "replica-a"));
    fs.files.emplace(QStringLiteral("C:/Root/b"),
                     opened(QStringLiteral("C:/Root/b"), valid, "replica-b"));
    const auto result = resolveLinkedPartSources(
        {descriptor("a", valid), descriptor("b", valid)}, windowsPolicy(), fs);
    CHECK(result.succeeded());
    CHECK(result.sources.size() == 1);
    CHECK(result.filesRead == 2);
    CHECK(result.sourcesDecoded == 1);
    CHECK(fs.reads == 2);
  }

  for (const auto& [bytes, status] :
       std::vector<std::pair<QByteArray, LinkedPartSourceResolveStatus>>{
           {versionBytes(1), LinkedPartSourceResolveStatus::IdentityUnavailable},
           {versionBytes(2), LinkedPartSourceResolveStatus::IdentityUnavailable},
           {versionBytes(3), LinkedPartSourceResolveStatus::IdentityUnavailable},
           {versionBytes(4), LinkedPartSourceResolveStatus::IdentityUnavailable},
           {versionBytes(6), LinkedPartSourceResolveStatus::NestingUnsupported},
           {versionBytes(99), LinkedPartSourceResolveStatus::UnsupportedSource},
           {futureV5Bytes(valid),
            LinkedPartSourceResolveStatus::UnsupportedSource},
           {floatingVersionBytes(1e100),
            LinkedPartSourceResolveStatus::InvalidSource},
           {QByteArrayLiteral("not-json"),
            LinkedPartSourceResolveStatus::InvalidSource}}) {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), bytes));
    const auto result = resolveLinkedPartSources(
        {descriptor("a", bytes)}, windowsPolicy(), fs);
    checkFailure(result, status);
  }

  {
    // Materialization is transactional across the complete batch: even when
    // the second Candidate fails during recompute, no first output escapes.
    const QByteArray lateFailure = lateMaterializationFailureBytes();
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid, "valid"));
    fs.files.emplace(
        QStringLiteral("C:/Root/b"),
        opened(QStringLiteral("C:/Root/b"), lateFailure, "late-failure"));
    const auto result = resolveLinkedPartSources(
        {descriptor("a", valid),
         descriptor("b", lateFailure,
                    ExternalSourceKey{kOtherProject, kDefinition})},
        windowsPolicy(), fs);
    checkFailure(result, LinkedPartSourceResolveStatus::InvalidSource);
    CHECK(result.sources.empty());
  }

  {
    const QByteArray rebuild = nonzeroRebuildBytes();
    const auto scanned = scanProductEnvelopeV5(QByteArrayView(rebuild));
    CHECK(scanned.status == ProductEnvelopeV5ScanStatus::Candidate);
    CHECK(scanned.candidate && scanned.candidate->embeddedPart);
    const auto work = static_cast<std::uint64_t>(
        scanned.candidate->embeddedPart->legacySketch.topology.payload
            .rebuildWork);
    CHECK(work > 0);
    FakeFileSystem exact;
    exact.files.emplace(QStringLiteral("C:/Root/a"),
                        opened(QStringLiteral("C:/Root/a"), rebuild));
    auto policy = windowsPolicy();
    policy.budgets.maximumRebuildWork = work;
    CHECK(resolveLinkedPartSources({descriptor("a", rebuild)}, policy, exact)
              .succeeded());
    FakeFileSystem overflow;
    overflow.files.emplace(QStringLiteral("C:/Root/a"),
                           opened(QStringLiteral("C:/Root/a"), rebuild));
    policy.budgets.maximumRebuildWork = work - 1;
    checkFailure(resolveLinkedPartSources({descriptor("a", rebuild)}, policy,
                                          overflow),
                 LinkedPartSourceResolveStatus::BudgetExceeded);
  }

  {
    FakeFileSystem fs;
    auto policy = windowsPolicy();
    policy.budgets.maximumDescriptors = 0;
    const auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                                 policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::BudgetExceeded);
    CHECK(fs.attempts.empty());
  }
  {
    FakeFileSystem fs;
    auto policy = windowsPolicy();
    policy.budgets.maximumRoots = 1;
    const auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                                 policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::BudgetExceeded);
    CHECK(fs.attempts.empty());
  }
  {
    FakeFileSystem fs;
    auto policy = windowsPolicy();
    policy.libraryRoots.clear();
    policy.budgets.maximumTotalRootUtf8Bytes = 7;  // Exact UTF-8 boundary.
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid));
    CHECK(resolveLinkedPartSources({descriptor("a", valid)}, policy, fs)
              .succeeded());
    fs.attempts.clear();
    policy.budgets.maximumTotalRootUtf8Bytes = 6;
    const auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                                 policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::BudgetExceeded);
    CHECK(fs.attempts.empty());
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid, "id", 1, 1));
    auto policy = windowsPolicy();
    policy.budgets.maximumDescriptors = 1;
    policy.budgets.maximumFileBytes =
        static_cast<std::uint64_t>(valid.size());
    policy.budgets.maximumTotalBytes =
        static_cast<std::uint64_t>(valid.size());
    policy.budgets.maximumTotalPathUtf8Bytes = 1;
    policy.budgets.maximumPathComponents = 1;
    policy.budgets.maximumAliasDepth = 1;
    policy.budgets.maximumFileSystemWork = 2;  // inspect + held-handle read
    policy.budgets.maximumRebuildWork = 0;
    CHECK(resolveLinkedPartSources({descriptor("a", valid)}, policy, fs)
              .succeeded());

    policy.budgets.maximumAliasDepth = 0;
    const auto aliasOverflow =
        resolveLinkedPartSources({descriptor("a", valid)}, policy, fs);
    checkFailure(aliasOverflow, LinkedPartSourceResolveStatus::BudgetExceeded);
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid));
    auto policy = windowsPolicy();
    policy.budgets.maximumFileBytes =
        static_cast<std::uint64_t>(valid.size() - 1);
    const auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                                 policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::BudgetExceeded);
    CHECK(fs.reads == 0);
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid));
    auto policy = windowsPolicy();
    policy.budgets.maximumTotalBytes =
        static_cast<std::uint64_t>(valid.size() - 1);
    const auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                                 policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::BudgetExceeded);
    CHECK(fs.reads == 0);
  }
  {
    const QByteArray second = v5Bytes(QStringLiteral("second"));
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid, "first"));
    fs.files.emplace(QStringLiteral("C:/Root/b"),
                     opened(QStringLiteral("C:/Root/b"), second, "second"));
    auto policy = windowsPolicy();
    policy.budgets.maximumTotalBytes =
        static_cast<std::uint64_t>(valid.size() + second.size() - 1);
    const auto result = resolveLinkedPartSources(
        {descriptor("a", valid), descriptor("b", second)}, policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::BudgetExceeded);
    CHECK(fs.reads == 1);
  }
  {
    FakeFileSystem fs;
    auto policy = windowsPolicy();
    policy.budgets.maximumFileSystemWork = 0;
    const auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                                 policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::BudgetExceeded);
    CHECK(fs.attempts.empty());
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(QStringLiteral("C:/Root/a"),
                     opened(QStringLiteral("C:/Root/a"), valid));
    auto policy = windowsPolicy();
    policy.budgets.maximumFileSystemWork = 1;
    const auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                                 policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::BudgetExceeded);
    CHECK(fs.reads == 0);
  }
  {
    FakeFileSystem fs;
    fs.files.emplace(
        QStringLiteral("C:/Root/a"),
        opened(QStringLiteral("C:/Root/a"), valid, "id", 0,
               (std::numeric_limits<std::uint64_t>::max)()));
    const auto result = resolveLinkedPartSources({descriptor("a", valid)},
                                                 windowsPolicy(), fs);
    checkFailure(result, LinkedPartSourceResolveStatus::BudgetExceeded);
  }
  {
    FakeFileSystem fs;
    auto policy = windowsPolicy();
    policy.budgets.maximumTotalPathUtf8Bytes = 1;
    const auto result = resolveLinkedPartSources({descriptor("aa", valid)},
                                                 policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::BudgetExceeded);
    CHECK(fs.attempts.empty());
  }
  {
    FakeFileSystem fs;
    auto policy = windowsPolicy();
    policy.budgets.maximumPathComponents = 1;
    const auto result = resolveLinkedPartSources({descriptor("a/b", valid)},
                                                 policy, fs);
    checkFailure(result, LinkedPartSourceResolveStatus::BudgetExceeded);
    CHECK(fs.attempts.empty());
  }

  return EXIT_SUCCESS;
}
