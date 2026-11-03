#pragma once

#include <compare>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "model/Document.h"
#include "model/ProjectId.h"
#include "model/RigidTransform3d.h"

namespace solidar {

class AssemblyMateDependencyTestAccess;
class ComponentExtractionDependencyTestAccess;
class ComponentDrawingValidationTestAccess;
class ComponentDefinition;

class DefinitionId {
 public:
  constexpr DefinitionId() noexcept = default;
  explicit constexpr DefinitionId(std::uint64_t value) noexcept
      : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }
  [[nodiscard]] constexpr bool isValid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(DefinitionId, DefinitionId) noexcept =
      default;
  friend constexpr auto operator<=>(DefinitionId, DefinitionId) noexcept =
      default;

 private:
  std::uint64_t value_{};
};

inline constexpr DefinitionId kInvalidDefinitionId{};

// Stable identity of a definition owned by another identity-bearing product.
// Locator and revision data deliberately do not participate in this identity.
struct ExternalSourceKey final {
  ProjectId projectId;
  DefinitionId definitionId;

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return projectId.isValid() && definitionId.isValid();
  }

  friend constexpr bool operator==(const ExternalSourceKey&,
                                   const ExternalSourceKey&) noexcept = default;
  friend constexpr auto operator<=>(const ExternalSourceKey&,
                                    const ExternalSourceKey&) noexcept = default;
};

// Exact SHA-256 of source bytes.  Revision is deliberately separate from
// identity and from its relocatable locator.
struct SourceContentHash final {
  std::array<std::uint8_t, 32> bytes{};

  friend constexpr bool operator==(const SourceContentHash&,
                                   const SourceContentHash&) noexcept = default;
  friend constexpr auto operator<=>(const SourceContentHash&,
                                    const SourceContentHash&) noexcept = default;
};

struct LinkedPartBinding final {
  ExternalSourceKey key;
  std::string relativePathHintUtf8;
  SourceContentHash acceptedHash;

  [[nodiscard]] bool isValid() const noexcept;
  friend bool operator==(const LinkedPartBinding&,
                         const LinkedPartBinding&) = default;
};

// Strict persistence-safe locator policy. It is lexical and performs no I/O.
[[nodiscard]] bool isSafeLinkedPartRelativePathHintUtf8(
    std::string_view hintUtf8) noexcept;

class OccurrenceId {
 public:
  constexpr OccurrenceId() noexcept = default;
  explicit constexpr OccurrenceId(std::uint64_t value) noexcept
      : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }
  [[nodiscard]] constexpr bool isValid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(OccurrenceId, OccurrenceId) noexcept =
      default;
  friend constexpr auto operator<=>(OccurrenceId, OccurrenceId) noexcept =
      default;

 private:
  std::uint64_t value_{};
};

inline constexpr OccurrenceId kInvalidOccurrenceId{};

class DrawingId {
 public:
  constexpr DrawingId() noexcept = default;
  explicit constexpr DrawingId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }
  [[nodiscard]] constexpr bool isValid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(DrawingId, DrawingId) noexcept = default;
  friend constexpr auto operator<=>(DrawingId, DrawingId) noexcept = default;

 private:
  std::uint64_t value_{};
};

inline constexpr DrawingId kInvalidDrawingId{};
inline constexpr std::size_t kComponentDrawingMaximumNameUtf8Bytes = 1024;
inline constexpr double kComponentDrawingMinimumScale = 1.0e-6;
inline constexpr double kComponentDrawingMaximumScale = 1.0e6;
inline constexpr std::size_t kComponentGraphMaximumDrawings = 4096;
inline constexpr std::size_t
    kComponentGraphMaximumDrawingBodyReferences = 65536;

class ProductIncarnationId {
 public:
  constexpr ProductIncarnationId() noexcept = default;
  explicit constexpr ProductIncarnationId(std::uint64_t value) noexcept
      : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }
  [[nodiscard]] constexpr bool isValid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(ProductIncarnationId,
                                   ProductIncarnationId) noexcept = default;
  friend constexpr auto operator<=>(ProductIncarnationId,
                                    ProductIncarnationId) noexcept = default;

 private:
  std::uint64_t value_{};
};

inline constexpr ProductIncarnationId kInvalidProductIncarnationId{};

class DefinitionStateId {
 public:
  constexpr DefinitionStateId() noexcept = default;
  explicit constexpr DefinitionStateId(std::uint64_t value) noexcept
      : value_(value) {}
  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }
  [[nodiscard]] constexpr bool isValid() const noexcept { return value_ != 0; }
  friend constexpr bool operator==(DefinitionStateId,
                                   DefinitionStateId) noexcept = default;
 private:
  std::uint64_t value_{};
};

inline constexpr DefinitionStateId kInvalidDefinitionStateId{};

enum class OccurrencePathStatus {
  Valid,
  Empty,
  InvalidOccurrenceId,
  UnsupportedDepth,
};

struct OccurrencePathCreateResult;

class OccurrencePath final {
 public:
  [[nodiscard]] static OccurrencePathCreateResult create(
      std::vector<OccurrenceId> entries);

  [[nodiscard]] const std::vector<OccurrenceId>& entries() const noexcept {
    return entries_;
  }
  [[nodiscard]] std::size_t depth() const noexcept { return entries_.size(); }

  friend bool operator==(const OccurrencePath&,
                         const OccurrencePath&) = default;

 private:
  explicit OccurrencePath(std::vector<OccurrenceId> entries) noexcept
      : entries_(std::move(entries)) {}

  std::vector<OccurrenceId> entries_;
};

struct OccurrencePathCreateResult {
  OccurrencePathStatus status{OccurrencePathStatus::Empty};
  std::optional<OccurrencePath> path;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == OccurrencePathStatus::Valid && path.has_value();
  }
};

struct OccurrenceFlags {
  bool visible{true};
  bool suppressed{false};
  bool grounded{false};

  friend bool operator==(const OccurrenceFlags&,
                         const OccurrenceFlags&) = default;
};

class ComponentOccurrence final {
 public:
  ComponentOccurrence(OccurrenceId id, DefinitionId targetDefinitionId,
                      RigidTransform3d placement,
                      OccurrenceFlags flags = {},
                      std::string displayNameUtf8 = {});
  ComponentOccurrence(OccurrenceId id, DefinitionId targetDefinitionId,
                      RigidTransform3d placement,
                      RigidTransform3d homePlacement,
                      OccurrenceFlags flags = {},
                      std::string displayNameUtf8 = {});

  [[nodiscard]] OccurrenceId id() const noexcept { return id_; }
  [[nodiscard]] DefinitionId targetDefinitionId() const noexcept {
    return targetDefinitionId_;
  }
  [[nodiscard]] const RigidTransform3d& placement() const noexcept {
    return placement_;
  }
  [[nodiscard]] const RigidTransform3d& homePlacement() const noexcept {
    return homePlacement_;
  }
  [[nodiscard]] OccurrenceFlags flags() const noexcept { return flags_; }
  [[nodiscard]] const std::string& displayNameUtf8() const noexcept {
    return displayNameUtf8_;
  }

 private:
  friend class AssemblyDefinition;
  friend class ComponentDefinition;

  OccurrenceId id_;
  DefinitionId targetDefinitionId_;
  RigidTransform3d placement_;
  RigidTransform3d homePlacement_;
  OccurrenceFlags flags_;
  std::string displayNameUtf8_;
};

enum class OccurrenceDisplayNameStatus {
  Valid,
  Empty,
  TooLong,
  InvalidUtf8,
  EmbeddedNull,
  ForbiddenControl,
  AllWhitespace,
};

[[nodiscard]] OccurrenceDisplayNameStatus validateOccurrenceDisplayNameUtf8(
    std::string_view nameUtf8, bool allowEmpty = false) noexcept;

enum class ComponentDrawingPaper : std::uint8_t { A4 };
enum class ComponentDrawingOrientation : std::uint8_t {
  Portrait,
  Landscape,
};

// Definition-owned drawing metadata.  Geometry/views are deliberately not
// introduced by D1; source bodies are stable definition-local references.
// Construction canonicalizes the reference set so every retained value is
// sorted and duplicate-free before it can enter a ProductDocument.
class ComponentDrawing final {
 public:
  ComponentDrawing(DrawingId id, std::string nameUtf8,
                   ComponentDrawingPaper paper,
                   ComponentDrawingOrientation orientation, double scale,
                   std::vector<BodyId> sourceBodyIds);

  [[nodiscard]] DrawingId id() const noexcept { return id_; }
  [[nodiscard]] const std::string& nameUtf8() const noexcept {
    return nameUtf8_;
  }
  [[nodiscard]] ComponentDrawingPaper paper() const noexcept { return paper_; }
  [[nodiscard]] ComponentDrawingOrientation orientation() const noexcept {
    return orientation_;
  }
  [[nodiscard]] double scale() const noexcept { return scale_; }
  [[nodiscard]] const std::vector<BodyId>& sourceBodyIds() const noexcept {
    return sourceBodyIds_;
  }

  friend bool operator==(const ComponentDrawing&,
                         const ComponentDrawing&) = default;

 private:
  friend class ComponentDrawingValidationTestAccess;

  DrawingId id_;
  std::string nameUtf8_;
  ComponentDrawingPaper paper_{ComponentDrawingPaper::A4};
  ComponentDrawingOrientation orientation_{
      ComponentDrawingOrientation::Portrait};
  double scale_{1.0};
  std::vector<BodyId> sourceBodyIds_;
};

class PartDefinition final {
 public:
  PartDefinition(DefinitionId id, std::unique_ptr<Document>&& document);
  PartDefinition(DefinitionId id, std::unique_ptr<Document>&& document,
                 LinkedPartBinding binding);
  ~PartDefinition() = default;

  PartDefinition(const PartDefinition&) = delete;
  PartDefinition& operator=(const PartDefinition&) = delete;
  PartDefinition(PartDefinition&&) noexcept = default;
  PartDefinition& operator=(PartDefinition&&) noexcept = default;

  [[nodiscard]] DefinitionId id() const noexcept { return id_; }
  [[nodiscard]] Document& document() noexcept { return *document_; }
  [[nodiscard]] const Document& document() const noexcept { return *document_; }
  [[nodiscard]] DefinitionStateId stateId() const noexcept { return stateId_; }
  [[nodiscard]] bool isLinked() const noexcept { return binding_.has_value(); }
  [[nodiscard]] const std::optional<LinkedPartBinding>& linkedBinding() const
      noexcept {
    return binding_;
  }

 private:
  friend class ProductDocument;

  [[nodiscard]] std::unique_ptr<Document> releaseDocument() && noexcept {
    return std::move(document_);
  }

  DefinitionId id_;
  std::unique_ptr<Document> document_;
  std::optional<LinkedPartBinding> binding_;
  DefinitionStateId stateId_{1};
  std::uint64_t nextStateIdValue_{2};
};

enum class AssemblyOccurrenceInsertStatus {
  Inserted,
  DuplicateOccurrenceId,
};

enum class AssemblyOccurrenceEraseStatus {
  Erased,
  OccurrenceMissing,
};

enum class FlatOccurrenceResolveStatus {
  Resolved,
  RootIsNotAssembly,
  UnsupportedDepth,
  OccurrenceMissing,
};

struct FlatOccurrenceResolveResult {
  FlatOccurrenceResolveStatus status{
      FlatOccurrenceResolveStatus::OccurrenceMissing};
  const ComponentOccurrence* occurrence{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == FlatOccurrenceResolveStatus::Resolved &&
           occurrence != nullptr;
  }
};

class AssemblyDefinition final {
 public:
  explicit AssemblyDefinition(DefinitionId id);
  ~AssemblyDefinition();

  AssemblyDefinition(const AssemblyDefinition&) = delete;
  AssemblyDefinition& operator=(const AssemblyDefinition&) = delete;
  AssemblyDefinition(AssemblyDefinition&&) noexcept;
  AssemblyDefinition& operator=(AssemblyDefinition&&) noexcept;

  [[nodiscard]] DefinitionId id() const noexcept { return id_; }
  [[nodiscard]] AssemblyOccurrenceInsertStatus insert(
      ComponentOccurrence&& occurrence);
  [[nodiscard]] AssemblyOccurrenceEraseStatus erase(OccurrenceId id);
  [[nodiscard]] const ComponentOccurrence* find(OccurrenceId id) const
      noexcept;
  [[nodiscard]] FlatOccurrenceResolveResult resolve(
      const OccurrencePath& path) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept {
    return occurrences_.size();
  }
  [[nodiscard]] const std::map<OccurrenceId, ComponentOccurrence>&
  occurrences() const noexcept {
    return occurrences_;
  }
  [[nodiscard]] class MateRegistry& mateRegistry() noexcept;
  [[nodiscard]] const class MateRegistry& mateRegistry() const noexcept;

 private:
  friend class ProductDocument;
  friend class AssemblyMateDependencyTestAccess;

  [[nodiscard]] bool setPlacement(OccurrenceId id,
                                  RigidTransform3d placement) noexcept;
  [[nodiscard]] bool setFlags(OccurrenceId id,
                              OccurrenceFlags flags) noexcept;
  // A5c2 visibility transactions must never overwrite concurrently authored
  // suppression/grounding state with a stale whole-flags snapshot.
  [[nodiscard]] bool setVisible(OccurrenceId id, bool visible) noexcept;
  // A5c3 grounding transactions mutate only their authored bit and never
  // restore a stale whole-flags snapshot.
  [[nodiscard]] bool setGrounded(OccurrenceId id, bool grounded) noexcept;
  // A5c4 suppression transactions mutate only their authored bit and never
  // restore a stale whole-flags snapshot.
  [[nodiscard]] bool setSuppressed(OccurrenceId id, bool suppressed) noexcept;
  [[nodiscard]] bool swapDisplayName(OccurrenceId id,
                                     std::string& displayNameUtf8) noexcept;

  DefinitionId id_;
  std::map<OccurrenceId, ComponentOccurrence> occurrences_;
  std::unique_ptr<class MateRegistry> mateRegistry_;
};

enum class ComponentOccurrenceInsertStatus {
  Inserted,
  DuplicateOccurrenceId,
};

enum class ComponentOccurrenceEraseStatus {
  Erased,
  OccurrenceMissing,
};

// A component is a project-like authored definition: it owns ordinary model
// content and may instantiate other components.  Unlike the legacy split
// PartDefinition/AssemblyDefinition representation, its own Document and its
// child occurrences are one identity-bearing unit.
class ComponentDefinition final {
 public:
  ComponentDefinition(DefinitionId id, std::unique_ptr<Document>&& document);
  ComponentDefinition(DefinitionId id, std::unique_ptr<Document>&& document,
                      std::vector<ComponentDrawing>&& drawings);
  ~ComponentDefinition() = default;

  ComponentDefinition(const ComponentDefinition&) = delete;
  ComponentDefinition& operator=(const ComponentDefinition&) = delete;
  ComponentDefinition(ComponentDefinition&&) noexcept = default;
  ComponentDefinition& operator=(ComponentDefinition&&) noexcept = default;

  [[nodiscard]] DefinitionId id() const noexcept { return id_; }
  [[nodiscard]] Document& document() noexcept { return *document_; }
  [[nodiscard]] const Document& document() const noexcept { return *document_; }
  [[nodiscard]] DefinitionStateId stateId() const noexcept { return stateId_; }
  [[nodiscard]] ComponentOccurrenceInsertStatus insert(
      ComponentOccurrence&& occurrence);
  [[nodiscard]] ComponentOccurrenceEraseStatus erase(OccurrenceId id);
  [[nodiscard]] const ComponentOccurrence* find(OccurrenceId id) const
      noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return occurrences_.size(); }
  [[nodiscard]] const std::map<OccurrenceId, ComponentOccurrence>&
  occurrences() const noexcept {
    return occurrences_;
  }
  [[nodiscard]] const std::vector<ComponentDrawing>& drawings() const noexcept {
    return drawings_;
  }
  [[nodiscard]] const ComponentDrawing* findDrawing(DrawingId id) const
      noexcept;

 private:
  friend class ProductDocument;
  friend class ComponentExtractionDependencyTestAccess;
  friend class ComponentDrawingValidationTestAccess;

  [[nodiscard]] bool setPlacement(OccurrenceId id,
                                  RigidTransform3d placement) noexcept;
  [[nodiscard]] bool setHomePlacement(
      OccurrenceId id, RigidTransform3d homePlacement) noexcept;
  [[nodiscard]] bool setVisible(OccurrenceId id, bool visible) noexcept;
  [[nodiscard]] bool swapDisplayName(OccurrenceId id,
                                     std::string& displayNameUtf8) noexcept;
  using OccurrenceNode =
      std::map<OccurrenceId, ComponentOccurrence>::node_type;
  [[nodiscard]] OccurrenceNode extract(OccurrenceId id) noexcept;
  void restore(OccurrenceNode&& node) noexcept;

  DefinitionId id_;
  std::unique_ptr<Document> document_;
  std::map<OccurrenceId, ComponentOccurrence> occurrences_;
  std::vector<ComponentDrawing> drawings_;
  DefinitionStateId stateId_{1};
  std::uint64_t nextStateIdValue_{2};
};

enum class DefinitionKind { Part, Assembly, Component };

enum class RecursiveOccurrenceResolveStatus {
  Resolved,
  RootIsNotAssembly,
  EmptyPath,
  UnsupportedDepth,
  OccurrenceMissing,
  IntermediateTargetIsPart,
  DanglingTarget,
};

struct RecursiveOccurrenceResolveResult final {
  RecursiveOccurrenceResolveStatus status{
      RecursiveOccurrenceResolveStatus::OccurrenceMissing};
  const AssemblyDefinition* ownerAssembly{};
  const ComponentOccurrence* occurrence{};
  const PartDefinition* targetPart{};
  const AssemblyDefinition* targetAssembly{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == RecursiveOccurrenceResolveStatus::Resolved &&
           ownerAssembly != nullptr && occurrence != nullptr &&
           ((targetPart != nullptr) != (targetAssembly != nullptr));
  }
  [[nodiscard]] bool terminalPart() const noexcept {
    return this->operator bool() && targetPart != nullptr;
  }
};

enum class DefinitionInsertStatus {
  Inserted,
  Busy,
  DuplicateDefinitionId,
  DuplicateExternalSourceKey,
  DanglingOccurrenceTarget,
  UnsupportedAssemblyTarget,
  UnsupportedNonRootAssemblyDefinition,
  UnsupportedComponentTarget,
  ComponentGraphCycle,
};

enum class DefinitionEraseStatus {
  Erased,
  DefinitionMissing,
  DefinitionReferenced,
};

class DefinitionRegistry final {
 public:
  DefinitionRegistry() = default;
  ~DefinitionRegistry() = default;

  DefinitionRegistry(const DefinitionRegistry&) = delete;
  DefinitionRegistry& operator=(const DefinitionRegistry&) = delete;
  DefinitionRegistry(DefinitionRegistry&&) noexcept = default;
  DefinitionRegistry& operator=(DefinitionRegistry&&) noexcept = default;

  // The argument is moved only after the duplicate check succeeds. A caller
  // can therefore inspect or retry a named candidate after a rejected insert.
  [[nodiscard]] DefinitionInsertStatus insert(PartDefinition&& definition);
  [[nodiscard]] DefinitionInsertStatus insert(
      AssemblyDefinition&& definition);
  [[nodiscard]] DefinitionInsertStatus insert(
      ComponentDefinition&& definition);
  [[nodiscard]] DefinitionEraseStatus erase(DefinitionId id);

  // Kept as a part-only compatibility lookup for the existing V3-V5 path.
  [[nodiscard]] const PartDefinition* find(DefinitionId id) const noexcept;
  [[nodiscard]] const PartDefinition* findPart(DefinitionId id) const noexcept;
  [[nodiscard]] const AssemblyDefinition* findAssembly(
      DefinitionId id) const noexcept;
  [[nodiscard]] const ComponentDefinition* findComponent(
      DefinitionId id) const noexcept;
  [[nodiscard]] std::optional<DefinitionKind> kind(DefinitionId id) const
      noexcept;
  [[nodiscard]] bool contains(DefinitionId id) const noexcept;
  [[nodiscard]] std::vector<DefinitionId> definitionIds() const;
  [[nodiscard]] std::size_t size() const noexcept { return definitions_.size(); }

 private:
  friend class ProductDocument;
  friend class ComponentExtractionDependencyTestAccess;

  using StoredDefinition =
      std::variant<PartDefinition, AssemblyDefinition, ComponentDefinition>;

  [[nodiscard]] PartDefinition* findPartMutable(DefinitionId id) noexcept;
  [[nodiscard]] AssemblyDefinition* findAssemblyMutable(
      DefinitionId id) noexcept;
  [[nodiscard]] ComponentDefinition* findComponentMutable(
      DefinitionId id) noexcept;

  std::map<DefinitionId, StoredDefinition> definitions_;
};

enum class ProductDocumentStatus {
  Valid,
  InvalidRootId,
  MissingRootDefinition,
  InvalidProjectId,
  DanglingOccurrenceTarget,
  UnsupportedAssemblyTarget,
  UnsupportedNonRootAssemblyDefinition,
  DuplicateDefinitionId,
  DuplicateExternalSourceKey,
  AssemblyGraphCycle,
  AssemblyGraphBudgetExceeded,
  ComponentGraphCycle,
  ComponentGraphBudgetExceeded,
  InvalidComponentDrawing,
  DuplicateComponentDrawingId,
  DanglingComponentDrawingBody,
  ComponentDrawingBudgetExceeded,
  UnsupportedComponentDefinition,
  ResourceFailure,
};

enum class ProductDocumentRemoveStatus {
  Removed,
  Busy,
  Missing,
  RootProtected,
  DefinitionRemovalUnsupported,
};

enum class ProductOccurrenceMutationStatus {
  Committed,
  Busy,
  RootIsNotAssembly,
  DuplicateOccurrenceId,
  DanglingTargetDefinition,
  UnsupportedAssemblyTarget,
  UnsupportedPathDepth,
  OccurrenceMissing,
  OccurrenceGrounded,
};

enum class AssemblyMateRestoreStatus {
  Restored,
  Busy,
  MissingAssembly,
  AlreadyAuthored,
  Invalid,
  ResourceFailure,
};

// A5b-only move carrier for the single atomic operation supported by the
// first assembly authoring tool.  It is deliberately not a generic model
// delta: while detached it owns exactly one authored part Document; while
// applied the ProductDocument owns that Document and this pointer is null.
struct EmbeddedPartInsertPayload final {
  DefinitionId definitionId;
  OccurrenceId occurrenceId;
  RigidTransform3d placement{RigidTransform3d::identity()};
  OccurrenceFlags flags{};
  std::unique_ptr<Document> document;
  std::string displayNameUtf8;

  [[nodiscard]] bool detached() const noexcept { return document != nullptr; }
};

enum class EmbeddedPartInsertStatus {
  Committed,
  Extracted,
  Busy,
  RootIsNotAssembly,
  InvalidPayload,
  DuplicateDefinitionId,
  DuplicateOccurrenceId,
  GuardMismatch,
  DefinitionReferenced,
  SuccessorRejected,
  SuccessorResourceFailure,
  AllocationFailure,
};

// Deterministic seams for proving the strong ownership guarantee of the
// narrow A5b paired insert. Production callers always use None.
enum class EmbeddedPartInsertFailurePoint {
  None,
  PlaceholderAllocation,
  DefinitionNodeInsert,
  OccurrenceNodeInsert,
  DefinitionNodeCommit,
  OccurrenceNodeCommit,
};

enum class ProductDocumentStandaloneConversionStatus {
  Committed,
  Busy,
  SelfTransition,
  SourceAlreadyIdentified,
  SourceNotSingleRoot,
  PreparedMissingIdentity,
  PreparedInvalidIdentity,
  PreparedNotSingleRoot,
};

enum class ProductDocumentForkIdentityStatus {
  Committed,
  Busy,
  SourceMissingIdentity,
  InvalidReplacementProjectId,
  SameProjectId,
  StaleExpectedIdentity,
};

struct ProductDocumentMetadata {
  std::string nameUtf8;
  std::string createdAtUtf8;

  friend bool operator==(const ProductDocumentMetadata&,
                         const ProductDocumentMetadata&) = default;
};

struct ProductDocumentIdentity {
  ProjectId projectId;
  ProductDocumentMetadata metadata;

  friend bool operator==(const ProductDocumentIdentity&,
                         const ProductDocumentIdentity&) = default;
};

class ProductDocument;
class ComponentIdentityTransitionTestAccess;
class ProjectApplicationService;
struct ComponentIdentityTransitionPrepareResult;

enum class ComponentIdentityTransitionKind : std::uint8_t {
  FirstSave,
  SaveAs,
};

enum class ComponentIdentityTransitionStatus : std::uint8_t {
  Prepared,
  InvalidTargetIdentity,
  InvalidSource,
  SourceBusy,
  RootIsNotComponent,
  SourceIdentityRequired,
  SourceIdentityMustBeAbsent,
  SameProjectIdentity,
  InvalidDefinitionState,
  ResourceFailure,
};

// Opaque, immutable stale-source seal for the only persistence operations
// allowed to change a component ProductDocument identity.  It contains no
// Document/B-Rep/Qt ownership and can be safely retained by an off-side UI
// projection until the atomic writer commits the exact sealed target.
class ComponentIdentityTransition final {
 public:
  [[nodiscard]] static ComponentIdentityTransitionPrepareResult
  prepareFirstSave(const ProductDocument& product,
                   ProductDocumentIdentity target) noexcept;
  [[nodiscard]] static ComponentIdentityTransitionPrepareResult prepareSaveAs(
      const ProductDocument& product,
      ProductDocumentIdentity target) noexcept;

  [[nodiscard]] ComponentIdentityTransitionKind kind() const noexcept {
    return kind_;
  }
  [[nodiscard]] const ProductDocumentIdentity& targetIdentity() const noexcept {
    return targetIdentity_;
  }
  [[nodiscard]] bool isCurrentSource(
      const ProductDocument& product) const noexcept;
  [[nodiscard]] bool matchesCommittedProduct(
      const ProductDocument& product) const noexcept;

 private:
  friend class ComponentIdentityTransitionTestAccess;
  friend class ProjectApplicationService;
  [[nodiscard]] static ComponentIdentityTransitionPrepareResult prepare(
      const ProductDocument& product, ProductDocumentIdentity target,
      ComponentIdentityTransitionKind kind) noexcept;
  ComponentIdentityTransition(
      ComponentIdentityTransitionKind kind,
      ProductIncarnationId sourceIncarnation, DefinitionId sourceRoot,
      std::optional<ProductDocumentIdentity> sourceIdentity,
      std::vector<std::pair<DefinitionId, DefinitionStateId>> definitionStates,
      ProductDocumentIdentity targetIdentity) noexcept;
  [[nodiscard]] bool matchesStructure(
      const ProductDocument& product) const noexcept;
  [[nodiscard]] static std::uint64_t validationCountForTesting() noexcept;
  static void resetValidationCountForTesting() noexcept;

  ComponentIdentityTransitionKind kind_{};
  ProductIncarnationId sourceIncarnation_;
  DefinitionId sourceRoot_;
  std::optional<ProductDocumentIdentity> sourceIdentity_;
  std::vector<std::pair<DefinitionId, DefinitionStateId>> definitionStates_;
  ProductDocumentIdentity targetIdentity_;
};

struct ComponentIdentityTransitionPrepareResult final {
  ComponentIdentityTransitionStatus status{
      ComponentIdentityTransitionStatus::InvalidSource};
  std::optional<ComponentIdentityTransition> transition;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentIdentityTransitionStatus::Prepared &&
           transition.has_value();
  }
};

struct ProductDocumentCreateResult;
class OccurrenceDelta;
enum class OccurrenceDeltaStatus;
struct DuplicateOccurrenceColdPayload;
enum class DuplicateOccurrenceStatus;
enum class DuplicateOccurrenceFailurePoint;
struct RemoveOccurrenceColdPayload;
enum class RemoveOccurrenceStatus;
enum class RemoveOccurrenceFailurePoint;
class MatePlacementCandidate;
class PlanarMateAuthoringTransaction;
struct MateSolverOptions;
class PlanarCoincidentMate;
class MateRegistry;
enum class MatePlacementApplyStatus : std::uint8_t;

enum class ProductDocumentSuccessorStatus {
  Prepared,
  Rejected,
  ResourceFailure,
};

enum class ProductDocumentSuccessorImpact {
  FullProjection,
  TreeOnly,
  VisibilityProjection,
  GroundingProjection,
  SuppressionProjection,
  DuplicateProjection,
  RemoveProjection,
  DefinitionProjection,
  MateProjection,
};

struct DefinitionEditAddress final {
  ProductIncarnationId productIncarnation;
  DefinitionId rootDefinitionId;
  DefinitionId targetDefinitionId;
  DefinitionStateId expectedStateId;
};

enum class DefinitionEditStatus {
  Committed,
  Busy,
  InvalidAddress,
  ProductIncarnationMismatch,
  RootDefinitionMismatch,
  RootIsNotAssembly,
  DefinitionMissing,
  DefinitionStateMismatch,
  LinkedDefinitionRequiresLifecycle,
  RecomputeFailed,
  SuccessorRejected,
  SuccessorResourceFailure,
  ResourceFailure,
};

struct DefinitionEditResult final {
  DefinitionEditStatus status{DefinitionEditStatus::InvalidAddress};
  DefinitionStateId committedStateId;
};

enum class LinkedPartMutationStatus {
  Committed,
  Busy,
  InvalidAddress,
  ProductIncarnationMismatch,
  RootDefinitionMismatch,
  RootIsNotAssembly,
  DefinitionMissing,
  DefinitionStateMismatch,
  DefinitionIsNotLinked,
  BindingMismatch,
  InvalidReplacementBinding,
  ForbiddenIdentityChange,
  ForbiddenLocatorChange,
  ForbiddenRevisionChange,
  NoChange,
  RecomputeFailed,
  SuccessorRejected,
  SuccessorResourceFailure,
  ResourceFailure,
};

struct LinkedPartMutationResult final {
  LinkedPartMutationStatus status{LinkedPartMutationStatus::InvalidAddress};
  DefinitionStateId committedStateId;
};

enum class DefinitionEditFailurePoint : std::uint8_t {
  None,
  BeforeRecompute,
  BeforeCommit,
  Finalization,
};

struct ComponentExtractionAddress final {
  ProductIncarnationId productIncarnation;
  DefinitionId rootDefinitionId;
  DefinitionId ownerDefinitionId;
  DefinitionStateId expectedOwnerStateId;
  BodyId bodyId{kInvalidBodyId};
  DefinitionId childDefinitionId;
  OccurrenceId childOccurrenceId;
};

enum class ComponentExtractionStatus {
  Prepared,
  Committed,
  Reverted,
  Busy,
  InvalidAddress,
  ProductIncarnationMismatch,
  RootDefinitionMismatch,
  OwnerMustBeRoot,
  UnsupportedRootKind,
  OwnerStateMismatch,
  BodyMissing,
  CrossDefinitionDependency,
  InvalidDependencyGraph,
  StalePlan,
  DuplicateDefinitionId,
  DuplicateOccurrenceId,
  StructureMismatch,
  ParentRecomputeFailed,
  ChildRecomputeFailed,
  SuccessorRejected,
  SuccessorResourceFailure,
  ResourceFailure,
};

// Scalar-only cold transaction. It owns no Document, B-Rep, QObject or UI
// state and can therefore be retained by a later history adapter.
struct ComponentExtractionTransaction final {
  ComponentExtractionAddress address;
  ComponentExtractionPlan plan;
  DefinitionStateId beforeOwnerStateId;
  DefinitionStateId afterOwnerStateId;
  std::uint64_t beforeNextStateIdValue{};
  std::uint64_t afterNextStateIdValue{};
  DefinitionStateId createdChildStateId{1};
  std::uint64_t createdChildNextStateIdValue{2};
  bool convertedPartRoot{};
  std::vector<ComponentDrawing> beforeOwnerDrawings;
  std::vector<ComponentDrawing> afterOwnerDrawings;
  std::vector<ComponentDrawing> createdChildDrawings;
};

struct ComponentExtractionPrepareResult final {
  ComponentExtractionStatus status{ComponentExtractionStatus::InvalidAddress};
  std::optional<ComponentExtractionTransaction> transaction;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentExtractionStatus::Prepared &&
           transaction.has_value();
  }
};

enum class ComponentExtractionFailurePoint : std::uint8_t {
  None,
  CandidateAllocation,
  BeforeSuccessor,
  BeforeCommit,
};

// Exact, scalar-only request produced when a component drag is released.
// A missing path denotes the authored root and is intentionally rejected:
// only an occurrence has a mutable parent-local placement.
struct ComponentPlacementRequest final {
  ProductIncarnationId productIncarnation;
  DefinitionId rootDefinitionId;
  std::optional<OccurrencePath> path;
  DefinitionId expectedTargetDefinitionId;
  DefinitionStateId expectedOwnerStateId;
  RigidTransform3d expectedPlacement{RigidTransform3d::identity()};
  std::array<double, 3> worldDeltaMm{};

  friend bool operator==(const ComponentPlacementRequest&,
                         const ComponentPlacementRequest&) = default;
};

enum class ComponentPlacementStatus {
  Prepared,
  Committed,
  Reverted,
  NoChange,
  Busy,
  InvalidRequest,
  ProductIncarnationMismatch,
  RootDefinitionMismatch,
  RootIsNotComponent,
  RootCannotMove,
  OccurrenceMissing,
  TargetDefinitionMismatch,
  OwnerStateMismatch,
  PlacementMismatch,
  Grounded,
  Suppressed,
  EffectivelyHidden,
  NonFiniteDelta,
  ValueOutOfRange,
  DefinitionStateExhausted,
  SuccessorRejected,
  SuccessorResourceFailure,
  ResourceFailure,
  UnexpectedFailure,
};

enum class ComponentVisibilityStatus : std::uint8_t {
  Committed,
  NoChange,
  Busy,
  RootIsNotComponent,
  InvalidPath,
  OccurrenceMissing,
  StateIdExhausted,
  SuccessorRejected,
  SuccessorResourceFailure,
};

enum class ComponentOccurrenceEditStatus : std::uint8_t {
  Committed,
  NoChange,
  Busy,
  RootIsNotComponent,
  InvalidPath,
  InvalidName,
  OccurrenceMissing,
  StateIdExhausted,
  SuccessorRejected,
  SuccessorResourceFailure,
  ResourceFailure,
};

// Cold transaction retained by history. It owns no Document/B-Rep/QObject.
struct ComponentPlacementTransaction final {
  ComponentPlacementRequest request;
  DefinitionId ownerDefinitionId;
  DefinitionStateId beforeOwnerStateId;
  DefinitionStateId afterOwnerStateId;
  std::uint64_t beforeNextStateIdValue{};
  std::uint64_t afterNextStateIdValue{};
  RigidTransform3d beforePlacement{RigidTransform3d::identity()};
  RigidTransform3d afterPlacement{RigidTransform3d::identity()};

  friend bool operator==(const ComponentPlacementTransaction&,
                         const ComponentPlacementTransaction&) = default;
};

struct ComponentPlacementPrepareResult final {
  ComponentPlacementStatus status{ComponentPlacementStatus::InvalidRequest};
  std::optional<ComponentPlacementTransaction> transaction;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentPlacementStatus::Prepared &&
           transaction.has_value();
  }
};

enum class ComponentHomePlacementOperation : std::uint8_t {
  AdoptCurrent,
  Restore,
};

struct ComponentHomePlacementRequest final {
  ProductIncarnationId productIncarnation;
  DefinitionId rootDefinitionId;
  OccurrencePath path;
  DefinitionId expectedTargetDefinitionId;
  DefinitionId expectedOwnerDefinitionId;
  DefinitionStateId expectedOwnerStateId;
  RigidTransform3d expectedPlacement{RigidTransform3d::identity()};
  RigidTransform3d expectedHomePlacement{RigidTransform3d::identity()};
  ComponentHomePlacementOperation operation{
      ComponentHomePlacementOperation::AdoptCurrent};

  friend bool operator==(const ComponentHomePlacementRequest&,
                         const ComponentHomePlacementRequest&) = default;
};

enum class ComponentHomePlacementStatus : std::uint8_t {
  Prepared,
  Committed,
  Reverted,
  NoChange,
  Busy,
  InvalidRequest,
  ProductIncarnationMismatch,
  RootDefinitionMismatch,
  RootIsNotComponent,
  OccurrenceMissing,
  TargetDefinitionMismatch,
  OwnerDefinitionMismatch,
  OwnerStateMismatch,
  PlacementMismatch,
  HomePlacementMismatch,
  Grounded,
  Suppressed,
  EffectivelyHidden,
  DefinitionStateExhausted,
  SuccessorRejected,
  SuccessorResourceFailure,
  ResourceFailure,
  UnexpectedFailure,
};

struct ComponentHomePlacementTransaction final {
  ComponentHomePlacementRequest request;
  DefinitionId ownerDefinitionId;
  DefinitionStateId beforeOwnerStateId;
  DefinitionStateId afterOwnerStateId;
  std::uint64_t beforeNextStateIdValue{};
  std::uint64_t afterNextStateIdValue{};
  RigidTransform3d beforePlacement{RigidTransform3d::identity()};
  RigidTransform3d afterPlacement{RigidTransform3d::identity()};
  RigidTransform3d beforeHomePlacement{RigidTransform3d::identity()};
  RigidTransform3d afterHomePlacement{RigidTransform3d::identity()};

  friend bool operator==(const ComponentHomePlacementTransaction&,
                         const ComponentHomePlacementTransaction&) = default;
};

struct ComponentHomePlacementPrepareResult final {
  ComponentHomePlacementStatus status{
      ComponentHomePlacementStatus::InvalidRequest};
  std::optional<ComponentHomePlacementTransaction> transaction;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentHomePlacementStatus::Prepared &&
           transaction.has_value();
  }
};

// One cold, atomic restore operation for every displaced occurrence in the
// reachable component graph. Transactions are ordered so repeated edits of
// occurrences owned by the same definition form a valid state-id chain.
struct ComponentHomePlacementBatchPrepareResult final {
  ComponentHomePlacementStatus status{
      ComponentHomePlacementStatus::InvalidRequest};
  std::vector<ComponentHomePlacementTransaction> transactions;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentHomePlacementStatus::Prepared &&
           !transactions.empty();
  }
};

// A stack-scoped, non-owning dispatch boundary for prepare-before-swap.
// Implementations must contain their own exceptions and report a typed status;
// ProductDocument never stores this interface.
class ProductDocumentSuccessor {
 public:
  virtual ~ProductDocumentSuccessor() = default;
  [[nodiscard]] virtual ProductDocumentSuccessorStatus prepare(
      const ProductDocument& candidate,
      ProductDocumentSuccessorImpact impact) noexcept = 0;
};

class ProductDocument final {
 public:
  ~ProductDocument() = default;

  ProductDocument(const ProductDocument&) = delete;
  ProductDocument& operator=(const ProductDocument&) = delete;
  ProductDocument(ProductDocument&& other) noexcept;
  ProductDocument& operator=(ProductDocument&& other) noexcept;

  [[nodiscard]] static ProductDocumentCreateResult create(
      DefinitionRegistry&& definitions, DefinitionId rootDefinitionId,
      std::optional<ProductDocumentIdentity> identity = std::nullopt);
  // Atomic nested-assembly construction boundary. Unlike incremental registry
  // insertion, all definitions are staged together so forward references can
  // be validated before any registry is published.
  [[nodiscard]] static ProductDocumentCreateResult createNested(
      std::vector<PartDefinition>&& parts,
      std::vector<AssemblyDefinition>&& assemblies,
      DefinitionId rootDefinitionId,
      std::optional<ProductDocumentIdentity> identity = std::nullopt);
  // Atomic construction boundary for the unified component graph. Forward
  // references are allowed inside the candidate set; no partially validated
  // registry is ever published.
  [[nodiscard]] static ProductDocumentCreateResult createComponentGraph(
      std::vector<ComponentDefinition>&& components,
      DefinitionId rootDefinitionId,
      std::optional<ProductDocumentIdentity> identity = std::nullopt);

  [[nodiscard]] const DefinitionRegistry& definitions() const noexcept {
    return definitions_;
  }
  [[nodiscard]] DefinitionId rootDefinitionId() const noexcept {
    return rootDefinitionId_;
  }
  [[nodiscard]] ProductIncarnationId incarnation() const noexcept {
    return incarnation_;
  }
  [[nodiscard]] bool finalizeReservedIncarnation() noexcept;
  [[nodiscard]] bool mutationTransactionActive() const noexcept {
    return productMutationTransactionActive_;
  }
  [[nodiscard]] const std::optional<ProductDocumentIdentity>& identity()
      const noexcept {
    return identity_;
  }
  [[nodiscard]] std::optional<ProjectId> projectId() const noexcept {
    if (!identity_) return std::nullopt;
    return identity_->projectId;
  }
  // One-shot commit of an identity bundle prepared by a successful
  // persistence transaction. Invalid IDs and subsequent commits are rejected
  // without changing the authoritative bundle.
  [[nodiscard]] bool commitProductIdentity(
      ProductDocumentIdentity identity) noexcept;
  // Re-identifies an already identified product after its clone has been
  // published. The complete expected bundle is a stale-source guard. Every
  // rejection is an exact no-op; success swaps only the identity bundle and
  // never touches the definition registry or live Document graph.
  [[nodiscard]] ProductDocumentForkIdentityStatus commitForkIdentity(
      const ProductDocumentIdentity& expectedIdentity,
      ProductDocumentIdentity replacementIdentity) noexcept;
  // Atomically adopts a fully prepared identity-bearing carrier while
  // retaining this standalone product's live root Document allocation. The
  // prepared reconstruction is destroyed as part of a successful commit;
  // every rejection leaves both operands unchanged.
  [[nodiscard]] ProductDocumentStandaloneConversionStatus
  commitPreparedStandaloneConversion(ProductDocument&& prepared) noexcept;
  // Compatibility accessors for the existing single-part application path.
  // They throw if called for an assembly-root product; assembly-aware callers
  // must use the typed pointer accessors below.
  [[nodiscard]] const PartDefinition& rootDefinition() const;
  [[nodiscard]] DefinitionKind rootDefinitionKind() const noexcept;
  [[nodiscard]] const PartDefinition* rootPartDefinition() const noexcept;
  [[nodiscard]] const AssemblyDefinition* rootAssemblyDefinition() const
      noexcept;
  [[nodiscard]] const ComponentDefinition* rootComponentDefinition() const
      noexcept;
  [[nodiscard]] bool hasSingleValidRootDefinition() const noexcept;
  [[nodiscard]] Document& rootDocument();
  [[nodiscard]] const Document& rootDocument() const;
  [[nodiscard]] const Document* partDocument(DefinitionId id) const noexcept;
  [[nodiscard]] const Document* componentDocument(
      DefinitionId id) const noexcept;
  [[nodiscard]] const LinkedPartBinding* partLinkedBinding(
      DefinitionId id) const noexcept;
  [[nodiscard]] DefinitionStateId definitionStateId(
      DefinitionId id) const noexcept;
  [[nodiscard]] ComponentExtractionPrepareResult prepareComponentExtraction(
      const ComponentExtractionAddress& address) const noexcept;
  [[nodiscard]] ComponentExtractionStatus applyComponentExtraction(
      const ComponentExtractionTransaction& transaction,
      ProductDocumentSuccessor& prepareSuccessor,
      ComponentExtractionFailurePoint failurePoint =
          ComponentExtractionFailurePoint::None) noexcept;
  [[nodiscard]] ComponentExtractionStatus revertComponentExtraction(
      const ComponentExtractionTransaction& transaction,
      ProductDocumentSuccessor& prepareSuccessor,
      ComponentExtractionFailurePoint failurePoint =
          ComponentExtractionFailurePoint::None) noexcept;
  [[nodiscard]] ComponentPlacementPrepareResult prepareComponentPlacement(
      const ComponentPlacementRequest& request) const noexcept;
  [[nodiscard]] ComponentPlacementStatus applyComponentPlacement(
      const ComponentPlacementTransaction& transaction,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] ComponentPlacementStatus revertComponentPlacement(
      const ComponentPlacementTransaction& transaction,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] ComponentHomePlacementPrepareResult
  prepareComponentHomePlacement(
      const ComponentHomePlacementRequest& request) const noexcept;
  [[nodiscard]] ComponentHomePlacementBatchPrepareResult
  prepareAllComponentHomePlacements() const noexcept;
  [[nodiscard]] ComponentHomePlacementStatus applyComponentHomePlacement(
      const ComponentHomePlacementTransaction& transaction,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] ComponentHomePlacementStatus applyAllComponentHomePlacements(
      const std::vector<ComponentHomePlacementTransaction>& transactions,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] ComponentHomePlacementStatus revertComponentHomePlacement(
      const ComponentHomePlacementTransaction& transaction,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] ComponentHomePlacementStatus revertAllComponentHomePlacements(
      const std::vector<ComponentHomePlacementTransaction>& transactions,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] ComponentVisibilityStatus setComponentOccurrenceVisible(
      const OccurrencePath& path, bool visible,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] ComponentOccurrenceEditStatus renameComponentOccurrence(
      const OccurrencePath& path, std::string displayNameUtf8,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] ComponentOccurrenceEditStatus removeComponentOccurrence(
      const OccurrencePath& path,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] std::unique_ptr<Document> releaseRootDocument() && noexcept;

  [[nodiscard]] DefinitionInsertStatus insert(PartDefinition&& definition);
  [[nodiscard]] DefinitionInsertStatus insert(
      AssemblyDefinition&& definition);
  [[nodiscard]] ProductDocumentRemoveStatus remove(DefinitionId id);
  [[nodiscard]] ProductOccurrenceMutationStatus insertRootOccurrence(
      ComponentOccurrence&& occurrence);
  [[nodiscard]] ProductOccurrenceMutationStatus removeRootOccurrence(
      const OccurrencePath& path);
  [[nodiscard]] ProductOccurrenceMutationStatus setRootOccurrencePlacement(
      const OccurrencePath& path, RigidTransform3d placement) noexcept;
  [[nodiscard]] ProductOccurrenceMutationStatus setRootOccurrenceFlags(
      const OccurrencePath& path, OccurrenceFlags flags);
  // Lowest positive project-local occurrence ID eligible for a structural
  // Duplicate command. The authored root is bounded to 4096 occurrences;
  // this query never reserves or consumes an ID.
  [[nodiscard]] std::optional<OccurrenceId>
  lowestFreeRootOccurrenceId() const noexcept;
  // Narrow paired boundary used only by A5b Insert Apply/Redo. Every failure
  // is an exact no-op and returns ownership of the detached Document in
  // payload. IDs are caller-proposed project-local values; this boundary does
  // not consume any global or hidden cursor.
  [[nodiscard]] EmbeddedPartInsertStatus applyEmbeddedPartInsert(
      EmbeddedPartInsertPayload& payload) noexcept;
  [[nodiscard]] EmbeddedPartInsertStatus applyEmbeddedPartInsert(
      EmbeddedPartInsertPayload& payload,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] EmbeddedPartInsertStatus applyEmbeddedPartInsert(
      EmbeddedPartInsertPayload& payload,
      ProductDocumentSuccessor& prepareSuccessor,
      EmbeddedPartInsertFailurePoint failurePoint) noexcept;
  // Exact inverse used by A5b rollback/Undo. It refuses to extract when any
  // field differs or another occurrence references the definition.
  [[nodiscard]] EmbeddedPartInsertStatus extractEmbeddedPartInsert(
      EmbeddedPartInsertPayload& payload) noexcept;
  [[nodiscard]] EmbeddedPartInsertStatus extractEmbeddedPartInsert(
      EmbeddedPartInsertPayload& payload,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] DuplicateOccurrenceStatus applyDuplicateOccurrence(
      const DuplicateOccurrenceColdPayload& payload,
      ProductDocumentSuccessor& prepareSuccessor,
      DuplicateOccurrenceFailurePoint failurePoint) noexcept;
  [[nodiscard]] DuplicateOccurrenceStatus extractDuplicateOccurrence(
      const DuplicateOccurrenceColdPayload& payload,
      ProductDocumentSuccessor& prepareSuccessor,
      DuplicateOccurrenceFailurePoint failurePoint) noexcept;
  [[nodiscard]] RemoveOccurrenceStatus applyRemoveOccurrence(
      const RemoveOccurrenceColdPayload& payload,
      ProductDocumentSuccessor& prepareSuccessor,
      RemoveOccurrenceFailurePoint failurePoint) noexcept;
  [[nodiscard]] RemoveOccurrenceStatus restoreRemovedOccurrence(
      const RemoveOccurrenceColdPayload& payload,
      ProductDocumentSuccessor& prepareSuccessor,
      RemoveOccurrenceFailurePoint failurePoint) noexcept;
  [[nodiscard]] FlatOccurrenceResolveResult resolveRootOccurrence(
      const OccurrencePath& path) const noexcept;
  [[nodiscard]] RecursiveOccurrenceResolveResult resolveOccurrence(
      const OccurrencePath& path) const noexcept;
  [[nodiscard]] OccurrenceDeltaStatus applyOccurrenceDelta(
      const OccurrenceDelta& delta) noexcept;
  [[nodiscard]] OccurrenceDeltaStatus applyOccurrenceDelta(
      const OccurrenceDelta& delta,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] OccurrenceDeltaStatus revertOccurrenceDelta(
      const OccurrenceDelta& delta) noexcept;
  [[nodiscard]] OccurrenceDeltaStatus revertOccurrenceDelta(
      const OccurrenceDelta& delta,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] DefinitionEditResult applyDefinitionEdit(
      const DefinitionEditAddress& address, Document& candidate,
      ProductDocumentSuccessor& prepareSuccessor,
      DefinitionEditFailurePoint failurePoint =
          DefinitionEditFailurePoint::None) noexcept;
  [[nodiscard]] DefinitionEditResult revertDefinitionEdit(
      const DefinitionEditAddress& address, Document& candidate,
      ProductDocumentSuccessor& prepareSuccessor,
      DefinitionEditFailurePoint failurePoint =
          DefinitionEditFailurePoint::None) noexcept;
  [[nodiscard]] LinkedPartMutationResult applyLinkedPartReload(
      const DefinitionEditAddress& address,
      const LinkedPartBinding& expectedBinding,
      const LinkedPartBinding& replacementBinding, Document& candidate,
      ProductDocumentSuccessor& prepareSuccessor,
      DefinitionEditFailurePoint failurePoint =
          DefinitionEditFailurePoint::None) noexcept;
  [[nodiscard]] LinkedPartMutationResult applyLinkedPartRepair(
      const DefinitionEditAddress& address,
      const LinkedPartBinding& expectedBinding,
      const LinkedPartBinding& replacementBinding,
      ProductDocumentSuccessor& prepareSuccessor,
      DefinitionEditFailurePoint failurePoint =
          DefinitionEditFailurePoint::None) noexcept;
  [[nodiscard]] MatePlacementApplyStatus applyMatePlacementCandidate(
      const MatePlacementCandidate& candidate,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] MatePlacementApplyStatus applyPlanarMateAuthoring(
      const PlanarMateAuthoringTransaction& transaction,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] MatePlacementApplyStatus revertPlanarMateAuthoring(
      const PlanarMateAuthoringTransaction& transaction,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  // Persistence-only staging boundary.  It atomically installs authored
  // registry state on a disconnected product; existing authored mates are
  // never replaced.
  [[nodiscard]] AssemblyMateRestoreStatus restoreAssemblyMateState(
      DefinitionId ownerAssemblyId, const MateSolverOptions& options,
      std::vector<PlanarCoincidentMate> mates) noexcept;
  [[nodiscard]] AssemblyMateRestoreStatus restoreAssemblyMateRegistry(
      DefinitionId ownerAssemblyId,
      std::unique_ptr<MateRegistry> registry) noexcept;

 private:
  friend class ComponentExtractionDependencyTestAccess;
  ProductDocument(DefinitionRegistry&& definitions,
                  DefinitionId rootDefinitionId,
                  std::optional<ProductDocumentIdentity> identity) noexcept;
  [[nodiscard]] OccurrenceDeltaStatus mutateOccurrenceDeltaInternal(
      const OccurrenceDelta& delta, bool apply,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] DefinitionEditResult mutateDefinitionEditInternal(
      const DefinitionEditAddress& address, Document& candidate,
      ProductDocumentSuccessor& prepareSuccessor,
      DefinitionEditFailurePoint failurePoint) noexcept;
  [[nodiscard]] LinkedPartMutationResult validateLinkedPartMutation(
      const DefinitionEditAddress& address,
      const LinkedPartBinding& expectedBinding,
      PartDefinition** part) noexcept;
  DefinitionRegistry definitions_;
  DefinitionId rootDefinitionId_;
  std::optional<ProductDocumentIdentity> identity_;
  ProductIncarnationId incarnation_;
  // Shared by every assembly-product mutation. Successor preparation may
  // inspect the candidate graph but cannot re-enter another mutation path.
  bool productMutationTransactionActive_{};
};

struct ProductDocumentCreateResult {
  ProductDocumentStatus status{ProductDocumentStatus::InvalidRootId};
  std::unique_ptr<ProductDocument> product;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ProductDocumentStatus::Valid && product != nullptr;
  }
};

}  // namespace solidar
