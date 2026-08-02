#include "render/paintings.h"

#include "world/blocks.h"
#include "world/world.h"

namespace hr::render {
namespace {

// The frame is one texel of a 16-unit block wide, so the picture is inset by the
// same amount and sits inside it rather than over it.
constexpr float kInset = 1.0f / 16.0f;
constexpr float kThick = 1.0f / 16.0f;

// The picture is nudged this far out of the frame's front face, toward whoever is
// looking at it. Without it the quad is coplanar with the face the mesher already
// emitted there and the two z-fight: which one wins is down to per-pixel depth
// rounding, so a painting shimmers between its picture and blank canvas as you
// move. Two millimetres of a one-metre block is far below anything visible and
// comfortably above the depth buffer's resolution at any range you can see a
// painting from.
constexpr float kLift = 0.002f;

// Vertex: pos(3) uv(2) light(2).
constexpr int kFloatsPerVert = 7;

}  // namespace

PaintingRenderer::~PaintingRenderer() { shutdown(); }

bool PaintingRenderer::init(ShaderCache& shaders) {
  prog_ = shaders.load({.name = "painting",
                        .vertAsset = "shaders/painting.vert",
                        .fragAsset = "shaders/painting.frag",
                        .defines = {},
                        .attribs = {"aPos", "aUV", "aLight"}});
  if (!prog_) return false;

  glGenVertexArrays(1, &vao_);
  glGenBuffers(1, &vbo_);
  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  const GLsizei stride = kFloatsPerVert * sizeof(float);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(3 * sizeof(float)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(5 * sizeof(float)));
  glBindVertexArray(0);
  return true;
}

void PaintingRenderer::shutdown() {
  for (auto& [key, e] : textures_) {
    if (e.tex) glDeleteTextures(1, &e.tex);
  }
  textures_.clear();
  if (vbo_) glDeleteBuffers(1, &vbo_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  vbo_ = 0;
  vao_ = 0;
  haveRevision_ = false;
}

void PaintingRenderer::syncTextures(const world::World& world) {
  const std::uint32_t rev = world.paintingRevision();
  if (haveRevision_ && rev == seenRevision_) return;
  seenRevision_ = rev;
  haveRevision_ = true;

  // Drop textures whose painting is gone, and upload any that arrived. Comparing
  // one revision counter rather than 48 KB of pixels per painting per frame is the
  // whole point of the counter existing.
  for (auto it = textures_.begin(); it != textures_.end();) {
    const auto found = world.paintings().find(it->first);
    if (found == world.paintings().end() || found->second.blank()) {
      if (it->second.tex) glDeleteTextures(1, &it->second.tex);
      it = textures_.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& [key, art] : world.paintings()) {
    if (art.blank() || textures_.count(key)) continue;
    Entry e;
    glGenTextures(1, &e.tex);
    glBindTexture(GL_TEXTURE_2D, e.tex);
    // Linear, unlike every other texture in the game: this one is a photograph
    // rather than pixel art, and the nearest-neighbour look the atlas wants would
    // make a downscaled screenshot crawl.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, game::kPaintingSize, game::kPaintingSize, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, art.rgb.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    textures_[key] = e;
  }
}

void PaintingRenderer::drawGBuffer(const world::World& world, const Camera& camera) {
  drawn_ = 0;
  if (!prog_ || world.paintings().empty()) return;
  syncTextures(world);
  if (textures_.empty()) return;

  const world::BlockId canvas = world::wk().canvas;
  const Frustum& frustum = camera.frustum();

  // Corners are built in world space rather than posed by a model matrix, because
  // the quad's orientation is one of four fixed cases and writing them out is
  // shorter than composing a basis and a transform for each.
  struct Item {
    GLuint tex;
    int first;
  };
  std::vector<Item> items;
  verts_.clear();

  for (const auto& [key, art] : world.paintings()) {
    if (art.blank()) continue;
    const auto found = textures_.find(key);
    if (found == textures_.end()) continue;

    int wx = 0, wy = 0, wz = 0;
    game::unpackBlockEntityKey(key, wx, wy, wz);
    // The picture follows the block. A painting whose canvas is gone — mined while
    // its chunk was unloaded, so setBlock never ran here — draws nothing rather
    // than hanging in the air.
    if (world.getBlock(wx, wy, wz) != canvas) continue;
    if (!frustum.testAabb(static_cast<float>(wx), static_cast<float>(wy),
                          static_cast<float>(wz), static_cast<float>(wx + 1),
                          static_cast<float>(wy + 1), static_cast<float>(wz + 1))) {
      continue;
    }

    const int meta = world.getMeta(wx, wy, wz) & 3;
    const float x = static_cast<float>(wx), y = static_cast<float>(wy);
    const float z = static_cast<float>(wz);
    const float lo = kInset, hi = 1.0f - kInset;

    // Origin, then the two edge vectors, per wall. u runs right as the picture is
    // seen from in front, v runs up.
    float ox = 0, oy = y + lo, oz = 0;
    float ux = 0, uz = 0;
    switch (meta) {
      case 0:  // hung on the +x wall, seen from -x
        ox = x + 1.0f - kThick - kLift;
        oz = z + lo;
        uz = hi - lo;
        break;
      case 1:  // -x wall, seen from +x
        ox = x + kThick + kLift;
        oz = z + hi;
        uz = -(hi - lo);
        break;
      case 2:  // +z wall, seen from -z
        ox = x + hi;
        oz = z + 1.0f - kThick - kLift;
        ux = -(hi - lo);
        break;
      default:  // -z wall, seen from +z
        ox = x + lo;
        oz = z + kThick + kLift;
        ux = hi - lo;
        break;
    }
    const float vy = hi - lo;

    // Lit by the cell the painting occupies, which is the air in front of the wall
    // rather than the wall itself — the same cell the mesher lit its frame from.
    const world::LoadedChunk* lc =
        world.chunkAt(world::World::floorDiv16(wx), world::World::floorDiv16(wz));
    float sky = 1.0f, block = 0.0f;
    if (lc && lc->chunk.data) {
      sky = static_cast<float>(lc->chunk.data->sky(wx & 15, wy, wz & 15)) / 15.0f;
      block = static_cast<float>(lc->chunk.data->blockLight(wx & 15, wy, wz & 15)) / 15.0f;
    }

    const int first = static_cast<int>(verts_.size()) / kFloatsPerVert;
    const auto push = [&](float fu, float fv) {
      verts_.push_back(ox + ux * fu);
      verts_.push_back(oy + vy * fv);
      verts_.push_back(oz + uz * fu);
      verts_.push_back(fu);
      verts_.push_back(1.0f - fv);  // image rows run top-down
      verts_.push_back(sky);
      verts_.push_back(block);
    };
    push(0, 0);
    push(1, 0);
    push(1, 1);
    push(0, 0);
    push(1, 1);
    push(0, 1);
    items.push_back({found->second.tex, first});
  }

  if (items.empty()) return;

  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts_.size() * sizeof(float)),
                 verts_.data(), GL_STREAM_DRAW);

  prog_->use();
  prog_->setMat4("uViewProj", camera.viewProj().data());
  prog_->set("uPicture", 0);
  glActiveTexture(GL_TEXTURE0);
  for (const Item& it : items) {
    glBindTexture(GL_TEXTURE_2D, it.tex);
    glDrawArrays(GL_TRIANGLES, it.first, 6);
    ++drawn_;
  }
  glBindVertexArray(0);
}

}  // namespace hr::render
