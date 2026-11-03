#pragma once

#include <cstdint>
#include <array>
#include <cstddef>
#include <utility>

#include "ui/application/AssemblyCommandHistory.h"

namespace solidar::detail {

enum class AssemblyCommandHistoryFailurePoint : std::uint8_t {
  None,
  Allocation,
  Reserve,
  SemanticStatePreparation,
  PayloadPreparation,
  BeforeOperation,
  AfterFirstSyntheticOperation,
  Finalization,
};

enum class AssemblyCommandHistoryFailureKind : std::uint8_t {
  BadAlloc,
  Runtime,
  Unknown,
};

class AssemblyCommandHistoryTestAccess final {
 public:
  static void inject(AssemblyCommandHistory& history,
                     AssemblyCommandHistoryFailurePoint point,
                     AssemblyCommandHistoryFailureKind kind) noexcept {
    history.testFailurePoint_ = static_cast<std::uint8_t>(point);
    history.testFailureKind_ = static_cast<std::uint8_t>(kind);
  }

  static void clearInjection(AssemblyCommandHistory& history) noexcept {
    history.testFailurePoint_ = 0;
    history.testFailureKind_ = 0;
    history.definitionEditFailurePointForTesting_ =
        DefinitionEditFailurePoint::None;
  }

  static void injectDefinitionEdit(
      AssemblyCommandHistory& history,
      DefinitionEditFailurePoint point) noexcept {
    history.definitionEditFailurePointForTesting_ = point;
  }

  static OccurrenceDelta syntheticDelta(RootOccurrenceAddress address) {
    return OccurrenceDelta(std::move(address), true);
  }

  static AssemblyCommandHistoryStatus executeSyntheticTwoStep(
      AssemblyCommandHistory& history, OccurrenceDeltaCommand command,
      int& first, int& second, std::array<int, 4>& trace,
      std::size_t& traceSize, bool contradictory = false) noexcept {
    return history.executeSyntheticTwoStepForTesting(
        std::move(command), first, second, trace, traceSize, contradictory);
  }

  static void setNextRevision(AssemblyCommandHistory& history,
                              std::uint64_t value) noexcept {
    history.nextRevisionValue_ = value;
  }

  static std::optional<std::size_t> dynamicBytes(
      const AssemblyCommand& command) noexcept {
    return AssemblyCommandHistory::dynamicBytes(command);
  }

  static std::optional<std::size_t> mateRegistryDynamicBytes(
      const MateRegistry& registry) noexcept {
    return AssemblyCommandHistory::mateRegistryDynamicBytesForTesting(
        registry);
  }
};

}  // namespace solidar::detail
