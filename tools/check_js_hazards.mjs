// Static scan for a line break after a keyword where engines disagree on the
// parse: js_of_ocaml printed `static`, a line break, then a field name, and
// JavaScriptCore 17 read `static` as its own field, leaving
// `MlInt64.UNSIGNED_MAX` undefined on iOS 17. `return`/`yield`/`break`/
// `continue` are worse: ASI inserts a semicolon and silently changes the
// meaning everywhere, on every engine. `static` is a reserved word in strict
// mode, so a minified identifier can't collide with it.
//
//   node tools/check_js_hazards.mjs <file or dir> ...

import { readFile, readdir, stat } from "node:fs/promises";
import path from "node:path";

const HAZARDS = [
  ["static", /\bstatic[\t ]*\r?\n/g, "JavaScriptCore 17 reads this as a field named `static`"],
  ["return", /\breturn[\t ]*\r?\n[\t ]*[A-Za-z_$"'`[(]/g, "ASI inserts a semicolon: the value is dropped"],
  ["throw", /\bthrow[\t ]*\r?\n/g, "ASI inserts a semicolon: this is a syntax error"],
  ["yield", /\byield[\t ]*\r?\n[\t ]*[A-Za-z_$"'`[(]/g, "ASI inserts a semicolon: the value is dropped"],
  ["break", /\bbreak[\t ]*\r?\n[\t ]*[A-Za-z_$]/g, "ASI inserts a semicolon: the label is dropped"],
  ["continue", /\bcontinue[\t ]*\r?\n[\t ]*[A-Za-z_$]/g, "ASI inserts a semicolon: the label is dropped"],
  // `async` is a legal identifier, so only the two forms a line break
  // actually changes are worth reporting.
  ["async", /\basync[\t ]*\r?\n[\t ]*(?:function\b|\()/g, "the line break makes this a plain identifier, not an async function"],
];

async function targets(arg) {
  let info;
  try {
    info = await stat(arg);
  } catch {
    throw new Error(`cannot read ${arg}`);
  }
  if (info.isFile()) return [arg];
  const names = await readdir(arg);
  return names.filter((n) => n.endsWith(".js") || n.endsWith(".mjs")).map((n) => path.join(arg, n));
}

const roots = process.argv.slice(2);
if (!roots.length) {
  console.error("usage: node tools/check_js_hazards.mjs <file or directory> ...");
  process.exit(2);
}

let hits = 0;
for (const root of roots) {
  let files;
  try {
    files = await targets(root);
  } catch (e) {
    console.error(String(e.message ?? e));
    process.exit(2);
  }
  for (const file of files) {
    const text = await readFile(file, "utf8");
    for (const [name, re, why] of HAZARDS) {
      re.lastIndex = 0;
      for (let m; (m = re.exec(text)); ) {
        const line = text.slice(0, m.index).split("\n").length;
        const next = text.slice(m.index, m.index + 60).replace(/\n/g, "\\n");
        console.log(`${file}:${line}: line break after \`${name}\` -- ${why}`);
        console.log(`  ${next}`);
        ++hits;
      }
    }
  }
}

if (hits) console.log(`\n${hits} hazard${hits === 1 ? "" : "s"}; the fix belongs in whatever generated the file`);
process.exit(hits ? 1 : 0);
