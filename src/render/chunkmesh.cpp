#include "render/chunkmesh.h"

#include <utility>

namespace hr::render {

void bindTerrainAttributes() {
  using V = world::TerrainVertex;
  const GLsizei stride = sizeof(V);

  // 0: position, world space.
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                        reinterpret_cast<const void*>(offsetof(V, x)));
  // 1: atlas UV, normalised unsigned short.
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_UNSIGNED_SHORT, GL_TRUE, stride,
                        reinterpret_cast<const void*>(offsetof(V, u)));
  // 2: shade, skylight, blocklight, flags. NOT normalised — the shader divides the
  // first three by 255 itself and needs `flags` as a whole number, and a single
  // attribute cannot be half normalised.
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride,
                        reinterpret_cast<const void*>(offsetof(V, shade)));
  // 3: biome tint, normalised. White until a resource pack needs otherwise.
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                        reinterpret_cast<const void*>(offsetof(V, tintR)));
}

ChunkMeshBuffer& ChunkMeshBuffer::operator=(ChunkMeshBuffer&& other) noexcept {
  if (this != &other) {
    destroy();
    vao_ = other.vao_;
    vbo_ = other.vbo_;
    count_ = other.count_;
    capacityBytes_ = other.capacityBytes_;
    sections_ = other.sections_;
    other.vao_ = other.vbo_ = 0;
    other.count_ = 0;
    other.capacityBytes_ = 0;
    other.sections_ = world::MeshSections{};
  }
  return *this;
}

void ChunkMeshBuffer::upload(const std::vector<world::TerrainVertex>& verts,
                             const world::MeshSections& sections) {
  sections_ = sections;
  if (verts.empty()) {
    destroy();
    return;
  }

  const std::size_t bytes = verts.size() * sizeof(world::TerrainVertex);

  if (vao_ == 0) {
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    bindTerrainAttributes();
  } else {
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  }

  // Orphan and refill when it grows, subdata when it fits. Remeshing is frequent —
  // every edit dirties a 3x3 neighbourhood — so avoiding a reallocation per remesh
  // is worth the branch.
  if (bytes > capacityBytes_) {
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), verts.data(),
                 GL_DYNAMIC_DRAW);
    capacityBytes_ = bytes;
  } else {
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), verts.data());
  }

  count_ = static_cast<GLsizei>(verts.size());
  glBindVertexArray(0);
}

void ChunkMeshBuffer::destroy() {
  if (vbo_) {
    glDeleteBuffers(1, &vbo_);
    vbo_ = 0;
  }
  if (vao_) {
    glDeleteVertexArrays(1, &vao_);
    vao_ = 0;
  }
  count_ = 0;
  capacityBytes_ = 0;
  sections_ = world::MeshSections{};
}

void ChunkMeshBuffer::draw() const {
  if (count_ == 0) return;
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, count_);
}

void ChunkMeshBuffer::drawSectionMask(unsigned mask) const {
  if (mask == 0 || count_ == 0) return;
  GLint first[world::kSections];
  GLsizei counts[world::kSections];
  GLsizei n = 0;
  for (int i = 0; i < world::kSections; ++i) {
    if (!(mask & (1u << i)) || sections_.count[i] == 0) continue;
    // Merge with the previous range when they abut, which they usually do: the
    // visible sections of a chunk are contiguous far more often than not.
    if (n > 0 && first[n - 1] + counts[n - 1] == static_cast<GLint>(sections_.first[i])) {
      counts[n - 1] += static_cast<GLsizei>(sections_.count[i]);
      continue;
    }
    first[n] = static_cast<GLint>(sections_.first[i]);
    counts[n] = static_cast<GLsizei>(sections_.count[i]);
    ++n;
  }
  if (n == 0) return;
  // One bind, then the merged ranges. glMultiDrawArrays is not in the loader's
  // set, and it would save little here: the merge above usually leaves one or
  // two ranges, and the bind was the part being repeated six times per chunk.
  glBindVertexArray(vao_);
  for (GLsizei i = 0; i < n; ++i) glDrawArrays(GL_TRIANGLES, first[i], counts[i]);
}

void ChunkMeshBuffer::drawSection(int index) const {
  const GLsizei n = static_cast<GLsizei>(sections_.count[index]);
  if (n == 0) return;
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, static_cast<GLint>(sections_.first[index]), n);
}

}  // namespace hr::render
