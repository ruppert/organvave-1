// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guilherme Ruppert (OrganVave-1)
// The SMK-37 installer page (web/make_smk_site.py's smk.html): every marker filled, both scripts compile, the three packages
// beside it carry their models' identities; then the page itself runs (node, with a fake fetch, Web MIDI and Updater):
// Install waits for the backup box, the keyboard's model picks its own package (SMK-37 Pro, Elite, MKE-P37, also when
// it is already in update mode: resume), an FM-1 or an unknown model gets nothing.
//   python3 web/make_smk_site.py SITE 0.9.45 PRO ELITE MKE && node web/test_smk_site.mjs SITE/smk.html
import { readFileSync, existsSync } from "node:fs";
import { dirname, join } from "node:path";
import vm from "node:vm";

const file = process.argv[2] || "SITE/smk.html";
const dir = dirname(file);
const html = readFileSync(file, "utf8");
let failed = 0;
const ok = (cond, what) => { console.log(`${what.padEnd(96)} ${cond ? "ok" : "FAIL"}`); if (!cond) failed++; };

ok(!/\/\*(LIB|META|VERSION)\*\//.test(html) && !html.includes("/*META*/{}"), "every marker is filled");
const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
ok(scripts.length === 2, "two scripts: the installer library, the page");
for (const [i, s] of scripts.entries()) {
  let compiles = true;
  try { new vm.Script(s); } catch (e) { compiles = false; console.log(e.message); }
  ok(compiles, `script ${i + 1} compiles`);
}
const m = scripts[1].match(/const meta = (\{.*\});/);
const meta = m ? JSON.parse(m[1]) : null;
const want = [["SMK-37 Pro", ""], ["SMK-37 Elite", "-elite"], ["MKE-P37", "-mke"]];
ok(meta && meta.pkgs.length === 3 && want.every(([mo, suf], i) => meta.pkgs[i].model === mo &&
   new RegExp("^" + mo + "_8\\d\\d$").test(meta.pkgs[i].product) && meta.pkgs[i].file === `smk/organvave-1-smk37${suf}-v${meta.version}.fwsc`),
   `meta: ${meta && meta.pkgs.map((p) => p.product).join(", ")}`);
ok(meta && meta.pkgs.every((p) => existsSync(join(dir, p.file)) && readFileSync(join(dir, p.file)).includes(Buffer.from("FELUCCA-LOADER-1"))),
   "the three packages beside the page, each a Felucca-loader package");
ok(existsSync(join(dir, "smk/index.html")) && readFileSync(join(dir, "smk/index.html"), "utf8").includes("../smk.html"), "smk/ leads to smk.html");

// the page, run
const el = () => {
  const e = { textContent: "", value: "", style: {}, className: "", dataset: {}, children: [], hidden: false, disabled: false, checked: false,
    href: "", download: "", h: {}, setAttribute() {}, append() {}, close() {}, showModal: null, classList: { add() {} } };
  e.appendChild = (c) => { e.children.push(c); };
  e.addEventListener = (ev, fn) => { e.h[ev] = fn; };
  return e;
};
const elements = new Map();
const get = (id) => { if (!elements.has(id)) elements.set(id, el()); return elements.get(id); };
const document = { getElementById: get, querySelector: () => el(), createElement: () => el(), body: { className: "" } };
const fetch = async (p) => ({ ok: true, arrayBuffer: async () => { const b = readFileSync(join(dir, p)); return b.buffer.slice(b.byteOffset, b.byteOffset + b.length); } });
const calls = [];
let device = null;                                   // the identity the fake keyboard answers with
const ctx = { document, navigator: { language: "en-US", requestMIDIAccess: async () => ({ inputs: new Map(), outputs: new Map() }) },
  console, setTimeout, clearTimeout, fetch, TextEncoder, Uint8Array, Date, window: {}, Object, Error, calls, getDevice: () => device };
vm.createContext(ctx);
let threw = null;
try {
  vm.runInContext(scripts[0], ctx);
  vm.runInContext(`Updater = class {                // (the fake: what the page asks of the update protocol)
    constructor() { this.portName = null; }
    async find() { const t = getDevice(); if (!t) return null; const [mo] = t.split("_"); return { id: { text: t, model: mo }, link: { close() {} } }; }
    async install(image, product, step) { calls.push(["install", product, image.length]); step("done", product); }
    async resume(image, step, product) { calls.push(["resume", product, image.length]); return true; }
  };`, ctx);
  vm.runInContext(scripts[1], ctx);
  await new Promise((r) => setTimeout(r, 100));
} catch (e) { threw = e; console.log(String(e && e.stack || e).split("\n").slice(0, 3).join("\n")); }
ok(!threw, "the page's scripts run");
ok(get("files").children.length === 3, "the three downloads listed");
ok(get("go").disabled && get("status").textContent.startsWith("Ready"), `loaded, Install held until the backup box (${get("status").textContent.slice(0, 40)})`);
get("backup").checked = true;
get("backup").h.change();
ok(!get("go").disabled, "the backup box ticked: Install enabled");

const run = async (t) => { device = t; calls.length = 0; await vm.runInContext("runInstall(null)", ctx); return calls.slice(); };
let c = await run("SMK-37 Elite_016");
ok(c.length === 1 && c[0][0] === "install" && /^SMK-37 Elite_8/.test(c[0][1]), `an SMK-37 Elite (stock): the Elite package (${c[0] && c[0][1]})`);
c = await run("SMK-37 Pro_800");
ok(c.length === 1 && c[0][0] === "install" && /^SMK-37 Pro_8/.test(c[0][1]), `an SMK-37 Pro running OrganVave-1: the Pro package (${c[0] && c[0][1]})`);
c = await run("ota-MKE-P37_900");
ok(c.length === 1 && c[0][0] === "resume" && /^MKE-P37_8/.test(c[0][1]), `an MKE-P37 in update mode: resume with the MKE package (${c[0] && c[0][1]})`);
c = await run("FM-1_015");
ok(c.length === 0 && get("status").textContent.includes("not an SMK-37"), `an FM-1: nothing installed (${get("status").textContent.slice(0, 50)})`);
c = await run("ota-FM-1_900");
ok(c.length === 0, "an FM-1 in update mode: nothing written");
c = await run(null);
ok(c.length === 0 && get("status").textContent.startsWith("No keyboard"), "no keyboard: nothing, and it says so");
get("backup").checked = false;
get("backup").h.change();
c = await run("SMK-37 Pro_016");
ok(c.length === 0 && get("go").disabled, "the backup box cleared: no install");

console.log(failed ? `${failed} FAILED` : "smk site ok");
process.exit(failed ? 1 : 0);
