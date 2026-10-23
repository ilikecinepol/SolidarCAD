#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

#include "model/Document.h"

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
  std::map<DefinitionId, PartDefinition> definitions_;
};

enum class ProductDocumentStatus {
  Valid,
  InvalidRootId,
  MissingRootDefinition,
};

enum class ProductDocumentRemoveStatus {
  Removed,
  Missing,
  RootProtected,
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
      DefinitionRegistry&& definitions, DefinitionId rootDefinitionId);

  [[nodiscard]] const DefinitionRegistry& definitions() const noexcept {
    return definitions_;
  }
  [[nodiscard]] DefinitionId rootDefinitionId() const noexcept {
    return rootDefinitionId_;
  }
  [[nodiscard]] const PartDefinition& rootDefinition() const noexcept;

  [[nodiscard]] DefinitionInsertStatus insert(PartDefinition&& definition);
  [[nodiscard]] ProductDocumentRemoveStatus remove(DefinitionId id);

 private:
  ProductDocument(DefinitionRegistry&& definitions,
                  DefinitionId rootDefinitionId) noexcept;

  DefinitionRegistry definitions_;
  DefinitionId rootDefinitionId_;
};

struct ProductDocumentCreateResult {
  ProductDocumentStatus status{ProductDocumentStatus::InvalidRootId};
  std::unique_ptr<ProductDocument> product;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ProductDocumentStatus::Valid && product != nullptr;
  }
};

}  // namespace solidar
