// Verifies the chip wasm can instantiate against a 2-page imported memory (what
// the Wokwi simulator supplies) and that chip_init() can grow it via malloc.
// Run: node chip/tools/check-memory.js
const fs = require('node:fs');
const path = require('node:path');

const wasmPath = path.join(__dirname, '..', 'dist', 'logic-scope.chip.wasm');
const mod = new WebAssembly.Module(fs.readFileSync(wasmPath));

const memory = new WebAssembly.Memory({ initial: 2 });
const dv = () => new DataView(memory.buffer);
const pages = () => memory.buffer.byteLength / 65536;

// Must match SCREEN_W / SCREEN_H in src/render.h.
const renderH = fs.readFileSync(path.join(__dirname, '..', 'src', 'render.h'), 'utf8');
const screenW = Number(/#define SCREEN_W (\d+)/.exec(renderH)[1]);
const screenH = Number(/#define SCREEN_H (\d+)/.exec(renderH)[1]);

const env = {
  memory,
  framebufferInit(wPtr, hPtr) {
    dv().setUint32(wPtr, screenW, true);
    dv().setUint32(hPtr, screenH, true);
    return 1;
  },
};
const wasi = {
  fd_write(fd, iovs, iovsLen, nwrittenPtr) {
    let total = 0;
    for (let i = 0; i < iovsLen; i++) {
      const ptr = dv().getUint32(iovs + i * 8, true);
      const len = dv().getUint32(iovs + i * 8 + 4, true);
      process.stdout.write(Buffer.from(memory.buffer, ptr, len).toString());
      total += len;
    }
    dv().setUint32(nwrittenPtr, total, true);
    return 0;
  },
};

const stub = (base) => new Proxy(base, { get: (t, k) => (k in t ? t[k] : () => 0) });
const inst = new WebAssembly.Instance(mod, {
  env: stub(env),
  wasi_snapshot_preview1: stub(wasi),
});

console.log(`instantiated with ${pages()} pages`);
inst.exports.chipInit();
console.log(`after chipInit: ${pages()} pages (${memory.buffer.byteLength} bytes)`);
