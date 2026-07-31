// GL buffers for one chunk's geometry.
//
// One VAO and VBO per chunk per pass, non-indexed triangles, exactly as the web
// build had it (js/world/world.js:696-712). Positions are world-space so there is
// no per-chunk model matrix; the draw call is a bare glDrawArrays.

#pragma once

#include "core/gl.h"
#include "world/mesher.h"

namespace hr::render {

class ChunkMeshBuffer {
 public:
  ChunkMeshBuffer() = default;
  ~ChunkMeshBuffer() { destroy(); }
  ChunkMeshBuffer(const ChunkMeshBuffer&) = delete;
  ChunkMeshBuffer& operator=(const ChunkMeshBuffer&) = delete;
  ChunkMeshBuffer(ChunkMeshBuffer&& other) noexcept { *this = std::move(other); }
  ChunkMeshBuffer& operator=(ChunkMeshBuffer&& other) noexcept;

  // Uploads, reusing the existing buffer when it is large enough. An empty vertex
  // list frees the GL objects: an all-air or fully-enclosed chunk should not hold
  // a buffer.
  void upload(const std::vector<world::TerrainVertex>& verts,
              const world::MeshSections& sections);
  void destroy();

  bool empty() const { return count_ == 0; }
  GLsizei count() const { return count_; }

  // The whole column, for anything that genuinely wants all of it.
  void draw() const;
  // One vertical slice. The buffer is not split — this is a range into the same
  // VBO — so drawing sections costs an extra glDrawArrays each and no extra
  // memory, state change or bind.
  void drawSection(int index) const;
  // Every section whose bit is set, after a SINGLE bind. All sections share the
  // chunk's VAO, so binding per section was six redundant state changes per chunk.
  // Abutting ranges are merged, so a chunk is usually one draw call.
  void drawSectionMask(unsigned mask) const;
  GLsizei sectionCount(int index) const {
    return static_cast<GLsizei>(sections_.count[index]);
  }
  // Whether the sun reaches any face in this section. False for pure cave wall,
  // which the shadow pass then skips entirely.
  bool sectionSunlit(int index) const { return sections_.sunlit[index]; }

 private:
  GLuint vao_ = 0;
  GLuint vbo_ = 0;
  GLsizei count_ = 0;
  std::size_t capacityBytes_ = 0;
  world::MeshSections sections_;
};

// Binds the terrain vertex layout on the currently bound VAO and VBO. Shared so
// the layout is declared once and cannot drift from assets/shaders/terrain.vert.
void bindTerrainAttributes();

}  // namespace hr::render
