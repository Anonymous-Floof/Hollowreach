// One 3D model per item, shared by every out-of-world rendition of it: dropped
// item entities, the held viewmodel, and (as the same source art) the inventory
// icons. Two kinds of mesh, picked automatically:
//
//  • "shape"  — block items with solid geometry render as their actual display
//               shape (shapes.displayBoxes: stairs are stairs, doors are doors),
//               textured from the block's atlas tiles with the mesher's face
//               shades, so a dropped slab matches a placed slab.
//  • "sprite" — everything else renders as its 16×16 sprite extruded 1px thick
//               (front + back plates plus little walls along every pixel edge),
//               textured from the atlas: non-block items use their `item:<key>`
//               tile (painted from the icon sprite grid), cross-render blocks
//               (torch, flowers) use their block tile.
//
// Meshes are built in unit space — x/z centred on the origin, y up from 0 — and
// cached; consumers place them with a model matrix (scale/rotate/translate).
// Vertex format matches the entity + viewmodel programs: pos3, uv2, shade, then
// two zeroed floats (the entity program's bone index + colour slot).

import { getBlock, texForFace } from "../world/blocks.js";
import { getItem } from "../game/items.js";
import { displayBoxes } from "../world/shapes.js";

const T = 1 / 16;          // sprite extrusion thickness (one texel)
const ALPHA = 128;         // a texel is "filled" when its alpha clears this

export class ItemMeshCache {
  constructor(gl, atlas) {
    this.gl = gl;
    this.atlas = atlas;
    this.meshes = new Map();   // item key -> { vao, vbo, count, kind } | null
  }

  // Mesh for an item key (null when the key is unknown).
  get(key) {
    let m = this.meshes.get(key);
    if (m === undefined) {
      m = this._build(key);
      this.meshes.set(key, m);
    }
    return m;
  }

  _build(key) {
    const it = getItem(key);
    if (!it) return null;
    if (it.type === "block") {
      const block = getBlock(it.blockId);
      if (block.render === "cross") return this._sprite(block.tex.all);
      return this._shape(block);
    }
    return this._sprite("item:" + key);
  }

  // ---- block shapes ---------------------------------------------------------

  // Emit the display boxes with the mesher's winding, shades and full-tile UVs
  // (the world mesher stretches the tile over each sub-box face too, so a
  // dropped stair matches a placed one). Centred: x/z shifted by -0.5.
  _shape(block) {
    const verts = [];
    const uvOf = (fi) => this.atlas.uvForName(texForFace(block, fi));
    const quad = (fi, shade, p0, p1, p2, p3) => {
      const [u0, v0, u1, v1] = uvOf(fi);
      const c = [[u0, v1], [u1, v1], [u1, v0], [u0, v0]];
      const P = [p0, p1, p2, p3];
      const w = (i) => verts.push(P[i][0] - 0.5, P[i][1], P[i][2] - 0.5, c[i][0], c[i][1], shade, 0, 0);
      w(0); w(1); w(2); w(0); w(2); w(3);
    };
    for (const [x0, y0, z0, x1, y1, z1] of displayBoxes(block.render)) {
      quad(0, 0.68, [x1, y0, z0], [x1, y0, z1], [x1, y1, z1], [x1, y1, z0]);
      quad(1, 0.68, [x0, y0, z1], [x0, y0, z0], [x0, y1, z0], [x0, y1, z1]);
      quad(2, 1.00, [x0, y1, z0], [x1, y1, z0], [x1, y1, z1], [x0, y1, z1]);
      quad(3, 0.50, [x0, y0, z1], [x1, y0, z1], [x1, y0, z0], [x0, y0, z0]);
      quad(4, 0.85, [x1, y0, z1], [x0, y0, z1], [x0, y1, z1], [x1, y1, z1]);
      quad(5, 0.85, [x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0]);
    }
    return this._upload(verts, "shape", block.render);
  }

  // ---- extruded sprites -----------------------------------------------------

  _sprite(tileName) {
    const atlas = this.atlas;
    const [rx, ry, tw, th] = atlas.pixelRect(tileName);
    const ctx = atlas.canvas.getContext("2d");
    const data = ctx.getImageData(rx, ry, tw, th).data;
    const filled = (px, py) =>
      px >= 0 && px < tw && py >= 0 && py < th && data[(py * tw + px) * 4 + 3] >= ALPHA;

    // content bounds, so the model is centred on x and rests its lowest pixel
    // on y=0 (a dropped sword shouldn't hover on its sprite's empty margin)
    let minX = tw, maxX = -1, maxY = -1;
    for (let py = 0; py < th; py++) for (let px = 0; px < tw; px++) {
      if (!filled(px, py)) continue;
      if (px < minX) minX = px;
      if (px > maxX) maxX = px;
      if (py > maxY) maxY = py;
    }
    if (maxX < 0) return null;                    // fully transparent tile
    const shX = ((minX + maxX + 1) / 2) * T;      // content centre -> x=0
    const shY = (th - 1 - maxY) * T;              // lowest content row -> y=0
    const X = (px) => px * T - shX;
    const Y = (py) => (th - py) * T - shY;

    // per-texel atlas coords (used by the edge walls); a hair of inset keeps
    // run ends from bleeding into the neighbouring tile under NEAREST sampling
    const W = atlas.canvas.width, H = atlas.canvas.height, e = 0.02;
    const U = (px, end) => (rx + px + (end ? -e : e)) / W;
    const V = (py, end) => (ry + py + (end ? -e : e)) / H;

    const verts = [];
    const quad = (shade, p0, p1, p2, p3, uv) => {
      const P = [p0, p1, p2, p3];
      const w = (i) => verts.push(P[i][0], P[i][1], P[i][2], uv[i][0], uv[i][1], shade, 0, 0);
      w(0); w(1); w(2); w(0); w(2); w(3);
    };

    // Back plate, then the edge walls, then the front plate. Emission order no
    // longer matters for correctness (every consumer depth-tests), but keeping
    // it back-to-front costs nothing and helps early-z.
    const [fu0, fv0, fu1, fv1] = atlas.uvForName(tileName);
    const zF = T / 2, zB = -T / 2;
    const xa = X(0), xb = X(tw), ya = Y(th), yb = Y(0);
    // back (-z): SAME uv-at-position mapping as the front — the alpha holes of
    // the two plates must line up exactly (a flipped back plate shows its art
    // through the front's transparent pixels as a ghosted mirror image). Seen
    // from behind the sprite mirrors naturally, like a real extruded object.
    quad(0.85, [xa, ya, zB], [xb, ya, zB], [xb, yb, zB], [xa, yb, zB],
      [[fu0, fv1], [fu1, fv1], [fu1, fv0], [fu0, fv0]]);

    // Edge walls: wherever a filled texel borders an empty one (or the tile
    // rim), stand a 1-texel-deep wall on that boundary, textured by the filled
    // texel's own row/column so the rim carries the sprite's colours. Adjacent
    // boundary texels merge into runs — one quad per run.
    // vertical walls (left = facing -x, right = facing +x)
    for (const right of [false, true]) {
      for (let px = 0; px < tw; px++) {
        const isEdge = (py) => filled(px, py) && !filled(right ? px + 1 : px - 1, py);
        for (let py = 0; py < th; py++) {
          if (!isEdge(py) || (py > 0 && isEdge(py - 1))) continue;
          let py1 = py;
          while (py1 + 1 < th && isEdge(py1 + 1)) py1++;
          const x = X(right ? px + 1 : px), y0 = Y(py1 + 1), y1 = Y(py);
          const u = (rx + px + 0.5) / W, v0 = V(py, false), v1 = V(py1 + 1, true);
          const uv = [[u, v1], [u, v1], [u, v0], [u, v0]];
          if (right) quad(0.68, [x, y0, zB], [x, y0, zF], [x, y1, zF], [x, y1, zB], uv);
          else quad(0.68, [x, y0, zF], [x, y0, zB], [x, y1, zB], [x, y1, zF], uv);
        }
      }
    }
    // horizontal walls (top = facing +y, bottom = facing -y)
    for (const top of [true, false]) {
      for (let py = 0; py < th; py++) {
        const isEdge = (px) => filled(px, py) && !filled(px, top ? py - 1 : py + 1);
        for (let px = 0; px < tw; px++) {
          if (!isEdge(px) || (px > 0 && isEdge(px - 1))) continue;
          let px1 = px;
          while (px1 + 1 < tw && isEdge(px1 + 1)) px1++;
          const y = Y(top ? py : py + 1), x0 = X(px), x1 = X(px1 + 1);
          const v = (ry + py + 0.5) / H, u0 = U(px, false), u1 = U(px1 + 1, true);
          const uv = [[u0, v], [u1, v], [u1, v], [u0, v]];
          if (top) quad(1.0, [x0, y, zB], [x1, y, zB], [x1, y, zF], [x0, y, zF], uv);
          else quad(0.5, [x0, y, zF], [x1, y, zF], [x1, y, zB], [x0, y, zB], uv);
        }
      }
    }

    // front (+z): art upright and unmirrored for the viewer the face points at
    quad(0.85, [xa, ya, zF], [xb, ya, zF], [xb, yb, zF], [xa, yb, zF],
      [[fu0, fv1], [fu1, fv1], [fu1, fv0], [fu0, fv0]]);
    return this._upload(verts, "sprite");
  }

  // `shape` is the block's render shape ("stair", "door", ...) for solid block
  // items, so the viewmodel can hold a door differently from a cube without
  // needing the block table.
  _upload(verts, kind, shape = null) {
    const gl = this.gl;
    const arr = new Float32Array(verts);
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    const vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.bufferData(gl.ARRAY_BUFFER, arr, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 32, 0);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 2, gl.FLOAT, false, 32, 12);
    gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 1, gl.FLOAT, false, 32, 20);
    gl.enableVertexAttribArray(3); gl.vertexAttribPointer(3, 1, gl.FLOAT, false, 32, 24);
    gl.enableVertexAttribArray(4); gl.vertexAttribPointer(4, 1, gl.FLOAT, false, 32, 28);
    gl.bindVertexArray(null);
    return { vao, vbo, count: arr.length / 8, kind, shape };
  }
}
