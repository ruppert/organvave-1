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
  connect, readPatch, sendPatch, get patch() { return patch; }, set patch(v) { patch = v; }, OP_TEMP, OP_XTALK,
  OP_PERC, OP_PERC_SOFT, OP_PERC_FAST, OP_PERC_SEL, OP_VIB, OP_ROT, OP_ROT_MODEL, OP_ROT_SPEED, OP_ROT_HORN, OP_ROT_MIC,
  OP_ROT_BAL, OP_DRV, OP_DRV_GAIN, OP_EQ, OP_EQ_LOW, OP_EQ_MID, OP_EQ_FREQ, OP_EQ_HIGH,
  OP_DRV_TYPE, OP_DRV_TONE, OP_DRV_OUT, OP_ROT_HSLOW, OP_ROT_HFAST, OP_ROT_DSLOW, OP_ROT_DFAST, OP_ROT_HRAMP, OP_ROT_DRAMP, ADVC
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
ok(P.patch && P.patch.length === 96, "connect: a 96-byte organ patch is read");
const live = await direct(85, [0, 0]);
ok(bytesEq(P.patch, live.slice(3, 3 + 96)), "connect: the patch equals the firmware's own (ORG_GET target 0)");

// ---- 2: a factory patch copies here (target 2, index 3)
byId("source").value = "2:3";
await P.readPatch();
await wait(120);
const fac = await direct(85, [2, 3]);
ok(bytesEq(P.patch, fac.slice(3, 3 + 96)), "factory 4 (2:3) reads the firmware's factory patch");

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
// ---- 3b: the percussion and the vibrato (0.2.0): the bytes 29..33 go to the instrument
byId("perc").checked = true;
byId("psel").checked = true;
byId("vib").value = "5";
await P.sendPatch();
await wait(120);
const after2 = await direct(85, [0, 0]);
const got2 = after2.slice(3, 3 + 64);
ok(got2[P.OP_PERC] === 1 && got2[P.OP_PERC_SOFT] === 0 && got2[P.OP_PERC_FAST] === 0 && got2[P.OP_PERC_SEL] === 1 && got2[P.OP_VIB] === 5,
   "send: the percussion (on, 2 2/3') and the vibrato (CHO 2) reach the instrument");
ok(bytesEq(got.slice(1, 27), P.patch.slice(1, 27)), "send: the other drawbars are the same as sent");

// ---- 3c: the rotary and the drive (0.3.0): the bytes 34..42 go to the instrument, and the page shows what it reads back
byId("rot").checked = true;
byId("rmodel").value = "1";
byId("rspeed").value = "2";
byId("rhorn").value = "1";
byId("rmic").value = "3";
byId("rbal").value = "6";
byId("drv").checked = true;
byId("dgain").value = "9";
byId("dtype").value = "1";
byId("dtone").value = "6";                   // index 6 (+2, bright): the byte (6 + 5) % 9 = 2
byId("dout").value = "2";                    // index 2 (-4 dB): the byte 7
byId("rhs").value = "6";                     // the horn's slow speed, index 6: the byte 2
byId("rdf").value = "0";                     // the drum's fast speed, index 0: the byte 5
byId("rhr").value = "8";                     // the horn's ramp, index 8: the byte 4
byId("rdr").value = "4";                     // the drum's ramp, index 4 (the default): the byte 0
await P.sendPatch();
await wait(120);
const got3 = (await direct(85, [0, 0])).slice(3, 3 + 64);
ok(got3[P.OP_ROT] === 1 && got3[P.OP_ROT_MODEL] === 1 && got3[P.OP_ROT_SPEED] === 2 && got3[P.OP_ROT_HORN] === 1 &&
   got3[P.OP_ROT_MIC] === 3 && got3[P.OP_ROT_BAL] === 6, "send: the rotary (on, LIGHT, FAST, sharp horn, 90 degrees, horn level 6) reaches the instrument");
ok(got3[P.OP_DRV] === 1 && got3[P.OP_DRV_GAIN] === 9 && got3[P.OP_DRV_TYPE] === 1 && got3[P.OP_DRV_TONE] === 2 && got3[P.OP_DRV_OUT] === 7,
   "send: the drive (on, gain 9, TUBE, tone +2, output -4 dB) reaches the instrument (the second frame, bytes 48 and on, too)");
ok(got3[P.OP_ROT_HSLOW] === 2 && got3[P.OP_ROT_DFAST] === 5 && got3[P.OP_ROT_HRAMP] === 4 && got3[P.OP_ROT_DRAMP] === 0 &&
   got3[P.OP_ROT_HFAST] === 0 && got3[P.OP_ROT_DSLOW] === 0, "send: the Leslie's speeds and ramps (horn slow 6, drum fast 0, horn ramp 8, drum ramp 4) reach the instrument");
ok(byId("rhso").textContent === "46 rpm" || byId("rhso").textContent.endsWith("rpm"), "the sliders show their speeds in rpm (" + byId("rhso").textContent + ")");
ok(got3[P.OP_PERC] === 1 && got3[P.OP_VIB] === 5, "send: the earlier bytes are kept");
byId("rot").checked = false; byId("drv").checked = false; byId("rmic").value = "0"; byId("dgain").value = "0";
byId("source").value = "0";
await P.readPatch();
await wait(120);
ok(byId("rot").checked === true && byId("rmodel").value === "1" && byId("rmic").value === "3" && byId("drv").checked === true &&
   Number(byId("dgain").value) === 9, "read: the page shows the rotary and the drive the instrument holds");
ok(byId("dtype").value === "1" && Number(byId("dtone").value) === 6 && Number(byId("dout").value) === 2 && Number(byId("rhs").value) === 6 &&
   Number(byId("rdf").value) === 0 && Number(byId("rhr").value) === 8, "read: ... and the drive's type, tone and output and the Leslie's speeds and ramps");


// ---- 3d: ADVANCED EDIT (0.9.4): the bytes 57..81 go to the instrument (the second frame carries bytes 48..81), and read back
const advc = (op) => P.ADVC.find((c) => c.op === op);
ok(P.ADVC.length === 25 && new Set(P.ADVC.map((c) => c.op)).size === 25 && P.ADVC.every((c) => c.op >= 57 && c.op <= 81), "advanced: 25 sliders, one for each of the bytes 57..81");
ok(P.ADVC.every((c) => Number(c.inp.value) === c.d), "advanced: a patch of zeros shows every default (the middle of the step)");
advc(57).inp.value = 64 + 20;                  // 450 Hz
advc(59).inp.value = 32;                       // attack click 80 %
advc(62).inp.value = 0;                        // release model CLICK
advc(63).inp.value = 24;                       // release click 60 %
advc(67).inp.value = 41;                       // wiring -20 dB
advc(77).inp.value = 0;                        // no muted drawbar
advc(81).inp.value = 2;
await P.sendPatch();
await wait(150);
const got4 = (await direct(85, [0, 0])).slice(3, 3 + 96);
ok(got4[57] === 20 && got4[59] === 12 && got4[62] === 2 && got4[63] === 14 && got4[67] === 10 && got4[77] === 1 && got4[81] === (2 - 18 + 25) % 25,
   "advanced: the bytes reach the instrument as (step - default) mod n (tuning 450 Hz, attack click 80 %, release CLICK, wiring -20 dB, no mute)");
ok(got4[58] === 0 && got4[64] === 0 && got4[70] === 0 && got4[82] === 0 && got4[95] === 0, "advanced: the untouched ones stay 0 (the defaults), the reserved bytes too");
ok(got4[P.OP_PERC] === 1 && got4[P.OP_VIB] === 5 && got4[P.OP_ROT] === 1, "advanced: the earlier bytes are kept");
for (const c of P.ADVC) c.inp.value = c.d;
byId("source").value = "0";
await P.readPatch();
await wait(120);
ok(Number(advc(57).inp.value) === 84 && Number(advc(59).inp.value) === 32 && Number(advc(62).inp.value) === 0 && Number(advc(81).inp.value) === 2 &&
   Number(advc(64).inp.value) === 31, "advanced: read shows the steps the instrument holds");
ok(advc(57).out.textContent === "450.0 Hz" && advc(59).out.textContent === "80 %" && advc(67).out.textContent === "-20 dB" && advc(77).out.textContent === "none",
   "advanced: the texts (" + ["57", "59", "67", "77"].map((o) => advc(Number(o)).out.textContent).join(", ") + ")");

// ---- 4: a refusal shows the device's reason: factory 99 does not exist
byId("source").value = "2:99";
await P.readPatch();
await wait(120);
ok(status().startsWith("Read failed") && status().includes("refused"), `read: a bad index is refused by the device ("${status()}")`);

bridge.stdin.end();
console.log(failed ? `${failed} FAILED` : "all ok");
process.exit(failed ? 1 : 0);
