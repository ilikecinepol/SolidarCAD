#include "model/ProductDocument.h"

#include "model/AssemblyGraphAnalysis.h"
#include "model/AssemblyMate.h"
#include "model/ComponentGraphAnalysis.h"
#include "model/IdGeneration.h"

#include "model/OccurrenceDuplicate.h"
#include "model/PlanarMateSolver.h"

#include <atomic>
#include <Standard_Failure.hxx>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <set>
#include <type_traits>
#include <utility>

namespace solidar {
namespace {

std::atomic<std::uint64_t> componentIdentityTransitionValidationCount{};

constexpr double kComponentPlacementNoChangeMm = 1.0e-9;
constexpr double kComponentPlacementMaximumDeltaMm = 1.0e9;
constexpr double kComponentPlacementMaximumCoordinateMm = 1.0e12;

std::array<double, 3> inverseRotateVector(
    const std::array<double, 4>& quaternionWxyz,
    const std::array<double, 3>& vector) noexcept {
  const std::array<double, 3> u{-quaternionWxyz[1],
                                -quaternionWxyz[2],
                                -quaternionWxyz[3]};
  const std::array<double, 3> uv{
      u[1] * vector[2] - u[2] * vector[1],
      u[2] * vector[0] - u[0] * vector[2],
      u[0] * vector[1] - u[1] * vector[0]};
  const std::array<double, 3> uuv{
      u[1] * uv[2] - u[2] * uv[1],
      u[2] * uv[0] - u[0] * uv[2],
      u[0] * uv[1] - u[1] * uv[0]};
  std::array<double, 3> result{};
  for (std::size_t index = 0; index < result.size(); ++index)
    result[index] = vector[index] +
                    2.0 * (quaternionWxyz[0] * uv[index] + uuv[index]);
  return result;
}

class AcceptProductDocumentSuccessor final
    : public ProductDocumentSuccessor {
 public:
  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&,
      ProductDocumentSuccessorImpact) noexcept override {
    return ProductDocumentSuccessorStatus::Prepared;
  }
};

class ProductMutationGuard final {
 public:
  explicit ProductMutationGuard(bool& active) noexcept : active_(active) {
    active_ = true;
  }
  ~ProductMutationGuard() { active_ = false; }

  ProductMutationGuard(const ProductMutationGuard&) = delete;
  ProductMutationGuard& operator=(const ProductMutationGuard&) = delete;

 private:
  bool& active_;
};

enum class ComponentDrawingPartitionStatus {
  Partitioned,
  CrossDefinitionDependency,
  MissingBodyReference,
  ResourceFailure,
};

struct ComponentDrawingPartition final {
  ComponentDrawingPartitionStatus status{
      ComponentDrawingPartitionStatus::ResourceFailure};
  std::vector<ComponentDrawing> before;
  std::vector<ComponentDrawing> parentAfter;
  std::vector<ComponentDrawing> child;
};

ComponentDrawingPartition partitionComponentDrawings(
    const std::vector<ComponentDrawing>& drawings, const Document& document,
    BodyId movedBody) noexcept {
  try {
    ComponentDrawingPartition result;
    result.before = drawings;
    result.parentAfter.reserve(drawings.size());
    result.child.reserve(drawings.size());
    for (const auto& drawing : drawings) {
      bool referencesMoved = false;
      bool referencesRetained = false;
      for (const BodyId sourceBody : drawing.sourceBodyIds()) {
        if (!document.findBody(sourceBody)) {
          result.status =
              ComponentDrawingPartitionStatus::MissingBodyReference;
          return result;
        }
        if (sourceBody == movedBody)
          referencesMoved = true;
        else
          referencesRetained = true;
      }
      if (referencesMoved && referencesRetained) {
        result.status =
            ComponentDrawingPartitionStatus::CrossDefinitionDependency;
        return result;
      }
      if (referencesMoved)
        result.child.push_back(drawing);
      else
        result.parentAfter.push_back(drawing);
    }
    result.status = ComponentDrawingPartitionStatus::Partitioned;
    return result;
  } catch (...) {
    return {};
  }
}

constexpr ProductIncarnationId kReservedProductIncarnation{
    std::numeric_limits<std::uint64_t>::max()};

ProductIncarnationId nextProductIncarnation(bool force = false) noexcept {
  if (!force && !detail::explicitIdReservationEnabled())
    return kReservedProductIncarnation;
  static std::atomic<std::uint64_t> next{1};
  std::uint64_t value = next.fetch_add(1, std::memory_order_relaxed);
  if (value == 0) value = next.fetch_add(1, std::memory_order_relaxed);
  return ProductIncarnationId{value};
}

bool unicodeWhitespace(std::uint32_t codepoint) noexcept {
  return codepoint == 0x20 || (codepoint >= 0x09 && codepoint <= 0x0d) ||
         codepoint == 0x85 || codepoint == 0xa0 || codepoint == 0x1680 ||
         (codepoint >= 0x2000 && codepoint <= 0x200a) ||
         codepoint == 0x2028 || codepoint == 0x2029 ||
         codepoint == 0x202f || codepoint == 0x205f || codepoint == 0x3000;
}

bool decodeUtf8(std::string_view text, std::size_t& offset,
                std::uint32_t& codepoint) noexcept {
  const auto first = static_cast<unsigned char>(text[offset]);
  std::size_t length = 0;
  if (first <= 0x7f) {
    codepoint = first;
    length = 1;
  } else if (first >= 0xc2 && first <= 0xdf) {
    codepoint = first & 0x1f;
    length = 2;
  } else if (first >= 0xe0 && first <= 0xef) {
    codepoint = first & 0x0f;
    length = 3;
  } else if (first >= 0xf0 && first <= 0xf4) {
    codepoint = first & 0x07;
    length = 4;
  } else {
    return false;
  }
  if (offset + length > text.size()) return false;
  for (std::size_t index = 1; index < length; ++index) {
    const auto next = static_cast<unsigned char>(text[offset + index]);
    if ((next & 0xc0) != 0x80) return false;
    codepoint = (codepoint << 6) | (next & 0x3f);
  }
  if ((length == 3 && codepoint < 0x800) ||
      (length == 4 && codepoint < 0x10000) ||
      (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
      codepoint > 0x10ffff)
    return false;
  offset += length;
  return true;
}

}  // namespace

OccurrencePathCreateResult OccurrencePath::create(
    std::vector<OccurrenceId> entries) {
  if (entries.empty())
    return {OccurrencePathStatus::Empty, std::nullopt};
  if (entries.size() > kAssemblyGraphMaximumPathDepth)
    return {OccurrencePathStatus::UnsupportedDepth, std::nullopt};
  for (const auto id : entries) {
    if (!id.isValid())
      return {OccurrencePathStatus::InvalidOccurrenceId, std::nullopt};
  }
  return {OccurrencePathStatus::Valid,
          OccurrencePath(std::move(entries))};
}

ComponentOccurrence::ComponentOccurrence(
    OccurrenceId id, DefinitionId targetDefinitionId,
    RigidTransform3d placement, OccurrenceFlags flags,
    std::string displayNameUtf8)
    : ComponentOccurrence(id, targetDefinitionId, placement, placement, flags,
                          std::move(displayNameUtf8)) {}

ComponentOccurrence::ComponentOccurrence(
    OccurrenceId id, DefinitionId targetDefinitionId,
    RigidTransform3d placement, RigidTransform3d homePlacement,
    OccurrenceFlags flags, std::string displayNameUtf8)
    : id_(id),
      targetDefinitionId_(targetDefinitionId),
      placement_(std::move(placement)),
      homePlacement_(std::move(homePlacement)),
      flags_(flags),
      displayNameUtf8_(std::move(displayNameUtf8)) {
  if (!id.isValid())
    throw std::invalid_argument("ComponentOccurrence requires a valid ID");
  if (!targetDefinitionId.isValid())
    throw std::invalid_argument(
        "ComponentOccurrence requires a valid target definition ID");
  if (!displayNameUtf8_.empty() &&
      validateOccurrenceDisplayNameUtf8(displayNameUtf8_) !=
          OccurrenceDisplayNameStatus::Valid)
    throw std::invalid_argument(
        "ComponentOccurrence requires a valid UTF-8 display name");
}

OccurrenceDisplayNameStatus validateOccurrenceDisplayNameUtf8(
    std::string_view nameUtf8, bool allowEmpty) noexcept {
  constexpr std::size_t kMaximumBytes = 16 * 1024;
  if (nameUtf8.empty())
    return allowEmpty ? OccurrenceDisplayNameStatus::Valid
                      : OccurrenceDisplayNameStatus::Empty;
  if (nameUtf8.size() > kMaximumBytes)
    return OccurrenceDisplayNameStatus::TooLong;
  bool hasNonWhitespace = false;
  std::size_t offset = 0;
  while (offset < nameUtf8.size()) {
    if (nameUtf8[offset] == '\0')
      return OccurrenceDisplayNameStatus::EmbeddedNull;
    std::uint32_t codepoint = 0;
    if (!decodeUtf8(nameUtf8, offset, codepoint))
      return OccurrenceDisplayNameStatus::InvalidUtf8;
    if (codepoint <= 0x1f || (codepoint >= 0x7f && codepoint <= 0x9f) ||
        codepoint == 0x2028 || codepoint == 0x2029)
      return OccurrenceDisplayNameStatus::ForbiddenControl;
    if (!unicodeWhitespace(codepoint)) hasNonWhitespace = true;
  }
  return hasNonWhitespace ? OccurrenceDisplayNameStatus::Valid
                          : OccurrenceDisplayNameStatus::AllWhitespace;
}

ComponentDrawing::ComponentDrawing(
    DrawingId id, std::string nameUtf8, ComponentDrawingPaper paper,
    ComponentDrawingOrientation orientation, double scale,
    std::vector<BodyId> sourceBodyIds)
    : id_(id),
      nameUtf8_(std::move(nameUtf8)),
      paper_(paper),
      orientation_(orientation),
      scale_(scale),
      sourceBodyIds_(std::move(sourceBodyIds)) {
  if (!id_.isValid())
    throw std::invalid_argument("ComponentDrawing requires a valid ID");
  if (nameUtf8_.size() > kComponentDrawingMaximumNameUtf8Bytes ||
      validateOccurrenceDisplayNameUtf8(nameUtf8_) !=
          OccurrenceDisplayNameStatus::Valid)
    throw std::invalid_argument(
        "ComponentDrawing requires a bounded UTF-8 name");
  if (paper_ != ComponentDrawingPaper::A4)
    throw std::invalid_argument("ComponentDrawing requires A4 paper");
  if (orientation_ != ComponentDrawingOrientation::Portrait &&
      orientation_ != ComponentDrawingOrientation::Landscape)
    throw std::invalid_argument(
        "ComponentDrawing requires a valid orientation");
  if (!std::isfinite(scale_) || scale_ < kComponentDrawingMinimumScale ||
      scale_ > kComponentDrawingMaximumScale)
    throw std::invalid_argument("ComponentDrawing scale is out of range");
  if (sourceBodyIds_.size() >
      kComponentGraphMaximumDrawingBodyReferences)
    throw std::length_error("ComponentDrawing source budget exceeded");
  if (std::any_of(sourceBodyIds_.begin(), sourceBodyIds_.end(),
                  [](BodyId body) { return body == kInvalidBodyId; }))
    throw std::invalid_argument(
        "ComponentDrawing requires valid source body IDs");
  std::sort(sourceBodyIds_.begin(), sourceBodyIds_.end());
  sourceBodyIds_.erase(
      std::unique(sourceBodyIds_.begin(), sourceBodyIds_.end()),
      sourceBodyIds_.end());
}

PartDefinition::PartDefinition(DefinitionId id,
                               std::unique_ptr<Document>&& document)
    : id_(id) {
  if (!id.isValid())
    throw std::invalid_argument("PartDefinition requires a valid ID");
  if (!document)
    throw std::invalid_argument("PartDefinition requires a Document");
  document_ = std::move(document);
}

bool LinkedPartBinding::isValid() const noexcept {
  return key.isValid() &&
         isSafeLinkedPartRelativePathHintUtf8(relativePathHintUtf8);
}

bool isSafeLinkedPartRelativePathHintUtf8(std::string_view hint) noexcept {
  if (hint.empty() || hint.front() == '/' || hint.front() == '\\' ||
      hint.find('\0') != std::string_view::npos ||
      hint.find(':') != std::string_view::npos)
    return false;
  std::size_t utf8Offset = 0;
  while (utf8Offset < hint.size()) {
    std::uint32_t codepoint = 0;
    if (!decodeUtf8(hint, utf8Offset, codepoint)) return false;
  }
  std::size_t componentDepth = 0;
  std::size_t start = 0;
  while (start <= hint.size()) {
    std::size_t end = hint.find_first_of("/\\", start);
    if (end == std::string_view::npos) end = hint.size();
    const std::string_view component = hint.substr(start, end - start);
    if (!component.empty() && component != ".") {
      if (component == "..") {
        if (componentDepth == 0) return false;
        --componentDepth;
      } else {
        if (component.back() == ' ' || component.back() == '.') return false;
        const std::size_t dot = component.find('.');
        const auto base = component.substr(0, dot);
        auto asciiEqualFold = [](std::string_view value,
                                 std::string_view upper) noexcept {
          if (value.size() != upper.size()) return false;
          for (std::size_t index = 0; index < value.size(); ++index) {
            const unsigned char byte =
                static_cast<unsigned char>(value[index]);
            const char folded = byte >= 'a' && byte <= 'z'
                                    ? static_cast<char>(byte - 'a' + 'A')
                                    : static_cast<char>(byte);
            if (folded != upper[index]) return false;
          }
          return true;
        };
        const bool numberedDevice =
            base.size() == 4 && base[3] >= '1' && base[3] <= '9' &&
            (asciiEqualFold(base.substr(0, 3), "COM") ||
             asciiEqualFold(base.substr(0, 3), "LPT"));
        if (asciiEqualFold(base, "CON") || asciiEqualFold(base, "PRN") ||
            asciiEqualFold(base, "AUX") || asciiEqualFold(base, "NUL") ||
            numberedDevice)
          return false;
        ++componentDepth;
        if (componentDepth > 256) return false;
      }
    }
    if (end == hint.size()) break;
    start = end + 1;
  }
  return componentDepth != 0;
}

PartDefinition::PartDefinition(DefinitionId id,
                               std::unique_ptr<Document>&& document,
                               LinkedPartBinding binding)
    : PartDefinition(id, std::move(document)) {
  if (!binding.isValid())
    throw std::invalid_argument("PartDefinition requires a valid link binding");
  binding_.emplace(std::move(binding));
}

AssemblyDefinition::AssemblyDefinition(DefinitionId id)
    : id_(id), mateRegistry_(std::make_unique<MateRegistry>()) {
  if (!id.isValid())
    throw std::invalid_argument("AssemblyDefinition requires a valid ID");
}

AssemblyDefinition::~AssemblyDefinition() = default;
AssemblyDefinition::AssemblyDefinition(AssemblyDefinition&&) noexcept = default;
AssemblyDefinition& AssemblyDefinition::operator=(AssemblyDefinition&&) noexcept =
    default;

MateRegistry& AssemblyDefinition::mateRegistry() noexcept {
  return *mateRegistry_;
}

const MateRegistry& AssemblyDefinition::mateRegistry() const noexcept {
  return *mateRegistry_;
}

AssemblyOccurrenceInsertStatus AssemblyDefinition::insert(
    ComponentOccurrence&& occurrence) {
  const auto id = occurrence.id();
  if (occurrences_.contains(id))
    return AssemblyOccurrenceInsertStatus::DuplicateOccurrenceId;
  occurrences_.emplace(id, std::move(occurrence));
  return AssemblyOccurrenceInsertStatus::Inserted;
}

AssemblyOccurrenceEraseStatus AssemblyDefinition::erase(OccurrenceId id) {
  return occurrences_.erase(id) == 0
             ? AssemblyOccurrenceEraseStatus::OccurrenceMissing
             : AssemblyOccurrenceEraseStatus::Erased;
}

const ComponentOccurrence* AssemblyDefinition::find(OccurrenceId id) const
    noexcept {
  const auto it = occurrences_.find(id);
  return it == occurrences_.end() ? nullptr : &it->second;
}

FlatOccurrenceResolveResult AssemblyDefinition::resolve(
    const OccurrencePath& path) const noexcept {
  if (path.depth() != 1)
    return {FlatOccurrenceResolveStatus::UnsupportedDepth, nullptr};
  const auto* occurrence = find(path.entries().front());
  return occurrence == nullptr
             ? FlatOccurrenceResolveResult{
                   FlatOccurrenceResolveStatus::OccurrenceMissing, nullptr}
             : FlatOccurrenceResolveResult{
                   FlatOccurrenceResolveStatus::Resolved, occurrence};
}

bool AssemblyDefinition::setPlacement(OccurrenceId id,
                                      RigidTransform3d placement) noexcept {
  const auto it = occurrences_.find(id);
  if (it == occurrences_.end()) return false;
  it->second.placement_ = std::move(placement);
  return true;
}

bool AssemblyDefinition::setFlags(OccurrenceId id,
                                  OccurrenceFlags flags) noexcept {
  const auto it = occurrences_.find(id);
  if (it == occurrences_.end()) return false;
  it->second.flags_ = flags;
  return true;
}

bool AssemblyDefinition::setVisible(OccurrenceId id, bool visible) noexcept {
  const auto it = occurrences_.find(id);
  if (it == occurrences_.end()) return false;
  it->second.flags_.visible = visible;
  return true;
}

bool AssemblyDefinition::setGrounded(OccurrenceId id, bool grounded) noexcept {
  const auto it = occurrences_.find(id);
  if (it == occurrences_.end()) return false;
  it->second.flags_.grounded = grounded;
  return true;
}

bool AssemblyDefinition::setSuppressed(OccurrenceId id,
                                       bool suppressed) noexcept {
  const auto it = occurrences_.find(id);
  if (it == occurrences_.end()) return false;
  it->second.flags_.suppressed = suppressed;
  return true;
}

bool AssemblyDefinition::swapDisplayName(
    OccurrenceId id, std::string& displayNameUtf8) noexcept {
  const auto it = occurrences_.find(id);
  if (it == occurrences_.end()) return false;
  it->second.displayNameUtf8_.swap(displayNameUtf8);
  return true;
}

ComponentDefinition::ComponentDefinition(
    DefinitionId id, std::unique_ptr<Document>&& document)
    : ComponentDefinition(id, std::move(document), {}) {}

ComponentDefinition::ComponentDefinition(
    DefinitionId id, std::unique_ptr<Document>&& document,
    std::vector<ComponentDrawing>&& drawings)
    : id_(id) {
  if (!id.isValid())
    throw std::invalid_argument("ComponentDefinition requires a valid ID");
  if (!document)
    throw std::invalid_argument("ComponentDefinition requires a Document");
  if (drawings.size() > kComponentGraphMaximumDrawings)
    throw std::length_error("ComponentDefinition drawing budget exceeded");
  std::size_t referenceCount = 0;
  for (std::size_t index = 0; index < drawings.size(); ++index) {
    const auto& drawing = drawings[index];
    if (!drawing.id().isValid())
      throw std::invalid_argument("ComponentDefinition drawing ID is invalid");
    if (std::any_of(drawings.begin(), drawings.begin() +
                                         static_cast<std::ptrdiff_t>(index),
                    [&drawing](const ComponentDrawing& previous) {
                      return previous.id() == drawing.id();
                    }))
      throw std::invalid_argument(
          "ComponentDefinition drawing IDs must be unique");
    if (drawing.sourceBodyIds().size() >
        kComponentGraphMaximumDrawingBodyReferences - referenceCount)
      throw std::length_error(
          "ComponentDefinition drawing source budget exceeded");
    referenceCount += drawing.sourceBodyIds().size();
    for (const BodyId bodyId : drawing.sourceBodyIds())
      if (!document->findBody(bodyId))
        throw std::invalid_argument(
            "ComponentDefinition drawing source body is missing");
  }
  document_ = std::move(document);
  drawings_ = std::move(drawings);
}

ComponentOccurrenceInsertStatus ComponentDefinition::insert(
    ComponentOccurrence&& occurrence) {
  const auto id = occurrence.id();
  if (occurrences_.contains(id))
    return ComponentOccurrenceInsertStatus::DuplicateOccurrenceId;
  occurrences_.emplace(id, std::move(occurrence));
  return ComponentOccurrenceInsertStatus::Inserted;
}

ComponentOccurrenceEraseStatus ComponentDefinition::erase(OccurrenceId id) {
  return occurrences_.erase(id) == 0
             ? ComponentOccurrenceEraseStatus::OccurrenceMissing
             : ComponentOccurrenceEraseStatus::Erased;
}

const ComponentOccurrence* ComponentDefinition::find(OccurrenceId id) const
    noexcept {
  const auto found = occurrences_.find(id);
  return found == occurrences_.end() ? nullptr : &found->second;
}

const ComponentDrawing* ComponentDefinition::findDrawing(DrawingId id) const
    noexcept {
  const auto found = std::find_if(
      drawings_.begin(), drawings_.end(),
      [id](const ComponentDrawing& drawing) { return drawing.id() == id; });
  return found == drawings_.end() ? nullptr : &*found;
}

bool ComponentDefinition::setPlacement(
    OccurrenceId id, RigidTransform3d placement) noexcept {
  const auto found = occurrences_.find(id);
  if (found == occurrences_.end()) return false;
  found->second.placement_ = std::move(placement);
  return true;
}

bool ComponentDefinition::setHomePlacement(
    OccurrenceId id, RigidTransform3d homePlacement) noexcept {
  const auto found = occurrences_.find(id);
  if (found == occurrences_.end()) return false;
  found->second.homePlacement_ = std::move(homePlacement);
  return true;
}

bool ComponentDefinition::setVisible(OccurrenceId id, bool visible) noexcept {
  const auto found = occurrences_.find(id);
  if (found == occurrences_.end()) return false;
  found->second.flags_.visible = visible;
  return true;
}

bool ComponentDefinition::swapDisplayName(
    OccurrenceId id, std::string& displayNameUtf8) noexcept {
  const auto found = occurrences_.find(id);
  if (found == occurrences_.end()) return false;
  found->second.displayNameUtf8_.swap(displayNameUtf8);
  return true;
}

ComponentDefinition::OccurrenceNode ComponentDefinition::extract(
    OccurrenceId id) noexcept {
  return occurrences_.extract(id);
}

void ComponentDefinition::restore(OccurrenceNode&& node) noexcept {
  if (!node.empty()) occurrences_.insert(std::move(node));
}

DefinitionInsertStatus DefinitionRegistry::insert(
    PartDefinition&& definition) {
  const auto id = definition.id();
  if (definitions_.contains(id))
    return DefinitionInsertStatus::DuplicateDefinitionId;
  if (definition.linkedBinding()) {
    for (const auto& [existingId, stored] : definitions_) {
      static_cast<void>(existingId);
      const auto* part = std::get_if<PartDefinition>(&stored);
      if (part && part->linkedBinding() &&
          part->linkedBinding()->key == definition.linkedBinding()->key)
        return DefinitionInsertStatus::DuplicateExternalSourceKey;
    }
  }
  definitions_.emplace(
      id, StoredDefinition(std::in_place_type<PartDefinition>,
                           std::move(definition)));
  return DefinitionInsertStatus::Inserted;
}

DefinitionInsertStatus DefinitionRegistry::insert(
    AssemblyDefinition&& definition) {
  const auto id = definition.id();
  if (definitions_.contains(id))
    return DefinitionInsertStatus::DuplicateDefinitionId;
  for (const auto& [occurrenceId, occurrence] : definition.occurrences()) {
    static_cast<void>(occurrenceId);
    if (findPart(occurrence.targetDefinitionId()) != nullptr) continue;
    if (findAssembly(occurrence.targetDefinitionId()) != nullptr)
      return DefinitionInsertStatus::UnsupportedAssemblyTarget;
    return DefinitionInsertStatus::DanglingOccurrenceTarget;
  }
  definitions_.emplace(
      id, StoredDefinition(std::in_place_type<AssemblyDefinition>,
                           std::move(definition)));
  return DefinitionInsertStatus::Inserted;
}

DefinitionInsertStatus DefinitionRegistry::insert(
    ComponentDefinition&& definition) {
  const auto id = definition.id();
  if (definitions_.contains(id))
    return DefinitionInsertStatus::DuplicateDefinitionId;
  for (const auto& [occurrenceId, occurrence] : definition.occurrences()) {
    static_cast<void>(occurrenceId);
    if (occurrence.targetDefinitionId() == id)
      return DefinitionInsertStatus::ComponentGraphCycle;
    if (findComponent(occurrence.targetDefinitionId()) != nullptr) continue;
    if (contains(occurrence.targetDefinitionId()))
      return DefinitionInsertStatus::UnsupportedComponentTarget;
    return DefinitionInsertStatus::DanglingOccurrenceTarget;
  }
  definitions_.emplace(
      id, StoredDefinition(std::in_place_type<ComponentDefinition>,
                           std::move(definition)));
  return DefinitionInsertStatus::Inserted;
}

DefinitionEraseStatus DefinitionRegistry::erase(DefinitionId id) {
  for (const auto& [definitionId, stored] : definitions_) {
    static_cast<void>(definitionId);
    if (const auto* assembly = std::get_if<AssemblyDefinition>(&stored)) {
      for (const auto& [occurrenceId, occurrence] :
           assembly->occurrences()) {
        static_cast<void>(occurrenceId);
        if (occurrence.targetDefinitionId() == id)
          return DefinitionEraseStatus::DefinitionReferenced;
      }
    }
    if (const auto* component = std::get_if<ComponentDefinition>(&stored)) {
      for (const auto& [occurrenceId, occurrence] :
           component->occurrences()) {
        static_cast<void>(occurrenceId);
        if (occurrence.targetDefinitionId() == id)
          return DefinitionEraseStatus::DefinitionReferenced;
      }
    }
  }
  if (definitions_.erase(id) == 0)
    return DefinitionEraseStatus::DefinitionMissing;
  return DefinitionEraseStatus::Erased;
}

const PartDefinition* DefinitionRegistry::find(DefinitionId id) const
    noexcept {
  return findPart(id);
}

const PartDefinition* DefinitionRegistry::findPart(DefinitionId id) const
    noexcept {
  const auto it = definitions_.find(id);
  return it == definitions_.end() ? nullptr
                                  : std::get_if<PartDefinition>(&it->second);
}

const AssemblyDefinition* DefinitionRegistry::findAssembly(
    DefinitionId id) const noexcept {
  const auto it = definitions_.find(id);
  return it == definitions_.end()
             ? nullptr
             : std::get_if<AssemblyDefinition>(&it->second);
}

const ComponentDefinition* DefinitionRegistry::findComponent(
    DefinitionId id) const noexcept {
  const auto it = definitions_.find(id);
  return it == definitions_.end()
             ? nullptr
             : std::get_if<ComponentDefinition>(&it->second);
}

std::optional<DefinitionKind> DefinitionRegistry::kind(
    DefinitionId id) const noexcept {
  const auto it = definitions_.find(id);
  if (it == definitions_.end()) return std::nullopt;
  if (std::holds_alternative<PartDefinition>(it->second))
    return DefinitionKind::Part;
  if (std::holds_alternative<AssemblyDefinition>(it->second))
    return DefinitionKind::Assembly;
  return DefinitionKind::Component;
}

bool DefinitionRegistry::contains(DefinitionId id) const noexcept {
  return definitions_.contains(id);
}

std::vector<DefinitionId> DefinitionRegistry::definitionIds() const {
  std::vector<DefinitionId> ids;
  ids.reserve(definitions_.size());
  for (const auto& [id, stored] : definitions_) {
    static_cast<void>(stored);
    ids.push_back(id);
  }
  return ids;
}

PartDefinition* DefinitionRegistry::findPartMutable(DefinitionId id) noexcept {
  const auto it = definitions_.find(id);
  return it == definitions_.end() ? nullptr
                                  : std::get_if<PartDefinition>(&it->second);
}

AssemblyDefinition* DefinitionRegistry::findAssemblyMutable(
    DefinitionId id) noexcept {
  const auto it = definitions_.find(id);
  return it == definitions_.end()
             ? nullptr
             : std::get_if<AssemblyDefinition>(&it->second);
}

ComponentDefinition* DefinitionRegistry::findComponentMutable(
    DefinitionId id) noexcept {
  const auto it = definitions_.find(id);
  return it == definitions_.end()
             ? nullptr
             : std::get_if<ComponentDefinition>(&it->second);
}

ProductDocument::ProductDocument(
    DefinitionRegistry&& definitions, DefinitionId rootDefinitionId,
    std::optional<ProductDocumentIdentity> identity) noexcept
    : definitions_(std::move(definitions)),
      rootDefinitionId_(rootDefinitionId),
      identity_(std::move(identity)),
      incarnation_(nextProductIncarnation()) {}

bool ProductDocument::finalizeReservedIncarnation() noexcept {
  if (incarnation_ != kReservedProductIncarnation) return false;
  incarnation_ = nextProductIncarnation(true);
  return incarnation_.isValid() && incarnation_ != kReservedProductIncarnation;
}

ProductDocument::ProductDocument(ProductDocument&& other) noexcept
    : definitions_(std::move(other.definitions_)),
      rootDefinitionId_(other.rootDefinitionId_),
      identity_(std::move(other.identity_)),
      incarnation_(other.incarnation_) {
  other.rootDefinitionId_ = kInvalidDefinitionId;
  other.incarnation_ = nextProductIncarnation();
  other.productMutationTransactionActive_ = false;
}

ProductDocument& ProductDocument::operator=(ProductDocument&& other) noexcept {
  if (this == &other) return *this;
  definitions_ = std::move(other.definitions_);
  rootDefinitionId_ = other.rootDefinitionId_;
  identity_ = std::move(other.identity_);
  incarnation_ = other.incarnation_;
  productMutationTransactionActive_ = false;
  other.rootDefinitionId_ = kInvalidDefinitionId;
  other.incarnation_ = nextProductIncarnation();
  other.productMutationTransactionActive_ = false;
  return *this;
}

ProductDocumentCreateResult ProductDocument::create(
    DefinitionRegistry&& definitions, DefinitionId rootDefinitionId,
    std::optional<ProductDocumentIdentity> identity) {
  if (!rootDefinitionId.isValid())
    return {ProductDocumentStatus::InvalidRootId, nullptr};
  if (!definitions.contains(rootDefinitionId))
    return {ProductDocumentStatus::MissingRootDefinition, nullptr};
  if (identity.has_value() && !identity->projectId.isValid())
    return {ProductDocumentStatus::InvalidProjectId, nullptr};

  std::size_t assemblyCount = 0;
  for (const auto& [definitionId, stored] : definitions.definitions_) {
    if (std::holds_alternative<ComponentDefinition>(stored))
      return {ProductDocumentStatus::UnsupportedComponentDefinition,
              nullptr};
    const auto* assembly = std::get_if<AssemblyDefinition>(&stored);
    if (!assembly) continue;
    ++assemblyCount;
    if (definitionId != rootDefinitionId)
      return {ProductDocumentStatus::UnsupportedNonRootAssemblyDefinition,
              nullptr};
    for (const auto& [occurrenceId, occurrence] : assembly->occurrences()) {
      static_cast<void>(occurrenceId);
      if (definitions.findPart(occurrence.targetDefinitionId()) != nullptr)
        continue;
      if (definitions.findAssembly(occurrence.targetDefinitionId()) != nullptr)
        return {ProductDocumentStatus::UnsupportedAssemblyTarget, nullptr};
      return {ProductDocumentStatus::DanglingOccurrenceTarget, nullptr};
    }
  }
  if (assemblyCount > 1)
    return {ProductDocumentStatus::UnsupportedNonRootAssemblyDefinition,
            nullptr};

  return {ProductDocumentStatus::Valid,
          std::unique_ptr<ProductDocument>(
              new ProductDocument(std::move(definitions), rootDefinitionId,
                                  std::move(identity)))};
}

ProductDocumentCreateResult ProductDocument::createNested(
    std::vector<PartDefinition>&& parts,
    std::vector<AssemblyDefinition>&& assemblies,
    DefinitionId rootDefinitionId,
    std::optional<ProductDocumentIdentity> identity) {
  if (!rootDefinitionId.isValid())
    return {ProductDocumentStatus::InvalidRootId, nullptr};
  if (identity && !identity->projectId.isValid())
    return {ProductDocumentStatus::InvalidProjectId, nullptr};
  if (parts.size() > kAssemblyGraphMaximumDefinitions ||
      assemblies.size() > kAssemblyGraphMaximumDefinitions -
                              std::min(parts.size(),
                                       kAssemblyGraphMaximumDefinitions))
    return {ProductDocumentStatus::UnsupportedNonRootAssemblyDefinition,
            nullptr};
  if (parts.size() > kAssemblyGraphMaximumUniqueSources)
    return {ProductDocumentStatus::AssemblyGraphBudgetExceeded, nullptr};
  std::size_t authoredOccurrenceCount = 0;
  for (const auto& assembly : assemblies) {
    if (!fitsAssemblyGraphBudget(authoredOccurrenceCount, assembly.size(),
                                 kAssemblyGraphMaximumAuthoredOccurrences))
      return {ProductDocumentStatus::AssemblyGraphBudgetExceeded, nullptr};
    authoredOccurrenceCount += assembly.size();
  }

  DefinitionRegistry staged;
  std::set<ExternalSourceKey> linkedKeys;
  for (auto& part : parts) {
    if (!part.id().isValid() || staged.definitions_.contains(part.id()))
      return {ProductDocumentStatus::DuplicateDefinitionId, nullptr};
    if (part.linkedBinding() &&
        !linkedKeys.insert(part.linkedBinding()->key).second)
      return {ProductDocumentStatus::DuplicateExternalSourceKey, nullptr};
    staged.definitions_.emplace(
        part.id(), DefinitionRegistry::StoredDefinition(
                       std::in_place_type<PartDefinition>, std::move(part)));
  }
  for (auto& assembly : assemblies) {
    if (!assembly.id().isValid() || staged.definitions_.contains(assembly.id()))
      return {ProductDocumentStatus::DuplicateDefinitionId, nullptr};
    staged.definitions_.emplace(
        assembly.id(), DefinitionRegistry::StoredDefinition(
                           std::in_place_type<AssemblyDefinition>,
                           std::move(assembly)));
  }
  const auto analysis = analyzeAssemblyGraph(staged, rootDefinitionId);
  if (!analysis) {
    switch (analysis.status) {
      case AssemblyGraphStatus::InvalidRoot:
        return {ProductDocumentStatus::MissingRootDefinition, nullptr};
      case AssemblyGraphStatus::RootIsNotAssembly:
        return {ProductDocumentStatus::UnsupportedAssemblyTarget, nullptr};
      case AssemblyGraphStatus::UnsupportedDefinitionKind:
        return {ProductDocumentStatus::UnsupportedComponentDefinition,
                nullptr};
      case AssemblyGraphStatus::DanglingTarget:
        return {ProductDocumentStatus::DanglingOccurrenceTarget, nullptr};
      case AssemblyGraphStatus::Cycle:
        return {ProductDocumentStatus::AssemblyGraphCycle, nullptr};
      case AssemblyGraphStatus::DefinitionBudgetExceeded:
      case AssemblyGraphStatus::OccurrenceBudgetExceeded:
      case AssemblyGraphStatus::UnsupportedDepth:
      case AssemblyGraphStatus::ExpandedWorkBudgetExceeded:
      case AssemblyGraphStatus::LeafBudgetExceeded:
      case AssemblyGraphStatus::UniqueSourceBudgetExceeded:
        return {ProductDocumentStatus::AssemblyGraphBudgetExceeded, nullptr};
      case AssemblyGraphStatus::ResourceFailure:
        return {ProductDocumentStatus::ResourceFailure, nullptr};
      default:
        return {ProductDocumentStatus::UnsupportedNonRootAssemblyDefinition,
                nullptr};
    }
  }
  return {ProductDocumentStatus::Valid,
          std::unique_ptr<ProductDocument>(new ProductDocument(
              std::move(staged), rootDefinitionId, std::move(identity)))};
}

ProductDocumentCreateResult ProductDocument::createComponentGraph(
    std::vector<ComponentDefinition>&& components,
    DefinitionId rootDefinitionId,
    std::optional<ProductDocumentIdentity> identity) {
  if (!rootDefinitionId.isValid())
    return {ProductDocumentStatus::InvalidRootId, nullptr};
  if (identity && !identity->projectId.isValid())
    return {ProductDocumentStatus::InvalidProjectId, nullptr};
  const auto validation = analyzeComponentGraph(components, rootDefinitionId);
  switch (validation) {
    case ComponentGraphStatus::Valid:
      break;
    case ComponentGraphStatus::DuplicateDefinitionId:
      return {ProductDocumentStatus::DuplicateDefinitionId, nullptr};
    case ComponentGraphStatus::MissingRootDefinition:
      return {ProductDocumentStatus::MissingRootDefinition, nullptr};
    case ComponentGraphStatus::UnsupportedComponentDefinition:
      return {ProductDocumentStatus::UnsupportedComponentDefinition, nullptr};
    case ComponentGraphStatus::DanglingTarget:
      return {ProductDocumentStatus::DanglingOccurrenceTarget, nullptr};
    case ComponentGraphStatus::Cycle:
      return {ProductDocumentStatus::ComponentGraphCycle, nullptr};
    case ComponentGraphStatus::DefinitionBudgetExceeded:
    case ComponentGraphStatus::OccurrenceBudgetExceeded:
    case ComponentGraphStatus::UnsupportedDepth:
    case ComponentGraphStatus::ExpandedWorkBudgetExceeded:
      return {ProductDocumentStatus::ComponentGraphBudgetExceeded, nullptr};
    case ComponentGraphStatus::InvalidDrawing:
      return {ProductDocumentStatus::InvalidComponentDrawing, nullptr};
    case ComponentGraphStatus::DuplicateDrawingId:
      return {ProductDocumentStatus::DuplicateComponentDrawingId, nullptr};
    case ComponentGraphStatus::DanglingDrawingBody:
      return {ProductDocumentStatus::DanglingComponentDrawingBody, nullptr};
    case ComponentGraphStatus::DrawingBudgetExceeded:
      return {ProductDocumentStatus::ComponentDrawingBudgetExceeded, nullptr};
    case ComponentGraphStatus::ResourceFailure:
      return {ProductDocumentStatus::ResourceFailure, nullptr};
  }
  try {
    DefinitionRegistry staged;
    for (auto& component : components) {
      staged.definitions_.emplace(
          component.id(), DefinitionRegistry::StoredDefinition(
                              std::in_place_type<ComponentDefinition>,
                              std::move(component)));
    }
    return {ProductDocumentStatus::Valid,
            std::unique_ptr<ProductDocument>(new ProductDocument(
                std::move(staged), rootDefinitionId, std::move(identity)))};
  } catch (...) {
    return {ProductDocumentStatus::ResourceFailure, nullptr};
  }
}

ComponentIdentityTransition::ComponentIdentityTransition(
    ComponentIdentityTransitionKind kind,
    ProductIncarnationId sourceIncarnation, DefinitionId sourceRoot,
    std::optional<ProductDocumentIdentity> sourceIdentity,
    std::vector<std::pair<DefinitionId, DefinitionStateId>> definitionStates,
    ProductDocumentIdentity targetIdentity) noexcept
    : kind_(kind),
      sourceIncarnation_(sourceIncarnation),
      sourceRoot_(sourceRoot),
      sourceIdentity_(std::move(sourceIdentity)),
      definitionStates_(std::move(definitionStates)),
      targetIdentity_(std::move(targetIdentity)) {}

ComponentIdentityTransitionPrepareResult ComponentIdentityTransition::prepare(
    const ProductDocument& product, ProductDocumentIdentity target,
    ComponentIdentityTransitionKind kind) noexcept {
  if (!target.projectId.isValid())
    return {ComponentIdentityTransitionStatus::InvalidTargetIdentity,
            std::nullopt};
  if (product.mutationTransactionActive())
    return {ComponentIdentityTransitionStatus::SourceBusy, std::nullopt};
  if (!product.incarnation().isValid() ||
      !product.rootDefinitionId().isValid())
    return {ComponentIdentityTransitionStatus::InvalidSource, std::nullopt};
  if (product.rootDefinitionKind() != DefinitionKind::Component ||
      !product.rootComponentDefinition())
    return {ComponentIdentityTransitionStatus::RootIsNotComponent,
            std::nullopt};
  if (kind == ComponentIdentityTransitionKind::FirstSave) {
    if (product.identity())
      return {ComponentIdentityTransitionStatus::SourceIdentityMustBeAbsent,
              std::nullopt};
  } else {
    if (!product.identity() || !product.identity()->projectId.isValid())
      return {ComponentIdentityTransitionStatus::SourceIdentityRequired,
              std::nullopt};
    if (product.identity()->projectId == target.projectId)
      return {ComponentIdentityTransitionStatus::SameProjectIdentity,
              std::nullopt};
  }
  try {
    const auto ids = product.definitions().definitionIds();
    if (ids.empty() || ids.size() != product.definitions().size())
      return {ComponentIdentityTransitionStatus::InvalidSource,
              std::nullopt};
    std::vector<std::pair<DefinitionId, DefinitionStateId>> states;
    states.reserve(ids.size());
    for (const DefinitionId id : ids) {
      const DefinitionStateId state = product.definitionStateId(id);
      if (!id.isValid() || !state.isValid() ||
          product.definitions().kind(id) != DefinitionKind::Component)
        return {ComponentIdentityTransitionStatus::InvalidDefinitionState,
                std::nullopt};
      states.emplace_back(id, state);
    }
    ComponentIdentityTransition transition(
        kind, product.incarnation(), product.rootDefinitionId(),
        product.identity(), std::move(states), std::move(target));
    if (!transition.isCurrentSource(product))
      return {ComponentIdentityTransitionStatus::InvalidSource,
              std::nullopt};
    return {ComponentIdentityTransitionStatus::Prepared,
            std::move(transition)};
  } catch (...) {
    return {ComponentIdentityTransitionStatus::ResourceFailure,
            std::nullopt};
  }
}

ComponentIdentityTransitionPrepareResult
ComponentIdentityTransition::prepareFirstSave(
    const ProductDocument& product, ProductDocumentIdentity target) noexcept {
  return prepare(
      product, std::move(target), ComponentIdentityTransitionKind::FirstSave);
}

ComponentIdentityTransitionPrepareResult
ComponentIdentityTransition::prepareSaveAs(
    const ProductDocument& product, ProductDocumentIdentity target) noexcept {
  return prepare(
      product, std::move(target), ComponentIdentityTransitionKind::SaveAs);
}

bool ComponentIdentityTransition::matchesStructure(
    const ProductDocument& product) const noexcept {
  componentIdentityTransitionValidationCount.fetch_add(
      1, std::memory_order_relaxed);
  if (product.mutationTransactionActive() ||
      product.incarnation() != sourceIncarnation_ ||
      product.rootDefinitionId() != sourceRoot_ ||
      product.rootDefinitionKind() != DefinitionKind::Component ||
      !product.rootComponentDefinition() ||
      product.definitions().size() != definitionStates_.size())
    return false;
  try {
    const auto ids = product.definitions().definitionIds();
    if (ids.size() != definitionStates_.size()) return false;
    for (std::size_t index = 0; index < ids.size(); ++index) {
      if (ids[index] != definitionStates_[index].first ||
          product.definitionStateId(ids[index]) !=
              definitionStates_[index].second ||
          product.definitions().kind(ids[index]) != DefinitionKind::Component)
        return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

std::uint64_t ComponentIdentityTransition::validationCountForTesting()
    noexcept {
  return componentIdentityTransitionValidationCount.load(
      std::memory_order_relaxed);
}

void ComponentIdentityTransition::resetValidationCountForTesting() noexcept {
  componentIdentityTransitionValidationCount.store(0,
                                                   std::memory_order_relaxed);
}

bool ComponentIdentityTransition::isCurrentSource(
    const ProductDocument& product) const noexcept {
  return matchesStructure(product) && product.identity() == sourceIdentity_;
}

bool ComponentIdentityTransition::matchesCommittedProduct(
    const ProductDocument& product) const noexcept {
  return matchesStructure(product) &&
         product.identity() ==
             std::optional<ProductDocumentIdentity>{targetIdentity_};
}

bool ProductDocument::commitProductIdentity(
    ProductDocumentIdentity identity) noexcept {
  if (productMutationTransactionActive_) return false;
  if (identity_ || !identity.projectId.isValid()) return false;
  identity_.emplace(std::move(identity));
  return true;
}

ProductDocumentForkIdentityStatus ProductDocument::commitForkIdentity(
    const ProductDocumentIdentity& expectedIdentity,
    ProductDocumentIdentity replacementIdentity) noexcept {
  if (productMutationTransactionActive_)
    return ProductDocumentForkIdentityStatus::Busy;
  if (!identity_)
    return ProductDocumentForkIdentityStatus::SourceMissingIdentity;
  if (!replacementIdentity.projectId.isValid())
    return ProductDocumentForkIdentityStatus::InvalidReplacementProjectId;
  if (replacementIdentity.projectId == identity_->projectId)
    return ProductDocumentForkIdentityStatus::SameProjectId;
  if (*identity_ != expectedIdentity)
    return ProductDocumentForkIdentityStatus::StaleExpectedIdentity;

  static_assert(std::is_nothrow_move_constructible_v<
                ProductDocumentIdentity>);
  static_assert(std::is_nothrow_swappable_v<
                std::optional<ProductDocumentIdentity>>);
  std::optional<ProductDocumentIdentity> prepared(
      std::in_place, std::move(replacementIdentity));
  identity_.swap(prepared);
  return ProductDocumentForkIdentityStatus::Committed;
}

ProductDocumentStandaloneConversionStatus
ProductDocument::commitPreparedStandaloneConversion(
    ProductDocument&& prepared) noexcept {
  if (productMutationTransactionActive_ ||
      prepared.productMutationTransactionActive_)
    return ProductDocumentStandaloneConversionStatus::Busy;
  if (this == &prepared)
    return ProductDocumentStandaloneConversionStatus::SelfTransition;
  if (identity_)
    return ProductDocumentStandaloneConversionStatus::SourceAlreadyIdentified;

  if (!hasSingleValidRootDefinition())
    return ProductDocumentStandaloneConversionStatus::SourceNotSingleRoot;
  if (!prepared.identity_)
    return ProductDocumentStandaloneConversionStatus::PreparedMissingIdentity;
  if (!prepared.identity_->projectId.isValid())
    return ProductDocumentStandaloneConversionStatus::PreparedInvalidIdentity;
  if (!prepared.hasSingleValidRootDefinition())
    return ProductDocumentStandaloneConversionStatus::PreparedNotSingleRoot;

  // Both registries contain exactly their root node. Swapping only the owned
  // Document pointers is allocation-free and preserves the active graph's
  // address. Moving the prepared carrier then adopts its root ID and identity
  // while destroying the reconstructed candidate now held by this carrier.
  auto* sourceRoot = definitions_.findPartMutable(rootDefinitionId_);
  auto* preparedRoot =
      prepared.definitions_.findPartMutable(prepared.rootDefinitionId_);
  sourceRoot->document_.swap(preparedRoot->document_);
  *this = std::move(prepared);
  return ProductDocumentStandaloneConversionStatus::Committed;
}

const PartDefinition& ProductDocument::rootDefinition() const {
  const auto* root = rootPartDefinition();
  if (!root)
    throw std::logic_error(
        "rootDefinition is only valid for a part-root product");
  return *root;
}

DefinitionKind ProductDocument::rootDefinitionKind() const noexcept {
  return *definitions_.kind(rootDefinitionId_);
}

const PartDefinition* ProductDocument::rootPartDefinition() const noexcept {
  return definitions_.findPart(rootDefinitionId_);
}

const AssemblyDefinition* ProductDocument::rootAssemblyDefinition() const
    noexcept {
  return definitions_.findAssembly(rootDefinitionId_);
}

const ComponentDefinition* ProductDocument::rootComponentDefinition() const
    noexcept {
  return definitions_.findComponent(rootDefinitionId_);
}

bool ProductDocument::hasSingleValidRootDefinition() const noexcept {
  if (!rootDefinitionId_.isValid() ||
      definitions_.definitions_.size() != 1)
    return false;
  const auto& [storedId, stored] = *definitions_.definitions_.begin();
  const auto* root = std::get_if<PartDefinition>(&stored);
  return root != nullptr && storedId == rootDefinitionId_ &&
         root->id_ == rootDefinitionId_ && root->document_ != nullptr;
}

Document& ProductDocument::rootDocument() {
  auto* root = definitions_.findPartMutable(rootDefinitionId_);
  if (!root)
    throw std::logic_error(
        "rootDocument is only valid for a part-root product");
  return root->document();
}

const Document& ProductDocument::rootDocument() const {
  return rootDefinition().document();
}

const Document* ProductDocument::partDocument(DefinitionId id) const noexcept {
  const auto* part = definitions_.findPart(id);
  return part ? &part->document() : nullptr;
}

const Document* ProductDocument::componentDocument(
    DefinitionId id) const noexcept {
  const auto* component = definitions_.findComponent(id);
  return component ? &component->document() : nullptr;
}

const LinkedPartBinding* ProductDocument::partLinkedBinding(
    DefinitionId id) const noexcept {
  const auto* part = definitions_.findPart(id);
  return part && part->binding_ ? &*part->binding_ : nullptr;
}

DefinitionStateId ProductDocument::definitionStateId(
    DefinitionId id) const noexcept {
  const auto* part = definitions_.findPart(id);
  if (part) return part->stateId();
  const auto* component = definitions_.findComponent(id);
  return component ? component->stateId() : kInvalidDefinitionStateId;
}

ComponentExtractionPrepareResult ProductDocument::prepareComponentExtraction(
    const ComponentExtractionAddress& address) const noexcept {
  const auto fail = [](ComponentExtractionStatus status) {
    return ComponentExtractionPrepareResult{status, std::nullopt};
  };
  if (productMutationTransactionActive_)
    return fail(ComponentExtractionStatus::Busy);
  if (!address.productIncarnation.isValid() ||
      !address.rootDefinitionId.isValid() ||
      !address.ownerDefinitionId.isValid() ||
      !address.expectedOwnerStateId.isValid() ||
      address.bodyId == kInvalidBodyId ||
      !address.childDefinitionId.isValid() ||
      !address.childOccurrenceId.isValid() ||
      address.childDefinitionId == address.ownerDefinitionId)
    return fail(ComponentExtractionStatus::InvalidAddress);
  if (address.productIncarnation != incarnation_)
    return fail(ComponentExtractionStatus::ProductIncarnationMismatch);
  if (address.rootDefinitionId != rootDefinitionId_)
    return fail(ComponentExtractionStatus::RootDefinitionMismatch);
  if (definitions_.contains(address.childDefinitionId))
    return fail(ComponentExtractionStatus::DuplicateDefinitionId);

  const Document* ownerDocument = nullptr;
  DefinitionStateId beforeState;
  std::uint64_t beforeNext = 0;
  bool convertedPartRoot = false;
  const std::vector<ComponentDrawing>* ownerDrawings = nullptr;
  if (const auto* part = rootPartDefinition()) {
    if (address.ownerDefinitionId != rootDefinitionId_)
      return fail(ComponentExtractionStatus::OwnerMustBeRoot);
    if (!hasSingleValidRootDefinition() || part->isLinked())
      return fail(ComponentExtractionStatus::UnsupportedRootKind);
    ownerDocument = &part->document();
    beforeState = part->stateId_;
    beforeNext = part->nextStateIdValue_;
    convertedPartRoot = true;
  } else if (rootComponentDefinition()) {
    const auto* component =
        definitions_.findComponent(address.ownerDefinitionId);
    if (!component)
      return fail(ComponentExtractionStatus::InvalidAddress);
    ownerDocument = &component->document();
    ownerDrawings = &component->drawings();
    beforeState = component->stateId_;
    beforeNext = component->nextStateIdValue_;
    if (component->find(address.childOccurrenceId))
      return fail(ComponentExtractionStatus::DuplicateOccurrenceId);
  } else {
    return fail(ComponentExtractionStatus::UnsupportedRootKind);
  }
  if (beforeState != address.expectedOwnerStateId)
    return fail(ComponentExtractionStatus::OwnerStateMismatch);
  if (beforeNext == 0 ||
      beforeNext == std::numeric_limits<std::uint64_t>::max())
    return fail(ComponentExtractionStatus::ResourceFailure);

  ComponentExtractionPlan plan =
      ownerDocument->planComponentExtraction(address.bodyId);
  switch (plan.status) {
    case ComponentExtractionPlanStatus::Planned:
      break;
    case ComponentExtractionPlanStatus::BodyMissing:
    case ComponentExtractionPlanStatus::InvalidBodyId:
      return fail(ComponentExtractionStatus::BodyMissing);
    case ComponentExtractionPlanStatus::CrossDefinitionDependency:
      return fail(ComponentExtractionStatus::CrossDefinitionDependency);
    case ComponentExtractionPlanStatus::InvalidDependencyGraph:
      return fail(ComponentExtractionStatus::InvalidDependencyGraph);
    case ComponentExtractionPlanStatus::ResourceFailure:
      return fail(ComponentExtractionStatus::ResourceFailure);
  }
  ComponentDrawingPartition drawingPartition;
  if (ownerDrawings)
    drawingPartition = partitionComponentDrawings(
        *ownerDrawings, *ownerDocument, address.bodyId);
  else
    drawingPartition.status = ComponentDrawingPartitionStatus::Partitioned;
  switch (drawingPartition.status) {
    case ComponentDrawingPartitionStatus::Partitioned:
      break;
    case ComponentDrawingPartitionStatus::CrossDefinitionDependency:
      return fail(ComponentExtractionStatus::CrossDefinitionDependency);
    case ComponentDrawingPartitionStatus::MissingBodyReference:
      return fail(ComponentExtractionStatus::InvalidDependencyGraph);
    case ComponentDrawingPartitionStatus::ResourceFailure:
      return fail(ComponentExtractionStatus::ResourceFailure);
  }
  try {
    ComponentExtractionTransaction transaction{
        address,
        std::move(plan),
        beforeState,
        DefinitionStateId{beforeNext},
        beforeNext,
        beforeNext + 1,
        DefinitionStateId{1},
        2,
        convertedPartRoot,
        std::move(drawingPartition.before),
        std::move(drawingPartition.parentAfter),
        std::move(drawingPartition.child)};
    return {ComponentExtractionStatus::Prepared, std::move(transaction)};
  } catch (...) {
    return fail(ComponentExtractionStatus::ResourceFailure);
  }
}

ComponentExtractionStatus ProductDocument::applyComponentExtraction(
    const ComponentExtractionTransaction& transaction,
    ProductDocumentSuccessor& prepareSuccessor,
    ComponentExtractionFailurePoint failurePoint) noexcept {
  if (productMutationTransactionActive_)
    return ComponentExtractionStatus::Busy;
  const auto prepared = prepareComponentExtraction(transaction.address);
  if (!prepared) return prepared.status;
  if (prepared.transaction->plan != transaction.plan ||
      prepared.transaction->beforeOwnerStateId !=
          transaction.beforeOwnerStateId ||
      prepared.transaction->afterOwnerStateId !=
          transaction.afterOwnerStateId ||
      prepared.transaction->beforeNextStateIdValue !=
          transaction.beforeNextStateIdValue ||
      prepared.transaction->afterNextStateIdValue !=
          transaction.afterNextStateIdValue ||
      prepared.transaction->createdChildStateId !=
          transaction.createdChildStateId ||
      prepared.transaction->createdChildNextStateIdValue !=
          transaction.createdChildNextStateIdValue ||
      prepared.transaction->convertedPartRoot !=
          transaction.convertedPartRoot ||
      prepared.transaction->beforeOwnerDrawings !=
          transaction.beforeOwnerDrawings ||
      prepared.transaction->afterOwnerDrawings !=
          transaction.afterOwnerDrawings ||
      prepared.transaction->createdChildDrawings !=
          transaction.createdChildDrawings)
    return ComponentExtractionStatus::StalePlan;
  ProductMutationGuard mutation(productMutationTransactionActive_);

  const auto* componentOwner = transaction.convertedPartRoot
                                   ? nullptr
                                   : definitions_.findComponent(
                                         transaction.address.ownerDefinitionId);
  const Document* ownerDocument = transaction.convertedPartRoot
                                      ? &rootPartDefinition()->document()
                                      : componentOwner
                                            ? &componentOwner->document()
                                            : nullptr;
  if (!ownerDocument) return ComponentExtractionStatus::StructureMismatch;
  auto staged = ownerDocument->stageComponentExtraction(transaction.plan);
  switch (staged.status) {
    case ComponentExtractionStageStatus::Staged:
      break;
    case ComponentExtractionStageStatus::StalePlan:
    case ComponentExtractionStageStatus::PlanNotApplicable:
      return ComponentExtractionStatus::StalePlan;
    case ComponentExtractionStageStatus::ParentRecomputeFailed:
      return ComponentExtractionStatus::ParentRecomputeFailed;
    case ComponentExtractionStageStatus::ChildRecomputeFailed:
      return ComponentExtractionStatus::ChildRecomputeFailed;
    case ComponentExtractionStageStatus::ResourceFailure:
      return ComponentExtractionStatus::ResourceFailure;
  }

  if (failurePoint == ComponentExtractionFailurePoint::CandidateAllocation)
    return ComponentExtractionStatus::ResourceFailure;
  try {
    std::vector<ComponentDefinition> components;
    components.reserve(definitions_.size() + 1);
    if (transaction.convertedPartRoot) {
      if (!transaction.beforeOwnerDrawings.empty() ||
          !transaction.afterOwnerDrawings.empty() ||
          !transaction.createdChildDrawings.empty())
        return ComponentExtractionStatus::StructureMismatch;
      ComponentDefinition root(rootDefinitionId_,
                               std::move(staged.parentAfter));
      root.stateId_ = transaction.afterOwnerStateId;
      root.nextStateIdValue_ = transaction.afterNextStateIdValue;
      if (root.insert(ComponentOccurrence(
              transaction.address.childOccurrenceId,
              transaction.address.childDefinitionId,
              RigidTransform3d::identity())) !=
          ComponentOccurrenceInsertStatus::Inserted)
        return ComponentExtractionStatus::DuplicateOccurrenceId;
      components.push_back(std::move(root));
    } else {
      for (const DefinitionId id : definitions_.definitionIds()) {
        const ComponentDefinition* source = definitions_.findComponent(id);
        if (!source)
          return ComponentExtractionStatus::UnsupportedRootKind;
        auto document = id == transaction.address.ownerDefinitionId
                            ? std::move(staged.parentAfter)
                            : std::make_unique<Document>(source->document());
        std::vector<ComponentDrawing> drawings =
            id == transaction.address.ownerDefinitionId
                ? transaction.afterOwnerDrawings
                : source->drawings();
        ComponentDefinition clone(id, std::move(document),
                                  std::move(drawings));
        clone.stateId_ = source->stateId_;
        clone.nextStateIdValue_ = source->nextStateIdValue_;
        for (const auto& [occurrenceId, occurrence] :
             source->occurrences()) {
          static_cast<void>(occurrenceId);
          if (clone.insert(ComponentOccurrence(
                  occurrence.id(), occurrence.targetDefinitionId(),
                  occurrence.placement(), occurrence.homePlacement(),
                  occurrence.flags(),
                  occurrence.displayNameUtf8())) !=
              ComponentOccurrenceInsertStatus::Inserted)
            return ComponentExtractionStatus::StructureMismatch;
        }
        if (id == transaction.address.ownerDefinitionId) {
          clone.stateId_ = transaction.afterOwnerStateId;
          clone.nextStateIdValue_ = transaction.afterNextStateIdValue;
          if (clone.insert(ComponentOccurrence(
                  transaction.address.childOccurrenceId,
                  transaction.address.childDefinitionId,
                  RigidTransform3d::identity())) !=
              ComponentOccurrenceInsertStatus::Inserted)
            return ComponentExtractionStatus::DuplicateOccurrenceId;
        }
        components.push_back(std::move(clone));
      }
    }
    std::vector<ComponentDrawing> childDrawings =
        transaction.createdChildDrawings;
    ComponentDefinition child(transaction.address.childDefinitionId,
                              std::move(staged.child),
                              std::move(childDrawings));
    child.stateId_ = transaction.createdChildStateId;
    child.nextStateIdValue_ = transaction.createdChildNextStateIdValue;
    components.push_back(std::move(child));
    auto candidate = [&] {
      detail::ScopedExplicitIdReservationPause reservationPause;
      return createComponentGraph(std::move(components), rootDefinitionId_,
                                  identity_);
    }();
    if (!candidate)
      return candidate.status == ProductDocumentStatus::ResourceFailure
                 ? ComponentExtractionStatus::ResourceFailure
                 : ComponentExtractionStatus::StructureMismatch;
    candidate.product->incarnation_ = incarnation_;
    if (failurePoint == ComponentExtractionFailurePoint::BeforeSuccessor)
      return ComponentExtractionStatus::ResourceFailure;
    const auto successorStatus = prepareSuccessor.prepare(
        *candidate.product, ProductDocumentSuccessorImpact::FullProjection);
    if (successorStatus != ProductDocumentSuccessorStatus::Prepared)
      return successorStatus ==
                     ProductDocumentSuccessorStatus::ResourceFailure
                 ? ComponentExtractionStatus::SuccessorResourceFailure
                 : ComponentExtractionStatus::SuccessorRejected;
    if (failurePoint == ComponentExtractionFailurePoint::BeforeCommit)
      return ComponentExtractionStatus::ResourceFailure;
    definitions_.definitions_.swap(
        candidate.product->definitions_.definitions_);
    return ComponentExtractionStatus::Committed;
  } catch (...) {
    return ComponentExtractionStatus::ResourceFailure;
  }
}

ComponentExtractionStatus ProductDocument::revertComponentExtraction(
    const ComponentExtractionTransaction& transaction,
    ProductDocumentSuccessor& prepareSuccessor,
    ComponentExtractionFailurePoint failurePoint) noexcept {
  if (productMutationTransactionActive_)
    return ComponentExtractionStatus::Busy;
  ProductMutationGuard mutation(productMutationTransactionActive_);
  const auto& address = transaction.address;
  if (address.productIncarnation != incarnation_)
    return ComponentExtractionStatus::ProductIncarnationMismatch;
  if (address.rootDefinitionId != rootDefinitionId_)
    return ComponentExtractionStatus::RootDefinitionMismatch;
  const ComponentDefinition* owner =
      definitions_.findComponent(address.ownerDefinitionId);
  const ComponentDefinition* child =
      definitions_.findComponent(address.childDefinitionId);
  if (!owner || !child)
    return ComponentExtractionStatus::StructureMismatch;
  if (owner->stateId_ != transaction.afterOwnerStateId ||
      owner->nextStateIdValue_ != transaction.afterNextStateIdValue)
    return ComponentExtractionStatus::OwnerStateMismatch;
  if (child->stateId_ != transaction.createdChildStateId ||
      child->nextStateIdValue_ !=
          transaction.createdChildNextStateIdValue)
    return ComponentExtractionStatus::StructureMismatch;
  if (owner->drawings() != transaction.afterOwnerDrawings ||
      child->drawings() != transaction.createdChildDrawings)
    return ComponentExtractionStatus::StructureMismatch;
  if (transaction.convertedPartRoot &&
      (!transaction.beforeOwnerDrawings.empty() ||
       !transaction.afterOwnerDrawings.empty() ||
       !transaction.createdChildDrawings.empty()))
    return ComponentExtractionStatus::StructureMismatch;
  const ComponentOccurrence* occurrence =
      owner->find(address.childOccurrenceId);
  if (!occurrence ||
      occurrence->targetDefinitionId() != address.childDefinitionId ||
      occurrence->placement() != RigidTransform3d::identity() ||
      occurrence->flags() != OccurrenceFlags{} ||
      !occurrence->displayNameUtf8().empty())
    return ComponentExtractionStatus::StructureMismatch;
  if (child->size() != 0 || child->document().bodies().size() != 1 ||
      child->document().bodies().front().id() != address.bodyId ||
      child->document().sketches().size() != transaction.plan.sketches.size() ||
      !child->document().referenceImages().empty())
    return ComponentExtractionStatus::StructureMismatch;
  const Body& extractedBody = child->document().bodies().front();
  if (extractedBody.features().size() != transaction.plan.featureIds.size())
    return ComponentExtractionStatus::StructureMismatch;
  for (std::size_t index = 0; index < transaction.plan.featureIds.size();
       ++index)
    if (extractedBody.features()[index]->id() !=
        transaction.plan.featureIds[index])
      return ComponentExtractionStatus::StructureMismatch;
  for (std::size_t index = 0; index < transaction.plan.sketches.size();
       ++index)
    if (child->document().sketches()[index].id !=
        transaction.plan.sketches[index].id)
      return ComponentExtractionStatus::StructureMismatch;
  if (owner->document().findBody(address.bodyId))
    return ComponentExtractionStatus::StructureMismatch;
  for (const auto& location : transaction.plan.sketches)
    if (owner->document().findSketch(location.id))
      return ComponentExtractionStatus::StructureMismatch;
  for (const auto& frozen : transaction.plan.frozenSketchSupports) {
    const Document& source =
        frozen.movesToChild ? child->document() : owner->document();
    const DocumentSketch* sketch = source.findSketch(frozen.id);
    if (!sketch || sketch->support.type != SketchSupportType::BasePlane)
      return ComponentExtractionStatus::StructureMismatch;
  }
  for (const DefinitionId id : definitions_.definitionIds()) {
    const auto* component = definitions_.findComponent(id);
    if (!component) return ComponentExtractionStatus::UnsupportedRootKind;
    for (const auto& [occurrenceId, candidate] : component->occurrences()) {
      if (candidate.targetDefinitionId() != address.childDefinitionId)
        continue;
      if (id != address.ownerDefinitionId ||
          occurrenceId != address.childOccurrenceId)
        return ComponentExtractionStatus::StructureMismatch;
    }
  }

  if (failurePoint == ComponentExtractionFailurePoint::CandidateAllocation)
    return ComponentExtractionStatus::ResourceFailure;
  try {
    auto restored = std::make_unique<Document>(owner->document());
    if (transaction.plan.originalBodyIndex > restored->bodies_.size())
      return ComponentExtractionStatus::StructureMismatch;
    restored->bodies_.insert(
        restored->bodies_.begin() + static_cast<std::ptrdiff_t>(
                                         transaction.plan.originalBodyIndex),
        extractedBody);
    for (std::size_t index = 0; index < transaction.plan.sketches.size();
         ++index) {
      const auto& location = transaction.plan.sketches[index];
      if (location.originalIndex > restored->sketches_.size())
        return ComponentExtractionStatus::StructureMismatch;
      restored->sketches_.insert(
          restored->sketches_.begin() +
              static_cast<std::ptrdiff_t>(location.originalIndex),
          child->document().sketches()[index]);
    }
    for (const auto& frozen : transaction.plan.frozenSketchSupports) {
      DocumentSketch* sketch = restored->findSketch(frozen.id);
      if (!sketch)
        return ComponentExtractionStatus::StructureMismatch;
      sketch->support = {SketchSupportType::Face, frozen.authoredFace};
      sketch->effectiveSupportAuthoredFace.reset();
      sketch->effectiveSupportFace.reset();
      sketch->supportFailure = {};
      sketch->supportResolved = false;
      sketch->placementDirty = true;
    }
    restored->dependencyError_.clear();
    if (!restored->recomputeDetailed().success)
      return ComponentExtractionStatus::ParentRecomputeFailed;

    std::unique_ptr<ProductDocument> candidateProduct;
    if (transaction.convertedPartRoot) {
      if (definitions_.size() != 2 || owner->size() != 1)
        return ComponentExtractionStatus::StructureMismatch;
      DefinitionRegistry registry;
      PartDefinition root(rootDefinitionId_, std::move(restored));
      root.stateId_ = transaction.beforeOwnerStateId;
      root.nextStateIdValue_ = transaction.beforeNextStateIdValue;
      if (registry.insert(std::move(root)) !=
          DefinitionInsertStatus::Inserted)
        return ComponentExtractionStatus::StructureMismatch;
      auto candidate = [&] {
        detail::ScopedExplicitIdReservationPause reservationPause;
        return create(std::move(registry), rootDefinitionId_, identity_);
      }();
      if (!candidate)
        return candidate.status == ProductDocumentStatus::ResourceFailure
                   ? ComponentExtractionStatus::ResourceFailure
                   : ComponentExtractionStatus::StructureMismatch;
      candidateProduct = std::move(candidate.product);
    } else {
      std::vector<ComponentDefinition> components;
      components.reserve(definitions_.size() - 1);
      for (const DefinitionId id : definitions_.definitionIds()) {
        if (id == address.childDefinitionId) continue;
        const ComponentDefinition* source = definitions_.findComponent(id);
        auto document = id == address.ownerDefinitionId
                            ? std::move(restored)
                            : std::make_unique<Document>(source->document());
        std::vector<ComponentDrawing> drawings =
            id == address.ownerDefinitionId
                ? transaction.beforeOwnerDrawings
                : source->drawings();
        ComponentDefinition clone(id, std::move(document),
                                  std::move(drawings));
        clone.stateId_ = source->stateId_;
        clone.nextStateIdValue_ = source->nextStateIdValue_;
        for (const auto& [occurrenceId, sourceOccurrence] :
             source->occurrences()) {
          if (id == address.ownerDefinitionId &&
              occurrenceId == address.childOccurrenceId)
            continue;
          if (clone.insert(ComponentOccurrence(
                  sourceOccurrence.id(),
                  sourceOccurrence.targetDefinitionId(),
                  sourceOccurrence.placement(),
                  sourceOccurrence.homePlacement(), sourceOccurrence.flags(),
                  sourceOccurrence.displayNameUtf8())) !=
              ComponentOccurrenceInsertStatus::Inserted)
            return ComponentExtractionStatus::StructureMismatch;
        }
        if (id == address.ownerDefinitionId) {
          clone.stateId_ = transaction.beforeOwnerStateId;
          clone.nextStateIdValue_ = transaction.beforeNextStateIdValue;
        }
        components.push_back(std::move(clone));
      }
      auto candidate = [&] {
        detail::ScopedExplicitIdReservationPause reservationPause;
        return createComponentGraph(std::move(components), rootDefinitionId_,
                                    identity_);
      }();
      if (!candidate)
        return candidate.status == ProductDocumentStatus::ResourceFailure
                   ? ComponentExtractionStatus::ResourceFailure
                   : ComponentExtractionStatus::StructureMismatch;
      candidateProduct = std::move(candidate.product);
    }
    candidateProduct->incarnation_ = incarnation_;
    if (failurePoint == ComponentExtractionFailurePoint::BeforeSuccessor)
      return ComponentExtractionStatus::ResourceFailure;
    const auto successorStatus = prepareSuccessor.prepare(
        *candidateProduct, ProductDocumentSuccessorImpact::FullProjection);
    if (successorStatus != ProductDocumentSuccessorStatus::Prepared)
      return successorStatus ==
                     ProductDocumentSuccessorStatus::ResourceFailure
                 ? ComponentExtractionStatus::SuccessorResourceFailure
                 : ComponentExtractionStatus::SuccessorRejected;
    if (failurePoint == ComponentExtractionFailurePoint::BeforeCommit)
      return ComponentExtractionStatus::ResourceFailure;
    definitions_.definitions_.swap(candidateProduct->definitions_.definitions_);
    return ComponentExtractionStatus::Reverted;
  } catch (...) {
    return ComponentExtractionStatus::ResourceFailure;
  }
}

ComponentPlacementPrepareResult ProductDocument::prepareComponentPlacement(
    const ComponentPlacementRequest& request) const noexcept {
  const auto fail = [](ComponentPlacementStatus status) {
    return ComponentPlacementPrepareResult{status, std::nullopt};
  };
  if (productMutationTransactionActive_)
    return fail(ComponentPlacementStatus::Busy);
  if (!request.productIncarnation.isValid() ||
      !request.rootDefinitionId.isValid() ||
      !request.expectedTargetDefinitionId.isValid() ||
      !request.expectedOwnerStateId.isValid())
    return fail(ComponentPlacementStatus::InvalidRequest);
  if (request.productIncarnation != incarnation_)
    return fail(ComponentPlacementStatus::ProductIncarnationMismatch);
  if (request.rootDefinitionId != rootDefinitionId_)
    return fail(ComponentPlacementStatus::RootDefinitionMismatch);
  if (!rootComponentDefinition())
    return fail(ComponentPlacementStatus::RootIsNotComponent);
  if (!request.path)
    return fail(ComponentPlacementStatus::RootCannotMove);
  if (request.path->depth() == 0)
    return fail(ComponentPlacementStatus::InvalidRequest);

  double squaredDelta = 0.0;
  for (const double value : request.worldDeltaMm) {
    if (!std::isfinite(value))
      return fail(ComponentPlacementStatus::NonFiniteDelta);
    if (std::abs(value) > kComponentPlacementMaximumDeltaMm)
      return fail(ComponentPlacementStatus::ValueOutOfRange);
    squaredDelta = std::fma(value, value, squaredDelta);
  }
  if (!std::isfinite(squaredDelta))
    return fail(ComponentPlacementStatus::ValueOutOfRange);

  try {
    const ComponentDefinition* owner = rootComponentDefinition();
    RigidTransform3d parentWorld = RigidTransform3d::identity();
    bool effectiveVisible = true;
    bool effectiveSuppressed = false;
    const auto& entries = request.path->entries();
    for (std::size_t index = 0; index < entries.size(); ++index) {
      const ComponentOccurrence* occurrence = owner->find(entries[index]);
      if (!occurrence)
        return fail(ComponentPlacementStatus::OccurrenceMissing);
      const OccurrenceFlags flags = occurrence->flags();
      effectiveVisible = effectiveVisible && flags.visible;
      effectiveSuppressed = effectiveSuppressed || flags.suppressed;
      const bool terminal = index + 1 == entries.size();
      if (terminal) {
        if (occurrence->targetDefinitionId() !=
            request.expectedTargetDefinitionId)
          return fail(ComponentPlacementStatus::TargetDefinitionMismatch);
        if (!definitions_.findComponent(occurrence->targetDefinitionId()))
          return fail(ComponentPlacementStatus::OccurrenceMissing);
        if (owner->stateId_ != request.expectedOwnerStateId)
          return fail(ComponentPlacementStatus::OwnerStateMismatch);
        if (occurrence->placement() != request.expectedPlacement)
          return fail(ComponentPlacementStatus::PlacementMismatch);
        if (flags.grounded)
          return fail(ComponentPlacementStatus::Grounded);
        if (effectiveSuppressed)
          return fail(ComponentPlacementStatus::Suppressed);
        if (!effectiveVisible)
          return fail(ComponentPlacementStatus::EffectivelyHidden);
        if (squaredDelta <=
            kComponentPlacementNoChangeMm * kComponentPlacementNoChangeMm)
          return fail(ComponentPlacementStatus::NoChange);
        if (owner->nextStateIdValue_ == 0 ||
            owner->nextStateIdValue_ ==
                std::numeric_limits<std::uint64_t>::max())
          return fail(ComponentPlacementStatus::DefinitionStateExhausted);

        const auto localDelta = inverseRotateVector(
            parentWorld.quaternionWxyz(), request.worldDeltaMm);
        auto translation = occurrence->placement().translationMm();
        for (std::size_t axis = 0; axis < translation.size(); ++axis) {
          translation[axis] += localDelta[axis];
          if (!std::isfinite(translation[axis]) ||
              std::abs(translation[axis]) >
                  kComponentPlacementMaximumCoordinateMm)
            return fail(ComponentPlacementStatus::ValueOutOfRange);
        }
        const auto after = RigidTransform3d::create(
            translation, occurrence->placement().quaternionWxyz());
        if (!after)
          return fail(ComponentPlacementStatus::ValueOutOfRange);
        const std::uint64_t beforeNext = owner->nextStateIdValue_;
        return {ComponentPlacementStatus::Prepared,
                ComponentPlacementTransaction{
                    request, owner->id_, owner->stateId_,
                    DefinitionStateId{beforeNext}, beforeNext,
                    beforeNext + 1, occurrence->placement(),
                    *after.transform}};
      }

      const auto composed =
          RigidTransform3d::compose(parentWorld, occurrence->placement());
      if (!composed)
        return fail(ComponentPlacementStatus::ValueOutOfRange);
      parentWorld = *composed.transform;
      owner = definitions_.findComponent(occurrence->targetDefinitionId());
      if (!owner)
        return fail(ComponentPlacementStatus::OccurrenceMissing);
    }
  } catch (...) {
    return fail(ComponentPlacementStatus::ResourceFailure);
  }
  return fail(ComponentPlacementStatus::OccurrenceMissing);
}

ComponentPlacementStatus ProductDocument::applyComponentPlacement(
    const ComponentPlacementTransaction& transaction,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_)
    return ComponentPlacementStatus::Busy;
  const auto prepared = prepareComponentPlacement(transaction.request);
  if (!prepared) return prepared.status;
  if (*prepared.transaction != transaction)
    return ComponentPlacementStatus::PlacementMismatch;
  ProductMutationGuard mutation(productMutationTransactionActive_);
  try {
    ComponentDefinition* owner =
        definitions_.findComponentMutable(rootDefinitionId_);
    const auto& entries = transaction.request.path->entries();
    for (std::size_t index = 0; index + 1 < entries.size(); ++index) {
      const ComponentOccurrence* occurrence = owner->find(entries[index]);
      if (!occurrence) return ComponentPlacementStatus::OccurrenceMissing;
      owner = definitions_.findComponentMutable(
          occurrence->targetDefinitionId());
      if (!owner) return ComponentPlacementStatus::OccurrenceMissing;
    }
    if (owner->id_ != transaction.ownerDefinitionId ||
        owner->stateId_ != transaction.beforeOwnerStateId ||
        owner->nextStateIdValue_ != transaction.beforeNextStateIdValue)
      return ComponentPlacementStatus::OwnerStateMismatch;
    const OccurrenceId occurrenceId = entries.back();
    if (!owner->setPlacement(occurrenceId, transaction.afterPlacement))
      return ComponentPlacementStatus::OccurrenceMissing;
    owner->stateId_ = transaction.afterOwnerStateId;
    owner->nextStateIdValue_ = transaction.afterNextStateIdValue;
    const auto rollback = [&]() noexcept {
      static_cast<void>(owner->setPlacement(occurrenceId,
                                            transaction.beforePlacement));
      owner->stateId_ = transaction.beforeOwnerStateId;
      owner->nextStateIdValue_ = transaction.beforeNextStateIdValue;
    };
    const auto successor = prepareSuccessor.prepare(
        *this, ProductDocumentSuccessorImpact::FullProjection);
    if (successor != ProductDocumentSuccessorStatus::Prepared) {
      rollback();
      return successor == ProductDocumentSuccessorStatus::ResourceFailure
                 ? ComponentPlacementStatus::SuccessorResourceFailure
                 : ComponentPlacementStatus::SuccessorRejected;
    }
    return ComponentPlacementStatus::Committed;
  } catch (const std::bad_alloc&) {
    return ComponentPlacementStatus::ResourceFailure;
  } catch (...) {
    return ComponentPlacementStatus::UnexpectedFailure;
  }
}

ComponentPlacementStatus ProductDocument::revertComponentPlacement(
    const ComponentPlacementTransaction& transaction,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_)
    return ComponentPlacementStatus::Busy;
  const auto& request = transaction.request;
  if (!transaction.ownerDefinitionId.isValid() ||
      !transaction.beforeOwnerStateId.isValid() ||
      !transaction.afterOwnerStateId.isValid() ||
      request.expectedOwnerStateId != transaction.beforeOwnerStateId ||
      request.expectedPlacement != transaction.beforePlacement ||
      transaction.beforePlacement == transaction.afterPlacement ||
      transaction.beforeNextStateIdValue == 0 ||
      transaction.beforeNextStateIdValue ==
          std::numeric_limits<std::uint64_t>::max() ||
      transaction.afterOwnerStateId.value() !=
          transaction.beforeNextStateIdValue ||
      transaction.afterNextStateIdValue !=
          transaction.beforeNextStateIdValue + 1)
    return ComponentPlacementStatus::InvalidRequest;
  if (!request.productIncarnation.isValid() ||
      request.productIncarnation != incarnation_)
    return ComponentPlacementStatus::ProductIncarnationMismatch;
  if (!request.rootDefinitionId.isValid() ||
      request.rootDefinitionId != rootDefinitionId_)
    return ComponentPlacementStatus::RootDefinitionMismatch;
  if (!request.path) return ComponentPlacementStatus::RootCannotMove;
  if (!rootComponentDefinition())
    return ComponentPlacementStatus::RootIsNotComponent;
  ProductMutationGuard mutation(productMutationTransactionActive_);
  try {
    ComponentDefinition* owner =
        definitions_.findComponentMutable(rootDefinitionId_);
    const auto& entries = request.path->entries();
    for (std::size_t index = 0; index + 1 < entries.size(); ++index) {
      const ComponentOccurrence* occurrence = owner->find(entries[index]);
      if (!occurrence) return ComponentPlacementStatus::OccurrenceMissing;
      if (!occurrence->flags().visible)
        return ComponentPlacementStatus::EffectivelyHidden;
      if (occurrence->flags().suppressed)
        return ComponentPlacementStatus::Suppressed;
      owner = definitions_.findComponentMutable(
          occurrence->targetDefinitionId());
      if (!owner) return ComponentPlacementStatus::OccurrenceMissing;
    }
    const OccurrenceId occurrenceId = entries.back();
    const ComponentOccurrence* occurrence = owner->find(occurrenceId);
    if (!occurrence) return ComponentPlacementStatus::OccurrenceMissing;
    if (occurrence->targetDefinitionId() !=
        request.expectedTargetDefinitionId)
      return ComponentPlacementStatus::TargetDefinitionMismatch;
    if (occurrence->flags().grounded)
      return ComponentPlacementStatus::Grounded;
    if (occurrence->flags().suppressed)
      return ComponentPlacementStatus::Suppressed;
    if (!occurrence->flags().visible)
      return ComponentPlacementStatus::EffectivelyHidden;
    if (owner->id_ != transaction.ownerDefinitionId ||
        owner->stateId_ != transaction.afterOwnerStateId ||
        owner->nextStateIdValue_ != transaction.afterNextStateIdValue)
      return ComponentPlacementStatus::OwnerStateMismatch;
    if (occurrence->placement() != transaction.afterPlacement)
      return ComponentPlacementStatus::PlacementMismatch;

    if (!owner->setPlacement(occurrenceId, transaction.beforePlacement))
      return ComponentPlacementStatus::OccurrenceMissing;
    owner->stateId_ = transaction.beforeOwnerStateId;
    owner->nextStateIdValue_ = transaction.beforeNextStateIdValue;
    const auto rollback = [&]() noexcept {
      static_cast<void>(owner->setPlacement(occurrenceId,
                                            transaction.afterPlacement));
      owner->stateId_ = transaction.afterOwnerStateId;
      owner->nextStateIdValue_ = transaction.afterNextStateIdValue;
    };
    const auto successor = prepareSuccessor.prepare(
        *this, ProductDocumentSuccessorImpact::FullProjection);
    if (successor != ProductDocumentSuccessorStatus::Prepared) {
      rollback();
      return successor == ProductDocumentSuccessorStatus::ResourceFailure
                 ? ComponentPlacementStatus::SuccessorResourceFailure
                 : ComponentPlacementStatus::SuccessorRejected;
    }
    return ComponentPlacementStatus::Reverted;
  } catch (const std::bad_alloc&) {
    return ComponentPlacementStatus::ResourceFailure;
  } catch (...) {
    return ComponentPlacementStatus::UnexpectedFailure;
  }
}

ComponentHomePlacementPrepareResult
ProductDocument::prepareComponentHomePlacement(
    const ComponentHomePlacementRequest& request) const noexcept {
  const auto fail = [](ComponentHomePlacementStatus status) {
    return ComponentHomePlacementPrepareResult{status, std::nullopt};
  };
  if (productMutationTransactionActive_)
    return fail(ComponentHomePlacementStatus::Busy);
  if (!request.productIncarnation.isValid() ||
      !request.rootDefinitionId.isValid() ||
      !request.expectedTargetDefinitionId.isValid() ||
      !request.expectedOwnerDefinitionId.isValid() ||
      !request.expectedOwnerStateId.isValid() ||
      request.path.entries().empty())
    return fail(ComponentHomePlacementStatus::InvalidRequest);
  if (request.productIncarnation != incarnation_)
    return fail(ComponentHomePlacementStatus::ProductIncarnationMismatch);
  if (request.rootDefinitionId != rootDefinitionId_)
    return fail(ComponentHomePlacementStatus::RootDefinitionMismatch);

  try {
    const ComponentDefinition* owner = rootComponentDefinition();
    if (!owner) return fail(ComponentHomePlacementStatus::RootIsNotComponent);
    bool effectiveVisible = true;
    bool effectiveSuppressed = false;
    const auto& entries = request.path.entries();
    for (std::size_t index = 0; index < entries.size(); ++index) {
      const ComponentOccurrence* occurrence = owner->find(entries[index]);
      if (!occurrence)
        return fail(ComponentHomePlacementStatus::OccurrenceMissing);
      const OccurrenceFlags flags = occurrence->flags();
      effectiveVisible = effectiveVisible && flags.visible;
      effectiveSuppressed = effectiveSuppressed || flags.suppressed;
      if (index + 1 == entries.size()) {
        if (owner->id() != request.expectedOwnerDefinitionId)
          return fail(ComponentHomePlacementStatus::OwnerDefinitionMismatch);
        if (occurrence->targetDefinitionId() !=
            request.expectedTargetDefinitionId)
          return fail(ComponentHomePlacementStatus::TargetDefinitionMismatch);
        if (!definitions_.findComponent(occurrence->targetDefinitionId()))
          return fail(ComponentHomePlacementStatus::OccurrenceMissing);
        if (owner->stateId_ != request.expectedOwnerStateId)
          return fail(ComponentHomePlacementStatus::OwnerStateMismatch);
        if (occurrence->placement() != request.expectedPlacement)
          return fail(ComponentHomePlacementStatus::PlacementMismatch);
        if (occurrence->homePlacement() != request.expectedHomePlacement)
          return fail(ComponentHomePlacementStatus::HomePlacementMismatch);
        if (occurrence->placement() == occurrence->homePlacement())
          return fail(ComponentHomePlacementStatus::NoChange);
        if (request.operation == ComponentHomePlacementOperation::Restore) {
          if (flags.grounded)
            return fail(ComponentHomePlacementStatus::Grounded);
          if (effectiveSuppressed)
            return fail(ComponentHomePlacementStatus::Suppressed);
          if (!effectiveVisible)
            return fail(ComponentHomePlacementStatus::EffectivelyHidden);
        }
        if (owner->nextStateIdValue_ == 0 ||
            owner->nextStateIdValue_ ==
                std::numeric_limits<std::uint64_t>::max())
          return fail(ComponentHomePlacementStatus::DefinitionStateExhausted);
        const std::uint64_t beforeNext = owner->nextStateIdValue_;
        const RigidTransform3d afterPlacement =
            request.operation == ComponentHomePlacementOperation::Restore
                ? occurrence->homePlacement()
                : occurrence->placement();
        const RigidTransform3d afterHomePlacement =
            request.operation == ComponentHomePlacementOperation::AdoptCurrent
                ? occurrence->placement()
                : occurrence->homePlacement();
        return {ComponentHomePlacementStatus::Prepared,
                ComponentHomePlacementTransaction{
                    request, owner->id_, owner->stateId_,
                    DefinitionStateId{beforeNext}, beforeNext, beforeNext + 1,
                    occurrence->placement(), afterPlacement,
                    occurrence->homePlacement(), afterHomePlacement}};
      }
      owner = definitions_.findComponent(occurrence->targetDefinitionId());
      if (!owner)
        return fail(ComponentHomePlacementStatus::OccurrenceMissing);
    }
  } catch (const std::bad_alloc&) {
    return fail(ComponentHomePlacementStatus::ResourceFailure);
  } catch (...) {
    return fail(ComponentHomePlacementStatus::UnexpectedFailure);
  }
  return fail(ComponentHomePlacementStatus::OccurrenceMissing);
}

ComponentHomePlacementBatchPrepareResult
ProductDocument::prepareAllComponentHomePlacements() const noexcept {
  const auto fail = [](ComponentHomePlacementStatus status) {
    return ComponentHomePlacementBatchPrepareResult{status, {}};
  };
  if (productMutationTransactionActive_)
    return fail(ComponentHomePlacementStatus::Busy);
  if (!incarnation_.isValid() || !rootDefinitionId_.isValid())
    return fail(ComponentHomePlacementStatus::InvalidRequest);
  if (!rootComponentDefinition())
    return fail(ComponentHomePlacementStatus::RootIsNotComponent);

  try {
    const auto traversal =
        expandComponentGraph(definitions_, rootDefinitionId_);
    if (!traversal)
      return fail(ComponentHomePlacementStatus::ResourceFailure);

    struct Candidate final {
      OccurrencePath path;
      DefinitionId ownerDefinitionId;
      DefinitionId targetDefinitionId;
      bool grounded{};
      bool effectivelyVisible{true};
      bool effectivelySuppressed{};
    };
    std::map<std::pair<DefinitionId, OccurrenceId>, Candidate> candidates;
    for (const auto& visit : traversal.visits) {
      if (!visit.path || visit.path->entries().empty()) continue;
      const auto& entries = visit.path->entries();
      const ComponentDefinition* owner = rootComponentDefinition();
      for (std::size_t index = 0; index + 1 < entries.size(); ++index) {
        const auto* occurrence = owner ? owner->find(entries[index]) : nullptr;
        owner = occurrence
                    ? definitions_.findComponent(
                          occurrence->targetDefinitionId())
                    : nullptr;
      }
      if (!owner) return fail(ComponentHomePlacementStatus::OccurrenceMissing);
      const auto* occurrence = owner->find(entries.back());
      if (!occurrence)
        return fail(ComponentHomePlacementStatus::OccurrenceMissing);
      const bool guarded = visit.authoredFlags.grounded ||
                           visit.effectiveSuppressed ||
                           !visit.effectiveVisible;
      const auto key = std::pair{owner->id(), entries.back()};
      const auto found = candidates.find(key);
      const bool foundGuarded =
          found != candidates.end() &&
          (found->second.grounded || found->second.effectivelySuppressed ||
           !found->second.effectivelyVisible);
      if (found == candidates.end() || (foundGuarded && !guarded)) {
        candidates.insert_or_assign(
            key, Candidate{*visit.path, owner->id(),
                           occurrence->targetDefinitionId(),
                           visit.authoredFlags.grounded,
                           visit.effectiveVisible,
                           visit.effectiveSuppressed});
      }
    }

    struct SimulatedOwnerState final {
      DefinitionStateId stateId;
      std::uint64_t nextStateIdValue{};
    };
    std::map<DefinitionId, SimulatedOwnerState> simulatedStates;
    std::vector<ComponentHomePlacementTransaction> transactions;
    transactions.reserve(candidates.size());
    for (const auto& [key, candidate] : candidates) {
      const auto* owner = definitions_.findComponent(candidate.ownerDefinitionId);
      const auto* occurrence = owner ? owner->find(key.second) : nullptr;
      if (!owner || !occurrence)
        return fail(ComponentHomePlacementStatus::OccurrenceMissing);
      if (occurrence->placement() == occurrence->homePlacement()) continue;
      if (candidate.grounded)
        return fail(ComponentHomePlacementStatus::Grounded);
      if (candidate.effectivelySuppressed)
        return fail(ComponentHomePlacementStatus::Suppressed);
      if (!candidate.effectivelyVisible)
        return fail(ComponentHomePlacementStatus::EffectivelyHidden);

      auto [state, inserted] = simulatedStates.try_emplace(
          owner->id(), SimulatedOwnerState{owner->stateId_,
                                           owner->nextStateIdValue_});
      static_cast<void>(inserted);
      if (state->second.nextStateIdValue == 0 ||
          state->second.nextStateIdValue ==
              std::numeric_limits<std::uint64_t>::max())
        return fail(ComponentHomePlacementStatus::DefinitionStateExhausted);
      const auto beforeState = state->second.stateId;
      const auto beforeNext = state->second.nextStateIdValue;
      const auto afterState = DefinitionStateId{beforeNext};
      ComponentHomePlacementRequest request{
          incarnation_, rootDefinitionId_, candidate.path,
          candidate.targetDefinitionId, candidate.ownerDefinitionId,
          beforeState, occurrence->placement(), occurrence->homePlacement(),
          ComponentHomePlacementOperation::Restore};
      transactions.push_back(ComponentHomePlacementTransaction{
          std::move(request), owner->id(), beforeState, afterState, beforeNext,
          beforeNext + 1, occurrence->placement(), occurrence->homePlacement(),
          occurrence->homePlacement(), occurrence->homePlacement()});
      state->second.stateId = afterState;
      state->second.nextStateIdValue = beforeNext + 1;
    }
    if (transactions.empty())
      return fail(ComponentHomePlacementStatus::NoChange);
    return {ComponentHomePlacementStatus::Prepared, std::move(transactions)};
  } catch (const std::bad_alloc&) {
    return fail(ComponentHomePlacementStatus::ResourceFailure);
  } catch (...) {
    return fail(ComponentHomePlacementStatus::UnexpectedFailure);
  }
}

ComponentHomePlacementStatus ProductDocument::applyComponentHomePlacement(
    const ComponentHomePlacementTransaction& transaction,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_)
    return ComponentHomePlacementStatus::Busy;
  const auto prepared = prepareComponentHomePlacement(transaction.request);
  if (!prepared) return prepared.status;
  if (*prepared.transaction != transaction)
    return ComponentHomePlacementStatus::InvalidRequest;
  ProductMutationGuard mutation(productMutationTransactionActive_);
  try {
    ComponentDefinition* owner =
        definitions_.findComponentMutable(rootDefinitionId_);
    const auto& entries = transaction.request.path.entries();
    for (std::size_t index = 0; index + 1 < entries.size(); ++index) {
      const ComponentOccurrence* occurrence = owner->find(entries[index]);
      if (!occurrence)
        return ComponentHomePlacementStatus::OccurrenceMissing;
      owner = definitions_.findComponentMutable(
          occurrence->targetDefinitionId());
      if (!owner) return ComponentHomePlacementStatus::OccurrenceMissing;
    }
    if (owner->id_ != transaction.ownerDefinitionId ||
        owner->stateId_ != transaction.beforeOwnerStateId ||
        owner->nextStateIdValue_ != transaction.beforeNextStateIdValue)
      return ComponentHomePlacementStatus::OwnerStateMismatch;
    const OccurrenceId occurrenceId = entries.back();
    if (!owner->setPlacement(occurrenceId, transaction.afterPlacement) ||
        !owner->setHomePlacement(occurrenceId,
                                 transaction.afterHomePlacement))
      return ComponentHomePlacementStatus::OccurrenceMissing;
    owner->stateId_ = transaction.afterOwnerStateId;
    owner->nextStateIdValue_ = transaction.afterNextStateIdValue;
    const auto rollback = [&]() noexcept {
      static_cast<void>(owner->setPlacement(occurrenceId,
                                            transaction.beforePlacement));
      static_cast<void>(owner->setHomePlacement(
          occurrenceId, transaction.beforeHomePlacement));
      owner->stateId_ = transaction.beforeOwnerStateId;
      owner->nextStateIdValue_ = transaction.beforeNextStateIdValue;
    };
    const auto impact =
        transaction.request.operation ==
                ComponentHomePlacementOperation::AdoptCurrent
            ? ProductDocumentSuccessorImpact::TreeOnly
            : ProductDocumentSuccessorImpact::FullProjection;
    const auto successor = prepareSuccessor.prepare(*this, impact);
    if (successor != ProductDocumentSuccessorStatus::Prepared) {
      rollback();
      return successor == ProductDocumentSuccessorStatus::ResourceFailure
                 ? ComponentHomePlacementStatus::SuccessorResourceFailure
                 : ComponentHomePlacementStatus::SuccessorRejected;
    }
    return ComponentHomePlacementStatus::Committed;
  } catch (const std::bad_alloc&) {
    return ComponentHomePlacementStatus::ResourceFailure;
  } catch (...) {
    return ComponentHomePlacementStatus::UnexpectedFailure;
  }
}

ComponentHomePlacementStatus ProductDocument::applyAllComponentHomePlacements(
    const std::vector<ComponentHomePlacementTransaction>& transactions,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_)
    return ComponentHomePlacementStatus::Busy;
  const auto prepared = prepareAllComponentHomePlacements();
  if (!prepared) return prepared.status;
  if (prepared.transactions != transactions)
    return ComponentHomePlacementStatus::InvalidRequest;

  ProductMutationGuard mutation(productMutationTransactionActive_);
  std::size_t applied = 0;
  const auto ownerFor = [this](const ComponentHomePlacementTransaction& item)
      noexcept -> ComponentDefinition* {
    ComponentDefinition* owner =
        definitions_.findComponentMutable(rootDefinitionId_);
    const auto& entries = item.request.path.entries();
    for (std::size_t index = 0; owner && index + 1 < entries.size(); ++index) {
      const auto* occurrence = owner->find(entries[index]);
      owner = occurrence
                  ? definitions_.findComponentMutable(
                        occurrence->targetDefinitionId())
                  : nullptr;
    }
    return owner;
  };
  const auto rollback = [&]() noexcept {
    while (applied != 0) {
      const auto& item = transactions[--applied];
      auto* owner = ownerFor(item);
      if (!owner) continue;
      const auto occurrenceId = item.request.path.entries().back();
      static_cast<void>(owner->setPlacement(occurrenceId,
                                            item.beforePlacement));
      static_cast<void>(owner->setHomePlacement(
          occurrenceId, item.beforeHomePlacement));
      owner->stateId_ = item.beforeOwnerStateId;
      owner->nextStateIdValue_ = item.beforeNextStateIdValue;
    }
  };
  try {
    for (const auto& item : transactions) {
      auto* owner = ownerFor(item);
      if (!owner || owner->id_ != item.ownerDefinitionId)
        return rollback(), ComponentHomePlacementStatus::OccurrenceMissing;
      const auto occurrenceId = item.request.path.entries().back();
      const auto* occurrence = owner->find(occurrenceId);
      if (!occurrence)
        return rollback(), ComponentHomePlacementStatus::OccurrenceMissing;
      if (owner->stateId_ != item.beforeOwnerStateId ||
          owner->nextStateIdValue_ != item.beforeNextStateIdValue)
        return rollback(), ComponentHomePlacementStatus::OwnerStateMismatch;
      if (occurrence->placement() != item.beforePlacement ||
          occurrence->homePlacement() != item.beforeHomePlacement)
        return rollback(), ComponentHomePlacementStatus::PlacementMismatch;
      if (!owner->setPlacement(occurrenceId, item.afterPlacement) ||
          !owner->setHomePlacement(occurrenceId, item.afterHomePlacement))
        return rollback(), ComponentHomePlacementStatus::OccurrenceMissing;
      owner->stateId_ = item.afterOwnerStateId;
      owner->nextStateIdValue_ = item.afterNextStateIdValue;
      ++applied;
    }
    const auto successor = prepareSuccessor.prepare(
        *this, ProductDocumentSuccessorImpact::FullProjection);
    if (successor != ProductDocumentSuccessorStatus::Prepared) {
      rollback();
      return successor == ProductDocumentSuccessorStatus::ResourceFailure
                 ? ComponentHomePlacementStatus::SuccessorResourceFailure
                 : ComponentHomePlacementStatus::SuccessorRejected;
    }
    return ComponentHomePlacementStatus::Committed;
  } catch (const std::bad_alloc&) {
    rollback();
    return ComponentHomePlacementStatus::ResourceFailure;
  } catch (...) {
    rollback();
    return ComponentHomePlacementStatus::UnexpectedFailure;
  }
}

ComponentHomePlacementStatus ProductDocument::revertComponentHomePlacement(
    const ComponentHomePlacementTransaction& transaction,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_)
    return ComponentHomePlacementStatus::Busy;
  const auto& request = transaction.request;
  if (!transaction.ownerDefinitionId.isValid() ||
      !transaction.beforeOwnerStateId.isValid() ||
      !transaction.afterOwnerStateId.isValid() ||
      request.expectedOwnerDefinitionId != transaction.ownerDefinitionId ||
      request.expectedOwnerStateId != transaction.beforeOwnerStateId ||
      request.expectedPlacement != transaction.beforePlacement ||
      request.expectedHomePlacement != transaction.beforeHomePlacement ||
      transaction.beforeNextStateIdValue == 0 ||
      transaction.afterOwnerStateId.value() !=
          transaction.beforeNextStateIdValue ||
      transaction.afterNextStateIdValue !=
          transaction.beforeNextStateIdValue + 1)
    return ComponentHomePlacementStatus::InvalidRequest;
  if (request.productIncarnation != incarnation_)
    return ComponentHomePlacementStatus::ProductIncarnationMismatch;
  if (request.rootDefinitionId != rootDefinitionId_)
    return ComponentHomePlacementStatus::RootDefinitionMismatch;
  ProductMutationGuard mutation(productMutationTransactionActive_);
  try {
    ComponentDefinition* owner =
        definitions_.findComponentMutable(rootDefinitionId_);
    if (!owner) return ComponentHomePlacementStatus::RootIsNotComponent;
    const auto& entries = request.path.entries();
    for (std::size_t index = 0; index + 1 < entries.size(); ++index) {
      const ComponentOccurrence* occurrence = owner->find(entries[index]);
      if (!occurrence)
        return ComponentHomePlacementStatus::OccurrenceMissing;
      owner = definitions_.findComponentMutable(
          occurrence->targetDefinitionId());
      if (!owner) return ComponentHomePlacementStatus::OccurrenceMissing;
    }
    const OccurrenceId occurrenceId = entries.back();
    const ComponentOccurrence* occurrence = owner->find(occurrenceId);
    if (!occurrence)
      return ComponentHomePlacementStatus::OccurrenceMissing;
    if (occurrence->targetDefinitionId() !=
        request.expectedTargetDefinitionId)
      return ComponentHomePlacementStatus::TargetDefinitionMismatch;
    if (owner->id_ != transaction.ownerDefinitionId)
      return ComponentHomePlacementStatus::OwnerDefinitionMismatch;
    if (owner->stateId_ != transaction.afterOwnerStateId ||
        owner->nextStateIdValue_ != transaction.afterNextStateIdValue)
      return ComponentHomePlacementStatus::OwnerStateMismatch;
    if (occurrence->placement() != transaction.afterPlacement)
      return ComponentHomePlacementStatus::PlacementMismatch;
    if (occurrence->homePlacement() != transaction.afterHomePlacement)
      return ComponentHomePlacementStatus::HomePlacementMismatch;

    if (!owner->setPlacement(occurrenceId, transaction.beforePlacement) ||
        !owner->setHomePlacement(occurrenceId,
                                 transaction.beforeHomePlacement))
      return ComponentHomePlacementStatus::OccurrenceMissing;
    owner->stateId_ = transaction.beforeOwnerStateId;
    owner->nextStateIdValue_ = transaction.beforeNextStateIdValue;
    const auto rollback = [&]() noexcept {
      static_cast<void>(owner->setPlacement(occurrenceId,
                                            transaction.afterPlacement));
      static_cast<void>(owner->setHomePlacement(
          occurrenceId, transaction.afterHomePlacement));
      owner->stateId_ = transaction.afterOwnerStateId;
      owner->nextStateIdValue_ = transaction.afterNextStateIdValue;
    };
    const auto impact =
        request.operation == ComponentHomePlacementOperation::AdoptCurrent
            ? ProductDocumentSuccessorImpact::TreeOnly
            : ProductDocumentSuccessorImpact::FullProjection;
    const auto successor = prepareSuccessor.prepare(*this, impact);
    if (successor != ProductDocumentSuccessorStatus::Prepared) {
      rollback();
      return successor == ProductDocumentSuccessorStatus::ResourceFailure
                 ? ComponentHomePlacementStatus::SuccessorResourceFailure
                 : ComponentHomePlacementStatus::SuccessorRejected;
    }
    return ComponentHomePlacementStatus::Reverted;
  } catch (const std::bad_alloc&) {
    return ComponentHomePlacementStatus::ResourceFailure;
  } catch (...) {
    return ComponentHomePlacementStatus::UnexpectedFailure;
  }
}

ComponentHomePlacementStatus ProductDocument::revertAllComponentHomePlacements(
    const std::vector<ComponentHomePlacementTransaction>& transactions,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_)
    return ComponentHomePlacementStatus::Busy;
  if (transactions.empty())
    return ComponentHomePlacementStatus::InvalidRequest;
  for (const auto& item : transactions) {
    const auto& request = item.request;
    if (!item.ownerDefinitionId.isValid() ||
        !item.beforeOwnerStateId.isValid() ||
        !item.afterOwnerStateId.isValid() ||
        !request.productIncarnation.isValid() ||
        !request.rootDefinitionId.isValid() ||
        request.path.entries().empty() ||
        request.operation != ComponentHomePlacementOperation::Restore ||
        request.expectedOwnerDefinitionId != item.ownerDefinitionId ||
        request.expectedOwnerStateId != item.beforeOwnerStateId ||
        request.expectedPlacement != item.beforePlacement ||
        request.expectedHomePlacement != item.beforeHomePlacement ||
        item.beforePlacement == item.beforeHomePlacement ||
        item.afterPlacement != item.beforeHomePlacement ||
        item.afterHomePlacement != item.beforeHomePlacement ||
        item.beforeNextStateIdValue == 0 ||
        item.afterOwnerStateId.value() != item.beforeNextStateIdValue ||
        item.afterNextStateIdValue != item.beforeNextStateIdValue + 1)
      return ComponentHomePlacementStatus::InvalidRequest;
    if (request.productIncarnation != incarnation_)
      return ComponentHomePlacementStatus::ProductIncarnationMismatch;
    if (request.rootDefinitionId != rootDefinitionId_)
      return ComponentHomePlacementStatus::RootDefinitionMismatch;
  }
  ProductMutationGuard mutation(productMutationTransactionActive_);
  std::size_t reverted = 0;
  const auto ownerFor = [this](const ComponentHomePlacementTransaction& item)
      noexcept -> ComponentDefinition* {
    ComponentDefinition* owner =
        definitions_.findComponentMutable(rootDefinitionId_);
    const auto& entries = item.request.path.entries();
    for (std::size_t index = 0; owner && index + 1 < entries.size(); ++index) {
      const auto* occurrence = owner->find(entries[index]);
      owner = occurrence
                  ? definitions_.findComponentMutable(
                        occurrence->targetDefinitionId())
                  : nullptr;
    }
    return owner;
  };
  const auto rollback = [&]() noexcept {
    const std::size_t begin = transactions.size() - reverted;
    for (std::size_t index = begin; index < transactions.size(); ++index) {
      const auto& item = transactions[index];
      auto* owner = ownerFor(item);
      if (!owner) continue;
      const auto occurrenceId = item.request.path.entries().back();
      static_cast<void>(owner->setPlacement(occurrenceId,
                                            item.afterPlacement));
      static_cast<void>(owner->setHomePlacement(
          occurrenceId, item.afterHomePlacement));
      owner->stateId_ = item.afterOwnerStateId;
      owner->nextStateIdValue_ = item.afterNextStateIdValue;
    }
    reverted = 0;
  };
  try {
    for (auto iterator = transactions.rbegin();
         iterator != transactions.rend(); ++iterator) {
      const auto& item = *iterator;
      if (item.request.productIncarnation != incarnation_ ||
          item.request.rootDefinitionId != rootDefinitionId_ ||
          item.request.operation != ComponentHomePlacementOperation::Restore ||
          item.request.path.entries().empty())
        return rollback(), ComponentHomePlacementStatus::InvalidRequest;
      auto* owner = ownerFor(item);
      if (!owner || owner->id_ != item.ownerDefinitionId)
        return rollback(), ComponentHomePlacementStatus::OccurrenceMissing;
      const auto occurrenceId = item.request.path.entries().back();
      const auto* occurrence = owner->find(occurrenceId);
      if (!occurrence)
        return rollback(), ComponentHomePlacementStatus::OccurrenceMissing;
      if (occurrence->targetDefinitionId() !=
          item.request.expectedTargetDefinitionId)
        return rollback(),
               ComponentHomePlacementStatus::TargetDefinitionMismatch;
      if (owner->stateId_ != item.afterOwnerStateId ||
          owner->nextStateIdValue_ != item.afterNextStateIdValue)
        return rollback(), ComponentHomePlacementStatus::OwnerStateMismatch;
      if (occurrence->placement() != item.afterPlacement ||
          occurrence->homePlacement() != item.afterHomePlacement)
        return rollback(), ComponentHomePlacementStatus::PlacementMismatch;
      if (!owner->setPlacement(occurrenceId, item.beforePlacement) ||
          !owner->setHomePlacement(occurrenceId, item.beforeHomePlacement))
        return rollback(), ComponentHomePlacementStatus::OccurrenceMissing;
      owner->stateId_ = item.beforeOwnerStateId;
      owner->nextStateIdValue_ = item.beforeNextStateIdValue;
      ++reverted;
    }
    const auto successor = prepareSuccessor.prepare(
        *this, ProductDocumentSuccessorImpact::FullProjection);
    if (successor != ProductDocumentSuccessorStatus::Prepared) {
      rollback();
      return successor == ProductDocumentSuccessorStatus::ResourceFailure
                 ? ComponentHomePlacementStatus::SuccessorResourceFailure
                 : ComponentHomePlacementStatus::SuccessorRejected;
    }
    return ComponentHomePlacementStatus::Reverted;
  } catch (const std::bad_alloc&) {
    rollback();
    return ComponentHomePlacementStatus::ResourceFailure;
  } catch (...) {
    rollback();
    return ComponentHomePlacementStatus::UnexpectedFailure;
  }
}

ComponentVisibilityStatus ProductDocument::setComponentOccurrenceVisible(
    const OccurrencePath& path, bool visible,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_)
    return ComponentVisibilityStatus::Busy;
  if (path.entries().empty() ||
      path.depth() > kAssemblyGraphMaximumPathDepth)
    return ComponentVisibilityStatus::InvalidPath;
  ComponentDefinition* owner =
      definitions_.findComponentMutable(rootDefinitionId_);
  if (!owner) return ComponentVisibilityStatus::RootIsNotComponent;
  ProductMutationGuard mutation(productMutationTransactionActive_);
  const auto& entries = path.entries();
  for (std::size_t index = 0; index + 1 < entries.size(); ++index) {
    const ComponentOccurrence* occurrence = owner->find(entries[index]);
    if (!occurrence) return ComponentVisibilityStatus::OccurrenceMissing;
    owner = definitions_.findComponentMutable(
        occurrence->targetDefinitionId());
    if (!owner) return ComponentVisibilityStatus::OccurrenceMissing;
  }
  const OccurrenceId occurrenceId = entries.back();
  const ComponentOccurrence* occurrence = owner->find(occurrenceId);
  if (!occurrence) return ComponentVisibilityStatus::OccurrenceMissing;
  if (occurrence->flags().visible == visible)
    return ComponentVisibilityStatus::NoChange;
  if (owner->nextStateIdValue_ == 0 ||
      owner->nextStateIdValue_ == std::numeric_limits<std::uint64_t>::max())
    return ComponentVisibilityStatus::StateIdExhausted;

  const DefinitionStateId oldState = owner->stateId_;
  const std::uint64_t oldNext = owner->nextStateIdValue_;
  if (!owner->setVisible(occurrenceId, visible))
    return ComponentVisibilityStatus::OccurrenceMissing;
  owner->stateId_ = DefinitionStateId{oldNext};
  const auto rollback = [&]() noexcept {
    static_cast<void>(owner->setVisible(occurrenceId, !visible));
    owner->stateId_ = oldState;
    owner->nextStateIdValue_ = oldNext;
  };
  const auto successor = prepareSuccessor.prepare(
      *this, ProductDocumentSuccessorImpact::FullProjection);
  if (successor != ProductDocumentSuccessorStatus::Prepared) {
    rollback();
    return successor == ProductDocumentSuccessorStatus::ResourceFailure
               ? ComponentVisibilityStatus::SuccessorResourceFailure
               : ComponentVisibilityStatus::SuccessorRejected;
  }
  ++owner->nextStateIdValue_;
  return ComponentVisibilityStatus::Committed;
}

ComponentOccurrenceEditStatus ProductDocument::renameComponentOccurrence(
    const OccurrencePath& path, std::string displayNameUtf8,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_)
    return ComponentOccurrenceEditStatus::Busy;
  ProductMutationGuard transaction(productMutationTransactionActive_);
  if (path.depth() == 0 ||
      path.depth() > kAssemblyGraphMaximumPathDepth)
    return ComponentOccurrenceEditStatus::InvalidPath;
  if (validateOccurrenceDisplayNameUtf8(displayNameUtf8) !=
      OccurrenceDisplayNameStatus::Valid)
    return ComponentOccurrenceEditStatus::InvalidName;
  ComponentDefinition* owner =
      definitions_.findComponentMutable(rootDefinitionId_);
  if (!owner) return ComponentOccurrenceEditStatus::RootIsNotComponent;
  for (std::size_t index = 0; index + 1 < path.depth(); ++index) {
    const ComponentOccurrence* occurrence =
        owner->find(path.entries()[index]);
    if (!occurrence) return ComponentOccurrenceEditStatus::OccurrenceMissing;
    owner = definitions_.findComponentMutable(
        occurrence->targetDefinitionId());
    if (!owner) return ComponentOccurrenceEditStatus::OccurrenceMissing;
  }
  const OccurrenceId occurrenceId = path.entries().back();
  const ComponentOccurrence* occurrence = owner->find(occurrenceId);
  if (!occurrence) return ComponentOccurrenceEditStatus::OccurrenceMissing;
  if (occurrence->displayNameUtf8() == displayNameUtf8)
    return ComponentOccurrenceEditStatus::NoChange;
  if (owner->nextStateIdValue_ == 0 ||
      owner->nextStateIdValue_ == std::numeric_limits<std::uint64_t>::max())
    return ComponentOccurrenceEditStatus::StateIdExhausted;
  try {
    if (!owner->swapDisplayName(occurrenceId, displayNameUtf8))
      return ComponentOccurrenceEditStatus::OccurrenceMissing;
    const auto prepared = prepareSuccessor.prepare(
        *this, ProductDocumentSuccessorImpact::TreeOnly);
    if (prepared != ProductDocumentSuccessorStatus::Prepared) {
      static_cast<void>(
          owner->swapDisplayName(occurrenceId, displayNameUtf8));
      return prepared == ProductDocumentSuccessorStatus::ResourceFailure
                 ? ComponentOccurrenceEditStatus::SuccessorResourceFailure
                 : ComponentOccurrenceEditStatus::SuccessorRejected;
    }
    owner->stateId_ = DefinitionStateId{owner->nextStateIdValue_++};
    return ComponentOccurrenceEditStatus::Committed;
  } catch (...) {
    return ComponentOccurrenceEditStatus::ResourceFailure;
  }
}

ComponentOccurrenceEditStatus ProductDocument::removeComponentOccurrence(
    const OccurrencePath& path,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_)
    return ComponentOccurrenceEditStatus::Busy;
  ProductMutationGuard transaction(productMutationTransactionActive_);
  if (path.depth() == 0 ||
      path.depth() > kAssemblyGraphMaximumPathDepth)
    return ComponentOccurrenceEditStatus::InvalidPath;
  ComponentDefinition* owner =
      definitions_.findComponentMutable(rootDefinitionId_);
  if (!owner) return ComponentOccurrenceEditStatus::RootIsNotComponent;
  for (std::size_t index = 0; index + 1 < path.depth(); ++index) {
    const ComponentOccurrence* occurrence =
        owner->find(path.entries()[index]);
    if (!occurrence) return ComponentOccurrenceEditStatus::OccurrenceMissing;
    owner = definitions_.findComponentMutable(
        occurrence->targetDefinitionId());
    if (!owner) return ComponentOccurrenceEditStatus::OccurrenceMissing;
  }
  if (owner->nextStateIdValue_ == 0 ||
      owner->nextStateIdValue_ == std::numeric_limits<std::uint64_t>::max())
    return ComponentOccurrenceEditStatus::StateIdExhausted;
  auto removed = owner->extract(path.entries().back());
  if (removed.empty())
    return ComponentOccurrenceEditStatus::OccurrenceMissing;
  const auto prepared = prepareSuccessor.prepare(
      *this, ProductDocumentSuccessorImpact::FullProjection);
  if (prepared != ProductDocumentSuccessorStatus::Prepared) {
    owner->restore(std::move(removed));
    return prepared == ProductDocumentSuccessorStatus::ResourceFailure
               ? ComponentOccurrenceEditStatus::SuccessorResourceFailure
               : ComponentOccurrenceEditStatus::SuccessorRejected;
  }
  owner->stateId_ = DefinitionStateId{owner->nextStateIdValue_++};
  return ComponentOccurrenceEditStatus::Committed;
}

std::unique_ptr<Document> ProductDocument::releaseRootDocument() && noexcept {
  auto* root = definitions_.findPartMutable(rootDefinitionId_);
  if (!root) return {};
  return std::move(*root).releaseDocument();
}

DefinitionInsertStatus ProductDocument::insert(PartDefinition&& definition) {
  if (productMutationTransactionActive_)
    return DefinitionInsertStatus::Busy;
  return definitions_.insert(std::move(definition));
}

DefinitionInsertStatus ProductDocument::insert(
    AssemblyDefinition&&) {
  if (productMutationTransactionActive_)
    return DefinitionInsertStatus::Busy;
  return DefinitionInsertStatus::UnsupportedNonRootAssemblyDefinition;
}

ProductDocumentRemoveStatus ProductDocument::remove(DefinitionId id) {
  if (productMutationTransactionActive_)
    return ProductDocumentRemoveStatus::Busy;
  if (!definitions_.contains(id)) return ProductDocumentRemoveStatus::Missing;
  if (id == rootDefinitionId_)
    return ProductDocumentRemoveStatus::RootProtected;
  if (rootDefinitionKind() == DefinitionKind::Assembly)
    return ProductDocumentRemoveStatus::DefinitionRemovalUnsupported;
  const auto erased = definitions_.erase(id);
  return erased == DefinitionEraseStatus::Erased
             ? ProductDocumentRemoveStatus::Removed
             : ProductDocumentRemoveStatus::Missing;
}

ProductOccurrenceMutationStatus ProductDocument::insertRootOccurrence(
    ComponentOccurrence&& occurrence) {
  if (productMutationTransactionActive_)
    return ProductOccurrenceMutationStatus::Busy;
  auto* assembly = definitions_.findAssemblyMutable(rootDefinitionId_);
  if (!assembly)
    return ProductOccurrenceMutationStatus::RootIsNotAssembly;
  if (assembly->find(occurrence.id()) != nullptr)
    return ProductOccurrenceMutationStatus::DuplicateOccurrenceId;
  if (definitions_.findPart(occurrence.targetDefinitionId()) == nullptr) {
    if (definitions_.findAssembly(occurrence.targetDefinitionId()) != nullptr)
      return ProductOccurrenceMutationStatus::UnsupportedAssemblyTarget;
    return ProductOccurrenceMutationStatus::DanglingTargetDefinition;
  }
  return assembly->insert(std::move(occurrence)) ==
                 AssemblyOccurrenceInsertStatus::Inserted
             ? ProductOccurrenceMutationStatus::Committed
             : ProductOccurrenceMutationStatus::DuplicateOccurrenceId;
}

ProductOccurrenceMutationStatus ProductDocument::removeRootOccurrence(
    const OccurrencePath& path) {
  if (productMutationTransactionActive_)
    return ProductOccurrenceMutationStatus::Busy;
  auto* assembly = definitions_.findAssemblyMutable(rootDefinitionId_);
  if (!assembly)
    return ProductOccurrenceMutationStatus::RootIsNotAssembly;
  if (path.depth() != 1)
    return ProductOccurrenceMutationStatus::UnsupportedPathDepth;
  return assembly->erase(path.entries().front()) ==
                 AssemblyOccurrenceEraseStatus::Erased
             ? ProductOccurrenceMutationStatus::Committed
             : ProductOccurrenceMutationStatus::OccurrenceMissing;
}

ProductOccurrenceMutationStatus ProductDocument::setRootOccurrencePlacement(
    const OccurrencePath& path, RigidTransform3d placement) noexcept {
  if (productMutationTransactionActive_)
    return ProductOccurrenceMutationStatus::Busy;
  auto* assembly = definitions_.findAssemblyMutable(rootDefinitionId_);
  if (!assembly)
    return ProductOccurrenceMutationStatus::RootIsNotAssembly;
  if (path.depth() != 1)
    return ProductOccurrenceMutationStatus::UnsupportedPathDepth;
  const auto* occurrence = assembly->find(path.entries().front());
  if (!occurrence)
    return ProductOccurrenceMutationStatus::OccurrenceMissing;
  if (occurrence->flags().grounded)
    return ProductOccurrenceMutationStatus::OccurrenceGrounded;
  return assembly->setPlacement(path.entries().front(), std::move(placement))
             ? ProductOccurrenceMutationStatus::Committed
             : ProductOccurrenceMutationStatus::OccurrenceMissing;
}

ProductOccurrenceMutationStatus ProductDocument::setRootOccurrenceFlags(
    const OccurrencePath& path, OccurrenceFlags flags) {
  if (productMutationTransactionActive_)
    return ProductOccurrenceMutationStatus::Busy;
  auto* assembly = definitions_.findAssemblyMutable(rootDefinitionId_);
  if (!assembly)
    return ProductOccurrenceMutationStatus::RootIsNotAssembly;
  if (path.depth() != 1)
    return ProductOccurrenceMutationStatus::UnsupportedPathDepth;
  return assembly->setFlags(path.entries().front(), flags)
             ? ProductOccurrenceMutationStatus::Committed
             : ProductOccurrenceMutationStatus::OccurrenceMissing;
}

std::optional<OccurrenceId> ProductDocument::lowestFreeRootOccurrenceId()
    const noexcept {
  const auto* assembly = rootAssemblyDefinition();
  if (!assembly || assembly->size() >= kMaximumRootOccurrences)
    return std::nullopt;
  for (std::uint64_t value = 1; value <= kMaximumRootOccurrences; ++value) {
    const OccurrenceId candidate{value};
    if (!assembly->find(candidate)) return candidate;
  }
  return std::nullopt;
}

FlatOccurrenceResolveResult ProductDocument::resolveRootOccurrence(
    const OccurrencePath& path) const noexcept {
  const auto* assembly = rootAssemblyDefinition();
  if (!assembly)
    return {FlatOccurrenceResolveStatus::RootIsNotAssembly, nullptr};
  return assembly->resolve(path);
}

RecursiveOccurrenceResolveResult ProductDocument::resolveOccurrence(
    const OccurrencePath& path) const noexcept {
  if (path.depth() == 0)
    return {RecursiveOccurrenceResolveStatus::EmptyPath};
  if (path.depth() > kAssemblyGraphMaximumPathDepth)
    return {RecursiveOccurrenceResolveStatus::UnsupportedDepth};
  const AssemblyDefinition* owner = rootAssemblyDefinition();
  if (!owner)
    return {RecursiveOccurrenceResolveStatus::RootIsNotAssembly};
  for (std::size_t index = 0; index < path.depth(); ++index) {
    const ComponentOccurrence* occurrence = owner->find(path.entries()[index]);
    if (!occurrence)
      return {RecursiveOccurrenceResolveStatus::OccurrenceMissing};
    const DefinitionId target = occurrence->targetDefinitionId();
    const PartDefinition* part = definitions_.findPart(target);
    const AssemblyDefinition* assembly = definitions_.findAssembly(target);
    if (!part && !assembly)
      return {RecursiveOccurrenceResolveStatus::DanglingTarget};
    if (index + 1 == path.depth())
      return {RecursiveOccurrenceResolveStatus::Resolved, owner, occurrence,
              part, assembly};
    if (part)
      return {RecursiveOccurrenceResolveStatus::IntermediateTargetIsPart};
    owner = assembly;
  }
  return {RecursiveOccurrenceResolveStatus::OccurrenceMissing};
}

EmbeddedPartInsertStatus ProductDocument::applyEmbeddedPartInsert(
    EmbeddedPartInsertPayload& payload) noexcept {
  AcceptProductDocumentSuccessor successor;
  return applyEmbeddedPartInsert(payload, successor);
}

EmbeddedPartInsertStatus ProductDocument::applyEmbeddedPartInsert(
    EmbeddedPartInsertPayload& payload,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  return applyEmbeddedPartInsert(payload, prepareSuccessor,
                                 EmbeddedPartInsertFailurePoint::None);
}

EmbeddedPartInsertStatus ProductDocument::applyEmbeddedPartInsert(
    EmbeddedPartInsertPayload& payload,
    ProductDocumentSuccessor& prepareSuccessor,
    EmbeddedPartInsertFailurePoint failurePoint) noexcept {
  if (productMutationTransactionActive_)
    return EmbeddedPartInsertStatus::Busy;
  ProductMutationGuard transaction(productMutationTransactionActive_);
  auto* assembly = definitions_.findAssemblyMutable(rootDefinitionId_);
  if (!assembly) return EmbeddedPartInsertStatus::RootIsNotAssembly;
  if (!payload.definitionId.isValid() || !payload.occurrenceId.isValid() ||
      !payload.document ||
      validateOccurrenceDisplayNameUtf8(payload.displayNameUtf8, true) !=
          OccurrenceDisplayNameStatus::Valid)
    return EmbeddedPartInsertStatus::InvalidPayload;
  if (definitions_.contains(payload.definitionId))
    return EmbeddedPartInsertStatus::DuplicateDefinitionId;
  if (assembly->find(payload.occurrenceId))
    return EmbeddedPartInsertStatus::DuplicateOccurrenceId;

  // Allocate both map nodes with a disposable placeholder while payload still
  // owns the authored graph. Only node-handle insertion and unique_ptr moves
  // happen after this preflight; both are non-throwing. Thus no allocator can
  // ever strand or destroy payload.document.
  DefinitionRegistry stagedDefinitions;
  AssemblyDefinition stagedOccurrences(rootDefinitionId_);
  try {
    if (failurePoint ==
        EmbeddedPartInsertFailurePoint::PlaceholderAllocation)
      throw std::bad_alloc();
    auto placeholder = std::make_unique<Document>();
    PartDefinition stagedDefinition(payload.definitionId,
                                    std::move(placeholder));
    if (failurePoint ==
        EmbeddedPartInsertFailurePoint::DefinitionNodeInsert)
      throw std::bad_alloc();
    if (stagedDefinitions.insert(std::move(stagedDefinition)) !=
        DefinitionInsertStatus::Inserted) {
      return EmbeddedPartInsertStatus::DuplicateDefinitionId;
    }
    ComponentOccurrence occurrence(payload.occurrenceId,
                                    payload.definitionId,
                                    payload.placement, payload.flags,
                                    payload.displayNameUtf8);
    if (failurePoint ==
        EmbeddedPartInsertFailurePoint::OccurrenceNodeInsert)
      throw std::bad_alloc();
    if (stagedOccurrences.insert(std::move(occurrence)) !=
        AssemblyOccurrenceInsertStatus::Inserted)
      return EmbeddedPartInsertStatus::DuplicateOccurrenceId;
  } catch (...) {
    return EmbeddedPartInsertStatus::AllocationFailure;
  }

  auto definitionNode =
      stagedDefinitions.definitions_.extract(payload.definitionId);
  auto occurrenceNode =
      stagedOccurrences.occurrences_.extract(payload.occurrenceId);
  // Node-handle insertion into maps with the same allocator does not allocate.
  auto definitionResult =
      definitions_.definitions_.insert(std::move(definitionNode));
  if (!definitionResult.inserted)
    return EmbeddedPartInsertStatus::DuplicateDefinitionId;
  if (failurePoint == EmbeddedPartInsertFailurePoint::DefinitionNodeCommit) {
    stagedDefinitions.definitions_.insert(
        definitions_.definitions_.extract(payload.definitionId));
    return EmbeddedPartInsertStatus::AllocationFailure;
  }
  auto occurrenceResult =
      assembly->occurrences_.insert(std::move(occurrenceNode));
  if (!occurrenceResult.inserted) {
    stagedDefinitions.definitions_.insert(
        definitions_.definitions_.extract(payload.definitionId));
    return EmbeddedPartInsertStatus::DuplicateOccurrenceId;
  }
  if (failurePoint == EmbeddedPartInsertFailurePoint::OccurrenceNodeCommit) {
    stagedOccurrences.occurrences_.insert(
        assembly->occurrences_.extract(payload.occurrenceId));
    stagedDefinitions.definitions_.insert(
        definitions_.definitions_.extract(payload.definitionId));
    return EmbeddedPartInsertStatus::AllocationFailure;
  }

  auto* committedPart =
      definitions_.findPartMutable(payload.definitionId);
  committedPart->document_.swap(payload.document);
  payload.document.reset();
  const ProductDocumentSuccessorStatus successorStatus =
      prepareSuccessor.prepare(*this,
                               ProductDocumentSuccessorImpact::FullProjection);
  if (successorStatus != ProductDocumentSuccessorStatus::Prepared) {
    auto rollbackOccurrence =
        assembly->occurrences_.extract(payload.occurrenceId);
    auto rollbackDefinition =
        definitions_.definitions_.extract(payload.definitionId);
    auto& part = std::get<PartDefinition>(rollbackDefinition.mapped());
    payload.document = std::move(part).releaseDocument();
    static_cast<void>(rollbackOccurrence);
    return successorStatus == ProductDocumentSuccessorStatus::ResourceFailure
               ? EmbeddedPartInsertStatus::SuccessorResourceFailure
               : EmbeddedPartInsertStatus::SuccessorRejected;
  }
  return EmbeddedPartInsertStatus::Committed;
}

EmbeddedPartInsertStatus ProductDocument::extractEmbeddedPartInsert(
    EmbeddedPartInsertPayload& payload) noexcept {
  AcceptProductDocumentSuccessor successor;
  return extractEmbeddedPartInsert(payload, successor);
}

EmbeddedPartInsertStatus ProductDocument::extractEmbeddedPartInsert(
    EmbeddedPartInsertPayload& payload,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_)
    return EmbeddedPartInsertStatus::Busy;
  ProductMutationGuard transaction(productMutationTransactionActive_);
  auto* assembly = definitions_.findAssemblyMutable(rootDefinitionId_);
  if (!assembly) return EmbeddedPartInsertStatus::RootIsNotAssembly;
  if (!payload.definitionId.isValid() || !payload.occurrenceId.isValid() ||
      payload.document)
    return EmbeddedPartInsertStatus::InvalidPayload;
  const auto* occurrence = assembly->find(payload.occurrenceId);
  if (!occurrence || occurrence->targetDefinitionId() != payload.definitionId ||
      occurrence->placement() != payload.placement ||
      occurrence->flags() != payload.flags ||
      occurrence->displayNameUtf8() != payload.displayNameUtf8)
    return EmbeddedPartInsertStatus::GuardMismatch;

  for (const auto& [otherId, other] : assembly->occurrences()) {
    if (otherId != payload.occurrenceId &&
        other.targetDefinitionId() == payload.definitionId)
      return EmbeddedPartInsertStatus::DefinitionReferenced;
  }
  const auto found = definitions_.definitions_.find(payload.definitionId);
  if (found == definitions_.definitions_.end() ||
      std::get_if<PartDefinition>(&found->second) == nullptr)
    return EmbeddedPartInsertStatus::GuardMismatch;

  // Both extracts are allocation-free. We inspect everything first, then
  // remove the occurrence and definition as one non-throwing commit.
  auto occurrenceNode = assembly->occurrences_.extract(payload.occurrenceId);
  auto definitionNode = definitions_.definitions_.extract(payload.definitionId);
  if (occurrenceNode.empty() || definitionNode.empty()) {
    // This cannot happen in this single-threaded model after the guards.
    if (!occurrenceNode.empty())
      assembly->occurrences_.insert(std::move(occurrenceNode));
    if (!definitionNode.empty())
      definitions_.definitions_.insert(std::move(definitionNode));
    return EmbeddedPartInsertStatus::GuardMismatch;
  }
  const ProductDocumentSuccessorStatus successorStatus =
      prepareSuccessor.prepare(*this,
                               ProductDocumentSuccessorImpact::FullProjection);
  if (successorStatus != ProductDocumentSuccessorStatus::Prepared) {
    definitions_.definitions_.insert(std::move(definitionNode));
    assembly->occurrences_.insert(std::move(occurrenceNode));
    return successorStatus == ProductDocumentSuccessorStatus::ResourceFailure
               ? EmbeddedPartInsertStatus::SuccessorResourceFailure
               : EmbeddedPartInsertStatus::SuccessorRejected;
  }
  auto& part = std::get<PartDefinition>(definitionNode.mapped());
  payload.document = std::move(part).releaseDocument();
  return EmbeddedPartInsertStatus::Extracted;
}

DefinitionEditResult ProductDocument::applyDefinitionEdit(
    const DefinitionEditAddress& address, Document& candidate,
    ProductDocumentSuccessor& prepareSuccessor,
    DefinitionEditFailurePoint failurePoint) noexcept {
  return mutateDefinitionEditInternal(address, candidate, prepareSuccessor,
                                      failurePoint);
}

DefinitionEditResult ProductDocument::revertDefinitionEdit(
    const DefinitionEditAddress& address, Document& candidate,
    ProductDocumentSuccessor& prepareSuccessor,
    DefinitionEditFailurePoint failurePoint) noexcept {
  return mutateDefinitionEditInternal(address, candidate, prepareSuccessor,
                                      failurePoint);
}

DefinitionEditResult ProductDocument::mutateDefinitionEditInternal(
    const DefinitionEditAddress& address, Document& candidate,
    ProductDocumentSuccessor& prepareSuccessor,
    DefinitionEditFailurePoint failurePoint) noexcept {
  if (productMutationTransactionActive_)
    return {DefinitionEditStatus::Busy, {}};
  ProductMutationGuard transaction(productMutationTransactionActive_);
  if (!address.productIncarnation.isValid() ||
      !address.rootDefinitionId.isValid() ||
      !address.targetDefinitionId.isValid() ||
      !address.expectedStateId.isValid())
    return {DefinitionEditStatus::InvalidAddress, {}};
  if (address.productIncarnation != incarnation_)
    return {DefinitionEditStatus::ProductIncarnationMismatch, {}};
  if (address.rootDefinitionId != rootDefinitionId_)
    return {DefinitionEditStatus::RootDefinitionMismatch, {}};
  const bool assemblyRoot = rootAssemblyDefinition() != nullptr;
  const bool componentRoot = rootComponentDefinition() != nullptr;
  if (!assemblyRoot && !componentRoot)
    return {DefinitionEditStatus::RootIsNotAssembly, {}};
  PartDefinition* part = assemblyRoot
                             ? definitions_.findPartMutable(
                                   address.targetDefinitionId)
                             : nullptr;
  ComponentDefinition* component = componentRoot
                                       ? definitions_.findComponentMutable(
                                             address.targetDefinitionId)
                                       : nullptr;
  if (!part && !component)
    return {DefinitionEditStatus::DefinitionMissing, {}};
  if (part && part->isLinked())
    return {DefinitionEditStatus::LinkedDefinitionRequiresLifecycle, {}};
  const DefinitionStateId currentState =
      part ? part->stateId_ : component->stateId_;
  std::uint64_t& nextState =
      part ? part->nextStateIdValue_ : component->nextStateIdValue_;
  Document& document = part ? part->document() : component->document();
  if (currentState != address.expectedStateId)
    return {DefinitionEditStatus::DefinitionStateMismatch, {}};
  if (nextState == 0 ||
      nextState == std::numeric_limits<std::uint64_t>::max())
    return {DefinitionEditStatus::ResourceFailure, {}};
  if (failurePoint == DefinitionEditFailurePoint::BeforeRecompute)
    return {DefinitionEditStatus::ResourceFailure, {}};
  try {
    const RecomputeResult recomputed = candidate.recomputeDetailed();
    if (!recomputed.success)
      return {DefinitionEditStatus::RecomputeFailed, {}};
  } catch (const std::bad_alloc&) {
    return {DefinitionEditStatus::ResourceFailure, {}};
  } catch (...) {
    return {DefinitionEditStatus::RecomputeFailed, {}};
  }
  if (failurePoint == DefinitionEditFailurePoint::BeforeCommit)
    return {DefinitionEditStatus::ResourceFailure, {}};

  const DefinitionStateId oldState = currentState;
  const DefinitionStateId proposedState{nextState};
  document.swapCommittedState(candidate);
  if (part)
    part->stateId_ = proposedState;
  else
    component->stateId_ = proposedState;
  ProductDocumentSuccessorStatus successorStatus =
      prepareSuccessor.prepare(*this,
                               ProductDocumentSuccessorImpact::DefinitionProjection);
  if (successorStatus == ProductDocumentSuccessorStatus::Prepared &&
      failurePoint == DefinitionEditFailurePoint::Finalization)
    successorStatus = ProductDocumentSuccessorStatus::ResourceFailure;
  if (successorStatus != ProductDocumentSuccessorStatus::Prepared) {
    document.swapCommittedState(candidate);
    if (part)
      part->stateId_ = oldState;
    else
      component->stateId_ = oldState;
    return {successorStatus == ProductDocumentSuccessorStatus::ResourceFailure
                ? DefinitionEditStatus::SuccessorResourceFailure
                : DefinitionEditStatus::SuccessorRejected,
            {}};
  }
  ++nextState;
  return {DefinitionEditStatus::Committed, proposedState};
}

LinkedPartMutationResult ProductDocument::validateLinkedPartMutation(
    const DefinitionEditAddress& address,
    const LinkedPartBinding& expectedBinding,
    PartDefinition** part) noexcept {
  if (!part || !address.productIncarnation.isValid() ||
      !address.rootDefinitionId.isValid() ||
      !address.targetDefinitionId.isValid() ||
      !address.expectedStateId.isValid())
    return {LinkedPartMutationStatus::InvalidAddress, {}};
  if (address.productIncarnation != incarnation_)
    return {LinkedPartMutationStatus::ProductIncarnationMismatch, {}};
  if (address.rootDefinitionId != rootDefinitionId_)
    return {LinkedPartMutationStatus::RootDefinitionMismatch, {}};
  if (!rootAssemblyDefinition())
    return {LinkedPartMutationStatus::RootIsNotAssembly, {}};
  *part = definitions_.findPartMutable(address.targetDefinitionId);
  if (!*part) return {LinkedPartMutationStatus::DefinitionMissing, {}};
  if ((*part)->stateId_ != address.expectedStateId)
    return {LinkedPartMutationStatus::DefinitionStateMismatch, {}};
  if (!(*part)->binding_)
    return {LinkedPartMutationStatus::DefinitionIsNotLinked, {}};
  if (*(*part)->binding_ != expectedBinding)
    return {LinkedPartMutationStatus::BindingMismatch, {}};
  if ((*part)->nextStateIdValue_ == 0 ||
      (*part)->nextStateIdValue_ ==
          std::numeric_limits<std::uint64_t>::max())
    return {LinkedPartMutationStatus::ResourceFailure, {}};
  return {LinkedPartMutationStatus::Committed, {}};
}

LinkedPartMutationResult ProductDocument::applyLinkedPartReload(
    const DefinitionEditAddress& address,
    const LinkedPartBinding& expectedBinding,
    const LinkedPartBinding& replacementBinding, Document& candidate,
    ProductDocumentSuccessor& prepareSuccessor,
    DefinitionEditFailurePoint failurePoint) noexcept {
  if (productMutationTransactionActive_)
    return {LinkedPartMutationStatus::Busy, {}};
  ProductMutationGuard transaction(productMutationTransactionActive_);
  PartDefinition* part = nullptr;
  const auto validated =
      validateLinkedPartMutation(address, expectedBinding, &part);
  if (validated.status != LinkedPartMutationStatus::Committed)
    return validated;
  if (!replacementBinding.isValid())
    return {LinkedPartMutationStatus::InvalidReplacementBinding, {}};
  if (replacementBinding.key != expectedBinding.key)
    return {LinkedPartMutationStatus::ForbiddenIdentityChange, {}};
  if (replacementBinding.relativePathHintUtf8 !=
      expectedBinding.relativePathHintUtf8)
    return {LinkedPartMutationStatus::ForbiddenLocatorChange, {}};
  if (failurePoint == DefinitionEditFailurePoint::BeforeRecompute)
    return {LinkedPartMutationStatus::ResourceFailure, {}};
  try {
    const RecomputeResult recomputed = candidate.recomputeDetailed();
    if (!recomputed.success)
      return {LinkedPartMutationStatus::RecomputeFailed, {}};
  } catch (const std::bad_alloc&) {
    return {LinkedPartMutationStatus::ResourceFailure, {}};
  } catch (...) {
    return {LinkedPartMutationStatus::RecomputeFailed, {}};
  }
  if (failurePoint == DefinitionEditFailurePoint::BeforeCommit)
    return {LinkedPartMutationStatus::ResourceFailure, {}};

  LinkedPartBinding staged;
  try {
    staged = replacementBinding;
  } catch (...) {
    return {LinkedPartMutationStatus::ResourceFailure, {}};
  }
  const DefinitionStateId oldState = part->stateId_;
  const DefinitionStateId proposedState{part->nextStateIdValue_};
  part->document().swapCommittedState(candidate);
  using std::swap;
  swap(*part->binding_, staged);
  part->stateId_ = proposedState;
  ProductDocumentSuccessorStatus successorStatus = prepareSuccessor.prepare(
      *this, ProductDocumentSuccessorImpact::DefinitionProjection);
  if (successorStatus == ProductDocumentSuccessorStatus::Prepared &&
      failurePoint == DefinitionEditFailurePoint::Finalization)
    successorStatus = ProductDocumentSuccessorStatus::ResourceFailure;
  if (successorStatus != ProductDocumentSuccessorStatus::Prepared) {
    part->document().swapCommittedState(candidate);
    swap(*part->binding_, staged);
    part->stateId_ = oldState;
    return {successorStatus == ProductDocumentSuccessorStatus::ResourceFailure
                ? LinkedPartMutationStatus::SuccessorResourceFailure
                : LinkedPartMutationStatus::SuccessorRejected,
            {}};
  }
  ++part->nextStateIdValue_;
  return {LinkedPartMutationStatus::Committed, proposedState};
}

LinkedPartMutationResult ProductDocument::applyLinkedPartRepair(
    const DefinitionEditAddress& address,
    const LinkedPartBinding& expectedBinding,
    const LinkedPartBinding& replacementBinding,
    ProductDocumentSuccessor& prepareSuccessor,
    DefinitionEditFailurePoint failurePoint) noexcept {
  if (productMutationTransactionActive_)
    return {LinkedPartMutationStatus::Busy, {}};
  ProductMutationGuard transaction(productMutationTransactionActive_);
  PartDefinition* part = nullptr;
  const auto validated =
      validateLinkedPartMutation(address, expectedBinding, &part);
  if (validated.status != LinkedPartMutationStatus::Committed)
    return validated;
  if (!replacementBinding.isValid())
    return {LinkedPartMutationStatus::InvalidReplacementBinding, {}};
  if (replacementBinding.key != expectedBinding.key)
    return {LinkedPartMutationStatus::ForbiddenIdentityChange, {}};
  if (replacementBinding.acceptedHash != expectedBinding.acceptedHash)
    return {LinkedPartMutationStatus::ForbiddenRevisionChange, {}};
  if (replacementBinding.relativePathHintUtf8 ==
      expectedBinding.relativePathHintUtf8)
    return {LinkedPartMutationStatus::NoChange, {}};
  if (failurePoint == DefinitionEditFailurePoint::BeforeRecompute ||
      failurePoint == DefinitionEditFailurePoint::BeforeCommit)
    return {LinkedPartMutationStatus::ResourceFailure, {}};

  LinkedPartBinding staged;
  try {
    staged = replacementBinding;
  } catch (...) {
    return {LinkedPartMutationStatus::ResourceFailure, {}};
  }
  const DefinitionStateId oldState = part->stateId_;
  const DefinitionStateId proposedState{part->nextStateIdValue_};
  using std::swap;
  swap(*part->binding_, staged);
  part->stateId_ = proposedState;
  ProductDocumentSuccessorStatus successorStatus = prepareSuccessor.prepare(
      *this, ProductDocumentSuccessorImpact::TreeOnly);
  if (successorStatus == ProductDocumentSuccessorStatus::Prepared &&
      failurePoint == DefinitionEditFailurePoint::Finalization)
    successorStatus = ProductDocumentSuccessorStatus::ResourceFailure;
  if (successorStatus != ProductDocumentSuccessorStatus::Prepared) {
    swap(*part->binding_, staged);
    part->stateId_ = oldState;
    return {successorStatus == ProductDocumentSuccessorStatus::ResourceFailure
                ? LinkedPartMutationStatus::SuccessorResourceFailure
                : LinkedPartMutationStatus::SuccessorRejected,
            {}};
  }
  ++part->nextStateIdValue_;
  return {LinkedPartMutationStatus::Committed, proposedState};
}

MatePlacementApplyStatus ProductDocument::applyMatePlacementCandidate(
    const MatePlacementCandidate& candidate,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_)
    return MatePlacementApplyStatus::Busy;
  AssemblyDefinition* owner = nullptr;
  try {
  if (!candidate.integrityValid())
    return MatePlacementApplyStatus::InvalidCandidate;
  if (candidate.address_.productIncarnation != incarnation_ ||
      candidate.address_.rootDefinitionId != rootDefinitionId_)
    return MatePlacementApplyStatus::Stale;
  owner = definitions_.findAssemblyMutable(candidate.address_.ownerAssemblyId);
  if (!owner) return MatePlacementApplyStatus::Stale;
  if (owner->mateRegistry().revision() !=
          candidate.address_.mateRegistryRevision ||
      owner->mateRegistry().fingerprint() !=
          candidate.address_.mateRegistryFingerprint)
    return MatePlacementApplyStatus::Stale;
  for (const auto& dependency : candidate.occurrenceDependencies_) {
    const auto* dependencyOwner =
        definitions_.findAssembly(dependency.ownerAssemblyId);
    const auto* occurrence = dependencyOwner
                                 ? dependencyOwner->find(
                                       dependency.occurrenceId)
                                 : nullptr;
    if (!occurrence ||
        occurrence->targetDefinitionId() != dependency.targetDefinitionId ||
        occurrence->placement() != dependency.placement ||
        occurrence->flags() != dependency.flags ||
        occurrence->displayNameUtf8() != dependency.displayNameUtf8)
      return MatePlacementApplyStatus::Stale;
  }
  for (const auto& dependency : candidate.definitionDependencies_) {
    const auto* part = definitions_.findPart(dependency.definitionId);
    if (!part || part->stateId() != dependency.stateId)
      return MatePlacementApplyStatus::Stale;
    if (!dependency.topology) continue;
    const auto* body =
        part->document().findBody(dependency.topology->bodyId);
    const auto snapshot =
        body ? body->topologySnapshot(dependency.topology->featureId)
             : std::nullopt;
    if (!snapshot ||
        snapshot->revision() != dependency.topology->shapeRevision ||
        (snapshot->topologyIndex() != nullptr) !=
            dependency.topology->topologyAvailable ||
        snapshot->v2Authoritative() !=
            dependency.topology->v2Authoritative)
      return MatePlacementApplyStatus::Stale;
  }
  if (candidate.changes_.empty() || !candidate.seed_.isValid())
    return MatePlacementApplyStatus::InvalidCandidate;

  std::set<MateId> selectedMates;
  std::set<OccurrenceId> selectedNodes;
  if (candidate.seed_.seedImmediateOccurrenceId)
    selectedNodes.insert(*candidate.seed_.seedImmediateOccurrenceId);
  if (candidate.seed_.seedMateId) {
    const auto* seed =
        owner->mateRegistry().findMate(*candidate.seed_.seedMateId);
    if (!seed) return MatePlacementApplyStatus::Stale;
    selectedMates.insert(*candidate.seed_.seedMateId);
    selectedNodes.insert(
        assemblyMateFirstOccurrencePath(*seed).entries().front());
    selectedNodes.insert(
        assemblyMateSecondOccurrencePath(*seed).entries().front());
  }
  bool expanded = true;
  while (expanded) {
    expanded = false;
    for (const auto& [id, relation] : owner->mateRegistry().relations()) {
      const auto first =
          assemblyMateFirstOccurrencePath(relation).entries().front();
      const auto second =
          assemblyMateSecondOccurrencePath(relation).entries().front();
      if (!selectedNodes.contains(first) && !selectedNodes.contains(second))
        continue;
      if (selectedMates.insert(id).second) expanded = true;
      if (selectedNodes.insert(first).second) expanded = true;
      if (selectedNodes.insert(second).second) expanded = true;
    }
  }
  if (std::vector<MateId>(selectedMates.begin(), selectedMates.end()) !=
      candidate.componentMateIds_)
    return MatePlacementApplyStatus::Stale;
  std::vector<OccurrenceId> candidateNodes;
  candidateNodes.reserve(candidate.changes_.size());
  for (const auto& change : candidate.changes_)
    candidateNodes.push_back(change.before.occurrenceId);
  if (candidateNodes !=
      std::vector<OccurrenceId>(selectedNodes.begin(), selectedNodes.end()))
    return MatePlacementApplyStatus::InvalidCandidate;

  OccurrenceId previous;
  for (const auto& change : candidate.changes_) {
    if (!change.before.occurrenceId.isValid() ||
        (previous.isValid() && !(previous < change.before.occurrenceId)))
      return MatePlacementApplyStatus::InvalidCandidate;
    previous = change.before.occurrenceId;
    const auto* occurrence = owner->find(change.before.occurrenceId);
    if (!occurrence ||
        occurrence->targetDefinitionId() != change.before.targetDefinitionId ||
        occurrence->placement() != change.before.placement ||
        occurrence->flags() != change.before.flags)
      return MatePlacementApplyStatus::Stale;
    const auto canonical = RigidTransform3d::create(
        change.afterPlacement.translationMm(),
        change.afterPlacement.quaternionWxyz());
    if (!canonical || *canonical.transform != change.afterPlacement)
      return MatePlacementApplyStatus::InvalidCandidate;
    if (change.before.flags.suppressed ||
        (change.before.flags.grounded &&
         change.afterPlacement != change.before.placement))
      return MatePlacementApplyStatus::InvalidCandidate;
  }
  } catch (const std::bad_alloc&) {
    return MatePlacementApplyStatus::ResourceFailure;
  } catch (const Standard_Failure&) {
    return MatePlacementApplyStatus::Failed;
  } catch (...) {
    return MatePlacementApplyStatus::InvalidCandidate;
  }

  ProductMutationGuard transaction(productMutationTransactionActive_);
  std::size_t changed = 0;
  const auto rollback = [&]() noexcept {
    while (changed > 0) {
      --changed;
      const auto& before = candidate.changes_[changed].before;
      static_cast<void>(owner->setPlacement(before.occurrenceId,
                                            before.placement));
    }
  };
  try {
    for (const auto& change : candidate.changes_) {
      if (!owner->setPlacement(change.before.occurrenceId,
                               change.afterPlacement)) {
        rollback();
        return MatePlacementApplyStatus::Failed;
      }
      ++changed;
    }
    auto successorStatus = prepareSuccessor.prepare(
        *this, ProductDocumentSuccessorImpact::MateProjection);
    if (successorStatus != ProductDocumentSuccessorStatus::Prepared) {
      rollback();
      return successorStatus == ProductDocumentSuccessorStatus::ResourceFailure
                 ? MatePlacementApplyStatus::ResourceFailure
                 : MatePlacementApplyStatus::SuccessorRejected;
    }
    return MatePlacementApplyStatus::Applied;
  } catch (const std::bad_alloc&) {
    rollback();
    return MatePlacementApplyStatus::ResourceFailure;
  } catch (const Standard_Failure&) {
    rollback();
    return MatePlacementApplyStatus::Failed;
  } catch (...) {
    rollback();
    return MatePlacementApplyStatus::Failed;
  }
}

MatePlacementApplyStatus ProductDocument::applyPlanarMateAuthoring(
    const PlanarMateAuthoringTransaction& transaction,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_) return MatePlacementApplyStatus::Busy;
  try {
    if (!transaction.integrityValid() ||
        transaction.ownerAssemblyId_ !=
            transaction.candidate_.address_.ownerAssemblyId ||
        transaction.candidate_.address_.productIncarnation != incarnation_ ||
        transaction.candidate_.address_.rootDefinitionId != rootDefinitionId_)
      return MatePlacementApplyStatus::InvalidCandidate;
    MateRegistry expectedAfter = transaction.beforeRegistry_;
    if (expectedAfter.insert(transaction.relation_) !=
            MateRegistryMutationStatus::Inserted ||
        expectedAfter != transaction.afterRegistry_)
      return MatePlacementApplyStatus::InvalidCandidate;
    auto* owner =
        definitions_.findAssemblyMutable(transaction.ownerAssemblyId_);
    if (!owner || owner->mateRegistry() != transaction.beforeRegistry_)
      return MatePlacementApplyStatus::Stale;
    const auto* proposed =
        transaction.afterRegistry_.find(transaction.relation_.id());
    if (!proposed || *proposed != transaction.relation_ ||
        transaction.afterRegistry_.revision() !=
            transaction.candidate_.address_.mateRegistryRevision ||
        transaction.afterRegistry_.fingerprint() !=
            transaction.candidate_.address_.mateRegistryFingerprint)
      return MatePlacementApplyStatus::InvalidCandidate;
    auto replacement =
        std::make_unique<MateRegistry>(transaction.afterRegistry_);
    using std::swap;
    swap(owner->mateRegistry_, replacement);
    const auto applied =
        applyMatePlacementCandidate(transaction.candidate_, prepareSuccessor);
    if (applied != MatePlacementApplyStatus::Applied)
      swap(owner->mateRegistry_, replacement);
    return applied;
  } catch (const std::bad_alloc&) {
    return MatePlacementApplyStatus::ResourceFailure;
  } catch (...) {
    return MatePlacementApplyStatus::Failed;
  }
}

MatePlacementApplyStatus ProductDocument::revertPlanarMateAuthoring(
    const PlanarMateAuthoringTransaction& transaction,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_) return MatePlacementApplyStatus::Busy;
  AssemblyDefinition* owner = nullptr;
  try {
    if (!transaction.integrityValid() ||
        transaction.ownerAssemblyId_ !=
            transaction.candidate_.address_.ownerAssemblyId ||
        transaction.candidate_.address_.productIncarnation != incarnation_ ||
        transaction.candidate_.address_.rootDefinitionId != rootDefinitionId_)
      return MatePlacementApplyStatus::InvalidCandidate;
    MateRegistry expectedAfter = transaction.beforeRegistry_;
    if (expectedAfter.insert(transaction.relation_) !=
            MateRegistryMutationStatus::Inserted ||
        expectedAfter != transaction.afterRegistry_)
      return MatePlacementApplyStatus::InvalidCandidate;
    owner = definitions_.findAssemblyMutable(transaction.ownerAssemblyId_);
    if (!owner || owner->mateRegistry() != transaction.afterRegistry_)
      return MatePlacementApplyStatus::Stale;
    const auto* authored =
        owner->mateRegistry().find(transaction.relation_.id());
    if (!authored || *authored != transaction.relation_)
      return MatePlacementApplyStatus::Stale;
    for (const auto& change : transaction.candidate_.changes_) {
      const auto* occurrence = owner->find(change.before.occurrenceId);
      if (!occurrence ||
          occurrence->targetDefinitionId() != change.before.targetDefinitionId ||
          occurrence->flags() != change.before.flags ||
          occurrence->placement() != change.afterPlacement)
        return MatePlacementApplyStatus::Stale;
    }
    for (const auto& dependency :
         transaction.candidate_.occurrenceDependencies_) {
      const auto* dependencyOwner =
          definitions_.findAssembly(dependency.ownerAssemblyId);
      const auto* occurrence = dependencyOwner
                                   ? dependencyOwner->find(
                                         dependency.occurrenceId)
                                   : nullptr;
      RigidTransform3d expectedPlacement = dependency.placement;
      if (dependency.ownerAssemblyId == transaction.ownerAssemblyId_) {
        const auto changed = std::find_if(
            transaction.candidate_.changes_.begin(),
            transaction.candidate_.changes_.end(),
            [&dependency](const auto& value) {
              return value.before.occurrenceId == dependency.occurrenceId;
            });
        if (changed != transaction.candidate_.changes_.end())
          expectedPlacement = changed->afterPlacement;
      }
      if (!occurrence ||
          occurrence->targetDefinitionId() != dependency.targetDefinitionId ||
          occurrence->placement() != expectedPlacement ||
          occurrence->flags() != dependency.flags ||
          occurrence->displayNameUtf8() != dependency.displayNameUtf8)
        return MatePlacementApplyStatus::Stale;
    }
    for (const auto& dependency : transaction.candidate_.definitionDependencies_) {
      const auto* part = definitions_.findPart(dependency.definitionId);
      if (!part || part->stateId() != dependency.stateId)
        return MatePlacementApplyStatus::Stale;
      if (!dependency.topology) continue;
      const auto* body = part->document().findBody(dependency.topology->bodyId);
      const auto snapshot =
          body ? body->topologySnapshot(dependency.topology->featureId)
               : std::nullopt;
      if (!snapshot ||
          snapshot->revision() != dependency.topology->shapeRevision ||
          (snapshot->topologyIndex() != nullptr) !=
              dependency.topology->topologyAvailable ||
          snapshot->v2Authoritative() !=
              dependency.topology->v2Authoritative)
        return MatePlacementApplyStatus::Stale;
    }
    auto replacement =
        std::make_unique<MateRegistry>(transaction.beforeRegistry_);
    ProductMutationGuard guard(productMutationTransactionActive_);
    std::size_t changed = 0;
    const auto rollbackPlacements = [&]() noexcept {
      while (changed > 0) {
        --changed;
        const auto& change = transaction.candidate_.changes_[changed];
        static_cast<void>(owner->setPlacement(change.before.occurrenceId,
                                              change.afterPlacement));
      }
    };
    for (const auto& change : transaction.candidate_.changes_) {
      if (!owner->setPlacement(change.before.occurrenceId,
                               change.before.placement)) {
        rollbackPlacements();
        return MatePlacementApplyStatus::Failed;
      }
      ++changed;
    }
    using std::swap;
    swap(owner->mateRegistry_, replacement);
    const auto successor = prepareSuccessor.prepare(
        *this, ProductDocumentSuccessorImpact::MateProjection);
    if (successor != ProductDocumentSuccessorStatus::Prepared) {
      swap(owner->mateRegistry_, replacement);
      rollbackPlacements();
      return successor == ProductDocumentSuccessorStatus::ResourceFailure
                 ? MatePlacementApplyStatus::ResourceFailure
                 : MatePlacementApplyStatus::SuccessorRejected;
    }
    return MatePlacementApplyStatus::Applied;
  } catch (const std::bad_alloc&) {
    return MatePlacementApplyStatus::ResourceFailure;
  } catch (...) {
    return MatePlacementApplyStatus::Failed;
  }
}

AssemblyMateRestoreStatus ProductDocument::restoreAssemblyMateState(
    DefinitionId ownerAssemblyId, const MateSolverOptions& options,
    std::vector<PlanarCoincidentMate> mates) noexcept {
  try {
    auto staged = std::make_unique<MateRegistry>();
    const auto optionStatus = staged->setOptions(options);
    if (optionStatus != MateRegistryMutationStatus::Inserted &&
        optionStatus != MateRegistryMutationStatus::Unchanged)
      return AssemblyMateRestoreStatus::Invalid;
    for (auto& mate : mates) {
      const auto status = staged->insert(std::move(mate));
      if (status != MateRegistryMutationStatus::Inserted)
        return status == MateRegistryMutationStatus::ResourceFailure
                   ? AssemblyMateRestoreStatus::ResourceFailure
                   : AssemblyMateRestoreStatus::Invalid;
    }
    return restoreAssemblyMateRegistry(ownerAssemblyId, std::move(staged));
  } catch (...) {
    return AssemblyMateRestoreStatus::ResourceFailure;
  }
}

AssemblyMateRestoreStatus ProductDocument::restoreAssemblyMateRegistry(
    DefinitionId ownerAssemblyId,
    std::unique_ptr<MateRegistry> registry) noexcept {
  if (mutationTransactionActive()) return AssemblyMateRestoreStatus::Busy;
  auto* assembly = definitions_.findAssemblyMutable(ownerAssemblyId);
  if (!assembly) return AssemblyMateRestoreStatus::MissingAssembly;
  if (assembly->mateRegistry().size() != 0)
    return AssemblyMateRestoreStatus::AlreadyAuthored;
  if (!registry || !registry->options().isValid() ||
      registry->size() > registry->options().maximumMates)
    return AssemblyMateRestoreStatus::Invalid;
  for (const auto& [id, mate] : registry->relations()) {
    if (id != assemblyMateId(mate) ||
        !isValidAssemblyMate(mate,
                             registry->options().maximumAbsoluteOffsetMm,
                             registry->options().angularToleranceRad))
      return AssemblyMateRestoreStatus::Invalid;
  }
  assembly->mateRegistry_ = std::move(registry);
  return AssemblyMateRestoreStatus::Restored;
}

}  // namespace solidar
