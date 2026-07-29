// The shared fullscreen triangle.
//
// Every post and composite pass in the web build drew three vertices covering
// clip space with overhang the rasteriser clips away — cheaper than a quad and
// with no diagonal seam. One VAO serves all of them.

#pragma once

#include "core/gl.h"

namespace hr::render {

class ScreenQuad {
 public:
  ~ScreenQuad() { destroy(); }

  void create();
  void destroy();

  // Assumes the caller has already bound its program and uniforms.
  void draw() const;

 private:
  GLuint vao_ = 0;
  GLuint vbo_ = 0;
};

}  // namespace hr::render
