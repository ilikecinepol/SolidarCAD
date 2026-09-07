#include "ui/ViewportRenderer.h"
#include "ui/ViewportCamera.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <string>

namespace solidar {
namespace {

constexpr int kMaxHighlights = 32;

struct GpuVertex {
  float position[3];
  float normal[3];
  float faceIndex;
};

struct GpuEdgeVertex {
  float position[3];
  float edgeIndex;
};

const char* surfaceVertexShader = R"(
  #version 330 core
  layout(location=0) in vec3 aPosition;
  layout(location=1) in vec3 aNormal;
  layout(location=2) in float aFaceIndex;
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
  #version 330 core
  in vec3 vNormal;
  flat in int vFaceIndex;
  uniform int uSelectedFaces[32];
  uniform int uSelectedCount;
  uniform int uHoveredFace;
  uniform bool uPreview;
  out vec4 color;
  void main() {
    vec3 n = normalize(vNormal);
    vec3 key = normalize(vec3(0.28, -0.42, 0.86));
    vec3 fill = normalize(vec3(-0.55, 0.30, 0.48));
    float diffuse = max(dot(n, key), 0.0);
    float fillDiffuse = max(dot(n, fill), 0.0);
    float specular = pow(max(dot(reflect(-key, n), vec3(0, 0, 1)), 0.0), 32.0);
    vec3 base = uPreview ? vec3(0.73, 0.82, 0.92) : vec3(0.72, 0.77, 0.83);
    bool selected = false;
    for (int i = 0; i < uSelectedCount; ++i)
      selected = selected || vFaceIndex == uSelectedFaces[i];
    if (selected) base = mix(base, vec3(0.10, 0.43, 0.94), 0.58);
    else if (vFaceIndex == uHoveredFace)
      base = mix(base, vec3(0.25, 0.65, 1.0), 0.38);
    color = vec4(base * (0.46 + 0.38 * diffuse + 0.16 * fillDiffuse) +
                 vec3(0.10 * specular), 1.0);
  })";

const char* edgeVertexShader = R"(
  #version 330 core
  layout(location=0) in vec3 aPosition;
  layout(location=1) in float aEdgeIndex;
  uniform mat4 uMvp;
  flat out int vEdgeIndex;
  void main() {
    gl_Position = uMvp * vec4(aPosition, 1.0);
    vEdgeIndex = int(aEdgeIndex + 0.5);
  })";

const char* edgeFragmentShader = R"(
  #version 330 core
  flat in int vEdgeIndex;
  uniform int uSelectedEdges[32];
  uniform int uSelectedCount;
  uniform int uHoveredEdge;
  uniform bool uOrdinaryEdges;
  out vec4 color;
  void main() {
    bool selected = false;
    for (int i = 0; i < uSelectedCount; ++i)
      selected = selected || vEdgeIndex == uSelectedEdges[i];
    bool hovered = vEdgeIndex == uHoveredEdge;
    if (!uOrdinaryEdges && !selected && !hovered) discard;
    color = selected ? vec4(1.0, 0.43, 0.0, 1.0)
                     : hovered ? vec4(0.0, 0.65, 1.0, 1.0)
                               : vec4(0.16, 0.22, 0.29, 1.0);
  })";

std::array<int, kMaxHighlights> highlightArray(
    const std::vector<std::size_t>& values, int& count) {
  std::array<int, kMaxHighlights> result{};
  count = std::min<int>(static_cast<int>(values.size()), kMaxHighlights);
  for (int i = 0; i < count; ++i)
    result[i] = static_cast<int>(values[static_cast<std::size_t>(i)]);
  return result;
}

}  // namespace

struct ViewportRenderer::GpuMesh {
  QOpenGLBuffer vertices{QOpenGLBuffer::VertexBuffer};
  QOpenGLBuffer indices{QOpenGLBuffer::IndexBuffer};
  QOpenGLBuffer edges{QOpenGLBuffer::VertexBuffer};
  QOpenGLVertexArrayObject surfaceVao;
  QOpenGLVertexArrayObject edgeVao;
  std::uint64_t revision{std::numeric_limits<std::uint64_t>::max()};
  int indexCount{};
  int edgeVertexCount{};
};

ViewportRenderer::ViewportRenderer()
    : source_(std::make_unique<GpuMesh>()),
      preview_(std::make_unique<GpuMesh>()) {}

ViewportRenderer::~ViewportRenderer() { release(); }

bool ViewportRenderer::initialize() {
  if (initialized_) return true;
  if (!QOpenGLContext::currentContext()) {
    error_ = QStringLiteral("OpenGL context is unavailable");
    return false;
  }
  auto compile = [this](QOpenGLShaderProgram& program, const char* vertex,
                        const char* fragment) {
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) ||
        !program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) ||
        !program.link()) {
      error_ = program.log();
      return false;
    }
    return true;
  };
  initialized_ = compile(surfaceProgram_, surfaceVertexShader,
                         surfaceFragmentShader) &&
                 compile(edgeProgram_, edgeVertexShader, edgeFragmentShader);
  return initialized_;
}

void ViewportRenderer::release() {
  if (!initialized_ || !QOpenGLContext::currentContext()) return;
  for (GpuMesh* gpu : {source_.get(), preview_.get()}) {
    gpu->surfaceVao.destroy();
    gpu->edgeVao.destroy();
    gpu->vertices.destroy();
    gpu->indices.destroy();
    gpu->edges.destroy();
  }
  surfaceProgram_.removeAllShaders();
  edgeProgram_.removeAllShaders();
  initialized_ = false;
}

bool ViewportRenderer::upload(GpuMesh& gpu, const BodyRenderMesh& mesh) {
  if (gpu.revision == mesh.revision()) return true;
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

  if (!gpu.vertices.isCreated()) gpu.vertices.create();
  if (!gpu.indices.isCreated()) gpu.indices.create();
  if (!gpu.edges.isCreated()) gpu.edges.create();
  if (!gpu.surfaceVao.isCreated()) gpu.surfaceVao.create();
  if (!gpu.edgeVao.isCreated()) gpu.edgeVao.create();

  QOpenGLVertexArrayObject::Binder surfaceBinder(&gpu.surfaceVao);
  gpu.vertices.bind();
  gpu.vertices.allocate(vertices.data(), static_cast<int>(vertices.size() * sizeof(GpuVertex)));
  gpu.indices.bind();
  gpu.indices.allocate(mesh.triangleIndices().data(),
                       static_cast<int>(mesh.triangleIndices().size() * sizeof(std::uint32_t)));
  surfaceProgram_.bind();
  surfaceProgram_.enableAttributeArray(0);
  surfaceProgram_.setAttributeBuffer(0, GL_FLOAT, offsetof(GpuVertex, position), 3,
                                     sizeof(GpuVertex));
  surfaceProgram_.enableAttributeArray(1);
  surfaceProgram_.setAttributeBuffer(1, GL_FLOAT, offsetof(GpuVertex, normal), 3,
                                     sizeof(GpuVertex));
  surfaceProgram_.enableAttributeArray(2);
  surfaceProgram_.setAttributeBuffer(2, GL_FLOAT, offsetof(GpuVertex, faceIndex), 1,
                                     sizeof(GpuVertex));
  surfaceProgram_.release();
  gpu.indices.release();
  gpu.vertices.release();

  QOpenGLVertexArrayObject::Binder edgeBinder(&gpu.edgeVao);
  gpu.edges.bind();
  gpu.edges.allocate(edges.data(), static_cast<int>(edges.size() * sizeof(GpuEdgeVertex)));
  edgeProgram_.bind();
  edgeProgram_.enableAttributeArray(0);
  edgeProgram_.setAttributeBuffer(0, GL_FLOAT, offsetof(GpuEdgeVertex, position), 3,
                                  sizeof(GpuEdgeVertex));
  edgeProgram_.enableAttributeArray(1);
  edgeProgram_.setAttributeBuffer(1, GL_FLOAT, offsetof(GpuEdgeVertex, edgeIndex), 1,
                                  sizeof(GpuEdgeVertex));
  edgeProgram_.release();
  gpu.edges.release();

  gpu.indexCount = static_cast<int>(mesh.triangleIndices().size());
  gpu.edgeVertexCount = static_cast<int>(edges.size());
  gpu.revision = mesh.revision();
  lastUploadMilliseconds_ = timer.nsecsElapsed() / 1'000'000.0;
  return true;
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
    const std::vector<std::size_t>& selectedFaces, std::size_t hoveredFace,
    bool preview) {
  if (gpu.indexCount == 0) return;
  const float yaw = yawDeg * std::numbers::pi_v<float> / 180.0F;
  const float pitch = pitchDeg * std::numbers::pi_v<float> / 180.0F;
  QMatrix3x3 normalMatrix;
  normalMatrix(0, 0) = std::cos(yaw); normalMatrix(0, 1) = -std::sin(yaw); normalMatrix(0, 2) = 0;
  normalMatrix(1, 0) = -std::sin(yaw) * std::cos(pitch); normalMatrix(1, 1) = -std::cos(yaw) * std::cos(pitch); normalMatrix(1, 2) = std::sin(pitch);
  normalMatrix(2, 0) = -std::sin(yaw) * std::sin(pitch); normalMatrix(2, 1) = -std::cos(yaw) * std::sin(pitch); normalMatrix(2, 2) = -std::cos(pitch);
  int count = 0;
  const auto selected = highlightArray(selectedFaces, count);
  surfaceProgram_.bind();
  surfaceProgram_.setUniformValue("uMvp", matrix);
  surfaceProgram_.setUniformValue("uNormalMatrix", normalMatrix);
  for (int i = 0; i < count; ++i) {
    surfaceProgram_.setUniformValue(
        ("uSelectedFaces[" + std::to_string(i) + "]").c_str(), selected[i]);
  }
  surfaceProgram_.setUniformValue("uSelectedCount", count);
  surfaceProgram_.setUniformValue("uHoveredFace", hoveredFace == std::size_t(-1) ? -1 : static_cast<int>(hoveredFace));
  surfaceProgram_.setUniformValue("uPreview", preview);
  QOpenGLVertexArrayObject::Binder binder(&gpu.surfaceVao);
  gpu.indices.bind();
  QOpenGLContext::currentContext()->functions()->glDrawElements(
      GL_TRIANGLES, gpu.indexCount, GL_UNSIGNED_INT, nullptr);
  gpu.indices.release();
  surfaceProgram_.release();
}

void ViewportRenderer::drawEdges(
    GpuMesh& gpu, const QMatrix4x4& matrix,
    const std::vector<std::size_t>& selectedEdges, std::size_t hoveredEdge,
    bool ordinaryEdges, float dpr) {
  if (gpu.edgeVertexCount == 0) return;
  int count = 0;
  const auto selected = highlightArray(selectedEdges, count);
  edgeProgram_.bind();
  edgeProgram_.setUniformValue("uMvp", matrix);
  for (int i = 0; i < count; ++i) {
    edgeProgram_.setUniformValue(
        ("uSelectedEdges[" + std::to_string(i) + "]").c_str(), selected[i]);
  }
  edgeProgram_.setUniformValue("uSelectedCount", count);
  edgeProgram_.setUniformValue("uHoveredEdge", hoveredEdge == std::size_t(-1) ? -1 : static_cast<int>(hoveredEdge));
  edgeProgram_.setUniformValue("uOrdinaryEdges", ordinaryEdges);
  auto* gl = QOpenGLContext::currentContext()->functions();
  gl->glLineWidth((ordinaryEdges ? 1.15F : 2.8F) * dpr);
  QOpenGLVertexArrayObject::Binder binder(&gpu.edgeVao);
  gl->glDrawArrays(GL_LINES, 0, gpu.edgeVertexCount);
  edgeProgram_.release();
}

void ViewportRenderer::render(
    const BodyRenderMesh& source, const BodyRenderMesh* preview,
    const QSize& logicalSize, float dpr, float yawDeg, float pitchDeg,
    float zoom, QPointF pan, ViewportDisplayMode mode,
    const std::vector<std::size_t>& selectedFaces, std::size_t hoveredFace,
    const std::vector<std::size_t>& selectedEdges, std::size_t hoveredEdge) {
  if (!initialized_ && !initialize()) return;
  upload(*source_, source);
  if (preview) upload(*preview_, *preview);
  GpuMesh& gpu = preview ? *preview_ : *source_;
  const auto depthRange = ViewportDepthRange::combined(
      source.center(), source.diagonal(),
      preview ? preview->center() : source.center(),
      preview ? preview->diagonal() : source.diagonal(), preview != nullptr);
  const auto matrix = projectionMatrix(logicalSize, yawDeg, pitchDeg, zoom, pan,
                                       depthRange.extent, depthRange.center);
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
    drawSurfaces(gpu, matrix, yawDeg, pitchDeg,
                 preview ? std::vector<std::size_t>{} : selectedFaces,
                 preview ? std::size_t(-1) : hoveredFace, preview != nullptr);
    gl->glDisable(GL_POLYGON_OFFSET_FILL);
  }
  if (mode != ViewportDisplayMode::Shaded)
    drawEdges(gpu, matrix, preview ? std::vector<std::size_t>{} : selectedEdges,
              preview ? std::size_t(-1) : hoveredEdge, true, dpr);
  if (preview && (!selectedEdges.empty() || hoveredEdge != std::size_t(-1)))
    drawEdges(*source_, matrix, selectedEdges, hoveredEdge, false, dpr);
  else if (mode == ViewportDisplayMode::Shaded &&
           (!selectedEdges.empty() || hoveredEdge != std::size_t(-1)))
    drawEdges(*source_, matrix, selectedEdges, hoveredEdge, false, dpr);
  gl->glLineWidth(1.0F);
  gl->glDisable(GL_POLYGON_OFFSET_FILL);
  gl->glDisable(GL_DEPTH_TEST);
  gl->glDisable(GL_MULTISAMPLE);
  gl->glDepthMask(GL_TRUE);
  gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

}  // namespace solidar
