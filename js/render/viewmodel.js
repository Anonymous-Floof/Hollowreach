// The first-person hand: how the held item is posed, framed and animated.
//
// Everything about the viewmodel lives here so that adding an item never means
// touching the renderer:
//
//  • STYLES   — a table of hold styles, pure data. Where the grip sits on the
//               screen, how the item is turned in the hand, and how big it
//               reads. Add a style, or tweak one, without reading a line of GL.
//  • styleFor — resolves an item to a style. An item can name one explicitly
//               with `hold: "..."` in the registry; otherwise it falls out of
//               its type (tool / sword / food / block) so new items inherit a
//               sensible pose for free.
//  • Viewmodel — the animation clocks (swing, equip, walk sway, idle drift).
//               They only ever nudge the same numbers the style defines, so an
//               animation can never disagree with a pose.
//
// The framing is deliberately resolution-independent: `anchor` is a fraction of
// the half-frame (±1 = screen edge) and `size` is a fraction of the frame
// HEIGHT, both measured at the hold distance. So the item sits in the same spot
// of the picture on an ultrawide monitor, a phone, or a 4:3 window — the old
// fixed view-space offsets slid off the edge as the aspect ratio narrowed.

// Every style is complete (no inheritance to trace):
//   rot    [x, y, z]  radians, applied roll(z) -> pitch(x) -> yaw(y) about the grip.
//                     +z rolls the item's top to the LEFT, -x tips its top AWAY
//                     into the scene, -y turns its face back toward the camera.
//   anchor [x, y]     where the grip lands, as a fraction of the half-frame.
//                     [0,0] is the crosshair, [1,-1] the bottom-right corner.
//   size              model height as a fraction of the frame height. The mesh
//                     is unit-scaled (1 = one block, or one full 16px sprite),
//                     so a slab really does read as half a block.
//   dist              how far in front of the camera the item hangs.
//   grip   [x, y, z]  the model-space point that lands on the anchor. Item
//                     meshes are built bottom-centred, so [0,0,0] is the butt of
//                     a tool's handle / the bottom of a block.
//   flip              mirror the model's x. Sword art is drawn hilt-low-left,
//                     blade-high-right (the icon convention); mirrored, the hilt
//                     falls into the hand and the blade reaches for the crosshair.
export const STYLES = {
  // A block, held out at the classic three-quarter angle: you can see its top
  // and two sides at once, so stairs read as stairs and slabs as slabs.
  block: {
    rot: [0.30, -0.66, 0], anchor: [0.66, -0.95], size: 0.34, dist: 0.62,
    grip: [0, 0, 0], flip: false,
  },
  // Pick / axe / shovel: handle butt in the hand at the bottom right, rolled so
  // the head leans up toward the crosshair, tipped away so it reaches into the
  // scene, and turned a few degrees off face-on so the extruded edge shows and
  // the sprite reads as an object rather than a sticker.
  tool: {
    rot: [-0.28, -0.46, 0.52], anchor: [0.62, -1.02], size: 0.56, dist: 0.62,
    grip: [0, 0, 0], flip: false,
  },
  // A sword stands closer to upright than a tool. Its art is drawn on the
  // diagonal, so the roll here is NEGATIVE — it cancels most of the 45° the
  // sprite already has and lifts the blade up the screen.
  sword: {
    rot: [-0.22, -0.46, -0.48], anchor: [0.62, -1.06], size: 0.58, dist: 0.62,
    grip: [0.30, 0.06, 0], flip: true,
  },
  // Food is brought up nearer the middle, turned to face you — you are about to
  // eat it, so it should be readable.
  food: {
    rot: [-0.10, -0.34, 0.22], anchor: [0.58, -0.92], size: 0.40, dist: 0.62,
    grip: [0, 0, 0], flip: false,
  },
  // Doors, trapdoors and ladders are flat panels: held at a cube's angle they
  // are a meaningless sliver, so they get turned to show their face.
  panel: {
    rot: [0.10, -1.15, 0.18], anchor: [0.66, -0.92], size: 0.44, dist: 0.62,
    grip: [0, 0, 0], flip: false,
  },
  // Everything else: ingots, gems, buckets, torches, the atlas. Mostly face-on
  // with a hint of turn so the extrusion catches the light.
  item: {
    rot: [-0.10, -0.42, 0.20], anchor: [0.66, -0.98], size: 0.36, dist: 0.62,
    grip: [0, 0, 0], flip: false,
  },
};

// Block render shapes that want something other than the cube pose. Keyed on
// the shape rather than the block, so every wood's door gets it for free.
const SHAPE_STYLES = { door: "panel", trapdoor: "panel", ladder: "panel" };

// Which style an item is held in. `mesh` is its entry from the shared item-model
// cache (render/itemmesh.js): `kind` is "shape" for solid blocks and "sprite"
// for everything else, `shape` names a block's render shape.
export function styleFor(item, mesh) {
  if (item && item.hold && STYLES[item.hold]) return STYLES[item.hold];
  if (mesh && mesh.kind === "shape") {
    return STYLES[SHAPE_STYLES[mesh.shape]] || STYLES.block;
  }
  if (item) {
    if (item.toolType === "sword") return STYLES.sword;
    if (item.type === "tool") return STYLES.tool;
    if (item.type === "food") return STYLES.food;
  }
  return STYLES.item;
}

const SWING = 0.26;   // seconds for one swing arc
const EQUIP = 0.18;   // seconds for a newly selected item to rise into frame

export class Viewmodel {
  constructor() {
    this.key = null;      // item key currently in hand
    this.swingT = 1;      // 0..1 through a swing; >= 1 means idle
    this.equipT = 1;      // 0 = fully lowered out of frame, 1 = in place
    this.clock = 0;       // free-running seconds, for the idle drift
    this.bob = null;      // {phase, mag} from the player's walk cycle
    this._queued = false; // a swing asked for while one was already running
  }

  // Called every frame with the selected hotbar key: a change lowers the old
  // item and raises the new one.
  setItem(key) {
    if (key === this.key) return;
    this.key = key;
    this.equipT = 0;
    this.swingT = 1;
    this._queued = false;
  }

  // Start a swing, or queue one so a swing triggered mid-arc still happens.
  // Held mining calls this every frame, which chains into a continuous swing.
  swing() {
    if (this.swingT >= 1) this.swingT = 0;
    else this._queued = true;
  }

  update(dt, bob) {
    this.clock += dt;
    this.bob = bob;
    if (this.equipT < 1) this.equipT = Math.min(1, this.equipT + dt / EQUIP);
    if (this.swingT < 1) {
      this.swingT += dt / SWING;
      if (this.swingT >= 1 && this._queued) { this.swingT = 0; this._queued = false; }
    }
  }

  // Build the view-space model matrix for one item (column-major, for GL).
  // `item` is the registry entry (may be null), `mesh` its item-model entry.
  modelMatrix(out, item, mesh, fovRad, aspect) {
    const st = styleFor(item, mesh);
    let [rx, ry, rz] = st.rot;
    let ax = st.anchor[0], ay = st.anchor[1];

    // --- swing: a chop that drops toward the crosshair, rolls the head over and
    // snaps back. `g` is the broad arc (fast out, slow back), `f` peaks later
    // and harder — the moment of contact. Both are 0 at the ends, so the pose is
    // untouched at rest no matter how the style is tuned.
    if (this.swingT < 1) {
      const t = this.swingT;
      const g = Math.sin(Math.sqrt(t) * Math.PI);
      const f = Math.sin(t * t * Math.PI);
      ax -= 0.13 * g;
      ay -= 0.20 * g;
      rx -= 0.78 * g;
      rz -= 0.26 * g;
      ry += 0.30 * f;
    }

    // --- equip: the item rises from below the frame when you switch slots
    if (this.equipT < 1) {
      const e = 1 - this.equipT;
      ay -= 0.95 * e;
      rx -= 0.35 * e;
      rz += 0.20 * e;
    }

    // --- walk sway: the same bob phase that moves the camera, so the hand and
    // the head agree about which foot is down
    const bob = this.bob;
    if (bob && bob.mag > 0.001) {
      const m = bob.mag;
      ax += Math.cos(bob.phase) * 0.10 * m;
      ay -= Math.abs(Math.sin(bob.phase)) * 0.10 * m;
      rz += Math.sin(bob.phase) * 0.05 * m;
    }

    // --- idle drift: a slow breath so a still hand is never perfectly frozen
    const c = this.clock;
    ax += Math.sin(c * 0.63) * 0.010;
    ay += Math.sin(c * 0.91) * 0.013;
    rz += Math.sin(c * 0.77) * 0.014;

    // --- framing: anchor and size are fractions of the frame at `dist`, so the
    // item keeps its place in the picture at any aspect ratio
    const halfH = st.dist * Math.tan(fovRad / 2);
    const px = ax * halfH * aspect, py = ay * halfH, pz = -st.dist;
    const s = st.size * 2 * halfH;

    // --- compose T(p) . Ry . Rx . Rz . S . T(-grip) . flip.
    // Written out rather than built from four matrix products: this runs every
    // frame for one object, and the expanded form is the whole transform in one
    // place. R = Ry*Rx*Rz, rows below, stored column-major for GL.
    const cx = Math.cos(rx), sx = Math.sin(rx);
    const cy = Math.cos(ry), sy = Math.sin(ry);
    const cz = Math.cos(rz), sz = Math.sin(rz);
    const r00 = cy * cz + sy * sx * sz, r01 = -cy * sz + sy * sx * cz, r02 = sy * cx;
    const r10 = cx * sz,                r11 = cx * cz,                 r12 = -sx;
    const r20 = -sy * cz + cy * sx * sz, r21 = sy * sz + cy * sx * cz, r22 = cy * cx;

    const fx = st.flip ? -s : s;          // column 0 carries the mirror
    out[0] = r00 * fx; out[1] = r10 * fx; out[2] = r20 * fx; out[3] = 0;
    out[4] = r01 * s;  out[5] = r11 * s;  out[6] = r21 * s;  out[7] = 0;
    out[8] = r02 * s;  out[9] = r12 * s;  out[10] = r22 * s; out[11] = 0;
    // The grip is given in MIRRORED model space (what you see), so it moves
    // along the un-mirrored rotated axes — hence r00*s here, not out[0].
    const [gx, gy, gz] = st.grip;
    out[12] = px - (r00 * gx + r01 * gy + r02 * gz) * s;
    out[13] = py - (r10 * gx + r11 * gy + r12 * gz) * s;
    out[14] = pz - (r20 * gx + r21 * gy + r22 * gz) * s;
    out[15] = 1;
    return out;
  }
}
