// The webR runtime artifact loads into webR and resolves the C API.
//
// This is the ABI question the emsdk pin exists to answer: a side module
// built against the wrong Emscripten fails right here, in dyn.load. The
// full R-package flow (r-universe bridge, log_prob_grad, sampling) was
// verified by hand against webR 0.6.0 when this landed; this test keeps
// the load-and-resolve half, which needs nothing beyond the npm webr
// package.
//
// Usage: node tests/test_webr.mjs build-wasm-side/libstanli.so
import { WebR } from 'webr';
import fs from 'node:fs';

const soPath = process.argv[2];
if (!soPath) {
  console.error('usage: node tests/test_webr.mjs <libstanli.so>');
  process.exit(2);
}

const webR = new WebR();
await webR.init();
const arch = (await (await webR.evalR('R.version$arch')).toJs()).values[0];
if (arch !== 'wasm32') {
  console.error(`FAIL unexpected webR arch ${arch}`);
  process.exit(1);
}

await webR.FS.writeFile('/tmp/libstanli.so',
                        new Uint8Array(fs.readFileSync(soPath)));

// The symbols the R bridge dlsyms, stanli_abi_version first: it is the
// handshake, and the one a hand-kept export list forgot once already.
const res = await webR.evalR(`
  dyn.load('/tmp/libstanli.so')
  syms <- c('stanli_abi_version', 'stanli_model_new', 'stanli_exact_lp',
            'stanli_grad', 'stanli_sample_multi', 'stanli_optimize',
            'stanli_diagnose_text', 'stanli_wa_row')
  syms[!vapply(syms, is.loaded, logical(1))]
`);
const missing = (await res.toJs()).values;
if (missing.length > 0) {
  console.error('FAIL symbols missing from the side module:', missing.join(' '));
  process.exit(1);
}
console.log('webr load OK: side module loads and the bridge symbols resolve');
process.exit(0);
