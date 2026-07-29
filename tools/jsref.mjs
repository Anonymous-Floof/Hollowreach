// Locating the archived JavaScript build, which is no longer in this checkout.
//
// Hollowreach was forked out of the web version's repository and the js/ tree
// stayed behind with the archive. The golden dumpers still run that code — it is
// the reference every determinism claim in this port is measured against — so it
// has to be found rather than assumed.
//
// Point HOLLOWREACH_JS at the js/ directory of a checkout of the archived repo:
//
//   Windows   set HOLLOWREACH_JS=C:\path\to\Hollow-Reach\js
//   POSIX     export HOLLOWREACH_JS=/path/to/Hollow-Reach/js
//
// Failing that a couple of conventional sibling layouts are tried, so somebody who
// cloned both repos next to each other needs no environment at all. The check is
// for a file that only the real thing has, not merely for a directory called js —
// an empty or half-copied tree should fail here with a clear message rather than
// three imports later with a module resolution error.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const kProbe = path.join("world", "worldgen.js");

function candidates(toolsDir) {
  const out = [];
  if (process.env.HOLLOWREACH_JS) out.push(path.resolve(process.env.HOLLOWREACH_JS));
  const repo = path.resolve(toolsDir, "..");
  const parent = path.resolve(repo, "..");
  // A sibling clone of the archived repo, under any of the names it has had.
  for (const name of ["Hollow-Reach", "Hollowreach-web", "Minecraft Clone"]) {
    out.push(path.join(parent, name, "js"));
  }
  return out;
}

// Returns the absolute path to the reference js/ directory, or exits with a message
// that says exactly what to do about it.
export function findJsDir(importMetaUrl) {
  const toolsDir = path.dirname(fileURLToPath(importMetaUrl));
  const tried = candidates(toolsDir);
  for (const dir of tried) {
    if (fs.existsSync(path.join(dir, kProbe))) return dir;
  }
  process.stderr.write(
    "error: the reference JavaScript build was not found.\n\n" +
      "  The golden vectors diff this port against the original web version, which\n" +
      "  lives in the archived repository this one was forked from:\n" +
      "      https://github.com/Anonymous-Floof/Hollow-Reach\n\n" +
      "  Clone it and point HOLLOWREACH_JS at its js/ directory.\n\n" +
      "  Looked in:\n" +
      tried.map((d) => `    ${d}\n`).join(""),
  );
  process.exit(2);
}

// The loader every dumper builds on: an ES import of a module inside that tree.
export function makeLoader(jsDir) {
  return (rel) => import(pathToFileURL(path.join(jsDir, rel)).href);
}
