#include "project/LinkedPartSourceResolver.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <utility>

#include "project/ProductEnvelopeV5Decoder.h"
#include "project/ProductEnvelopeV5Materialization.h"
#include "project/internal/LinkedPartSourceFileSystem.h"

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace solidar::project {
namespace {

struct NormalizedHint {
  LinkedPartSourceResolveStatus status{
      LinkedPartSourceResolveStatus::InvalidDescriptor};
  QString path;
  std::size_t components{};
  QString error;
};

struct ResolverPreflight {
  bool succeeded{};
  LinkedPartSourceResolveResult failure;
  QStringList roots;
  std::vector<NormalizedHint> normalizedHints;
};

bool checkedAdd(std::uint64_t value, std::uint64_t limit,
                std::uint64_t* total) {
  if (!total || value > limit || *total > limit - value) return false;
  *total += value;
  return true;
}

bool boundedUtf8Size(QStringView text, std::uint64_t remaining,
                     std::uint64_t* exactSize) {
  if (!exactSize) return false;
  const auto units = static_cast<std::uint64_t>(text.size());
  // Avoid an unbounded temporary. If the conservative three-bytes-per-UTF-16
  // upper bound fits, Qt may allocate; otherwise count exact UTF-8 bytes first.
  if (units <= remaining / 3) {
    const QByteArray utf8 = text.toString().toUtf8();
    *exactSize = static_cast<std::uint64_t>(utf8.size());
    return *exactSize <= remaining;
  }
  std::uint64_t size = 0;
  for (qsizetype index = 0; index < text.size(); ++index) {
    const ushort value = text[index].unicode();
    std::uint64_t bytes = value <= 0x7f ? 1 : value <= 0x7ff ? 2 : 3;
    if (QChar::isHighSurrogate(value) && index + 1 < text.size() &&
        QChar::isLowSurrogate(text[index + 1].unicode())) {
      bytes = 4;
      ++index;
    }
    if (bytes > remaining - size) return false;
    size += bytes;
  }
  *exactSize = size;
  return true;
}

bool windowsReservedComponent(QString component) {
  while (component.endsWith(QLatin1Char(' ')) ||
         component.endsWith(QLatin1Char('.'))) {
    component.chop(1);
  }
  component = component.section(QLatin1Char('.'), 0, 0).toUpper();
  if (component == QStringLiteral("CON") ||
      component == QStringLiteral("PRN") ||
      component == QStringLiteral("AUX") ||
      component == QStringLiteral("NUL")) {
    return true;
  }
  if (component.size() == 4 &&
      (component.startsWith(QStringLiteral("COM")) ||
       component.startsWith(QStringLiteral("LPT"))) &&
      component.back() >= QLatin1Char('1') &&
      component.back() <= QLatin1Char('9')) {
    return true;
  }
  return false;
}

NormalizedHint normalizeHint(const QString& original,
                             LinkedPathSemantics semantics,
                             std::size_t maximumComponents) {
  if (original.isEmpty() || original.contains(QChar::Null))
    return {LinkedPartSourceResolveStatus::InvalidDescriptor, {}, 0,
            QStringLiteral("linked locator is empty or contains NUL")};

  QString path = original;
  path.replace(QLatin1Char('\\'), QLatin1Char('/'));
  if (path.startsWith(QStringLiteral("//?/")) ||
      path.startsWith(QStringLiteral("//./"))) {
    return {LinkedPartSourceResolveStatus::PathEscape, {}, 0,
            QStringLiteral("Windows device locators are forbidden")};
  }
  if (path.startsWith(QStringLiteral("//")))
    return {LinkedPartSourceResolveStatus::NetworkDisabled, {}, 0,
            QStringLiteral("network locators are disabled")};
  if (path.startsWith(QLatin1Char('/')))
    return {LinkedPartSourceResolveStatus::PathEscape, {}, 0,
            QStringLiteral("linked locator must be relative")};
  if (semantics == LinkedPathSemantics::Windows &&
      path.contains(QLatin1Char(':'))) {
    return {LinkedPartSourceResolveStatus::PathEscape, {}, 0,
            QStringLiteral("drive, device and ADS locators are forbidden")};
  }

  QStringList stack;
  const QStringList authored = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
  for (const QString& component : authored) {
    if (component.isEmpty() || component == QStringLiteral(".")) continue;
    if (component == QStringLiteral("..")) {
      if (stack.isEmpty())
        return {LinkedPartSourceResolveStatus::PathEscape, {}, 0,
                QStringLiteral("linked locator escapes its root")};
      stack.removeLast();
      continue;
    }
    if (semantics == LinkedPathSemantics::Windows &&
        (component.endsWith(QLatin1Char(' ')) ||
         component.endsWith(QLatin1Char('.')) ||
         windowsReservedComponent(component))) {
      return {LinkedPartSourceResolveStatus::PathEscape, {}, 0,
              QStringLiteral("Windows device or alias component is forbidden")};
    }
    stack.push_back(component);
    if (static_cast<std::size_t>(stack.size()) > maximumComponents)
      return {LinkedPartSourceResolveStatus::BudgetExceeded, {}, 0,
              QStringLiteral("linked locator component budget exceeded")};
  }
  if (stack.isEmpty())
    return {LinkedPartSourceResolveStatus::InvalidDescriptor, {}, 0,
            QStringLiteral("linked locator resolves to an empty path")};
  return {LinkedPartSourceResolveStatus::Resolved, stack.join(QLatin1Char('/')),
          static_cast<std::size_t>(stack.size()), {}};
}

QString normalizedComparisonPath(QString path) {
  path.replace(QLatin1Char('\\'), QLatin1Char('/'));
  while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) path.chop(1);
  return path;
}

std::optional<int> windowsOrdinalCompare(QStringView lhs, QStringView rhs) {
#if defined(Q_OS_WIN)
  const int result = CompareStringOrdinal(
      reinterpret_cast<LPCWCH>(lhs.utf16()), lhs.size(),
      reinterpret_cast<LPCWCH>(rhs.utf16()), rhs.size(), TRUE);
  if (result == CSTR_LESS_THAN) return -1;
  if (result == CSTR_GREATER_THAN) return 1;
  if (result == CSTR_EQUAL) return 0;
  return std::nullopt;
#else
  const qsizetype count = (std::min)(lhs.size(), rhs.size());
  for (qsizetype index = 0; index < count; ++index) {
    const ushort left = lhs[index].toUpper().unicode();
    const ushort right = rhs[index].toUpper().unicode();
    if (left < right) return -1;
    if (left > right) return 1;
  }
  return lhs.size() < rhs.size() ? -1 : lhs.size() > rhs.size() ? 1 : 0;
#endif
}

bool pathEqual(QStringView lhs, QStringView rhs,
               LinkedPathSemantics semantics) {
  return semantics == LinkedPathSemantics::Windows
             ? windowsOrdinalCompare(lhs, rhs) == std::optional<int>{0}
             : lhs == rhs;
}

bool pathLess(QStringView lhs, QStringView rhs,
              LinkedPathSemantics semantics) {
  return semantics == LinkedPathSemantics::Windows
             ? windowsOrdinalCompare(lhs, rhs).value_or(0) < 0
             : lhs < rhs;
}

bool isNetworkPath(QString path) {
  path.replace(QLatin1Char('\\'), QLatin1Char('/'));
  return path.startsWith(QStringLiteral("//"));
}

bool containedBy(const QString& target, const QStringList& roots,
                 LinkedPathSemantics semantics) {
  const QString comparedTarget = normalizedComparisonPath(target);
  for (const QString& root : roots) {
    const QString comparedRoot = normalizedComparisonPath(root);
    if (pathEqual(comparedTarget, comparedRoot, semantics) ||
        (comparedTarget.size() > comparedRoot.size() &&
         comparedTarget[comparedRoot.size()] == QLatin1Char('/') &&
         pathEqual(QStringView{comparedTarget}.first(comparedRoot.size()),
                   comparedRoot, semantics))) {
      return true;
    }
  }
  return false;
}

LinkedPartSourceResolveResult failure(LinkedPartSourceResolveStatus status,
                                      std::size_t index, QString error) {
  LinkedPartSourceResolveResult result;
  result.status = status;
  result.failureIndex = index;
  result.error = std::move(error);
  return result;
}

ResolverPreflight preflightLinkedDescriptorsAndPolicy(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    const LinkedPartSourcePolicy& policy) {
  ResolverPreflight result;
  const auto& budgets = policy.budgets;
  if (descriptors.size() > budgets.maximumDescriptors) {
    result.failure = failure(LinkedPartSourceResolveStatus::BudgetExceeded, 0,
                             QStringLiteral("linked descriptor budget exceeded"));
    return result;
  }
  if (policy.projectRoot.isEmpty()) {
    result.failure = failure(LinkedPartSourceResolveStatus::InvalidDescriptor, 0,
                             QStringLiteral("project root is empty"));
    return result;
  }
  result.roots = QStringList{policy.projectRoot};
  result.roots.append(policy.libraryRoots);
  if (static_cast<std::size_t>(result.roots.size()) > budgets.maximumRoots) {
    result.failure = failure(LinkedPartSourceResolveStatus::BudgetExceeded, 0,
                             QStringLiteral("allowed root count budget exceeded"));
    return result;
  }
  std::uint64_t rootBytes = 0;
  for (const QString& root : result.roots) {
    if (root.isEmpty() || root.contains(QChar::Null) || isNetworkPath(root)) {
      result.failure = failure(
          isNetworkPath(root) ? LinkedPartSourceResolveStatus::NetworkDisabled
                              : LinkedPartSourceResolveStatus::InvalidDescriptor,
          0, QStringLiteral("invalid allowed root"));
      return result;
    }
    if (!QDir::isAbsolutePath(root)) {
      result.failure = failure(LinkedPartSourceResolveStatus::InvalidDescriptor,
                               0, QStringLiteral("allowed roots must be absolute"));
      return result;
    }
    std::uint64_t exact = 0;
    if (!boundedUtf8Size(root, budgets.maximumTotalRootUtf8Bytes - rootBytes,
                         &exact) ||
        !checkedAdd(exact, budgets.maximumTotalRootUtf8Bytes, &rootBytes)) {
      result.failure = failure(LinkedPartSourceResolveStatus::BudgetExceeded, 0,
                               QStringLiteral("allowed root byte budget exceeded"));
      return result;
    }
  }

  std::uint64_t pathBytes = 0;
  result.normalizedHints.reserve(descriptors.size());
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    const auto& descriptor = descriptors[index];
    if (!descriptor.key.isValid()) {
      result.failure = failure(LinkedPartSourceResolveStatus::InvalidDescriptor,
                               index,
                               QStringLiteral("external source key is invalid"));
      return result;
    }
    std::uint64_t exact = 0;
    if (!boundedUtf8Size(descriptor.relativePathHint,
                         budgets.maximumTotalPathUtf8Bytes - pathBytes,
                         &exact) ||
        !checkedAdd(exact, budgets.maximumTotalPathUtf8Bytes, &pathBytes)) {
      result.failure = failure(LinkedPartSourceResolveStatus::BudgetExceeded,
                               index,
                               QStringLiteral("linked path byte budget exceeded"));
      return result;
    }
    auto normalized = normalizeHint(descriptor.relativePathHint,
                                    policy.pathSemantics,
                                    budgets.maximumPathComponents);
    if (normalized.status != LinkedPartSourceResolveStatus::Resolved) {
      result.failure = failure(normalized.status, index, normalized.error);
      return result;
    }
    result.normalizedHints.push_back(std::move(normalized));
  }
  result.succeeded = true;
  return result;
}

LinkedPartSourceResolveStatus mapOpenStatus(
    detail::LinkedSourceOpenStatus status) {
  switch (status) {
    case detail::LinkedSourceOpenStatus::Opened:
      return LinkedPartSourceResolveStatus::Resolved;
    case detail::LinkedSourceOpenStatus::Missing:
      return LinkedPartSourceResolveStatus::Missing;
    case detail::LinkedSourceOpenStatus::NetworkDisabled:
      return LinkedPartSourceResolveStatus::NetworkDisabled;
    case detail::LinkedSourceOpenStatus::SourceNotRegular:
      return LinkedPartSourceResolveStatus::SourceNotRegular;
    case detail::LinkedSourceOpenStatus::TraversalLoop:
      return LinkedPartSourceResolveStatus::TraversalLoop;
    case detail::LinkedSourceOpenStatus::BudgetExceeded:
      return LinkedPartSourceResolveStatus::BudgetExceeded;
    case detail::LinkedSourceOpenStatus::SourceChanged:
      return LinkedPartSourceResolveStatus::SourceChanged;
    case detail::LinkedSourceOpenStatus::ResourceFailure:
      return LinkedPartSourceResolveStatus::ResourceFailure;
  }
  return LinkedPartSourceResolveStatus::ResourceFailure;
}

LinkedPartSourceResolveStatus classifyNonV5(QByteArrayView bytes) {
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(
      QByteArray(bytes.data(), bytes.size()), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject())
    return LinkedPartSourceResolveStatus::InvalidSource;
  const QJsonValue version = document.object().value(QStringLiteral("version"));
  if (!version.isDouble()) return LinkedPartSourceResolveStatus::InvalidSource;
  const double authored = version.toDouble();
  if (!std::isfinite(authored) || std::trunc(authored) != authored ||
      authored < static_cast<double>((std::numeric_limits<int>::min)()) ||
      authored > static_cast<double>((std::numeric_limits<int>::max)())) {
    return LinkedPartSourceResolveStatus::InvalidSource;
  }
  const int integral = static_cast<int>(authored);
  if (integral >= 1 && integral <= 4)
    return LinkedPartSourceResolveStatus::IdentityUnavailable;
  if (integral == 6) return LinkedPartSourceResolveStatus::NestingUnsupported;
  if (integral > 6) return LinkedPartSourceResolveStatus::UnsupportedSource;
  return LinkedPartSourceResolveStatus::InvalidSource;
}

struct PreparedSource {
  std::size_t descriptorIndex{};
  ExternalSourceKey actualKey;
  SourceContentHash actualHash;
  QString canonicalPath;
  ProductEnvelopeV5Candidate candidate;
};

std::uint64_t rebuildWork(const ProductEnvelopeV5Candidate& candidate) {
  if (!candidate.embeddedPart) return 0;
  return static_cast<std::uint64_t>(
      candidate.embeddedPart->legacySketch.topology.payload.rebuildWork);
}

class NativeFileSystem final : public detail::LinkedPartSourceFileSystem {
 public:
  detail::LinkedSourceCanonicalRoot canonicalizeRoot(
      const QString& absolutePath, std::uint32_t maximumAliasDepth,
      std::uint64_t maximumWorkUnits) override {
    return detail::canonicalizeNativeLinkedRoot(
        absolutePath, maximumAliasDepth, maximumWorkUnits);
  }

  detail::LinkedSourceOpenedFile openExact(
      const QString& absolutePath, std::uint32_t maximumAliasDepth,
      std::uint64_t maximumWorkUnits) override {
    return detail::openNativeLinkedPartSource(
        absolutePath, maximumAliasDepth, maximumWorkUnits);
  }
};

class RootBudgetFileSystem final : public detail::LinkedPartSourceFileSystem {
 public:
  RootBudgetFileSystem(detail::LinkedPartSourceFileSystem& inner,
                       QStringList roots,
                       std::vector<std::uint32_t> aliasDepths,
                       LinkedPathSemantics semantics)
      : inner_(inner), roots_(std::move(roots)),
        aliasDepths_(std::move(aliasDepths)), semantics_(semantics) {}

  detail::LinkedSourceCanonicalRoot canonicalizeRoot(
      const QString&, std::uint32_t, std::uint64_t) override {
    detail::LinkedSourceCanonicalRoot result;
    result.error = QStringLiteral("root canonicalization is not recursive");
    return result;
  }

  detail::LinkedSourceOpenedFile openExact(
      const QString& absolutePath, std::uint32_t maximumAliasDepth,
      std::uint64_t maximumWorkUnits) override {
    std::optional<std::uint32_t> rootDepth;
    const QString target = normalizedComparisonPath(absolutePath);
    for (std::size_t index = 0; index < aliasDepths_.size(); ++index) {
      const QString root = normalizedComparisonPath(
          roots_[static_cast<qsizetype>(index)]);
      if (pathEqual(target, root, semantics_) ||
          (target.size() > root.size() &&
           target[root.size()] == QLatin1Char('/') &&
           pathEqual(QStringView{target}.first(root.size()), root,
                     semantics_))) {
        rootDepth = rootDepth ? (std::max)(*rootDepth, aliasDepths_[index])
                              : aliasDepths_[index];
      }
    }
    if (!rootDepth || *rootDepth > maximumAliasDepth) {
      detail::LinkedSourceOpenedFile result;
      result.status = detail::LinkedSourceOpenStatus::BudgetExceeded;
      result.error =
          QStringLiteral("combined root and target alias depth exceeded");
      return result;
    }
    auto result = inner_.openExact(absolutePath,
                                   maximumAliasDepth - *rootDepth,
                                   maximumWorkUnits);
    if (result.aliasDepth >
        (std::numeric_limits<std::uint32_t>::max)() - *rootDepth) {
      result.status = detail::LinkedSourceOpenStatus::BudgetExceeded;
      result.error = QStringLiteral("combined alias depth overflow");
      return result;
    }
    result.aliasDepth += *rootDepth;
    return result;
  }

 private:
  detail::LinkedPartSourceFileSystem& inner_;
  QStringList roots_;
  std::vector<std::uint32_t> aliasDepths_;
  LinkedPathSemantics semantics_;
};

}  // namespace

SourceContentHash linkedSourceSha256(QByteArrayView bytes) {
  const QByteArray hash = QCryptographicHash::hash(
      QByteArray(bytes.data(), bytes.size()), QCryptographicHash::Sha256);
  SourceContentHash result;
  std::copy_n(reinterpret_cast<const std::uint8_t*>(hash.constData()),
              result.bytes.size(), result.bytes.begin());
  return result;
}

bool isSafeRelativeLinkedPathHint(QStringView hint,
                                  LinkedPathSemantics semantics,
                                  std::size_t maximumComponents) {
  return normalizeHint(hint.toString(), semantics, maximumComponents).status ==
         LinkedPartSourceResolveStatus::Resolved;
}

enum class ResolverIntent { Exact, Reload, RepairProbe };

LinkedPartSourceResolveResult resolveLinkedPartSourcesWithIntent(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    const LinkedPartSourcePolicy& policy,
    detail::LinkedPartSourceFileSystem& fileSystem, ResolverIntent intent) {
  try {
    const auto& budgets = policy.budgets;
    auto preflight = preflightLinkedDescriptorsAndPolicy(descriptors, policy);
    if (!preflight.succeeded) return std::move(preflight.failure);
    const QStringList& roots = preflight.roots;
    std::uint64_t totalBytes = 0;
    std::uint64_t work = 0;
    std::uint64_t filesInspected = 0;
    std::uint64_t filesRead = 0;
    std::uint64_t sourcesDecoded = 0;
    std::vector<PreparedSource> prepared;
    prepared.reserve(descriptors.size());
    struct IdentityCacheEntry {
      SourceContentHash hash;
      ExternalSourceKey key;
      QString version;
      QString canonicalPath;
      ProductEnvelopeV5Candidate candidate;
    };
    std::map<QString, IdentityCacheEntry> identityCache;
    std::map<SourceContentHash, IdentityCacheEntry> contentCache;

    for (std::size_t index = 0; index < descriptors.size(); ++index) {
      const NormalizedHint& normalized = preflight.normalizedHints[index];
      std::optional<detail::LinkedSourceOpenedFile> opened;
      for (const QString& root : roots) {
        if (work >= budgets.maximumFileSystemWork)
          return failure(LinkedPartSourceResolveStatus::BudgetExceeded, index,
                         QStringLiteral("filesystem work budget exhausted"));
        const QString attempted = root + QLatin1Char('/') + normalized.path;
        auto candidate = fileSystem.openExact(
            attempted, budgets.maximumAliasDepth,
            budgets.maximumFileSystemWork - work);
        if (!checkedAdd(candidate.workUnits, budgets.maximumFileSystemWork,
                        &work)) {
          return failure(LinkedPartSourceResolveStatus::BudgetExceeded, index,
                         QStringLiteral("filesystem work budget exceeded"));
        }
        if (candidate.status == detail::LinkedSourceOpenStatus::Missing)
          continue;
        if (candidate.status != detail::LinkedSourceOpenStatus::Opened)
          return failure(mapOpenStatus(candidate.status), index,
                         std::move(candidate.error));
        opened.emplace(std::move(candidate));
        ++filesInspected;
        break;
      }
      if (!opened)
        return failure(LinkedPartSourceResolveStatus::Missing, index,
                       QStringLiteral("linked source is missing"));
      if (opened->aliasDepth > budgets.maximumAliasDepth)
        return failure(LinkedPartSourceResolveStatus::BudgetExceeded, index,
                       QStringLiteral("alias depth budget exceeded"));
      if (isNetworkPath(opened->canonicalPath))
        return failure(LinkedPartSourceResolveStatus::NetworkDisabled, index,
                       QStringLiteral("network targets are disabled"));
      if (!containedBy(opened->canonicalPath, roots, policy.pathSemantics))
        return failure(LinkedPartSourceResolveStatus::OutsideAllowedRoot, index,
                       QStringLiteral("linked source resolves outside allowed roots"));
      if (!opened->fileIdentity.isEmpty()) {
        const auto cached = identityCache.find(opened->fileIdentity);
        if (cached != identityCache.end()) {
          if (cached->second.version != opened->objectVersion)
            return failure(LinkedPartSourceResolveStatus::SourceChanged, index,
                           QStringLiteral("same file identity changed during batch"));
          prepared.push_back({index, cached->second.key, cached->second.hash,
                              std::move(opened->canonicalPath),
                              cached->second.candidate});
          continue;
        }
      }
      if (!opened->handle)
        return failure(LinkedPartSourceResolveStatus::ResourceFailure, index,
                       QStringLiteral("linked source handle is unavailable"));
      if (opened->sizeBytes > budgets.maximumFileBytes ||
          opened->sizeBytes > budgets.maximumTotalBytes - totalBytes) {
        return failure(LinkedPartSourceResolveStatus::BudgetExceeded, index,
                       QStringLiteral("aggregate linked byte budget exceeded"));
      }
      if (work >= budgets.maximumFileSystemWork)
        return failure(LinkedPartSourceResolveStatus::BudgetExceeded, index,
                       QStringLiteral("filesystem work budget exhausted"));
      auto read = opened->handle->readBounded(
          (std::min)(budgets.maximumFileBytes,
                     budgets.maximumTotalBytes - totalBytes),
          budgets.maximumFileSystemWork - work);
      if (!checkedAdd(read.workUnits, budgets.maximumFileSystemWork, &work))
        return failure(LinkedPartSourceResolveStatus::BudgetExceeded, index,
                       QStringLiteral("filesystem work budget exceeded"));
      if (read.status != detail::LinkedSourceOpenStatus::Opened)
        return failure(mapOpenStatus(read.status), index, std::move(read.error));
      ++filesRead;
      if (!checkedAdd(static_cast<std::uint64_t>(read.bytes.size()),
                      budgets.maximumTotalBytes, &totalBytes))
        return failure(LinkedPartSourceResolveStatus::BudgetExceeded, index,
                       QStringLiteral("aggregate linked byte budget exceeded"));
      const SourceContentHash actualHash =
          linkedSourceSha256(QByteArrayView(read.bytes));

      const auto cachedContent = contentCache.find(actualHash);
      if (cachedContent != contentCache.end()) {
        prepared.push_back({index, cachedContent->second.key,
                            cachedContent->second.hash,
                            std::move(opened->canonicalPath),
                            cachedContent->second.candidate});
        if (!opened->fileIdentity.isEmpty()) {
          identityCache.emplace(opened->fileIdentity, cachedContent->second);
        }
        continue;
      }

      auto scanned = scanProductEnvelopeV5(QByteArrayView(read.bytes));
      ++sourcesDecoded;
      if (scanned.status == ProductEnvelopeV5ScanStatus::NotApplicable)
        return failure(classifyNonV5(QByteArrayView(read.bytes)), index,
                       QStringLiteral("linked source is not a V5 part envelope"));
      if (scanned.status == ProductEnvelopeV5ScanStatus::Unsupported) {
        const auto nonV5 = classifyNonV5(QByteArrayView(read.bytes));
        if (nonV5 == LinkedPartSourceResolveStatus::NestingUnsupported)
          return failure(nonV5, index,
                         QStringLiteral("linked assembly nesting is unsupported"));
        return failure(LinkedPartSourceResolveStatus::UnsupportedSource, index,
                       std::move(scanned.error));
      }
      if (scanned.status != ProductEnvelopeV5ScanStatus::Candidate ||
          !scanned.candidate) {
        return failure(LinkedPartSourceResolveStatus::InvalidSource, index,
                       std::move(scanned.error));
      }
      const ExternalSourceKey actualKey{scanned.candidate->projectId,
                                        scanned.candidate->definitionId};
      if (!opened->fileIdentity.isEmpty()) {
        identityCache.emplace(
            opened->fileIdentity,
            IdentityCacheEntry{actualHash, actualKey, opened->objectVersion,
                               opened->canonicalPath,
                               *scanned.candidate});
      }
      contentCache.emplace(
          actualHash,
          IdentityCacheEntry{actualHash, actualKey, opened->objectVersion,
                             opened->canonicalPath,
                             *scanned.candidate});
      prepared.push_back({index, actualKey, actualHash,
                          std::move(opened->canonicalPath),
                          std::move(*scanned.candidate)});
    }

    std::map<ExternalSourceKey, SourceContentHash> hashes;
    for (const auto& source : prepared) {
      const auto [position, inserted] =
          hashes.emplace(source.actualKey, source.actualHash);
      if (!inserted && position->second != source.actualHash)
        return failure(LinkedPartSourceResolveStatus::IdentityConflict,
                       source.descriptorIndex,
                       QStringLiteral("same external identity has different content"));
    }

    for (const auto& source : prepared) {
      const auto& expected = descriptors[source.descriptorIndex];
      if (source.actualKey.projectId != expected.key.projectId)
        return failure(LinkedPartSourceResolveStatus::IdentityMismatch,
                       source.descriptorIndex,
                       QStringLiteral("linked project identity does not match"));
      if (source.actualKey.definitionId != expected.key.definitionId)
        return failure(LinkedPartSourceResolveStatus::DefinitionMissing,
                       source.descriptorIndex,
                       QStringLiteral("linked definition is missing"));
      if (intent == ResolverIntent::Exact &&
          source.actualHash != expected.expectedHash)
        return failure(LinkedPartSourceResolveStatus::Outdated,
                       source.descriptorIndex,
                       QStringLiteral("linked source revision is outdated"));
    }

    struct UniquePrepared {
      SourceContentHash hash;
      QString canonicalPath;
      ProductEnvelopeV5Candidate candidate;
      std::vector<std::size_t> indexes;
    };
    std::map<ExternalSourceKey, UniquePrepared> unique;
    std::uint64_t totalRebuildWork = 0;
    for (auto& source : prepared) {
      auto position = unique.find(source.actualKey);
      if (position == unique.end()) {
        if (!checkedAdd(rebuildWork(source.candidate),
                        budgets.maximumRebuildWork, &totalRebuildWork)) {
          return failure(LinkedPartSourceResolveStatus::BudgetExceeded,
                         source.descriptorIndex,
                         QStringLiteral("aggregate rebuild budget exceeded"));
        }
        UniquePrepared value{source.actualHash, source.canonicalPath,
                             std::move(source.candidate),
                             {source.descriptorIndex}};
        unique.emplace(source.actualKey, std::move(value));
      } else {
        position->second.indexes.push_back(source.descriptorIndex);
        if (pathLess(normalizedComparisonPath(source.canonicalPath),
                     normalizedComparisonPath(position->second.canonicalPath),
                     policy.pathSemantics)) {
          position->second.canonicalPath = source.canonicalPath;
        }
      }
    }

    std::vector<ResolvedLinkedPartSource> staged;
    staged.reserve(unique.size());
    for (auto& [key, source] : unique) {
      if (intent == ResolverIntent::RepairProbe) {
        staged.push_back({key, source.hash, std::move(source.canonicalPath),
                          std::move(source.indexes), {}});
        continue;
      }
      auto materialized = materializeProductEnvelopeV5(source.candidate);
      if (materialized.status ==
          ProductEnvelopeV5MaterializationStatus::Unsupported) {
        return failure(LinkedPartSourceResolveStatus::UnsupportedSource,
                       source.indexes.front(), std::move(materialized.error));
      }
      if (!materialized.succeeded())
        return failure(LinkedPartSourceResolveStatus::InvalidSource,
                       source.indexes.front(), std::move(materialized.error));
      staged.push_back({key, source.hash, std::move(source.canonicalPath),
                        std::move(source.indexes),
                        std::move(materialized.product)});
    }

    LinkedPartSourceResolveResult result;
    result.status = LinkedPartSourceResolveStatus::Resolved;
    result.sources = std::move(staged);
    result.filesInspected = filesInspected;
    result.filesRead = filesRead;
    result.sourcesDecoded = sourcesDecoded;
    return result;
  } catch (const std::bad_alloc&) {
    return failure(LinkedPartSourceResolveStatus::ResourceFailure, 0,
                   QStringLiteral("linked resolver allocation failure"));
  } catch (...) {
    return failure(LinkedPartSourceResolveStatus::ResourceFailure, 0,
                   QStringLiteral("unexpected linked resolver failure"));
  }
}

LinkedPartSourceResolveResult resolveLinkedPartSources(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    const LinkedPartSourcePolicy& policy,
    detail::LinkedPartSourceFileSystem& fileSystem) {
  return resolveLinkedPartSourcesWithIntent(
      descriptors, policy, fileSystem, ResolverIntent::Exact);
}

LinkedPartSourceResolveResult resolveLinkedPartSourcesForReload(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    const LinkedPartSourcePolicy& policy,
    detail::LinkedPartSourceFileSystem& fileSystem) {
  return resolveLinkedPartSourcesWithIntent(
      descriptors, policy, fileSystem, ResolverIntent::Reload);
}

LinkedPartSourceResolveResult probeLinkedPartSourcesForRepair(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    const LinkedPartSourcePolicy& policy,
    detail::LinkedPartSourceFileSystem& fileSystem) {
  return resolveLinkedPartSourcesWithIntent(
      descriptors, policy, fileSystem, ResolverIntent::RepairProbe);
}

LinkedPartSourceResolveResult resolveLinkedPartSourcesNativeWithFileSystemIntent(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    LinkedPartSourcePolicy policy,
    detail::LinkedPartSourceFileSystem& fileSystem, ResolverIntent intent) {
#if defined(Q_OS_WIN)
  policy.pathSemantics = LinkedPathSemantics::Windows;
#else
  policy.pathSemantics = LinkedPathSemantics::Posix;
#endif
  auto authoredPreflight =
      preflightLinkedDescriptorsAndPolicy(descriptors, policy);
  if (!authoredPreflight.succeeded)
    return std::move(authoredPreflight.failure);
  const QStringList& authoredRoots = authoredPreflight.roots;
  QStringList canonicalRoots;
  canonicalRoots.reserve(authoredRoots.size());
  std::uint64_t rootWork = 0;
  std::vector<std::uint32_t> rootAliasDepths;
  rootAliasDepths.reserve(static_cast<std::size_t>(authoredRoots.size()));
  for (const QString& root : authoredRoots) {
    if (rootWork >= policy.budgets.maximumFileSystemWork)
      return failure(LinkedPartSourceResolveStatus::BudgetExceeded, 0,
                     QStringLiteral("filesystem work budget exhausted by roots"));
    auto canonical = fileSystem.canonicalizeRoot(
        root, policy.budgets.maximumAliasDepth,
        policy.budgets.maximumFileSystemWork - rootWork);
    if (!checkedAdd(canonical.workUnits,
                    policy.budgets.maximumFileSystemWork, &rootWork))
      return failure(LinkedPartSourceResolveStatus::BudgetExceeded, 0,
                     QStringLiteral("filesystem work budget exceeded by roots"));
    if (canonical.status != detail::LinkedSourceOpenStatus::Opened)
      return failure(mapOpenStatus(canonical.status), 0,
                     std::move(canonical.error));
    canonicalRoots.push_back(std::move(canonical.canonicalPath));
    rootAliasDepths.push_back(canonical.aliasDepth);
  }
  policy.projectRoot = canonicalRoots.front();
  policy.libraryRoots = canonicalRoots.sliced(1);
  policy.budgets.maximumFileSystemWork -= rootWork;
  RootBudgetFileSystem budgetedFileSystem(
      fileSystem, canonicalRoots, std::move(rootAliasDepths),
      policy.pathSemantics);
  return resolveLinkedPartSourcesWithIntent(descriptors, policy,
                                            budgetedFileSystem, intent);
}

LinkedPartSourceResolveResult detail::resolveLinkedPartSourcesNativeWithFileSystem(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    LinkedPartSourcePolicy policy,
    detail::LinkedPartSourceFileSystem& fileSystem) {
  return resolveLinkedPartSourcesNativeWithFileSystemIntent(
      descriptors, std::move(policy), fileSystem, ResolverIntent::Exact);
}

LinkedPartSourceResolveResult resolveLinkedPartSourcesNative(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    LinkedPartSourcePolicy policy) {
  NativeFileSystem fileSystem;
  return detail::resolveLinkedPartSourcesNativeWithFileSystem(
      descriptors, std::move(policy), fileSystem);
}

LinkedPartSourceResolveResult resolveLinkedPartSourcesForReloadNative(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    LinkedPartSourcePolicy policy) {
  NativeFileSystem fileSystem;
  return resolveLinkedPartSourcesNativeWithFileSystemIntent(
      descriptors, std::move(policy), fileSystem, ResolverIntent::Reload);
}

LinkedPartSourceResolveResult probeLinkedPartSourcesForRepairNative(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    LinkedPartSourcePolicy policy) {
  NativeFileSystem fileSystem;
  return resolveLinkedPartSourcesNativeWithFileSystemIntent(
      descriptors, std::move(policy), fileSystem,
      ResolverIntent::RepairProbe);
}

}  // namespace solidar::project
