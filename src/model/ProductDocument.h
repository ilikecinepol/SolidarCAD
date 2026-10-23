#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "model/Document.h"
#include "model/ProjectId.h"

namespace solidar {

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

class PartDefinition final {
 public:
  PartDefinition(DefinitionId id, std::unique_ptr<Document>&& document);
  ~PartDefinition() = default;

  PartDefinition(const PartDefinition&) = delete;
  PartDefinition& operator=(const PartDefinition&) = delete;
  PartDefinition(PartDefinition&&) noexcept = default;
  PartDefinition& operator=(PartDefinition&&) noexcept = default;

  [[nodiscard]] DefinitionId id() const noexcept { return id_; }
  [[nodiscard]] Document& document() noexcept { return *document_; }
  [[nodiscard]] const Document& document() const noexcept { return *document_; }

 private:
  friend class ProductDocument;

  [[nodiscard]] std::unique_ptr<Document> releaseDocument() && noexcept {
    return std::move(document_);
  }

  DefinitionId id_;
  std::unique_ptr<Document> document_;
};

enum class DefinitionInsertStatus {
  Inserted,
  DuplicateDefinitionId,
};

enum class DefinitionEraseStatus {
  Erased,
  DefinitionMissing,
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
  [[nodiscard]] DefinitionEraseStatus erase(DefinitionId id);

  [[nodiscard]] const PartDefinition* find(DefinitionId id) const noexcept;
  [[nodiscard]] bool contains(DefinitionId id) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return definitions_.size(); }

 private:
  friend class ProductDocument;

  std::map<DefinitionId, PartDefinition> definitions_;
};

enum class ProductDocumentStatus {
  Valid,
  InvalidRootId,
  MissingRootDefinition,
  InvalidProjectId,
};

enum class ProductDocumentRemoveStatus {
  Removed,
  Missing,
  RootProtected,
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

struct ProductDocumentCreateResult;

class ProductDocument final {
 public:
  ~ProductDocument() = default;

  ProductDocument(const ProductDocument&) = delete;
  ProductDocument& operator=(const ProductDocument&) = delete;
  ProductDocument(ProductDocument&&) noexcept = default;
  ProductDocument& operator=(ProductDocument&&) noexcept = default;

  [[nodiscard]] static ProductDocumentCreateResult create(
      DefinitionRegistry&& definitions, DefinitionId rootDefinitionId,
      std::optional<ProductDocumentIdentity> identity = std::nullopt);

  [[nodiscard]] const DefinitionRegistry& definitions() const noexcept {
    return definitions_;
  }
  [[nodiscard]] DefinitionId rootDefinitionId() const noexcept {
    return rootDefinitionId_;
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
  [[nodiscard]] const PartDefinition& rootDefinition() const noexcept;
  [[nodiscard]] Document& rootDocument() noexcept;
  [[nodiscard]] const Document& rootDocument() const noexcept;
  [[nodiscard]] std::unique_ptr<Document> releaseRootDocument() && noexcept;

  [[nodiscard]] DefinitionInsertStatus insert(PartDefinition&& definition);
  [[nodiscard]] ProductDocumentRemoveStatus remove(DefinitionId id);

 private:
  ProductDocument(DefinitionRegistry&& definitions,
                  DefinitionId rootDefinitionId,
                  std::optional<ProductDocumentIdentity> identity) noexcept;

  DefinitionRegistry definitions_;
  DefinitionId rootDefinitionId_;
  std::optional<ProductDocumentIdentity> identity_;
};

struct ProductDocumentCreateResult {
  ProductDocumentStatus status{ProductDocumentStatus::InvalidRootId};
  std::unique_ptr<ProductDocument> product;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ProductDocumentStatus::Valid && product != nullptr;
  }
};

}  // namespace solidar
