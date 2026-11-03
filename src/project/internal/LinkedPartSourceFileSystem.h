#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

namespace solidar::project {
struct LinkedPartSourceDescriptor;
struct LinkedPartSourcePolicy;
struct LinkedPartSourceResolveResult;
}

namespace solidar::project::detail {

enum class LinkedSourceOpenStatus {
  Opened,
  Missing,
  NetworkDisabled,
  SourceNotRegular,
  TraversalLoop,
  BudgetExceeded,
  SourceChanged,
  ResourceFailure,
};

struct LinkedSourceReadResult {
  LinkedSourceOpenStatus status{LinkedSourceOpenStatus::ResourceFailure};
  QByteArray bytes;
  std::uint64_t workUnits{};
  QString error;
};

// Owns the exact object inspected by openExact(). Implementations must read
// through this object; reopening the authored pathname is forbidden.
class LinkedSourceReadHandle {
 public:
  virtual ~LinkedSourceReadHandle() = default;
  [[nodiscard]] virtual LinkedSourceReadResult readBounded(
      std::uint64_t maximumBytes, std::uint64_t maximumWorkUnits) = 0;
};

struct LinkedSourceOpenedFile {
  LinkedSourceOpenStatus status{LinkedSourceOpenStatus::ResourceFailure};
  QString canonicalPath;
  QString fileIdentity;
  QString objectVersion;
  std::uint64_t sizeBytes{};
  std::uint64_t workUnits{};
  std::uint32_t aliasDepth{};
  QString error;
  std::unique_ptr<LinkedSourceReadHandle> handle;

  LinkedSourceOpenedFile() = default;
  LinkedSourceOpenedFile(LinkedSourceOpenedFile&&) noexcept = default;
  LinkedSourceOpenedFile& operator=(LinkedSourceOpenedFile&&) noexcept =
      default;
  LinkedSourceOpenedFile(const LinkedSourceOpenedFile&) = delete;
  LinkedSourceOpenedFile& operator=(const LinkedSourceOpenedFile&) = delete;
};

struct LinkedSourceCanonicalRoot {
  LinkedSourceOpenStatus status{LinkedSourceOpenStatus::ResourceFailure};
  QString canonicalPath;
  std::uint64_t workUnits{};
  std::uint32_t aliasDepth{};
  QString error;
};

#if defined(Q_OS_WIN)
struct WindowsWalkerObjectInfo {
  QString identity;
  QString version;
  std::uint64_t sizeBytes{};
  bool directory{};
  bool diskFile{};
  bool reparsePoint{};
};

struct WindowsWalkerReparseTarget {
  QString path;
  bool relative{};
};

class WindowsWalkerBackend {
 public:
  using HandleToken = std::uintptr_t;
  virtual ~WindowsWalkerBackend() = default;
  virtual LinkedSourceOpenStatus queryRemoteDrive(
      const QString& driveRoot, bool* remote) = 0;
  virtual LinkedSourceOpenStatus openDriveRoot(
      const QString& driveRoot, HandleToken* handle) = 0;
  virtual LinkedSourceOpenStatus openRelative(
      HandleToken parent, const QString& component, bool final,
      HandleToken* handle) = 0;
  virtual LinkedSourceOpenStatus queryInformation(
      HandleToken handle, WindowsWalkerObjectInfo* info) = 0;
  virtual LinkedSourceOpenStatus queryCaseSensitive(
      HandleToken directory, bool* caseSensitive) = 0;
  virtual LinkedSourceOpenStatus queryReparseTarget(
      HandleToken handle, WindowsWalkerReparseTarget* target) = 0;
  // Production performs exactly two GetFinalPathNameByHandleW calls. The
  // walker reserves both units before entering this operation.
  virtual LinkedSourceOpenStatus canonicalPath(
      HandleToken handle, QString* path) = 0;
  virtual void close(HandleToken handle) noexcept = 0;
};

class WindowsWalkerHandleOwner final {
 public:
  WindowsWalkerHandleOwner() = default;
  WindowsWalkerHandleOwner(WindowsWalkerBackend& backend,
                           WindowsWalkerBackend::HandleToken token) noexcept
      : backend_(&backend), token_(token) {}
  ~WindowsWalkerHandleOwner() { reset(); }
  WindowsWalkerHandleOwner(const WindowsWalkerHandleOwner&) = delete;
  WindowsWalkerHandleOwner& operator=(const WindowsWalkerHandleOwner&) = delete;
  WindowsWalkerHandleOwner(WindowsWalkerHandleOwner&& other) noexcept
      : backend_(other.backend_), token_(other.token_) {
    other.backend_ = nullptr;
    other.token_ = 0;
  }
  WindowsWalkerHandleOwner& operator=(
      WindowsWalkerHandleOwner&& other) noexcept {
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
  [[nodiscard]] explicit operator bool() const noexcept { return token_ != 0; }
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

struct WindowsWalkerResult {
  LinkedSourceOpenStatus status{LinkedSourceOpenStatus::ResourceFailure};
  WindowsWalkerHandleOwner heldHandle;
  WindowsWalkerObjectInfo info;
  QString canonicalPath;
  std::uint64_t workUnits{};
  std::uint32_t aliasDepth{};
  QString error;
};

[[nodiscard]] WindowsWalkerResult walkWindowsLinkedPath(
    const QString& absolutePath, std::uint32_t maximumAliasDepth,
    std::uint64_t maximumWorkUnits, bool requireDirectory,
    WindowsWalkerBackend& backend);
#endif

// Deliberately narrow capability surface.  A linked-source resolver can open
// one exact locator, inspect it and read that same held object. It cannot enumerate,
// write, create a sidecar, or access a network API through this seam.
class LinkedPartSourceFileSystem {
 public:
  virtual ~LinkedPartSourceFileSystem() = default;

  [[nodiscard]] virtual LinkedSourceCanonicalRoot canonicalizeRoot(
      const QString& absolutePath, std::uint32_t maximumAliasDepth,
      std::uint64_t maximumWorkUnits) = 0;

  [[nodiscard]] virtual LinkedSourceOpenedFile openExact(
      const QString& absolutePath, std::uint32_t maximumAliasDepth,
      std::uint64_t maximumWorkUnits) = 0;
};

[[nodiscard]] LinkedPartSourceResolveResult
resolveLinkedPartSourcesNativeWithFileSystem(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    LinkedPartSourcePolicy policy, LinkedPartSourceFileSystem& fileSystem);

[[nodiscard]] LinkedSourceOpenedFile openNativeLinkedPartSource(
    const QString& absolutePath, std::uint32_t maximumAliasDepth,
    std::uint64_t maximumWorkUnits);

[[nodiscard]] LinkedSourceCanonicalRoot canonicalizeNativeLinkedRoot(
    const QString& absolutePath, std::uint32_t maximumAliasDepth,
    std::uint64_t maximumWorkUnits);

}  // namespace solidar::project::detail
