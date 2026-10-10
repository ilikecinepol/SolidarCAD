#include "model/Document.h"
#include "model/GeometryOperation.h"
#include "model/IdGeneration.h"
#include "model/TopologyReferenceResolver.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <sstream>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace solidar {
namespace {
std::atomic<SketchId> g_nextSketchId{1};

template <typename Id>
void advancePast(std::atomic<Id>& next, Id used) noexcept {
  Id expected = next.load(std::memory_order_relaxed);
  while (expected <= used &&
         !next.compare_exchange_weak(expected, used + 1,
                                     std::memory_order_relaxed)) {
  }
}

enum class DependencyNodeKind { Feature, Sketch };

struct DependencyNode {
  DependencyNodeKind kind{DependencyNodeKind::Feature};
  std::uint64_t id{};

  friend bool operator==(const DependencyNode&, const DependencyNode&) = default;
  friend bool operator<(const DependencyNode& left,
                        const DependencyNode& right) noexcept {
    if (left.kind != right.kind)
      return static_cast<int>(left.kind) < static_cast<int>(right.kind);
    return left.id < right.id;
  }
};

struct DependencyNodeHash {
  std::size_t operator()(DependencyNode node) const noexcept {
    const auto kind = static_cast<std::uint64_t>(node.kind);
    return std::hash<std::uint64_t>{}(node.id ^ (kind << 63));
  }
};

DependencyNode featureNode(FeatureId id) noexcept {
  return {DependencyNodeKind::Feature, id};
}

DependencyNode sketchNode(SketchId id) noexcept {
  return {DependencyNodeKind::Sketch, id};
}

struct FeatureLocation {
  BodyId bodyId{kInvalidBodyId};
  std::size_t bodyIndex{};
  std::size_t featureIndex{};
  const ShapeFeature* feature{};
};

struct DocumentIndex {
  std::unordered_map<BodyId, std::size_t> bodies;
  std::unordered_map<FeatureId, FeatureLocation> features;
  std::unordered_map<SketchId, std::size_t> sketches;
  std::vector<DependencyNode> nodes;
  std::unordered_map<DependencyNode, std::size_t, DependencyNodeHash>
      nodeIndices;
  std::vector<std::vector<std::size_t>> dependents;
  std::vector<std::vector<std::size_t>> dependencies;
  std::vector<std::size_t> topologicalOrder;
  std::string error;
  FeatureId errorFeatureId{kInvalidFeatureId};

  [[nodiscard]] std::optional<std::size_t> nodeIndex(
      DependencyNode node) const noexcept {
    const auto found = nodeIndices.find(node);
    return found == nodeIndices.end()
               ? std::optional<std::size_t>{}
               : std::optional<std::size_t>{found->second};
  }
};

void finalizeTopologicalOrder(DocumentIndex* index);

DocumentIndex buildDocumentIndex(const Document& document) {
  DocumentIndex index;
  const auto& bodies = document.bodies();
  std::vector<std::pair<DependencyNode, DependencyNode>> edges;
  for (std::size_t bodyIndex = 0; bodyIndex < bodies.size(); ++bodyIndex) {
    const Body& body = bodies[bodyIndex];
    if (body.id() == kInvalidBodyId) {
      index.error = "Dependency graph contains an invalid BodyId";
      return index;
    }
    if (!index.bodies.emplace(body.id(), bodyIndex).second) {
      index.error = "Dependency graph contains duplicate Body #" +
                    std::to_string(body.id());
      return index;
    }
    for (std::size_t featureIndex = 0;
         featureIndex < body.features().size(); ++featureIndex) {
      const auto& feature = body.features()[featureIndex];
      if (!feature || feature->id() == kInvalidFeatureId) {
        index.error = "Dependency graph contains an invalid Feature";
        return index;
      }
      if (!index.features
               .emplace(feature->id(),
                        FeatureLocation{body.id(), bodyIndex, featureIndex,
                                        feature.get()})
               .second) {
        index.error = "Dependency graph contains duplicate Feature #" +
                      std::to_string(feature->id());
        return index;
      }
      index.nodes.push_back(featureNode(feature->id()));
    }
  }

  const auto& sketches = document.sketches();
  for (std::size_t sketchIndex = 0; sketchIndex < sketches.size();
       ++sketchIndex) {
    const auto& sketch = sketches[sketchIndex];
    if (sketch.id == kInvalidSketchId ||
        !index.sketches.emplace(sketch.id, sketchIndex).second) {
      index.error = "Dependency graph contains an invalid or duplicate Sketch #" +
                    std::to_string(sketch.id);
      return index;
    }
    index.nodes.push_back(sketchNode(sketch.id));
  }

  std::sort(index.nodes.begin(), index.nodes.end());
  index.dependents.resize(index.nodes.size());
  index.dependencies.resize(index.nodes.size());
  for (std::size_t nodeIndex = 0; nodeIndex < index.nodes.size(); ++nodeIndex)
    index.nodeIndices.emplace(index.nodes[nodeIndex], nodeIndex);

  // Local history is an explicit dependency chain even when a Feature also
  // stores topology references to its immediate predecessor.
  for (const Body& body : bodies)
    for (std::size_t historyIndex = 1;
         historyIndex < body.features().size(); ++historyIndex)
      edges.emplace_back(
          featureNode(body.features()[historyIndex - 1]->id()),
          featureNode(body.features()[historyIndex]->id()));

  // Every Feature enumerates only the IDs it actually consumes. This keeps
  // graph construction linear in the declared dependency count.
  for (const auto& node : index.nodes) {
    if (node.kind != DependencyNodeKind::Feature) continue;
    const auto location = index.features.find(static_cast<FeatureId>(node.id));
    const FeatureDependencies declared = location->second.feature->dependencies();
    for (const FeatureId upstreamId : declared.featureIds) {
      if (upstreamId == kInvalidFeatureId ||
          !index.features.contains(upstreamId)) {
        index.error = "Feature #" + std::to_string(node.id) +
                      " references missing Feature #" +
                      std::to_string(upstreamId);
        index.errorFeatureId = static_cast<FeatureId>(node.id);
        return index;
      }
      edges.emplace_back(featureNode(upstreamId), node);
    }
    for (const SketchId sketchId : declared.sketchIds) {
      if (sketchId == kInvalidSketchId || !index.sketches.contains(sketchId)) {
        index.error = "Feature #" + std::to_string(node.id) +
                      " references missing Sketch #" +
                      std::to_string(sketchId);
        index.errorFeatureId = static_cast<FeatureId>(node.id);
        return index;
      }
      edges.emplace_back(sketchNode(sketchId), node);
    }
  }

  // A face-supported Sketch is a first-class graph node. This preserves the
  // full Feature -> Sketch -> Feature path for cycle diagnostics, dirty
  // propagation, and removal closure.
  for (const auto& sketch : sketches) {
    if (sketch.support.type != SketchSupportType::Face) continue;
    const auto owner = index.features.find(sketch.support.face.featureId);
    if (owner == index.features.end()) {
      index.error = "Sketch #" + std::to_string(sketch.id) +
                    " references missing support Feature #" +
                    std::to_string(sketch.support.face.featureId);
      return index;
    }
    if (owner->second.bodyId != sketch.support.face.bodyId) {
      index.error = "Sketch #" + std::to_string(sketch.id) +
                    " support owner Body does not match Feature #" +
                    std::to_string(sketch.support.face.featureId);
      return index;
    }
    edges.emplace_back(featureNode(owner->first), sketchNode(sketch.id));
  }

  const auto edgeLess = [](const auto& left, const auto& right) {
    if (left.first < right.first) return true;
    if (right.first < left.first) return false;
    return left.second < right.second;
  };
  std::sort(edges.begin(), edges.end(), edgeLess);
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  for (const auto& [upstream, dependent] : edges) {
    const std::size_t upstreamIndex = index.nodeIndices.at(upstream);
    const std::size_t dependentIndex = index.nodeIndices.at(dependent);
    index.dependents[upstreamIndex].push_back(dependentIndex);
    index.dependencies[dependentIndex].push_back(upstreamIndex);
  }
  finalizeTopologicalOrder(&index);
  return index;
}

const std::vector<std::size_t>& outgoing(const DocumentIndex& index,
                                         DependencyNode node) {
  static const std::vector<std::size_t> empty;
  const auto ordinal = index.nodeIndex(node);
  return ordinal ? index.dependents[*ordinal] : empty;
}

const std::vector<std::size_t>& incoming(const DocumentIndex& index,
                                         DependencyNode node) {
  static const std::vector<std::size_t> empty;
  const auto ordinal = index.nodeIndex(node);
  return ordinal ? index.dependencies[*ordinal] : empty;
}

std::string nodeLabel(const DocumentIndex& index,
                      DependencyNode node) {
  if (node.kind == DependencyNodeKind::Sketch)
    return "Sketch #" + std::to_string(node.id);
  const auto found = index.features.find(static_cast<FeatureId>(node.id));
  if (found == index.features.end())
    return "Feature #" + std::to_string(node.id);
  return "Body #" + std::to_string(found->second.bodyId) + " / Feature #" +
         std::to_string(node.id);
}

bool findCycleFrom(const DocumentIndex& index,
                   const std::vector<bool>& unresolved,
                   std::size_t node, std::vector<int>* colors,
                   std::vector<std::size_t>* stack,
                   std::vector<std::size_t>* cycle) {
  (*colors)[node] = 1;
  stack->push_back(node);
  for (const std::size_t next : index.dependents[node]) {
    if (!unresolved[next]) continue;
    const int color = (*colors)[next];
    if (color == 0) {
      if (findCycleFrom(index, unresolved, next, colors, stack, cycle))
        return true;
    } else if (color == 1) {
      const auto start = std::find(stack->begin(), stack->end(), next);
      cycle->assign(start, stack->end());
      cycle->push_back(next);
      return true;
    }
  }
  stack->pop_back();
  (*colors)[node] = 2;
  return false;
}

void finalizeTopologicalOrder(DocumentIndex* index) {
  if (!index || !index->error.empty()) return;
  std::vector<std::size_t> indegree(index->nodes.size());
  std::deque<std::size_t> ready;
  for (std::size_t node = 0; node < index->nodes.size(); ++node) {
    indegree[node] = index->dependencies[node].size();
    if (indegree[node] == 0) ready.push_back(node);
  }
  while (!ready.empty()) {
    const std::size_t node = ready.front();
    ready.pop_front();
    index->topologicalOrder.push_back(node);
    for (const std::size_t dependent : index->dependents[node]) {
      auto& count = indegree[dependent];
      if (--count == 0) ready.push_back(dependent);
    }
  }
  if (index->topologicalOrder.size() == index->nodes.size()) return;

  std::vector<bool> unresolved(index->nodes.size(), true);
  for (const std::size_t node : index->topologicalOrder)
    unresolved[node] = false;
  std::vector<int> colors(index->nodes.size());
  std::vector<std::size_t> stack;
  std::vector<std::size_t> cycle;
  for (std::size_t node = 0; node < index->nodes.size(); ++node) {
    if (!unresolved[node]) continue;
    if (colors[node] != 0) continue;
    if (findCycleFrom(*index, unresolved, node, &colors, &stack, &cycle)) break;
  }
  std::ostringstream message;
  message << "Dependency cycle detected";
  if (!cycle.empty()) {
    message << ": ";
    for (std::size_t position = 0; position < cycle.size(); ++position) {
      if (position != 0) message << " -> ";
      message << nodeLabel(*index, index->nodes[cycle[position]]);
    }
  }
  index->error = message.str();
}

FeatureRemovalPlan makeRemovalPlan(
    const Document& document, const DocumentIndex& index,
    std::set<DependencyNode> removedNodes,
    std::set<BodyId> removedBodies = {}) {
  FeatureRemovalPlan plan;
  std::deque<DependencyNode> pending(removedNodes.begin(), removedNodes.end());
  for (const BodyId bodyId : removedBodies) {
    const Body* body = document.findBody(bodyId);
    if (!body) continue;
    for (const auto& feature : body->features()) {
      const DependencyNode node = featureNode(feature->id());
      if (removedNodes.insert(node).second) pending.push_back(node);
    }
  }
  while (!pending.empty()) {
    const DependencyNode node = pending.front();
    pending.pop_front();
    for (const std::size_t dependent : outgoing(index, node)) {
      const DependencyNode dependentNode = index.nodes[dependent];
      if (removedNodes.insert(dependentNode).second)
        pending.push_back(dependentNode);
    }
  }

  plan.bodyIds.assign(removedBodies.begin(), removedBodies.end());
  for (const Body& body : document.bodies()) {
    if (removedBodies.contains(body.id())) continue;
    std::optional<std::size_t> first;
    for (std::size_t featureIndex = 0;
         featureIndex < body.features().size(); ++featureIndex) {
      const bool removed =
          removedNodes.contains(featureNode(body.features()[featureIndex]->id()));
      if (removed && !first) first = featureIndex;
      if (!removed && first) {
        plan.diagnostic = "Dependency closure is not a Body history suffix";
        return plan;
      }
    }
    if (!first) continue;
    BodyFeatureRemovalRange range;
    range.bodyId = body.id();
    range.firstFeatureIndex = *first;
    for (std::size_t featureIndex = *first;
         featureIndex < body.features().size(); ++featureIndex)
      range.featureIds.push_back(body.features()[featureIndex]->id());
    plan.bodyRanges.push_back(std::move(range));
  }
  std::sort(plan.bodyRanges.begin(), plan.bodyRanges.end(),
            [](const auto& left, const auto& right) {
              return left.bodyId < right.bodyId;
            });
  for (const auto& range : plan.bodyRanges)
    plan.featureIds.insert(plan.featureIds.end(), range.featureIds.begin(),
                           range.featureIds.end());
  for (const BodyId bodyId : plan.bodyIds) {
    const Body* body = document.findBody(bodyId);
    if (!body) continue;
    for (const auto& feature : body->features())
      plan.featureIds.push_back(feature->id());
  }
  for (const auto& sketch : document.sketches())
    if (removedNodes.contains(sketchNode(sketch.id)))
      plan.sketchIds.push_back(sketch.id);
  std::sort(plan.sketchIds.begin(), plan.sketchIds.end());
  plan.applicable = true;
  return plan;
}

bool equivalentPlans(const FeatureRemovalPlan& left,
                     const FeatureRemovalPlan& right) {
  return left.applicable == right.applicable && left.bodyId == right.bodyId &&
         left.selectedFeatureId == right.selectedFeatureId &&
         left.selectedSketchId == right.selectedSketchId &&
         left.selectedBodyId == right.selectedBodyId &&
         left.bodyRanges == right.bodyRanges && left.bodyIds == right.bodyIds &&
         left.featureIds == right.featureIds && left.sketchIds == right.sketchIds;
}
}  // namespace

Document::Document() = default;

DocumentSketch& Document::addSketch(std::string name) {
  return addSketch(nextSketchId(), std::move(name));
}

DocumentSketch& Document::addSketch(SketchId id, std::string name,
                                    sketch::Sketch geometry) {
  if (id == kInvalidSketchId) id = nextSketchId();
  if (findSketch(id)) throw std::invalid_argument("Duplicate SketchId");
  if (detail::explicitIdReservationEnabled()) advancePast(g_nextSketchId, id);
  if (name.empty()) name = "Sketch " + std::to_string(sketches_.size() + 1);
  sketches_.push_back({id, std::move(name), std::move(geometry)});
  return sketches_.back();
}

const std::vector<DocumentSketch>& Document::sketches() const noexcept {
  return sketches_;
}

DocumentSketch* Document::sketchAt(std::size_t index) noexcept {
  return index < sketches_.size() ? &sketches_[index] : nullptr;
}

DocumentSketch* Document::findSketch(SketchId id) noexcept {
  const auto found = std::find_if(sketches_.begin(), sketches_.end(),
                                  [id](const auto& item) { return item.id == id; });
  return found == sketches_.end() ? nullptr : &*found;
}

const DocumentSketch* Document::findSketch(SketchId id) const noexcept {
  const auto found = std::find_if(sketches_.begin(), sketches_.end(),
                                  [id](const auto& item) { return item.id == id; });
  return found == sketches_.end() ? nullptr : &*found;
}

bool Document::replaceSketchGeometry(SketchId id, sketch::Sketch geometry) {
  auto* target = findSketch(id);
  if (!target) return false;
  target->geometry = std::move(geometry);
  markSketchDirty(id);
  return true;
}

bool Document::markSketchDirty(SketchId id) {
  auto* sketch = findSketch(id);
  if (!sketch) return false;
  bool affected = false;
  for (auto& body : bodies_) {
    const auto& features = body.features();
    for (std::size_t index = 0; index < features.size(); ++index) {
      const auto dependencies = features[index]->dependencies();
      if (std::find(dependencies.sketchIds.begin(),
                    dependencies.sketchIds.end(), id) ==
          dependencies.sketchIds.end())
        continue;
      body.markDirtyFrom(index);
      affected = true;
      break;
    }
  }
  return affected;
}

Body& Document::addBody(std::string name) {
  if (name.empty()) name = "Body " + std::to_string(bodies_.size() + 1);
  bodies_.emplace_back(std::move(name));
  return bodies_.back();
}

Body& Document::addBody(BodyId id, std::string name) {
  if (id != kInvalidBodyId && findBody(id))
    throw std::invalid_argument("Duplicate BodyId");
  if (name.empty()) name = "Body " + std::to_string(bodies_.size() + 1);
  bodies_.emplace_back(id, std::move(name));
  return bodies_.back();
}

const std::vector<Body>& Document::bodies() const noexcept { return bodies_; }
Body* Document::findBody(BodyId id) noexcept {
  const auto found = std::find_if(bodies_.begin(), bodies_.end(),
                                  [id](const auto& body) { return body.id() == id; });
  return found == bodies_.end() ? nullptr : &*found;
}
const Body* Document::findBody(BodyId id) const noexcept {
  const auto found = std::find_if(bodies_.begin(), bodies_.end(),
                                  [id](const auto& body) { return body.id() == id; });
  return found == bodies_.end() ? nullptr : &*found;
}
ShapeFeature* Document::findFeature(FeatureId id) noexcept {
  Body* body = findBodyForFeature(id);
  if (!body) return nullptr;
  const auto index = body->featureIndex(id);
  return index ? body->features_[*index].get() : nullptr;
}
const ShapeFeature* Document::findFeature(FeatureId id) const noexcept {
  const Body* body = findBodyForFeature(id);
  if (!body) return nullptr;
  const auto index = body->featureIndex(id);
  return index ? body->features()[*index].get() : nullptr;
}
Body* Document::findBodyForFeature(FeatureId id) noexcept {
  const auto found = std::find_if(
      bodies_.begin(), bodies_.end(), [id](const Body& body) {
        return body.featureIndex(id).has_value();
      });
  return found == bodies_.end() ? nullptr : &*found;
}
const Body* Document::findBodyForFeature(FeatureId id) const noexcept {
  const auto found = std::find_if(
      bodies_.begin(), bodies_.end(), [id](const Body& body) {
        return body.featureIndex(id).has_value();
      });
  return found == bodies_.end() ? nullptr : &*found;
}
Body* Document::activeBody() noexcept {
  return bodies_.empty() ? nullptr : &bodies_.back();
}
const Body* Document::activeBody() const noexcept {
  return bodies_.empty() ? nullptr : &bodies_.back();
}

bool Document::rebuild() {
  GeometryFailure schedulerFailure;
  const bool rebuilt = runGeometryOperation(
      [&]() -> bool {
        const DocumentIndex index = buildDocumentIndex(*this);
        if (!index.error.empty()) {
          dependencyError_ = index.error;
          if (index.errorFeatureId != kInvalidFeatureId)
            if (auto* feature = findFeature(index.errorFeatureId))
              feature->markBlocked(index.error);
          return false;
        }
        dependencyError_.clear();

        std::vector<bool> dirtyNodes(index.nodes.size());
        std::deque<std::size_t> pending;
        for (const auto& [featureId, location] : index.features)
          if (location.feature->isDirty() || location.feature->isFailed()) {
            const std::size_t node = *index.nodeIndex(featureNode(featureId));
            dirtyNodes[node] = true;
            pending.push_back(node);
          }
        for (const auto& [sketchId, sketchIndex] : index.sketches)
          if (sketches_[sketchIndex].placementDirty) {
            const std::size_t node = *index.nodeIndex(sketchNode(sketchId));
            dirtyNodes[node] = true;
            pending.push_back(node);
          }
        while (!pending.empty()) {
          const std::size_t node = pending.front();
          pending.pop_front();
          for (const std::size_t dependent : index.dependents[node])
            if (!dirtyNodes[dependent]) {
              dirtyNodes[dependent] = true;
              pending.push_back(dependent);
            }
        }

        std::vector<bool> unavailable(index.nodes.size());
        bool valid = true;
        for (const std::size_t nodeOrdinal : index.topologicalOrder) {
          const DependencyNode node = index.nodes[nodeOrdinal];
          if (node.kind == DependencyNodeKind::Sketch) {
            const auto location =
                index.sketches.find(static_cast<SketchId>(node.id));
            const bool upstreamUnavailable = std::any_of(
                index.dependencies[nodeOrdinal].begin(),
                index.dependencies[nodeOrdinal].end(),
                [&unavailable](std::size_t dependency) {
                  return unavailable[dependency];
                });
            if (location == index.sketches.end()) {
              dependencyError_ = "Dependency scheduler lost Sketch #" +
                                 std::to_string(node.id);
              return false;
            }
            DocumentSketch& sketch = sketches_[location->second];
            if (!upstreamUnavailable && dirtyNodes[nodeOrdinal])
              updateSketchPlacement(sketch);
            if (upstreamUnavailable || !sketch.supportResolved)
              unavailable[nodeOrdinal] = true;
            continue;
          }

          const auto location =
              index.features.find(static_cast<FeatureId>(node.id));
          if (location == index.features.end() ||
              location->second.bodyIndex >= bodies_.size()) {
            dependencyError_ =
                "Dependency scheduler lost Feature #" + std::to_string(node.id);
            return false;
          }
          Body& body = bodies_[location->second.bodyIndex];
          if (location->second.featureIndex >= body.features_.size()) {
            dependencyError_ =
                "Dependency scheduler lost Feature #" + std::to_string(node.id);
            return false;
          }
          ShapeFeature& feature =
              *body.features_[location->second.featureIndex];
          const auto blockedBy = std::find_if(
              index.dependencies[nodeOrdinal].begin(),
              index.dependencies[nodeOrdinal].end(),
              [&unavailable](std::size_t dependency) {
                return unavailable[dependency];
              });
          if (blockedBy != index.dependencies[nodeOrdinal].end()) {
            feature.markBlocked("Blocked by invalid upstream dependency '" +
                                nodeLabel(index, index.nodes[*blockedBy]) + "'");
            unavailable[nodeOrdinal] = true;
            valid = false;
            continue;
          }

          if (dirtyNodes[nodeOrdinal]) {
            feature.setDirty();
            const ShapeFeature* previousFeature =
                location->second.featureIndex == 0
                    ? nullptr
                    : body.features_[location->second.featureIndex - 1].get();
            const ShapeFeature::ShapePtr previousShape =
                previousFeature ? previousFeature->shape()
                                : ShapeFeature::ShapePtr{};
            const RebuildContext context{*this, &body, previousShape.get(),
                                         previousFeature};
            if (!feature.rebuild(context)) {
              unavailable[nodeOrdinal] = true;
              valid = false;
              continue;
            }
          }
          if (!feature.isValid()) {
            unavailable[nodeOrdinal] = true;
            valid = false;
          }
        }
        return valid;
      },
      &schedulerFailure);
  if (schedulerFailure.kind != GeometryFailureKind::None) {
    switch (schedulerFailure.kind) {
    case GeometryFailureKind::OcctException:
      dependencyError_ = "Document rebuild failed in an OpenCASCADE operation";
      break;
    case GeometryFailureKind::StandardException:
      dependencyError_ = "Document rebuild failed with a standard exception";
      break;
    default:
      dependencyError_ = "Document rebuild failed with an unknown exception";
      break;
    }
    return false;
  }
  return rebuilt;
}

bool Document::recompute() { return rebuild(); }

std::string Document::rebuildError() const {
  if (!dependencyError_.empty())
    return dependencyError_;
  for (const auto &body : bodies_)
    for (const auto &feature : body.features())
      if (feature->isFailed() && !feature->error().empty())
        return feature->error();
  for (const auto& body : bodies_)
    for (const auto& feature : body.features())
      if (!feature->isValid())
        return "Body '" + body.name() + "', feature '" + feature->name() +
               "' is invalid without a diagnostic message";
  return {};
}

FeatureRemovalPlan Document::planFeatureRemoval(BodyId bodyId,
                                                FeatureId featureId) const {
  FeatureRemovalPlan plan;
  plan.bodyId = bodyId;
  plan.selectedFeatureId = featureId;
  const Body* body = findBody(bodyId);
  if (!body || !body->featureIndex(featureId)) {
    plan.diagnostic = "Feature to remove was not found";
    return plan;
  }
  const DocumentIndex index = buildDocumentIndex(*this);
  if (!index.error.empty()) {
    plan.diagnostic = index.error;
    return plan;
  }
  plan = makeRemovalPlan(*this, index, {featureNode(featureId)});
  plan.bodyId = bodyId;
  plan.selectedFeatureId = featureId;
  return plan;
}

FeatureRemovalPlan Document::planSketchRemoval(SketchId sketchId) const {
  FeatureRemovalPlan plan;
  plan.selectedSketchId = sketchId;
  if (!findSketch(sketchId)) {
    plan.diagnostic = "Sketch to remove was not found";
    return plan;
  }
  const DocumentIndex index = buildDocumentIndex(*this);
  if (!index.error.empty()) {
    plan.diagnostic = index.error;
    return plan;
  }
  plan = makeRemovalPlan(*this, index, {sketchNode(sketchId)});
  plan.selectedSketchId = sketchId;
  if (plan.bodyRanges.size() == 1) plan.bodyId = plan.bodyRanges.front().bodyId;
  return plan;
}

FeatureRemovalPlan Document::planBodyRemoval(BodyId bodyId) const {
  FeatureRemovalPlan plan;
  plan.selectedBodyId = bodyId;
  if (!findBody(bodyId)) {
    plan.diagnostic = "Body to remove was not found";
    return plan;
  }
  const DocumentIndex index = buildDocumentIndex(*this);
  if (!index.error.empty()) {
    plan.diagnostic = index.error;
    return plan;
  }
  plan = makeRemovalPlan(*this, index, {}, {bodyId});
  plan.bodyId = bodyId;
  plan.selectedBodyId = bodyId;
  return plan;
}

bool Document::applyValidatedRemovalPlan(const FeatureRemovalPlan& plan,
                                         std::string* error) {
  std::set<BodyId> removedBodies;
  for (const BodyId bodyId : plan.bodyIds) {
    if (!findBody(bodyId) || !removedBodies.insert(bodyId).second) {
      if (error) *error = "Removal plan contains an invalid Body";
      return false;
    }
  }
  std::set<BodyId> rangedBodies;
  for (const auto& range : plan.bodyRanges) {
    Body* body = findBody(range.bodyId);
    if (!body || removedBodies.contains(range.bodyId) ||
        !rangedBodies.insert(range.bodyId).second || range.featureIds.empty() ||
        range.firstFeatureIndex >= body->features_.size() ||
        range.featureIds.size() !=
            body->features_.size() - range.firstFeatureIndex) {
      if (error) *error = "Removal plan contains an invalid Body range";
      return false;
    }
    for (std::size_t offset = 0; offset < range.featureIds.size(); ++offset)
      if (body->features_[range.firstFeatureIndex + offset]->id() !=
          range.featureIds[offset]) {
        if (error) *error = "Removal plan no longer matches Body history";
        return false;
      }
  }
  std::set<SketchId> removedSketches;
  for (const SketchId sketchId : plan.sketchIds)
    if (!findSketch(sketchId) || !removedSketches.insert(sketchId).second) {
      if (error) *error = "Removal plan contains an invalid Sketch";
      return false;
    }

  for (const auto& range : plan.bodyRanges)
    findBody(range.bodyId)->eraseFeaturesFrom(range.firstFeatureIndex);
  sketches_.erase(std::remove_if(
                      sketches_.begin(), sketches_.end(),
                      [&removedSketches](const DocumentSketch& sketch) {
                        return removedSketches.contains(sketch.id);
                      }),
                  sketches_.end());
  bodies_.erase(std::remove_if(bodies_.begin(), bodies_.end(),
                               [&removedBodies](const Body& body) {
                                 return removedBodies.contains(body.id());
                               }),
                bodies_.end());
  dependencyError_.clear();

  const DocumentIndex remaining = buildDocumentIndex(*this);
  if (!remaining.error.empty()) {
    if (error) *error = remaining.error;
    return false;
  }
  return true;
}

bool Document::applyRemovalPlan(const FeatureRemovalPlan& plan,
                                std::string* error) {
  if (error) error->clear();
  if (!plan.applicable) {
    if (error)
      *error = plan.diagnostic.empty() ? "Removal plan is not applicable"
                                       : plan.diagnostic;
    return false;
  }

  FeatureRemovalPlan current;
  const int selections =
      (plan.selectedFeatureId != kInvalidFeatureId ? 1 : 0) +
      (plan.selectedSketchId != kInvalidSketchId ? 1 : 0) +
      (plan.selectedBodyId != kInvalidBodyId ? 1 : 0);
  if (selections != 1) {
    if (error) *error = "Removal plan has an invalid selection";
    return false;
  }
  if (plan.selectedFeatureId != kInvalidFeatureId)
    current = planFeatureRemoval(plan.bodyId, plan.selectedFeatureId);
  else if (plan.selectedSketchId != kInvalidSketchId)
    current = planSketchRemoval(plan.selectedSketchId);
  else
    current = planBodyRemoval(plan.selectedBodyId);
  if (!current.applicable || !equivalentPlans(plan, current)) {
    if (error) *error = "Removal plan is stale; document history changed";
    return false;
  }

  try {
    Document staged(*this);
    std::string stagedError;
    if (!staged.applyValidatedRemovalPlan(plan, &stagedError)) {
      if (error) *error = std::move(stagedError);
      return false;
    }
    *this = std::move(staged);
    return true;
  } catch (const std::exception& failure) {
    if (error) *error = std::string("Removal could not be staged: ") + failure.what();
    return false;
  } catch (...) {
    if (error) *error = "Removal could not be staged";
    return false;
  }
}

bool Document::removeFeatureCascade(BodyId bodyId, FeatureId featureId,
                                    std::string* error) {
  return applyRemovalPlan(planFeatureRemoval(bodyId, featureId), error);
}

bool Document::removeSketchCascade(SketchId sketchId, std::string* error) {
  return applyRemovalPlan(planSketchRemoval(sketchId), error);
}

bool Document::removeBodyCascade(BodyId bodyId, std::string* error) {
  return applyRemovalPlan(planBodyRemoval(bodyId), error);
}

bool Document::recomputeFrom(FeatureId featureId) {
  if (featureId == kInvalidFeatureId) return false;
  bool found = false;
  for (auto& body : bodies_) {
    const auto& features = body.features();
    for (std::size_t index = 0; index < features.size(); ++index) {
      if (features[index]->id() != featureId) continue;
      body.markDirtyFrom(index);
      found = true;
      break;
    }
  }
  return found && recompute();
}

bool Document::attachSketchToFace(SketchId sketchId, FaceReference reference) {
  auto* sketch = findSketch(sketchId);
  if (!sketch) return false;
  const Body* body = findBody(reference.bodyId);
  if (body && !reference.signature) {
    for (const auto& feature : body->features())
      if (feature->id() == reference.featureId && feature->shape()) {
        std::string indexError;
        const auto topology = feature->topologyIndex(&indexError);
        if (!topology) return false;
        const auto created = topology->createFaceReference(
            reference.bodyId, reference.featureId, reference.faceIndex);
        if (!created) return false;
        reference = created.reference;
        break;
      }
  }
  sketch->support = {SketchSupportType::Face, reference};
  sketch->placementDirty = true;
  markSketchDirty(sketchId);
  updateSketchPlacement(*sketch);
  return sketch->supportResolved;
}

bool Document::updateSketchPlacement(DocumentSketch& sketch) {
  sketch.placementDirty = false;
  ++sketch.placementRevision;
  if (sketch.support.type != SketchSupportType::Face) {
    sketch.supportResolved = true;
    return true;
  }
  auto& reference = sketch.support.face;
  const Body* body = findBody(reference.bodyId);
  const ShapeFeature* feature = nullptr;
  if (body)
    for (const auto& candidate : body->features())
      if (candidate->id() == reference.featureId) {
        feature = candidate.get();
        break;
      }
  if (!feature || !feature->isValid() || !feature->shape()) {
    sketch.supportResolved = false;
    return false;
  }
  std::string indexError;
  const auto topology = feature->topologyIndex(&indexError);
  if (!topology) {
    sketch.supportResolved = false;
    return false;
  }
  const auto resolution = topology->resolveFace(reference.topology());
  if (!resolution) {
    sketch.supportResolved = false;
    return false;
  }
  if (!reference.signature) {
    const auto created = topology->createFaceReference(
        reference.bodyId, reference.featureId, resolution.index);
    if (!created) {
      sketch.supportResolved = false;
      return false;
    }
    reference = created.reference;
  }
  const auto resolved = resolveFacePlacement(*resolution.subshape);
  sketch.supportResolved = resolved.resolved && resolved.planar;
  if (sketch.supportResolved) sketch.placement = resolved.placement;
  return sketch.supportResolved;
}

void Document::updateSketchPlacements() {
  for (auto& sketch : sketches_) {
    sketch.placementDirty = true;
    updateSketchPlacement(sketch);
  }
}

SketchId Document::nextSketchId() noexcept {
  return g_nextSketchId.fetch_add(1, std::memory_order_relaxed);
}

const BoxParameters& Document::box() const noexcept { return box_; }

void Document::setBox(BoxParameters parameters) {
  if (parameters.widthMm <= 0.0 || parameters.depthMm <= 0.0 ||
      parameters.heightMm <= 0.0) {
    throw std::invalid_argument("Solid dimensions must be positive");
  }
  box_ = parameters;
}

void Document::reserveIdsForEditing() const noexcept {
  for (const auto& sketch : sketches_) advancePast(g_nextSketchId, sketch.id);
  for (const auto& body : bodies_) {
    Body::reserveId(body.id());
    for (const auto& feature : body.features())
      if (feature) Feature::reserveId(feature->id());
  }
}

bool Document::applyBodySlice(std::size_t index, std::optional<Body> body) {
  if (body) {
    if (index > bodies_.size()) return false;
    if (index == bodies_.size()) bodies_.push_back(std::move(*body));
    else if (bodies_[index].id() == body->id()) bodies_[index] = std::move(*body);
    else bodies_.insert(bodies_.begin() + static_cast<std::ptrdiff_t>(index),
                        std::move(*body));
  } else {
    if (index >= bodies_.size()) return false;
    bodies_.erase(bodies_.begin() + static_cast<std::ptrdiff_t>(index));
  }
  reserveIdsForEditing();
  return true;
}

bool Document::applyFeatureSlice(BodyId bodyId, std::size_t index,
                                 const ShapeFeature* feature) {
  Body* body = findBody(bodyId);
  if (!body) return false;
  if (!feature) {
    if (index >= body->features_.size()) return false;
    body->features_.erase(body->features_.begin() +
                          static_cast<std::ptrdiff_t>(index));
  } else {
    auto cloned = feature->clone();
    auto* shape = dynamic_cast<ShapeFeature*>(cloned.get());
    if (!shape) return false;
    cloned.release();
    std::unique_ptr<ShapeFeature> replacement(shape);
    replacement->prepareForHistory();
    if (index > body->features_.size()) return false;
    if (index == body->features_.size())
      body->features_.push_back(std::move(replacement));
    else if (body->features_[index]->id() == feature->id())
      body->features_[index] = std::move(replacement);
    else
      body->features_.insert(body->features_.begin() +
                                 static_cast<std::ptrdiff_t>(index),
                             std::move(replacement));
  }
  reserveIdsForEditing();
  return true;
}

bool Document::applySketchSlice(std::size_t index,
                                std::optional<DocumentSketch> sketch) {
  if (sketch) {
    if (index > sketches_.size()) return false;
    if (index == sketches_.size()) sketches_.push_back(std::move(*sketch));
    else if (sketches_[index].id == sketch->id)
      sketches_[index] = std::move(*sketch);
    else sketches_.insert(
        sketches_.begin() + static_cast<std::ptrdiff_t>(index),
        std::move(*sketch));
  } else {
    if (index >= sketches_.size()) return false;
    sketches_.erase(sketches_.begin() + static_cast<std::ptrdiff_t>(index));
  }
  reserveIdsForEditing();
  return true;
}

}  // namespace solidar
