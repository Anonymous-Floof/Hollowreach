// Shader compilation, the #include resolver, and the program cache.
//
// The web build kept all GLSL in JS template strings and stitched shared chunks
// in with ${SKY_GLSL} / ${CLOUD_GLSL} / ${BONE_GLSL} / ${MAX_LIGHTS}
// interpolation (js/core/shaders_deferred.js). Here the GLSL lives in real files
// under assets/shaders/, shared chunks are `#include "sky.glsl"`, and the
// interpolated constants become injected `#define`s. Same result, but the
// shaders are editable on their own and hot-reloadable.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/gl.h"

namespace hr {

// Name/value pairs injected as #define immediately after the #version line.
using ShaderDefines = std::vector<std::pair<std::string, std::string>>;

class Program {
 public:
  Program() = default;
  ~Program();
  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;
  Program(Program&& other) noexcept;
  Program& operator=(Program&& other) noexcept;

  bool valid() const { return id_ != 0; }
  GLuint id() const { return id_; }
  void use() const { glUseProgram(id_); }

  // Cached lookup, like the JS prog.uniform(name). Returns -1 for a name the
  // linker dropped, and every setter below tolerates -1, so a shader that stops
  // using a uniform does not need its call sites edited.
  GLint uniform(const std::string& name) const;

  void set(const std::string& n, int v) const { glUniform1i(uniform(n), v); }
  void set(const std::string& n, float v) const { glUniform1f(uniform(n), v); }
  void set(const std::string& n, float a, float b) const { glUniform2f(uniform(n), a, b); }
  void set(const std::string& n, float a, float b, float c) const {
    glUniform3f(uniform(n), a, b, c);
  }
  void set(const std::string& n, float a, float b, float c, float d) const {
    glUniform4f(uniform(n), a, b, c, d);
  }
  void setVec3(const std::string& n, const float* v) const { glUniform3fv(uniform(n), 1, v); }
  void setVec4(const std::string& n, const float* v) const { glUniform4fv(uniform(n), 1, v); }
  void setMat4(const std::string& n, const float* m) const {
    glUniformMatrix4fv(uniform(n), 1, GL_FALSE, m);
  }
  // Array uniforms (the 16 point lights, the 6 entity bones).
  void setVec3Array(const std::string& n, const float* v, int count) const {
    glUniform3fv(uniform(n), count, v);
  }
  void setVec4Array(const std::string& n, const float* v, int count) const {
    glUniform4fv(uniform(n), count, v);
  }
  void setFloatArray(const std::string& n, const float* v, int count) const {
    glUniform1fv(uniform(n), count, v);
  }

  // Used by ShaderCache when a hot reload succeeds.
  void adopt(GLuint newId);
  void destroy();

 private:
  GLuint id_ = 0;
  mutable std::unordered_map<std::string, GLint> uniforms_;
};

// One entry per program: what it was built from, so a reload can rebuild it.
struct ProgramSpec {
  std::string name;       // for logs
  std::string vertAsset;  // e.g. "shaders/terrain.vert"
  std::string fragAsset;
  ShaderDefines defines;
  // Bound to locations 0..n-1 before linking, matching the JS createProgram's
  // attribs array so the ported vertex layouts keep their slot numbers.
  std::vector<std::string> attribs;
};

class ShaderCache {
 public:
  // Compiles and links, or returns nullptr after logging the error with the
  // preprocessed source. The returned pointer stays valid for the cache's life,
  // and a hot reload swaps the program behind it.
  Program* load(ProgramSpec spec);

  // Recompiles any program whose source files changed on disk. Returns how many
  // were reloaded. A failed reload keeps the previous working program, so a
  // typo in a shader does not black-screen the game.
  int reloadChanged();

  void clear();

 private:
  struct Item {
    ProgramSpec spec;
    Program program;
    // Last-modified stamp per source file involved, includes as well.
    std::vector<std::pair<std::string, long long>> deps;
  };
  std::vector<std::unique_ptr<Item>> items_;

  bool build(Item& item);
};

// Resolves #include directives and injects defines. Exposed for tests and for
// dumping the exact source a driver rejected.
// `depsOut` collects every asset name pulled in, including the entry point.
std::string preprocess(const std::string& assetName, const ShaderDefines& defines,
                       std::vector<std::string>* depsOut, std::string* errorOut);

}  // namespace hr
