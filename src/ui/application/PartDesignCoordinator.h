#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "model/ChamferToolSession.h"
#include "model/CircularPatternToolSession.h"
#include "model/DraftToolSession.h"
#include "model/ExtrudeToolSession.h"
#include "model/FilletToolSession.h"
#include "model/JoinBodiesToolSession.h"
#include "model/LinearPatternToolSession.h"
#include "model/MirrorToolSession.h"
#include "model/MoveToolSession.h"
#include "model/RevolveToolSession.h"
#include "model/ShellToolSession.h"
#include "ui/tools/PartDesignToolController.h"

namespace solidar {

enum class PartDesignTransitionEffect {
  None, Activated, CommitStarted, Accepted, Rejected, Cancelled,
  ReselectionCancelled, SelectionCleared,
};

struct PartDesignRevisionToken {
  std::uint64_t value{};
  PartDesignToolKind activeTool{PartDesignToolKind::None};
  bool operator==(const PartDesignRevisionToken&) const = default;
};

struct PartDesignTransitionOutcome {
  PartDesignTransitionEffect effect{PartDesignTransitionEffect::None};
  PartDesignToolKind previousTool{PartDesignToolKind::None};
  PartDesignToolKind activeTool{PartDesignToolKind::None};
  PartDesignRevisionToken revision;
  OperationFailure failure;
};

// Immutable value projection. It contains no session pointer and cannot
// mutate coordinator-owned state.
struct PartDesignToolSnapshot {
  PartDesignToolKind kind{PartDesignToolKind::None};
  ToolLifecycle lifecycle{ToolLifecycle::Inactive};
  ToolSelectionStage selectionStage{ToolSelectionStage::None};
  BodyId bodyId{kInvalidBodyId};
  FeatureId sourceFeatureId{kInvalidFeatureId};
  std::optional<FeatureId> editingFeatureId;
  ShapeFeature::ShapePtr previewShape;
  ShapeFeature::ShapePtr subtractivePreviewShape;
  OperationFailure failure;
  std::string error;
  OperationFailureCode errorCode{OperationFailureCode::None};
  std::vector<EdgeReference> edges;
  std::vector<FaceReference> faces;
  std::vector<JoinBodyInput> bodies;
  FaceReference face;
  double radiusMm{};
  double distanceMm{};
  double thicknessMm{};
  double lengthMm{};
  double angleDeg{};
  double spacingMm{};
  int count{};
  bool outside{};
  bool reversed{};
  bool sketchSource{};
  bool limitReached{};
  std::optional<double> maximumValidValueMm;
  Vector3d offsetMm{};
  SketchId profileSketchId{kInvalidSketchId};
  std::optional<sketch::Sketch> profileOverride;
  ExtrudeOperation extrudeOperation{ExtrudeOperation::NewBody};
  PatternOperation patternOperation{PatternOperation::NewBody};
  std::optional<AxisReference> axisReference;
  std::optional<PrincipalAxis> principalAxis;
  std::optional<PrincipalAxis> principalDirection;
  std::optional<MirrorPlane> mirrorPlane;
  std::optional<PlaneReference> neutralPlane;
  std::optional<AxisReference> pullDirection;
  std::optional<EdgeReference> rotationEdge;
  std::optional<int> principalAxisIndex;
  std::vector<ToolParameterDescriptor> parameters;
  std::optional<LinearToolManipulator> linearManipulator;
  std::optional<AngularToolManipulator> angularManipulator;
  std::optional<TranslationToolManipulator> translationManipulator;
};

struct PartDesignUiEffect {
  PartDesignTransitionOutcome transition;
  PartDesignToolSnapshot state;
  bool clearPreview{};
  bool clearSelection{};
  bool preserveSelectionMode{};
  bool beginInputSelection{};
  bool hidePanel{};
  bool refreshPreview{};
  struct ExtrudeCommit {};
  struct RevolveCommit {};
  struct FilletCommit {};
  struct ChamferCommit {};
  struct JoinBodiesCommit {};
  struct ShellCommit {};
  struct DraftCommit {};
  struct MirrorCommit {};
  struct MoveCommit {};
  struct LinearPatternCommit {};
  struct CircularPatternCommit {};
  using CommitRequest =
      std::variant<std::monostate, ExtrudeCommit, RevolveCommit, FilletCommit,
                   ChamferCommit, JoinBodiesCommit, ShellCommit, DraftCommit,
                   MirrorCommit, MoveCommit, LinearPatternCommit,
                   CircularPatternCommit>;
  CommitRequest commitRequest;
  CommitRequest target;
};

enum class PartDesignAction { Apply, Cancel, Escape, ClearSelection };
enum class PartDesignParameterSource { Panel, Manipulator, Boundary };

class PartDesignCoordinator;
class PartDesignCommitGuard final {
 public:
  PartDesignCommitGuard() = default;
  PartDesignCommitGuard(const PartDesignCommitGuard&) = delete;
  PartDesignCommitGuard& operator=(const PartDesignCommitGuard&) = delete;
  PartDesignCommitGuard(PartDesignCommitGuard&& other) noexcept;
  PartDesignCommitGuard& operator=(PartDesignCommitGuard&& other) noexcept;
  ~PartDesignCommitGuard();
  [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }
  PartDesignUiEffect accept() noexcept;

 private:
  friend class PartDesignCoordinator;
  explicit PartDesignCommitGuard(PartDesignCoordinator* owner,
                                 PartDesignRevisionToken token)
      : owner_(owner), token_(token) {}
  PartDesignCoordinator* owner_{};
  PartDesignRevisionToken token_{};
};

class PartDesignCoordinator final {
 public:
  PartDesignCoordinator();
  PartDesignCoordinator(const PartDesignCoordinator&) = delete;
  PartDesignCoordinator& operator=(const PartDesignCoordinator&) = delete;
  PartDesignCoordinator(PartDesignCoordinator&&) = delete;
  PartDesignCoordinator& operator=(PartDesignCoordinator&&) = delete;

  [[nodiscard]] PartDesignCommitGuard startCommit() noexcept;
  PartDesignTransitionOutcome cancelActive() noexcept;
  PartDesignTransitionOutcome prepareDocumentReplacement() noexcept;
  PartDesignTransitionOutcome cancelAll() noexcept;
  PartDesignUiEffect dispatchActiveAction(PartDesignAction,
                                          const Document* = nullptr);
  PartDesignUiEffect dispatchActiveInput(const Document&, double,
                                         PartDesignParameterSource);
  void beginReselection(ToolSelectionStage);
  void finishReselection() noexcept;

  [[nodiscard]] PartDesignToolKind activeTool() const noexcept;
  [[nodiscard]] ToolSelectionStage selectionStage() const noexcept;
  [[nodiscard]] bool isReselecting() const noexcept;
  [[nodiscard]] bool commitPending() const noexcept;
  [[nodiscard]] std::size_t sessionCount() const noexcept;
  [[nodiscard]] PartDesignRevisionToken revisionToken() const noexcept;
  [[nodiscard]] bool isCurrent(PartDesignRevisionToken) const noexcept;
  [[nodiscard]] bool invariantHolds() const noexcept;
  [[nodiscard]] PartDesignToolSnapshot snapshot(PartDesignToolKind) const;
  [[nodiscard]] PartDesignToolSnapshot snapshot(
      PartDesignToolKind, const Document&) const;

  PartDesignTransitionOutcome beginFillet(BodyId, FeatureId, ShapeFeature::ShapePtr,
                   std::vector<EdgeReference>, double,
                   std::optional<FeatureId> = std::nullopt,
                   std::shared_ptr<const TopologyIndex> = {});
  void setFilletEdges(std::vector<EdgeReference>);
  void setFilletRadiusFromPanel(double);
  void setFilletRadiusFromManipulator(double);
  bool refineFilletRadiusToBoundary(double);
  PartDesignTransitionOutcome beginChamfer(BodyId, FeatureId, ShapeFeature::ShapePtr,
                    std::vector<EdgeReference>, double,
                    std::optional<FeatureId> = std::nullopt,
                    std::shared_ptr<const TopologyIndex> = {});
  void setChamferEdges(std::vector<EdgeReference>);
  void setChamferDistanceFromPanel(double);
  void setChamferDistanceFromManipulator(double);
  bool refineChamferDistanceToBoundary(double);
  PartDesignTransitionOutcome beginJoinBodies(std::optional<FeatureId> = std::nullopt,
                       BodyId = kInvalidBodyId);
  void setJoinBodies(std::vector<JoinBodyInput>);
  PartDesignTransitionOutcome beginShell(BodyId, FeatureId, ShapeFeature::ShapePtr,
                  std::vector<FaceReference> = {}, double = 2.0,
                  bool = false, std::optional<FeatureId> = std::nullopt,
                  std::shared_ptr<const TopologyIndex> = {});
  void setShellRemovedFaces(std::vector<FaceReference>);
  void setShellThicknessFromPanel(double);
  void setShellThicknessFromManipulator(double);
  bool refineShellThicknessToBoundary(double);
  void setShellOutside(bool);

  PartDesignTransitionOutcome beginDraft(const Document&, BodyId, FeatureId, ShapeFeature::ShapePtr,
                  std::vector<FaceReference> = {},
                  std::optional<PlaneReference> = std::nullopt,
                  std::optional<AxisReference> = std::nullopt,
                  double = 5.0, bool = false,
                  std::optional<FeatureId> = std::nullopt,
                  std::optional<EdgeReference> = std::nullopt,
                  std::shared_ptr<const TopologyIndex> = {});
  void setDraftFaces(const Document&, std::vector<FaceReference>);
  bool setDraftPrincipalAxis(const Document&, int);
  bool setDraftRotationEdge(const Document&, EdgeReference);
  void clearDraftPrincipalAxis(const Document&);
  void setDraftAngleFromPanel(const Document&, double);
  void setDraftAngleFromManipulator(const Document&, double);

  PartDesignTransitionOutcome beginExtrude(BodyId, FeatureId, ShapeFeature::ShapePtr, FaceReference,
                    double, ExtrudeOperation, bool,
                    std::optional<FeatureId> = std::nullopt,
                    std::shared_ptr<const TopologyIndex> = {});
  PartDesignTransitionOutcome beginSketchExtrude(DocumentSketch, SketchId, ShapeFeature::ShapePtr,
                          double, ExtrudeOperation, bool,
                          std::optional<FeatureId> = std::nullopt,
                          std::optional<sketch::Sketch> = std::nullopt);
  void setExtrudeFace(FaceReference);
  void setExtrudeLengthFromPanel(double);
  void setExtrudeLengthFromManipulator(double);
  void setExtrudeOperation(ExtrudeOperation);
  void setExtrudeOperationFollowsDirection(bool);

  PartDesignTransitionOutcome beginRevolve(const Document&, BodyId, FeatureId,
                    ShapeFeature::ShapePtr = {},
                    std::optional<FeatureId> = std::nullopt);
  void setRevolveProfile(const Document&, SketchId,
                         std::optional<sketch::Sketch> = std::nullopt);
  void clearRevolveProfile(const Document&);
  void setRevolveAxis(const Document&, AxisReference);
  void clearRevolveAxis(const Document&);
  void setRevolveAngleFromPanel(const Document&, double);
  void setRevolveAngleFromManipulator(const Document&, double);
  void setRevolveOperation(const Document&, ExtrudeOperation);
  void setRevolveReversed(const Document&, bool);

  PartDesignTransitionOutcome beginMirror(std::optional<FeatureId> = std::nullopt);
  void setMirrorBody(BodyId, FeatureId, ShapeFeature::ShapePtr);
  void clearMirrorBody();
  void setMirrorPlane(MirrorPlane);
  void clearMirrorPlane();
  PartDesignTransitionOutcome beginMove(Vector3d = {}, std::optional<FeatureId> = std::nullopt);
  void setMoveBody(BodyId, FeatureId, ShapeFeature::ShapePtr);
  void clearMoveBody();
  void setMoveOffset(Vector3d);
  void setMoveOffsetComponent(int, double);

  PartDesignTransitionOutcome beginLinearPattern(double = 30.0, int = 3,
                          PatternOperation = PatternOperation::NewBody,
                          std::optional<FeatureId> = std::nullopt);
  void setLinearPatternBody(BodyId, FeatureId, ShapeFeature::ShapePtr);
  void clearLinearPatternBody();
  void setLinearPatternDirection(PrincipalAxis);
  void clearLinearPatternDirection();
  void setLinearPatternSpacing(double);
  void setLinearPatternCount(int);
  void setLinearPatternOperation(PatternOperation);
  PartDesignTransitionOutcome beginCircularPattern(double = kMaximumPatternAngleDeg, int = 4,
                            PatternOperation = PatternOperation::NewBody,
                            std::optional<FeatureId> = std::nullopt);
  void setCircularPatternBody(BodyId, FeatureId, ShapeFeature::ShapePtr);
  void clearCircularPatternBody();
  void setCircularPatternAxis(PrincipalAxis);
  void clearCircularPatternAxis();
  void setCircularPatternAngle(double);
  void setCircularPatternCount(int);
  void setCircularPatternOperation(PatternOperation);

 private:
  friend class PartDesignCommitGuard;
  PartDesignTransitionOutcome beginCommit(PartDesignToolKind) noexcept;
  PartDesignTransitionOutcome completeCommit(PartDesignRevisionToken,
                                               bool accepted) noexcept;
  template <typename Prepare>
  PartDesignTransitionOutcome beginAtomic(PartDesignToolKind, Prepare&&);
  PartDesignTransitionOutcome rejectBegin(
      PartDesignToolKind,
      OperationFailure = {OperationFailureCode::InvalidInput,
                          "Invalid tool input"}) noexcept;
  [[nodiscard]] ToolSession* session(PartDesignToolKind) noexcept;
  [[nodiscard]] const ToolSession* session(PartDesignToolKind) const noexcept;
  void advanceRevision() noexcept;

  FilletToolSession fillet_;
  ChamferToolSession chamfer_;
  JoinBodiesToolSession joinBodies_;
  ShellToolSession shell_;
  DraftToolSession draft_;
  ExtrudeToolSession extrude_;
  RevolveToolSession revolve_;
  MirrorToolSession mirror_;
  MoveToolSession move_;
  LinearPatternToolSession linearPattern_;
  CircularPatternToolSession circularPattern_;
  PartDesignToolController controller_;
  PartDesignToolKind committingTool_{PartDesignToolKind::None};
  PartDesignRevisionToken committingToken_{};
  std::uint64_t revision_{1};
};

}  // namespace solidar
