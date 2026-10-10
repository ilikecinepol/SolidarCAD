#include "ui/application/PartDesignCoordinator.h"

#include <array>
#include <utility>

namespace solidar {
namespace {
constexpr std::array kSessionKinds{
    PartDesignToolKind::Extrude, PartDesignToolKind::Revolve,
    PartDesignToolKind::Fillet, PartDesignToolKind::Chamfer,
    PartDesignToolKind::JoinBodies, PartDesignToolKind::Move,
    PartDesignToolKind::Mirror, PartDesignToolKind::LinearPattern,
    PartDesignToolKind::CircularPattern, PartDesignToolKind::Shell,
    PartDesignToolKind::Draft,
};

PartDesignUiEffect::CommitRequest targetFor(PartDesignToolKind kind) {
  switch (kind) {
    case PartDesignToolKind::Extrude: return PartDesignUiEffect::ExtrudeCommit{};
    case PartDesignToolKind::Revolve: return PartDesignUiEffect::RevolveCommit{};
    case PartDesignToolKind::Fillet: return PartDesignUiEffect::FilletCommit{};
    case PartDesignToolKind::Chamfer: return PartDesignUiEffect::ChamferCommit{};
    case PartDesignToolKind::JoinBodies: return PartDesignUiEffect::JoinBodiesCommit{};
    case PartDesignToolKind::Shell: return PartDesignUiEffect::ShellCommit{};
    case PartDesignToolKind::Draft: return PartDesignUiEffect::DraftCommit{};
    case PartDesignToolKind::Mirror: return PartDesignUiEffect::MirrorCommit{};
    case PartDesignToolKind::Move: return PartDesignUiEffect::MoveCommit{};
    case PartDesignToolKind::LinearPattern: return PartDesignUiEffect::LinearPatternCommit{};
    case PartDesignToolKind::CircularPattern: return PartDesignUiEffect::CircularPatternCommit{};
    case PartDesignToolKind::None:
    case PartDesignToolKind::Pocket: return std::monostate{};
  }
  return std::monostate{};
}
}

PartDesignCommitGuard::PartDesignCommitGuard(
    PartDesignCommitGuard&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), token_(other.token_) {}
PartDesignCommitGuard& PartDesignCommitGuard::operator=(
    PartDesignCommitGuard&& other) noexcept {
  if (this == &other) return *this;
  if (owner_) static_cast<void>(owner_->completeCommit(token_, false));
  owner_ = std::exchange(other.owner_, nullptr);
  token_ = other.token_;
  return *this;
}
PartDesignCommitGuard::~PartDesignCommitGuard() {
  if (owner_) static_cast<void>(owner_->completeCommit(token_, false));
}
PartDesignUiEffect PartDesignCommitGuard::accept() noexcept {
  if (!owner_) return {};
  auto* owner = std::exchange(owner_, nullptr);
  PartDesignUiEffect effect;
  effect.transition = owner->completeCommit(token_, true);
  if (effect.transition.effect == PartDesignTransitionEffect::Accepted) {
    effect.clearPreview = true;
    effect.clearSelection = true;
    effect.hidePanel = true;
  }
  return effect;
}

PartDesignCoordinator::PartDesignCoordinator() {
  for (const auto kind : kSessionKinds)
    controller_.registerTool(kind, {session(kind)});
}

PartDesignTransitionOutcome PartDesignCoordinator::beginCommit(
    PartDesignToolKind kind) noexcept {
  const auto previous = activeTool();
  const auto* current = session(kind);
  if (commitPending() || previous != kind || !current ||
      current->lifecycle() != ToolLifecycle::PreviewValid)
    return {PartDesignTransitionEffect::None, previous, previous,
            revisionToken()};
  committingTool_ = kind;
  advanceRevision();
  committingToken_ = revisionToken();
  return {PartDesignTransitionEffect::CommitStarted, previous, previous,
          committingToken_};
}

PartDesignCommitGuard PartDesignCoordinator::startCommit() noexcept {
  const auto outcome = beginCommit(activeTool());
  return outcome.effect == PartDesignTransitionEffect::CommitStarted
             ? PartDesignCommitGuard(this, outcome.revision)
             : PartDesignCommitGuard{};
}

PartDesignTransitionOutcome PartDesignCoordinator::completeCommit(
    PartDesignRevisionToken token, bool accepted) noexcept {
  const auto kind = committingTool_;
  if (kind == PartDesignToolKind::None || token != committingToken_ ||
      token != revisionToken())
    return {PartDesignTransitionEffect::None, activeTool(), activeTool(),
            revisionToken()};
  committingTool_ = PartDesignToolKind::None;
  committingToken_ = {};
  if (!accepted) {
    advanceRevision();
    return {PartDesignTransitionEffect::Rejected, kind, kind,
            revisionToken()};
  }
  controller_.deactivate(kind);
  session(kind)->cancel();
  advanceRevision();
  return {PartDesignTransitionEffect::Accepted, kind,
          PartDesignToolKind::None, revisionToken()};
}

PartDesignTransitionOutcome PartDesignCoordinator::cancelActive() noexcept {
  const auto previous = activeTool();
  if (commitPending())
    return {PartDesignTransitionEffect::Rejected, previous, previous,
            revisionToken()};
  if (previous == PartDesignToolKind::None)
    return {PartDesignTransitionEffect::None, previous, previous,
            revisionToken()};
  committingTool_ = PartDesignToolKind::None;
  controller_.cancelActive();
  advanceRevision();
  return {PartDesignTransitionEffect::Cancelled, previous,
          PartDesignToolKind::None, revisionToken()};
}

PartDesignTransitionOutcome
PartDesignCoordinator::prepareDocumentReplacement() noexcept {
  const auto previous = activeTool();
  committingTool_ = PartDesignToolKind::None;
  committingToken_ = {};
  controller_.cancelActive();
  for (const auto kind : kSessionKinds)
    if (kind != previous) session(kind)->cancel();
  advanceRevision();
  return {PartDesignTransitionEffect::Cancelled, previous,
          PartDesignToolKind::None, revisionToken()};
}

PartDesignTransitionOutcome PartDesignCoordinator::cancelAll() noexcept {
  const auto previous = activeTool();
  if (commitPending())
    return {PartDesignTransitionEffect::Rejected, previous, previous,
            revisionToken()};
  committingTool_ = PartDesignToolKind::None;
  committingToken_ = {};
  controller_.cancelActive();
  for (const auto kind : kSessionKinds)
    if (kind != previous) session(kind)->cancel();
  advanceRevision();
  return {PartDesignTransitionEffect::Cancelled, previous,
          PartDesignToolKind::None, revisionToken()};
}

PartDesignUiEffect PartDesignCoordinator::dispatchActiveAction(
    PartDesignAction action, const Document* document) {
  const auto previous = activeTool();
  PartDesignTransitionOutcome transition;
  PartDesignUiEffect effect;
  if (commitPending()) {
    effect.transition = {PartDesignTransitionEffect::Rejected, previous,
                         previous, revisionToken()};
    effect.state = snapshot(previous);
    effect.target = targetFor(previous);
    return effect;
  }
  if (action == PartDesignAction::Apply) {
    const auto state = snapshot(previous);
    if (state.lifecycle != ToolLifecycle::PreviewValid) {
      effect.transition = {PartDesignTransitionEffect::Rejected, previous,
                           previous, revisionToken()};
      effect.state = state;
      effect.target = targetFor(previous);
      return effect;
    }
    effect.commitRequest = targetFor(previous);
    effect.target = targetFor(previous);
    effect.transition = {PartDesignTransitionEffect::None, previous, previous,
                         revisionToken()};
    effect.state = state;
    return effect;
  }
  if (action == PartDesignAction::ClearSelection) {
    bool handled = true;
    switch (previous) {
      case PartDesignToolKind::Extrude:
        extrude_.setFace({});
        break;
      case PartDesignToolKind::Fillet:
        fillet_.setEdges({});
        break;
      case PartDesignToolKind::Chamfer:
        chamfer_.setEdges({});
        break;
      case PartDesignToolKind::JoinBodies:
        joinBodies_.setBodies({});
        break;
      case PartDesignToolKind::Shell:
        shell_.setRemovedFaces({});
        break;
      case PartDesignToolKind::Draft:
        if (!document) {
          handled = false;
          break;
        }
        draft_.setFaces(*document, {});
        draft_.clearPrincipalAxis(*document);
        break;
      default:
        handled = false;
        break;
    }
    effect.transition = {
        handled ? PartDesignTransitionEffect::SelectionCleared
                : PartDesignTransitionEffect::None,
        previous, previous, revisionToken()};
    effect.state = document ? snapshot(previous, *document) : snapshot(previous);
    effect.target = targetFor(previous);
    effect.clearSelection = handled;
    effect.preserveSelectionMode = handled;
    effect.beginInputSelection = handled && previous == PartDesignToolKind::Draft;
    effect.refreshPreview = handled;
    return effect;
  }
  if (action == PartDesignAction::Escape && controller_.isReselecting()) {
    static_cast<void>(controller_.handleEscape());
    advanceRevision();
    transition = {PartDesignTransitionEffect::ReselectionCancelled, previous,
                  previous, revisionToken()};
  } else {
    transition = cancelActive();
  }
  effect.transition = transition;
  effect.state = snapshot(previous);
  effect.target = targetFor(previous);
  effect.clearPreview = transition.effect == PartDesignTransitionEffect::Cancelled;
  effect.clearSelection = effect.clearPreview;
  effect.hidePanel = effect.clearPreview;
  return effect;
}

PartDesignUiEffect PartDesignCoordinator::dispatchActiveInput(
    const Document& document, double value, PartDesignParameterSource source) {
  const auto kind = activeTool();
  if (commitPending()) {
    PartDesignUiEffect effect;
    effect.transition = {PartDesignTransitionEffect::Rejected, kind, kind,
                         revisionToken()};
    effect.state = snapshot(kind, document);
    effect.target = targetFor(kind);
    return effect;
  }
  bool inputHandled = true;
  switch (kind) {
    case PartDesignToolKind::Fillet:
      if (source == PartDesignParameterSource::Boundary)
        static_cast<void>(fillet_.refineRadiusToBoundary(value));
      else if (source == PartDesignParameterSource::Panel)
        fillet_.setRadiusFromPanel(value);
      else
        fillet_.setRadiusFromManipulator(value);
      break;
    case PartDesignToolKind::Chamfer:
      if (source == PartDesignParameterSource::Boundary)
        static_cast<void>(chamfer_.refineDistanceToBoundary(value));
      else if (source == PartDesignParameterSource::Panel)
        chamfer_.setDistanceFromPanel(value);
      else
        chamfer_.setDistanceFromManipulator(value);
      break;
    case PartDesignToolKind::Shell:
      if (source == PartDesignParameterSource::Boundary)
        static_cast<void>(shell_.refineThicknessToBoundary(value));
      else if (source == PartDesignParameterSource::Panel)
        shell_.setThicknessFromPanel(value);
      else
        shell_.setThicknessFromManipulator(value);
      break;
    case PartDesignToolKind::Draft:
      if (source == PartDesignParameterSource::Boundary) {
        inputHandled = false;
        break;
      }
      source == PartDesignParameterSource::Panel
          ? draft_.setAngleFromPanel(document, value)
          : draft_.setAngleFromManipulator(document, value);
      break;
    case PartDesignToolKind::Extrude:
      if (source == PartDesignParameterSource::Boundary) {
        inputHandled = false;
        break;
      }
      source == PartDesignParameterSource::Panel
          ? extrude_.setLengthFromPanel(value)
          : extrude_.setLengthFromManipulator(value);
      break;
    case PartDesignToolKind::Revolve:
      if (source == PartDesignParameterSource::Boundary) {
        inputHandled = false;
        break;
      }
      source == PartDesignParameterSource::Panel
          ? revolve_.setAngleFromPanel(document, value)
          : revolve_.setAngleFromManipulator(document, value);
      break;
    default:
      inputHandled = false;
      break;
  }
  PartDesignUiEffect effect;
  effect.transition = {PartDesignTransitionEffect::None, kind, kind,
                       revisionToken()};
  effect.state = snapshot(kind, document);
  effect.target = targetFor(kind);
  effect.refreshPreview = inputHandled;
  return effect;
}

void PartDesignCoordinator::beginReselection(ToolSelectionStage stage) {
  if (commitPending()) return;
  const bool before = controller_.isReselecting();
  controller_.beginReselection(stage);
  if (!before && controller_.isReselecting()) advanceRevision();
}

void PartDesignCoordinator::finishReselection() noexcept {
  if (commitPending()) return;
  const bool before = controller_.isReselecting();
  controller_.finishReselection();
  if (before) advanceRevision();
}

PartDesignToolKind PartDesignCoordinator::activeTool() const noexcept {
  return controller_.activeTool();
}
ToolSelectionStage PartDesignCoordinator::selectionStage() const noexcept {
  return controller_.selectionStage();
}
bool PartDesignCoordinator::isReselecting() const noexcept {
  return controller_.isReselecting();
}
bool PartDesignCoordinator::commitPending() const noexcept {
  return committingTool_ != PartDesignToolKind::None;
}
std::size_t PartDesignCoordinator::sessionCount() const noexcept {
  return controller_.registrationCount();
}
PartDesignRevisionToken PartDesignCoordinator::revisionToken() const noexcept {
  return {revision_, activeTool()};
}
bool PartDesignCoordinator::isCurrent(
    PartDesignRevisionToken token) const noexcept {
  return token == revisionToken();
}
bool PartDesignCoordinator::invariantHolds() const noexcept {
  const auto active = activeTool();
  if ((active == PartDesignToolKind::None) !=
      (controller_.activeSession() == nullptr)) return false;
  if (commitPending() &&
      (committingTool_ != active || committingToken_ != revisionToken()))
    return false;
  std::size_t nonInactiveCount = 0;
  for (const auto kind : kSessionKinds) {
    const bool nonInactive =
        session(kind)->lifecycle() != ToolLifecycle::Inactive;
    if (nonInactive) ++nonInactiveCount;
    if (kind == active) {
      if (!nonInactive) return false;
    } else if (nonInactive) {
      return false;
    }
  }
  if (active == PartDesignToolKind::None && nonInactiveCount != 0) return false;
  if (active != PartDesignToolKind::None && nonInactiveCount != 1) return false;
  return true;
}

PartDesignToolSnapshot PartDesignCoordinator::snapshot(
    PartDesignToolKind kind) const {
  PartDesignToolSnapshot result;
  result.kind = kind;
  const auto* common = session(kind);
  if (!common) return result;
  result.lifecycle = common->lifecycle();
  result.selectionStage = common->selectionStage();
  result.editingFeatureId = common->editingFeatureId();
  result.previewShape = common->previewShape();
  result.failure = common->failure();
  result.error = common->error();
  result.errorCode = common->errorCode();
  switch (kind) {
    case PartDesignToolKind::Fillet:
      result.bodyId = fillet_.bodyId(); result.sourceFeatureId = fillet_.sourceFeatureId();
      result.edges = fillet_.edges(); result.radiusMm = fillet_.radiusMm();
      result.maximumValidValueMm = fillet_.maximumValidRadiusMm();
      result.limitReached = fillet_.limitReached(); result.linearManipulator = fillet_.manipulator(); break;
    case PartDesignToolKind::Chamfer:
      result.bodyId = chamfer_.bodyId(); result.sourceFeatureId = chamfer_.sourceFeatureId();
      result.edges = chamfer_.edges(); result.distanceMm = chamfer_.distanceMm();
      result.maximumValidValueMm = chamfer_.maximumValidDistanceMm();
      result.limitReached = chamfer_.limitReached(); result.linearManipulator = chamfer_.manipulator(); break;
    case PartDesignToolKind::JoinBodies:
      result.bodies = joinBodies_.bodies(); break;
    case PartDesignToolKind::Shell:
      result.bodyId = shell_.bodyId(); result.sourceFeatureId = shell_.sourceFeatureId();
      result.faces = shell_.removedFaces(); result.thicknessMm = shell_.thicknessMm();
      result.outside = shell_.outside(); result.maximumValidValueMm = shell_.maximumValidThicknessMm();
      result.limitReached = shell_.limitReached(); result.linearManipulator = shell_.manipulator(); break;
    case PartDesignToolKind::Draft:
      result.bodyId = draft_.bodyId(); result.sourceFeatureId = draft_.sourceFeatureId();
      result.faces = draft_.faces(); result.neutralPlane = draft_.neutralPlane();
      result.pullDirection = draft_.pullDirection(); result.rotationEdge = draft_.rotationEdge();
      result.angleDeg = draft_.angleDeg(); result.principalAxisIndex = draft_.principalAxisIndex(); break;
    case PartDesignToolKind::Extrude:
      result.bodyId = extrude_.bodyId(); result.sourceFeatureId = extrude_.sourceFeatureId();
      result.face = extrude_.face(); result.sketchSource = extrude_.isSketchSource();
      result.profileSketchId = extrude_.profileSketchId(); result.profileOverride = extrude_.profileOverride();
      result.lengthMm = extrude_.lengthMm(); result.extrudeOperation = extrude_.operation();
      result.reversed = extrude_.reversed(); result.parameters = extrude_.parameters();
      result.linearManipulator = extrude_.manipulator();
      result.subtractivePreviewShape = extrude_.subtractivePreviewShape(); break;
    case PartDesignToolKind::Revolve:
      result.bodyId = revolve_.bodyId(); result.sourceFeatureId = revolve_.sourceFeatureId();
      result.profileSketchId = revolve_.profileSketchId(); result.profileOverride = revolve_.profileOverride();
      result.axisReference = revolve_.axis(); result.angleDeg = revolve_.angleDeg();
      result.extrudeOperation = revolve_.operation(); result.reversed = revolve_.reversed(); break;
    case PartDesignToolKind::Mirror:
      result.bodyId = mirror_.bodyId(); result.sourceFeatureId = mirror_.sourceFeatureId();
      result.mirrorPlane = mirror_.plane(); break;
    case PartDesignToolKind::Move:
      result.bodyId = move_.bodyId(); result.sourceFeatureId = move_.sourceFeatureId();
      result.offsetMm = move_.offsetMm(); result.translationManipulator = move_.manipulator(); break;
    case PartDesignToolKind::LinearPattern:
      result.bodyId = linearPattern_.bodyId(); result.sourceFeatureId = linearPattern_.sourceFeatureId();
      result.principalDirection = linearPattern_.direction(); result.spacingMm = linearPattern_.spacingMm();
      result.count = linearPattern_.count(); result.patternOperation = linearPattern_.operation();
      result.linearManipulator = linearPattern_.manipulator(); break;
    case PartDesignToolKind::CircularPattern:
      result.bodyId = circularPattern_.bodyId(); result.sourceFeatureId = circularPattern_.sourceFeatureId();
      result.principalAxis = circularPattern_.axis(); result.angleDeg = circularPattern_.angleDeg();
      result.count = circularPattern_.count(); result.patternOperation = circularPattern_.operation();
      result.angularManipulator = circularPattern_.manipulator(); break;
    case PartDesignToolKind::None:
    case PartDesignToolKind::Pocket: break;
  }
  return result;
}

PartDesignToolSnapshot PartDesignCoordinator::snapshot(
    PartDesignToolKind kind, const Document& document) const {
  auto result = snapshot(kind);
  if (kind == PartDesignToolKind::Draft)
    result.angularManipulator = draft_.manipulator(document);
  else if (kind == PartDesignToolKind::Revolve)
    result.angularManipulator = revolve_.manipulator(document);
  return result;
}

#define REQUIRE_ACTIVE(kind) \
  if (activeTool() != PartDesignToolKind::kind || commitPending()) return
#define REQUIRE_ACTIVE_RESULT(kind, value) \
  if (activeTool() != PartDesignToolKind::kind || commitPending()) return value

PartDesignTransitionOutcome PartDesignCoordinator::rejectBegin(
    PartDesignToolKind, OperationFailure failure) noexcept {
  const auto previous = activeTool();
  if (commitPending())
    return {PartDesignTransitionEffect::Rejected, previous, previous,
            revisionToken(), std::move(failure)};
  controller_.cancelActive();
  for (const auto candidate : kSessionKinds)
    if (candidate != previous) session(candidate)->cancel();
  advanceRevision();
  return {PartDesignTransitionEffect::Rejected, previous,
          PartDesignToolKind::None, revisionToken(), std::move(failure)};
}

template <typename Prepare>
PartDesignTransitionOutcome PartDesignCoordinator::beginAtomic(
    PartDesignToolKind kind, Prepare&& prepare) {
  const auto previous = activeTool();
  if (commitPending())
    return {PartDesignTransitionEffect::Rejected, previous, previous,
            revisionToken()};
  controller_.cancelActive();
  for (const auto candidate : kSessionKinds)
    if (candidate != previous) session(candidate)->cancel();
  try {
    prepare();
  } catch (...) {
    session(kind)->cancel();
  }
  if (session(kind)->lifecycle() == ToolLifecycle::Inactive) {
    OperationFailure failure = session(kind)->failure();
    if (failure.code == OperationFailureCode::None)
      failure = {OperationFailureCode::Unknown,
                 "Tool session did not enter an active lifecycle"};
    for (const auto candidate : kSessionKinds)
      if (candidate != kind) session(candidate)->cancel();
    advanceRevision();
    return {PartDesignTransitionEffect::Rejected, previous,
            PartDesignToolKind::None, revisionToken(), std::move(failure)};
  }
  controller_.activate(kind);
  advanceRevision();
  return {PartDesignTransitionEffect::Activated, previous, kind,
          revisionToken()};
}

PartDesignTransitionOutcome PartDesignCoordinator::beginFillet(BodyId a, FeatureId b, ShapeFeature::ShapePtr c, std::vector<EdgeReference> d, double e, std::optional<FeatureId> f, std::shared_ptr<const TopologyIndex> g) {
  if (a == kInvalidBodyId || b == kInvalidFeatureId || !c)
    return rejectBegin(PartDesignToolKind::Fillet);
  return beginAtomic(PartDesignToolKind::Fillet, [&] { fillet_.begin(a,b,std::move(c),std::move(d),e,f,std::move(g)); });
}
void PartDesignCoordinator::setFilletEdges(std::vector<EdgeReference> v) { REQUIRE_ACTIVE(Fillet); fillet_.setEdges(std::move(v)); }
void PartDesignCoordinator::setFilletRadiusFromPanel(double v) { REQUIRE_ACTIVE(Fillet); fillet_.setRadiusFromPanel(v); }
void PartDesignCoordinator::setFilletRadiusFromManipulator(double v) { REQUIRE_ACTIVE(Fillet); fillet_.setRadiusFromManipulator(v); }
bool PartDesignCoordinator::refineFilletRadiusToBoundary(double v) { REQUIRE_ACTIVE_RESULT(Fillet, false); return fillet_.refineRadiusToBoundary(v); }
PartDesignTransitionOutcome PartDesignCoordinator::beginChamfer(BodyId a, FeatureId b, ShapeFeature::ShapePtr c, std::vector<EdgeReference> d, double e, std::optional<FeatureId> f, std::shared_ptr<const TopologyIndex> g) {
  if (a == kInvalidBodyId || b == kInvalidFeatureId || !c)
    return rejectBegin(PartDesignToolKind::Chamfer);
  return beginAtomic(PartDesignToolKind::Chamfer, [&] { chamfer_.begin(a,b,std::move(c),std::move(d),e,f,std::move(g)); });
}
void PartDesignCoordinator::setChamferEdges(std::vector<EdgeReference> v) { REQUIRE_ACTIVE(Chamfer); chamfer_.setEdges(std::move(v)); }
void PartDesignCoordinator::setChamferDistanceFromPanel(double v) { REQUIRE_ACTIVE(Chamfer); chamfer_.setDistanceFromPanel(v); }
void PartDesignCoordinator::setChamferDistanceFromManipulator(double v) { REQUIRE_ACTIVE(Chamfer); chamfer_.setDistanceFromManipulator(v); }
bool PartDesignCoordinator::refineChamferDistanceToBoundary(double v) { REQUIRE_ACTIVE_RESULT(Chamfer, false); return chamfer_.refineDistanceToBoundary(v); }
PartDesignTransitionOutcome PartDesignCoordinator::beginJoinBodies(std::optional<FeatureId> a, BodyId b) { return beginAtomic(PartDesignToolKind::JoinBodies, [&] { joinBodies_.begin(a,b); }); }
void PartDesignCoordinator::setJoinBodies(std::vector<JoinBodyInput> v) { REQUIRE_ACTIVE(JoinBodies); joinBodies_.setBodies(std::move(v)); }
PartDesignTransitionOutcome PartDesignCoordinator::beginShell(BodyId a, FeatureId b, ShapeFeature::ShapePtr c, std::vector<FaceReference> d, double e, bool f, std::optional<FeatureId> g, std::shared_ptr<const TopologyIndex> h) {
  if (a == kInvalidBodyId || b == kInvalidFeatureId || !c)
    return rejectBegin(PartDesignToolKind::Shell);
  return beginAtomic(PartDesignToolKind::Shell, [&] { shell_.begin(a,b,std::move(c),std::move(d),e,f,g,std::move(h)); });
}
void PartDesignCoordinator::setShellRemovedFaces(std::vector<FaceReference> v) { REQUIRE_ACTIVE(Shell); shell_.setRemovedFaces(std::move(v)); }
void PartDesignCoordinator::setShellThicknessFromPanel(double v) { REQUIRE_ACTIVE(Shell); shell_.setThicknessFromPanel(v); }
void PartDesignCoordinator::setShellThicknessFromManipulator(double v) { REQUIRE_ACTIVE(Shell); shell_.setThicknessFromManipulator(v); }
bool PartDesignCoordinator::refineShellThicknessToBoundary(double v) { REQUIRE_ACTIVE_RESULT(Shell, false); return shell_.refineThicknessToBoundary(v); }
void PartDesignCoordinator::setShellOutside(bool v) { REQUIRE_ACTIVE(Shell); shell_.setOutside(v); }
PartDesignTransitionOutcome PartDesignCoordinator::beginDraft(const Document& d, BodyId a, FeatureId b, ShapeFeature::ShapePtr c, std::vector<FaceReference> e, std::optional<PlaneReference> f, std::optional<AxisReference> g, double h, bool i, std::optional<FeatureId> j, std::optional<EdgeReference> k, std::shared_ptr<const TopologyIndex> l) {
  if (a == kInvalidBodyId || b == kInvalidFeatureId || !c)
    return rejectBegin(PartDesignToolKind::Draft);
  return beginAtomic(PartDesignToolKind::Draft, [&] { draft_.begin(d,a,b,std::move(c),std::move(e),std::move(f),std::move(g),h,i,j,std::move(k),std::move(l)); });
}
void PartDesignCoordinator::setDraftFaces(const Document& d, std::vector<FaceReference> v) { REQUIRE_ACTIVE(Draft); draft_.setFaces(d,std::move(v)); }
bool PartDesignCoordinator::setDraftPrincipalAxis(const Document& d, int v) { REQUIRE_ACTIVE_RESULT(Draft, false); return draft_.setPrincipalAxis(d,v); }
bool PartDesignCoordinator::setDraftRotationEdge(const Document& d, EdgeReference v) { REQUIRE_ACTIVE_RESULT(Draft, false); return draft_.setRotationEdge(d,std::move(v)); }
void PartDesignCoordinator::clearDraftPrincipalAxis(const Document& d) { REQUIRE_ACTIVE(Draft); draft_.clearPrincipalAxis(d); }
void PartDesignCoordinator::setDraftAngleFromPanel(const Document& d, double v) { REQUIRE_ACTIVE(Draft); draft_.setAngleFromPanel(d,v); }
void PartDesignCoordinator::setDraftAngleFromManipulator(const Document& d, double v) { REQUIRE_ACTIVE(Draft); draft_.setAngleFromManipulator(d,v); }
PartDesignTransitionOutcome PartDesignCoordinator::beginExtrude(BodyId a, FeatureId b, ShapeFeature::ShapePtr c, FaceReference d, double e, ExtrudeOperation f, bool g, std::optional<FeatureId> h, std::shared_ptr<const TopologyIndex> i) {
  if (a == kInvalidBodyId || b == kInvalidFeatureId || !c)
    return rejectBegin(PartDesignToolKind::Extrude);
  return beginAtomic(PartDesignToolKind::Extrude, [&] { extrude_.begin(a,b,std::move(c),std::move(d),e,f,g,h,std::move(i)); });
}
PartDesignTransitionOutcome PartDesignCoordinator::beginSketchExtrude(DocumentSketch a, SketchId b, ShapeFeature::ShapePtr c, double d, ExtrudeOperation e, bool f, std::optional<FeatureId> g, std::optional<sketch::Sketch> h) {
  if (b == kInvalidSketchId)
    return rejectBegin(PartDesignToolKind::Extrude);
  return beginAtomic(PartDesignToolKind::Extrude, [&] { extrude_.beginSketch(std::move(a),b,std::move(c),d,e,f,g,std::move(h)); });
}
void PartDesignCoordinator::setExtrudeFace(FaceReference v) { REQUIRE_ACTIVE(Extrude); extrude_.setFace(std::move(v)); }
void PartDesignCoordinator::setExtrudeLengthFromPanel(double v) { REQUIRE_ACTIVE(Extrude); extrude_.setLengthFromPanel(v); }
void PartDesignCoordinator::setExtrudeLengthFromManipulator(double v) { REQUIRE_ACTIVE(Extrude); extrude_.setLengthFromManipulator(v); }
void PartDesignCoordinator::setExtrudeOperation(ExtrudeOperation v) { REQUIRE_ACTIVE(Extrude); extrude_.setOperation(v); }
void PartDesignCoordinator::setExtrudeOperationFollowsDirection(bool v) { REQUIRE_ACTIVE(Extrude); extrude_.setOperationFollowsDirection(v); }
PartDesignTransitionOutcome PartDesignCoordinator::beginRevolve(const Document& d, BodyId a, FeatureId b, ShapeFeature::ShapePtr c, std::optional<FeatureId> e) { return beginAtomic(PartDesignToolKind::Revolve, [&] { revolve_.begin(d,a,b,std::move(c),e); }); }
void PartDesignCoordinator::setRevolveProfile(const Document& d, SketchId a, std::optional<sketch::Sketch> b) { REQUIRE_ACTIVE(Revolve); revolve_.setProfile(d,a,std::move(b)); }
void PartDesignCoordinator::clearRevolveProfile(const Document& d) { REQUIRE_ACTIVE(Revolve); revolve_.clearProfile(d); }
void PartDesignCoordinator::setRevolveAxis(const Document& d, AxisReference v) { REQUIRE_ACTIVE(Revolve); revolve_.setAxis(d,std::move(v)); }
void PartDesignCoordinator::clearRevolveAxis(const Document& d) { REQUIRE_ACTIVE(Revolve); revolve_.clearAxis(d); }
void PartDesignCoordinator::setRevolveAngleFromPanel(const Document& d, double v) { REQUIRE_ACTIVE(Revolve); revolve_.setAngleFromPanel(d,v); }
void PartDesignCoordinator::setRevolveAngleFromManipulator(const Document& d, double v) { REQUIRE_ACTIVE(Revolve); revolve_.setAngleFromManipulator(d,v); }
void PartDesignCoordinator::setRevolveOperation(const Document& d, ExtrudeOperation v) { REQUIRE_ACTIVE(Revolve); revolve_.setOperation(d,v); }
void PartDesignCoordinator::setRevolveReversed(const Document& d, bool v) { REQUIRE_ACTIVE(Revolve); revolve_.setReversed(d,v); }
PartDesignTransitionOutcome PartDesignCoordinator::beginMirror(std::optional<FeatureId> v) { return beginAtomic(PartDesignToolKind::Mirror, [&] { mirror_.begin(v); }); }
void PartDesignCoordinator::setMirrorBody(BodyId a, FeatureId b, ShapeFeature::ShapePtr c) { REQUIRE_ACTIVE(Mirror); mirror_.setBody(a,b,std::move(c)); }
void PartDesignCoordinator::clearMirrorBody() { REQUIRE_ACTIVE(Mirror); mirror_.clearBody(); }
void PartDesignCoordinator::setMirrorPlane(MirrorPlane v) { REQUIRE_ACTIVE(Mirror); mirror_.setPlane(v); }
void PartDesignCoordinator::clearMirrorPlane() { REQUIRE_ACTIVE(Mirror); mirror_.clearPlane(); }
PartDesignTransitionOutcome PartDesignCoordinator::beginMove(Vector3d a, std::optional<FeatureId> b) { return beginAtomic(PartDesignToolKind::Move, [&] { move_.begin(a,b); }); }
void PartDesignCoordinator::setMoveBody(BodyId a, FeatureId b, ShapeFeature::ShapePtr c) { REQUIRE_ACTIVE(Move); move_.setBody(a,b,std::move(c)); }
void PartDesignCoordinator::clearMoveBody() { REQUIRE_ACTIVE(Move); move_.clearBody(); }
void PartDesignCoordinator::setMoveOffset(Vector3d v) { REQUIRE_ACTIVE(Move); move_.setOffsetMm(v); }
void PartDesignCoordinator::setMoveOffsetComponent(int a, double b) { REQUIRE_ACTIVE(Move); move_.setOffsetComponent(a,b); }
PartDesignTransitionOutcome PartDesignCoordinator::beginLinearPattern(double a, int b, PatternOperation c, std::optional<FeatureId> d) { return beginAtomic(PartDesignToolKind::LinearPattern, [&] { linearPattern_.begin(a,b,c,d); }); }
void PartDesignCoordinator::setLinearPatternBody(BodyId a, FeatureId b, ShapeFeature::ShapePtr c) { REQUIRE_ACTIVE(LinearPattern); linearPattern_.setBody(a,b,std::move(c)); }
void PartDesignCoordinator::clearLinearPatternBody() { REQUIRE_ACTIVE(LinearPattern); linearPattern_.clearBody(); }
void PartDesignCoordinator::setLinearPatternDirection(PrincipalAxis v) { REQUIRE_ACTIVE(LinearPattern); linearPattern_.setDirection(v); }
void PartDesignCoordinator::clearLinearPatternDirection() { REQUIRE_ACTIVE(LinearPattern); linearPattern_.clearDirection(); }
void PartDesignCoordinator::setLinearPatternSpacing(double v) { REQUIRE_ACTIVE(LinearPattern); linearPattern_.setSpacingMm(v); }
void PartDesignCoordinator::setLinearPatternCount(int v) { REQUIRE_ACTIVE(LinearPattern); linearPattern_.setCount(v); }
void PartDesignCoordinator::setLinearPatternOperation(PatternOperation v) { REQUIRE_ACTIVE(LinearPattern); linearPattern_.setOperation(v); }
PartDesignTransitionOutcome PartDesignCoordinator::beginCircularPattern(double a, int b, PatternOperation c, std::optional<FeatureId> d) { return beginAtomic(PartDesignToolKind::CircularPattern, [&] { circularPattern_.begin(a,b,c,d); }); }
void PartDesignCoordinator::setCircularPatternBody(BodyId a, FeatureId b, ShapeFeature::ShapePtr c) { REQUIRE_ACTIVE(CircularPattern); circularPattern_.setBody(a,b,std::move(c)); }
void PartDesignCoordinator::clearCircularPatternBody() { REQUIRE_ACTIVE(CircularPattern); circularPattern_.clearBody(); }
void PartDesignCoordinator::setCircularPatternAxis(PrincipalAxis v) { REQUIRE_ACTIVE(CircularPattern); circularPattern_.setAxis(v); }
void PartDesignCoordinator::clearCircularPatternAxis() { REQUIRE_ACTIVE(CircularPattern); circularPattern_.clearAxis(); }
void PartDesignCoordinator::setCircularPatternAngle(double v) { REQUIRE_ACTIVE(CircularPattern); circularPattern_.setAngleDeg(v); }
void PartDesignCoordinator::setCircularPatternCount(int v) { REQUIRE_ACTIVE(CircularPattern); circularPattern_.setCount(v); }
void PartDesignCoordinator::setCircularPatternOperation(PatternOperation v) { REQUIRE_ACTIVE(CircularPattern); circularPattern_.setOperation(v); }

#undef REQUIRE_ACTIVE
#undef REQUIRE_ACTIVE_RESULT

ToolSession* PartDesignCoordinator::session(PartDesignToolKind kind) noexcept {
  switch (kind) {
    case PartDesignToolKind::Extrude: return &extrude_;
    case PartDesignToolKind::Revolve: return &revolve_;
    case PartDesignToolKind::Fillet: return &fillet_;
    case PartDesignToolKind::Chamfer: return &chamfer_;
    case PartDesignToolKind::JoinBodies: return &joinBodies_;
    case PartDesignToolKind::Move: return &move_;
    case PartDesignToolKind::Mirror: return &mirror_;
    case PartDesignToolKind::LinearPattern: return &linearPattern_;
    case PartDesignToolKind::CircularPattern: return &circularPattern_;
    case PartDesignToolKind::Shell: return &shell_;
    case PartDesignToolKind::Draft: return &draft_;
    case PartDesignToolKind::None:
    case PartDesignToolKind::Pocket: return nullptr;
  }
  return nullptr;
}
const ToolSession* PartDesignCoordinator::session(
    PartDesignToolKind kind) const noexcept {
  switch (kind) {
    case PartDesignToolKind::Extrude: return &extrude_;
    case PartDesignToolKind::Revolve: return &revolve_;
    case PartDesignToolKind::Fillet: return &fillet_;
    case PartDesignToolKind::Chamfer: return &chamfer_;
    case PartDesignToolKind::JoinBodies: return &joinBodies_;
    case PartDesignToolKind::Move: return &move_;
    case PartDesignToolKind::Mirror: return &mirror_;
    case PartDesignToolKind::LinearPattern: return &linearPattern_;
    case PartDesignToolKind::CircularPattern: return &circularPattern_;
    case PartDesignToolKind::Shell: return &shell_;
    case PartDesignToolKind::Draft: return &draft_;
    case PartDesignToolKind::None:
    case PartDesignToolKind::Pocket: return nullptr;
  }
  return nullptr;
}
void PartDesignCoordinator::advanceRevision() noexcept { ++revision_; }

}  // namespace solidar
