#include "ui/ViewportRenderer.h"
#include "ui/ViewportCamera.h"
#include "model/GeometryOperation.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <unordered_set>

namespace solidar {
namespace {

struct GpuVertex {
  float position[3];
  float normal[3];
  float faceIndex;
};

struct GpuEdgeVertex {
  float position[3];
  float edgeIndex;
};

struct VaoReleaseGuard {
  QOpenGLVertexArrayObject* vao{};
  ~VaoReleaseGuard() {
    if (vao) vao->release();
  }
};

const char* surfaceVertexShader = R"(
  #version 130
  in vec3 aPosition;
  in vec3 aNormal;
  in float aFaceIndex;
  uniform mat4 uMvp;
  uniform mat3 uNormalMatrix;
  out vec3 vNormal;
  flat out int vFaceIndex;
  void main() {
    gl_Position = uMvp * vec4(aPosition, 1.0);
    vNormal = normalize(uNormalMatrix * aNormal);
    vFaceIndex = int(aFaceIndex + 0.5);
  })";

const char* surfaceFragmentShader = R"(
  #version 130
  in vec3 vNormal;
  flat in int vFaceIndex;
  uniform int uHighlightMode;
  uniform bool uPreview;
  uniform bool uCutPreview;
  uniform vec4 uSurfaceColor;
  uniform vec4 uPreviewColor;
  uniform vec4 uCutPreviewColor;
  uniform vec4 uSelectedColor;
  uniform vec4 uHoverColor;
  uniform vec4 uSpecularColor;
  out vec4 color;
  void main() {
    vec3 n = normalize(vNormal);
    vec3 key = normalize(vec3(0.28, -0.42, 0.86));
    vec3 fill = normalize(vec3(-0.55, 0.30, 0.48));
    float diffuse = max(dot(n, key), 0.0);
    float fillDiffuse = max(dot(n, fill), 0.0);
    float specular = pow(max(dot(reflect(-key, n), vec3(0, 0, 1)), 0.0), 32.0);
    vec4 semantic = uCutPreview ? uCutPreviewColor
                                : uPreview ? uPreviewColor
                                           : uSurfaceColor;
    vec3 base = semantic.rgb;
    if (uHighlightMode == 1)
      base = mix(base, uSelectedColor.rgb, 0.58);
    else if (uHighlightMode == 2)
      base = mix(base, uHoverColor.rgb, 0.38);
    float alpha = semantic.a;
    color = vec4(base * (0.46 + 0.38 * diffuse + 0.16 * fillDiffuse) +
                 uSpecularColor.rgb * (0.10 * specular), alpha);
  })";

const char* edgeVertexShader = R"(
  #version 130
  in vec3 aPosition;
  in float aEdgeIndex;
  uniform mat4 uMvp;
  flat out int vEdgeIndex;
  void main() {
    gl_Position = uMvp * vec4(aPosition, 1.0);
    vEdgeIndex = int(aEdgeIndex + 0.5);
  })";

const char* edgeFragmentShader = R"(
  #version 130
  flat in int vEdgeIndex;
  uniform int uHighlightMode;
  uniform bool uOrdinaryEdges;
  uniform vec4 uEdgeColor;
  uniform vec4 uSelectedColor;
  uniform vec4 uHoverColor;
  out vec4 color;
  void main() {
    if (!uOrdinaryEdges && uHighlightMode == 0) discard;
    color = uHighlightMode == 1 ? uSelectedColor
            : uHighlightMode == 2 ? uHoverColor
                                  : uEdgeColor;
  })";

}  // namespace

struct ViewportRenderer::GpuMesh {
  struct FaceIndexSpan {
    std::uint32_t begin{};
    std::uint32_t count{};
  };
  QOpenGLBuffer vertices{QOpenGLBuffer::VertexBuffer};
  QOpenGLBuffer indices{QOpenGLBuffer::IndexBuffer};
  QOpenGLBuffer edges{QOpenGLBuffer::VertexBuffer};
  QOpenGLBuffer selectedIndices{QOpenGLBuffer::IndexBuffer};
  QOpenGLBuffer hoveredIndices{QOpenGLBuffer::IndexBuffer};
  QOpenGLBuffer selectedEdges{QOpenGLBuffer::VertexBuffer};
  QOpenGLBuffer hoveredEdges{QOpenGLBuffer::VertexBuffer};
  QOpenGLVertexArrayObject surfaceVao;
  QOpenGLVertexArrayObject edgeVao;
  QOpenGLVertexArrayObject selectedEdgeVao;
  QOpenGLVertexArrayObject hoveredEdgeVao;
  std::optional<BodyMeshKey> identity;
  std::uint64_t revision{std::numeric_limits<std::uint64_t>::max()};
  int indexCount{};
  int edgeVertexCount{};
  int selectedIndexCount{};
  int hoveredIndexCount{};
  int selectedEdgeVertexCount{};
  int hoveredEdgeVertexCount{};
  std::vector<std::size_t> selectedFaceKey;
  std::vector<std::size_t> hoveredFaceKey;
  std::vector<std::size_t> selectedEdgeKey;
  std::vector<std::size_t> hoveredEdgeKey;
  // Compact lookup metadata only. Canonical indices and edge points remain in
  // BodyRenderMesh; overlay vertices/indices are materialized for the current
  // selection and immediately uploaded, never retained as a second full mesh.
  std::vector<FaceIndexSpan> faceIndexSpans;
  std::vector<std::uint32_t> edgeSourceIndices;
  std::uint64_t uploadGeneration{};
  std::uint64_t contentHash{};
  std::size_t highlightFaceIndexVisits{};
  std::size_t highlightEdgeVertexVisits{};
  std::size_t topologyEdgeCount{};
};

QVector4D colorVector(const QColor& color) {
  return {color.redF(), color.greenF(), color.blueF(), color.alphaF()};
}

namespace {

template <typename Value>
void hashValues(std::uint64_t& hash, std::span<const Value> values) noexcept {
  const auto bytes = std::as_bytes(values);
  for (const std::byte value : bytes) {
    hash ^= std::to_integer<std::uint8_t>(value);
    hash *= 1099511628211ULL;
  }
}

std::uint64_t meshContentHash(const std::vector<GpuVertex>& vertices,
                              const std::vector<std::uint32_t>& indices,
                              const std::vector<GpuEdgeVertex>& edges) noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  hashValues(hash, std::span<const GpuVertex>(vertices));
  hashValues(hash, std::span<const std::uint32_t>(indices));
  hashValues(hash, std::span<const GpuEdgeVertex>(edges));
  return hash;
}

}  // namespace

ViewportRenderer::ViewportRenderer()
    : preview_(std::make_unique<GpuMesh>()),
      cutPreview_(std::make_unique<GpuMesh>()) {}

ViewportRenderer::~ViewportRenderer() {
  try {
    release();
  } catch (...) {
  }
}

bool ViewportRenderer::initialize() {
  QOpenGLContext* current = QOpenGLContext::currentContext();
  if (initialized_) {
    if (currentContextIsCompatible()) return true;
    error_ = QStringLiteral("OpenGL context does not own renderer resources");
    return false;
  }
  if (!current) {
    error_ = QStringLiteral("OpenGL context is unavailable");
    return false;
  }
  context_ = current;
  error_.clear();
  auto compile = [this](QOpenGLShaderProgram& program, const char* vertex,
                        const char* fragment, bool edgeProgram) {
    program.removeAllShaders();
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertex)) {
      error_ = program.log();
      return false;
    }
    if (!program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment)) {
      error_ = program.log();
      return false;
    }

    // GLSL 1.30 predates layout(location=...). Keep the same attribute
    // indices expected by upload() by assigning them through Qt before link.
    program.bindAttributeLocation("aPosition", 0);
    if (edgeProgram) {
      program.bindAttributeLocation("aEdgeIndex", 1);
    } else {
      program.bindAttributeLocation("aNormal", 1);
      program.bindAttributeLocation("aFaceIndex", 2);
    }

    if (!program.link()) {
      error_ = program.log();
      return false;
    }
    return true;
  };
  initialized_ = compile(surfaceProgram_, surfaceVertexShader,
                         surfaceFragmentShader, false) &&
                 compile(edgeProgram_, edgeVertexShader, edgeFragmentShader,
                         true);
  if (!initialized_) {
    // Roll back any partial program state so a failed initialization leaves no
    // stale shaders or GPU program objects behind. Without this, a surface
    // program that linked before an edge-program failure would outlive
    // initialize() while release() early-returns on !initialized_.
    surfaceProgram_.removeAllShaders();
    edgeProgram_.removeAllShaders();
    context_.clear();
  }
  return initialized_;
}

bool ViewportRenderer::currentContextIsCompatible() const noexcept {
  QOpenGLContext* current = QOpenGLContext::currentContext();
  if (!current || !context_) return false;
  return current == context_ ||
         (current->shareGroup() &&
          current->shareGroup() == context_->shareGroup());
}

bool ViewportRenderer::bindVertexArrayChecked(
    QOpenGLVertexArrayObject& vao) noexcept {
  if (!vao.isCreated() || !QOpenGLContext::currentContext()) return false;
  if (vaoBindFailureCountdown_ > 0 && --vaoBindFailureCountdown_ == 0)
    return false;
  vao.bind();
  GLint binding = 0;
  constexpr GLenum kVertexArrayBinding = 0x85B5;
  QOpenGLContext::currentContext()->functions()->glGetIntegerv(
      kVertexArrayBinding, &binding);
  const bool matches = static_cast<GLuint>(binding) == vao.objectId();
  if (!matches) vao.release();
  return matches;
}

void ViewportRenderer::destroyGpuMesh(GpuMesh& gpu,
                                      bool destroyGlObjects) noexcept {
  if (destroyGlObjects) {
    gpu.surfaceVao.destroy();
    gpu.edgeVao.destroy();
    gpu.selectedEdgeVao.destroy();
    gpu.hoveredEdgeVao.destroy();
    gpu.vertices.destroy();
    gpu.indices.destroy();
    gpu.edges.destroy();
    gpu.selectedIndices.destroy();
    gpu.hoveredIndices.destroy();
    gpu.selectedEdges.destroy();
    gpu.hoveredEdges.destroy();
  }
  gpu.identity.reset();
  gpu.revision = std::numeric_limits<std::uint64_t>::max();
  gpu.indexCount = gpu.edgeVertexCount = 0;
  gpu.selectedIndexCount = gpu.hoveredIndexCount = 0;
  gpu.selectedEdgeVertexCount = gpu.hoveredEdgeVertexCount = 0;
  gpu.selectedFaceKey.clear();
  gpu.hoveredFaceKey.clear();
  gpu.selectedEdgeKey.clear();
  gpu.hoveredEdgeKey.clear();
  gpu.faceIndexSpans.clear();
  gpu.edgeSourceIndices.clear();
  gpu.uploadGeneration = 0;
  gpu.contentHash = 0;
  gpu.highlightFaceIndexVisits = 0;
  gpu.highlightEdgeVertexVisits = 0;
  gpu.topologyEdgeCount = 0;
}

void ViewportRenderer::release() {
  if (initialized_ && QOpenGLContext::currentContext() &&
      !currentContextIsCompatible()) {
    error_ = QStringLiteral(
        "Refusing to release renderer resources from an unrelated OpenGL context");
    return;
  }
  const bool canDestroyGlObjects = currentContextIsCompatible();
  destroyGpuMesh(*preview_, canDestroyGlObjects);
  destroyGpuMesh(*cutPreview_, canDestroyGlObjects);
  for (auto& [identity, source] : sources_) {
    static_cast<void>(identity);
    if (source) destroyGpuMesh(*source, canDestroyGlObjects);
  }
  sources_.clear();
  if (canDestroyGlObjects) {
    surfaceProgram_.removeAllShaders();
    edgeProgram_.removeAllShaders();
  }
  initialized_ = false;
  context_.clear();
}

void ViewportRenderer::synchronizeResources(
    const std::vector<BodyMeshKey>& activeSources, bool keepPreview,
    bool keepCutPreview) {
  if (!initialized_ || !currentContextIsCompatible()) return;
  try {
    const std::unordered_set<BodyMeshKey, BodyMeshKeyHash> active(
        activeSources.begin(), activeSources.end());
    for (auto iterator = sources_.begin(); iterator != sources_.end();) {
      if (iterator->second && active.contains(iterator->first)) {
        ++iterator;
        continue;
      }
      if (iterator->second) destroyGpuMesh(*iterator->second, true);
      iterator = sources_.erase(iterator);
    }
    if (!keepPreview) destroyGpuMesh(*preview_, true);
    if (!keepCutPreview) destroyGpuMesh(*cutPreview_, true);
  } catch (...) {
    try {
      error_ = QStringLiteral("Renderer resource synchronization failed");
    } catch (...) {
    }
  }
}

RendererResourceDiagnostics
ViewportRenderer::resourceDiagnostics() const noexcept {
  RendererResourceDiagnostics result;
  result.initialized = initialized_;
  result.sourceMeshes = sources_.size();
  result.previewResident = preview_ && preview_->identity.has_value();
  result.cutPreviewResident = cutPreview_ && cutPreview_->identity.has_value();
  result.ordinarySourceMeshes = lastOrdinarySourceMeshes_;
  result.highlightOnlySourceMeshes = lastHighlightOnlySourceMeshes_;
  result.highlightOverlayMeshes = lastHighlightOverlayMeshes_;
  for (const auto& [identity, gpu] : sources_) {
    static_cast<void>(identity);
    if (!gpu) continue;
    result.selectedFaces += gpu->selectedFaceKey.size();
    result.hoveredFaces += gpu->hoveredFaceKey.size();
    result.selectedEdges += gpu->selectedEdgeKey.size();
    result.hoveredEdges += gpu->hoveredEdgeKey.size();
    result.highlightFaceIndexVisits += gpu->highlightFaceIndexVisits;
    result.highlightEdgeVertexVisits += gpu->highlightEdgeVertexVisits;
    result.highlightLookupBytes +=
        gpu->faceIndexSpans.capacity() * sizeof(GpuMesh::FaceIndexSpan) +
        gpu->edgeSourceIndices.capacity() * sizeof(std::uint32_t);
  }
  if (preview_) {
    result.previewUploadGeneration = preview_->uploadGeneration;
    result.previewContentHash = preview_->contentHash;
    result.previewIndexCount = static_cast<std::size_t>(preview_->indexCount);
  }
  if (cutPreview_) {
    result.cutPreviewUploadGeneration = cutPreview_->uploadGeneration;
    result.cutPreviewContentHash = cutPreview_->contentHash;
    result.cutPreviewIndexCount =
        static_cast<std::size_t>(cutPreview_->indexCount);
  }
  return result;
}

bool ViewportRenderer::upload(GpuMesh& gpu, const BodyRenderMesh& mesh,
                              const BodyMeshKey& identity) {
  if (gpu.identity && *gpu.identity == identity &&
      gpu.revision == mesh.revision())
    return true;
  QElapsedTimer timer;
  timer.start();
  std::vector<GpuVertex> vertices;
  vertices.reserve(mesh.vertices().size());
  for (const auto& vertex : mesh.vertices())
    vertices.push_back({{static_cast<float>(vertex.position.x),
                         static_cast<float>(vertex.position.y),
                         static_cast<float>(vertex.position.z)},
                        {static_cast<float>(vertex.normal.x),
                         static_cast<float>(vertex.normal.y),
                         static_cast<float>(vertex.normal.z)},
                        static_cast<float>(vertex.faceIndex)});
  std::vector<GpuEdgeVertex> edges;
  for (const auto& edge : mesh.edges()) {
    for (std::size_t i = 1; i < edge.points.size(); ++i) {
      for (const auto& p : {edge.points[i - 1], edge.points[i]})
        edges.push_back({{static_cast<float>(p.x), static_cast<float>(p.y),
                          static_cast<float>(p.z)},
                         static_cast<float>(edge.edgeIndex)});
    }
  }

  // Build compact direct-index lookup tables once per mesh upload. The mesh
  // builder appends each face's triangles contiguously, so two uint32 values
  // per face are sufficient; canonical index/edge data is never duplicated.
  std::vector<GpuMesh::FaceIndexSpan> faceIndexSpans(mesh.faceCount());
  const auto& sourceIndices = mesh.triangleIndices();
  for (std::size_t index = 0; index + 2 < sourceIndices.size(); index += 3) {
    const std::uint32_t first = sourceIndices[index];
    if (first >= mesh.vertices().size()) {
      error_ = QStringLiteral("Renderer mesh index is out of range");
      return false;
    }
    const std::uint32_t face = mesh.vertices()[first].faceIndex;
    if (face >= faceIndexSpans.size() ||
        index > std::numeric_limits<std::uint32_t>::max()) {
      error_ = QStringLiteral("Renderer mesh topology is out of range");
      return false;
    }
    auto& span = faceIndexSpans[face];
    if (span.count == 0) {
      span.begin = static_cast<std::uint32_t>(index);
      span.count = 3;
    } else if (static_cast<std::size_t>(span.begin) + span.count == index &&
               span.count <= std::numeric_limits<std::uint32_t>::max() - 3) {
      span.count += 3;
    } else {
      error_ = QStringLiteral("Renderer face triangles are not contiguous");
      return false;
    }
  }
  std::size_t topologyEdgeCount = 0;
  for (const auto& edge : mesh.edges())
    topologyEdgeCount = std::max(topologyEdgeCount, edge.edgeIndex + 1);
  constexpr std::uint32_t missingEdge =
      std::numeric_limits<std::uint32_t>::max();
  std::vector<std::uint32_t> edgeSourceIndices(topologyEdgeCount, missingEdge);
  for (std::size_t sourceIndex = 0; sourceIndex < mesh.edges().size();
       ++sourceIndex) {
    const auto& edge = mesh.edges()[sourceIndex];
    if (sourceIndex > std::numeric_limits<std::uint32_t>::max() ||
        edge.edgeIndex >= edgeSourceIndices.size() ||
        edgeSourceIndices[edge.edgeIndex] != missingEdge) {
      error_ = QStringLiteral("Renderer edge topology is invalid");
      return false;
    }
    edgeSourceIndices[edge.edgeIndex] =
        static_cast<std::uint32_t>(sourceIndex);
  }

  const auto checkedBytes = [](std::size_t count, std::size_t elementSize) {
    if (count > static_cast<std::size_t>(std::numeric_limits<int>::max()) /
                    elementSize)
      return std::optional<int>{};
    return std::optional<int>{static_cast<int>(count * elementSize)};
  };
  const auto vertexBytes = checkedBytes(vertices.size(), sizeof(GpuVertex));
  const auto indexBytes =
      checkedBytes(mesh.triangleIndices().size(), sizeof(std::uint32_t));
  const auto edgeBytes = checkedBytes(edges.size(), sizeof(GpuEdgeVertex));
  if (!vertexBytes || !indexBytes || !edgeBytes) {
    error_ = QStringLiteral("Renderer GPU buffer size is out of range");
    return false;
  }
  const auto ensureBuffer = [](QOpenGLBuffer& buffer) {
    return buffer.isCreated() || buffer.create();
  };
  const auto ensureVao = [](QOpenGLVertexArrayObject& vao) {
    return vao.isCreated() || vao.create();
  };
  if (!ensureBuffer(gpu.vertices) || !ensureBuffer(gpu.indices) ||
      !ensureBuffer(gpu.edges) || !ensureVao(gpu.surfaceVao) ||
      !ensureVao(gpu.edgeVao)) {
    error_ = QStringLiteral("Renderer GPU resource creation failed");
    return false;
  }

  {
    if (!bindVertexArrayChecked(gpu.surfaceVao)) {
      error_ = QStringLiteral("Renderer surface VAO bind failed");
      return false;
    }
    VaoReleaseGuard vaoGuard{&gpu.surfaceVao};
    if (!gpu.vertices.bind()) {
      error_ = QStringLiteral("Renderer vertex buffer bind failed");
      return false;
    }
    gpu.vertices.allocate(vertices.data(), *vertexBytes);
    if (gpu.vertices.size() != *vertexBytes) {
      gpu.vertices.release();
      error_ = QStringLiteral("Renderer vertex buffer allocation failed");
      return false;
    }
    gpu.vertices.release();
    if (uploadFailureCountdown_ > 0 && --uploadFailureCountdown_ == 0) {
      error_ = QStringLiteral("Injected renderer upload failure");
      return false;
    }
    if (!gpu.indices.bind()) {
      error_ = QStringLiteral("Renderer index buffer bind failed");
      return false;
    }
    gpu.indices.allocate(mesh.triangleIndices().data(), *indexBytes);
    if (gpu.indices.size() != *indexBytes) {
      gpu.indices.release();
      error_ = QStringLiteral("Renderer index buffer allocation failed");
      return false;
    }
    if (!gpu.vertices.bind()) {
      gpu.indices.release();
      error_ = QStringLiteral("Renderer vertex buffer rebind failed");
      return false;
    }
    if (!surfaceProgram_.bind()) {
      gpu.indices.release();
      gpu.vertices.release();
      error_ = QStringLiteral("Renderer surface shader bind failed");
      return false;
    }
    surfaceProgram_.enableAttributeArray(0);
    surfaceProgram_.setAttributeBuffer(
        0, GL_FLOAT, offsetof(GpuVertex, position), 3, sizeof(GpuVertex));
    surfaceProgram_.enableAttributeArray(1);
    surfaceProgram_.setAttributeBuffer(
        1, GL_FLOAT, offsetof(GpuVertex, normal), 3, sizeof(GpuVertex));
    surfaceProgram_.enableAttributeArray(2);
    surfaceProgram_.setAttributeBuffer(
        2, GL_FLOAT, offsetof(GpuVertex, faceIndex), 1, sizeof(GpuVertex));
    surfaceProgram_.release();
    gpu.indices.release();
    gpu.vertices.release();
  }

  {
    if (!bindVertexArrayChecked(gpu.edgeVao)) {
      error_ = QStringLiteral("Renderer edge VAO bind failed");
      return false;
    }
    VaoReleaseGuard vaoGuard{&gpu.edgeVao};
    if (!gpu.edges.bind()) {
      error_ = QStringLiteral("Renderer edge buffer bind failed");
      return false;
    }
    gpu.edges.allocate(edges.data(), *edgeBytes);
    if (gpu.edges.size() != *edgeBytes) {
      gpu.edges.release();
      error_ = QStringLiteral("Renderer edge buffer allocation failed");
      return false;
    }
    if (!edgeProgram_.bind()) {
      gpu.edges.release();
      error_ = QStringLiteral("Renderer edge shader bind failed");
      return false;
    }
    edgeProgram_.enableAttributeArray(0);
    edgeProgram_.setAttributeBuffer(
        0, GL_FLOAT, offsetof(GpuEdgeVertex, position), 3,
        sizeof(GpuEdgeVertex));
    edgeProgram_.enableAttributeArray(1);
    edgeProgram_.setAttributeBuffer(
        1, GL_FLOAT, offsetof(GpuEdgeVertex, edgeIndex), 1,
        sizeof(GpuEdgeVertex));
    edgeProgram_.release();
    gpu.edges.release();
  }

  gpu.indexCount = static_cast<int>(mesh.triangleIndices().size());
  gpu.edgeVertexCount = static_cast<int>(edges.size());
  gpu.identity = identity;
  gpu.revision = mesh.revision();
  gpu.selectedFaceKey.clear();
  gpu.hoveredFaceKey.clear();
  gpu.selectedEdgeKey.clear();
  gpu.hoveredEdgeKey.clear();
  gpu.selectedIndexCount = gpu.hoveredIndexCount = 0;
  gpu.selectedEdgeVertexCount = gpu.hoveredEdgeVertexCount = 0;
  gpu.faceIndexSpans = std::move(faceIndexSpans);
  gpu.edgeSourceIndices = std::move(edgeSourceIndices);
  gpu.topologyEdgeCount = topologyEdgeCount;
  gpu.uploadGeneration = ++uploadGenerationClock_;
  gpu.contentHash = meshContentHash(vertices, mesh.triangleIndices(), edges);
  gpu.highlightFaceIndexVisits = 0;
  gpu.highlightEdgeVertexVisits = 0;
  lastUploadMilliseconds_ = timer.nsecsElapsed() / 1'000'000.0;
  return true;
}

RendererHighlightRepresentation
ViewportRenderer::buildHighlightRepresentation(
    std::size_t faceOffset, std::size_t faceCount, std::size_t edgeOffset,
    std::size_t edgeCount, const std::vector<std::size_t>& selectedFaces,
    const std::vector<std::size_t>& hoveredFaces,
    const std::vector<std::size_t>& selectedEdges, std::size_t hoveredEdge) {
  RendererHighlightRepresentation result;
  const auto localize = [](const std::vector<std::size_t>& values,
                           std::size_t offset, std::size_t count) {
    std::vector<std::size_t> local;
    local.reserve(values.size());
    for (const std::size_t value : values)
      if (value >= offset && value - offset < count)
        local.push_back(value - offset);
    std::sort(local.begin(), local.end());
    local.erase(std::unique(local.begin(), local.end()), local.end());
    return local;
  };
  result.selectedFaces = localize(selectedFaces, faceOffset, faceCount);
  result.hoveredFaces = localize(hoveredFaces, faceOffset, faceCount);
  result.selectedEdges = localize(selectedEdges, edgeOffset, edgeCount);
  if (hoveredEdge >= edgeOffset && hoveredEdge - edgeOffset < edgeCount)
    result.hoveredEdge = hoveredEdge - edgeOffset;
  return result;
}

void ViewportRenderer::prepareHighlights(
    GpuMesh& gpu, const BodyRenderMesh& mesh, std::size_t faceOffset,
    std::size_t edgeOffset, const std::vector<std::size_t>& selectedFaces,
    const std::vector<std::size_t>& hoveredFaces,
    const std::vector<std::size_t>& selectedEdges, std::size_t hoveredEdge) {
  gpu.highlightFaceIndexVisits = 0;
  gpu.highlightEdgeVertexVisits = 0;
  const auto representation = buildHighlightRepresentation(
      faceOffset, mesh.faceCount(), edgeOffset, gpu.topologyEdgeCount,
      selectedFaces,
      hoveredFaces, selectedEdges, hoveredEdge);

  const auto uploadFaceOverlay = [&](const std::vector<std::size_t>& faces,
                                     std::vector<std::size_t>& key,
                                     QOpenGLBuffer& buffer, int& count) {
    if (faces == key) return;
    std::vector<std::uint32_t> indices;
    const auto& canonical = mesh.triangleIndices();
    for (const std::size_t face : faces) {
      if (face >= gpu.faceIndexSpans.size()) continue;
      const auto span = gpu.faceIndexSpans[face];
      if (span.count == 0) continue;
      const std::size_t begin = span.begin;
      const std::size_t end = begin + span.count;
      if (end > canonical.size())
        throw std::runtime_error("Renderer face lookup is inconsistent");
      gpu.highlightFaceIndexVisits += span.count;
      indices.insert(indices.end(), canonical.begin() + begin,
                     canonical.begin() + end);
    }
    if (!buffer.isCreated() && !buffer.create())
      throw std::runtime_error("Renderer face overlay buffer creation failed");
    if (indices.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max()) /
            sizeof(std::uint32_t))
      throw std::runtime_error("Renderer face overlay is too large");
    const int bytes =
        static_cast<int>(indices.size() * sizeof(std::uint32_t));
    if (!buffer.bind())
      throw std::runtime_error("Renderer face overlay buffer bind failed");
    buffer.allocate(indices.data(), bytes);
    if (buffer.size() != bytes) {
      buffer.release();
      throw std::runtime_error("Renderer face overlay allocation failed");
    }
    buffer.release();
    count = static_cast<int>(indices.size());
    key = faces;
  };
  uploadFaceOverlay(representation.selectedFaces, gpu.selectedFaceKey,
                    gpu.selectedIndices, gpu.selectedIndexCount);
  uploadFaceOverlay(representation.hoveredFaces, gpu.hoveredFaceKey,
                    gpu.hoveredIndices, gpu.hoveredIndexCount);

  const auto uploadEdgeOverlay = [&](const std::vector<std::size_t>& edges,
                                     std::vector<std::size_t>& key,
                                     QOpenGLBuffer& buffer,
                                     QOpenGLVertexArrayObject& vao,
                                     int& count) {
    if (edges == key) return;
    std::vector<GpuEdgeVertex> vertices;
    for (const std::size_t edge : edges) {
      if (edge >= gpu.edgeSourceIndices.size()) continue;
      const std::uint32_t sourceIndex = gpu.edgeSourceIndices[edge];
      if (sourceIndex == std::numeric_limits<std::uint32_t>::max()) continue;
      if (sourceIndex >= mesh.edges().size())
        throw std::runtime_error("Renderer edge lookup is inconsistent");
      const auto& source = mesh.edges()[sourceIndex];
      if (source.points.size() < 2) continue;
      vertices.reserve(vertices.size() + (source.points.size() - 1) * 2);
      for (std::size_t index = 1; index < source.points.size(); ++index) {
        for (const auto& point : {source.points[index - 1],
                                  source.points[index]}) {
          vertices.push_back({{static_cast<float>(point.x),
                               static_cast<float>(point.y),
                               static_cast<float>(point.z)},
                              static_cast<float>(source.edgeIndex)});
          ++gpu.highlightEdgeVertexVisits;
        }
      }
    }
    if (!buffer.isCreated() && !buffer.create())
      throw std::runtime_error("Renderer edge overlay buffer creation failed");
    if (!vao.isCreated() && !vao.create())
      throw std::runtime_error("Renderer edge overlay VAO creation failed");
    if (vertices.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max()) /
            sizeof(GpuEdgeVertex))
      throw std::runtime_error("Renderer edge overlay is too large");
    const int bytes =
        static_cast<int>(vertices.size() * sizeof(GpuEdgeVertex));
    if (!bindVertexArrayChecked(vao))
      throw std::runtime_error("Renderer edge overlay VAO bind failed");
    VaoReleaseGuard vaoGuard{&vao};
    if (!buffer.bind())
      throw std::runtime_error("Renderer edge overlay buffer bind failed");
    buffer.allocate(vertices.data(), bytes);
    if (buffer.size() != bytes) {
      buffer.release();
      throw std::runtime_error("Renderer edge overlay allocation failed");
    }
    if (!edgeProgram_.bind()) {
      buffer.release();
      throw std::runtime_error("Renderer edge overlay shader bind failed");
    }
    edgeProgram_.enableAttributeArray(0);
    edgeProgram_.setAttributeBuffer(0, GL_FLOAT,
                                    offsetof(GpuEdgeVertex, position), 3,
                                    sizeof(GpuEdgeVertex));
    edgeProgram_.enableAttributeArray(1);
    edgeProgram_.setAttributeBuffer(1, GL_FLOAT,
                                    offsetof(GpuEdgeVertex, edgeIndex), 1,
                                    sizeof(GpuEdgeVertex));
    edgeProgram_.release();
    buffer.release();
    count = static_cast<int>(vertices.size());
    key = edges;
  };
  uploadEdgeOverlay(representation.selectedEdges, gpu.selectedEdgeKey,
                    gpu.selectedEdges, gpu.selectedEdgeVao,
                    gpu.selectedEdgeVertexCount);
  const std::vector<std::size_t> hovered = representation.hoveredEdge
                                               ? std::vector<std::size_t>{
                                                     *representation.hoveredEdge}
                                               : std::vector<std::size_t>{};
  uploadEdgeOverlay(hovered, gpu.hoveredEdgeKey, gpu.hoveredEdges,
                    gpu.hoveredEdgeVao, gpu.hoveredEdgeVertexCount);
}

QMatrix4x4 ViewportRenderer::projectionMatrix(
    const QSize& size, float yawDeg, float pitchDeg, float zoom, QPointF pan,
    double depthExtent, Point3d center) {
  return ViewportCameraState{yawDeg, pitchDeg, zoom, pan, size, 1.0F, center,
                             depthExtent}
      .worldToClip();
}

void ViewportRenderer::drawSurfaces(
    GpuMesh& gpu, const QMatrix4x4& matrix, float yawDeg, float pitchDeg,
    const ThemeColors& theme, bool preview, bool cutPreview,
    int highlightMode) {
  const int count = highlightMode == 1 ? gpu.selectedIndexCount
                    : highlightMode == 2 ? gpu.hoveredIndexCount
                                         : gpu.indexCount;
  if (count == 0) return;
  const float yaw = yawDeg * std::numbers::pi_v<float> / 180.0F;
  const float pitch = pitchDeg * std::numbers::pi_v<float> / 180.0F;
  QMatrix3x3 normalMatrix;
  normalMatrix(0, 0) = std::cos(yaw); normalMatrix(0, 1) = -std::sin(yaw); normalMatrix(0, 2) = 0;
  normalMatrix(1, 0) = -std::sin(yaw) * std::cos(pitch); normalMatrix(1, 1) = -std::cos(yaw) * std::cos(pitch); normalMatrix(1, 2) = std::sin(pitch);
  normalMatrix(2, 0) = -std::sin(yaw) * std::sin(pitch); normalMatrix(2, 1) = -std::cos(yaw) * std::sin(pitch); normalMatrix(2, 2) = -std::cos(pitch);
  surfaceProgram_.bind();
  surfaceProgram_.setUniformValue("uMvp", matrix);
  surfaceProgram_.setUniformValue("uNormalMatrix", normalMatrix);
  surfaceProgram_.setUniformValue("uHighlightMode", highlightMode);
  surfaceProgram_.setUniformValue("uPreview", preview);
  surfaceProgram_.setUniformValue("uCutPreview", cutPreview);
  surfaceProgram_.setUniformValue("uSurfaceColor",
                                  colorVector(theme.viewportSurfaceLight));
  surfaceProgram_.setUniformValue("uPreviewColor",
                                  colorVector(theme.previewPositive));
  QColor cutColor = theme.previewNegative;
  cutColor.setAlphaF(0.44F);
  surfaceProgram_.setUniformValue("uCutPreviewColor", colorVector(cutColor));
  surfaceProgram_.setUniformValue("uSelectedColor",
                                  colorVector(theme.viewportSelection));
  surfaceProgram_.setUniformValue("uHoverColor",
                                  colorVector(theme.viewportHover));
  surfaceProgram_.setUniformValue("uSpecularColor",
                                  colorVector(theme.viewportHandle));
  if (!bindVertexArrayChecked(gpu.surfaceVao)) {
    surfaceProgram_.release();
    error_ = QStringLiteral("Renderer surface VAO draw bind failed");
    return;
  }
  VaoReleaseGuard vaoGuard{&gpu.surfaceVao};
  QOpenGLBuffer& indices = highlightMode == 1 ? gpu.selectedIndices
                           : highlightMode == 2 ? gpu.hoveredIndices
                                                : gpu.indices;
  indices.bind();
  QOpenGLContext::currentContext()->functions()->glDrawElements(
      GL_TRIANGLES, count, GL_UNSIGNED_INT, nullptr);
  indices.release();
  surfaceProgram_.release();
}

void ViewportRenderer::drawEdges(
    GpuMesh& gpu, const QMatrix4x4& matrix, const ThemeColors& theme,
    bool ordinaryEdges, float dpr, int highlightMode) {
  const int count = highlightMode == 1 ? gpu.selectedEdgeVertexCount
                    : highlightMode == 2 ? gpu.hoveredEdgeVertexCount
                                         : gpu.edgeVertexCount;
  if (count == 0) return;
  edgeProgram_.bind();
  edgeProgram_.setUniformValue("uMvp", matrix);
  edgeProgram_.setUniformValue("uHighlightMode", highlightMode);
  edgeProgram_.setUniformValue("uOrdinaryEdges", ordinaryEdges);
  edgeProgram_.setUniformValue("uEdgeColor", colorVector(theme.viewportEdge));
  edgeProgram_.setUniformValue("uSelectedColor",
                               colorVector(theme.viewportSelection));
  edgeProgram_.setUniformValue("uHoverColor",
                               colorVector(theme.viewportHover));
  auto* gl = QOpenGLContext::currentContext()->functions();
  gl->glLineWidth((ordinaryEdges ? 1.15F : 2.8F) * dpr);
  QOpenGLVertexArrayObject* vao = highlightMode == 1 ? &gpu.selectedEdgeVao
                                  : highlightMode == 2 ? &gpu.hoveredEdgeVao
                                                       : &gpu.edgeVao;
  if (!bindVertexArrayChecked(*vao)) {
    edgeProgram_.release();
    error_ = QStringLiteral("Renderer edge VAO draw bind failed");
    return;
  }
  VaoReleaseGuard vaoGuard{vao};
  gl->glDrawArrays(GL_LINES, 0, count);
  edgeProgram_.release();
}

void ViewportRenderer::render(
    const BodyRenderMesh& source, const BodyRenderMesh* preview,
    const QSize& logicalSize, float dpr, float yawDeg, float pitchDeg,
    float zoom, QPointF pan, ViewportDisplayMode mode,
    const std::vector<std::size_t>& selectedFaces,
    const std::vector<std::size_t>& hoveredFaces,
    const std::vector<std::size_t>& selectedEdges, std::size_t hoveredEdge,
    const BodyRenderMesh* cutPreview,
    std::uint64_t previewPresentationRevision,
    std::uint64_t cutPreviewPresentationRevision,
    const ThemeColors& theme) {
  GeometryFailure failure;
  const bool rendered = runGeometryOperation(
      [&] {
        const BodyMeshKey identity{0, 0, source.revision(), &source,
                                   source.quality()};
        const std::vector<RenderMeshInstance> instances{
            RenderMeshInstance{&source, identity, 0, 0}};
        renderImpl(instances, preview, logicalSize, dpr, yawDeg, pitchDeg,
                   zoom, pan, mode, selectedFaces, hoveredFaces, selectedEdges,
                   hoveredEdge, cutPreview, previewPresentationRevision,
                   cutPreviewPresentationRevision, theme);
      },
      &failure);
  if (!rendered) {
    try {
      error_ = QStringLiteral("Renderer operation failed safely");
    } catch (...) {
    }
  }
}

void ViewportRenderer::render(
    const std::vector<RenderMeshInstance>& sourceInstances,
    const BodyRenderMesh* preview, const QSize& logicalSize, float dpr,
    float yawDeg, float pitchDeg, float zoom, QPointF pan,
    ViewportDisplayMode mode, const std::vector<std::size_t>& selectedFaces,
    const std::vector<std::size_t>& hoveredFaces,
    const std::vector<std::size_t>& selectedEdges, std::size_t hoveredEdge,
    const BodyRenderMesh* cutPreview,
    std::uint64_t previewPresentationRevision,
    std::uint64_t cutPreviewPresentationRevision,
    const ThemeColors& theme) {
  GeometryFailure failure;
  const bool rendered = runGeometryOperation(
      [&] {
        renderImpl(sourceInstances, preview, logicalSize, dpr, yawDeg,
                   pitchDeg, zoom, pan, mode, selectedFaces, hoveredFaces,
                   selectedEdges, hoveredEdge, cutPreview,
                   previewPresentationRevision,
                   cutPreviewPresentationRevision, theme);
      },
      &failure);
  if (!rendered) {
    try {
      error_ = QStringLiteral("Renderer operation failed safely");
    } catch (...) {
    }
  }
}

void ViewportRenderer::renderImpl(
    const std::vector<RenderMeshInstance>& sourceInstances,
    const BodyRenderMesh* preview, const QSize& logicalSize, float dpr,
    float yawDeg, float pitchDeg, float zoom, QPointF pan,
    ViewportDisplayMode mode, const std::vector<std::size_t>& selectedFaces,
    const std::vector<std::size_t>& hoveredFaces,
    const std::vector<std::size_t>& selectedEdges, std::size_t hoveredEdge,
    const BodyRenderMesh* cutPreview,
    std::uint64_t previewPresentationRevision,
    std::uint64_t cutPreviewPresentationRevision,
    const ThemeColors& theme) {
  if (!initialized_ && !initialize()) return;
  error_.clear();
  lastOrdinarySourceMeshes_ = 0;
  lastHighlightOnlySourceMeshes_ = 0;
  lastHighlightOverlayMeshes_ = 0;
  std::vector<std::pair<const RenderMeshInstance*, GpuMesh*>> sources;
  sources.reserve(sourceInstances.size());
  std::vector<BodyMeshKey> activeIdentities;
  activeIdentities.reserve(sourceInstances.size());
  for (const auto& instance : sourceInstances) {
    if (!instance.mesh) continue;
    BodyMeshKey identity = instance.identity;
    if (!identity.shapeIdentity) {
      identity.shapeRevision = instance.mesh->revision();
      identity.shapeIdentity = instance.mesh;
      identity.quality = instance.mesh->quality();
    }
    activeIdentities.push_back(identity);
  }
  synchronizeResources(activeIdentities, preview != nullptr,
                       cutPreview != nullptr);
  for (const auto& instance : sourceInstances) {
    if (!instance.mesh) continue;
    BodyMeshKey identity = instance.identity;
    if (!identity.shapeIdentity) {
      identity.shapeRevision = instance.mesh->revision();
      identity.shapeIdentity = instance.mesh;
      identity.quality = instance.mesh->quality();
    }
    auto found = sources_.find(identity);
    GpuMesh* slot = found != sources_.end() ? found->second.get() : nullptr;
    const bool current =
        slot && slot->identity && *slot->identity == identity &&
        slot->revision == instance.mesh->revision();
    if (!current) {
      if (failureInjection_ == FailureInjection::SourceCacheAllocation) {
        failureInjection_ = FailureInjection::None;
        throw std::bad_alloc();
      }
      // Prepare a complete GPU slot before changing the map. Allocation or
      // upload failure therefore cannot publish a null/partial cache entry.
      auto pending = std::make_unique<GpuMesh>();
      if (!upload(*pending, *instance.mesh, identity))
        throw std::runtime_error("Renderer source upload failed");
      prepareHighlights(*pending, *instance.mesh, instance.faceOffset,
                        instance.edgeOffset, selectedFaces, hoveredFaces,
                        selectedEdges, hoveredEdge);
      if (found == sources_.end()) {
        const auto [inserted, accepted] =
            sources_.emplace(identity, std::move(pending));
        if (!accepted || !inserted->second)
          throw std::runtime_error("Renderer source cache publication failed");
        slot = inserted->second.get();
      } else {
        auto previous = std::move(found->second);
        found->second = std::move(pending);
        slot = found->second.get();
        if (previous) destroyGpuMesh(*previous, true);
      }
    } else {
      prepareHighlights(*slot, *instance.mesh, instance.faceOffset,
                        instance.edgeOffset, selectedFaces, hoveredFaces,
                        selectedEdges, hoveredEdge);
    }
    sources.push_back({&instance, slot});
    if (instance.drawOrdinary)
      ++lastOrdinarySourceMeshes_;
    else
      ++lastHighlightOnlySourceMeshes_;
    if (slot->selectedIndexCount > 0 || slot->hoveredIndexCount > 0 ||
        slot->selectedEdgeVertexCount > 0 ||
        slot->hoveredEdgeVertexCount > 0)
      ++lastHighlightOverlayMeshes_;
  }
  std::unique_ptr<GpuMesh> pendingPreview;
  std::unique_ptr<GpuMesh> pendingCutPreview;
  const auto preparePreview = [&](const BodyRenderMesh* mesh,
                                  std::uint64_t presentationRevision,
                                  const std::unique_ptr<GpuMesh>& currentSlot,
                                  std::unique_ptr<GpuMesh>& pending,
                                  const char* failureText) {
    if (!mesh) return;
    const BodyMeshKey key{0, 0,
                          presentationRevision != 0
                              ? presentationRevision
                              : mesh->revision(),
                          mesh, mesh->quality()};
    if (currentSlot && currentSlot->identity &&
        *currentSlot->identity == key &&
        currentSlot->revision == mesh->revision())
      return;
    pending = std::make_unique<GpuMesh>();
    if (!upload(*pending, *mesh, key)) throw std::runtime_error(failureText);
  };
  preparePreview(preview, previewPresentationRevision, preview_, pendingPreview,
                 "Renderer preview upload failed");
  preparePreview(cutPreview, cutPreviewPresentationRevision, cutPreview_,
                 pendingCutPreview, "Renderer cut-preview upload failed");

  // Publish preview and cut slots together only after both pending uploads
  // succeeded. A failed cut upload cannot make a new preview look committed.
  if (pendingPreview) {
    auto previous = std::move(preview_);
    preview_ = std::move(pendingPreview);
    if (previous) destroyGpuMesh(*previous, true);
  }
  if (pendingCutPreview) {
    auto previous = std::move(cutPreview_);
    cutPreview_ = std::move(pendingCutPreview);
    if (previous) destroyGpuMesh(*previous, true);
  }

  bool haveBounds = false;
  Point3d minimum{};
  Point3d maximum{};
  const auto includeBounds = [&](const BodyRenderMesh& mesh) {
    const double radius = mesh.diagonal() * 0.5;
    const Point3d low{mesh.center().x - radius, mesh.center().y - radius,
                      mesh.center().z - radius};
    const Point3d high{mesh.center().x + radius, mesh.center().y + radius,
                       mesh.center().z + radius};
    if (!haveBounds) {
      minimum = low;
      maximum = high;
      haveBounds = true;
    } else {
      minimum = {std::min(minimum.x, low.x), std::min(minimum.y, low.y),
                 std::min(minimum.z, low.z)};
      maximum = {std::max(maximum.x, high.x), std::max(maximum.y, high.y),
                 std::max(maximum.z, high.z)};
    }
  };
  for (const auto& [instance, gpu] : sources) {
    static_cast<void>(gpu);
    includeBounds(*instance->mesh);
  }
  if (preview) includeBounds(*preview);
  if (cutPreview) includeBounds(*cutPreview);
  const Point3d center{(minimum.x + maximum.x) * 0.5,
                       (minimum.y + maximum.y) * 0.5,
                       (minimum.z + maximum.z) * 0.5};
  const double diagonal = haveBounds
      ? std::hypot(std::hypot(maximum.x - minimum.x,
                              maximum.y - minimum.y),
                   maximum.z - minimum.z)
      : 1.0;
  const auto matrix = projectionMatrix(logicalSize, yawDeg, pitchDeg, zoom, pan,
                                       std::max(1.0, diagonal * 3.0), center);
  auto* gl = QOpenGLContext::currentContext()->functions();
  const int pixelWidth = std::max(1, static_cast<int>(logicalSize.width() * dpr));
  const int pixelHeight = std::max(1, static_cast<int>(logicalSize.height() * dpr));
  gl->glViewport(0, 0, pixelWidth, pixelHeight);
  gl->glEnable(GL_DEPTH_TEST);
  gl->glDepthFunc(GL_LEQUAL);
  gl->glDepthMask(GL_TRUE);
  gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  gl->glDisable(GL_CULL_FACE);
  gl->glDisable(GL_SCISSOR_TEST);
  gl->glDisable(GL_STENCIL_TEST);
  gl->glDisable(GL_BLEND);
  if (QOpenGLContext::currentContext()->format().samples() > 0)
    gl->glEnable(GL_MULTISAMPLE);
  else
    gl->glDisable(GL_MULTISAMPLE);
  gl->glClearDepthf(1.0F);
  gl->glClear(GL_DEPTH_BUFFER_BIT);
  if (mode != ViewportDisplayMode::Wireframe) {
    // Push filled fragments slightly away so the subsequent B-Rep edge pass is
    // crisp without disabling depth testing for rear geometry.
    gl->glEnable(GL_POLYGON_OFFSET_FILL);
    gl->glPolygonOffset(1.0F, 1.0F);
    for (const auto& [instance, gpu] : sources) {
      if (instance->drawOrdinary)
        drawSurfaces(*gpu, matrix, yawDeg, pitchDeg, theme, false);
    }
    if (preview)
      drawSurfaces(*preview_, matrix, yawDeg, pitchDeg, theme, true);

    // Visual-only subtractive volume. Use the same translucent red palette as
    // the legacy sketch-extrude Cut preview. The boolean-result preview remains
    // authoritative; this second pass only shows the material being removed.
    if (cutPreview) {
      gl->glEnable(GL_BLEND);
      gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      gl->glDepthMask(GL_FALSE);
      // Pull coincident cutter faces slightly toward the camera so the red
      // overlay remains visible on the walls/floor of the resulting cavity.
      gl->glPolygonOffset(-1.0F, -1.0F);
      drawSurfaces(*cutPreview_, matrix, yawDeg, pitchDeg, theme, false, true);
      gl->glDepthMask(GL_TRUE);
      gl->glDisable(GL_BLEND);
      gl->glPolygonOffset(1.0F, 1.0F);
    }
    // A ReplaceSource preview suppresses only the source's ordinary passes.
    // Its canonical topology remains resident so selected/hovered faces can
    // be redrawn after every preview layer without rendering the whole source.
    for (const auto& [instance, gpu] : sources) {
      static_cast<void>(instance);
      drawSurfaces(*gpu, matrix, yawDeg, pitchDeg, theme, false, false, 2);
      drawSurfaces(*gpu, matrix, yawDeg, pitchDeg, theme, false, false, 1);
    }
    gl->glDisable(GL_POLYGON_OFFSET_FILL);
  }
  // Ordinary model/preview edges are drawn without interaction coloring.
  // Selection/hover is then rendered exactly once from the selectable source
  // topology. Keeping this inside the native OpenGL pass avoids mixing a
  // second QPainter overlay into QOpenGLWidget::paintGL().
  if (mode != ViewportDisplayMode::Shaded) {
    for (const auto& [instance, gpu] : sources) {
      if (instance->drawOrdinary)
        drawEdges(*gpu, matrix, theme, true, dpr);
    }
    if (preview) drawEdges(*preview_, matrix, theme, true, dpr);
  }

  if (!selectedEdges.empty() || hoveredEdge != std::size_t(-1)) {
    // Picking already rejects occluded hover candidates. Disable depth only for
    // this tiny highlighted-edge pass to avoid z-fighting with the source face
    // and to keep the candidate visible while a Fillet/Chamfer preview exists.
    gl->glDisable(GL_DEPTH_TEST);
    for (const auto& [instance, gpu] : sources) {
      static_cast<void>(instance);
      drawEdges(*gpu, matrix, theme, false, dpr, 1);
      drawEdges(*gpu, matrix, theme, false, dpr, 2);
    }
    gl->glEnable(GL_DEPTH_TEST);
  }

  gl->glLineWidth(1.0F);
  gl->glDisable(GL_POLYGON_OFFSET_FILL);
  gl->glDisable(GL_DEPTH_TEST);
  gl->glDisable(GL_MULTISAMPLE);
  gl->glDepthMask(GL_TRUE);
  gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

}

}  // namespace solidar
