// Loads the shipped bundle in chromium, firefox and webkit with the same
// model and seed, and requires every column and sampler stat to agree bit
// for bit; wasm_check.sh only verifies the numerics under Node (V8).
// --bundle <dir> (default js/), --engines <comma list>. Needs playwright:
// npm i -D playwright && npx playwright install chromium firefox webkit.

import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import path from "node:path";

// build_web.sh renames the entry point when staging web/, so it's looked up.
const ENTRIES = ["index.mjs", "stanli.mjs"];
const ARTIFACTS = [
  "worker.js",
  "stanli.js",
  "stanli.wasm",
  "stanli-compiler.js",
  "stancjs.bc.js",
];

const ENGINES = ["chromium", "firefox", "webkit"];

const MODEL = `
data { int N; array[N] real y; }
parameters { real mu; real<lower=0> sigma; }
model { y ~ normal(mu, sigma); }`;

const DATA = { N: 8, y: [1.1, 0.4, 2.2, -0.3, 1.7, 0.9, 2.6, 0.1] };
const SEED = 20260907;
const SAMPLE_TIMEOUT_MS = 180000;

const MIME = {
  ".mjs": "text/javascript",
  ".js": "text/javascript",
  ".wasm": "application/wasm",
  ".html": "text/html",
};

function parseArgs(argv) {
  const out = { bundle: "js", engines: ENGINES };
  for (let i = 0; i < argv.length; ++i) {
    const value = () => {
      if (i + 1 >= argv.length) throw new Error(`${argv[i]} needs a value`);
      return argv[++i];
    };
    if (argv[i] === "--bundle") out.bundle = value();
    else if (argv[i] === "--engines") out.engines = value().split(",").filter(Boolean);
    else throw new Error(`unknown argument: ${argv[i]}`);
  }
  const unknown = out.engines.filter((e) => !ENGINES.includes(e));
  if (unknown.length) {
    throw new Error(`unknown engine: ${unknown.join(", ")}; pick from ${ENGINES.join(", ")}`);
  }
  if (!out.engines.length) throw new Error("--engines matched nothing");
  return out;
}

// Hex of the raw bytes, not the decimal rendering, so last-bit differences
// between engines can't hide.
const PAGE = `<!doctype html><meta charset="utf-8"><title>stanli browser smoke</title>
<script type="module">
import { sample } from "./__ENTRY__";
const hex = (a) => Array.from(new Uint8Array(a.buffer, a.byteOffset, a.byteLength))
  .map((b) => b.toString(16).padStart(2, "0")).join("");
window.__run = async (code, data, seed) => {
  const fit = await sample({ code, data, seed });
  const digests = {};
  for (const name of fit.names) {
    const col = fit.columns[name];
    if (col) digests[name] = hex(col);
  }
  if (fit.samplerStats) digests["__samplerStats"] = hex(fit.samplerStats);
  const mu = fit.columns["mu"];
  return {
    names: fit.names,
    digests,
    n: mu.length,
    mean: mu.reduce((s, v) => s + v, 0) / mu.length,
  };
};
window.__ready = true;
</script>`;

async function serve(dir, entry) {
  const server = createServer(async (req, res) => {
    const name = decodeURIComponent(new URL(req.url, "http://x").pathname);
    if (name === "/") {
      res.writeHead(200, { "content-type": "text/html" });
      return res.end(PAGE.replace("__ENTRY__", entry));
    }
    try {
      const body = await readFile(path.join(dir, path.basename(name)));
      res.writeHead(200, {
        "content-type": MIME[path.extname(name)] ?? "application/octet-stream",
      });
      res.end(body);
    } catch {
      res.writeHead(404).end();
    }
  });
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  return { server, port: server.address().port };
}

function withTimeout(promise, ms, what) {
  let timer;
  const bell = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`${what} did not finish within ${ms} ms`)), ms);
  });
  return Promise.race([promise, bell]).finally(() => clearTimeout(timer));
}

async function runEngine(playwright, name, port) {
  const browser = await playwright[name].launch();
  try {
    const page = await browser.newPage();
    const errors = [];
    page.on("pageerror", (e) => errors.push(String(e)));
    await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: "load" });
    await page.waitForFunction("window.__ready === true", null, { timeout: 60000 });
    const out = await withTimeout(
      page.evaluate(([code, data, seed]) => window.__run(code, data, seed), [MODEL, DATA, SEED]),
      SAMPLE_TIMEOUT_MS,
      "sampling",
    );
    if (errors.length) throw new Error(errors.join("\n"));
    return out;
  } finally {
    await browser.close();
  }
}

let args;
try {
  args = parseArgs(process.argv.slice(2));
} catch (e) {
  console.error(String(e.message ?? e));
  process.exit(2);
}

const entry = ENTRIES.find((f) => existsSync(path.join(args.bundle, f)));
const missing = ARTIFACTS.filter((f) => !existsSync(path.join(args.bundle, f)));
if (!entry || missing.length) {
  const want = entry ? missing : [ENTRIES.join(" or "), ...missing];
  console.error(`${args.bundle} is missing ${want.join(", ")}`);
  console.error("build it with tools/build_web.sh, or point --bundle at an unpacked npm package");
  process.exit(2);
}

let playwright;
try {
  playwright = await import("playwright");
} catch {
  console.error("playwright is not installed; see the header of this file");
  process.exit(2);
}

const { server, port } = await serve(args.bundle, entry);
const results = new Map();
let engineFailed = false;

for (const name of args.engines) {
  const started = Date.now();
  try {
    const out = await runEngine(playwright, name, port);
    results.set(name, out);
    const ms = Date.now() - started;
    console.log(
      `${name.padEnd(9)} ok    ${out.n} draws  mean(mu)=${out.mean.toFixed(6)}  ${ms}ms`,
    );
  } catch (e) {
    engineFailed = true;
    console.log(`${name.padEnd(9)} FAIL  ${String(e.message ?? e).split("\n")[0]}`);
  }
}
server.close();

// Tracked apart from per-engine failures so one engine failing to launch
// doesn't hide the comparison among the engines that did run.
let mismatched = false;
const ran = [...results.keys()];
if (ran.length > 1) {
  const [ref, ...rest] = ran;
  for (const other of rest) {
    const a = results.get(ref);
    const b = results.get(other);
    if (a.names.join(" ") !== b.names.join(" ")) {
      console.log(`MISMATCH  column layout: ${ref} and ${other} disagree`);
      mismatched = true;
    }
    for (const key of Object.keys(a.digests)) {
      if (a.digests[key] !== b.digests[key]) {
        console.log(`MISMATCH  ${key}: ${ref} and ${other} disagree bit for bit`);
        mismatched = true;
      }
    }
  }
  const keys = Object.keys(results.get(ref).digests);
  const columns = keys.filter((k) => k !== "__samplerStats").length;
  const also = keys.includes("__samplerStats") ? " and the sampler statistics" : "";
  if (!mismatched) {
    console.log(`agree     ${ran.join(", ")} identical over ${columns} columns${also}`);
  }
} else if (ran.length === 1) {
  console.log(`only ${ran[0]} ran; nothing to compare`);
}

process.exit(engineFailed || mismatched ? 1 : 0);
