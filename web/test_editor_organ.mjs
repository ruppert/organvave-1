// SPDX-License-Identifier: GPL-3.0-only
// The organ patch editor (web/editor_organ.html) against the firmware's real editor handler (tests/editor_bridge.c):
// the page's script runs in a VM with a stub page and a fake MIDI port; every SysEx it sends goes to the firmware.
//   node web/test_editor_organ.mjs build/host/editor_bridge
import { readFileSync } from "node:fs";
import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import vm from "node:vm";

const bridgePath = process.argv[2] || "build/host/editor_bridge";
const html = readFileSync(new URL("./editor_organ.html", import.meta.url), "utf8");
const script = html.slice(html.lastIndexOf("<script>") + 8, html.lastIndexOf("</script>"));

let failed = 0;
const ok = (cond, what) => { console.log(`${what.padEnd(70)} ${cond ? "ok" : "FAIL"}`); if (!cond) failed++; };

// ---- the firmware: one request per line in (hex with spaces), one reply line out (hex; empty when none)
const bridge = spawn(bridgePath, { stdio: ["pipe", "pipe", "inherit"] });
const replies = [];
let waiting = null;
createInterface({ input: bridge.stdout }).on("line", (line) => {
  const bytes = (line.match(/../g) || []).map((h) => parseInt(h, 16));
  if (waiting) { const w = waiting; waiting = null; w(bytes); }
});
const ask = (bytes) => new Promise((resolve) => {        // the firmware's answer to one frame
  waiting = resolve;
  bridge.stdin.write(bytes.map((b) => b.toString(16).padStart(2, "0")).join(" ") + "\n");
});

// ---- a stub page: every element the script reads exists, and is a plain object
class El {
  constructor(tag = "div") { this.tag = tag; this.children = []; this.value = ""; this.checked = false; this.hidden = false;
    this.disabled = false; this.textContent = ""; this.className = ""; this.dataset = {}; this.style = {}; this.selectedOptions = [{ textContent: "" }];
    this.onclick = null; this.onchange = null; this.oninput = null; this.onmidimessage = null; this.labelText = ""; }
  set innerHTML(v) { this.children = []; }
  append(...c) { this.children.push(...c); }
  appendChild(c) { this.children.push(c); return c; }
  get options() { return this.children; }
  addEventListener() {}
  setAttribute(k, v) { this[k] = v; }
}
const els = new Map();
const byId = (id) => { if (!els.has(id)) els.set(id, new El()); return els.get(id); };
// the page's controls start as the page has them
byId("source").value = "0";
byId("temp").value = "0";

// ---- the fake MIDI port: sending a frame sends it to the firmware, its reply comes back as an incoming message
const input = { name: "FM-1 MIDI", state: "connected", onmidimessage: null, open: async () => {} };
const output = {
  name: "FM-1 MIDI", state: "connected", opened: 0,
  open: async () => {},
  send(bytes) {
    ask([...bytes]).then((reply) => { if (reply.length && input.onmidimessage) input.onmidimessage({ data: Uint8Array.from(reply) }); });
  },
};
const access = { inputs: new Map([["in", input]]), outputs: new Map([["out", output]]) };

const ctx = {
  console, setTimeout, clearTimeout,
  document: { getElementById: byId, createElement: (tag) => new El(tag) },
  navigator: { requestMIDIAccess: async () => access },
  Uint8Array,
};
vm.createContext(ctx);
// export the page's functions and state to the test
vm.runInContext(script + `
;globalThis.__page = {
  connect, readPatch, sendPatch, get patch() { return patch; }, set patch(v) { patch = v; }, OP_TEMP, OP_XTALK
};`, ctx);
const P = ctx.__page;
const status = () => byId("status").textContent;
const wait = (ms = 50) => new Promise((r) => setTimeout(r, ms));
const bytesEq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

// the same frames the page sends, asked of the firmware directly (the answer the editor must match)
const direct = async (cmd, args) => (await ask([0xF0, 0x7D, 0x46, 0x4C, cmd, ...args, 0xF7])).slice(5, -1);

// ---- 1: connect: the INFO tag, then the instrument's patch
await P.connect();
await wait(120);
ok(status().startsWith("Read ") && !byId("status").className.includes("bad"), `connect: the FM-1 answers and its patch is read ("${status()}")`);
ok(byId("dev").textContent === "FM-1 MIDI", "connect: the port's name shows");
ok(P.patch && P.patch.length === 64, "connect: a 64-byte organ patch is read");
const live = await direct(85, [0, 0]);
ok(bytesEq(P.patch, live.slice(3, 3 + 64)), "connect: the patch equals the firmware's own (ORG_GET target 0)");

// ---- 2: a factory patch copies here (target 2, index 3)
byId("source").value = "2:3";
await P.readPatch();
await wait(120);
const fac = await direct(85, [2, 3]);
ok(bytesEq(P.patch, fac.slice(3, 3 + 64)), "factory 4 (2:3) reads the firmware's factory patch");

// ---- 3: edits go to the instrument: drawbar 1 (16') to 3, the tuning to 50 Hz, the crosstalk on
P.patch[0] = 3;
byId("temp").value = "1";
byId("xtalk").checked = true;
await P.sendPatch();
await wait(120);
ok(status().startsWith("Sent"), `send: the firmware takes the patch ("${status()}")`);
const after = await direct(85, [0, 0]);
const got = after.slice(3, 3 + 64);
ok(got[0] === 3 && got[P.OP_TEMP] === 1 && got[P.OP_XTALK] === 1, "send: the instrument's patch holds the drawbar, tuning and crosstalk");
ok(bytesEq(got.slice(1, 27), P.patch.slice(1, 27)), "send: the other drawbars are the same as sent");

// ---- 4: a refusal shows the device's reason: factory 9 does not exist
byId("source").value = "2:9";
await P.readPatch();
await wait(120);
ok(status().startsWith("Read failed") && status().includes("refused"), `read: a bad index is refused by the device ("${status()}")`);

bridge.stdin.end();
console.log(failed ? `${failed} FAILED` : "all ok");
process.exit(failed ? 1 : 0);
