// DeciDB in a Web Worker: downloads the engine (decidb.wasm.gz), unpacks it, opens an in-memory database, and runs
// the page's queries one at a time, so a long solve never blocks the page. Built by context/website_src/wasm/build.sh.
import createDecidb from "./decidb.mjs";

let query = null;
const notes = [];

async function load() {
  const t0 = performance.now();
  const res = await fetch(new URL("decidb.wasm.gz", import.meta.url));
  if (!res.ok || !res.body) throw new Error(`could not download the engine (HTTP ${res.status})`);
  const total = Number(res.headers.get("content-length")) || 0;
  const reader = res.body.getReader();
  const parts = [];
  let loaded = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    parts.push(value);
    loaded += value.length;
    postMessage({ type: "progress", loaded, total });
  }
  // the bytes are gzip unless the server already unpacked them (a host that sends Content-Encoding: gzip)
  let blob = new Blob(parts);
  const head = new Uint8Array(await blob.slice(0, 2).arrayBuffer());
  if (head[0] === 0x1f && head[1] === 0x8b) {
    blob = await new Response(blob.stream().pipeThrough(new DecompressionStream("gzip"))).blob();
  }
  const bytes = await blob.arrayBuffer();
  const mod = await createDecidb({
    instantiateWasm(imports, done) {
      WebAssembly.instantiate(bytes, imports).then((r) => done(r.instance, r.module));
      return {};
    },
    print: (line) => notes.push(line),
    printErr: (line) => notes.push(line),
  });
  const opened = JSON.parse(mod.UTF8ToString(mod._decidb_open()));
  if (!opened.ok) throw new Error(opened.error);
  query = (sql) => JSON.parse(mod.UTF8ToString(mod.ccall("decidb_query", "number", ["string"], [sql])));
  return { version: opened.version, ms: Math.round(performance.now() - t0), bytes: loaded };
}

const ready = load().then(
  (info) => postMessage({ type: "ready", ...info }),
  (err) => postMessage({ type: "failed", error: String(err && err.message ? err.message : err) })
);

onmessage = async (e) => {
  const { id, sql } = e.data;
  await ready;
  if (!query) return postMessage({ type: "result", id, ok: false, error: "DeciDB did not start." });
  notes.length = 0;
  const t = performance.now();
  let r;
  try {
    r = query(sql);
  } catch (err) {
    r = { ok: false, error: String(err && err.message ? err.message : err) };
  }
  postMessage({ type: "result", id, ms: performance.now() - t, notes: notes.slice(), ...r });
};
