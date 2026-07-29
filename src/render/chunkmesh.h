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
  void upload(const std::vector<world::TerrainVertex>& verts);
  void destroy();

  bool empty() const { return count_ == 0; }
  GLsizei count() const { return count_; }

  void draw() const;

 private:
  GLuint vao_ = 0;
  GLuint vbo_ = 0;
  GLsizei count_ = 0;
  std::size_t capacityBytes_ = 0;
};

// Binds the terrain vertex layout on the currently bound VAO and VBO. Shared so
// the layout is declared once and cannot drift from assets/shaders/terrain.vert.
void bindTerrainAttributes();

}  // namespace hr::render
