#include "project/ProductEnvelopeV9Materialization.h"

#include <QJsonDocument>

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

#include "model/IdGeneration.h"
#include "project/ProductEnvelopeV8Materialization.h"

namespace solidar::project {
namespace {

ProductEnvelopeV9MaterializationResult failed(QString error) {
  ProductEnvelopeV9MaterializationResult result;
  result.error = std::move(error);
  return result;
}

class DisconnectedSuccessor final : public ProductDocumentSuccessor {
 public:
  bool reject{};
  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&,
      ProductDocumentSuccessorImpact) noexcept override {
    return reject ? ProductDocumentSuccessorStatus::Rejected
                  : ProductDocumentSuccessorStatus::Prepared;
  }
};

const ProductEnvelopeV9AssemblyCandidate* mateAssembly(
    const ProductEnvelopeV9Candidate& candidate, DefinitionId id) {
  const auto found = std::find_if(
      candidate.assemblies.begin(), candidate.assemblies.end(),
      [id](const auto& value) { return value.definitionId == id; });
  return found == candidate.assemblies.end() ? nullptr : &*found;
}

bool sameCandidate(const ProductEnvelopeV9Candidate& left,
                   const ProductEnvelopeV9Candidate& right) {
  return left.authoredRoot == right.authoredRoot &&
         left.base.authoredRoot == right.base.authoredRoot &&
         left.capabilities == right.capabilities &&
         left.assemblies == right.assemblies &&
         left.mateCount == right.mateCount &&
         left.endpointCount == right.endpointCount &&
         left.pathEntryCount == right.pathEntryCount &&
         left.provenanceRecordCount == right.provenanceRecordCount;
}

bool successful(MateSolveStatus status) {
  return status == MateSolveStatus::Solved ||
         status == MateSolveStatus::Underconstrained ||
         status == MateSolveStatus::Redundant;
}

bool retained(MateSolveStatus status) {
  return status == MateSolveStatus::Missing ||
         status == MateSolveStatus::Ambiguous ||
         status == MateSolveStatus::Broken ||
         status == MateSolveStatus::Overconstrained ||
         status == MateSolveStatus::NonConvergent;
}

std::size_t countComponents(
    const ProductDocument& product,
    const std::vector<DefinitionId>& childFirstOrder) {
  std::size_t count = 0;
  for (const auto ownerId : childFirstOrder) {
    const auto* owner = product.definitions().findAssembly(ownerId);
    if (!owner) return kProductEnvelopeV9MaximumSolveComponents + 1;
    std::set<MateId> visited;
    for (const auto& [seedId, seed] : owner->mateRegistry().relations()) {
      if (visited.contains(seedId)) continue;
      ++count;
      std::set<OccurrenceId> nodes{
          assemblyMateFirstOccurrencePath(seed).entries().front(),
          assemblyMateSecondOccurrencePath(seed).entries().front()};
      bool expanded = true;
      while (expanded) {
        expanded = false;
        for (const auto& [id, relation] : owner->mateRegistry().relations()) {
          const auto first =
              assemblyMateFirstOccurrencePath(relation).entries().front();
          const auto second =
              assemblyMateSecondOccurrencePath(relation).entries().front();
          if (!nodes.contains(first) && !nodes.contains(second)) continue;
          if (visited.insert(id).second) expanded = true;
          if (nodes.insert(first).second) expanded = true;
          if (nodes.insert(second).second) expanded = true;
        }
      }
    }
  }
  return count;
}

}  // namespace

ProductEnvelopeV9MaterializationResult materializeProductEnvelopeV9(
    const ProductEnvelopeV9Candidate& candidate,
    ProductEnvelopeV9MaterializationFailurePoint failurePoint,
    ProductEnvelopeV9MaterializationMode mode) {
  try {
    const QByteArray bytes =
        QJsonDocument(candidate.authoredRoot).toJson(QJsonDocument::Compact);
    auto rescanned = scanProductEnvelopeV9(QByteArrayView(bytes));
    if (!rescanned.hasCandidate() ||
        !sameCandidate(candidate, *rescanned.candidate))
      return failed(QStringLiteral("v9 candidate revalidation failed"));

    ::solidar::detail::ScopedExplicitIdReservationPause reservationPause;
    auto v8Failure = ProductEnvelopeV8MaterializationFailurePoint::None;
    if (failurePoint ==
        ProductEnvelopeV9MaterializationFailurePoint::BeforeLastPart)
      v8Failure =
          ProductEnvelopeV8MaterializationFailurePoint::DeepestPartRecompute;
    else if (failurePoint ==
             ProductEnvelopeV9MaterializationFailurePoint::BeforeProduct)
      v8Failure = ProductEnvelopeV8MaterializationFailurePoint::
          BeforeProductConstruction;
    auto base = materializeProductEnvelopeV8(rescanned.candidate->base,
                                             v8Failure);
    if (!base.succeeded())
      return failed(QStringLiteral("v9 inherited materialization failed: %1")
                        .arg(base.error));

    // Authoritative registry state is restored in dependency order while the
    // product is still disconnected from every application projection.
    for (const auto ownerId :
         rescanned.candidate->base.graph.childFirstAssemblyOrder) {
      const auto* authored = mateAssembly(*rescanned.candidate, ownerId);
      if (!authored)
        return failed(QStringLiteral("v9 mate assembly is missing"));
      auto registry = std::make_unique<MateRegistry>();
      const auto optionStatus = registry->setOptions(authored->options);
      if (optionStatus != MateRegistryMutationStatus::Inserted &&
          optionStatus != MateRegistryMutationStatus::Unchanged)
        return failed(QStringLiteral("v9 mate options restoration failed"));
      for (const auto& mate : authored->mates) {
        if (registry->insert(mate) != MateRegistryMutationStatus::Inserted)
          return failed(QStringLiteral("v9 mate restoration failed"));
      }
      if (base.product->restoreAssemblyMateRegistry(
              ownerId, std::move(registry)) !=
          AssemblyMateRestoreStatus::Restored)
        return failed(QStringLiteral("v9 mate registry restoration failed"));
    }

    std::uint64_t aggregateWork = 0;
    std::size_t componentCount = 0;
    const std::size_t totalComponents = countComponents(
        *base.product,
        rescanned.candidate->base.graph.childFirstAssemblyOrder);
    if (totalComponents > kProductEnvelopeV9MaximumSolveComponents)
      return failed(QStringLiteral("v9 solve component budget exceeded"));
    std::vector<MateRuntimeDiagnostics> runtime;
    DisconnectedSuccessor successor;
    for (const auto ownerId :
         rescanned.candidate->base.graph.childFirstAssemblyOrder) {
      const auto* owner = base.product->definitions().findAssembly(ownerId);
      if (!owner)
        return failed(QStringLiteral("v9 solve owner is missing"));
      std::set<MateId> visited;
      for (const auto& [mateId, relation] :
           owner->mateRegistry().relations()) {
        static_cast<void>(relation);
        if (visited.contains(mateId)) continue;
        if (++componentCount > kProductEnvelopeV9MaximumSolveComponents)
          return failed(QStringLiteral("v9 solve component budget exceeded"));
        if (failurePoint ==
                ProductEnvelopeV9MaterializationFailurePoint::BeforeLastSolve &&
            componentCount == totalComponents)
          return failed(QStringLiteral("injected v9 solve failure"));
        auto solved = solveAssemblyMates(
            *base.product, ownerId,
            MateSolveRequest{mateId, std::nullopt});
        if (solved.componentMateIds.empty())
          solved.componentMateIds.push_back(mateId);
        visited.insert(solved.componentMateIds.begin(),
                       solved.componentMateIds.end());
        if (solved.diagnostics.requiredWorkUnits >
            kProductEnvelopeV9MaximumSolveWork - aggregateWork)
          return failed(QStringLiteral("v9 aggregate solve work exceeded"));
        aggregateWork += solved.diagnostics.requiredWorkUnits;
        runtime.push_back({ownerId, solved.componentMateIds, solved.status,
                           solved.diagnostics});
        if (successful(solved.status)) {
          if (!solved.candidate)
            return failed(QStringLiteral("v9 successful solve has no candidate"));
          if (failurePoint ==
                  ProductEnvelopeV9MaterializationFailurePoint::BeforeLastApply &&
              componentCount == totalComponents)
            return failed(QStringLiteral("injected v9 apply failure"));
          successor.reject =
              failurePoint ==
                  ProductEnvelopeV9MaterializationFailurePoint::ApplyFailure &&
              componentCount == totalComponents;
          if (base.product->applyMatePlacementCandidate(*solved.candidate,
                                                        successor) !=
              MatePlacementApplyStatus::Applied)
            return failed(QStringLiteral("v9 mate application failed"));
          successor.reject = false;
          if (failurePoint == ProductEnvelopeV9MaterializationFailurePoint::
                                  AfterFirstApply &&
              componentCount == 1)
            return failed(QStringLiteral("injected v9 post-apply failure"));
        } else if (!retained(solved.status)) {
          return failed(QStringLiteral("v9 hard mate solve failure (%1)")
                            .arg(static_cast<int>(solved.status)));
        }
      }
    }
    std::sort(runtime.begin(), runtime.end(),
              [](const auto& left, const auto& right) {
                if (left.ownerAssemblyId != right.ownerAssemblyId)
                  return left.ownerAssemblyId < right.ownerAssemblyId;
                return left.componentMateIds < right.componentMateIds;
              });
    const auto finalGraph = analyzeAssemblyGraph(
        base.product->definitions(), base.product->rootDefinitionId());
    if (!finalGraph ||
        finalGraph.childFirstAssemblyOrder !=
            rescanned.candidate->base.graph.childFirstAssemblyOrder)
      return failed(QStringLiteral("v9 final graph validation failed"));
    if (failurePoint ==
        ProductEnvelopeV9MaterializationFailurePoint::Finalization)
      return failed(QStringLiteral("injected v9 finalization failure"));
    if (mode == ProductEnvelopeV9MaterializationMode::Publication &&
        !base.product->finalizeReservedIncarnation())
      return failed(QStringLiteral("v9 incarnation commit failed"));
    ProductEnvelopeV9MaterializationResult result;
    result.status = ProductEnvelopeV9MaterializationStatus::Materialized;
    result.product = std::move(base.product);
    result.mateDiagnostics = std::move(runtime);
    return result;
  } catch (const std::exception& exception) {
    return failed(QStringLiteral("v9 materialization failure: %1")
                      .arg(QString::fromUtf8(exception.what())));
  } catch (...) {
    return failed(QStringLiteral("unknown v9 materialization failure"));
  }
}

}  // namespace solidar::project
