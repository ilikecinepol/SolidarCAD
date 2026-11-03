#include "project/internal/ProjectFormatV4CandidateComparison.h"

namespace solidar::project::detail {

bool sameProjectFormatV4Candidate(
    const ProjectFormatV4ModelCandidate& lhs,
    const ProjectFormatV4ModelCandidate& rhs) {
  const auto& ll = lhs.legacySketch;
  const auto& rl = rhs.legacySketch;
  const auto& lt = ll.topology;
  const auto& rt = rl.topology;
  const auto& lp = lt.payload;
  const auto& rp = rt.payload;
  const auto& le = lp.envelope;
  const auto& re = rp.envelope;
  return ll.deferredLegacySketch == rl.deferredLegacySketch &&
         le.authoredRoot == re.authoredRoot && le.document == re.document &&
         le.legacySketches == re.legacySketches &&
         le.legacyExtrusion == re.legacyExtrusion && le.model == re.model &&
         le.capabilities == re.capabilities &&
         le.deferredUnknowns == re.deferredUnknowns &&
         lp.topologySites == rp.topologySites &&
         lp.featureOwners == rp.featureOwners &&
         lp.rebuildWork == rp.rebuildWork &&
         lt.topologyReferences == rt.topologyReferences &&
         lt.topologyInspections == rt.topologyInspections &&
         lt.deferredUnsupported == rt.deferredUnsupported &&
         lt.provenanceRecordCount == rt.provenanceRecordCount &&
         lt.localNameUtf8Bytes == rt.localNameUtf8Bytes;
}

}  // namespace solidar::project::detail
