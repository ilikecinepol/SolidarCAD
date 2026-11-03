#include "model/OccurrenceDelta.h"
#include "ui/application/AssemblyCommandHistory.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

using History = solidar::AssemblyCommandHistory;
using Result = solidar::AssemblyCommandHistoryResult;
using ExecuteSignature = Result (History::*)(
    solidar::ProductDocument&,
    solidar::OccurrenceDeltaCommand,
    solidar::ProductDocumentSuccessor&) noexcept;
using DuplicateSignature = Result (History::*)(
    solidar::ProductDocument&,
    solidar::DuplicateOccurrenceColdCommand,
    solidar::ProductDocumentSuccessor&) noexcept;
using RemoveSignature = Result (History::*)(
    solidar::ProductDocument&,
    solidar::RemoveOccurrenceColdCommand,
    solidar::ProductDocumentSuccessor&) noexcept;
using DefinitionEditSignature = Result (History::*)(
    solidar::ProductDocument&,
    solidar::DefinitionEditColdCommand,
    solidar::ProductDocumentSuccessor&) noexcept;
using MateSignature = Result (History::*)(
    solidar::ProductDocument&,
    solidar::PlanarMateColdCommand,
    solidar::ProductDocumentSuccessor&) noexcept;
using TransitionSignature = Result (History::*)(
    solidar::ProductDocument&,
    solidar::ProductDocumentSuccessor&) noexcept;
using ApplyInsertSignature = solidar::EmbeddedPartInsertStatus (
    solidar::ProductDocument::*)(solidar::EmbeddedPartInsertPayload&,
                                 solidar::ProductDocumentSuccessor&) noexcept;
using ExtractInsertSignature = ApplyInsertSignature;
using ResolveOccurrenceSignature = solidar::RecursiveOccurrenceResolveResult (
    solidar::ProductDocument::*)(const solidar::OccurrencePath&) const noexcept;

using ApplyDuplicateSignature = solidar::DuplicateOccurrenceStatus (
    solidar::ProductDocument::*)(
        const solidar::DuplicateOccurrenceColdPayload&,
        solidar::ProductDocumentSuccessor&,
        solidar::DuplicateOccurrenceFailurePoint) noexcept;
using ApplyRemoveSignature = solidar::RemoveOccurrenceStatus (
    solidar::ProductDocument::*)(
        const solidar::RemoveOccurrenceColdPayload&,
        solidar::ProductDocumentSuccessor&,
        solidar::RemoveOccurrenceFailurePoint) noexcept;

static_assert(std::variant_size_v<solidar::AssemblyCommand> == 8);
static_assert(std::is_same_v<
              decltype(solidar::AssemblyCommandSemanticState{}
                           .selectedOccurrence),
              std::optional<solidar::OccurrencePath>>);
static_assert(std::is_same_v<
              decltype(std::declval<const History&>().currentOccurrence()),
              const std::optional<solidar::OccurrencePath>&>);
static_assert(std::is_same_v<
              decltype(std::declval<const History&>().nextUndoState()),
              std::optional<std::reference_wrapper<
                  const solidar::AssemblyCommandSemanticState>>>);
static_assert(std::is_same_v<
              decltype(std::declval<const History&>().nextRedoState()),
              std::optional<std::reference_wrapper<
                  const solidar::AssemblyCommandSemanticState>>>);
static_assert(std::is_same_v<decltype(&solidar::ProductDocument::resolveOccurrence),
                             ResolveOccurrenceSignature>);
static_assert(std::is_same_v<
              std::variant_alternative_t<0, solidar::AssemblyCommand>,
              solidar::EmbeddedPartInsertColdCommand>);
static_assert(std::is_same_v<
              std::variant_alternative_t<1, solidar::AssemblyCommand>,
              solidar::OccurrenceDeltaCommand>);
static_assert(std::is_same_v<
              std::variant_alternative_t<2, solidar::AssemblyCommand>,
              solidar::DuplicateOccurrenceColdCommand>);
static_assert(std::is_same_v<
              std::variant_alternative_t<3, solidar::AssemblyCommand>,
              solidar::RemoveOccurrenceColdCommand>);
static_assert(std::is_same_v<
              std::variant_alternative_t<4, solidar::AssemblyCommand>,
              solidar::DefinitionEditColdCommand>);
static_assert(std::is_same_v<
              std::variant_alternative_t<5, solidar::AssemblyCommand>,
              solidar::LinkedPartReloadColdCommand>);
static_assert(std::is_same_v<
              std::variant_alternative_t<6, solidar::AssemblyCommand>,
              solidar::LinkedPartRepairColdCommand>);
static_assert(std::is_same_v<
              std::variant_alternative_t<7, solidar::AssemblyCommand>,
              solidar::PlanarMateColdCommand>);
static_assert(std::is_abstract_v<solidar::ProductDocumentSuccessor>);
static_assert(noexcept(std::declval<solidar::ProductDocumentSuccessor&>()
                           .prepare(std::declval<
                                    const solidar::ProductDocument&>(),
                                    solidar::ProductDocumentSuccessorImpact::TreeOnly)));
// Deliberately do not select an overload with static_cast: these expressions
// become ambiguous if a legacy overload is added in parallel.
static_assert(std::is_same_v<decltype(&History::executeOccurrenceDelta),
                             ExecuteSignature>);
static_assert(std::is_same_v<decltype(&History::executeDuplicate),
                             DuplicateSignature>);
static_assert(std::is_same_v<decltype(&History::executeRemove),
                             RemoveSignature>);
static_assert(std::is_same_v<decltype(&History::executeDefinitionEdit),
                             DefinitionEditSignature>);
static_assert(std::is_same_v<decltype(&History::executePlanarMate),
                             MateSignature>);
static_assert(
    std::is_same_v<decltype(&History::undo), TransitionSignature>);
static_assert(
    std::is_same_v<decltype(&History::redo), TransitionSignature>);
static_assert(std::is_same_v<
              decltype(static_cast<ApplyInsertSignature>(
                  &solidar::ProductDocument::applyEmbeddedPartInsert)),
              ApplyInsertSignature>);
static_assert(std::is_same_v<
              decltype(static_cast<ExtractInsertSignature>(
                  &solidar::ProductDocument::extractEmbeddedPartInsert)),
              ExtractInsertSignature>);
static_assert(std::is_nothrow_destructible_v<History>);
static_assert(std::is_same_v<
              decltype(&solidar::ProductDocument::applyDuplicateOccurrence),
              ApplyDuplicateSignature>);
static_assert(std::is_same_v<
              decltype(&solidar::ProductDocument::extractDuplicateOccurrence),
              ApplyDuplicateSignature>);
static_assert(std::is_same_v<
              decltype(&solidar::ProductDocument::applyRemoveOccurrence),
              ApplyRemoveSignature>);
static_assert(std::is_same_v<
              decltype(&solidar::ProductDocument::restoreRemovedOccurrence),
              ApplyRemoveSignature>);
using VisibilityFactory = solidar::OccurrenceDelta (*)(
    solidar::RootOccurrenceAddress, bool, bool) noexcept;
static_assert(std::is_same_v<decltype(&solidar::OccurrenceDelta::visibility),
                             VisibilityFactory>);
static_assert(std::is_same_v<
              decltype(std::declval<const solidar::OccurrenceDelta&>()
                           .visibilityChange()),
              const solidar::VisibilityOccurrenceDelta*>);

namespace {

bool excludesCallbackSyntax(const std::string& source,
                            const char* fileName) {
  const std::pair<std::string_view, std::regex> forbidden[]{
      {"void pointer", std::regex(R"(void\s*\*)")},
      {"raw function pointer", std::regex(R"(\(\s*\*)")},
      {"std::function", std::regex(R"(std::function\s*<)")},
      {"OccurrenceDeltaStatus output pointer",
       std::regex(R"(OccurrenceDeltaStatus\s*\*)")},
      {"AssemblyCommandSemanticState output pointer",
       std::regex(R"(AssemblyCommandSemanticState\s*\*)")}};
  for (const auto& [label, pattern] : forbidden) {
    if (std::regex_search(source, pattern)) {
      std::cerr << "forbidden public callback syntax '" << label << "' in "
                << fileName << '\n';
      return false;
    }
  }
  return true;
}

bool excludesForbiddenTokens(const char* fileName) {
  std::ifstream input(fileName, std::ios::binary);
  if (!input) {
    std::cerr << "cannot open public header: " << fileName << '\n';
    return false;
  }
  const std::string source((std::istreambuf_iterator<char>(input)), {});
  constexpr std::string_view forbidden[]{
      "void*",          "void *",          "std::function",
      "TopoDS",         "QObject",         "QModelIndex",
      "BodyRenderMesh", "RenderMesh",       "Gpu",
      "GPU",            "QOpenGL",          "OpenGL",
      "publication",    "Publication",
      "std::filesystem", "#include <filesystem>", "sourcePath", "QString",
      "std::unique_ptr<Document", "AssemblySuccessorPreparation"};
  for (const auto token : forbidden) {
    if (source.find(token) != std::string::npos) {
      std::cerr << "forbidden public-header token '" << token << "' in "
                << fileName << '\n';
      return false;
    }
  }
  return excludesCallbackSyntax(source, fileName);
}

bool excludesLegacyModelCallbacks(const char* fileName) {
  std::ifstream input(fileName, std::ios::binary);
  if (!input) {
    std::cerr << "cannot open model public header: " << fileName << '\n';
    return false;
  }
  const std::string source((std::istreambuf_iterator<char>(input)), {});
  constexpr std::string_view forbidden[]{"AssemblySuccessorPreparation"};
  for (const auto token : forbidden) {
    if (source.find(token) != std::string::npos) {
      std::cerr << "legacy callback token '" << token << "' in "
                << fileName << '\n';
      return false;
    }
  }
  return excludesCallbackSyntax(source, fileName);
}

}  // namespace

int main() {
  return excludesForbiddenTokens(SOLIDAR_ASSEMBLY_COMMAND_HISTORY_HEADER) &&
                 excludesForbiddenTokens(SOLIDAR_OCCURRENCE_DELTA_HEADER) &&
                 excludesForbiddenTokens(SOLIDAR_OCCURRENCE_DUPLICATE_HEADER) &&
                 excludesForbiddenTokens(SOLIDAR_OCCURRENCE_REMOVE_HEADER) &&
                 excludesLegacyModelCallbacks(SOLIDAR_PRODUCT_DOCUMENT_HEADER)
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
