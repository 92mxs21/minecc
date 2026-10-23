// docs/worker.js — Off-Main-Thread C++20 `minecc.wasm` Worker (Zero UI Thread Blocking!)
let wasm = null;
let memU8 = null;
const dec = new TextDecoder('utf-8');
const enc = new TextEncoder();

function refreshViews() {
  if (!wasm) return;
  memU8 = new Uint8Array(wasm.memory.buffer);
}

function writeCStr(str) {
  const bytes = enc.encode(str + '\0');
  const ptr = wasm.wasm_alloc(bytes.length);
  refreshViews();
  memU8.set(bytes, ptr);
  return ptr;
}

function readResultJson(ptr) {
  refreshViews();
  const len = wasm.wasm_result_len();
  const raw = memU8.subarray(ptr, ptr + len);
  return JSON.parse(dec.decode(raw));
}

async function initWasm() {
  if (wasm) return;
  // Minimal WASI preview1 shim for Zig C++ reactor module (`minecc.wasm` = 384 KB!)
  const wasi = {
    args_get: () => 0,
    args_sizes_get: (argcPtr, argvBufPtr) => {
      const dv = new DataView(wasm.memory.buffer);
      dv.setUint32(argcPtr, 0, true);
      dv.setUint32(argvBufPtr, 0, true);
      return 0;
    },
    environ_get: () => 0,
    environ_sizes_get: (cPtr, bPtr) => {
      const dv = new DataView(wasm.memory.buffer);
      dv.setUint32(cPtr, 0, true);
      dv.setUint32(bPtr, 0, true);
      return 0;
    },
    clock_time_get: (_id, _prec, timePtr) => {
      const dv = new DataView(wasm.memory.buffer);
      dv.setBigUint64(timePtr, BigInt(Math.floor(performance.now() * 1e6)), true);
      return 0;
    },
    fd_close: () => 0,
    fd_fdstat_get: () => 0,
    fd_seek: () => 0,
    fd_read: () => 0,
    fd_write: (_fd, iovsPtr, iovsLen, nwrittenPtr) => {
      const dv = new DataView(wasm.memory.buffer);
      let total = 0;
      for (let i = 0; i < iovsLen; i++) total += dv.getUint32(iovsPtr + i * 8 + 4, true);
      dv.setUint32(nwrittenPtr, total, true);
      return 0;
    },
    proc_exit: () => {},
    random_get: (bufPtr, bufLen) => {
      refreshViews();
      crypto.getRandomValues(memU8.subarray(bufPtr, bufPtr + bufLen));
      return 0;
    }
  };

  const resp = await fetch('./minecc.wasm');
  const bytes = await resp.arrayBuffer();
  const { instance } = await WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: wasi });
  wasm = instance.exports;
  if (typeof wasm._initialize === 'function') wasm._initialize();
  refreshViews();
}

self.onmessage = async (ev) => {
  const { id, cmd, payload } = ev.data;
  try {
    await initWasm();

    if (cmd === 'init') {
      const pQ = writeCStr('');
      const classes = readResultJson(wasm.wasm_list_classes(pQ, 5000));
      wasm.wasm_free(pQ);
      self.postMessage({ id, ok: true, data: { classes } });
      return;
    }

    if (cmd === 'setOptions') {
      wasm.wasm_set_options(
        payload.cfg ? 1 : 0,
        payload.lambdas ? 1 : 0,
        payload.generics ? 1 : 0,
        payload.pc ? 1 : 0
      );
      self.postMessage({ id, ok: true, data: {} });
      return;
    }

    if (cmd === 'loadJarBytes') {
      const u8 = new Uint8Array(payload.buffer);
      const bufPtr = wasm.wasm_alloc(u8.byteLength);
      refreshViews();
      memU8.set(u8, bufPtr);
      const lblPtr = writeCStr(payload.name || 'client.jar');
      const res = readResultJson(wasm.wasm_load_jar(bufPtr, u8.byteLength, lblPtr, payload.isBase ? 1 : 0));
      wasm.wasm_free(lblPtr);
      wasm.wasm_free(bufPtr);

      const pQ = writeCStr('');
      const classes = readResultJson(wasm.wasm_list_classes(pQ, 20000));
      wasm.wasm_free(pQ);
      self.postMessage({ id, ok: true, data: { info: res, classes } });
      return;
    }

    if (cmd === 'inspect') {
      const cPtr = writeCStr(payload.cls || 'DetectedVersion');
      const data = readResultJson(wasm.wasm_inspect_class(cPtr));
      wasm.wasm_free(cPtr);
      self.postMessage({ id, ok: true, data });
      return;
    }

    if (cmd === 'diffSummary') {
      const data = readResultJson(wasm.wasm_diff_summary(payload.limit || 2500));
      self.postMessage({ id, ok: true, data });
      return;
    }

    if (cmd === 'diffClass') {
      const cPtr = writeCStr(payload.cls || 'net/minecraft/DetectedVersion');
      const data = readResultJson(wasm.wasm_diff_class(cPtr));
      wasm.wasm_free(cPtr);
      self.postMessage({ id, ok: true, data });
      return;
    }
  } catch (err) {
    self.postMessage({ id, ok: false, error: String(err && err.message ? err.message : err) });
  }
};
