#include "TestAssertions.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <array>
#include <iostream>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV5Encoder.h"
#include "project/internal/ProjectFileRelocation.h"
#include "ui/application/ProjectApplicationService.h"
#include "ui/application/internal/ProductRelocationApplicationService.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using namespace solidar;

constexpr DefinitionId kRoot{4201};
constexpr ProjectId kProject{
    {0x23, 0x10, 0x20, 0x26, 0x7a, 0x41, 0x4f, 0x6b,
     0xa2, 0xb3, 0xc4, 0xd5, 0xe6, 0xf7, 0x18, 0x29}};

ProductDocumentIdentity identity() {
  return {kProject,
          {"relocation-product", "2026-10-23T00:00:00Z"}};
}

std::unique_ptr<ProductDocument> product(
    std::optional<ProductDocumentIdentity> productIdentity = identity()) {
  Document document;
  auto& sketch = document.addSketch(SketchId{730001}, "profile");
  sketch.geometry.addRectangle({-5.0, -3.0}, {5.0, 3.0});
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            kRoot, std::make_unique<Document>(std::move(document)))) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(
      std::move(definitions), kRoot, std::move(productIdentity));
  CHECK(created);
  return std::move(created.product);
}

QByteArray canonicalBytes(const ProductDocument& source) {
  CHECK(source.identity());
  const auto encoded = project::encodeProductEnvelopeV5Root(
      source, source.identity()->projectId,
      {QString::fromUtf8(source.identity()->metadata.nameUtf8),
       QString::fromUtf8(source.identity()->metadata.createdAtUtf8)});
  CHECK(encoded.succeeded());
  return encoded.payload->bytes;
}

QByteArray readAll(const QString& path) {
  QFile file(path);
  CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

void writeAll(const QString& path, QByteArrayView bytes) {
  QFile file(path);
  CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  CHECK(file.write(bytes.data(), bytes.size()) == bytes.size());
  file.close();
  CHECK(file.error() == QFileDevice::NoError);
}

class RecordingRelocation final
    : public project::detail::ProjectFileRelocationTransaction {
 public:
  project::detail::PreparedProjectFileRelocation prepare(
      const QString& sourcePath, const QString& targetPath) override {
    ++prepareCalls;
    source = sourcePath;
    target = targetPath;
    return prepared;
  }

  project::detail::ProjectFileRelocationResult commitNoReplace() override {
    ++commitCalls;
    return committed;
  }

  void cancel() noexcept override { ++cancelCalls; }

  int prepareCalls{};
  int commitCalls{};
  int cancelCalls{};
  QString source;
  QString target;
  project::detail::PreparedProjectFileRelocation prepared{
      {project::detail::ProjectFileRelocationFailure::None, {}}, {}};
  project::detail::ProjectFileRelocationResult committed{
      project::detail::ProjectFileRelocationFailure::None, {}};
};

ProjectApplicationErrorCode applicationCode(
    project::detail::ProjectFileRelocationFailure failure) {
  using Failure = project::detail::ProjectFileRelocationFailure;
  switch (failure) {
    case Failure::None:
      return ProjectApplicationErrorCode::None;
    case Failure::InvalidPath:
      return ProjectApplicationErrorCode::RelocateNotEligible;
    case Failure::SourceMissing:
    case Failure::SourceChanged:
      return ProjectApplicationErrorCode::RelocateSourceChanged;
    case Failure::SourceNotRegular:
    case Failure::AmbiguousPath:
    case Failure::MultipleLinks:
      return ProjectApplicationErrorCode::RelocateAmbiguousFileIdentity;
    case Failure::SameFile:
      return ProjectApplicationErrorCode::RelocateSameFile;
    case Failure::DestinationExists:
      return ProjectApplicationErrorCode::RelocateDestinationExists;
    case Failure::CrossVolume:
      return ProjectApplicationErrorCode::RelocateCrossVolume;
    case Failure::DestinationChanged:
      return ProjectApplicationErrorCode::RelocateDestinationChanged;
    case Failure::UnsupportedFilesystem:
      return ProjectApplicationErrorCode::RelocateUnsupportedFilesystem;
    case Failure::RenameFailed:
      return ProjectApplicationErrorCode::RelocateFailed;
  }
  return ProjectApplicationErrorCode::RelocateFailed;
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  static_assert(noexcept(std::declval<const ProjectApplicationService&>()
                             .relocateProduct(
                                 std::declval<const QString&>(),
                                 std::declval<const QString&>(),
                                 std::declval<const ProductDocument&>(),
                                 std::declval<const
                                     ProductRelocationPrecondition&>())));
  static_assert(noexcept(application::detail::relocateProduct(
      std::declval<const QString&>(), std::declval<const QString&>(),
      std::declval<const ProductDocument&>(),
      std::declval<const ProductRelocationPrecondition&>(),
      std::declval<project::detail::ProjectFileRelocationTransaction&>())));

  auto identified = product();
  const QByteArray expected = canonicalBytes(*identified);

  // Eligibility is rejected before touching the filesystem seam.
  {
    for (const ProductRelocationPrecondition precondition :
         std::array{ProductRelocationPrecondition{},
                    ProductRelocationPrecondition{true, SketchId{1}}}) {
      RecordingRelocation transaction;
      transaction.prepared.sourceBytes = expected;
      const auto result = application::detail::relocateProduct(
          QStringLiteral("source.solidar"), QStringLiteral("target.solidar"),
          *identified, precondition, transaction);
      CHECK(result.code == ProjectApplicationErrorCode::RelocateNotEligible);
      CHECK(transaction.prepareCalls == 0);
      CHECK(transaction.commitCalls == 0);
      CHECK(transaction.cancelCalls == 0);
    }
    auto identityless = product(std::nullopt);
    RecordingRelocation transaction;
    transaction.prepared.sourceBytes = expected;
    const auto result = application::detail::relocateProduct(
        QStringLiteral("source.solidar"), QStringLiteral("target.solidar"),
        *identityless, {true, std::nullopt}, transaction);
    CHECK(result.code == ProjectApplicationErrorCode::RelocateNotEligible);
    CHECK(transaction.prepareCalls == 0);
  }

  // Stable typed mapping for every prepare/commit failure. The application
  // boundary always cancels the seam, including a partially failed prepare.
  {
    using Failure = project::detail::ProjectFileRelocationFailure;
    constexpr std::array failures{
        Failure::InvalidPath,          Failure::SourceMissing,
        Failure::SourceNotRegular,     Failure::AmbiguousPath,
        Failure::MultipleLinks,        Failure::SameFile,
        Failure::DestinationExists,    Failure::CrossVolume,
        Failure::SourceChanged,        Failure::DestinationChanged,
        Failure::UnsupportedFilesystem, Failure::RenameFailed};
    for (const Failure value : failures) {
      RecordingRelocation transaction;
      transaction.prepared.status = {value, QStringLiteral("injected prepare")};
      const auto result = application::detail::relocateProduct(
          QStringLiteral("source.solidar"), QStringLiteral("target.solidar"),
          *identified, {true, std::nullopt}, transaction);
      CHECK(result.code == applicationCode(value));
      CHECK(!result.detail.isEmpty());
      CHECK(transaction.prepareCalls == 1);
      CHECK(transaction.commitCalls == 0);
      CHECK(transaction.cancelCalls == 1);
    }
  }
  {
    using Failure = project::detail::ProjectFileRelocationFailure;
    constexpr std::array failures{
        Failure::SourceChanged, Failure::DestinationChanged, Failure::CrossVolume,
        Failure::UnsupportedFilesystem, Failure::RenameFailed};
    for (const Failure value : failures) {
      RecordingRelocation transaction;
      transaction.prepared.sourceBytes = expected;
      transaction.committed = {value, QStringLiteral("injected commit")};
      const auto result = application::detail::relocateProduct(
          QStringLiteral("source.solidar"), QStringLiteral("target.solidar"),
          *identified, {true, std::nullopt}, transaction);
      CHECK(result.code == applicationCode(value));
      CHECK(transaction.prepareCalls == 1);
      CHECK(transaction.commitCalls == 1);
      CHECK(transaction.cancelCalls == 1);
    }
  }

  // The exact opened bytes must describe the current carrier. Mismatch fails
  // before commit and leaves the ProductDocument untouched.
  {
    RecordingRelocation transaction;
    transaction.prepared.sourceBytes = QByteArrayLiteral("{}");
    const ProductDocumentIdentity before = *identified->identity();
    const Document* documentAddress = &identified->rootDocument();
    const auto result = application::detail::relocateProduct(
        QStringLiteral("source.solidar"), QStringLiteral("target.solidar"),
        *identified, {true, std::nullopt}, transaction);
    CHECK(result.code == ProjectApplicationErrorCode::RelocateSourceChanged);
    CHECK(transaction.commitCalls == 0);
    CHECK(transaction.cancelCalls == 1);
    CHECK(identified->identity() == before);
    CHECK(&identified->rootDocument() == documentAddress);
  }

  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("product-relocation-service-XXXXXX")));
  CHECK(directory.isValid());
  ProjectApplicationService service;

#ifdef Q_OS_WIN
  const QString hardLinkProbeSource =
      directory.filePath(QStringLiteral("hardlink-probe-source.tmp"));
  const QString hardLinkProbeTarget =
      directory.filePath(QStringLiteral("hardlink-probe-target.tmp"));
  writeAll(hardLinkProbeSource, QByteArrayLiteral("hardlink-probe"));
  const HANDLE probeHandle = CreateFileW(
      reinterpret_cast<LPCWSTR>(hardLinkProbeSource.utf16()),
      FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(probeHandle != INVALID_HANDLE_VALUE);
  DWORD sourceVolumeFlags = 0;
  CHECK(GetVolumeInformationByHandleW(probeHandle, nullptr, 0, nullptr, nullptr,
                                      &sourceVolumeFlags, nullptr, 0));
  CHECK(CloseHandle(probeHandle));
  const bool sourceVolumeSupportsHardLinks =
      (sourceVolumeFlags & FILE_SUPPORTS_HARD_LINKS) != 0;
  const bool hardLinkCreationPermitted =
      CreateHardLinkW(reinterpret_cast<LPCWSTR>(hardLinkProbeTarget.utf16()),
                      reinterpret_cast<LPCWSTR>(hardLinkProbeSource.utf16()),
                      nullptr) != FALSE;
  if (hardLinkCreationPermitted) CHECK(QFile::remove(hardLinkProbeTarget));
  CHECK(QFile::remove(hardLinkProbeSource));

  // Exercise the real no-replace commit race rather than only its injected
  // status mapping: a destination created after prepare remains untouched.
  if (!sourceVolumeSupportsHardLinks) {
    const QString sourcePath =
        directory.filePath(QStringLiteral("native-race-source.solidar"));
    const QString targetPath =
        directory.filePath(QStringLiteral("native-race-target.solidar"));
    writeAll(sourcePath, expected);
    auto transaction =
        project::detail::makeProjectFileRelocationTransaction();
    auto prepared = transaction->prepare(sourcePath, targetPath);
    if (!prepared.succeeded())
      std::cerr << prepared.status.error.toStdString() << '\n';
    CHECK(prepared.succeeded());
    CHECK(prepared.sourceBytes == expected);
    const QByteArray sentinel("late-destination");
    writeAll(targetPath, sentinel);
    const auto committed = transaction->commitNoReplace();
    if (committed.failure !=
        project::detail::ProjectFileRelocationFailure::DestinationChanged)
      std::cerr << committed.error.toStdString() << '\n';
    CHECK(committed.failure ==
          project::detail::ProjectFileRelocationFailure::DestinationChanged);
    transaction->cancel();
    CHECK(readAll(sourcePath) == expected);
    CHECK(readAll(targetPath) == sentinel);
  }

  // Every target path component is leased through commit, so an absolute
  // no-replace rename cannot be redirected by swapping its parent directory.
  if (!sourceVolumeSupportsHardLinks) {
    const QString sourcePath =
        directory.filePath(QStringLiteral("anchored-source.solidar"));
    const QString originalParent =
        directory.filePath(QStringLiteral("anchored-parent"));
    const QString movedParent =
        directory.filePath(QStringLiteral("anchored-parent-moved"));
    CHECK(QDir().mkpath(originalParent));
    const QString targetPath =
        QDir(originalParent).filePath(QStringLiteral("target.solidar"));
    writeAll(sourcePath, expected);
    auto transaction =
        project::detail::makeProjectFileRelocationTransaction();
    CHECK(transaction->prepare(sourcePath, targetPath).succeeded());
    CHECK(!QDir().rename(originalParent, movedParent));
    const auto committed = transaction->commitNoReplace();
    if (!committed.succeeded())
      std::cerr << committed.error.toStdString() << '\n';
    CHECK(committed.succeeded());
    transaction->cancel();
    CHECK(!QFileInfo::exists(sourcePath));
    CHECK(readAll(targetPath) == expected);
    CHECK(!QFileInfo::exists(movedParent));
  }

  // There is no user-mode primitive that atomically couples link-count
  // validation to rename. Filesystems that advertise hard-link support must
  // therefore fail closed before any publication can occur.
  if (sourceVolumeSupportsHardLinks) {
    const QString sourcePath =
        directory.filePath(QStringLiteral("hardlink-source.solidar"));
    const QString targetPath =
        directory.filePath(QStringLiteral("hardlink-target.solidar"));
    const QString extraLink =
        directory.filePath(QStringLiteral("hardlink-extra.solidar"));
    writeAll(sourcePath, expected);
    auto transaction =
        project::detail::makeProjectFileRelocationTransaction();
    const auto prepared = transaction->prepare(sourcePath, targetPath);
    CHECK(prepared.status.failure ==
          project::detail::ProjectFileRelocationFailure::UnsupportedFilesystem);
    transaction->cancel();
    CHECK(readAll(sourcePath) == expected);
    CHECK(!QFileInfo::exists(targetPath));
    CHECK(!QFileInfo::exists(extraLink));
  }

  // A source that is already multiply linked is rejected before commit.
  if (hardLinkCreationPermitted) {
    const QString sourcePath =
        directory.filePath(QStringLiteral("existing-hardlink-source.solidar"));
    const QString targetPath = directory.filePath(
        QStringLiteral("existing-hardlink-target.solidar"));
    const QString extraLink =
        directory.filePath(QStringLiteral("existing-hardlink-extra.solidar"));
    writeAll(sourcePath, expected);
    CHECK(CreateHardLinkW(reinterpret_cast<LPCWSTR>(extraLink.utf16()),
                          reinterpret_cast<LPCWSTR>(sourcePath.utf16()),
                          nullptr));
    auto transaction =
        project::detail::makeProjectFileRelocationTransaction();
    const auto prepared = transaction->prepare(sourcePath, targetPath);
    CHECK(prepared.status.failure ==
          project::detail::ProjectFileRelocationFailure::MultipleLinks);
    transaction->cancel();
    CHECK(readAll(sourcePath) == expected);
    CHECK(readAll(extraLink) == expected);
    CHECK(!QFileInfo::exists(targetPath));
  }
  if (!hardLinkCreationPermitted)
    std::cerr << "INFO: hard-link creation is unavailable to the test process\n";
#endif

#ifndef Q_OS_WIN
  // Platforms without a primitive that both pins the inspected source object
  // and excludes concurrent writers fail closed before touching the file.
  {
    const QString sourcePath =
        directory.filePath(QStringLiteral("unsupported-source.solidar"));
    const QString targetPath =
        directory.filePath(QStringLiteral("unsupported-target.solidar"));
    writeAll(sourcePath, expected);
    auto transaction =
        project::detail::makeProjectFileRelocationTransaction();
    const auto prepared = transaction->prepare(sourcePath, targetPath);
    CHECK(prepared.status.failure ==
          project::detail::ProjectFileRelocationFailure::UnsupportedFilesystem);
    transaction->cancel();
    CHECK(readAll(sourcePath) == expected);
    CHECK(!QFileInfo::exists(targetPath));
  }
#endif

  // Production path: the held source object is renamed once, without a new
  // serialization. Bytes, identity and the live graph remain unchanged.
  {
    const QString sourcePath =
        directory.filePath(QStringLiteral("source.solidar"));
    const QString targetPath =
        directory.filePath(QStringLiteral("relocated.solidar"));
    CHECK(service.save(sourcePath, *identified).succeeded());
    const QByteArray beforeBytes = readAll(sourcePath);
    const auto beforeIdentity = identified->identity();
    const Document* documentAddress = &identified->rootDocument();
    const auto relocated = service.relocateProduct(
        sourcePath, targetPath, *identified, {true, std::nullopt});
#ifdef Q_OS_WIN
    if (sourceVolumeSupportsHardLinks) {
      CHECK(relocated.code ==
            ProjectApplicationErrorCode::RelocateUnsupportedFilesystem);
      CHECK(QFileInfo::exists(sourcePath));
      CHECK(!QFileInfo::exists(targetPath));
      CHECK(readAll(sourcePath) == beforeBytes);
      CHECK(identified->identity() == beforeIdentity);
      CHECK(&identified->rootDocument() == documentAddress);
    } else {
      if (!relocated.succeeded())
        std::cerr << relocated.detail.toStdString() << '\n';
      CHECK(relocated.succeeded());
      CHECK(!QFileInfo::exists(sourcePath));
      CHECK(QFileInfo::exists(targetPath));
      CHECK(readAll(targetPath) == beforeBytes);
      CHECK(identified->identity() == beforeIdentity);
      CHECK(&identified->rootDocument() == documentAddress);

      // An existing target is never overwritten.
      const QString occupied =
          directory.filePath(QStringLiteral("occupied.solidar"));
      const QByteArray sentinel("sentinel-destination");
      writeAll(occupied, sentinel);
      const auto occupiedResult = service.relocateProduct(
          targetPath, occupied, *identified, {true, std::nullopt});
      CHECK(occupiedResult.code ==
            ProjectApplicationErrorCode::RelocateDestinationExists);
      CHECK(QFileInfo::exists(targetPath));
      CHECK(readAll(targetPath) == beforeBytes);
      CHECK(readAll(occupied) == sentinel);

      const auto same = service.relocateProduct(
          targetPath, targetPath, *identified, {true, std::nullopt});
      CHECK(same.code == ProjectApplicationErrorCode::RelocateSameFile);
      CHECK(readAll(targetPath) == beforeBytes);

      // An externally replaced/non-equivalent source is rejected before rename.
      writeAll(targetPath, QByteArrayLiteral("{}"));
      const QString rejected =
          directory.filePath(QStringLiteral("rejected.solidar"));
      const auto changed = service.relocateProduct(
          targetPath, rejected, *identified, {true, std::nullopt});
      CHECK(changed.code == ProjectApplicationErrorCode::RelocateSourceChanged);
      CHECK(QFileInfo::exists(targetPath));
      CHECK(!QFileInfo::exists(rejected));
      CHECK(readAll(targetPath) == QByteArrayLiteral("{}"));

      // A valid standalone project is still not the current v5 source and must
      // not be relocated under the product identity.
      const QString standalone =
          directory.filePath(QStringLiteral("standalone-v3.solidar"));
      const QString standaloneTarget =
          directory.filePath(QStringLiteral("standalone-target.solidar"));
      CHECK(service.save(standalone, identified->rootDocument()).succeeded());
      const QByteArray standaloneBytes = readAll(standalone);
      const auto standaloneRejected = service.relocateProduct(
          standalone, standaloneTarget, *identified, {true, std::nullopt});
      CHECK(standaloneRejected.code ==
            ProjectApplicationErrorCode::RelocateSourceChanged);
      CHECK(readAll(standalone) == standaloneBytes);
      CHECK(!QFileInfo::exists(standaloneTarget));
    }
#else
    CHECK(relocated.code ==
          ProjectApplicationErrorCode::RelocateUnsupportedFilesystem);
    CHECK(QFileInfo::exists(sourcePath));
    CHECK(!QFileInfo::exists(targetPath));
    CHECK(readAll(sourcePath) == beforeBytes);
    CHECK(identified->identity() == beforeIdentity);
    CHECK(&identified->rootDocument() == documentAddress);
#endif
  }

  return EXIT_SUCCESS;
}
