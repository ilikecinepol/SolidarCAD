#include "ui/application/ProjectApplicationService.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <cerrno>
#include <unistd.h>
#endif

#include "io/DocumentExportShapes.h"
#include "io/StepExchange.h"
#include "io/StlExporter.h"
#include "model/IdGeneration.h"
#include "project/ProductEnvelopeV5Conversion.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/ProductEnvelopeV5Materialization.h"
#include "project/ProductEnvelopeV5Relocation.h"
#include "project/ProductEnvelopeV6Decoder.h"
#include "project/ProductEnvelopeV6Materialization.h"
#include "project/ProductEnvelopeV7Decoder.h"
#include "project/ProductEnvelopeV7Materialization.h"
#include "project/ProductEnvelopeV8Decoder.h"
#include "project/ProductEnvelopeV8Materialization.h"
#include "project/ProductEnvelopeV9Decoder.h"
#include "project/ProductEnvelopeV9Materialization.h"
#include "project/ProductEnvelopeV10Decoder.h"
#include "project/ProductEnvelopeV10Materialization.h"
#include "project/ProductEnvelopeV11Decoder.h"
#include "project/ProductEnvelopeV11Materialization.h"
#include "project/ProductEnvelopeV12Decoder.h"
#include "project/ProductEnvelopeV12Materialization.h"
#include "project/LinkedPartSourceResolver.h"
#include "project/ProjectFile.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"
#include "project/internal/ProjectAtomicWriting.h"
#include "project/internal/ProductEnvelopeV5AtomicWriting.h"
#include "project/internal/ProductEnvelopeV7AtomicWriting.h"
#include "project/internal/ProductEnvelopeV8AtomicWriting.h"
#include "project/internal/ProductEnvelopeV9AtomicWriting.h"
#include "project/internal/ProductEnvelopeV10AtomicWriting.h"
#include "project/internal/ProductEnvelopeV11AtomicWriting.h"
#include "project/internal/ProductEnvelopeV12AtomicWriting.h"
#include "ui/application/internal/ProductCloneApplicationService.h"
#include "ui/application/internal/ProductConversionApplicationService.h"
#include "ui/application/internal/ProductRelocationApplicationService.h"
#include "ui/application/internal/AssemblySaveAsPathMetadata.h"
#include "ui/application/internal/AssemblyLinkedOpenPolicy.h"

namespace solidar {
namespace {

constexpr DefinitionId kTransientApplicationRootDefinitionId{1};
constexpr double kMaximumExactJsonInteger = 9007199254740991.0;

ProductDocument makeSingleDefinitionProduct(
    Document&& document, DefinitionId rootDefinitionId,
    std::optional<ProductDocumentIdentity> identity = std::nullopt) {
  DefinitionRegistry definitions;
  PartDefinition root(rootDefinitionId,
                      std::make_unique<Document>(std::move(document)));
  const auto inserted = definitions.insert(std::move(root));
  if (inserted != DefinitionInsertStatus::Inserted)
    throw std::logic_error("failed to stage root definition");

  auto created = ProductDocument::create(std::move(definitions),
                                         rootDefinitionId,
                                         std::move(identity));
  if (!created)
    throw std::logic_error("failed to create staged product document");
  return std::move(*created.product);
}

ProductDocument makeTransientProductDocument(Document&& document) {
  return makeSingleDefinitionProduct(
      std::move(document), kTransientApplicationRootDefinitionId);
}

enum class ProjectEnvelopeDispatch {
  LegacyPart,
  ProductV5,
  ProductV6,
  ProductV7,
  ProductV8,
  ProductV9,
  ProductV10,
  ProductV11,
  ProductV12,
  Future,
  InvalidSolidarHeader,
};

struct ProjectEnvelopeProbe {
  ProjectEnvelopeDispatch dispatch{ProjectEnvelopeDispatch::LegacyPart};
  std::optional<QByteArray> payload;
  QString error;
};

ProjectEnvelopeProbe probeProjectEnvelope(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  const qint64 size = file.size();
  if (size < 0 || size > project::ProjectFile::kMaximumFileBytes) return {};
  const QByteArray payload = file.readAll();
  if (file.error() != QFileDevice::NoError || payload.size() != size)
    return {};

  QJsonParseError parseError;
  const QJsonDocument parsed = QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
    return {};
  const QJsonObject root = parsed.object();
  if (!root.value(QStringLiteral("format")).isString() ||
      root.value(QStringLiteral("format")).toString() !=
          QStringLiteral("solidar-project")) {
    return {};
  }
  const QJsonValue versionValue = root.value(QStringLiteral("version"));
  if (!versionValue.isDouble()) {
    return {ProjectEnvelopeDispatch::InvalidSolidarHeader, payload,
            QStringLiteral("$.version must be an exact positive integer")};
  }
  const double version = versionValue.toDouble();
  if (!std::isfinite(version) || std::trunc(version) != version ||
      version < 1.0 || version > kMaximumExactJsonInteger) {
    return {ProjectEnvelopeDispatch::InvalidSolidarHeader, payload,
            QStringLiteral("$.version is outside its integer range")};
  }
  if (version == 6.0)
    return {ProjectEnvelopeDispatch::ProductV6, payload, {}};
  if (version == 7.0)
    return {ProjectEnvelopeDispatch::ProductV7, payload, {}};
  if (version == 8.0)
    return {ProjectEnvelopeDispatch::ProductV8, payload, {}};
  if (version == 9.0)
    return {ProjectEnvelopeDispatch::ProductV9, payload, {}};
  if (version == 10.0)
    return {ProjectEnvelopeDispatch::ProductV10, payload, {}};
  if (version == 11.0)
    return {ProjectEnvelopeDispatch::ProductV11, payload, {}};
  if (version == 12.0)
    return {ProjectEnvelopeDispatch::ProductV12, payload, {}};
  if (version > 12.0)
    return {ProjectEnvelopeDispatch::Future, payload, {}};
  if (version == 5.0)
    return {ProjectEnvelopeDispatch::ProductV5, payload, {}};
  return {ProjectEnvelopeDispatch::LegacyPart, payload, {}};
}

StagedApplicationDocumentResult invalidStage(QString detail) {
  return {{ProjectApplicationErrorCode::InvalidProject, std::move(detail)},
          std::nullopt};
}

StagedApplicationDocumentResult unsupportedStage(QString detail) {
  return {{ProjectApplicationErrorCode::UnsupportedProject, std::move(detail)},
          std::nullopt};
}

StagedApplicationDocumentResult stageProductV5Payload(QByteArrayView payload) {
  auto scanned = project::scanProductEnvelopeV5(payload);
  if (scanned.status == project::ProductEnvelopeV5ScanStatus::Invalid ||
      scanned.status == project::ProductEnvelopeV5ScanStatus::NotApplicable)
    return invalidStage(std::move(scanned.error));
  if (!scanned.candidate)
    return unsupportedStage(std::move(scanned.error));

  auto materialized = project::materializeProductEnvelopeV5(*scanned.candidate);
  if (!materialized.succeeded()) {
    if (materialized.status ==
        project::ProductEnvelopeV5MaterializationStatus::Unsupported)
      return unsupportedStage(std::move(materialized.error));
    return invalidStage(std::move(materialized.error));
  }
  return {{}, StagedApplicationDocument(
                  std::move(*materialized.product), std::nullopt,
                  project::ProjectPersistenceState{
                      project::ProjectPersistenceFormat::V5})};
}

StagedApplicationDocumentResult stageProductV6Payload(QByteArrayView payload) {
  auto scanned = project::scanProductEnvelopeV6(payload);
  if (scanned.status == project::ProductEnvelopeV6ScanStatus::Invalid ||
      scanned.status == project::ProductEnvelopeV6ScanStatus::NotApplicable)
    return invalidStage(std::move(scanned.error));
  if (scanned.status == project::ProductEnvelopeV6ScanStatus::Unsupported ||
      !scanned.candidate)
    return unsupportedStage(std::move(scanned.error));

  auto materialized = project::materializeProductEnvelopeV6(*scanned.candidate);
  if (!materialized.succeeded()) {
    if (materialized.status ==
        project::ProductEnvelopeV6MaterializationStatus::Unsupported)
      return unsupportedStage(std::move(materialized.error));
    return invalidStage(std::move(materialized.error));
  }
  return {{}, StagedApplicationDocument(
                  std::move(*materialized.product), std::nullopt,
                  project::ProjectPersistenceState{
                      project::ProjectPersistenceFormat::V6})};
}

LinkedPartRuntimeStatus runtimeStatus(
    project::LinkedPartSourceResolveStatus status) noexcept {
  using Status = project::LinkedPartSourceResolveStatus;
  switch (status) {
    case Status::Resolved:
      return LinkedPartRuntimeStatus::Exact;
    case Status::Missing:
      return LinkedPartRuntimeStatus::Missing;
    case Status::Outdated:
      return LinkedPartRuntimeStatus::Outdated;
    case Status::IdentityMismatch:
    case Status::DefinitionMissing:
    case Status::IdentityUnavailable:
    case Status::IdentityConflict:
      return LinkedPartRuntimeStatus::IdentityMismatch;
    case Status::PathEscape:
    case Status::OutsideAllowedRoot:
    case Status::NetworkDisabled:
    case Status::SourceNotRegular:
    case Status::TraversalLoop:
      return LinkedPartRuntimeStatus::SecurityRejected;
    case Status::BudgetExceeded:
    case Status::ResourceFailure:
    case Status::SourceChanged:
      return LinkedPartRuntimeStatus::ResourceFailure;
    default:
      return LinkedPartRuntimeStatus::InvalidSource;
  }
}

std::vector<project::LinkedPartSourceDescriptor> linkedDescriptors(
    const ProductDocument& product,
    std::vector<DefinitionId>* definitionIds = nullptr) {
  std::vector<project::LinkedPartSourceDescriptor> descriptors;
  for (const DefinitionId id : product.definitions().definitionIds()) {
    const auto* binding = product.partLinkedBinding(id);
    if (!binding) continue;
    const QByteArray locator =
        QByteArray::fromStdString(binding->relativePathHintUtf8);
    descriptors.push_back(
        {binding->key, QString::fromUtf8(locator), binding->acceptedHash});
    if (definitionIds) definitionIds->push_back(id);
  }
  return descriptors;
}

project::LinkedPartSourcePolicy linkedPolicyForProject(
    const QString& projectPath) {
  project::LinkedPartSourcePolicy policy;
  policy.projectRoot = QFileInfo(projectPath).absolutePath();
#if defined(Q_OS_WIN)
  policy.pathSemantics = project::LinkedPathSemantics::Windows;
#else
  policy.pathSemantics = project::LinkedPathSemantics::Posix;
#endif
  return policy;
}

StagedApplicationDocumentResult finishResolvedAssembly(
    std::unique_ptr<ProductDocument> product,
    project::ProjectPersistenceFormat format,
    std::optional<project::LinkedPartSourceResolveResult> injected,
    const QString& path,
    std::vector<project::MateRuntimeDiagnostics> mateDiagnostics = {}) {
  LinkedPartRuntimeStates states;
  std::vector<DefinitionId> definitionIds;
  auto descriptors = linkedDescriptors(*product, &definitionIds);
  for (const DefinitionId id : definitionIds)
    states.emplace(id, LinkedPartRuntimeState{});
  if (!descriptors.empty()) {
    auto resolved = injected
        ? std::move(*injected)
        : project::resolveLinkedPartSourcesNative(
              descriptors, linkedPolicyForProject(path));
    if (resolved.succeeded()) {
      for (const DefinitionId id : definitionIds)
        states[id] = {LinkedPartRuntimeStatus::Exact, {}};
    } else {
      if (resolved.status ==
          project::LinkedPartSourceResolveStatus::InvalidDescriptor)
        return invalidStage(std::move(resolved.error));
      if (resolved.failureIndex < definitionIds.size())
        states[definitionIds[resolved.failureIndex]] = {
            runtimeStatus(resolved.status), resolved.error};
    }
  }

  StagedApplicationDocument staged(
      std::move(*product), std::nullopt,
      project::ProjectPersistenceState{format});
  staged.linkedPartRuntimeStates = std::move(states);
  staged.mateRuntimeDiagnostics = std::move(mateDiagnostics);
  return {{}, std::move(staged)};
}

StagedApplicationDocumentResult stageResolvedAssembly(
    std::unique_ptr<ProductDocument> product,
    project::ProjectPersistenceFormat format, const QString& path) {
  return finishResolvedAssembly(std::move(product), format, std::nullopt,
                                path);
}

StagedApplicationDocumentResult stageProductV7Payload(
    QByteArrayView payload, const QString& path) {
  auto scanned = project::scanProductEnvelopeV7(payload);
  if (scanned.status == project::ProductEnvelopeV7ScanStatus::Invalid ||
      scanned.status == project::ProductEnvelopeV7ScanStatus::NotApplicable)
    return invalidStage(std::move(scanned.error));
  if (scanned.status == project::ProductEnvelopeV7ScanStatus::Unsupported ||
      !scanned.candidate)
    return unsupportedStage(std::move(scanned.error));
  auto materialized = project::materializeProductEnvelopeV7(*scanned.candidate);
  if (!materialized.succeeded()) {
    if (materialized.status ==
        project::ProductEnvelopeV7MaterializationStatus::Unsupported)
      return unsupportedStage(std::move(materialized.error));
    return invalidStage(std::move(materialized.error));
  }
  return stageResolvedAssembly(std::move(materialized.product),
                               project::ProjectPersistenceFormat::V7, path);
}

StagedApplicationDocumentResult stageProductV8Payload(
    QByteArrayView payload, const QString& path) {
  auto scanned = project::scanProductEnvelopeV8(payload);
  if (scanned.status == project::ProductEnvelopeV8ScanStatus::Invalid ||
      scanned.status == project::ProductEnvelopeV8ScanStatus::NotApplicable)
    return invalidStage(std::move(scanned.error));
  if (scanned.status == project::ProductEnvelopeV8ScanStatus::Unsupported ||
      !scanned.candidate)
    return unsupportedStage(std::move(scanned.error));
  auto materialized = project::materializeProductEnvelopeV8(*scanned.candidate);
  if (!materialized.succeeded())
    return invalidStage(std::move(materialized.error));
  return stageResolvedAssembly(std::move(materialized.product),
                               project::ProjectPersistenceFormat::V8, path);
}

StagedApplicationDocumentResult stageProductV9Payload(
    QByteArrayView payload, const QString& path) {
  auto scanned = project::scanProductEnvelopeV9(payload);
  if (scanned.status == project::ProductEnvelopeV9ScanStatus::Invalid ||
      scanned.status == project::ProductEnvelopeV9ScanStatus::NotApplicable)
    return invalidStage(std::move(scanned.error));
  if (scanned.status == project::ProductEnvelopeV9ScanStatus::Unsupported ||
      !scanned.candidate)
    return unsupportedStage(std::move(scanned.error));
  auto materialized = project::materializeProductEnvelopeV9(*scanned.candidate);
  if (!materialized.succeeded())
    return invalidStage(std::move(materialized.error));
  return finishResolvedAssembly(
      std::move(materialized.product), project::ProjectPersistenceFormat::V9,
      std::nullopt, path, std::move(materialized.mateDiagnostics));
}

StagedApplicationDocumentResult stageProductV10Payload(
    QByteArrayView payload) {
  auto scanned = project::scanProductEnvelopeV10(payload);
  if (scanned.status == project::ProductEnvelopeV10ScanStatus::Invalid ||
      scanned.status == project::ProductEnvelopeV10ScanStatus::NotApplicable)
    return invalidStage(std::move(scanned.error));
  if (scanned.status == project::ProductEnvelopeV10ScanStatus::Unsupported ||
      !scanned.candidate)
    return unsupportedStage(std::move(scanned.error));
  auto materialized = project::materializeProductEnvelopeV10(
      *scanned.candidate,
      project::ProductEnvelopeV10MaterializationFailurePoint::None,
      project::ProductEnvelopeV10MaterializationMode::Publication);
  if (!materialized.succeeded())
    return invalidStage(std::move(materialized.error));
  return {{}, StagedApplicationDocument(
                  std::move(*materialized.product), std::nullopt,
                  project::ProjectPersistenceState{
                      project::ProjectPersistenceFormat::V10})};
}

StagedApplicationDocumentResult stageProductV11Payload(
    QByteArrayView payload) {
  auto scanned = project::scanProductEnvelopeV11(payload);
  if (scanned.status == project::ProductEnvelopeV11ScanStatus::Invalid ||
      scanned.status == project::ProductEnvelopeV11ScanStatus::NotApplicable)
    return invalidStage(std::move(scanned.error));
  if (scanned.status == project::ProductEnvelopeV11ScanStatus::Unsupported ||
      !scanned.candidate)
    return unsupportedStage(std::move(scanned.error));
  auto materialized = project::materializeProductEnvelopeV11(
      *scanned.candidate,
      project::ProductEnvelopeV11MaterializationFailurePoint::None,
      project::ProductEnvelopeV11MaterializationMode::Publication);
  if (!materialized.succeeded())
    return invalidStage(std::move(materialized.error));
  return {{}, StagedApplicationDocument(
                  std::move(*materialized.product), std::nullopt,
                  project::ProjectPersistenceState{
                      project::ProjectPersistenceFormat::V11})};
}

StagedApplicationDocumentResult stageProductV12Payload(
    QByteArrayView payload) {
  auto scanned = project::scanProductEnvelopeV12(payload);
  if (scanned.status == project::ProductEnvelopeV12ScanStatus::Invalid ||
      scanned.status == project::ProductEnvelopeV12ScanStatus::NotApplicable)
    return invalidStage(std::move(scanned.error));
  if (scanned.status == project::ProductEnvelopeV12ScanStatus::Unsupported ||
      !scanned.candidate)
    return unsupportedStage(std::move(scanned.error));
  auto materialized = project::materializeProductEnvelopeV12(
      *scanned.candidate,
      project::ProductEnvelopeV12MaterializationFailurePoint::None,
      project::ProductEnvelopeV12MaterializationMode::Publication);
  if (!materialized.succeeded())
    return invalidStage(std::move(materialized.error));
  return {{}, StagedApplicationDocument(
                  std::move(*materialized.product), std::nullopt,
                  project::ProjectPersistenceState{
                      project::ProjectPersistenceFormat::V12})};
}

std::optional<QString> exactQStringFromUtf8(const std::string& bytes) {
  const QByteArray original(bytes.data(),
                            static_cast<qsizetype>(bytes.size()));
  const QString decoded = QString::fromUtf8(original);
  if (decoded.toUtf8() != original) return std::nullopt;
  return decoded;
}

std::optional<std::string> exactUtf8FromQString(const QString& text) {
  if (text.size() > project::ProjectFile::kMaximumStringCharacters)
    return std::nullopt;
  const QByteArray encoded = text.toUtf8();
  if (QString::fromUtf8(encoded) != text) return std::nullopt;
  return std::string(encoded.constData(),
                     static_cast<std::size_t>(encoded.size()));
}

ProjectApplicationStatus failure(ProjectApplicationErrorCode code,
                                 QString detail);

std::filesystem::path nativePath(const QString& path) {
#if defined(Q_OS_WIN)
  return std::filesystem::path(path.toStdWString());
#else
  return std::filesystem::path(path.toStdString());
#endif
}

bool sameNormalizedAbsolutePath(const QString& left,
                                const QString& right) noexcept {
  const QString normalizedLeft =
      QDir::cleanPath(QFileInfo(left).absoluteFilePath());
  const QString normalizedRight =
      QDir::cleanPath(QFileInfo(right).absoluteFilePath());
#if defined(Q_OS_WIN)
  return normalizedLeft.compare(normalizedRight, Qt::CaseInsensitive) == 0;
#else
  return normalizedLeft == normalizedRight;
#endif
}

bool hasExplicitDotPathSegment(QStringView path) noexcept {
  qsizetype begin = 0;
  while (begin <= path.size()) {
    qsizetype end = begin;
    while (end < path.size() && path[end] != QLatin1Char('/') &&
           path[end] != QLatin1Char('\\'))
      ++end;
    const QStringView component = path.sliced(begin, end - begin);
    if (component == QStringView(u".") || component == QStringView(u".."))
      return true;
    if (end == path.size()) break;
    begin = end + 1;
  }
  return false;
}

bool isAmbiguousWindowsDriveRelativePath(QStringView path) noexcept {
  if (path.size() < 2 || path[1] != QLatin1Char(':')) return false;
  const QChar drive = path[0];
  if (!((drive >= QLatin1Char('A') && drive <= QLatin1Char('Z')) ||
        (drive >= QLatin1Char('a') && drive <= QLatin1Char('z'))))
    return false;
  return path.size() == 2 ||
         (path[2] != QLatin1Char('/') && path[2] != QLatin1Char('\\'));
}

bool invalidRawDestinationPath(QStringView path) noexcept {
  return hasExplicitDotPathSegment(path) ||
         isAmbiguousWindowsDriveRelativePath(path);
}

using PathRelation =
    application::detail::AssemblySaveAsPathRelation;
using PathMetadataReader =
    application::detail::AssemblySaveAsPathMetadataReader;
using MissingPathResolution =
    application::detail::MissingPathResolution;
using MissingPathResolutionStatus =
    application::detail::MissingPathResolutionStatus;
using PathNameComparisonPolicy =
    application::detail::PathNameComparisonPolicy;

PathNameComparisonPolicy pathNameComparisonPolicy(
    const std::filesystem::path& canonicalParent) noexcept {
#if defined(Q_OS_WIN)
  const HANDLE handle = CreateFileW(
      canonicalParent.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return PathNameComparisonPolicy::Error;
  struct CaseSensitiveInfo final {
    ULONG flags;
  } info{};
  constexpr auto kFileCaseSensitiveInfo =
      static_cast<FILE_INFO_BY_HANDLE_CLASS>(23);
  constexpr ULONG kCaseSensitiveDirectory = 1;
  const BOOL queried = GetFileInformationByHandleEx(
      handle, kFileCaseSensitiveInfo, &info, sizeof(info));
  const BOOL closed = CloseHandle(handle);
  if (!queried || !closed) return PathNameComparisonPolicy::Error;
  return (info.flags & kCaseSensitiveDirectory) != 0
             ? PathNameComparisonPolicy::CaseSensitiveExact
             : PathNameComparisonPolicy::UnicodeNormalizedCaseInsensitive;
#elif defined(Q_OS_MACOS) && defined(_PC_CASE_SENSITIVE)
  errno = 0;
  const long caseSensitive =
      ::pathconf(canonicalParent.c_str(), _PC_CASE_SENSITIVE);
  if (caseSensitive == 1)
    return PathNameComparisonPolicy::UnicodeNormalizedCaseSensitive;
  if (caseSensitive == 0)
    return PathNameComparisonPolicy::UnicodeNormalizedCaseInsensitive;
  return errno == 0 ? PathNameComparisonPolicy::Unknown
                    : PathNameComparisonPolicy::Error;
#elif defined(Q_OS_MACOS)
  return PathNameComparisonPolicy::Unknown;
#else
  // The resolver's POSIX policy is byte- and case-sensitive. Platforms that
  // cannot make that guarantee must add a native query and return Unknown.
  static_cast<void>(canonicalParent);
  return PathNameComparisonPolicy::CaseSensitiveExact;
#endif
}

class NativeAssemblySaveAsPathMetadataReader final
    : public PathMetadataReader {
 public:
  PathRelation compareFileIdentity(
      const QString& left, const QString& right) const noexcept override {
    try {
      std::error_code error;
      const auto leftStatus =
          std::filesystem::symlink_status(nativePath(left), error);
      if (error) {
        if (error == std::errc::no_such_file_or_directory)
          return PathRelation::Missing;
        return PathRelation::Error;
      }
      const auto rightStatus =
          std::filesystem::symlink_status(nativePath(right), error);
      if (error) {
        if (error == std::errc::no_such_file_or_directory)
          return PathRelation::Missing;
        return PathRelation::Error;
      }
      if (!std::filesystem::exists(leftStatus) ||
          !std::filesystem::exists(rightStatus))
        return PathRelation::Missing;
      const bool same =
          std::filesystem::equivalent(nativePath(left), nativePath(right),
                                      error);
      if (error) return PathRelation::Error;
      return same ? PathRelation::Same : PathRelation::Different;
    } catch (...) {
      return PathRelation::Error;
    }
  }

  PathRelation compareCanonicalPaths(
      const QString& left, const QString& right) const noexcept override {
    try {
      std::error_code error;
      const auto leftStatus =
          std::filesystem::symlink_status(nativePath(left), error);
      if (error) {
        if (error == std::errc::no_such_file_or_directory)
          return PathRelation::Missing;
        return PathRelation::Error;
      }
      const auto rightStatus =
          std::filesystem::symlink_status(nativePath(right), error);
      if (error) {
        if (error == std::errc::no_such_file_or_directory)
          return PathRelation::Missing;
        return PathRelation::Error;
      }
      if (!std::filesystem::exists(leftStatus) ||
          !std::filesystem::exists(rightStatus))
        return PathRelation::Missing;
      const auto leftCanonical =
          std::filesystem::canonical(nativePath(left), error);
      if (error) return PathRelation::Error;
      const auto rightCanonical =
          std::filesystem::canonical(nativePath(right), error);
      if (error) return PathRelation::Error;
#if defined(Q_OS_WIN)
      const QString normalizedLeft =
          QDir::cleanPath(QString::fromStdWString(leftCanonical.native()));
      const QString normalizedRight =
          QDir::cleanPath(QString::fromStdWString(rightCanonical.native()));
      return normalizedLeft.compare(normalizedRight, Qt::CaseInsensitive) == 0
                 ? PathRelation::Same
                 : PathRelation::Different;
#else
      return leftCanonical == rightCanonical ? PathRelation::Same
                                             : PathRelation::Different;
#endif
    } catch (...) {
      return PathRelation::Error;
    }
  }

  MissingPathResolution resolveMissingPath(
      const QString& authored) const noexcept override {
    try {
      constexpr std::size_t kMaximumComponents = 256;
      constexpr std::size_t kMaximumMetadataWork = 1024;
      std::size_t work = 0;
      std::filesystem::path cursor =
          nativePath(QFileInfo(authored).absoluteFilePath());
      if (cursor.empty() || !cursor.is_absolute()) return {};
      std::vector<QString> reversedSuffix;
      for (std::size_t depth = 0; depth <= kMaximumComponents; ++depth) {
        if (++work > kMaximumMetadataWork) return {};
        std::error_code error;
        const auto linkStatus = std::filesystem::symlink_status(cursor, error);
        if (error && error != std::errc::no_such_file_or_directory)
          return {MissingPathResolutionStatus::Error};
        if (!error && std::filesystem::exists(linkStatus)) {
          if (++work > kMaximumMetadataWork) return {};
          const auto canonical = std::filesystem::canonical(cursor, error);
          if (error) return {MissingPathResolutionStatus::Error};
          if (++work > kMaximumMetadataWork) return {};
          const auto canonicalStatus = std::filesystem::status(canonical, error);
          if (error) return {MissingPathResolutionStatus::Error};
          if (!reversedSuffix.empty() &&
              !std::filesystem::is_directory(canonicalStatus))
            return {};
          std::reverse(reversedSuffix.begin(), reversedSuffix.end());
#if defined(Q_OS_WIN)
          const QString parent = QString::fromStdWString(canonical.native());
#else
          const QString parent = QString::fromStdString(canonical.native());
#endif
          return {MissingPathResolutionStatus::Resolved, parent,
                  std::move(reversedSuffix),
                  pathNameComparisonPolicy(canonical)};
        }
        if (reversedSuffix.size() == kMaximumComponents) return {};
        const auto name = cursor.filename();
        const auto parent = cursor.parent_path();
        if (name.empty() || parent.empty() || parent == cursor) return {};
#if defined(Q_OS_WIN)
        reversedSuffix.push_back(QString::fromStdWString(name.native()));
#else
        reversedSuffix.push_back(QString::fromStdString(name.native()));
#endif
        cursor = parent;
      }
      return {};
    } catch (...) {
      return {MissingPathResolutionStatus::Error};
    }
  }
};

const PathMetadataReader& nativeAssemblySaveAsPathMetadataReader() {
  static const NativeAssemblySaveAsPathMetadataReader reader;
  return reader;
}

enum class AliasCheck { Distinct, Aliases, Error };

bool equivalentPathComponent(QStringView left, QStringView right,
                             PathNameComparisonPolicy policy) {
  switch (policy) {
    case PathNameComparisonPolicy::CaseSensitiveExact:
      return left == right;
    case PathNameComparisonPolicy::UnicodeNormalizedCaseSensitive:
      return left.toString().normalized(QString::NormalizationForm_D) ==
             right.toString().normalized(QString::NormalizationForm_D);
    case PathNameComparisonPolicy::UnicodeNormalizedCaseInsensitive:
      return left.toString()
                 .normalized(QString::NormalizationForm_D)
                 .toCaseFolded()
                 .normalized(QString::NormalizationForm_D) ==
             right.toString()
                 .normalized(QString::NormalizationForm_D)
                 .toCaseFolded()
                 .normalized(QString::NormalizationForm_D);
    case PathNameComparisonPolicy::Unknown:
    case PathNameComparisonPolicy::Error:
      return false;
  }
  return false;
}

AliasCheck compareMissingPathAliases(const QString& left,
                                     const QString& right,
                                     const PathMetadataReader& reader) {
  const MissingPathResolution leftResolved = reader.resolveMissingPath(left);
  const MissingPathResolution rightResolved = reader.resolveMissingPath(right);
  if (leftResolved.status != MissingPathResolutionStatus::Resolved ||
      rightResolved.status != MissingPathResolutionStatus::Resolved)
    return AliasCheck::Error;
  const PathRelation parentIdentity = reader.compareFileIdentity(
      leftResolved.physicalParent, rightResolved.physicalParent);
  if (parentIdentity == PathRelation::Different)
    return AliasCheck::Distinct;
  if (parentIdentity != PathRelation::Same)
    return AliasCheck::Error;
  if (leftResolved.nameComparisonPolicy !=
          rightResolved.nameComparisonPolicy ||
      leftResolved.nameComparisonPolicy == PathNameComparisonPolicy::Unknown ||
      leftResolved.nameComparisonPolicy == PathNameComparisonPolicy::Error)
    return AliasCheck::Error;
  if (leftResolved.suffix.size() != rightResolved.suffix.size())
    return AliasCheck::Distinct;
  for (std::size_t index = 0; index < leftResolved.suffix.size(); ++index) {
    if (!equivalentPathComponent(leftResolved.suffix[index],
                                 rightResolved.suffix[index],
                                 leftResolved.nameComparisonPolicy))
      return AliasCheck::Distinct;
  }
  return AliasCheck::Aliases;
}

AliasCheck comparePathAliases(const QString& left, const QString& right,
                              const PathMetadataReader& reader) noexcept {
  if (sameNormalizedAbsolutePath(left, right)) return AliasCheck::Aliases;
  const PathRelation fileIdentity = reader.compareFileIdentity(left, right);
  if (fileIdentity == PathRelation::Same) return AliasCheck::Aliases;
  if (fileIdentity == PathRelation::Error) return AliasCheck::Error;
  if (fileIdentity == PathRelation::Missing) {
    try {
      return compareMissingPathAliases(left, right, reader);
    } catch (...) {
      return AliasCheck::Error;
    }
  }
  const PathRelation canonical = reader.compareCanonicalPaths(left, right);
  if (canonical == PathRelation::Same) return AliasCheck::Aliases;
  if (canonical == PathRelation::Error) return AliasCheck::Error;
  return AliasCheck::Distinct;
}

AliasCheck destinationAliasesLinkedSource(
    const QString& destination, const ProductDocument& product,
    const QString& projectPath, const PathMetadataReader& reader) {
  const QDir root(QFileInfo(projectPath).absolutePath());
  for (const DefinitionId id : product.definitions().definitionIds()) {
    const auto* binding = product.partLinkedBinding(id);
    if (!binding) continue;
    const QString locator = QString::fromUtf8(
        binding->relativePathHintUtf8.data(),
        static_cast<qsizetype>(binding->relativePathHintUtf8.size()));
    const QString authoredTarget = root.filePath(locator);
    // The authored path itself is reserved even while Missing. Otherwise a
    // same-directory Save As could create an assembly at the component path
    // and silently convert a diagnostic missing link into self-reference.
    const AliasCheck relation =
        comparePathAliases(destination, authoredTarget, reader);
    if (relation != AliasCheck::Distinct) return relation;
  }
  return AliasCheck::Distinct;
}

bool sameProjectDirectory(const QString& left, const QString& right) {
  const QString a = QDir::cleanPath(QFileInfo(left).absolutePath());
  const QString b = QDir::cleanPath(QFileInfo(right).absolutePath());
#if defined(Q_OS_WIN)
  return a.compare(b, Qt::CaseInsensitive) == 0;
#else
  return a == b;
#endif
}

ProjectApplicationStatus strictLinkedPreflight(
    const ProductDocument& product, const QString& projectPath,
    std::vector<project::ResolvedLinkedPartSource>* sources = nullptr,
    std::optional<project::LinkedPartSourceResolveResult> injected =
        std::nullopt) {
  auto descriptors = linkedDescriptors(product);
  if (descriptors.empty()) return {};
  auto resolved = injected
      ? std::move(*injected)
      : project::resolveLinkedPartSourcesNative(
            descriptors, linkedPolicyForProject(projectPath));
  if (!resolved.succeeded())
    return failure(ProjectApplicationErrorCode::InvalidProject,
                   resolved.error.isEmpty()
                       ? QStringLiteral("linked source preflight failed")
                       : std::move(resolved.error));
  if (sources) *sources = std::move(resolved.sources);
  return {};
}

ProjectApplicationStatus crossDirectorySaveAsPreflight(
    const ProductDocument& product, const QString& sourcePath,
    const QString& targetPath, const PathMetadataReader& reader) {
  if (sameProjectDirectory(sourcePath, targetPath)) return {};
  std::vector<project::ResolvedLinkedPartSource> oldSources;
  std::vector<project::ResolvedLinkedPartSource> newSources;
  auto oldStatus = strictLinkedPreflight(product, sourcePath, &oldSources);
  if (!oldStatus.succeeded()) return oldStatus;
  auto newStatus = strictLinkedPreflight(product, targetPath, &newSources);
  if (!newStatus.succeeded()) return newStatus;
  if (oldSources.size() != newSources.size())
    return failure(ProjectApplicationErrorCode::InvalidProject,
                   QStringLiteral("linked source set changed across Save As"));
  for (std::size_t index = 0; index < oldSources.size(); ++index) {
    if (oldSources[index].key != newSources[index].key ||
        oldSources[index].contentHash != newSources[index].contentHash)
      return failure(
          ProjectApplicationErrorCode::InvalidProject,
          QStringLiteral("linked source identity changes across Save As"));
    const AliasCheck sourceRelationship = comparePathAliases(
        oldSources[index].canonicalPath, newSources[index].canonicalPath,
        reader);
    if (sourceRelationship != AliasCheck::Aliases)
      return failure(
          ProjectApplicationErrorCode::InvalidProject,
          sourceRelationship == AliasCheck::Error
              ? QStringLiteral(
                    "linked source metadata query failed across Save As")
              : QStringLiteral("linked source identity changes across Save As"));
  }
  return {};
}

std::optional<project::ProductEnvelopeV5Metadata> metadataFor(
    const ProductDocumentIdentity& identity) {
  const auto name = exactQStringFromUtf8(identity.metadata.nameUtf8);
  const auto createdAt = exactQStringFromUtf8(identity.metadata.createdAtUtf8);
  if (!name || !createdAt) return std::nullopt;
  return project::ProductEnvelopeV5Metadata{*name, *createdAt};
}

project::detail::ProjectAtomicWriteResult writeAssemblyAtomically(
    const QString& path, const ProductDocument& product, ProjectId projectId,
    const project::ProductEnvelopeV5Metadata& metadata,
    project::ProjectPersistenceState persistenceState) {
  switch (persistenceState.format) {
    case project::ProjectPersistenceFormat::V7:
      return project::detail::writeProductEnvelopeV7Atomically(
          path, product, projectId, metadata);
    case project::ProjectPersistenceFormat::V8:
      return project::detail::writeProductEnvelopeV8Atomically(
          path, product, projectId, metadata);
    case project::ProjectPersistenceFormat::V9:
      return project::detail::writeProductEnvelopeV9Atomically(
          path, product, projectId, metadata);
    default:
      return {project::detail::ProjectAtomicWriteFailure::Encode,
              QStringLiteral("assembly format is not writable")};
  }
}

project::detail::ProjectAtomicWriteResult writeComponentAtomically(
    const QString& path, const ProductDocument& product, ProjectId projectId,
    const project::ProductEnvelopeV5Metadata& metadata,
    project::ProjectPersistenceState currentPersistence) {
  bool hasDrawings = false;
  for (const DefinitionId id : product.definitions().definitionIds()) {
    const auto* component = product.definitions().findComponent(id);
    if (component && !component->drawings().empty()) {
      hasDrawings = true;
      break;
    }
  }
  bool hasIndependentHomePlacement = false;
  for (const DefinitionId id : product.definitions().definitionIds()) {
    const auto* component = product.definitions().findComponent(id);
    if (!component) continue;
    for (const auto& [occurrenceId, occurrence] : component->occurrences()) {
      static_cast<void>(occurrenceId);
      if (occurrence.homePlacement() != occurrence.placement()) {
        hasIndependentHomePlacement = true;
        break;
      }
    }
    if (hasIndependentHomePlacement) break;
  }
  const auto target =
      currentPersistence.format == project::ProjectPersistenceFormat::V12 ||
              hasIndependentHomePlacement
          ? project::ProjectPersistenceFormat::V12
          :
      currentPersistence.format == project::ProjectPersistenceFormat::V11 ||
              hasDrawings
          ? project::ProjectPersistenceFormat::V11
          : project::ProjectPersistenceFormat::V10;
  if (target == project::ProjectPersistenceFormat::V12)
    return project::detail::writeProductEnvelopeV12Atomically(
        path, product, projectId, metadata);
  if (target == project::ProjectPersistenceFormat::V11)
    return project::detail::writeProductEnvelopeV11Atomically(
        path, product, projectId, metadata);
  return project::detail::writeProductEnvelopeV10Atomically(
      path, product, projectId, metadata);
}

ProjectApplicationStatus failure(ProjectApplicationErrorCode code,
                                 QString detail) {
  return {code, std::move(detail)};
}

ProjectApplicationStatus unexpectedFailure(const char* operation,
                                            const std::exception& exception) {
  return failure(
      ProjectApplicationErrorCode::UnexpectedFailure,
      QString::fromUtf8("%1: %2")
          .arg(QString::fromUtf8(operation),
               QString::fromUtf8(exception.what())));
}

ProjectApplicationStatus unknownFailure(const char* operation) {
  return failure(ProjectApplicationErrorCode::UnexpectedFailure,
                 QString::fromUtf8("%1: неизвестная ошибка.")
                     .arg(QString::fromUtf8(operation)));
}

template <typename Publisher>
ProjectApplicationStatus convertStandaloneToProductImpl(
    ProductDocument& source,
    const StandaloneProductConversionRequest& request,
    const StandaloneProductConversionPrecondition& precondition,
    Publisher&& publisher) noexcept {
  try {
    if (precondition.historicalLegacyExtrusionSourceSketchId) {
      return failure(
          ProjectApplicationErrorCode::InvalidProject,
          QStringLiteral(
              "Historical v1 extrusion must be converted to a modern model "
              "feature before product conversion."));
    }
    const project::ProductEnvelopeV5Metadata metadata{request.name,
                                                       request.createdAt};
    auto prepared = project::prepareStandaloneProductEnvelopeV5Conversion(
        source, request.outputProjectId, request.outputRootDefinitionId,
        metadata);
    if (!prepared.succeeded()) {
      const auto code =
          prepared.status ==
                  project::ProductEnvelopeV5ConversionStatus::PayloadTooLarge
              ? ProjectApplicationErrorCode::SaveFailed
              : ProjectApplicationErrorCode::InvalidProject;
      return failure(code, std::move(prepared.error));
    }

    auto scanned = project::scanProductEnvelopeV5(
        QByteArrayView(prepared.prepared->payload.bytes));
    if (scanned.status != project::ProductEnvelopeV5ScanStatus::Candidate ||
        !scanned.candidate) {
      const auto code =
          scanned.status == project::ProductEnvelopeV5ScanStatus::Unsupported
              ? ProjectApplicationErrorCode::UnsupportedProject
              : ProjectApplicationErrorCode::InvalidProject;
      return failure(code, std::move(scanned.error));
    }

    auto materialized =
        project::materializeProductEnvelopeV5(*scanned.candidate);
    if (!materialized.succeeded()) {
      const auto code = materialized.status ==
                                project::ProductEnvelopeV5MaterializationStatus::
                                    Unsupported
                            ? ProjectApplicationErrorCode::UnsupportedProject
                            : ProjectApplicationErrorCode::InvalidProject;
      return failure(code, std::move(materialized.error));
    }

    const auto published = publisher(
        QByteArrayView(prepared.prepared->payload.bytes));
    if (!published.succeeded()) {
      return failure(
          ProjectApplicationErrorCode::SaveFailed,
          published.error.isEmpty()
              ? QStringLiteral("Failed to publish prepared product payload.")
              : std::move(published.error));
    }

    const auto committed = source.commitPreparedStandaloneConversion(
        std::move(*materialized.product));
    if (committed != ProductDocumentStandaloneConversionStatus::Committed) {
      return failure(
          ProjectApplicationErrorCode::UnexpectedFailure,
          QStringLiteral(
              "Prepared product was published but its model commit failed."));
    }
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка преобразования продукта", exception);
  } catch (...) {
    return unknownFailure("Ошибка преобразования продукта");
  }
}

template <typename Writer>
ProjectApplicationStatus saveProductAsCloneImpl(
    ProductDocument& source, const ProductCloneRequest& request,
    Writer&& writer) noexcept {
  try {
    if (!source.identity() || !source.identity()->projectId.isValid() ||
        !source.hasSingleValidRootDefinition() ||
        source.rootDefinitionId().value() >
            static_cast<std::uint64_t>(
                project::ProjectFile::kMaximumPersistedId) ||
        !request.outputProjectId.isValid() ||
        request.outputProjectId == source.identity()->projectId) {
      return failure(ProjectApplicationErrorCode::InvalidProject,
                     QStringLiteral(
                         "Product clone request or source is invalid."));
    }

    auto nameUtf8 = exactUtf8FromQString(request.name);
    auto createdAtUtf8 = exactUtf8FromQString(request.createdAt);
    if (!nameUtf8 || !createdAtUtf8) {
      return failure(
          ProjectApplicationErrorCode::InvalidProject,
          QStringLiteral("Product clone metadata is not exactly representable."));
    }

    // Prepare both sides of the guarded identity transition before opening a
    // transaction. Publication uses the same metadata values and existing v5
    // writer; no second serialization path is introduced here.
    const ProductDocumentIdentity expectedIdentity = *source.identity();
    ProductDocumentIdentity replacementIdentity{
        request.outputProjectId,
        {std::move(*nameUtf8), std::move(*createdAtUtf8)}};
    const project::ProductEnvelopeV5Metadata metadata{request.name,
                                                       request.createdAt};

    const auto written = writer(metadata);
    if (!written.succeeded()) {
      return failure(
          ProjectApplicationErrorCode::SaveFailed,
          written.error.isEmpty()
              ? QStringLiteral("Failed to publish product clone.")
              : std::move(written.error));
    }

    const auto committed = source.commitForkIdentity(
        expectedIdentity, std::move(replacementIdentity));
    if (committed != ProductDocumentForkIdentityStatus::Committed) {
      return failure(
          ProjectApplicationErrorCode::UnexpectedFailure,
          QStringLiteral(
              "Product clone was published but its identity commit failed."));
    }
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка клонирования продукта", exception);
  } catch (...) {
    return unknownFailure("Ошибка клонирования продукта");
  }
}

ProjectApplicationErrorCode relocationErrorCode(
    project::ProductEnvelopeV5RelocationStatus status) {
  using Status = project::ProductEnvelopeV5RelocationStatus;
  switch (status) {
    case Status::Relocated:
      return ProjectApplicationErrorCode::None;
    case Status::InvalidProduct:
    case Status::InvalidPath:
      return ProjectApplicationErrorCode::RelocateNotEligible;
    case Status::SameFile:
      return ProjectApplicationErrorCode::RelocateSameFile;
    case Status::DestinationExists:
      return ProjectApplicationErrorCode::RelocateDestinationExists;
    case Status::SourceNotRegular:
    case Status::AmbiguousPath:
    case Status::MultipleLinks:
      return ProjectApplicationErrorCode::RelocateAmbiguousFileIdentity;
    case Status::CrossVolume:
      return ProjectApplicationErrorCode::RelocateCrossVolume;
    case Status::SourceMissing:
    case Status::SourceChanged:
      return ProjectApplicationErrorCode::RelocateSourceChanged;
    case Status::DestinationChanged:
      return ProjectApplicationErrorCode::RelocateDestinationChanged;
    case Status::UnsupportedFilesystem:
      return ProjectApplicationErrorCode::RelocateUnsupportedFilesystem;
    case Status::RenameFailed:
      return ProjectApplicationErrorCode::RelocateFailed;
  }
  return ProjectApplicationErrorCode::RelocateFailed;
}

template <typename Relocator>
ProjectApplicationStatus relocateProductImpl(
    const ProductDocument& source,
    const ProductRelocationPrecondition& precondition,
    Relocator&& relocator) noexcept {
  try {
    if (!precondition.sourceIsClean ||
        precondition.historicalLegacyExtrusionSourceSketchId ||
        !source.identity() || !source.identity()->projectId.isValid() ||
        !source.hasSingleValidRootDefinition()) {
      return failure(ProjectApplicationErrorCode::RelocateNotEligible,
                     QStringLiteral(
                         "Only a clean identified single-root product can be relocated."));
    }
    auto relocated = relocator();
    if (!relocated.succeeded())
      return failure(relocationErrorCode(relocated.status),
                     std::move(relocated.error));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка перемещения документа изделия",
                             exception);
  } catch (...) {
    return unknownFailure("Ошибка перемещения документа изделия");
  }
}

SketchPlacement legacyV1SketchPlacement(const QString& support,
                                         const BoxParameters& box) {
  if (support.contains(QStringLiteral("XZ")) ||
      support.contains(QString::fromUtf8("Передняя")) ||
      support.contains(QString::fromUtf8("Задняя"))) {
    auto placement = SketchPlacement::xz();
    if (support.contains(QString::fromUtf8("Передняя")))
      placement.origin.y = -box.depthMm * 0.5;
    else if (support.contains(QString::fromUtf8("Задняя")))
      placement.origin.y = box.depthMm * 0.5;
    return placement;
  }
  if (support.contains(QStringLiteral("YZ")) ||
      support.contains(QString::fromUtf8("Правая")) ||
      support.contains(QString::fromUtf8("Левая"))) {
    auto placement = SketchPlacement::yz();
    if (support.contains(QString::fromUtf8("Правая")))
      placement.origin.x = box.widthMm * 0.5;
    else if (support.contains(QString::fromUtf8("Левая")))
      placement.origin.x = -box.widthMm * 0.5;
    return placement;
  }
  auto placement = SketchPlacement::xy();
  if (support.contains(QString::fromUtf8("Верхняя")))
    placement.origin.z = box.heightMm;
  return placement;
}

}  // namespace

namespace application::detail {

StagedApplicationDocumentResult stageAssemblyOpenWithResolutionForTesting(
    std::unique_ptr<ProductDocument> product,
    project::ProjectPersistenceFormat format,
    project::LinkedPartSourceResolveStatus status,
    std::size_t failureIndex, QString detail) {
  project::LinkedPartSourceResolveResult injected;
  injected.status = status;
  injected.failureIndex = failureIndex;
  injected.error = std::move(detail);
  return finishResolvedAssembly(std::move(product), format,
                                std::move(injected), QString{});
}

ProjectApplicationStatus preflightFullProductExportWithResolutionForTesting(
    const ProductDocument& product,
    project::LinkedPartSourceResolveStatus status,
    std::size_t failureIndex, QString detail) {
  project::LinkedPartSourceResolveResult injected;
  injected.status = status;
  injected.failureIndex = failureIndex;
  injected.error = std::move(detail);
  return strictLinkedPreflight(product, QString{}, nullptr,
                               std::move(injected));
}

}  // namespace application::detail

StagedApplicationDocument::StagedApplicationDocument(
    ProductDocument&& productDocument,
    std::optional<SketchId> historicalLegacyExtrusionSourceSketchId,
    std::optional<project::ProjectPersistenceState> persistenceState)
    : historicalLegacyExtrusionSourceSketchId(
          historicalLegacyExtrusionSourceSketchId),
      persistenceState(persistenceState),
      productDocument_(std::move(productDocument)) {}

StagedApplicationDocument::StagedApplicationDocument(
    Document&& document,
    std::optional<SketchId> historicalLegacyExtrusionSourceSketchId,
    std::optional<project::ProjectPersistenceState> persistenceState)
    : StagedApplicationDocument(
          makeTransientProductDocument(std::move(document)),
          historicalLegacyExtrusionSourceSketchId, persistenceState) {}

ProjectApplicationStatus ProjectApplicationService::createProject(
    const QString& path) const noexcept {
  try {
    QString detail;
    if (!project::ProjectFile::create(path, &detail))
      return failure(ProjectApplicationErrorCode::CreateFailed,
                     std::move(detail));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка создания проекта", exception);
  } catch (...) {
    return unknownFailure("Ошибка создания проекта");
  }
}

StagedApplicationDocumentResult ProjectApplicationService::stageOpen(
    const QString& path, ProductV5OpenPolicy productV5Policy) const noexcept {
  try {
    const ProjectEnvelopeProbe probe = probeProjectEnvelope(path);
    if (probe.dispatch == ProjectEnvelopeDispatch::InvalidSolidarHeader)
      return invalidStage(probe.error);
    if (probe.dispatch == ProjectEnvelopeDispatch::ProductV6) {
      if (!probe.payload)
        return invalidStage(QStringLiteral("failed to read v6 project"));
      const auto scanned =
          project::scanProductEnvelopeV6(QByteArrayView(*probe.payload));
      if (scanned.status == project::ProductEnvelopeV6ScanStatus::Invalid ||
          scanned.status == project::ProductEnvelopeV6ScanStatus::NotApplicable)
        return invalidStage(std::move(scanned.error));
      // The compatibility API is intentionally part-only. Even a fully valid
      // V6 assembly is never projected to one of its embedded definitions.
      return unsupportedStage(
          scanned.error.isEmpty()
              ? QStringLiteral("assembly projects require stageOpenProduct")
              : std::move(scanned.error));
    }
    if (probe.dispatch == ProjectEnvelopeDispatch::ProductV7 ||
        probe.dispatch == ProjectEnvelopeDispatch::ProductV8 ||
        probe.dispatch == ProjectEnvelopeDispatch::ProductV9 ||
        probe.dispatch == ProjectEnvelopeDispatch::ProductV10 ||
        probe.dispatch == ProjectEnvelopeDispatch::ProductV11 ||
        probe.dispatch == ProjectEnvelopeDispatch::ProductV12 ||
        probe.dispatch == ProjectEnvelopeDispatch::Future)
      return unsupportedStage(QStringLiteral(
          "product projects require stageOpenProduct"));
    if (probe.dispatch == ProjectEnvelopeDispatch::ProductV5 &&
        productV5Policy == ProductV5OpenPolicy::Allow) {
      if (!probe.payload)
        return invalidStage(QStringLiteral("failed to read v5 project"));
      return stageProductV5Payload(QByteArrayView(*probe.payload));
    }

    auto loaded = project::ProjectFile::stageLoad(path);
    if (!loaded.succeeded()) {
      const auto code = loaded.status == project::ProjectLoadStatus::Unsupported
                            ? ProjectApplicationErrorCode::UnsupportedProject
                            : ProjectApplicationErrorCode::InvalidProject;
      return {{code, std::move(loaded.error)}, std::nullopt};
    }

    Document candidate;
    std::optional<SketchId> historicalLegacyExtrusionSourceSketchId;
    switch (project::classifyPartPayload(loaded.payloadKind)) {
      case project::PartPayloadDisposition::Parametric:
        if (!loaded.document) {
          return {{ProjectApplicationErrorCode::InvalidProject,
                   QString::fromUtf8(
                       "Проект v2/v3/v4 не содержит восстановленного документа.")},
                  std::nullopt};
        }
        candidate = std::move(*loaded.document);
        break;
      case project::PartPayloadDisposition::Legacy: {
        candidate.setBox(loaded.legacy.box);
        // v1 does not persist IDs. Use deterministic local IDs while explicit
        // reservation is paused so speculative staging cannot consume process-
        // global edit IDs. The commit boundary reserves them atomically later.
        detail::ScopedExplicitIdReservationPause reservationPause;
        for (std::size_t index = 0; index < loaded.legacy.sketches.size();
             ++index) {
          const auto& saved = loaded.legacy.sketches[index];
          auto& modelSketch = candidate.addSketch(
              static_cast<SketchId>(index + 1), "Loaded sketch", {});
          modelSketch.geometry = saved.geometry;
          modelSketch.placement =
              legacyV1SketchPlacement(saved.support, loaded.legacy.box);
        }
        if (loaded.legacy.hasExtrusion &&
            loaded.legacy.extrusionSourceSketch &&
            *loaded.legacy.extrusionSourceSketch < candidate.sketches().size()) {
          historicalLegacyExtrusionSourceSketchId =
              candidate.sketches()[*loaded.legacy.extrusionSourceSketch]
                  .id;
        }
        break;
      }
      case project::PartPayloadDisposition::Unsupported:
      default:
        return {{ProjectApplicationErrorCode::UnsupportedProject,
                 QString::fromUtf8(
                     "Проект не содержит поддерживаемых данных детали.")},
                std::nullopt};
    }
    return {{}, StagedApplicationDocument(
                    std::move(candidate),
                    historicalLegacyExtrusionSourceSketchId,
                    loaded.persistenceState)};
  } catch (const std::exception& exception) {
    return {unexpectedFailure("Ошибка подготовки проекта", exception),
            std::nullopt};
  } catch (...) {
    return {unknownFailure("Ошибка подготовки проекта"), std::nullopt};
  }
}

StagedApplicationDocumentResult ProjectApplicationService::stageOpenProduct(
    const QString& path) const noexcept {
  try {
    const ProjectEnvelopeProbe probe = probeProjectEnvelope(path);
    switch (probe.dispatch) {
      case ProjectEnvelopeDispatch::InvalidSolidarHeader:
        return invalidStage(probe.error);
      case ProjectEnvelopeDispatch::ProductV6:
        if (!probe.payload)
          return invalidStage(QStringLiteral("failed to read v6 project"));
        return stageProductV6Payload(QByteArrayView(*probe.payload));
      case ProjectEnvelopeDispatch::ProductV7:
        if (!probe.payload)
          return invalidStage(QStringLiteral("failed to read v7 project"));
        return stageProductV7Payload(QByteArrayView(*probe.payload), path);
      case ProjectEnvelopeDispatch::ProductV8:
        if (!probe.payload)
          return invalidStage(QStringLiteral("failed to read v8 project"));
        return stageProductV8Payload(QByteArrayView(*probe.payload), path);
      case ProjectEnvelopeDispatch::ProductV9:
        if (!probe.payload)
          return invalidStage(QStringLiteral("failed to read v9 project"));
        return stageProductV9Payload(QByteArrayView(*probe.payload), path);
      case ProjectEnvelopeDispatch::ProductV10:
        if (!probe.payload)
          return invalidStage(QStringLiteral("failed to read v10 project"));
        return stageProductV10Payload(QByteArrayView(*probe.payload));
      case ProjectEnvelopeDispatch::ProductV11:
        if (!probe.payload)
          return invalidStage(QStringLiteral("failed to read v11 project"));
        return stageProductV11Payload(QByteArrayView(*probe.payload));
      case ProjectEnvelopeDispatch::ProductV12:
        if (!probe.payload)
          return invalidStage(QStringLiteral("failed to read v12 project"));
        return stageProductV12Payload(QByteArrayView(*probe.payload));
      case ProjectEnvelopeDispatch::Future:
        return unsupportedStage(QStringLiteral(
            "project version is newer than this application"));
      case ProjectEnvelopeDispatch::ProductV5:
        if (!probe.payload)
          return invalidStage(QStringLiteral("failed to read v5 project"));
        return stageProductV5Payload(QByteArrayView(*probe.payload));
      case ProjectEnvelopeDispatch::LegacyPart:
        return stageOpen(path, ProductV5OpenPolicy::Disallow);
    }
    return invalidStage(QStringLiteral("unknown project dispatch"));
  } catch (const std::exception& exception) {
    return {unexpectedFailure("Ошибка подготовки изделия", exception),
            std::nullopt};
  } catch (...) {
    return {unknownFailure("Ошибка подготовки изделия"), std::nullopt};
  }
}

StagedApplicationDocumentResult
ProjectApplicationService::stageNewAssembly() const noexcept {
  try {
    constexpr DefinitionId rootId{1};
    DefinitionRegistry definitions;
    AssemblyDefinition root(rootId);
    if (definitions.insert(std::move(root)) !=
        DefinitionInsertStatus::Inserted)
      return invalidStage(QStringLiteral("failed to create assembly root"));
    auto created = ProductDocument::create(std::move(definitions), rootId);
    if (!created)
      return invalidStage(QStringLiteral("failed to create assembly product"));
    return {{}, StagedApplicationDocument(
                    std::move(*created.product), std::nullopt,
                    project::ProjectPersistenceState{
                        project::ProjectPersistenceFormat::V7})};
  } catch (const std::exception& exception) {
    return {unexpectedFailure("Ошибка создания сборки", exception),
            std::nullopt};
  } catch (...) {
    return {unknownFailure("Ошибка создания сборки"), std::nullopt};
  }
}

StagedEmbeddedPartResult ProjectApplicationService::stageEmbeddedPart(
    const QString& path) const noexcept {
  try {
    auto opened = stageOpenProduct(path);
    if (!opened.succeeded()) return {std::move(opened.status), std::nullopt};
    if (opened.staged->rootDefinitionKind() != DefinitionKind::Part ||
        opened.staged->productDocument().definitions().size() != 1 ||
        opened.staged->historicalLegacyExtrusionSourceSketchId) {
      return {failure(ProjectApplicationErrorCode::UnsupportedProject,
                      QStringLiteral("embedded source must be one modern part")),
              std::nullopt};
    }

    const Document& source = opened.staged->rootDocument();
    const auto encoded = project::detail::encodeProjectFormatV4DocumentRoot(
        source, {QStringLiteral("Embedded component"), QString{}});
    if (!encoded.succeeded()) {
      return {failure(ProjectApplicationErrorCode::InvalidProject,
                      encoded.error.isEmpty()
                          ? QStringLiteral("failed to author embedded part")
                          : encoded.error),
              std::nullopt};
    }
    const QByteArray cold =
        QJsonDocument(*encoded.root).toJson(QJsonDocument::Compact);
    ProductDocument product =
        std::move(*opened.staged).releaseProductDocument();
    auto document = std::move(product).releaseRootDocument();
    if (!document || cold.isEmpty())
      return {failure(ProjectApplicationErrorCode::InvalidProject,
                      QStringLiteral("embedded source is empty")),
              std::nullopt};
    return {{}, StagedEmbeddedPart{std::move(document), cold}};
  } catch (const std::exception& exception) {
    return {unexpectedFailure("Ошибка подготовки компонента", exception),
            std::nullopt};
  } catch (...) {
    return {unknownFailure("Ошибка подготовки компонента"), std::nullopt};
  }
}

ProjectApplicationStatus ProjectApplicationService::save(
    const QString& path, const Document& document) const noexcept {
  return save(path, document, {project::ProjectPersistenceFormat::V3});
}

ProjectApplicationStatus ProjectApplicationService::convertStandaloneToProduct(
    const QString& targetPath, ProductDocument& source,
    const StandaloneProductConversionRequest& request,
    const StandaloneProductConversionPrecondition& precondition)
    const noexcept {
  return convertStandaloneToProductImpl(
      source, request, precondition, [&targetPath](QByteArrayView bytes) {
        return project::detail::publishProjectPayloadAtomically(targetPath,
                                                                 bytes);
      });
}

ProjectApplicationStatus ProjectApplicationService::saveProductAsClone(
    const QString& targetPath, ProductDocument& source,
    const ProductCloneRequest& request) const noexcept {
  return saveProductAsCloneImpl(
      source, request,
      [&targetPath, &source,
       &request](const project::ProductEnvelopeV5Metadata& metadata) {
        return project::detail::writeProductEnvelopeV5Atomically(
            targetPath, source, request.outputProjectId, metadata);
      });
}

ProjectApplicationStatus ProjectApplicationService::relocateProduct(
    const QString& sourcePath, const QString& targetPath,
    const ProductDocument& source,
    const ProductRelocationPrecondition& precondition) const noexcept {
  return relocateProductImpl(
      source, precondition,
      [&sourcePath, &targetPath, &source] {
        return project::relocateProductEnvelopeV5(sourcePath, targetPath,
                                                  source);
      });
}

ProjectApplicationStatus ProjectApplicationService::save(
    const QString& path, const Document& document,
    project::ProjectPersistenceState persistenceState) const noexcept {
  try {
    QString detail;
    if (!project::ProjectFile::saveDocument(path, document, persistenceState,
                                            &detail))
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     std::move(detail));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка сохранения проекта", exception);
  } catch (...) {
    return unknownFailure("Ошибка сохранения проекта");
  }
}

ProjectApplicationStatus ProjectApplicationService::save(
    const QString& path,
    const ProductDocument& productDocument) const noexcept {
  try {
    if (productDocument.rootDefinitionKind() != DefinitionKind::Part) {
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "Assembly-root products are not writable in v5."));
    }
    if (!productDocument.identity()) {
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "ProductDocument v5 requires an identity bundle."));
    }
    if (productDocument.definitions().size() != 1) {
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "ProductDocument v5 requires one definition."));
    }

    const ProductDocumentIdentity& identity = *productDocument.identity();
    const auto name = exactQStringFromUtf8(identity.metadata.nameUtf8);
    const auto createdAt =
        exactQStringFromUtf8(identity.metadata.createdAtUtf8);
    if (!name || !createdAt) {
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "ProductDocument metadata is not exact UTF-8."));
    }
    const project::ProductEnvelopeV5Metadata metadata{*name, *createdAt};
    const auto written = project::detail::writeProductEnvelopeV5Atomically(
        path, productDocument, identity.projectId, metadata);
    if (!written.succeeded()) {
      return failure(
          ProjectApplicationErrorCode::SaveFailed,
          written.error.isEmpty()
              ? QStringLiteral("Failed to atomically save product v5.")
              : std::move(written.error));
    }
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка сохранения продукта", exception);
  } catch (...) {
    return unknownFailure("Ошибка сохранения продукта");
  }
}

ProjectApplicationStatus ProjectApplicationService::saveAssemblyV7(
    const QString& path, ProductDocument& productDocument,
    std::optional<ProductDocumentIdentity> firstSaveIdentity) const noexcept {
  return saveAssembly(path, productDocument,
                      {project::ProjectPersistenceFormat::V7},
                      std::move(firstSaveIdentity));
}

ProjectApplicationStatus ProjectApplicationService::saveAssembly(
    const QString& path, ProductDocument& productDocument,
    project::ProjectPersistenceState persistenceState,
    std::optional<ProductDocumentIdentity> firstSaveIdentity) const noexcept {
  try {
    if (path.isEmpty() || invalidRawDestinationPath(path) ||
        productDocument.rootDefinitionKind() != DefinitionKind::Assembly ||
        !project::isWritableAssemblyPersistenceFormat(
            persistenceState.format))
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "writable assembly save requires V7/V8/V9 and a path"));
    const AliasCheck aliases = destinationAliasesLinkedSource(
        path, productDocument, path,
        nativeAssemblySaveAsPathMetadataReader());
    if (aliases != AliasCheck::Distinct)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     aliases == AliasCheck::Error
                         ? QStringLiteral(
                               "linked source metadata query failed")
                         : QStringLiteral(
                               "assembly destination aliases a linked source"));

    const bool needsIdentity = !productDocument.identity().has_value();
    if (needsIdentity && !firstSaveIdentity)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral("first assembly save requires identity"));
    const ProductDocumentIdentity identity = needsIdentity
        ? *firstSaveIdentity
        : *productDocument.identity();
    if (!identity.projectId.isValid())
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral("assembly identity is invalid"));
    const auto metadata = metadataFor(identity);
    if (!metadata)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral("assembly metadata is not exact UTF-8"));
    const auto written = writeAssemblyAtomically(
        path, productDocument, identity.projectId, *metadata,
        persistenceState);
    if (!written.succeeded())
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     std::move(written.error));
    if (needsIdentity &&
        !productDocument.commitProductIdentity(std::move(*firstSaveIdentity)))
      return failure(ProjectApplicationErrorCode::UnexpectedFailure,
                     QStringLiteral(
                          "published assembly identity could not be committed"));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка сохранения сборки", exception);
  } catch (...) {
    return unknownFailure("Ошибка сохранения сборки");
  }
}

ProjectApplicationStatus ProjectApplicationService::saveAssemblyV7As(
    const QString& targetPath, ProductDocument& productDocument,
    const AssemblySaveAsRequest& request) const noexcept {
  return saveAssemblyAs(targetPath, productDocument,
                        {project::ProjectPersistenceFormat::V7}, request);
}

ProjectApplicationStatus ProjectApplicationService::saveAssemblyAs(
    const QString& targetPath, ProductDocument& productDocument,
    project::ProjectPersistenceState persistenceState,
    const AssemblySaveAsRequest& request) const noexcept {
  return application::detail::saveAssemblyAs(
      targetPath, productDocument, persistenceState, request,
      nativeAssemblySaveAsPathMetadataReader());
}

ProjectApplicationStatus ProjectApplicationService::saveComponent(
    const QString& path, ProductDocument& productDocument,
    std::optional<ProductDocumentIdentity> firstSaveIdentity,
    project::ProjectPersistenceState currentPersistence) const noexcept {
  try {
    if (path.isEmpty() || invalidRawDestinationPath(path) ||
        productDocument.rootDefinitionKind() != DefinitionKind::Component ||
        !project::isWritableComponentPersistenceFormat(
            currentPersistence.format))
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "writable component save requires a component root and path"));
    const bool needsIdentity = !productDocument.identity().has_value();
    if (needsIdentity && !firstSaveIdentity)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral("first component save requires identity"));
    if (needsIdentity) {
      auto transition = ComponentIdentityTransition::prepareFirstSave(
          productDocument, std::move(*firstSaveIdentity));
      if (!transition)
        return failure(ProjectApplicationErrorCode::SaveFailed,
                       QStringLiteral(
                           "component first-save identity transition is invalid"));
      return saveComponentIdentityTransition(path, productDocument,
                                             *transition.transition,
                                             currentPersistence);
    }
    const ProductDocumentIdentity identity = *productDocument.identity();
    if (!identity.projectId.isValid())
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral("component identity is invalid"));
    const auto metadata = metadataFor(identity);
    if (!metadata)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral("component metadata is not exact UTF-8"));
    const auto written = writeComponentAtomically(
        path, productDocument, identity.projectId, *metadata,
        currentPersistence);
    if (!written.succeeded())
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     std::move(written.error));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка сохранения компонента", exception);
  } catch (...) {
    return unknownFailure("Ошибка сохранения компонента");
  }
}

ProjectApplicationStatus
ProjectApplicationService::saveComponentIdentityTransition(
    const QString& path, ProductDocument& productDocument,
    const ComponentIdentityTransition& transition,
    project::ProjectPersistenceState currentPersistence) const noexcept {
  try {
    if (path.isEmpty() || invalidRawDestinationPath(path) ||
        transition.kind() != ComponentIdentityTransitionKind::FirstSave ||
        !transition.isCurrentSource(productDocument) ||
        !project::isWritableComponentPersistenceFormat(
            currentPersistence.format))
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "component first-save transition is stale or invalid"));
    ProductDocumentIdentity target = transition.targetIdentity();
    const auto metadata = metadataFor(target);
    if (!metadata)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral("component metadata is not exact UTF-8"));
    const auto written = writeComponentAtomically(
        path, productDocument, target.projectId, *metadata,
        currentPersistence);
    if (!written.succeeded())
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     std::move(written.error));
    if (!productDocument.commitProductIdentity(std::move(target)))
      return failure(ProjectApplicationErrorCode::UnexpectedFailure,
                     QStringLiteral(
                         "sealed component identity could not be committed"));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка сохранения компонента", exception);
  } catch (...) {
    return unknownFailure("Ошибка сохранения компонента");
  }
}

ProjectApplicationStatus ProjectApplicationService::saveComponentAs(
    const QString& targetPath, ProductDocument& productDocument,
    const ComponentSaveAsRequest& request,
    project::ProjectPersistenceState currentPersistence) const noexcept {
  try {
    if (!productDocument.identity() || request.sourcePath.isEmpty() ||
        targetPath.isEmpty() || invalidRawDestinationPath(targetPath) ||
        productDocument.rootDefinitionKind() != DefinitionKind::Component ||
        !project::isWritableComponentPersistenceFormat(
            currentPersistence.format))
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral("component Save As precondition failed"));
    const ProductDocumentIdentity expected = *productDocument.identity();
    if (!request.replacementIdentity.projectId.isValid() ||
        request.replacementIdentity.projectId == expected.projectId)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral("component fork identity is invalid"));
    auto transition = ComponentIdentityTransition::prepareSaveAs(
        productDocument, request.replacementIdentity);
    if (!transition)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "component Save As identity transition is invalid"));
    return saveComponentAsIdentityTransition(
        targetPath, productDocument, request.sourcePath,
        *transition.transition, currentPersistence);
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка сохранения копии компонента", exception);
  } catch (...) {
    return unknownFailure("Ошибка сохранения копии компонента");
  }
}

ProjectApplicationStatus
ProjectApplicationService::saveComponentAsIdentityTransition(
    const QString& targetPath, ProductDocument& productDocument,
    const QString& sourcePath,
    const ComponentIdentityTransition& transition,
    project::ProjectPersistenceState currentPersistence) const noexcept {
  try {
    if (sourcePath.isEmpty() || targetPath.isEmpty() ||
        invalidRawDestinationPath(targetPath) ||
        transition.kind() != ComponentIdentityTransitionKind::SaveAs ||
        !transition.sourceIdentity_ ||
        !transition.isCurrentSource(productDocument) ||
        !project::isWritableComponentPersistenceFormat(
            currentPersistence.format))
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "component sealed Save As precondition failed"));
    const AliasCheck currentFile = comparePathAliases(
        targetPath, sourcePath, nativeAssemblySaveAsPathMetadataReader());
    if (currentFile != AliasCheck::Distinct)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     currentFile == AliasCheck::Error
                         ? QStringLiteral("Save As target metadata query failed")
                         : QStringLiteral("Save As target is the current file"));
    ProductDocumentIdentity target = transition.targetIdentity();
    const auto metadata = metadataFor(target);
    if (!metadata)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "component fork metadata is not exact UTF-8"));
    const auto written = writeComponentAtomically(
        targetPath, productDocument,
        target.projectId, *metadata, currentPersistence);
    if (!written.succeeded())
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     std::move(written.error));
    if (productDocument.commitForkIdentity(
            *transition.sourceIdentity_, std::move(target)) !=
        ProductDocumentForkIdentityStatus::Committed)
      return failure(ProjectApplicationErrorCode::UnexpectedFailure,
                     QStringLiteral(
                         "published component fork identity could not be committed"));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка сохранения копии компонента", exception);
  } catch (...) {
    return unknownFailure("Ошибка сохранения копии компонента");
  }
}

namespace application::detail {

ProjectApplicationStatus saveAssemblyV7As(
    const QString& targetPath, ProductDocument& productDocument,
    const AssemblySaveAsRequest& request,
    const AssemblySaveAsPathMetadataReader& metadataReader) noexcept {
  return saveAssemblyAs(targetPath, productDocument,
                        {project::ProjectPersistenceFormat::V7}, request,
                        metadataReader);
}

ProjectApplicationStatus saveAssemblyAs(
    const QString& targetPath, ProductDocument& productDocument,
    project::ProjectPersistenceState persistenceState,
    const AssemblySaveAsRequest& request,
    const AssemblySaveAsPathMetadataReader& metadataReader) noexcept {
  try {
    if (!productDocument.identity() || request.sourcePath.isEmpty() ||
        targetPath.isEmpty() || invalidRawDestinationPath(targetPath) ||
        productDocument.rootDefinitionKind() != DefinitionKind::Assembly ||
        !project::isWritableAssemblyPersistenceFormat(
            persistenceState.format))
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral("assembly Save As precondition failed"));
    const ProductDocumentIdentity expected = *productDocument.identity();
    if (!request.replacementIdentity.projectId.isValid() ||
        request.replacementIdentity.projectId == expected.projectId)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral("assembly fork identity is invalid"));
    const AliasCheck currentFile =
        comparePathAliases(targetPath, request.sourcePath, metadataReader);
    if (currentFile != AliasCheck::Distinct)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     currentFile == AliasCheck::Error
                         ? QStringLiteral(
                               "Save As target metadata query failed")
                         : QStringLiteral(
                               "Save As target is the current file"));
    const AliasCheck linkedSource = destinationAliasesLinkedSource(
        targetPath, productDocument, request.sourcePath, metadataReader);
    if (linkedSource != AliasCheck::Distinct)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     linkedSource == AliasCheck::Error
                         ? QStringLiteral(
                               "linked source metadata query failed")
                         : QStringLiteral(
                               "Save As target aliases a linked source"));
    const auto relocation = crossDirectorySaveAsPreflight(
        productDocument, request.sourcePath, targetPath, metadataReader);
    if (!relocation.succeeded()) return relocation;
    const auto metadata = metadataFor(request.replacementIdentity);
    if (!metadata)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "assembly fork metadata is not exact UTF-8"));
    const auto written = writeAssemblyAtomically(
        targetPath, productDocument, request.replacementIdentity.projectId,
        *metadata, persistenceState);
    if (!written.succeeded())
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     std::move(written.error));
    const auto committed = productDocument.commitForkIdentity(
        expected, request.replacementIdentity);
    if (committed != ProductDocumentForkIdentityStatus::Committed)
      return failure(ProjectApplicationErrorCode::UnexpectedFailure,
                     QStringLiteral(
                          "published assembly fork identity could not be committed"));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка сохранения копии сборки", exception);
  } catch (...) {
    return unknownFailure("Ошибка сохранения копии сборки");
  }
}

}  // namespace application::detail

ProjectApplicationStatus
ProjectApplicationService::validateAssemblyV9Promotion(
    const ProductDocument& productDocument,
    project::ProjectPersistenceState currentState) const noexcept {
  try {
    if (currentState.format != project::ProjectPersistenceFormat::V7 &&
        currentState.format != project::ProjectPersistenceFormat::V8)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral("only V7/V8 assemblies can promote to V9"));
    if (productDocument.rootDefinitionKind() != DefinitionKind::Assembly)
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral("V9 promotion requires an assembly root"));
    for (const auto id : productDocument.definitions().definitionIds()) {
      const auto* assembly = productDocument.definitions().findAssembly(id);
      if (assembly && !assembly->mateRegistry().options().isValid())
        return failure(ProjectApplicationErrorCode::SaveFailed,
                       QStringLiteral("V9 mate options are invalid"));
    }
    return {};
  } catch (...) {
    return unknownFailure("V9 promotion validation");
  }
}

ProjectApplicationStatus
ProjectApplicationService::promoteAssemblyPersistenceToV9(
    project::ProjectPersistenceState& state,
    const ProductDocument& productDocument) const noexcept {
  const auto validated = validateAssemblyV9Promotion(productDocument, state);
  if (!validated.succeeded()) return validated;
  state.format = project::ProjectPersistenceFormat::V9;
  return {};
}

ProjectApplicationStatus ProjectApplicationService::upgradeProjectFormat(
    const QString& path, const Document& document,
    project::ProjectPersistenceState currentState) const noexcept {
  switch (currentState.format) {
    case project::ProjectPersistenceFormat::V3:
      return save(path, document, {project::ProjectPersistenceFormat::V4});
    case project::ProjectPersistenceFormat::V4:
    case project::ProjectPersistenceFormat::V5:
      return {};
    case project::ProjectPersistenceFormat::V6:
    case project::ProjectPersistenceFormat::V7:
    case project::ProjectPersistenceFormat::V8:
    case project::ProjectPersistenceFormat::V9:
    case project::ProjectPersistenceFormat::V10:
    case project::ProjectPersistenceFormat::V11:
    case project::ProjectPersistenceFormat::V12:
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "Product persistence is not a part-format upgrade."));
  }
  return failure(ProjectApplicationErrorCode::SaveFailed,
                 QStringLiteral("Unknown project persistence format."));
}

StagedApplicationDocumentResult ProjectApplicationService::stageImportStep(
    const QString& path, const Document& currentDocument,
    const QString& featureName) const noexcept {
  try {
    Document candidate = currentDocument;
    QString detail;
    if (!io::importDocumentStep(path, &candidate, featureName, &detail)) {
      return {{ProjectApplicationErrorCode::StepImportFailed,
               std::move(detail)},
              std::nullopt};
    }
    return {{}, StagedApplicationDocument(
                    std::move(candidate), std::nullopt, std::nullopt)};
  } catch (const std::exception& exception) {
    return {unexpectedFailure("Ошибка импорта STEP", exception),
            std::nullopt};
  } catch (...) {
    return {unknownFailure("Ошибка импорта STEP"), std::nullopt};
  }
}

StagedApplicationDocumentResult ProjectApplicationService::stageImportStep(
    const QString& path, const ProductDocument& currentProductDocument,
    const QString& featureName) const noexcept {
  try {
    if (currentProductDocument.rootDefinitionKind() != DefinitionKind::Part ||
        currentProductDocument.definitions().size() != 1) {
      return {{ProjectApplicationErrorCode::InvalidProject,
               QStringLiteral(
                   "STEP product staging requires one definition.")},
              std::nullopt};
    }
    detail::ScopedExplicitIdReservationPause reservationPause;
    Document candidate = currentProductDocument.rootDocument();
    QString detail;
    if (!io::importDocumentStep(path, &candidate, featureName, &detail)) {
      return {{ProjectApplicationErrorCode::StepImportFailed,
               std::move(detail)},
              std::nullopt};
    }
    ProductDocument stagedProduct = makeSingleDefinitionProduct(
        std::move(candidate), currentProductDocument.rootDefinitionId(),
        currentProductDocument.identity());
    return {{}, StagedApplicationDocument(
                    std::move(stagedProduct), std::nullopt, std::nullopt)};
  } catch (const std::exception& exception) {
    return {unexpectedFailure("Ошибка импорта STEP", exception),
            std::nullopt};
  } catch (...) {
    return {unknownFailure("Ошибка импорта STEP"), std::nullopt};
  }
}

ProjectApplicationStatus ProjectApplicationService::exportStep(
    const QString& path, const Document& document) const noexcept {
  try {
    QString detail;
    if (!io::exportDocumentStep(path, document, &detail))
      return failure(ProjectApplicationErrorCode::StepExportFailed,
                     std::move(detail));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка экспорта STEP", exception);
  } catch (...) {
    return unknownFailure("Ошибка экспорта STEP");
  }
}

ProjectApplicationStatus ProjectApplicationService::preflightStlExport(
    const Document& document) const noexcept {
  try {
    QString detail;
    if (!io::hasExportableDocumentShapes(document, &detail))
      return failure(ProjectApplicationErrorCode::NoExportableShapes,
                     std::move(detail));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка проверки экспорта STL", exception);
  } catch (...) {
    return unknownFailure("Ошибка проверки экспорта STL");
  }
}

ProjectApplicationStatus ProjectApplicationService::exportStl(
    const QString& path, const Document& document) const noexcept {
  try {
    QString detail;
    if (!io::exportDocumentAsciiStl(path, document, &detail))
      return failure(ProjectApplicationErrorCode::StlExportFailed,
                     std::move(detail));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка экспорта STL", exception);
  } catch (...) {
    return unknownFailure("Ошибка экспорта STL");
  }
}

ProjectApplicationStatus ProjectApplicationService::preflightFullProductExport(
    const ProductDocument& productDocument,
    const QString& projectPath) const noexcept {
  try {
    if (productDocument.rootDefinitionKind() != DefinitionKind::Assembly)
      return failure(ProjectApplicationErrorCode::InvalidProject,
                     QStringLiteral("full-product export requires assembly"));
    // Every linked definition participates, including orphaned definitions and
    // definitions referenced only by suppressed occurrences.
    return strictLinkedPreflight(productDocument, projectPath);
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка проверки связей перед экспортом",
                             exception);
  } catch (...) {
    return unknownFailure("Ошибка проверки связей перед экспортом");
  }
}

}  // namespace solidar

namespace solidar::application::detail {

ProjectApplicationStatus saveProductAsClone(
    ProductDocument& source, const ProductCloneRequest& request,
    project::detail::ProjectAtomicWriteTransaction& transaction) noexcept {
  return saveProductAsCloneImpl(
      source, request,
      [&source, &request, &transaction](
          const project::ProductEnvelopeV5Metadata& metadata) {
        return project::detail::writeProductEnvelopeV5Atomically(
            source, request.outputProjectId, metadata, transaction);
      });
}

ProjectApplicationStatus convertStandaloneToProduct(
    ProductDocument& source,
    const StandaloneProductConversionRequest& request,
    const StandaloneProductConversionPrecondition& precondition,
    project::detail::ProjectAtomicWriteTransaction& transaction) noexcept {
  return convertStandaloneToProductImpl(
      source, request, precondition, [&transaction](QByteArrayView bytes) {
        return project::detail::publishProjectPayloadAtomically(bytes,
                                                                 transaction);
      });
}

ProjectApplicationStatus relocateProduct(
    const QString& sourcePath, const QString& targetPath,
    const ProductDocument& source,
    const ProductRelocationPrecondition& precondition,
    project::detail::ProjectFileRelocationTransaction& transaction) noexcept {
  return relocateProductImpl(
      source, precondition,
      [&sourcePath, &targetPath, &source, &transaction] {
        return project::detail::relocateProductEnvelopeV5(
            sourcePath, targetPath, source, transaction);
      });
}

}  // namespace solidar::application::detail
