#include "project/ProductEnvelopeV5Relocation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "project/ProductEnvelopeV5Decoder.h"
#include "project/ProductEnvelopeV5Encoder.h"
#include "project/ProjectFile.h"
#include "project/internal/ProductEnvelopeV5Relocation.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace solidar::project {
namespace {

using detail::PreparedProjectFileRelocation;
using detail::ProjectFileRelocationFailure;
using detail::ProjectFileRelocationResult;

ProductEnvelopeV5RelocationResult failure(
    ProductEnvelopeV5RelocationStatus status, QString error) {
  return {status, std::move(error)};
}

ProjectFileRelocationResult fileFailure(ProjectFileRelocationFailure failure,
                                        QString error) {
  return {failure, std::move(error)};
}

ProductEnvelopeV5RelocationStatus relocationStatus(
    ProjectFileRelocationFailure value) {
  switch (value) {
    case ProjectFileRelocationFailure::None:
      return ProductEnvelopeV5RelocationStatus::Relocated;
    case ProjectFileRelocationFailure::InvalidPath:
      return ProductEnvelopeV5RelocationStatus::InvalidPath;
    case ProjectFileRelocationFailure::SourceMissing:
      return ProductEnvelopeV5RelocationStatus::SourceMissing;
    case ProjectFileRelocationFailure::SourceNotRegular:
      return ProductEnvelopeV5RelocationStatus::SourceNotRegular;
    case ProjectFileRelocationFailure::AmbiguousPath:
      return ProductEnvelopeV5RelocationStatus::AmbiguousPath;
    case ProjectFileRelocationFailure::MultipleLinks:
      return ProductEnvelopeV5RelocationStatus::MultipleLinks;
    case ProjectFileRelocationFailure::SameFile:
      return ProductEnvelopeV5RelocationStatus::SameFile;
    case ProjectFileRelocationFailure::DestinationExists:
      return ProductEnvelopeV5RelocationStatus::DestinationExists;
    case ProjectFileRelocationFailure::CrossVolume:
      return ProductEnvelopeV5RelocationStatus::CrossVolume;
    case ProjectFileRelocationFailure::SourceChanged:
      return ProductEnvelopeV5RelocationStatus::SourceChanged;
    case ProjectFileRelocationFailure::DestinationChanged:
      return ProductEnvelopeV5RelocationStatus::DestinationChanged;
    case ProjectFileRelocationFailure::UnsupportedFilesystem:
      return ProductEnvelopeV5RelocationStatus::UnsupportedFilesystem;
    case ProjectFileRelocationFailure::RenameFailed:
      return ProductEnvelopeV5RelocationStatus::RenameFailed;
  }
  return ProductEnvelopeV5RelocationStatus::RenameFailed;
}

bool exactQStringFromUtf8(const std::string& bytes, QString* output) {
  const QByteArray encoded(bytes.data(), static_cast<qsizetype>(bytes.size()));
  const QString decoded = QString::fromUtf8(encoded);
  if (decoded.toUtf8() != encoded) return false;
  *output = decoded;
  return true;
}

QString cleanAbsolutePath(const QString& path) {
  return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool pathContainsAliasComponent(const QString& path) {
  QString current = cleanAbsolutePath(path);
  while (!current.isEmpty()) {
    const QFileInfo info(current);
    if (info.isSymLink() || info.isJunction()) return true;
    const QString parent = QDir::cleanPath(info.absolutePath());
    if (parent == current) break;
    current = parent;
  }
  return false;
}

bool lexicallySamePath(const QString& left, const QString& right) {
#ifdef Q_OS_WIN
  constexpr auto comparison = Qt::CaseInsensitive;
#else
  constexpr auto comparison = Qt::CaseSensitive;
#endif
  return cleanAbsolutePath(left).compare(cleanAbsolutePath(right), comparison) ==
         0;
}

#ifdef Q_OS_WIN

QString extendedWindowsPath(const QString& path) {
  QString native = QDir::toNativeSeparators(cleanAbsolutePath(path));
  if (native.startsWith(QStringLiteral("\\\\?\\"))) return native;
  if (native.startsWith(QStringLiteral("\\\\")))
    return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
  return QStringLiteral("\\\\?\\") + native;
}

QString windowsError(const QString& operation, DWORD code = GetLastError()) {
  return QStringLiteral("%1 (Win32 error %2)").arg(operation).arg(code);
}

using NtStatus = LONG;

struct NativeIoStatusBlock {
  union {
    NtStatus status;
    void* pointer;
  };
  ULONG_PTR information;
};

struct NativeUnicodeString {
  USHORT length;
  USHORT maximumLength;
  PWSTR buffer;
};

struct NativeObjectAttributes {
  ULONG length;
  HANDLE rootDirectory;
  NativeUnicodeString* objectName;
  ULONG attributes;
  void* securityDescriptor;
  void* securityQualityOfService;
};

using NtCreateFileFunction = NtStatus(NTAPI*)(
    HANDLE*, ACCESS_MASK, NativeObjectAttributes*, NativeIoStatusBlock*,
    LARGE_INTEGER*, ULONG, ULONG, ULONG, ULONG, void*, ULONG);
using RtlNtStatusToDosErrorFunction = ULONG(NTAPI*)(NtStatus);

constexpr ULONG kNativeObjectCaseInsensitive = 0x00000040;
constexpr ULONG kNativeFileOpen = 0x00000001;
constexpr ULONG kNativeFileCreate = 0x00000002;
constexpr ULONG kNativeFileDirectory = 0x00000001;
constexpr ULONG kNativeFileSynchronousIoNonAlert = 0x00000020;
constexpr ULONG kNativeFileNonDirectory = 0x00000040;
constexpr ULONG kNativeFileOpenForBackupIntent = 0x00004000;
constexpr ULONG kNativeFileOpenReparsePoint = 0x00200000;
constexpr ULONG kNativeFileDeleteOnClose = 0x00001000;

DWORD dosErrorFromStatus(NtStatus status) {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) return ERROR_GEN_FAILURE;
  const auto convert = reinterpret_cast<RtlNtStatusToDosErrorFunction>(
      GetProcAddress(ntdll, "RtlNtStatusToDosError"));
  return convert == nullptr ? ERROR_GEN_FAILURE : convert(status);
}

NtCreateFileFunction nativeCreateFile() {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  return ntdll == nullptr
             ? nullptr
             : reinterpret_cast<NtCreateFileFunction>(
                   GetProcAddress(ntdll, "NtCreateFile"));
}

class UniqueWinHandle final {
 public:
  explicit UniqueWinHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
      : handle_(handle) {}
  ~UniqueWinHandle() { reset(); }
  UniqueWinHandle(const UniqueWinHandle&) = delete;
  UniqueWinHandle& operator=(const UniqueWinHandle&) = delete;
  UniqueWinHandle(UniqueWinHandle&& other) noexcept
      : handle_(other.release()) {}
  UniqueWinHandle& operator=(UniqueWinHandle&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] HANDLE release() noexcept {
    return std::exchange(handle_, INVALID_HANDLE_VALUE);
  }
  void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
    if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr)
      CloseHandle(handle_);
    handle_ = handle;
  }

 private:
  HANDLE handle_;
};

NtStatus openRelative(NtCreateFileFunction createFile, HANDLE root,
                      const QString& name, ACCESS_MASK desiredAccess,
                      ULONG shareAccess, ULONG createOptions,
                      HANDLE* opened,
                      ULONG createDisposition = kNativeFileOpen,
                      ULONG fileAttributes = FILE_ATTRIBUTE_NORMAL) {
  NativeUnicodeString unicode{
      static_cast<USHORT>(name.size() * sizeof(wchar_t)),
      static_cast<USHORT>(name.size() * sizeof(wchar_t)),
      const_cast<PWSTR>(reinterpret_cast<LPCWSTR>(name.utf16()))};
  NativeObjectAttributes attributes{sizeof(NativeObjectAttributes),
                                    root,
                                    &unicode,
                                    kNativeObjectCaseInsensitive,
                                    nullptr,
                                    nullptr};
  NativeIoStatusBlock ioStatus{};
  *opened = INVALID_HANDLE_VALUE;
  return createFile(opened, desiredAccess, &attributes, &ioStatus, nullptr,
                    fileAttributes, shareAccess, createDisposition,
                    createOptions, nullptr, 0);
}

ProjectFileRelocationResult openAnchoredDirectory(
    const QString& path, DWORD shareAccess, HANDLE* opened,
    std::vector<HANDLE>* retained = nullptr) {
  *opened = INVALID_HANDLE_VALUE;
  const auto createFile = nativeCreateFile();
  if (createFile == nullptr)
    return fileFailure(ProjectFileRelocationFailure::UnsupportedFilesystem,
                       QStringLiteral("Handle-relative path traversal is unavailable."));

  const QString native = QDir::toNativeSeparators(cleanAbsolutePath(path));
  if (native.size() < 3 || native.at(1) != QLatin1Char(':') ||
      native.at(2) != QLatin1Char('\\'))
    return fileFailure(ProjectFileRelocationFailure::AmbiguousPath,
                       QStringLiteral("Only anchored local drive paths are supported."));

  const QString driveRoot = native.left(3);
  const QString rootPath = extendedWindowsPath(driveRoot);
  if (GetDriveTypeW(reinterpret_cast<LPCWSTR>(driveRoot.utf16())) !=
      DRIVE_FIXED)
    return fileFailure(ProjectFileRelocationFailure::AmbiguousPath,
                       QStringLiteral(
                           "Only fixed local volumes can be relocated."));
  std::array<wchar_t, MAX_PATH + 1> volumeName{};
  if (!GetVolumeNameForVolumeMountPointW(
          reinterpret_cast<LPCWSTR>(driveRoot.utf16()), volumeName.data(),
          static_cast<DWORD>(volumeName.size())))
    return fileFailure(ProjectFileRelocationFailure::AmbiguousPath,
                       QStringLiteral(
                           "Mapped or substituted drive paths are not supported."));

  UniqueWinHandle current(CreateFileW(
      reinterpret_cast<LPCWSTR>(rootPath.utf16()),
      FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      shareAccess, nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (current.get() == INVALID_HANDLE_VALUE)
    return fileFailure(ProjectFileRelocationFailure::AmbiguousPath,
                       windowsError(QStringLiteral("Cannot open path root")));

  BY_HANDLE_FILE_INFORMATION rootInformation{};
  if (!GetFileInformationByHandle(current.get(), &rootInformation) ||
      (rootInformation.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (rootInformation.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return fileFailure(ProjectFileRelocationFailure::AmbiguousPath,
                       QStringLiteral("Relocation path root is ambiguous."));
  }

  const QStringList components =
      native.mid(3).split(QLatin1Char('\\'), Qt::SkipEmptyParts);
  if (retained != nullptr) {
    retained->reserve(static_cast<std::size_t>(components.size()) + 1);
  }
  for (const QString& component : components) {
    HANDLE nextRaw = INVALID_HANDLE_VALUE;
    const NtStatus status = openRelative(
        createFile, current.get(), component,
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        shareAccess,
        kNativeFileDirectory | kNativeFileSynchronousIoNonAlert |
            kNativeFileOpenForBackupIntent | kNativeFileOpenReparsePoint,
        &nextRaw);
    UniqueWinHandle next(nextRaw);
    if (status < 0)
      return fileFailure(ProjectFileRelocationFailure::AmbiguousPath,
                         windowsError(QStringLiteral(
                             "Cannot traverse relocation path"),
                                      dosErrorFromStatus(status)));

    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(next.get(), &information) ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return fileFailure(ProjectFileRelocationFailure::AmbiguousPath,
                         QStringLiteral(
                             "Relocation path contains an ambiguous directory."));
    }
    if (retained != nullptr) retained->push_back(current.get());
    static_cast<void>(current.release());
    current = std::move(next);
  }

  if (retained != nullptr) retained->push_back(current.get());
  *opened = current.release();
  return {ProjectFileRelocationFailure::None, {}};
}

class NativeProjectFileRelocationTransaction final
    : public detail::ProjectFileRelocationTransaction {
 public:
  ~NativeProjectFileRelocationTransaction() override { cancel(); }

  PreparedProjectFileRelocation prepare(const QString& sourcePath,
                                        const QString& targetPath) override {
    cancel();
    if (sourcePath.isEmpty() || targetPath.isEmpty())
      return {fileFailure(ProjectFileRelocationFailure::InvalidPath,
                          QStringLiteral("Relocation paths must not be empty.")),
              {}};
    if (lexicallySamePath(sourcePath, targetPath))
      return {fileFailure(ProjectFileRelocationFailure::SameFile,
                          QStringLiteral("Source and destination are the same path.")),
              {}};

    const QFileInfo sourceInfo(sourcePath);
    const QFileInfo targetInfo(targetPath);
    const QFileInfo targetParentInfo(targetInfo.absolutePath());
    if (sourceInfo.fileName().isEmpty() || targetInfo.fileName().isEmpty())
      return {fileFailure(ProjectFileRelocationFailure::InvalidPath,
                          QStringLiteral("Relocation file name is empty.")),
              {}};
    if (pathContainsAliasComponent(sourcePath) ||
        pathContainsAliasComponent(targetInfo.absolutePath()) ||
        targetInfo.isSymLink() || targetInfo.isJunction()) {
      return {fileFailure(
                  ProjectFileRelocationFailure::AmbiguousPath,
                  QStringLiteral("Reparse, junction, or symbolic-link paths are not supported.")),
              {}};
    }
    if (!targetParentInfo.exists() || !targetParentInfo.isDir())
      return {fileFailure(ProjectFileRelocationFailure::InvalidPath,
                          QStringLiteral("Destination parent directory is invalid.")),
              {}};
    if (targetInfo.exists())
      return {fileFailure(ProjectFileRelocationFailure::DestinationExists,
                          QStringLiteral("Destination already exists.")),
              {}};

    HANDLE sourceParent = INVALID_HANDLE_VALUE;
    auto openedSourceParent =
        openAnchoredDirectory(
            sourceInfo.absolutePath(),
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &sourceParent, &sourceNamespace_);
    if (!openedSourceParent.succeeded())
      return {std::move(openedSourceParent), {}};
    const QString absoluteSource = extendedWindowsPath(sourcePath);
    source_ = CreateFileW(
        reinterpret_cast<LPCWSTR>(absoluteSource.utf16()),
        FILE_GENERIC_READ | DELETE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (source_ == INVALID_HANDLE_VALUE) {
      const DWORD code = GetLastError();
      return preparedFailure(
          code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
              ? ProjectFileRelocationFailure::SourceMissing
              : ProjectFileRelocationFailure::RenameFailed,
          windowsError(QStringLiteral("Cannot open relocation source"), code));
    }
    const auto createFile = nativeCreateFile();

    BY_HANDLE_FILE_INFORMATION sourceFileInfo{};
    if (!GetFileInformationByHandle(source_, &sourceFileInfo))
      return preparedFailure(ProjectFileRelocationFailure::RenameFailed,
                             windowsError(QStringLiteral(
                                 "Cannot inspect relocation source")));
    if ((sourceFileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
      return preparedFailure(ProjectFileRelocationFailure::SourceNotRegular,
                             QStringLiteral("Relocation source is not a regular file."));
    if ((sourceFileInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
      return preparedFailure(ProjectFileRelocationFailure::AmbiguousPath,
                             QStringLiteral("Relocation source is a reparse point."));
    if (sourceFileInfo.nNumberOfLinks != 1)
      return preparedFailure(ProjectFileRelocationFailure::MultipleLinks,
                             QStringLiteral("Hard-linked project files cannot be relocated."));
    DWORD fileSystemFlags = 0;
    if (!GetVolumeInformationByHandleW(source_, nullptr, 0, nullptr, nullptr,
                                       &fileSystemFlags, nullptr, 0))
      return preparedFailure(ProjectFileRelocationFailure::UnsupportedFilesystem,
                             windowsError(QStringLiteral(
                                 "Cannot inspect source filesystem capabilities")));
    if ((fileSystemFlags & FILE_SUPPORTS_HARD_LINKS) != 0)
      return preparedFailure(
          ProjectFileRelocationFailure::UnsupportedFilesystem,
          QStringLiteral(
              "Relocation is unavailable on filesystems that permit concurrent hard-link aliases."));
    // Retain every handle-relative, non-reparse component with delete sharing
    // denied so that the namespace cannot be renamed/replaced.
    auto openedTargetParent =
        openAnchoredDirectory(targetInfo.absolutePath(),
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &targetParent_, &targetNamespace_);
    if (!openedTargetParent.succeeded())
      return preparedFailure(openedTargetParent.failure,
                             std::move(openedTargetParent.error));

    BY_HANDLE_FILE_INFORMATION parentFileInfo{};
    if (!GetFileInformationByHandle(targetParent_, &parentFileInfo))
      return preparedFailure(ProjectFileRelocationFailure::InvalidPath,
                             windowsError(QStringLiteral(
                                 "Cannot inspect destination parent")));
    if ((parentFileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (parentFileInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
      return preparedFailure(ProjectFileRelocationFailure::AmbiguousPath,
                             QStringLiteral("Destination parent is not an unambiguous directory."));
    if (sourceFileInfo.dwVolumeSerialNumber !=
        parentFileInfo.dwVolumeSerialNumber)
      return preparedFailure(ProjectFileRelocationFailure::CrossVolume,
                             QStringLiteral("Cross-volume relocation is not supported."));
    HANDLE existingTarget = INVALID_HANDLE_VALUE;
    const NtStatus targetStatus = openRelative(
        createFile, targetParent_, targetInfo.fileName(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        kNativeFileNonDirectory | kNativeFileOpenReparsePoint,
        &existingTarget);
    if (targetStatus >= 0) {
      CloseHandle(existingTarget);
      return preparedFailure(ProjectFileRelocationFailure::DestinationExists,
                             QStringLiteral("Destination already exists."));
    }
    const DWORD targetError = dosErrorFromStatus(targetStatus);
    if (targetError != ERROR_FILE_NOT_FOUND &&
        targetError != ERROR_PATH_NOT_FOUND)
      return preparedFailure(ProjectFileRelocationFailure::AmbiguousPath,
                             windowsError(QStringLiteral(
                                 "Cannot prove destination absence"),
                                 targetError));

    // An absolute Win32 rename is the broadly supported no-replace primitive.
    // Keep the validated target directory non-empty until commit so Windows
    // cannot turn it into a reparse point, while the retained component
    // handles (without delete sharing) pin every directory name. The guard is
    // handle-relative and DELETE_ON_CLOSE, so cancellation and process exit
    // both remove it.
    const QString guardName =
        QStringLiteral(".solidarcad-relocation-%1.lock")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const NtStatus guardStatus = openRelative(
        createFile, targetParent_, guardName, DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        kNativeFileNonDirectory | kNativeFileSynchronousIoNonAlert |
            kNativeFileOpenForBackupIntent | kNativeFileOpenReparsePoint |
            kNativeFileDeleteOnClose,
        &targetDirectoryGuard_, kNativeFileCreate,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY);
    if (guardStatus < 0)
      return preparedFailure(ProjectFileRelocationFailure::AmbiguousPath,
                             windowsError(QStringLiteral(
                                 "Cannot pin destination directory"),
                                          dosErrorFromStatus(guardStatus)));
    BY_HANDLE_FILE_INFORMATION guardedParentInfo{};
    if (!GetFileInformationByHandle(targetParent_, &guardedParentInfo) ||
        (guardedParentInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) !=
            0 ||
        guardedParentInfo.dwVolumeSerialNumber !=
            parentFileInfo.dwVolumeSerialNumber ||
        guardedParentInfo.nFileIndexHigh != parentFileInfo.nFileIndexHigh ||
        guardedParentInfo.nFileIndexLow != parentFileInfo.nFileIndexLow)
      return preparedFailure(ProjectFileRelocationFailure::AmbiguousPath,
                             QStringLiteral(
                                 "Destination parent changed while being pinned."));

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(source_, &size) || size.QuadPart < 0 ||
        size.QuadPart > ProjectFile::kMaximumFileBytes)
      return preparedFailure(ProjectFileRelocationFailure::SourceChanged,
                             QStringLiteral("Relocation source size is invalid."));
    QByteArray bytes(static_cast<qsizetype>(size.QuadPart), Qt::Uninitialized);
    qsizetype offset = 0;
    while (offset < bytes.size()) {
      DWORD read = 0;
      const DWORD requested = static_cast<DWORD>(
          std::min<qsizetype>(bytes.size() - offset, 1024 * 1024));
      if (!ReadFile(source_, bytes.data() + offset, requested, &read, nullptr)) {
        return preparedFailure(ProjectFileRelocationFailure::SourceChanged,
                               windowsError(QStringLiteral(
                                   "Cannot read relocation source")));
      }
      if (read == 0)
        return preparedFailure(ProjectFileRelocationFailure::SourceChanged,
                               QStringLiteral(
                                   "Relocation source changed while reading."));
      offset += static_cast<qsizetype>(read);
    }

    targetRenamePath_ =
        QDir::toNativeSeparators(cleanAbsolutePath(targetPath));
    prepared_ = true;
    return {{ProjectFileRelocationFailure::None, {}}, std::move(bytes)};
  }

  ProjectFileRelocationResult commitNoReplace() override {
    if (!prepared_ || source_ == INVALID_HANDLE_VALUE ||
        targetParent_ == INVALID_HANDLE_VALUE ||
        targetDirectoryGuard_ == INVALID_HANDLE_VALUE ||
        targetRenamePath_.isEmpty())
      return fileFailure(ProjectFileRelocationFailure::RenameFailed,
                         QStringLiteral("Relocation transaction is not prepared."));

    BY_HANDLE_FILE_INFORMATION sourceInformation{};
    if (!GetFileInformationByHandle(source_, &sourceInformation) ||
        (sourceInformation.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        sourceInformation.nNumberOfLinks != 1)
      return fileFailure(ProjectFileRelocationFailure::SourceChanged,
                         QStringLiteral(
                             "Relocation source identity changed before commit."));
    const DWORD nameBytes =
        static_cast<DWORD>(targetRenamePath_.size() * sizeof(wchar_t));
    const std::size_t bufferSize =
        sizeof(FILE_RENAME_INFO) + static_cast<std::size_t>(nameBytes);
    auto buffer = std::make_unique<std::byte[]>(bufferSize);
    std::memset(buffer.get(), 0, bufferSize);
    auto* renameInfo = reinterpret_cast<FILE_RENAME_INFO*>(buffer.get());
    renameInfo->ReplaceIfExists = FALSE;
    renameInfo->RootDirectory = nullptr;
    renameInfo->FileNameLength = nameBytes;
    std::memcpy(renameInfo->FileName, targetRenamePath_.utf16(), nameBytes);
    if (!SetFileInformationByHandle(source_, FileRenameInfo, renameInfo,
                                    static_cast<DWORD>(bufferSize))) {
      const DWORD code = GetLastError();
      if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS)
        return fileFailure(ProjectFileRelocationFailure::DestinationChanged,
                           windowsError(QStringLiteral(
                               "Destination appeared before relocation commit"),
                                        code));
      if (code == ERROR_NOT_SAME_DEVICE)
        return fileFailure(ProjectFileRelocationFailure::CrossVolume,
                           windowsError(QStringLiteral(
                               "Cross-volume relocation is not supported"),
                                        code));
      if (code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED)
        return fileFailure(ProjectFileRelocationFailure::UnsupportedFilesystem,
                           windowsError(QStringLiteral(
                               "Atomic no-replace relocation is unsupported"),
                                        code));
      return fileFailure(ProjectFileRelocationFailure::RenameFailed,
                         windowsError(QStringLiteral("Relocation failed"), code));
    }
    prepared_ = false;
    return {ProjectFileRelocationFailure::None, {}};
  }

  void cancel() noexcept override {
    if (source_ != INVALID_HANDLE_VALUE) CloseHandle(source_);
    for (const HANDLE handle : sourceNamespace_) CloseHandle(handle);
    if (targetDirectoryGuard_ != INVALID_HANDLE_VALUE)
      CloseHandle(targetDirectoryGuard_);
    for (const HANDLE handle : targetNamespace_) CloseHandle(handle);
    source_ = INVALID_HANDLE_VALUE;
    sourceNamespace_.clear();
    targetDirectoryGuard_ = INVALID_HANDLE_VALUE;
    targetParent_ = INVALID_HANDLE_VALUE;
    targetNamespace_.clear();
    targetRenamePath_.clear();
    prepared_ = false;
  }

 private:
  PreparedProjectFileRelocation preparedFailure(
      ProjectFileRelocationFailure failure, QString error) {
    cancel();
    return {fileFailure(failure, std::move(error)), {}};
  }

  HANDLE source_{INVALID_HANDLE_VALUE};
  std::vector<HANDLE> sourceNamespace_;
  HANDLE targetParent_{INVALID_HANDLE_VALUE};
  HANDLE targetDirectoryGuard_{INVALID_HANDLE_VALUE};
  std::vector<HANDLE> targetNamespace_;
  QString targetRenamePath_;
  bool prepared_{false};
};

#else

class NativeProjectFileRelocationTransaction final
    : public detail::ProjectFileRelocationTransaction {
 public:
  PreparedProjectFileRelocation prepare(const QString&,
                                        const QString&) override {
    return {fileFailure(
                ProjectFileRelocationFailure::UnsupportedFilesystem,
                QStringLiteral(
                    "Atomic no-replace relocation is unsupported on this platform.")),
            {}};
  }
  ProjectFileRelocationResult commitNoReplace() override {
    return fileFailure(ProjectFileRelocationFailure::UnsupportedFilesystem,
                       QStringLiteral(
                           "Atomic no-replace relocation is unsupported on this platform."));
  }
  void cancel() noexcept override {}
};

#endif

ProductEnvelopeV5RelocationResult relocateImpl(
    const QString& sourcePath, const QString& targetPath,
    const ProductDocument& productDocument,
    detail::ProjectFileRelocationTransaction& transaction) noexcept {
  try {
    if (!productDocument.identity() ||
        !productDocument.identity()->projectId.isValid() ||
        !productDocument.hasSingleValidRootDefinition())
      return failure(ProductEnvelopeV5RelocationStatus::InvalidProduct,
                     QStringLiteral("Relocation requires one identified product root."));

    QString name;
    QString createdAt;
    if (!exactQStringFromUtf8(productDocument.identity()->metadata.nameUtf8,
                              &name) ||
        !exactQStringFromUtf8(
            productDocument.identity()->metadata.createdAtUtf8, &createdAt))
      return failure(ProductEnvelopeV5RelocationStatus::InvalidProduct,
                     QStringLiteral("Product metadata is not exact UTF-8."));

    const ProductEnvelopeV5Metadata metadata{name, createdAt};
    auto encoded = encodeProductEnvelopeV5Root(
        productDocument, productDocument.identity()->projectId, metadata);
    if (!encoded.succeeded())
      return failure(ProductEnvelopeV5RelocationStatus::InvalidProduct,
                     encoded.error.isEmpty()
                         ? QStringLiteral("Cannot encode current product state.")
                         : std::move(encoded.error));

    auto prepared = transaction.prepare(sourcePath, targetPath);
    if (!prepared.succeeded()) {
      transaction.cancel();
      return failure(relocationStatus(prepared.status.failure),
                     std::move(prepared.status.error));
    }

    auto scanned = scanProductEnvelopeV5(QByteArrayView(prepared.sourceBytes));
    if (scanned.status != ProductEnvelopeV5ScanStatus::Candidate ||
        !scanned.candidate ||
        scanned.candidate->authoredRoot != encoded.payload->root) {
      transaction.cancel();
      return failure(ProductEnvelopeV5RelocationStatus::SourceChanged,
                     scanned.error.isEmpty()
                         ? QStringLiteral(
                               "On-disk product does not match the current clean document.")
                         : std::move(scanned.error));
    }

    auto committed = transaction.commitNoReplace();
    if (!committed.succeeded()) {
      transaction.cancel();
      return failure(relocationStatus(committed.failure),
                     std::move(committed.error));
    }
    transaction.cancel();
    return {ProductEnvelopeV5RelocationStatus::Relocated, {}};
  } catch (const std::exception& exception) {
    transaction.cancel();
    return failure(ProductEnvelopeV5RelocationStatus::RenameFailed,
                   QString::fromUtf8(exception.what()));
  } catch (...) {
    transaction.cancel();
    return failure(ProductEnvelopeV5RelocationStatus::RenameFailed,
                   QStringLiteral("Unknown relocation failure."));
  }
}

}  // namespace

bool productEnvelopeV5RelocationIsSupported() noexcept {
#ifdef Q_OS_WIN
  return true;
#else
  return false;
#endif
}

ProductEnvelopeV5RelocationResult relocateProductEnvelopeV5(
    const QString& sourcePath, const QString& targetPath,
    const ProductDocument& productDocument) noexcept {
  try {
    auto transaction = detail::makeProjectFileRelocationTransaction();
    return relocateImpl(sourcePath, targetPath, productDocument, *transaction);
  } catch (const std::exception& exception) {
    return failure(ProductEnvelopeV5RelocationStatus::RenameFailed,
                   QString::fromUtf8(exception.what()));
  } catch (...) {
    return failure(ProductEnvelopeV5RelocationStatus::RenameFailed,
                   QStringLiteral("Unknown relocation setup failure."));
  }
}

}  // namespace solidar::project

namespace solidar::project::detail {

std::unique_ptr<ProjectFileRelocationTransaction>
makeProjectFileRelocationTransaction() {
  return std::make_unique<NativeProjectFileRelocationTransaction>();
}

ProductEnvelopeV5RelocationResult relocateProductEnvelopeV5(
    const QString& sourcePath, const QString& targetPath,
    const ProductDocument& productDocument,
    ProjectFileRelocationTransaction& transaction) noexcept {
  return relocateImpl(sourcePath, targetPath, productDocument, transaction);
}

}  // namespace solidar::project::detail
