#!/usr/bin/env bash
# Assemble the browser demo in web/: build stanli.wasm (emsdk), build
# stancjs (opam switch with js_of_ocaml), copy both next to index.html.
# Serve with: python3 -m http.server -d web
set -euo pipefail
cd "$(dirname "$0")/.."

source deps/emsdk/emsdk_env.sh >/dev/null 2>&1
emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-wasm -j8 --target stanli_wasm

if [ ! -f deps/stanc3-src/_build/default/src/stancjs/stancjs.bc.js ]; then
  (cd deps/stanc3-src && eval "$(opam env --switch=stanc3-55)" \
    && dune build --profile release src/stancjs/stancjs.bc.js)
fi

# js/ is the npm package: wrapper + worker + the three artifacts.
cp build-wasm/stanli.js build-wasm/stanli.wasm js/
rm -f js/stancjs.bc.js
cp deps/stanc3-src/_build/default/src/stancjs/stancjs.bc.js js/
chmod +w js/stancjs.bc.js

# web/ is the demo page, assembled as a consumer of the package.
cp js/index.mjs web/stanli.mjs
cp js/worker.js js/stanli.js js/stanli.wasm js/stancjs.bc.js web/
ls -la web/
