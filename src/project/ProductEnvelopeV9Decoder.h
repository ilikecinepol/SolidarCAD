#pragma once

#include <QByteArrayView>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

#include "model/AssemblyMate.h"
#include "project/ProductEnvelopeV8Decoder.h"

namespace solidar::project {

inline constexpr std::size_t kProductEnvelopeV9MaximumMatesPerAssembly = 128;
inline constexpr std::size_t kProductEnvelopeV9MaximumMates = 4096;
inline constexpr std::size_t kProductEnvelopeV9MaximumEndpoints = 8192;
inline constexpr std::size_t kProductEnvelopeV9MaximumPathEntries = 262144;
inline constexpr std::size_t kProductEnvelopeV9MaximumProvenanceRecords = 65536;
inline constexpr std::size_t kProductEnvelopeV9MaximumOriginsPerReference = 256;
inline constexpr std::size_t kProductEnvelopeV9MaximumSolveComponents = 4096;
inline constexpr std::uint64_t kProductEnvelopeV9MaximumSolveWork =
    1'000'000'000ULL;

enum class ProductEnvelopeV9ScanStatus { Candidate, Unsupported, Invalid,
                                         NotApplicable };

struct ProductEnvelopeV9AssemblyCandidate final {
  DefinitionId definitionId;
  MateSolverOptions options;
  std::vector<AssemblyMate> mates;

  friend bool operator==(const ProductEnvelopeV9AssemblyCandidate&,
                         const ProductEnvelopeV9AssemblyCandidate&) = default;
};

struct ProductEnvelopeV9Candidate final {
  QJsonObject authoredRoot;
  ProductEnvelopeV8Candidate base;
  QStringList capabilities;
  std::vector<ProductEnvelopeV9AssemblyCandidate> assemblies;
  std::size_t mateCount{};
  std::size_t endpointCount{};
  std::size_t pathEntryCount{};
  std::size_t provenanceRecordCount{};
};

struct ProductEnvelopeV9ScanResult final {
  ProductEnvelopeV9ScanStatus status{ProductEnvelopeV9ScanStatus::Invalid};
  std::optional<ProductEnvelopeV9Candidate> candidate;
  QString error;
  [[nodiscard]] bool hasCandidate() const noexcept {
    return status == ProductEnvelopeV9ScanStatus::Candidate && candidate;
  }
};

// Strict disconnected V9 scanner.  It reuses the complete V8 part/occurrence
// boundary after removing only the two V9 assembly-owned fields.
[[nodiscard]] ProductEnvelopeV9ScanResult scanProductEnvelopeV9(
    QByteArrayView bytes);

}  // namespace solidar::project
