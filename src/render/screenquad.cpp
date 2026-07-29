#include "render/screenquad.h"

namespace hr::render {

void ScreenQuad::create() {
  if (vao_) return;

  // Covers [-1,1]^2 with a single triangle: the two extra units of overhang on
  // each axis are clipped, and uv = pos*0.5+0.5 still maps the visible area to
  // [0,1]^2 exactly.
  static const float kVerts[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};

  glGenVertexArrays(1, &vao_);
  glGenBuffers(1, &vbo_);
  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kVerts), kVerts, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
  glBindVertexArray(0);
}

void ScreenQuad::destroy() {
  if (vbo_) {
    glDeleteBuffers(1, &vbo_);
    vbo_ = 0;
  }
  if (vao_) {
    glDeleteVertexArrays(1, &vao_);
    vao_ = 0;
  }
}

void ScreenQuad::draw() const {
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

}  // namespace hr::render
