// Run only against the separate probe_codegen_wasm target. No shipped exports.
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const {performance} = require('perf_hooks');
(async () => {
  const runtimePath = path.resolve(process.argv[2]);
  const generatedPath = path.resolve(process.argv[3]);
  const module = new WebAssembly.Module(fs.readFileSync(runtimePath.replace(/\.js$/, '.wasm')));
  let wasm;
  const M = await require(runtimePath)({instantiateWasm(imports, receive) {
    wasm = new WebAssembly.Instance(module, imports);
    receive(wasm, module);
    return wasm.exports;
  }});
  const generatedBytes = fs.readFileSync(generatedPath);
  const compileStart = performance.now();
  const generatedModule = new WebAssembly.Module(generatedBytes);
  const compileUs = (performance.now() - compileStart) * 1000;
  const memoryExport = WebAssembly.Module.exports(module).find(e => e.kind === "memory");
  const memory = wasm.exports[memoryExport.name];
  assert(memory instanceof WebAssembly.Memory);
  const start = performance.now();
  const generated = new WebAssembly.Instance(generatedModule, {
    env: {memory},
    stanli: {forward: M._probe_forward, reverse: M._probe_reverse},
  }).exports;
  const instantiationUs = (performance.now() - start) * 1000;
  const buffer = M._malloc(16 * 8);
  const input = buffer, output = buffer + 32, inputAdj = buffer + 64, outputAdj = buffer + 96;
  const handle = M._probe_create(input, output, inputAdj, outputAdj);
  assert(handle);
  const read = pointer => Array.from(M.HEAPF64.subarray(pointer / 8, pointer / 8 + 4));
  const seed = () => {
    M.HEAPF64.fill(0, inputAdj / 8, inputAdj / 8 + 4);
    M.HEAPF64.set([1.25, -0.75, 0.3, 2.0], outputAdj / 8);
  };
  const direct = x => {
    M.HEAPF64.set([x*x+1, .2, .2, 3], input / 8);
    M._probe_forward(handle);
    const values = read(output);
    seed(); M._probe_reverse(handle);
    return {values, adjoints: read(inputAdj), dx: (2*x)*M.HEAPF64[inputAdj / 8]};
  };
  const compiled = x => {
    generated.forward(handle, input, x);
    const values = read(output);
    seed(); const dx = generated.reverse(handle, inputAdj, x);
    return {values, adjoints: read(inputAdj), dx};
  };
  for (const x of [1.5, -0.7, 0, 0.3]) assert.deepStrictEqual(compiled(x), direct(x));
  let directRejected = false, generatedRejected = false;
  try { direct(NaN); } catch (e) { directRejected = true; }
  try { compiled(NaN); } catch (e) { generatedRejected = true; }
  assert(directRejected && generatedRejected);
  assert.deepStrictEqual(compiled(.8), direct(.8));
  assert.deepStrictEqual(read(outputAdj), [0, 0, 0, 0]);
  const accumulated = read(inputAdj);
  generated.reverse(handle, inputAdj, .8);
  assert.deepStrictEqual(read(inputAdj), accumulated);
  assert.deepStrictEqual(read(outputAdj), [0, 0, 0, 0]);
  const beforeGrowth = memory.buffer.byteLength;
  const growthBuffer = M._malloc(beforeGrowth + 65536);
  assert(growthBuffer && memory.buffer.byteLength > beforeGrowth);
  assert.deepStrictEqual(compiled(.9), direct(.9));
  M._free(growthBuffer);
  function measure(fn, ms) {
    const start = performance.now(); let count = 0;
    do { for (let i = 0; i < 100; ++i) { fn(); ++count; } }
    while (performance.now() - start < ms);
    return (performance.now() - start) * 1e6 / count;
  }
  const directCall = () => M._probe_forward(handle);
  const importedCall = () => generated.raw_forward(handle);
  measure(directCall, 200); measure(importedCall, 200);
  const samples = [];
  for (let pair = 0; pair < 6; ++pair) {
    let directNs, importedNs;
    if (pair % 2) { importedNs = measure(importedCall, 250); directNs = measure(directCall, 250); }
    else { directNs = measure(directCall, 250); importedNs = measure(importedCall, 250); }
    samples.push({pair, directNs, importedNs});
  }
  M._probe_free(handle); M._free(buffer);
  console.log(JSON.stringify({parity: 'passed', exceptionRecovery: 'passed', memoryGrowth: 'passed', adjointConsumption: 'passed',
    memoryBefore: beforeGrowth, memoryAfter: memory.buffer.byteLength, compileUs, instantiationUs,
    generatedImports: WebAssembly.Module.imports(generatedModule), samples}, null, 2));
})().catch(error => {console.error(error); process.exitCode = 1;});
