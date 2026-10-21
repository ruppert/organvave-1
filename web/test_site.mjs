// SPDX-License-Identifier: GPL-3.0-only
// The built site (web/make_site.py's index.html): every marker filled, both scripts compile and run to the first status
// without Web MIDI, the embedded package is the FM-1 package of the version. Node only (no browser, no hardware).
//   python3 web/make_site.py build/felucca.fwsc SITE 0.1.0 && node web/test_site.mjs SITE/index.html
import { readFileSync } from "node:fs";
import vm from "node:vm";

const file = process.argv[2] || "SITE/index.html";
const html = readFileSync(file, "utf8");
let failed = 0;
const ok = (cond, what) => { console.log(`${what.padEnd(70)} ${cond ? "ok" : "FAIL"}`); if (!cond) failed++; };

ok(!/\/\*(LIB|META|VERSION)\*\//.test(html) && !html.includes("/*META*/{}"), "every marker is filled");
const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
ok(scripts.length === 2, "two scripts: the installer library, the page");
for (const [i, s] of scripts.entries()) {
  let compiles = true;
  try { new vm.Script(s); } catch (e) { compiles = false; console.log(e.message); }
  ok(compiles, `script ${i + 1} compiles`);
}

// the page's meta: the package it carries, its identity and the file name of the download
const m = scripts[1].match(/const meta = (\{.*\});/);
const meta = m ? JSON.parse(m[1]) : null;
ok(meta && /^FM-1_8\d\d$/.test(meta.product) && /^organvave-1-\d+\.\d+\.\d+\.fwsc$/.test(meta.file), `meta: ${meta && meta.product}, ${meta && meta.file}`);
const b64 = meta ? meta.pkg.split(",")[1] : "";
const pkg = Buffer.from(b64, "base64");
ok(pkg.includes(Buffer.from("FELUCCA-LOADER-1")), `the embedded package (${pkg.length} B) is a Felucca loader package`);

// the page runs without Web MIDI: the first status says so, and nothing throws
const status = { textContent: "", className: "", style: {} };
const el = () => ({ textContent: "", value: "", style: {}, className: "", dataset: {}, children: [], hidden: false, disabled: false,
  setAttribute() {}, appendChild() {}, append() {}, addEventListener() {}, showModal: null, close() {}, classList: { add() {} } });
const elements = new Map();
const document = { getElementById: (id) => { if (id === "status") return status; if (!elements.has(id)) elements.set(id, el()); return elements.get(id); },
  querySelector: () => el(), createElement: () => el(), body: { className: "" } };
const ctx = { document, navigator: { language: "en-US" }, console, setTimeout, clearTimeout, fetch: undefined, TextEncoder, Uint8Array, Date, window: {} };
vm.createContext(ctx);
let threw = null;
try { vm.runInContext(scripts[0] + "\n" + scripts[1], ctx); await new Promise((r) => setTimeout(r, 30)); }
catch (e) { threw = e; console.log(String(e && e.stack || e).split("\n").slice(0, 3).join("\n")); }
ok(!threw, "the page's scripts run without Web MIDI");
ok(status.textContent.includes("Web MIDI") || status.textContent.length > 0, `the first status shows (${status.textContent.slice(0, 60)})`);

console.log(failed ? `${failed} FAILED` : "site ok");
process.exit(failed ? 1 : 0);
