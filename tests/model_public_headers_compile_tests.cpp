#include "model/Body.h"
#include "model/ChamferBuilder.h"
#include "model/ChamferFeature.h"
#include "model/ChamferToolSession.h"
#include "model/CircularPatternFeature.h"
#include "model/CircularPatternToolSession.h"
#include "model/Document.h"
#include "model/DraftBuilder.h"
#include "model/DraftFeature.h"
#include "model/DraftToolSession.h"
#include "model/EdgeFeatureLimits.h"
#include "model/EdgeManipulatorGeometry.h"
#include "model/EdgeReference.h"
#include "model/ExtrudeFeature.h"
#include "model/ExtrudeOperationDetector.h"
#include "model/ExtrudeSource.h"
#include "model/ExtrudeToolSession.h"
#include "model/FaceExtrudeBuilder.h"
#include "model/FaceBoundaryEdgeProvenance.h"
#include "model/Feature.h"
#include "model/FeatureTopologySnapshot.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"
#include "model/FilletToolSession.h"
#include "model/GeometryOperation.h"
#include "model/IdGeneration.h"
#include "model/ImportedShapeFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/JoinBodiesToolSession.h"
#include "model/LinearPatternFeature.h"
#include "model/LinearPatternToolSession.h"
#include "model/MirrorFeature.h"
#include "model/MirrorToolSession.h"
#include "model/MoveFeature.h"
#include "model/MoveToolSession.h"
#include "model/NumericParameterState.h"
#include "model/OcctFacePrismTopologyHistoryCapture.h"
#include "model/OcctRevolveTopologyHistoryCapture.h"
#include "model/OcctTopologyHistoryCapture.h"
#include "model/OcctBooleanTopologyHistoryCapture.h"
#include "model/OperationFailure.h"
#include "model/PartDesignToolFramework.h"
#include "model/PatternTypes.h"
#include "model/PocketFeature.h"
#include "model/ProductDocument.h"
#include "model/ProjectId.h"
#include "model/RevolveFeature.h"
#include "model/RevolveToolSession.h"
#include "model/ShapeContainerUtils.h"
#include "model/ShapeFeature.h"
#include "model/ShellBuilder.h"
#include "model/ShellFeature.h"
#include "model/ShellToolSession.h"
#include "model/SketchExtrudeBuilder.h"
#include "model/SketchPlacement.h"
#include "model/SketchProfileBuilder.h"
#include "model/ToolSession.h"
#include "model/TopologyReference.h"
#include "model/TopologyReferenceResolver.h"
#include "model/TopologyLineage.h"
#include "model/TopologyLineageBuilder.h"
#include "model/TransformFeatureHistoryBuilder.h"
#include "model/macro/MacroDefinition.h"
#include "model/macro/MacroExecutor.h"
#include "model/macro/MacroReferences.h"
#include "sketch/Sketch.h"
#include "sketch/SketchConstraintDiagnostics.h"
#include "sketch/SketchSolver.h"
#include "sketch/ConstraintGraph.h"
#include "sketch/ParameterLayout.h"
#include "sketch/ConstraintEquationRegistry.h"
#include "sketch/EquationSystem.h"
#include "sketch/EquationDiagnostics.h"

#include <cstdlib>
#include <type_traits>
#include <utility>
#include <variant>

static_assert(static_cast<unsigned>(solidar::FeatureKind::ImportedShape) == 1);
static_assert(solidar::kPersistedFeatureKindCount == 13);
static_assert(std::is_same_v<solidar::EdgeReference::Storage,
                             std::variant<solidar::TopologyReference,
                                          solidar::TopologyReferenceV2>>);
static_assert(std::is_same_v<decltype(
                                 std::declval<const solidar::EdgeReference&>()
                                     .storage()),
                             const solidar::EdgeReference::Storage&>);
static_assert(std::is_same_v<decltype(
                                 std::declval<const solidar::EdgeReference&>()
                                     .edgeIndex()),
                             const std::size_t&>);
static_assert(!std::is_convertible_v<solidar::EdgeReference,
                                     solidar::TopologyReference>);
static_assert(!std::is_convertible_v<std::uint64_t, solidar::DefinitionId>);
static_assert(!std::is_convertible_v<solidar::DefinitionId, std::uint64_t>);
static_assert(!std::is_convertible_v<solidar::ProjectId::Bytes,
                                     solidar::ProjectId>);
static_assert(std::is_trivially_copyable_v<solidar::ProjectId>);
static_assert(sizeof(solidar::ProjectId) == 16);
static_assert(noexcept(std::declval<const solidar::ProjectId&>().isValid()));
static_assert(!std::is_copy_constructible_v<solidar::PartDefinition>);
static_assert(std::is_nothrow_move_constructible_v<solidar::PartDefinition>);
static_assert(!std::is_copy_constructible_v<solidar::DefinitionRegistry>);
static_assert(std::is_nothrow_move_constructible_v<solidar::DefinitionRegistry>);
static_assert(std::is_same_v<
              decltype(std::declval<solidar::DefinitionRegistry&>().find(
                  solidar::DefinitionId{1})),
              const solidar::PartDefinition*>);
static_assert(!std::is_copy_constructible_v<solidar::ProductDocument>);
static_assert(std::is_nothrow_move_constructible_v<solidar::ProductDocument>);
static_assert(std::is_same_v<
              decltype(std::declval<solidar::ProductDocument&>()
                           .rootDocument()),
              solidar::Document&>);
static_assert(std::is_same_v<
              decltype(std::declval<const solidar::ProductDocument&>()
                           .rootDocument()),
              const solidar::Document&>);
static_assert(std::is_same_v<
              decltype(std::declval<solidar::ProductDocument&>()
                           .definitions()),
              const solidar::DefinitionRegistry&>);

int main() { return EXIT_SUCCESS; }
