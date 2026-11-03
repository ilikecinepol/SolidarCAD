#include "TestAssertions.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

#if defined(Q_OS_WIN)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "project/LinkedPartSourceResolver.h"

namespace {

using namespace solidar;
using namespace solidar::project;

QByteArray validBytes() {
  const QJsonObject embedded{
      {"format", "solidar-project"},
      {"version", 4},
      {"capabilities", QJsonArray{"persistent-topology-v2"}},
      {"constraintTypeEncoding", "stable-name-v1"},
      {"name", "native"},
      {"createdAt", "2026-10-26T00:00:00Z"},
      {"document", QJsonObject{{"widthMm", 1},
                                 {"heightMm", 1},
                                 {"extrusionMm", 1}}},
      {"sketches", QJsonArray{}},
      {"extrusion", QJsonObject{{"enabled", false}}},
      {"model", QJsonObject{{"sketches", QJsonArray{}},
                              {"bodies", QJsonArray{}},
                              {"referenceImages", QJsonArray{}}}}};
  const QJsonObject payload{{"payloadKind", "embedded-part-v4"},
                            {"data", embedded}};
  const QJsonObject definition{{"definitionId", 17},
                               {"definitionKind", "part"},
                               {"payload", payload}};
  return QJsonDocument(QJsonObject{
                           {"format", "solidar-project"},
                           {"version", 5},
                           {"capabilities", QJsonArray{"product-envelope-v1"}},
                           {"projectId", "123e4567-e89b-42d3-a456-426614174000"},
                           {"rootDefinitionId", 17},
                           {"metadata", QJsonObject{{"name", "native"},
                                                     {"createdAt", "2026-10-26T00:00:00Z"}}},
                           {"definitions", QJsonArray{definition}}})
      .toJson(QJsonDocument::Compact);
}

}  // namespace

int main() {
  QTemporaryDir directory(
      QDir::current().filePath(QStringLiteral("linked-resolver-native-XXXXXX")));
  CHECK(directory.isValid());
  const QString sourcePath = directory.filePath(QStringLiteral("part.solidar"));
  const QByteArray bytes = validBytes();
  {
    QFile file(sourcePath);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(file.write(bytes) == bytes.size());
  }
  const QStringList before = QDir(directory.path()).entryList(
      QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
  const auto modified = QFileInfo(sourcePath).lastModified();

  constexpr ProjectId project{
      {0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
       0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00}};
  LinkedPartSourcePolicy policy;
  policy.projectRoot = directory.path();
#if defined(Q_OS_WIN)
  policy.pathSemantics = LinkedPathSemantics::Windows;
#else
  policy.pathSemantics = LinkedPathSemantics::Posix;
#endif
  {
    // Pure descriptor preflight must win before an invalid root can trigger
    // native canonicalization or any target I/O.
    auto missingRootPolicy = policy;
    missingRootPolicy.projectRoot =
        directory.filePath(QStringLiteral("missing-root"));
    const LinkedPartSourceDescriptor escaped{
        {project, DefinitionId{17}}, QStringLiteral("../escape.solidar"),
        linkedSourceSha256(QByteArrayView(bytes))};
    CHECK(resolveLinkedPartSourcesNative({escaped}, missingRootPolicy).status ==
          LinkedPartSourceResolveStatus::PathEscape);
    auto descriptorBudgetPolicy = missingRootPolicy;
    descriptorBudgetPolicy.budgets.maximumDescriptors = 0;
    CHECK(resolveLinkedPartSourcesNative({escaped}, descriptorBudgetPolicy)
              .status == LinkedPartSourceResolveStatus::BudgetExceeded);
    auto invalidKey = escaped;
    invalidKey.relativePathHint = QStringLiteral("safe.solidar");
    invalidKey.key = {};
    CHECK(resolveLinkedPartSourcesNative({invalidKey}, missingRootPolicy)
              .status == LinkedPartSourceResolveStatus::InvalidDescriptor);
  }
  const LinkedPartSourceDescriptor descriptor{
      {project, DefinitionId{17}}, QStringLiteral("part.solidar"),
      linkedSourceSha256(QByteArrayView(bytes))};
#if defined(Q_OS_WIN)
  {
    const QString canonicalRoot = QFileInfo(directory.path()).canonicalFilePath();
    const qsizetype rootComponents =
        QDir::fromNativeSeparators(canonicalRoot)
            .sliced(3)
            .split(QLatin1Char('/'), Qt::SkipEmptyParts)
            .size();
    // Root walk: 3*N+5. Target walk: 3*N+7. Held non-empty read
    // plus verification metadata query: 2.
    const std::uint64_t exactWork =
        6ULL * static_cast<std::uint64_t>(rootComponents) + 14ULL;
    auto exactPolicy = policy;
    exactPolicy.budgets.maximumFileSystemWork = exactWork;
    CHECK(resolveLinkedPartSourcesNative({descriptor}, exactPolicy).succeeded());
    exactPolicy.budgets.maximumFileSystemWork = exactWork - 1;
    CHECK(resolveLinkedPartSourcesNative({descriptor}, exactPolicy).status ==
          LinkedPartSourceResolveStatus::BudgetExceeded);
  }
#endif
  const auto result = resolveLinkedPartSourcesNative({descriptor}, policy);
  CHECK(result.succeeded());
  CHECK(result.sources.size() == 1);
  CHECK(result.sources.front().product != nullptr);

  QFile source(sourcePath);
  CHECK(source.open(QIODevice::ReadOnly));
  CHECK(source.readAll() == bytes);
  source.close();
  CHECK(QFileInfo(sourcePath).lastModified() == modified);
  CHECK(QDir(directory.path()).entryList(QDir::Files | QDir::NoDotAndDotDot,
                                         QDir::Name) == before);

  const LinkedPartSourceDescriptor missingDescriptor{
      descriptor.key, QStringLiteral("missing.solidar"),
      descriptor.expectedHash};
  const auto missing =
      resolveLinkedPartSourcesNative({missingDescriptor}, policy);
  CHECK(missing.status == LinkedPartSourceResolveStatus::Missing);
  CHECK(missing.sources.empty());

  CHECK(QDir(directory.path()).mkdir(QStringLiteral("not-a-file")));
  const LinkedPartSourceDescriptor directoryDescriptor{
      descriptor.key, QStringLiteral("not-a-file"), descriptor.expectedHash};
  const auto nonRegular =
      resolveLinkedPartSourcesNative({directoryDescriptor}, policy);
  CHECK(nonRegular.status ==
        LinkedPartSourceResolveStatus::SourceNotRegular);
  CHECK(nonRegular.sources.empty());

  // Same physical file through a second exact locator is deduplicated by
  // file identity and consumes the aggregate byte budget once.
  const QString aliasPath = directory.filePath(QStringLiteral("alias.solidar"));
#if defined(Q_OS_WIN)
  const bool linkedAlias =
      CreateHardLinkW(aliasPath.toStdWString().c_str(),
                      sourcePath.toStdWString().c_str(), nullptr) != FALSE;
#else
  const bool linkedAlias =
      ::link(QFile::encodeName(sourcePath).constData(),
             QFile::encodeName(aliasPath).constData()) == 0;
#endif
  if (linkedAlias) {
    policy.budgets.maximumTotalBytes = static_cast<std::uint64_t>(bytes.size());
    const LinkedPartSourceDescriptor aliasDescriptor{
        descriptor.key, QStringLiteral("alias.solidar"),
        descriptor.expectedHash};
    const auto aliases =
        resolveLinkedPartSourcesNative({descriptor, aliasDescriptor}, policy);
    CHECK(aliases.succeeded());
    CHECK(aliases.sources.size() == 1);
    CHECK(aliases.sources.front().descriptorIndexes.size() == 2);
  } else {
    std::cerr << "INFO: hard-link creation unavailable\n";
  }

  // If the platform permits an unprivileged symbolic link, final-handle
  // containment accepts an in-root target and rejects an out-of-root target.
  QTemporaryDir outside(
      QDir::current().filePath(QStringLiteral("linked-resolver-outside-XXXXXX")));
  CHECK(outside.isValid());
  const QString outsideFile = outside.filePath(QStringLiteral("outside.solidar"));
  {
    QFile file(outsideFile);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(file.write(bytes) == bytes.size());
  }
  const QString insideLink =
      directory.filePath(QStringLiteral("inside-link.solidar"));
  const QString outsideLink =
      directory.filePath(QStringLiteral("outside-link.solidar"));
#if defined(Q_OS_WIN)
  const bool linkedInside =
      CreateSymbolicLinkW(insideLink.toStdWString().c_str(),
                          sourcePath.toStdWString().c_str(),
                          SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
  const bool linkedOutside =
      CreateSymbolicLinkW(outsideLink.toStdWString().c_str(),
                          outsideFile.toStdWString().c_str(),
                          SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
#else
  const bool linkedInside =
      ::symlink(QFile::encodeName(sourcePath).constData(),
                QFile::encodeName(insideLink).constData()) == 0;
  const bool linkedOutside =
      ::symlink(QFile::encodeName(outsideFile).constData(),
                QFile::encodeName(outsideLink).constData()) == 0;
#endif
  if (linkedInside && linkedOutside) {
    const LinkedPartSourceDescriptor insideDescriptor{
        descriptor.key, QStringLiteral("inside-link.solidar"),
        descriptor.expectedHash};
    CHECK(resolveLinkedPartSourcesNative({insideDescriptor}, policy).succeeded());
    const LinkedPartSourceDescriptor outsideDescriptor{
        descriptor.key, QStringLiteral("outside-link.solidar"),
        descriptor.expectedHash};
    const auto escaped =
        resolveLinkedPartSourcesNative({outsideDescriptor}, policy);
    CHECK(escaped.status ==
          LinkedPartSourceResolveStatus::OutsideAllowedRoot);
    CHECK(escaped.sources.empty());
  } else {
    std::cerr << "INFO: unprivileged symlink creation unavailable\n";
  }
  return EXIT_SUCCESS;
}
