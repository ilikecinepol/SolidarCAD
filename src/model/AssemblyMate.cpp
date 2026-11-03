#include "model/AssemblyMate.h"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace solidar {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

double canonicalScalar(double value) noexcept {
  return value == 0.0 ? 0.0 : value;
}

void hashBytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ULL;
  }
}

template <typename Value>
void hashValue(std::uint64_t& hash, const Value& value) noexcept {
  hashBytes(hash, &value, sizeof(value));
}

void hashString(std::uint64_t& hash, const std::string& value) noexcept {
  const auto size = static_cast<std::uint64_t>(value.size());
  hashValue(hash, size);
  hashBytes(hash, value.data(), value.size());
}

void hashReference(std::uint64_t& hash, const FaceReference& reference) noexcept {
  const auto* v2 = reference.v2();
  const std::uint8_t version = v2 ? 2 : 1;
  hashValue(hash, version);
  if (!v2) return;
  hashValue(hash, v2->bodyId);
  hashValue(hash, v2->provenance.featureId);
  hashValue(hash, v2->provenance.kind);
  hashValue(hash, v2->provenance.operation);
  hashValue(hash, v2->provenance.role);
  hashString(hash, v2->provenance.localName);
  const auto originCount =
      static_cast<std::uint64_t>(v2->provenance.origins.size());
  hashValue(hash, originCount);
  for (const auto& origin : v2->provenance.origins) {
    hashValue(hash, origin.featureId);
    hashValue(hash, origin.kind);
    hashValue(hash, origin.operation);
    hashValue(hash, origin.role);
    hashString(hash, origin.localName);
    const std::uint8_t hasOccurrence = origin.occurrence ? 1 : 0;
    hashValue(hash, hasOccurrence);
    if (origin.occurrence) {
      hashValue(hash, origin.occurrence->featureId);
      hashValue(hash, origin.occurrence->slot);
    }
  }
  const std::uint8_t hasOccurrence = v2->provenance.occurrence ? 1 : 0;
  hashValue(hash, hasOccurrence);
  if (v2->provenance.occurrence) {
    hashValue(hash, v2->provenance.occurrence->featureId);
    hashValue(hash, v2->provenance.occurrence->slot);
  }
}

void hashReference(std::uint64_t& hash, const EdgeReference& reference) noexcept {
  const auto* v2 = reference.v2();
  const std::uint8_t version = v2 ? 2 : 1;
  hashValue(hash, version);
  if (!v2) return;
  hashValue(hash, v2->bodyId);
  hashValue(hash, v2->provenance.featureId);
  hashValue(hash, v2->provenance.kind);
  hashValue(hash, v2->provenance.operation);
  hashValue(hash, v2->provenance.role);
  hashString(hash, v2->provenance.localName);
  const auto originCount =
      static_cast<std::uint64_t>(v2->provenance.origins.size());
  hashValue(hash, originCount);
  for (const auto& origin : v2->provenance.origins) {
    hashValue(hash, origin.featureId);
    hashValue(hash, origin.kind);
    hashValue(hash, origin.operation);
    hashValue(hash, origin.role);
    hashString(hash, origin.localName);
    const std::uint8_t hasOccurrence = origin.occurrence ? 1 : 0;
    hashValue(hash, hasOccurrence);
    if (origin.occurrence) {
      hashValue(hash, origin.occurrence->featureId);
      hashValue(hash, origin.occurrence->slot);
    }
  }
  const std::uint8_t hasOccurrence = v2->provenance.occurrence ? 1 : 0;
  hashValue(hash, hasOccurrence);
  if (v2->provenance.occurrence) {
    hashValue(hash, v2->provenance.occurrence->featureId);
    hashValue(hash, v2->provenance.occurrence->slot);
  }
}

bool endpointsAreValid(const MateEndpoint& first,
                       const MateEndpoint& second) noexcept {
  return first.occurrencePath != second.occurrencePath && first.face.isV2() &&
         second.face.isV2() && first.face.v2()->hasValidOwner() &&
         second.face.v2()->hasValidOwner();
}

bool axisReferenceIsValid(const MateAxisReference& reference) noexcept {
  if (const auto* datum = std::get_if<LocalDatumAxis>(&reference))
    return isValidLocalDatumAxis(*datum);
  const auto& face = std::get<CylindricalFaceAxis>(reference).face;
  return face.isV2() && face.v2()->hasValidOwner();
}

bool pointReferenceIsValid(const MatePointReference& reference) noexcept {
  if (const auto* datum = std::get_if<LocalDatumPoint>(&reference))
    return isValidLocalDatumPoint(*datum);
  const auto& edge = std::get<CircularEdgeCenter>(reference).edge;
  return edge.isV2() && edge.v2()->hasValidOwner() &&
         edge.v2()->provenance.kind == TopologyKind::Edge;
}

}  // namespace

bool MateSolverOptions::isValid() const noexcept {
  return std::isfinite(linearToleranceMm) && linearToleranceMm > 0.0 &&
         linearToleranceMm <= 1.0 && std::isfinite(angularToleranceRad) &&
         angularToleranceRad >= kMinimumMateAngularToleranceRad &&
         angularToleranceRad <= kPi &&
         std::isfinite(rankTolerance) && rankTolerance > 0.0 &&
         rankTolerance <= 1.0 && std::isfinite(maximumAbsoluteOffsetMm) &&
         maximumAbsoluteOffsetMm >= 0.0 &&
         maximumAbsoluteOffsetMm <= 1.0e9 &&
         std::isfinite(maximumTranslationStepMm) &&
         maximumTranslationStepMm > 0.0 &&
         maximumTranslationStepMm <= 1.0e6 &&
         std::isfinite(maximumAngularStepRad) &&
         maximumAngularStepRad > 0.0 && maximumAngularStepRad <= kPi &&
         maximumIterations >= 1 && maximumIterations <= 64 &&
         maximumNodes >= 1 && maximumNodes <= 256 && maximumMates >= 1 &&
         maximumMates <= kMaximumAssemblyMates && maximumWorkUnits >= 1 &&
         maximumWorkUnits <= 1'000'000'000ULL;
}

PlanarCoincidentMate::PlanarCoincidentMate(
    MateId id, MateEndpoint first, MateEndpoint second,
    PlanarMateOrientation orientation, double offsetMm)
    : id_(id),
      first_(std::move(first)),
      second_(std::move(second)),
      orientation_(orientation),
      offsetMm_(canonicalScalar(offsetMm)) {}

bool PlanarCoincidentMate::isValid(
    double maximumAbsoluteOffsetMm) const noexcept {
  return id_.isValid() && endpointsAreValid(first_, second_) &&
         isValidPlanarMateOrientation(orientation_) &&
         std::isfinite(offsetMm_) &&
         std::isfinite(maximumAbsoluteOffsetMm) &&
         maximumAbsoluteOffsetMm >= 0.0 &&
         std::abs(offsetMm_) <= maximumAbsoluteOffsetMm;
}

PlanarParallelMate::PlanarParallelMate(
    MateId id, MateEndpoint first, MateEndpoint second,
    PlanarMateOrientation orientation)
    : id_(id),
      first_(std::move(first)),
      second_(std::move(second)),
      orientation_(orientation) {}

bool PlanarParallelMate::isValid() const noexcept {
  return id_.isValid() && endpointsAreValid(first_, second_) &&
         isValidPlanarMateOrientation(orientation_);
}

PlanarPerpendicularMate::PlanarPerpendicularMate(
    MateId id, MateEndpoint first, MateEndpoint second)
    : id_(id), first_(std::move(first)), second_(std::move(second)) {}

bool PlanarPerpendicularMate::isValid() const noexcept {
  return id_.isValid() && endpointsAreValid(first_, second_);
}

AxisConcentricMate::AxisConcentricMate(MateId id, MateAxisEndpoint first,
                                       MateAxisEndpoint second)
    : id_(id), first_(std::move(first)), second_(std::move(second)) {}

bool AxisConcentricMate::isValid() const noexcept {
  return id_.isValid() &&
         first_.occurrencePath != second_.occurrencePath &&
         !first_.occurrencePath.entries().empty() &&
         !second_.occurrencePath.entries().empty() &&
         axisReferenceIsValid(first_.reference) &&
         axisReferenceIsValid(second_.reference);
}

PointDistanceMate::PointDistanceMate(MateId id, MatePointEndpoint first,
                                     MatePointEndpoint second,
                                     double distanceMm)
    : id_(id),
      first_(std::move(first)),
      second_(std::move(second)),
      distanceMm_(canonicalScalar(distanceMm)) {}

bool PointDistanceMate::isValid(
    double maximumAbsoluteOffsetMm) const noexcept {
  return id_.isValid() &&
         first_.occurrencePath != second_.occurrencePath &&
         !first_.occurrencePath.entries().empty() &&
         !second_.occurrencePath.entries().empty() &&
         pointReferenceIsValid(first_.reference) &&
         pointReferenceIsValid(second_.reference) &&
         std::isfinite(distanceMm_) && distanceMm_ > 0.0 &&
         std::isfinite(maximumAbsoluteOffsetMm) &&
         maximumAbsoluteOffsetMm >= distanceMm_;
}

PlanarAngleMate::PlanarAngleMate(
    MateId id, MateEndpoint first, MateEndpoint second,
    PlanarMateOrientation orientation, double angleRad)
    : id_(id),
      first_(std::move(first)),
      second_(std::move(second)),
      orientation_(orientation),
      angleRad_(canonicalScalar(angleRad)) {}

bool PlanarAngleMate::isStructurallyValid() const noexcept {
  return id_.isValid() && endpointsAreValid(first_, second_) &&
         isValidPlanarMateOrientation(orientation_) &&
         std::isfinite(angleRad_) && angleRad_ > 0.0 &&
         angleRad_ < kPi * 0.5;
}

bool PlanarAngleMate::isValid(double angularToleranceRad) const noexcept {
  return isStructurallyValid() && std::isfinite(angularToleranceRad) &&
         angularToleranceRad >= kMinimumMateAngularToleranceRad &&
         angleRad_ > angularToleranceRad &&
         kPi * 0.5 - angleRad_ > angularToleranceRad;
}

MateId assemblyMateId(const AssemblyMate& mate) noexcept {
  return std::visit([](const auto& value) { return value.id(); }, mate);
}

const MateEndpoint* assemblyPlanarMateFirst(const AssemblyMate& mate) noexcept {
  if (const auto* value = std::get_if<PlanarCoincidentMate>(&mate))
    return &value->first();
  if (const auto* value = std::get_if<PlanarParallelMate>(&mate))
    return &value->first();
  if (const auto* value = std::get_if<PlanarPerpendicularMate>(&mate))
    return &value->first();
  if (const auto* value = std::get_if<PlanarAngleMate>(&mate))
    return &value->first();
  return nullptr;
}

const MateEndpoint* assemblyPlanarMateSecond(const AssemblyMate& mate) noexcept {
  if (const auto* value = std::get_if<PlanarCoincidentMate>(&mate))
    return &value->second();
  if (const auto* value = std::get_if<PlanarParallelMate>(&mate))
    return &value->second();
  if (const auto* value = std::get_if<PlanarPerpendicularMate>(&mate))
    return &value->second();
  if (const auto* value = std::get_if<PlanarAngleMate>(&mate))
    return &value->second();
  return nullptr;
}

AssemblyMateEndpointView assemblyMateFirstEndpoint(
    const AssemblyMate& mate) noexcept {
  return std::visit(
      [](const auto& value) -> AssemblyMateEndpointView {
        return std::cref(value.first());
      },
      mate);
}

AssemblyMateEndpointView assemblyMateSecondEndpoint(
    const AssemblyMate& mate) noexcept {
  return std::visit(
      [](const auto& value) -> AssemblyMateEndpointView {
        return std::cref(value.second());
      },
      mate);
}

const OccurrencePath& assemblyMateFirstOccurrencePath(
    const AssemblyMate& mate) noexcept {
  return std::visit(
      [](const auto& endpoint) -> const OccurrencePath& {
        return endpoint.get().occurrencePath;
      },
      assemblyMateFirstEndpoint(mate));
}

const OccurrencePath& assemblyMateSecondOccurrencePath(
    const AssemblyMate& mate) noexcept {
  return std::visit(
      [](const auto& endpoint) -> const OccurrencePath& {
        return endpoint.get().occurrencePath;
      },
      assemblyMateSecondEndpoint(mate));
}

AssemblyMateKind assemblyMateKind(const AssemblyMate& mate) noexcept {
  return std::visit(
      [](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, PlanarCoincidentMate>)
          return AssemblyMateKind::PlanarCoincident;
        else if constexpr (std::is_same_v<Value, PlanarParallelMate>)
          return AssemblyMateKind::PlanarParallel;
        else if constexpr (std::is_same_v<Value, PlanarPerpendicularMate>)
          return AssemblyMateKind::PlanarPerpendicular;
        else if constexpr (std::is_same_v<Value, AxisConcentricMate>)
          return AssemblyMateKind::AxisConcentric;
        else if constexpr (std::is_same_v<Value, PointDistanceMate>)
          return AssemblyMateKind::PointDistance;
        else
          return AssemblyMateKind::PlanarAngle;
      },
      mate);
}

bool isValidAssemblyMate(const AssemblyMate& mate,
                         double maximumAbsoluteOffsetMm,
                         double angularToleranceRad) noexcept {
  return std::visit(
      [maximumAbsoluteOffsetMm, angularToleranceRad](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, PlanarCoincidentMate> ||
                      std::is_same_v<Value, PointDistanceMate>)
          return value.isValid(maximumAbsoluteOffsetMm);
        else if constexpr (std::is_same_v<Value, PlanarAngleMate>)
          return value.isValid(angularToleranceRad);
        else
          return value.isValid();
      },
      mate);
}

bool MateRegistry::advanceRevision() noexcept {
  if (revision_ == std::numeric_limits<std::uint64_t>::max()) return false;
  ++revision_;
  return true;
}

MateRegistryMutationStatus MateRegistry::insert(
    PlanarCoincidentMate relation) noexcept {
  return insert(AssemblyMate{std::move(relation)});
}

MateRegistryMutationStatus MateRegistry::insert(
    PlanarParallelMate relation) noexcept {
  return insert(AssemblyMate{std::move(relation)});
}

MateRegistryMutationStatus MateRegistry::insert(
    PlanarPerpendicularMate relation) noexcept {
  return insert(AssemblyMate{std::move(relation)});
}

MateRegistryMutationStatus MateRegistry::insert(
    AxisConcentricMate relation) noexcept {
  return insert(AssemblyMate{std::move(relation)});
}

MateRegistryMutationStatus MateRegistry::insert(
    PointDistanceMate relation) noexcept {
  return insert(AssemblyMate{std::move(relation)});
}

MateRegistryMutationStatus MateRegistry::insert(
    PlanarAngleMate relation) noexcept {
  return insert(AssemblyMate{std::move(relation)});
}

MateRegistryMutationStatus MateRegistry::insert(AssemblyMate relation) noexcept {
  if (!isValidAssemblyMate(relation, options_.maximumAbsoluteOffsetMm,
                           options_.angularToleranceRad))
    return MateRegistryMutationStatus::Invalid;
  const MateId id = assemblyMateId(relation);
  if (relations_.contains(id))
    return MateRegistryMutationStatus::Duplicate;
  if (relations_.size() >= options_.maximumMates)
    return MateRegistryMutationStatus::BudgetExceeded;
  if (revision_ == std::numeric_limits<std::uint64_t>::max())
    return MateRegistryMutationStatus::RevisionExhausted;
  try {
    relations_.emplace(id, std::move(relation));
    ++revision_;
    return MateRegistryMutationStatus::Inserted;
  } catch (const std::bad_alloc&) {
    return MateRegistryMutationStatus::ResourceFailure;
  } catch (...) {
    return MateRegistryMutationStatus::ResourceFailure;
  }
}

MateRegistryMutationStatus MateRegistry::setOptions(
    const MateSolverOptions& options) noexcept {
  if (!options.isValid()) return MateRegistryMutationStatus::Invalid;
  if (options == options_) return MateRegistryMutationStatus::Unchanged;
  if (relations_.size() > options.maximumMates)
    return MateRegistryMutationStatus::BudgetExceeded;
  for (const auto& [id, relation] : relations_) {
    static_cast<void>(id);
    if (!isValidAssemblyMate(relation, options.maximumAbsoluteOffsetMm,
                             options.angularToleranceRad))
      return MateRegistryMutationStatus::Invalid;
  }
  if (!advanceRevision()) return MateRegistryMutationStatus::RevisionExhausted;
  options_ = options;
  return MateRegistryMutationStatus::Inserted;
}

std::optional<MateId> MateRegistry::lowestFreeId() const noexcept {
  std::uint64_t expected = 1;
  for (const auto& [id, relation] : relations_) {
    static_cast<void>(relation);
    if (id.value() != expected) return MateId{expected};
    if (expected == std::numeric_limits<std::uint64_t>::max())
      return std::nullopt;
    ++expected;
  }
  return MateId{expected};
}

MateRegistryMutationStatus MateRegistry::erase(MateId id) noexcept {
  const auto found = relations_.find(id);
  if (found == relations_.end()) return MateRegistryMutationStatus::Missing;
  if (!advanceRevision()) return MateRegistryMutationStatus::RevisionExhausted;
  relations_.erase(found);
  return MateRegistryMutationStatus::Erased;
}

const PlanarCoincidentMate* MateRegistry::find(MateId id) const noexcept {
  const auto* mate = findMate(id);
  return mate ? std::get_if<PlanarCoincidentMate>(mate) : nullptr;
}

const AssemblyMate* MateRegistry::findMate(MateId id) const noexcept {
  const auto found = relations_.find(id);
  return found == relations_.end() ? nullptr : &found->second;
}

const PlanarParallelMate* MateRegistry::findParallel(MateId id) const noexcept {
  const auto* mate = findMate(id);
  return mate ? std::get_if<PlanarParallelMate>(mate) : nullptr;
}

const PlanarPerpendicularMate* MateRegistry::findPerpendicular(
    MateId id) const noexcept {
  const auto* mate = findMate(id);
  return mate ? std::get_if<PlanarPerpendicularMate>(mate) : nullptr;
}

const AxisConcentricMate* MateRegistry::findConcentric(
    MateId id) const noexcept {
  const auto* mate = findMate(id);
  return mate ? std::get_if<AxisConcentricMate>(mate) : nullptr;
}

const PointDistanceMate* MateRegistry::findDistance(MateId id) const noexcept {
  const auto* mate = findMate(id);
  return mate ? std::get_if<PointDistanceMate>(mate) : nullptr;
}

const PlanarAngleMate* MateRegistry::findAngle(MateId id) const noexcept {
  const auto* mate = findMate(id);
  return mate ? std::get_if<PlanarAngleMate>(mate) : nullptr;
}

std::uint64_t MateRegistry::fingerprint() const noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  hashValue(hash, std::bit_cast<std::uint64_t>(options_.linearToleranceMm));
  hashValue(hash, std::bit_cast<std::uint64_t>(options_.angularToleranceRad));
  hashValue(hash, std::bit_cast<std::uint64_t>(options_.rankTolerance));
  hashValue(hash,
            std::bit_cast<std::uint64_t>(options_.maximumAbsoluteOffsetMm));
  hashValue(hash,
            std::bit_cast<std::uint64_t>(options_.maximumTranslationStepMm));
  hashValue(hash,
            std::bit_cast<std::uint64_t>(options_.maximumAngularStepRad));
  hashValue(hash, options_.maximumIterations);
  hashValue(hash, options_.maximumNodes);
  hashValue(hash, options_.maximumMates);
  hashValue(hash, options_.maximumWorkUnits);
  for (const auto& [id, relation] : relations_) {
    hashValue(hash, id.value());
    hashValue(hash, assemblyMateKind(relation));
    for (const auto* path : {&assemblyMateFirstOccurrencePath(relation),
                             &assemblyMateSecondOccurrencePath(relation)}) {
      const auto depth = static_cast<std::uint64_t>(path->depth());
      hashValue(hash, depth);
      for (const auto occurrence : path->entries())
        hashValue(hash, occurrence.value());
    }
    if (const auto* axis = std::get_if<AxisConcentricMate>(&relation)) {
      for (const auto* endpoint : {&axis->first(), &axis->second()}) {
        const std::uint8_t discriminator =
            std::holds_alternative<LocalDatumAxis>(endpoint->reference) ? 0 : 1;
        hashValue(hash, discriminator);
        if (const auto* datum =
                std::get_if<LocalDatumAxis>(&endpoint->reference))
          hashValue(hash, *datum);
        else
          hashReference(
              hash,
              std::get<CylindricalFaceAxis>(endpoint->reference).face);
      }
    } else if (const auto* distance =
                   std::get_if<PointDistanceMate>(&relation)) {
      for (const auto* endpoint : {&distance->first(), &distance->second()}) {
        const std::uint8_t discriminator =
            std::holds_alternative<LocalDatumPoint>(endpoint->reference) ? 0 : 1;
        hashValue(hash, discriminator);
        if (const auto* datum =
                std::get_if<LocalDatumPoint>(&endpoint->reference))
          hashValue(hash, *datum);
        else
          hashReference(
              hash,
              std::get<CircularEdgeCenter>(endpoint->reference).edge);
      }
    } else {
      hashReference(hash, assemblyPlanarMateFirst(relation)->face);
      hashReference(hash, assemblyPlanarMateSecond(relation)->face);
    }
    std::visit(
        [&hash](const auto& value) {
          using Value = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, PlanarCoincidentMate> ||
                        std::is_same_v<Value, PlanarParallelMate> ||
                        std::is_same_v<Value, PlanarAngleMate>)
            hashValue(hash, value.orientation());
          if constexpr (std::is_same_v<Value, PlanarCoincidentMate>)
            hashValue(hash,
                      std::bit_cast<std::uint64_t>(value.offsetMm()));
          if constexpr (std::is_same_v<Value, PointDistanceMate>)
            hashValue(hash,
                      std::bit_cast<std::uint64_t>(value.distanceMm()));
          if constexpr (std::is_same_v<Value, PlanarAngleMate>)
            hashValue(hash,
                      std::bit_cast<std::uint64_t>(value.angleRad()));
        },
        relation);
  }
  return hash;
}

}  // namespace solidar
