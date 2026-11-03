#include "project/ProductEnvelopeV8Materialization.h"

#include <Standard_Failure.hxx>

#include <QJsonDocument>

#include <algorithm>
#include <exception>
#include <utility>

#include "model/IdGeneration.h"
#include "project/internal/ProjectFormatV4ApplicationGate.h"
#include "project/internal/ProjectFormatV4CandidateComparison.h"
#include "project/internal/ProjectFormatV4DocumentMaterialization.h"

namespace solidar::project {
namespace {

ProductEnvelopeV8MaterializationResult failed(QString error) {
  ProductEnvelopeV8MaterializationResult result;
  result.error = std::move(error);
  return result;
}

std::string utf8(const QString& value) {
  const QByteArray bytes = value.toUtf8();
  return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

bool sameCandidate(const ProductEnvelopeV8Candidate& left,
                   const ProductEnvelopeV8Candidate& right) {
  if (left.authoredRoot != right.authoredRoot ||
      left.projectId != right.projectId ||
      left.rootDefinitionId != right.rootDefinitionId ||
      left.metadata != right.metadata || left.capabilities != right.capabilities ||
      left.parts.size() != right.parts.size() ||
      left.assemblies != right.assemblies ||
      left.jsonStringUtf8Bytes != right.jsonStringUtf8Bytes ||
      left.partPayloadBytes != right.partPayloadBytes ||
      left.relativePathUtf8Bytes != right.relativePathUtf8Bytes ||
      left.rebuildWork != right.rebuildWork)
    return false;
  for (std::size_t i = 0; i < left.parts.size(); ++i) {
    const auto& a = left.parts[i];
    const auto& b = right.parts[i];
    if (a.definitionId != b.definitionId || a.kind != b.kind ||
        a.authoredDefinition != b.authoredDefinition ||
        a.authoredPayload != b.authoredPayload || a.binding != b.binding ||
        a.lastValid.has_value() != b.lastValid.has_value() ||
        (a.lastValid && !detail::sameProjectFormatV4Candidate(
                            *a.lastValid, *b.lastValid)))
      return false;
  }
  return left.graph.status == right.graph.status &&
         left.graph.childFirstAssemblyOrder ==
             right.graph.childFirstAssemblyOrder &&
         left.graph.authoredOccurrences == right.graph.authoredOccurrences &&
         left.graph.expandedVisits == right.graph.expandedVisits &&
         left.graph.uniqueSources == right.graph.uniqueSources &&
         left.graph.reachableSources == right.graph.reachableSources &&
         left.graph.leaves.size() == right.graph.leaves.size() &&
         std::equal(left.graph.leaves.begin(), left.graph.leaves.end(),
                    right.graph.leaves.begin(),
                    [](const AssemblyGraphLeaf& a,
                       const AssemblyGraphLeaf& b) {
                      return a.path == b.path &&
                             a.terminalPartDefinitionId ==
                                 b.terminalPartDefinitionId &&
                             a.worldTransform == b.worldTransform &&
                             a.authoredLeafFlags == b.authoredLeafFlags &&
                             a.effectiveVisible == b.effectiveVisible &&
                             a.effectiveSuppressed == b.effectiveSuppressed;
                    });
}

}  // namespace

ProductEnvelopeV8MaterializationResult materializeProductEnvelopeV8(
    const ProductEnvelopeV8Candidate& candidate,
    ProductEnvelopeV8MaterializationFailurePoint failurePoint) {
  try {
    const QByteArray bytes =
        QJsonDocument(candidate.authoredRoot).toJson(QJsonDocument::Compact);
    auto rescanned = scanProductEnvelopeV8(QByteArrayView(bytes));
    if (!rescanned.hasCandidate() ||
        !sameCandidate(candidate, *rescanned.candidate))
      return failed(QStringLiteral("v8 candidate revalidation failed"));

    DefinitionId deepestPart;
    std::size_t deepestDepth = 0;
    for (const auto& leaf : rescanned.candidate->graph.leaves) {
      if (leaf.path.depth() >= deepestDepth) {
        deepestDepth = leaf.path.depth();
        deepestPart = leaf.terminalPartDefinitionId;
      }
    }
    std::vector<PartDefinition> parts;
    std::vector<AssemblyDefinition> assemblies;
    parts.reserve(rescanned.candidate->parts.size());
    assemblies.reserve(rescanned.candidate->assemblies.size());
    {
      ::solidar::detail::ScopedExplicitIdReservationPause pause;
      for (const auto& part : rescanned.candidate->parts) {
        if (!part.lastValid)
          return failed(QStringLiteral("v8 part cache is missing"));
        auto materialized = detail::materializeProjectFormatV4Document(
            *part.lastValid);
        if (!materialized.succeeded())
          return failed(QStringLiteral("v8 part materialization failed: %1")
                            .arg(materialized.error));
        const RecomputeResult recomputed =
            materialized.document->recomputeDetailed();
        if (!detail::acceptsProjectFormatV4RecomputeResult(recomputed))
          return failed(QStringLiteral("v8 part recompute failed"));
        if (failurePoint ==
                ProductEnvelopeV8MaterializationFailurePoint::
                    DeepestPartRecompute &&
            part.definitionId == deepestPart)
          return failed(QStringLiteral(
              "injected deepest-part materialization failure"));
        auto document =
            std::make_unique<Document>(std::move(*materialized.document));
        parts.push_back(part.binding
                            ? PartDefinition(part.definitionId,
                                             std::move(document),
                                             *part.binding)
                            : PartDefinition(part.definitionId,
                                             std::move(document)));
      }
      // The analyzer's child-first order is a deterministic construction
      // schedule. Candidate lookup is by stable DefinitionId.
      for (const DefinitionId id :
           rescanned.candidate->graph.childFirstAssemblyOrder) {
        const auto found = std::find_if(
            rescanned.candidate->assemblies.begin(),
            rescanned.candidate->assemblies.end(),
            [id](const ProductEnvelopeV7AssemblyCandidate& value) {
              return value.definitionId == id;
            });
        if (found == rescanned.candidate->assemblies.end())
          return failed(QStringLiteral("v8 assembly candidate is missing"));
        AssemblyDefinition assembly(id);
        for (const auto& occurrence : found->occurrences) {
          if (assembly.insert(ComponentOccurrence(
                  occurrence.occurrenceId, occurrence.targetDefinitionId,
                  occurrence.placement, occurrence.flags,
                  occurrence.displayNameUtf8)) !=
              AssemblyOccurrenceInsertStatus::Inserted)
            return failed(QStringLiteral("v8 occurrence insertion failed"));
        }
        assemblies.push_back(std::move(assembly));
      }
    }
    ProductDocumentIdentity identity{
        rescanned.candidate->projectId,
        {utf8(rescanned.candidate->metadata.name),
         utf8(rescanned.candidate->metadata.createdAt)}};
    if (failurePoint == ProductEnvelopeV8MaterializationFailurePoint::
                            BeforeProductConstruction)
      return failed(QStringLiteral("injected pre-product failure"));
    auto created = ProductDocument::createNested(
        std::move(parts), std::move(assemblies),
        rescanned.candidate->rootDefinitionId, std::move(identity));
    if (!created)
      return failed(QStringLiteral("v8 atomic product construction failed"));
    ProductEnvelopeV8MaterializationResult result;
    result.status = ProductEnvelopeV8MaterializationStatus::Materialized;
    result.product = std::move(created.product);
    return result;
  } catch (const Standard_Failure& failure) {
    return failed(QStringLiteral("OCCT failure while materializing v8: %1")
                      .arg(QString::fromUtf8(failure.what())));
  } catch (const std::exception& exception) {
    return failed(QStringLiteral("v8 materialization failure: %1")
                      .arg(QString::fromUtf8(exception.what())));
  } catch (...) {
    return failed(QStringLiteral("unknown v8 materialization failure"));
  }
}

}  // namespace solidar::project
