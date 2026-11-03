#include "project/internal/LinkedPartSourceFileSystem.h"

#include <QFile>
#include <QDir>
#include <QtGlobal>

#include <array>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace solidar::project::detail {
namespace {

LinkedSourceOpenedFile failed(LinkedSourceOpenStatus status, QString error,
                              std::uint64_t work = 1) {
  LinkedSourceOpenedFile result;
  result.status = status;
  result.workUnits = work;
  result.error = std::move(error);
  return result;
}

#if defined(Q_OS_WIN)

class Handle final {
 public:
  Handle() noexcept = default;
  explicit Handle(HANDLE value) noexcept : value_(value) {}
  ~Handle() {
    if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value_(other.value_) {
    other.value_ = INVALID_HANDLE_VALUE;
  }
  Handle& operator=(Handle&& other) noexcept {
    if (this == &other) return *this;
    if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    value_ = other.value_;
    other.value_ = INVALID_HANDLE_VALUE;
    return *this;
  }
  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] HANDLE release() noexcept {
    const HANDLE result = value_;
    value_ = INVALID_HANDLE_VALUE;
    return result;
  }

 private:
  HANDLE value_{INVALID_HANDLE_VALUE};
};

QString canonicalPathFor(HANDLE handle) {
  const DWORD needed = GetFinalPathNameByHandleW(
      handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (needed == 0) return {};
  std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
  const DWORD written = GetFinalPathNameByHandleW(
      handle, buffer.data(), needed,
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (written == 0 || written >= needed) return {};
  buffer.resize(written);
  QString result = QString::fromWCharArray(buffer.data(),
                                           static_cast<qsizetype>(written));
  if (result.startsWith(QStringLiteral("\\\\?\\UNC\\"),
                        Qt::CaseInsensitive)) {
    result = QStringLiteral("//") + result.sliced(8);
  } else if (result.startsWith(QStringLiteral("\\\\?\\"))) {
    result = result.sliced(4);
  }
  return result;
}

QString identityFor(const BY_HANDLE_FILE_INFORMATION& info) {
  return QStringLiteral("%1:%2:%3")
      .arg(info.dwVolumeSerialNumber, 8, 16, QLatin1Char('0'))
      .arg(info.nFileIndexHigh, 8, 16, QLatin1Char('0'))
      .arg(info.nFileIndexLow, 8, 16, QLatin1Char('0'));
}

QString versionFor(const BY_HANDLE_FILE_INFORMATION& info) {
  return QStringLiteral("%1:%2:%3:%4")
      .arg(info.nFileSizeHigh, 8, 16, QLatin1Char('0'))
      .arg(info.nFileSizeLow, 8, 16, QLatin1Char('0'))
      .arg(info.ftLastWriteTime.dwHighDateTime, 8, 16, QLatin1Char('0'))
      .arg(info.ftLastWriteTime.dwLowDateTime, 8, 16, QLatin1Char('0'));
}

bool consumeWindowsWork(std::uint64_t count,
                        std::uint64_t maximumWorkUnits,
                        std::uint64_t* workUnits);

class WindowsReadHandle final : public LinkedSourceReadHandle {
 public:
  WindowsReadHandle(Handle&& handle, WindowsWalkerObjectInfo before)
      : handle_(std::move(handle)), before_(std::move(before)) {}

  LinkedSourceReadResult readBounded(std::uint64_t maximumBytes,
                                     std::uint64_t maximumWorkUnits) override {
    LinkedSourceReadResult result;
    const std::uint64_t size = before_.sizeBytes;
    if (size > maximumBytes ||
        size > static_cast<std::uint64_t>((std::numeric_limits<int>::max)())) {
      result.status = LinkedSourceOpenStatus::BudgetExceeded;
      result.error = QStringLiteral("linked source exceeds remaining byte budget");
      return result;
    }
    result.bytes = QByteArray(static_cast<qsizetype>(size), Qt::Uninitialized);
    std::uint64_t offset = 0;
    while (offset < size) {
      if (!consumeWindowsWork(1, maximumWorkUnits, &result.workUnits)) {
        result.status = LinkedSourceOpenStatus::BudgetExceeded;
        result.error = QStringLiteral("filesystem work budget exhausted");
        return result;
      }
      const DWORD request = static_cast<DWORD>((std::min<std::uint64_t>)(
          size - offset, (std::numeric_limits<DWORD>::max)()));
      DWORD read = 0;
      if (!ReadFile(handle_.get(),
                    result.bytes.data() + static_cast<qsizetype>(offset),
                    request, &read, nullptr)) {
        result.status = LinkedSourceOpenStatus::ResourceFailure;
        result.error = QStringLiteral("cannot read linked source");
        return result;
      }
      if (read == 0) {
        result.status = LinkedSourceOpenStatus::SourceChanged;
        result.error = QStringLiteral("linked source changed while reading");
        return result;
      }
      offset += read;
    }
    if (!consumeWindowsWork(1, maximumWorkUnits, &result.workUnits)) {
      result.status = LinkedSourceOpenStatus::BudgetExceeded;
      result.error = QStringLiteral("filesystem work budget exhausted");
      return result;
    }
    BY_HANDLE_FILE_INFORMATION after{};
    if (!GetFileInformationByHandle(handle_.get(), &after) ||
        before_.identity != identityFor(after) ||
        before_.version != versionFor(after)) {
      result.status = LinkedSourceOpenStatus::SourceChanged;
      result.error = QStringLiteral("linked source changed while reading");
      return result;
    }
    result.status = LinkedSourceOpenStatus::Opened;
    return result;
  }

 private:
  Handle handle_;
  WindowsWalkerObjectInfo before_;
};

struct ReparseTarget {
  QString path;
  bool relative{};
};

std::optional<ReparseTarget> reparseTarget(HANDLE handle) {
  struct ReparseBuffer {
    ULONG tag;
    USHORT dataLength;
    USHORT reserved;
    union {
      struct {
        USHORT substituteOffset;
        USHORT substituteLength;
        USHORT printOffset;
        USHORT printLength;
        ULONG flags;
        WCHAR path[1];
      } symbolic;
      struct {
        USHORT substituteOffset;
        USHORT substituteLength;
        USHORT printOffset;
        USHORT printLength;
        WCHAR path[1];
      } mount;
    } data;
  };
  std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> storage{};
  DWORD returned = 0;
  if (!DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0,
                       storage.data(), static_cast<DWORD>(storage.size()),
                       &returned, nullptr)) {
    return std::nullopt;
  }
  const auto* data = reinterpret_cast<const ReparseBuffer*>(storage.data());
  const WCHAR* buffer = nullptr;
  USHORT offset = 0;
  USHORT length = 0;
  if (data->tag == IO_REPARSE_TAG_SYMLINK) {
    buffer = data->data.symbolic.path;
    offset = data->data.symbolic.substituteOffset;
    length = data->data.symbolic.substituteLength;
  } else if (data->tag == IO_REPARSE_TAG_MOUNT_POINT) {
    buffer = data->data.mount.path;
    offset = data->data.mount.substituteOffset;
    length = data->data.mount.substituteLength;
  } else {
    return std::nullopt;
  }
  ReparseTarget result;
  result.path = QString::fromWCharArray(
      buffer + offset / sizeof(WCHAR), length / sizeof(WCHAR));
  constexpr ULONG kSymlinkFlagRelative = 1;
  result.relative = data->tag == IO_REPARSE_TAG_SYMLINK &&
                    (data->data.symbolic.flags & kSymlinkFlagRelative) != 0;
  return result;
}

using NtCreateFileFunction = NTSTATUS(NTAPI*)(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
    ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);

NtCreateFileFunction ntCreateFileFunction() {
  static const auto function = reinterpret_cast<NtCreateFileFunction>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
  return function;
}

Handle openRelative(HANDLE parent, const QString& name, bool final,
                    DWORD* windowsError) {
  const auto ntCreateFile = ntCreateFileFunction();
  if (!ntCreateFile) {
    if (windowsError) *windowsError = ERROR_PROC_NOT_FOUND;
    return Handle{};
  }
  UNICODE_STRING unicodeName{};
  unicodeName.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
  unicodeName.MaximumLength = unicodeName.Length;
  unicodeName.Buffer = const_cast<PWSTR>(
      reinterpret_cast<const wchar_t*>(name.utf16()));
  OBJECT_ATTRIBUTES attributes{};
  InitializeObjectAttributes(&attributes, &unicodeName, OBJ_CASE_INSENSITIVE,
                             parent, nullptr);
  IO_STATUS_BLOCK statusBlock{};
  HANDLE raw = INVALID_HANDLE_VALUE;
  const ACCESS_MASK access = FILE_READ_ATTRIBUTES | SYNCHRONIZE |
                             (final ? FILE_READ_DATA : 0);
  constexpr ULONG kOpenReparsePoint = 0x00200000UL;
  constexpr ULONG kSynchronousIoNonAlert = 0x00000020UL;
  const NTSTATUS status = ntCreateFile(
      &raw, access, &attributes, &statusBlock, nullptr, FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ, FILE_OPEN,
      kOpenReparsePoint | kSynchronousIoNonAlert, nullptr, 0);
  if (status < 0) {
    if (windowsError) {
      using RtlNtStatusToDosErrorFunction = ULONG(WINAPI*)(NTSTATUS);
      const auto convert = reinterpret_cast<RtlNtStatusToDosErrorFunction>(
          GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                         "RtlNtStatusToDosError"));
      *windowsError = convert ? convert(status) : ERROR_OPEN_FAILED;
    }
    return Handle{};
  }
  if (windowsError) *windowsError = ERROR_SUCCESS;
  return Handle(raw);
}

bool splitDrivePath(const QString& path, QString* driveRoot,
                    QStringList* components) {
  const QString normalized = QDir::fromNativeSeparators(path);
  if (normalized.startsWith(QStringLiteral("//")) ||
      normalized.startsWith(QStringLiteral("//?/")) ||
      normalized.size() < 3 || normalized[1] != QLatin1Char(':') ||
      normalized[2] != QLatin1Char('/')) {
    return false;
  }
  *driveRoot = normalized.left(3);
  *components = normalized.sliced(3).split(QLatin1Char('/'),
                                           Qt::SkipEmptyParts);
  return !components->empty();
}

std::optional<bool> directoryIsCaseSensitive(HANDLE directory) {
  struct CaseSensitiveInfo {
    ULONG flags;
  } info{};
  constexpr auto kFileCaseSensitiveInfo =
      static_cast<FILE_INFO_BY_HANDLE_CLASS>(23);
  constexpr ULONG kCaseSensitiveDirectory = 1;
  if (!GetFileInformationByHandleEx(directory, kFileCaseSensitiveInfo, &info,
                                    sizeof(info))) {
    return std::nullopt;
  }
  return (info.flags & kCaseSensitiveDirectory) != 0;
}

bool consumeWindowsWork(std::uint64_t count,
                        std::uint64_t maximumWorkUnits,
                        std::uint64_t* workUnits) {
  if (!workUnits || count > maximumWorkUnits ||
      *workUnits > maximumWorkUnits - count) {
    return false;
  }
  *workUnits += count;
  return true;
}

class BackendHandle final {
 public:
  BackendHandle() = default;
  BackendHandle(WindowsWalkerBackend& backend,
                WindowsWalkerBackend::HandleToken token)
      : backend_(&backend), token_(token) {}
  ~BackendHandle() { reset(); }
  BackendHandle(const BackendHandle&) = delete;
  BackendHandle& operator=(const BackendHandle&) = delete;
  BackendHandle(BackendHandle&& other) noexcept
      : backend_(other.backend_), token_(other.token_) {
    other.backend_ = nullptr;
    other.token_ = 0;
  }
  BackendHandle& operator=(BackendHandle&& other) noexcept {
    if (this == &other) return *this;
    reset();
    backend_ = other.backend_;
    token_ = other.token_;
    other.backend_ = nullptr;
    other.token_ = 0;
    return *this;
  }
  [[nodiscard]] WindowsWalkerBackend::HandleToken get() const noexcept {
    return token_;
  }
  [[nodiscard]] WindowsWalkerBackend::HandleToken release() noexcept {
    const auto result = token_;
    backend_ = nullptr;
    token_ = 0;
    return result;
  }

 private:
  void reset() noexcept {
    if (backend_ && token_ != 0) backend_->close(token_);
    backend_ = nullptr;
    token_ = 0;
  }
  WindowsWalkerBackend* backend_{};
  WindowsWalkerBackend::HandleToken token_{};
};

WindowsWalkerResult walkWindowsPathCore(
    const QString& authored, std::uint32_t maximumAliasDepth,
    std::uint64_t maximumWorkUnits, bool requireDirectory,
    WindowsWalkerBackend& backend) {
  WindowsWalkerResult result;
  if (authored.contains(QChar::Null)) {
    result.status = LinkedSourceOpenStatus::ResourceFailure;
    result.error = QStringLiteral("linked path contains NUL");
    return result;
  }
  QString currentPath = QDir::cleanPath(QDir::fromNativeSeparators(authored));
  std::set<QString> visitedAliases;
  BackendHandle finalHandle;
  for (;;) {
    QString driveRoot;
    QStringList components;
    if (!splitDrivePath(currentPath, &driveRoot, &components)) {
      result.status = LinkedSourceOpenStatus::NetworkDisabled;
      result.error = QStringLiteral("network and device paths are disabled");
      return result;
    }
    if (!consumeWindowsWork(1, maximumWorkUnits, &result.workUnits)) {
      result.status = LinkedSourceOpenStatus::BudgetExceeded;
      result.error = QStringLiteral("filesystem work budget exhausted");
      return result;
    }
    bool remoteDrive = false;
    result.status = backend.queryRemoteDrive(driveRoot, &remoteDrive);
    if (result.status != LinkedSourceOpenStatus::Opened || remoteDrive) {
      result.status = remoteDrive ? LinkedSourceOpenStatus::NetworkDisabled
                                  : LinkedSourceOpenStatus::ResourceFailure;
      result.error = remoteDrive
                         ? QStringLiteral("network drives are disabled")
                         : QStringLiteral("cannot classify drive root");
      return result;
    }
    if (!consumeWindowsWork(1, maximumWorkUnits, &result.workUnits)) {
      result.status = LinkedSourceOpenStatus::BudgetExceeded;
      result.error = QStringLiteral("filesystem work budget exhausted");
      return result;
    }
    WindowsWalkerBackend::HandleToken parentToken{};
    result.status = backend.openDriveRoot(driveRoot, &parentToken);
    if (result.status != LinkedSourceOpenStatus::Opened || parentToken == 0) {
      result.error = result.status == LinkedSourceOpenStatus::NetworkDisabled
                         ? QStringLiteral("network drives are disabled")
                         : QStringLiteral("cannot open local drive root");
      return result;
    }
    BackendHandle parent(backend, parentToken);
    if (!consumeWindowsWork(1, maximumWorkUnits, &result.workUnits)) {
      result.status = LinkedSourceOpenStatus::BudgetExceeded;
      result.error = QStringLiteral("filesystem work budget exhausted");
      return result;
    }
    bool rootCaseSensitive = false;
    result.status = backend.queryCaseSensitive(parent.get(),
                                               &rootCaseSensitive);
    if (result.status != LinkedSourceOpenStatus::Opened || rootCaseSensitive) {
      result.status = LinkedSourceOpenStatus::ResourceFailure;
      result.error =
          QStringLiteral("case-sensitive directory traversal is unsupported");
      return result;
    }
    if (components.empty()) {
      if (!requireDirectory ||
          !consumeWindowsWork(1, maximumWorkUnits, &result.workUnits)) {
        result.status = requireDirectory
                            ? LinkedSourceOpenStatus::BudgetExceeded
                            : LinkedSourceOpenStatus::SourceNotRegular;
        result.error = requireDirectory
                           ? QStringLiteral("filesystem work budget exhausted")
                           : QStringLiteral("linked source is not a regular file");
        return result;
      }
      result.status = backend.queryInformation(parent.get(), &result.info);
      if (result.status != LinkedSourceOpenStatus::Opened) {
        result.status = LinkedSourceOpenStatus::SourceNotRegular;
        result.error = QStringLiteral("linked source is not a regular file");
        return result;
      }
      finalHandle = std::move(parent);
      break;
    }

    bool restart = false;
    for (qsizetype index = 0; index < components.size(); ++index) {
      if (!consumeWindowsWork(1, maximumWorkUnits, &result.workUnits)) {
        result.status = LinkedSourceOpenStatus::BudgetExceeded;
        result.error = QStringLiteral("filesystem work budget exhausted");
        return result;
      }
      const bool final = index + 1 == components.size();
      WindowsWalkerBackend::HandleToken componentToken{};
      result.status = backend.openRelative(parent.get(), components[index],
                                           final && !requireDirectory,
                                           &componentToken);
      if (result.status != LinkedSourceOpenStatus::Opened ||
          componentToken == 0) {
        result.error = QStringLiteral("cannot open linked path component");
        return result;
      }
      BackendHandle component(backend, componentToken);
      if (!consumeWindowsWork(1, maximumWorkUnits, &result.workUnits)) {
        result.status = LinkedSourceOpenStatus::BudgetExceeded;
        result.error = QStringLiteral("filesystem work budget exhausted");
        return result;
      }
      WindowsWalkerObjectInfo info;
      result.status = backend.queryInformation(component.get(), &info);
      if (result.status != LinkedSourceOpenStatus::Opened) {
        result.error = QStringLiteral("cannot inspect linked path");
        return result;
      }
      if (info.reparsePoint) {
        if (++result.aliasDepth > maximumAliasDepth) {
          result.status = LinkedSourceOpenStatus::BudgetExceeded;
          result.error = QStringLiteral("alias depth budget exceeded");
          return result;
        }
        if (!visitedAliases.insert(info.identity).second) {
          result.status = LinkedSourceOpenStatus::TraversalLoop;
          result.error = QStringLiteral("reparse traversal loop");
          return result;
        }
        if (!consumeWindowsWork(1, maximumWorkUnits, &result.workUnits)) {
          result.status = LinkedSourceOpenStatus::BudgetExceeded;
          result.error = QStringLiteral("filesystem work budget exhausted");
          return result;
        }
        WindowsWalkerReparseTarget target;
        result.status = backend.queryReparseTarget(component.get(), &target);
        if (result.status != LinkedSourceOpenStatus::Opened) {
          result.error = QStringLiteral("unsupported reparse target");
          return result;
        }
        QString destination = QDir::fromNativeSeparators(target.path);
        if (target.relative) {
          if (!consumeWindowsWork(2, maximumWorkUnits, &result.workUnits)) {
            result.status = LinkedSourceOpenStatus::BudgetExceeded;
            result.error = QStringLiteral("filesystem work budget exhausted");
            return result;
          }
          QString base;
          result.status = backend.canonicalPath(parent.get(), &base);
          if (result.status != LinkedSourceOpenStatus::Opened || base.isEmpty()) {
            result.error = QStringLiteral("cannot resolve reparse parent");
            return result;
          }
          destination = QDir::cleanPath(base + QLatin1Char('/') + destination);
        } else {
          if (destination.startsWith(QStringLiteral("/??/UNC/"),
                                     Qt::CaseInsensitive) ||
              destination.startsWith(QStringLiteral("//"))) {
            result.status = LinkedSourceOpenStatus::NetworkDisabled;
            result.error = QStringLiteral("network reparse targets are disabled");
            return result;
          }
          if (destination.startsWith(QStringLiteral("/??/"),
                                     Qt::CaseInsensitive))
            destination = destination.sliced(4);
          destination = QDir::cleanPath(destination);
        }
        if (destination.startsWith(QStringLiteral("//"))) {
          result.status = LinkedSourceOpenStatus::NetworkDisabled;
          result.error = QStringLiteral("network reparse targets are disabled");
          return result;
        }
        if (destination.isEmpty()) {
          result.error = QStringLiteral("cannot resolve reparse target");
          return result;
        }
        if (!final)
          destination += QLatin1Char('/') +
                         components.sliced(index + 1).join(QLatin1Char('/'));
        currentPath = QDir::cleanPath(destination);
        restart = true;
        break;
      }
      if (!final) {
        if (!info.directory) {
          result.status = LinkedSourceOpenStatus::Missing;
          result.error = QStringLiteral("linked path component is not a directory");
          return result;
        }
        if (!consumeWindowsWork(1, maximumWorkUnits, &result.workUnits)) {
          result.status = LinkedSourceOpenStatus::BudgetExceeded;
          result.error = QStringLiteral("filesystem work budget exhausted");
          return result;
        }
        bool caseSensitive = false;
        result.status = backend.queryCaseSensitive(component.get(),
                                                   &caseSensitive);
        if (result.status != LinkedSourceOpenStatus::Opened || caseSensitive) {
          result.status = LinkedSourceOpenStatus::ResourceFailure;
          result.error = QStringLiteral(
              "case-sensitive directory traversal is unsupported");
          return result;
        }
        parent = std::move(component);
      } else {
        if (info.directory != requireDirectory) {
          result.status = LinkedSourceOpenStatus::SourceNotRegular;
          result.error = requireDirectory
                             ? QStringLiteral("allowed root is not a directory")
                             : QStringLiteral("linked source is not a regular file");
          return result;
        }
        if (requireDirectory) {
          if (!consumeWindowsWork(1, maximumWorkUnits, &result.workUnits)) {
            result.status = LinkedSourceOpenStatus::BudgetExceeded;
            result.error = QStringLiteral("filesystem work budget exhausted");
            return result;
          }
          bool caseSensitive = false;
          result.status = backend.queryCaseSensitive(component.get(),
                                                     &caseSensitive);
          if (result.status != LinkedSourceOpenStatus::Opened || caseSensitive) {
            result.status = LinkedSourceOpenStatus::ResourceFailure;
            result.error = QStringLiteral(
                "case-sensitive directory traversal is unsupported");
            return result;
          }
        } else if (!info.diskFile) {
          result.status = LinkedSourceOpenStatus::SourceNotRegular;
          result.error = QStringLiteral("linked source is not a regular file");
          return result;
        }
        finalHandle = std::move(component);
        result.info = info;
      }
    }
    if (!restart) break;
  }
  if (!consumeWindowsWork(2, maximumWorkUnits, &result.workUnits)) {
    result.status = LinkedSourceOpenStatus::BudgetExceeded;
    result.error = QStringLiteral("filesystem work budget exhausted");
    return result;
  }
  result.status = backend.canonicalPath(finalHandle.get(),
                                        &result.canonicalPath);
  if (result.status != LinkedSourceOpenStatus::Opened ||
      result.canonicalPath.isEmpty()) {
    result.error = QStringLiteral("cannot obtain canonical linked path");
    return result;
  }
  result.heldHandle =
      WindowsWalkerHandleOwner(backend, finalHandle.release());
  result.status = LinkedSourceOpenStatus::Opened;
  return result;
}

class NativeWindowsWalkerBackend final : public WindowsWalkerBackend {
 public:
  LinkedSourceOpenStatus queryRemoteDrive(
      const QString& driveRoot, bool* remote) override {
    if (!remote) return LinkedSourceOpenStatus::ResourceFailure;
    const QString nativeDrive = QDir::toNativeSeparators(driveRoot);
    *remote = GetDriveTypeW(nativeDrive.toStdWString().c_str()) == DRIVE_REMOTE;
    return LinkedSourceOpenStatus::Opened;
  }

  LinkedSourceOpenStatus openDriveRoot(
      const QString& driveRoot, HandleToken* token) override {
    if (!token) return LinkedSourceOpenStatus::ResourceFailure;
    const QString nativeDrive = QDir::toNativeSeparators(driveRoot);
    const QString nativeRoot = QStringLiteral("\\\\?\\") + nativeDrive;
    Handle handle(CreateFileW(nativeRoot.toStdWString().c_str(),
                              FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT |
                                  FILE_FLAG_BACKUP_SEMANTICS,
                              nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE)
      return LinkedSourceOpenStatus::ResourceFailure;
    *token = reinterpret_cast<HandleToken>(handle.release());
    return LinkedSourceOpenStatus::Opened;
  }

  LinkedSourceOpenStatus openRelative(
      HandleToken parent, const QString& component, bool final,
      HandleToken* token) override {
    if (!token) return LinkedSourceOpenStatus::ResourceFailure;
    DWORD error = ERROR_SUCCESS;
    Handle handle = ::solidar::project::detail::openRelative(
        reinterpret_cast<HANDLE>(parent), component, final, &error);
    if (handle.get() == INVALID_HANDLE_VALUE) {
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
          error == ERROR_INVALID_NAME || error == ERROR_DIRECTORY)
        return LinkedSourceOpenStatus::Missing;
      if (error == ERROR_CANT_RESOLVE_FILENAME ||
          error == ERROR_TOO_MANY_LINKS)
        return LinkedSourceOpenStatus::TraversalLoop;
      return LinkedSourceOpenStatus::ResourceFailure;
    }
    *token = reinterpret_cast<HandleToken>(handle.release());
    return LinkedSourceOpenStatus::Opened;
  }

  LinkedSourceOpenStatus queryInformation(
      HandleToken token, WindowsWalkerObjectInfo* info) override {
    if (!info) return LinkedSourceOpenStatus::ResourceFailure;
    BY_HANDLE_FILE_INFORMATION native{};
    if (!GetFileInformationByHandle(reinterpret_cast<HANDLE>(token), &native))
      return LinkedSourceOpenStatus::ResourceFailure;
    info->identity = identityFor(native);
    info->version = versionFor(native);
    info->sizeBytes =
        (static_cast<std::uint64_t>(native.nFileSizeHigh) << 32U) |
        native.nFileSizeLow;
    info->directory =
        (native.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info->diskFile = !info->directory;
    info->reparsePoint =
        (native.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return LinkedSourceOpenStatus::Opened;
  }

  LinkedSourceOpenStatus queryCaseSensitive(
      HandleToken token, bool* caseSensitive) override {
    if (!caseSensitive) return LinkedSourceOpenStatus::ResourceFailure;
    const auto queried =
        directoryIsCaseSensitive(reinterpret_cast<HANDLE>(token));
    if (!queried) return LinkedSourceOpenStatus::ResourceFailure;
    *caseSensitive = *queried;
    return LinkedSourceOpenStatus::Opened;
  }

  LinkedSourceOpenStatus queryReparseTarget(
      HandleToken token, WindowsWalkerReparseTarget* target) override {
    if (!target) return LinkedSourceOpenStatus::ResourceFailure;
    const auto queried = reparseTarget(reinterpret_cast<HANDLE>(token));
    if (!queried) return LinkedSourceOpenStatus::ResourceFailure;
    target->path = queried->path;
    target->relative = queried->relative;
    return LinkedSourceOpenStatus::Opened;
  }

  LinkedSourceOpenStatus canonicalPath(HandleToken token,
                                       QString* path) override {
    if (!path) return LinkedSourceOpenStatus::ResourceFailure;
    *path = canonicalPathFor(reinterpret_cast<HANDLE>(token));
    return path->isEmpty() ? LinkedSourceOpenStatus::ResourceFailure
                           : LinkedSourceOpenStatus::Opened;
  }

  void close(HandleToken token) noexcept override {
    if (token) CloseHandle(reinterpret_cast<HANDLE>(token));
  }
};

#else

class FileDescriptor final {
 public:
  explicit FileDescriptor(int value) noexcept : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) ::close(value_);
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&& other) noexcept : value_(other.value_) {
    other.value_ = -1;
  }
  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this == &other) return *this;
    if (value_ >= 0) ::close(value_);
    value_ = other.value_;
    other.value_ = -1;
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return value_; }

 private:
  int value_{-1};
};

bool sameStat(const struct stat& lhs, const struct stat& rhs) {
  if (lhs.st_dev != rhs.st_dev || lhs.st_ino != rhs.st_ino ||
      lhs.st_size != rhs.st_size) {
    return false;
  }
#if defined(Q_OS_MACOS)
  return lhs.st_mtimespec.tv_sec == rhs.st_mtimespec.tv_sec &&
         lhs.st_mtimespec.tv_nsec == rhs.st_mtimespec.tv_nsec &&
         lhs.st_ctimespec.tv_sec == rhs.st_ctimespec.tv_sec &&
         lhs.st_ctimespec.tv_nsec == rhs.st_ctimespec.tv_nsec;
#else
  return lhs.st_mtim.tv_sec == rhs.st_mtim.tv_sec &&
         lhs.st_mtim.tv_nsec == rhs.st_mtim.tv_nsec &&
         lhs.st_ctim.tv_sec == rhs.st_ctim.tv_sec &&
         lhs.st_ctim.tv_nsec == rhs.st_ctim.tv_nsec;
#endif
}

class PosixReadHandle final : public LinkedSourceReadHandle {
 public:
  PosixReadHandle(FileDescriptor&& descriptor, struct stat before)
      : descriptor_(std::move(descriptor)), before_(before) {}

  LinkedSourceReadResult readBounded(std::uint64_t maximumBytes,
                                     std::uint64_t maximumWorkUnits) override {
    LinkedSourceReadResult result;
    if (maximumWorkUnits == 0) {
      result.status = LinkedSourceOpenStatus::BudgetExceeded;
      result.error = QStringLiteral("filesystem work budget exhausted");
      return result;
    }
    if (before_.st_size < 0 ||
        static_cast<std::uint64_t>(before_.st_size) > maximumBytes ||
        static_cast<std::uint64_t>(before_.st_size) >
            static_cast<std::uint64_t>((std::numeric_limits<int>::max)())) {
      result.status = LinkedSourceOpenStatus::BudgetExceeded;
      result.error = QStringLiteral("linked source exceeds remaining byte budget");
      return result;
    }
    result.bytes =
        QByteArray(static_cast<qsizetype>(before_.st_size), Qt::Uninitialized);
    qsizetype offset = 0;
    while (offset < result.bytes.size()) {
      if (result.workUnits >= maximumWorkUnits) {
        result.status = LinkedSourceOpenStatus::BudgetExceeded;
        result.error = QStringLiteral("filesystem work budget exhausted");
        return result;
      }
      ++result.workUnits;
      ssize_t read = -1;
      do {
        read = ::read(descriptor_.get(), result.bytes.data() + offset,
                      static_cast<std::size_t>(result.bytes.size() - offset));
      } while (read < 0 && errno == EINTR);
      if (read <= 0) {
        result.status = read == 0 ? LinkedSourceOpenStatus::SourceChanged
                                  : LinkedSourceOpenStatus::ResourceFailure;
        result.error = QStringLiteral("linked source changed while reading");
        return result;
      }
      offset += static_cast<qsizetype>(read);
    }
    if (result.workUnits >= maximumWorkUnits) {
      result.status = LinkedSourceOpenStatus::BudgetExceeded;
      result.error = QStringLiteral("filesystem work budget exhausted");
      return result;
    }
    ++result.workUnits;
    struct stat after {};
    int statResult;
    do {
      statResult = ::fstat(descriptor_.get(), &after);
    } while (statResult != 0 && errno == EINTR);
    if (statResult != 0 ||
        !sameStat(before_, after)) {
      result.status = LinkedSourceOpenStatus::SourceChanged;
      result.error = QStringLiteral("linked source changed while reading");
      return result;
    }
    result.status = LinkedSourceOpenStatus::Opened;
    return result;
  }

 private:
  FileDescriptor descriptor_;
  struct stat before_ {};
};

bool consumePosixWork(std::uint64_t maximumWorkUnits,
                      std::uint64_t* workUnits) {
  if (!workUnits || *workUnits >= maximumWorkUnits) return false;
  ++*workUnits;
  return true;
}

QString canonicalPathFor(int descriptor, std::uint64_t maximumWorkUnits,
                         std::uint64_t* workUnits, bool* budgetExhausted) {
  if (budgetExhausted) *budgetExhausted = false;
  if (!consumePosixWork(maximumWorkUnits, workUnits)) {
    if (budgetExhausted) *budgetExhausted = true;
    return {};
  }
#if defined(Q_OS_LINUX)
  const QByteArray fdPath = QByteArrayLiteral("/proc/self/fd/") +
                            QByteArray::number(descriptor);
  QByteArray target(PATH_MAX + 1, Qt::Uninitialized);
  ssize_t count;
  do {
    count = ::readlink(fdPath.constData(), target.data(),
                       static_cast<std::size_t>(PATH_MAX));
  } while (count < 0 && errno == EINTR);
  if (count > 0 && count <= PATH_MAX)
    return QFile::decodeName(target.constData(), count);
#elif defined(Q_OS_MACOS)
  char target[PATH_MAX]{};
  int status;
  do {
    status = ::fcntl(descriptor, F_GETPATH, target);
  } while (status != 0 && errno == EINTR);
  if (status == 0)
    return QFile::decodeName(target);
#endif
  return {};
}

struct PosixWalkResult {
  LinkedSourceOpenStatus status{LinkedSourceOpenStatus::ResourceFailure};
  FileDescriptor descriptor{-1};
  struct stat info {};
  QString canonicalPath;
  std::uint64_t workUnits{};
  std::uint32_t aliasDepth{};
  QString error;
};

PosixWalkResult walkPosixPath(const QString& authored,
                              std::uint32_t maximumAliasDepth,
                              std::uint64_t maximumWorkUnits,
                              bool requireDirectory) {
  PosixWalkResult result;
  QString currentPath = QDir::cleanPath(authored);
  std::set<QString> visitedAliases;
  for (;;) {
    if (!currentPath.startsWith(QLatin1Char('/'))) {
      result.error = QStringLiteral("POSIX linked path must be absolute");
      return result;
    }
    if (result.workUnits >= maximumWorkUnits) {
      result.status = LinkedSourceOpenStatus::BudgetExceeded;
      result.error = QStringLiteral("filesystem work budget exhausted");
      return result;
    }
    int rootFd;
    do {
      rootFd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (rootFd < 0 && errno == EINTR);
    ++result.workUnits;
    if (rootFd < 0) {
      result.error = QStringLiteral("cannot open filesystem root");
      return result;
    }
    FileDescriptor parent(rootFd);
    const QStringList components = currentPath.sliced(1).split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    if (components.empty()) {
      if (!requireDirectory) {
        result.status = LinkedSourceOpenStatus::SourceNotRegular;
        result.error = QStringLiteral("linked source is not a regular file");
        return result;
      }
      if (!consumePosixWork(maximumWorkUnits, &result.workUnits)) {
        result.status = LinkedSourceOpenStatus::BudgetExceeded;
        result.error = QStringLiteral("filesystem work budget exhausted");
        return result;
      }
      int rootStat;
      do {
        rootStat = ::fstat(parent.get(), &result.info);
      } while (rootStat != 0 && errno == EINTR);
      if (rootStat != 0) {
        result.error = QStringLiteral("cannot inspect allowed root");
        return result;
      }
      result.descriptor = std::move(parent);
      break;
    }
    bool restart = false;
    for (qsizetype index = 0; index < components.size(); ++index) {
      const QByteArray component = QFile::encodeName(components[index]);
      if (result.workUnits >= maximumWorkUnits) {
        result.status = LinkedSourceOpenStatus::BudgetExceeded;
        result.error = QStringLiteral("filesystem work budget exhausted");
        return result;
      }
      struct stat linkInfo {};
      int statResult;
      do {
        statResult = ::fstatat(parent.get(), component.constData(), &linkInfo,
                              AT_SYMLINK_NOFOLLOW);
      } while (statResult != 0 && errno == EINTR);
      ++result.workUnits;
      if (statResult != 0) {
        result.status = (errno == ENOENT || errno == ENOTDIR)
                            ? LinkedSourceOpenStatus::Missing
                            : (errno == ELOOP
                                   ? LinkedSourceOpenStatus::TraversalLoop
                                   : LinkedSourceOpenStatus::ResourceFailure);
        result.error = QStringLiteral("cannot inspect linked path component");
        return result;
      }
      const bool final = index + 1 == components.size();
      if (S_ISLNK(linkInfo.st_mode)) {
        if (++result.aliasDepth > maximumAliasDepth) {
          result.status = LinkedSourceOpenStatus::BudgetExceeded;
          result.error = QStringLiteral("alias depth budget exceeded");
          return result;
        }
        const QString identity = QStringLiteral("%1:%2")
                                     .arg(static_cast<qulonglong>(linkInfo.st_dev))
                                     .arg(static_cast<qulonglong>(linkInfo.st_ino));
        if (!visitedAliases.insert(identity).second) {
          result.status = LinkedSourceOpenStatus::TraversalLoop;
          result.error = QStringLiteral("symbolic-link traversal loop");
          return result;
        }
        if (result.workUnits >= maximumWorkUnits) {
          result.status = LinkedSourceOpenStatus::BudgetExceeded;
          result.error = QStringLiteral("filesystem work budget exhausted");
          return result;
        }
        QByteArray target(PATH_MAX + 1, Qt::Uninitialized);
        ssize_t count;
        do {
          count = ::readlinkat(parent.get(), component.constData(),
                               target.data(), PATH_MAX);
        } while (count < 0 && errno == EINTR);
        ++result.workUnits;
        if (!consumePosixWork(maximumWorkUnits, &result.workUnits)) {
          result.status = LinkedSourceOpenStatus::BudgetExceeded;
          result.error = QStringLiteral("filesystem work budget exhausted");
          return result;
        }
        struct stat afterLink {};
        int afterStatus;
        do {
          afterStatus = ::fstatat(parent.get(), component.constData(),
                                  &afterLink, AT_SYMLINK_NOFOLLOW);
        } while (afterStatus != 0 && errno == EINTR);
        if (count <= 0 || count >= PATH_MAX || afterStatus != 0 ||
            !sameStat(linkInfo, afterLink)) {
          result.status = LinkedSourceOpenStatus::SourceChanged;
          result.error = QStringLiteral("symbolic link changed during traversal");
          return result;
        }
        QString destination = QFile::decodeName(target.constData(), count);
        if (!destination.startsWith(QLatin1Char('/'))) {
          bool budgetExhausted = false;
          const QString base = canonicalPathFor(
              parent.get(), maximumWorkUnits, &result.workUnits,
              &budgetExhausted);
          if (budgetExhausted) {
            result.status = LinkedSourceOpenStatus::BudgetExceeded;
            result.error = QStringLiteral("filesystem work budget exhausted");
            return result;
          }
          if (base.isEmpty()) {
            result.error = QStringLiteral("cannot resolve symbolic-link parent");
            return result;
          }
          destination = base + QLatin1Char('/') + destination;
        }
        if (!final)
          destination += QLatin1Char('/') +
                         components.sliced(index + 1).join(QLatin1Char('/'));
        currentPath = QDir::cleanPath(destination);
        restart = true;
        break;
      }
      if (result.workUnits >= maximumWorkUnits) {
        result.status = LinkedSourceOpenStatus::BudgetExceeded;
        result.error = QStringLiteral("filesystem work budget exhausted");
        return result;
      }
      const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                        ((!final || requireDirectory) ? O_DIRECTORY : 0);
      int openedFd;
      do {
        openedFd = ::openat(parent.get(), component.constData(), flags);
      } while (openedFd < 0 && errno == EINTR);
      ++result.workUnits;
      if (openedFd < 0) {
        result.status = errno == ELOOP
                            ? LinkedSourceOpenStatus::TraversalLoop
                            : ((errno == ENOENT || errno == ENOTDIR)
                                   ? LinkedSourceOpenStatus::Missing
                                   : LinkedSourceOpenStatus::ResourceFailure);
        result.error = QStringLiteral("cannot open linked path component");
        return result;
      }
      FileDescriptor opened(openedFd);
      if (!consumePosixWork(maximumWorkUnits, &result.workUnits)) {
        result.status = LinkedSourceOpenStatus::BudgetExceeded;
        result.error = QStringLiteral("filesystem work budget exhausted");
        return result;
      }
      struct stat openedInfo {};
      int openedStat;
      do {
        openedStat = ::fstat(opened.get(), &openedInfo);
      } while (openedStat != 0 && errno == EINTR);
      if (openedStat != 0 ||
          openedInfo.st_dev != linkInfo.st_dev ||
          openedInfo.st_ino != linkInfo.st_ino) {
        result.status = LinkedSourceOpenStatus::SourceChanged;
        result.error = QStringLiteral("linked path changed during traversal");
        return result;
      }
      if (!final) {
        parent = std::move(opened);
      } else {
        if (requireDirectory ? !S_ISDIR(openedInfo.st_mode)
                             : !S_ISREG(openedInfo.st_mode)) {
          result.status = LinkedSourceOpenStatus::SourceNotRegular;
          result.error = QStringLiteral("linked path has wrong object type");
          return result;
        }
        result.descriptor = std::move(opened);
        result.info = openedInfo;
      }
    }
    if (!restart) break;
  }
  bool budgetExhausted = false;
  result.canonicalPath = canonicalPathFor(
      result.descriptor.get(), maximumWorkUnits, &result.workUnits,
      &budgetExhausted);
  if (budgetExhausted) {
    result.status = LinkedSourceOpenStatus::BudgetExceeded;
    result.error = QStringLiteral("filesystem work budget exhausted");
    return result;
  }
  if (result.canonicalPath.isEmpty()) {
    result.error = QStringLiteral("cannot obtain canonical linked path");
    return result;
  }
  result.status = LinkedSourceOpenStatus::Opened;
  return result;
}

#endif

}  // namespace

#if defined(Q_OS_WIN)
WindowsWalkerResult walkWindowsLinkedPath(
    const QString& absolutePath, std::uint32_t maximumAliasDepth,
    std::uint64_t maximumWorkUnits, bool requireDirectory,
    WindowsWalkerBackend& backend) {
  return walkWindowsPathCore(absolutePath, maximumAliasDepth,
                             maximumWorkUnits, requireDirectory, backend);
}
#endif

LinkedSourceOpenedFile openNativeLinkedPartSource(
    const QString& absolutePath, std::uint32_t maximumAliasDepth,
    std::uint64_t maximumWorkUnits) {
  if (maximumWorkUnits == 0)
    return failed(LinkedSourceOpenStatus::BudgetExceeded,
                  QStringLiteral("filesystem work budget exhausted"), 0);
  if (absolutePath.contains(QChar::Null))
    return failed(LinkedSourceOpenStatus::ResourceFailure,
                  QStringLiteral("linked path contains NUL"), 0);

#if defined(Q_OS_WIN)
  NativeWindowsWalkerBackend backend;
  auto walked = walkWindowsLinkedPath(
      absolutePath, maximumAliasDepth, maximumWorkUnits, false, backend);
  if (walked.status != LinkedSourceOpenStatus::Opened)
    return failed(walked.status, std::move(walked.error), walked.workUnits);
  if (walked.info.sizeBytes >
      static_cast<std::uint64_t>((std::numeric_limits<int>::max)())) {
    return failed(LinkedSourceOpenStatus::BudgetExceeded,
                  QStringLiteral("linked source exceeds byte budget"),
                  walked.workUnits);
  }
  LinkedSourceOpenedFile staged;
  staged.status = LinkedSourceOpenStatus::Opened;
  staged.canonicalPath = std::move(walked.canonicalPath);
  staged.fileIdentity = walked.info.identity;
  staged.objectVersion = walked.info.version;
  staged.sizeBytes = walked.info.sizeBytes;
  staged.workUnits = walked.workUnits;
  staged.aliasDepth = walked.aliasDepth;
  Handle finalHandle(
      reinterpret_cast<HANDLE>(walked.heldHandle.release()));
  staged.handle = std::make_unique<WindowsReadHandle>(
      std::move(finalHandle), std::move(walked.info));
  return staged;

#else
  auto walked = walkPosixPath(absolutePath, maximumAliasDepth,
                              maximumWorkUnits, false);
  if (walked.status != LinkedSourceOpenStatus::Opened)
    return failed(walked.status, std::move(walked.error), walked.workUnits);
  const auto& before = walked.info;
  if (before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) >
          static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    return failed(LinkedSourceOpenStatus::BudgetExceeded,
                  QStringLiteral("linked source exceeds byte budget"));
  }

  LinkedSourceOpenedFile result;
  result.status = LinkedSourceOpenStatus::Opened;
  result.canonicalPath = std::move(walked.canonicalPath);
  result.fileIdentity = QStringLiteral("%1:%2")
                            .arg(static_cast<qulonglong>(before.st_dev))
                            .arg(static_cast<qulonglong>(before.st_ino));
#if defined(Q_OS_MACOS)
  result.objectVersion = QStringLiteral("%1:%2:%3:%4:%5")
                             .arg(static_cast<qlonglong>(before.st_size))
                             .arg(static_cast<qlonglong>(before.st_mtimespec.tv_sec))
                             .arg(static_cast<qlonglong>(before.st_mtimespec.tv_nsec))
                             .arg(static_cast<qlonglong>(before.st_ctimespec.tv_sec))
                             .arg(static_cast<qlonglong>(before.st_ctimespec.tv_nsec));
#else
  result.objectVersion = QStringLiteral("%1:%2:%3:%4:%5")
                             .arg(static_cast<qlonglong>(before.st_size))
                             .arg(static_cast<qlonglong>(before.st_mtim.tv_sec))
                             .arg(static_cast<qlonglong>(before.st_mtim.tv_nsec))
                             .arg(static_cast<qlonglong>(before.st_ctim.tv_sec))
                             .arg(static_cast<qlonglong>(before.st_ctim.tv_nsec));
#endif
  result.sizeBytes = static_cast<std::uint64_t>(before.st_size);
  result.workUnits = walked.workUnits;
  result.aliasDepth = walked.aliasDepth;
  result.handle =
      std::make_unique<PosixReadHandle>(std::move(walked.descriptor), before);
  return result;
#endif
}

LinkedSourceCanonicalRoot canonicalizeNativeLinkedRoot(
    const QString& absolutePath, std::uint32_t maximumAliasDepth,
    std::uint64_t maximumWorkUnits) {
  LinkedSourceCanonicalRoot result;
#if defined(Q_OS_WIN)
  if (absolutePath.contains(QChar::Null)) {
    result.status = LinkedSourceOpenStatus::ResourceFailure;
    result.error = QStringLiteral("allowed root contains NUL");
    return result;
  }
  NativeWindowsWalkerBackend backend;
  auto walked = walkWindowsLinkedPath(absolutePath, maximumAliasDepth,
                                      maximumWorkUnits, true, backend);
  result.status = walked.status;
  result.canonicalPath = std::move(walked.canonicalPath);
  result.workUnits = walked.workUnits;
  result.aliasDepth = walked.aliasDepth;
  result.error = std::move(walked.error);
#else
  if (absolutePath.contains(QChar::Null)) {
    result.status = LinkedSourceOpenStatus::ResourceFailure;
    result.error = QStringLiteral("allowed root contains NUL");
    return result;
  }
  auto walked = walkPosixPath(absolutePath, maximumAliasDepth,
                              maximumWorkUnits, true);
  result.status = walked.status;
  result.canonicalPath = std::move(walked.canonicalPath);
  result.workUnits = walked.workUnits;
  result.aliasDepth = walked.aliasDepth;
  result.error = std::move(walked.error);
#endif
  return result;
}

}  // namespace solidar::project::detail
