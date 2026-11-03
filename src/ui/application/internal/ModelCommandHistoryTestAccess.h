#pragma once

#include <cstddef>
#include <cstdint>

#include "ui/application/ModelCommandHistory.h"

namespace solidar::detail {

enum class ModelCommandHistoryFailurePoint : std::uint8_t {
  None,
  PrepareAccounting,
  PrepareReserve,
  UndoStatePreparation,
  RedoStatePreparation,
};

enum class ModelCommandHistoryFailureKind : std::uint8_t {
  BadAlloc,
  Runtime,
  Unknown,
};

class ModelCommandHistoryTestAccess final {
 public:
  static void inject(ModelCommandHistory& history,
                     ModelCommandHistoryFailurePoint point,
                     ModelCommandHistoryFailureKind kind) noexcept {
    history.testFailurePoint_ = static_cast<std::uint8_t>(point);
    history.testFailureKind_ = static_cast<std::uint8_t>(kind);
  }

  static void clearInjection(ModelCommandHistory& history) noexcept {
    history.testFailurePoint_ = 0;
    history.testFailureKind_ = 0;
  }

  static void setNextRevision(ModelCommandHistory& history,
                              std::uint64_t value) noexcept {
    history.setNextRevisionForTesting(value);
  }

  static void setProjectionCompareLimit(ModelCommandHistory& history,
                                        std::size_t value) noexcept {
    history.setProjectionCompareLimitForTesting(value);
  }

  [[nodiscard]] static std::size_t projectionCompareCount(
      const ModelCommandHistory& history) noexcept {
    return history.projectionCompareCountForTesting();
  }
};

}  // namespace solidar::detail
