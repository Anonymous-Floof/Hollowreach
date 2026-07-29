#include "core/shader.h"

#include <memory>
#include <sstream>
#include <unordered_set>

#include "core/assets.h"
#include "core/log.h"

namespace hr {
namespace {

// Trims ASCII whitespace from both ends.
std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// Pulls `name` out of `#include "name"`, or returns empty if the line is not an
// include directive.
std::string includeTarget(std::string_view line) {
  std::string_view t = trim(line);
  if (t.rfind("#include", 0) != 0) return {};
  std::size_t open = t.find('"');
  if (open == std::string_view::npos) return {};
  std::size_t close = t.find('"', open + 1);
  if (close == std::string_view::npos) return {};
  return std::string(t.substr(open + 1, close - open - 1));
}

// Shared chunks live alongside the shader that includes them.
std::string resolveInclude(const std::string& from, const std::string& target) {
  std::size_t slash = from.find_last_of('/');
  if (slash == std::string::npos) return target;
  return from.substr(0, slash + 1) + target;
}

bool expand(const std::string& assetName, std::ostringstream& out,
            std::unordered_set<std::string>& active, std::vector<std::string>* depsOut,
            std::string* errorOut, int depth) {
  if (depth > 16) {
    if (errorOut) *errorOut = "#include nested too deeply at " + assetName;
    return false;
  }
  if (active.count(assetName)) {
    if (errorOut) *errorOut = "circular #include of " + assetName;
    return false;
  }

  auto text = assets::readText(assetName);
  if (!text) {
    if (errorOut) *errorOut = "shader asset not found: " + assetName;
    return false;
  }

  active.insert(assetName);
  if (depsOut) depsOut->push_back(assetName);

  std::istringstream in(*text);
  std::string line;
  int lineNo = 0;
  while (std::getline(in, line)) {
    ++lineNo;
    std::string target = includeTarget(line);
    if (target.empty()) {
      out << line << '\n';
      continue;
    }
    if (!expand(resolveInclude(assetName, target), out, active, depsOut, errorOut, depth + 1)) {
      return false;
    }
    // #line so a compile error after an include still points at the right place
    // in the including file. GLSL numbers the *next* line, hence lineNo + 1.
    out << "#line " << (lineNo + 1) << '\n';
  }

  active.erase(assetName);
  return true;
}

GLuint compile(GLenum type, const std::string& source, const char* label) {
  GLuint sh = glCreateShader(type);
  const char* ptr = source.c_str();
  glShaderSource(sh, 1, &ptr, nullptr);
  glCompileShader(sh);

  GLint ok = GL_FALSE;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (ok == GL_TRUE) return sh;

  GLint len = 0;
  glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
  std::string info(static_cast<std::size_t>(len > 1 ? len : 1), '\0');
  glGetShaderInfoLog(sh, len, nullptr, info.data());
  log::error("shader compile failed (%s):\n%s", label, info.c_str());

  // The numbered dump is what makes a #include-mangled error tractable.
  std::istringstream src(source);
  std::string line;
  int n = 0;
  std::string listing;
  while (std::getline(src, line)) {
    listing += std::to_string(++n) + ": " + line + "\n";
  }
  log::debug("--- preprocessed %s ---\n%s", label, listing.c_str());

  glDeleteShader(sh);
  return 0;
}

}  // namespace

std::string preprocess(const std::string& assetName, const ShaderDefines& defines,
                       std::vector<std::string>* depsOut, std::string* errorOut) {
  std::ostringstream body;
  std::unordered_set<std::string> active;
  if (!expand(assetName, body, active, depsOut, errorOut, 0)) return {};

  std::string text = body.str();
  if (defines.empty()) return text;

  // #version must stay the first non-comment line, so defines go straight after
  // it rather than at the top.
  std::size_t insertAt = 0;
  std::size_t firstNewline = text.find('\n');
  if (text.rfind("#version", 0) == 0 && firstNewline != std::string::npos) {
    insertAt = firstNewline + 1;
  }

  std::ostringstream block;
  for (const auto& [name, value] : defines) {
    block << "#define " << name << ' ' << value << '\n';
  }
  // Reset the line counter so reported error lines match the source file.
  block << "#line " << (insertAt ? 2 : 1) << '\n';

  return text.substr(0, insertAt) + block.str() + text.substr(insertAt);
}

// --- Program ----------------------------------------------------------------

Program::~Program() { destroy(); }

Program::Program(Program&& other) noexcept
    : id_(other.id_), uniforms_(std::move(other.uniforms_)) {
  other.id_ = 0;
}

Program& Program::operator=(Program&& other) noexcept {
  if (this != &other) {
    destroy();
    id_ = other.id_;
    uniforms_ = std::move(other.uniforms_);
    other.id_ = 0;
  }
  return *this;
}

void Program::destroy() {
  if (id_) {
    glDeleteProgram(id_);
    id_ = 0;
  }
  uniforms_.clear();
}

void Program::adopt(GLuint newId) {
  if (id_) glDeleteProgram(id_);
  id_ = newId;
  uniforms_.clear();
}

GLint Program::uniform(const std::string& name) const {
  auto it = uniforms_.find(name);
  if (it != uniforms_.end()) return it->second;
  GLint loc = id_ ? glGetUniformLocation(id_, name.c_str()) : -1;
  uniforms_.emplace(name, loc);
  return loc;
}

// --- ShaderCache ------------------------------------------------------------

bool ShaderCache::build(Item& item) {
  std::vector<std::string> deps;
  std::string error;

  std::string vs = preprocess(item.spec.vertAsset, item.spec.defines, &deps, &error);
  if (vs.empty()) {
    log::error("shader %s: %s", item.spec.name.c_str(), error.c_str());
    return false;
  }
  std::string fs = preprocess(item.spec.fragAsset, item.spec.defines, &deps, &error);
  if (fs.empty()) {
    log::error("shader %s: %s", item.spec.name.c_str(), error.c_str());
    return false;
  }

  GLuint vsh = compile(GL_VERTEX_SHADER, vs, (item.spec.name + ".vert").c_str());
  if (!vsh) return false;
  GLuint fsh = compile(GL_FRAGMENT_SHADER, fs, (item.spec.name + ".frag").c_str());
  if (!fsh) {
    glDeleteShader(vsh);
    return false;
  }

  GLuint prog = glCreateProgram();
  glAttachShader(prog, vsh);
  glAttachShader(prog, fsh);
  for (std::size_t i = 0; i < item.spec.attribs.size(); ++i) {
    glBindAttribLocation(prog, static_cast<GLuint>(i), item.spec.attribs[i].c_str());
  }
  glLinkProgram(prog);
  glDeleteShader(vsh);
  glDeleteShader(fsh);

  GLint ok = GL_FALSE;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (ok != GL_TRUE) {
    GLint len = 0;
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
    std::string info(static_cast<std::size_t>(len > 1 ? len : 1), '\0');
    glGetProgramInfoLog(prog, len, nullptr, info.data());
    log::error("shader %s link failed:\n%s", item.spec.name.c_str(), info.c_str());
    glDeleteProgram(prog);
    return false;
  }

  item.program.adopt(prog);

  item.deps.clear();
  for (const std::string& dep : deps) {
    item.deps.emplace_back(dep, assets::overrideMTime(dep));
  }
  return true;
}

Program* ShaderCache::load(ProgramSpec spec) {
  auto item = std::make_unique<Item>();
  item->spec = std::move(spec);
  if (!build(*item)) return nullptr;
  Program* out = &item->program;
  items_.push_back(std::move(item));
  return out;
}

int ShaderCache::reloadChanged() {
  if (!assets::hasOverrideDir()) return 0;

  int reloaded = 0;
  for (auto& item : items_) {
    bool dirty = false;
    for (auto& [name, stamp] : item->deps) {
      long long now = assets::overrideMTime(name);
      if (now != 0 && now != stamp) {
        dirty = true;
        break;
      }
    }
    if (!dirty) continue;

    // Stamp the dependencies before rebuilding so a shader with a syntax error
    // is retried only after it changes again, instead of every frame.
    for (auto& [name, stamp] : item->deps) stamp = assets::overrideMTime(name);

    if (build(*item)) {
      log::info("shader %s reloaded", item->spec.name.c_str());
      ++reloaded;
    } else {
      log::warn("shader %s reload failed; keeping the previous program",
                item->spec.name.c_str());
    }
  }
  return reloaded;
}

void ShaderCache::clear() { items_.clear(); }

}  // namespace hr
