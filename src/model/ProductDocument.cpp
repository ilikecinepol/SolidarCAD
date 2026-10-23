#include "model/ProductDocument.h"

#include <stdexcept>
#include <utility>

namespace solidar {

PartDefinition::PartDefinition(DefinitionId id,
                               std::unique_ptr<Document>&& document)
    : id_(id) {
  if (!id.isValid())
    throw std::invalid_argument("PartDefinition requires a valid ID");
  if (!document)
    throw std::invalid_argument("PartDefinition requires a Document");
  document_ = std::move(document);
}

DefinitionInsertStatus DefinitionRegistry::insert(
    PartDefinition&& definition) {
  const auto id = definition.id();
  if (definitions_.contains(id))
    return DefinitionInsertStatus::DuplicateDefinitionId;
  definitions_.emplace(id, std::move(definition));
  return DefinitionInsertStatus::Inserted;
}

DefinitionEraseStatus DefinitionRegistry::erase(DefinitionId id) {
  if (definitions_.erase(id) == 0)
    return DefinitionEraseStatus::DefinitionMissing;
  return DefinitionEraseStatus::Erased;
}

const PartDefinition* DefinitionRegistry::find(DefinitionId id) const
    noexcept {
  const auto it = definitions_.find(id);
  return it == definitions_.end() ? nullptr : &it->second;
}

bool DefinitionRegistry::contains(DefinitionId id) const noexcept {
  return definitions_.contains(id);
}

ProductDocument::ProductDocument(DefinitionRegistry&& definitions,
                                 DefinitionId rootDefinitionId) noexcept
    : definitions_(std::move(definitions)),
      rootDefinitionId_(rootDefinitionId) {}

ProductDocumentCreateResult ProductDocument::create(
    DefinitionRegistry&& definitions, DefinitionId rootDefinitionId) {
  if (!rootDefinitionId.isValid())
    return {ProductDocumentStatus::InvalidRootId, nullptr};
  if (!definitions.contains(rootDefinitionId))
    return {ProductDocumentStatus::MissingRootDefinition, nullptr};

  return {ProductDocumentStatus::Valid,
          std::unique_ptr<ProductDocument>(
              new ProductDocument(std::move(definitions), rootDefinitionId))};
}

const PartDefinition& ProductDocument::rootDefinition() const noexcept {
  return *definitions_.find(rootDefinitionId_);
}

Document& ProductDocument::rootDocument() noexcept {
  return definitions_.definitions_.find(rootDefinitionId_)->second.document();
}

const Document& ProductDocument::rootDocument() const noexcept {
  return rootDefinition().document();
}

std::unique_ptr<Document> ProductDocument::releaseRootDocument() && noexcept {
  auto root = definitions_.definitions_.find(rootDefinitionId_);
  if (root == definitions_.definitions_.end()) return {};
  return std::move(root->second).releaseDocument();
}

DefinitionInsertStatus ProductDocument::insert(PartDefinition&& definition) {
  return definitions_.insert(std::move(definition));
}

ProductDocumentRemoveStatus ProductDocument::remove(DefinitionId id) {
  if (!definitions_.contains(id)) return ProductDocumentRemoveStatus::Missing;
  if (id == rootDefinitionId_)
    return ProductDocumentRemoveStatus::RootProtected;
  const auto erased = definitions_.erase(id);
  return erased == DefinitionEraseStatus::Erased
             ? ProductDocumentRemoveStatus::Removed
             : ProductDocumentRemoveStatus::Missing;
}

}  // namespace solidar
