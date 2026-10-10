#pragma once

#include <QObject>

#include <cstdint>
#include <functional>

class QTimer;

namespace solidar {

// GUI-thread-only last-write-wins scheduler for expensive synchronous OCCT
// previews. It deliberately does not move OCCT/OpenGL work to a worker: the
// generation token prevents delayed work from surviving tool/document state.
class PreviewUpdateCoordinator final : public QObject {
 public:
  struct Token {
    std::uint64_t generation{};
    std::uint64_t sequence{};
    bool operator==(const Token&) const noexcept = default;
  };
  struct Counters {
    std::uint64_t requests{};
    std::uint64_t executions{};
    std::uint64_t invalidations{};
    std::uint64_t replacements{};
    std::uint64_t flushes{};
    std::uint64_t cancellations{};
    std::uint64_t staleRejections{};
    std::uint64_t publications{};
    std::uint64_t cadenceStarts{};
  };
  using Work = std::function<void(Token)>;

  explicit PreviewUpdateCoordinator(QObject* owner, int debounceMs = 65);
  [[nodiscard]] Token request(Work work);
  void flush();
  void invalidate() noexcept;
  [[nodiscard]] bool isCurrent(Token token) const noexcept;
  // Consumes the single publication right for this token. A callback that was
  // superseded re-entrantly can finish its synchronous computation but cannot
  // publish stale geometry.
  [[nodiscard]] bool claimPublication(Token token) noexcept;
  [[nodiscard]] bool hasPending() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept;
  [[nodiscard]] const Counters& counters() const noexcept;
  void setCadenceIntervalForTests(int milliseconds);
  void dispatchCadenceForTests();

 private:
  void executePending();

  QTimer* timer_{};
  Work pending_;
  Token pendingToken_;
  std::uint64_t generation_{1};
  std::uint64_t nextSequence_{1};
  std::uint64_t latestRequestedSequence_{};
  std::uint64_t publishedSequence_{};
  Counters counters_;
};

}  // namespace solidar
