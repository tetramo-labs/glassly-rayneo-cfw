#!/usr/bin/env bun
// The g2flash demos' vector-suite.ts (revision 38 SVG paths and rotation on
// the object cache) on a RayNeo iO through the Mac harness's bridge socket.
// The steps are vector-cases.ts verbatim (the G2 fixtures, 576-wide
// coordinates; the RayNeo canvas is 540×220, so the lower rows of a few
// fixtures fall off the panel). The Bad Apple video playback of the original
// is not ported. Every step must be APPLIED by every lens; the pixel probes
// are host-side (tools/host/run_vector_tests.sh checks the firmware's own
// rasterizer against them).
//
//     rayneo-io bridge                        # first, in another terminal
//     bun vector-suite.ts                     # every step, 700 ms apart
//     bun vector-suite.ts --list
//     bun vector-suite.ts --dry-run           # encode only
//     bun vector-suite.ts --dump FILE         # the g2flash dump format
//     G2_PORT=7310 bun vector-suite.ts
//
// Needs the glassly-rayneo-cfw firmware, revision 6 or later.
import { vectorCases, type VectorStep } from "./vector-cases";
import { CacheLink, CfwTransport, RayNeoBridge } from "./rayneo-transport";
import { u32 as i32, u16 } from "./vector-protocol";
import { describeReply, REPLY } from "./object-cache";

const REQUIRED_REVISION = 6;
const args = process.argv.slice(2);
const option = (name: string) => { const i = args.indexOf(name); if (i < 0) return undefined; const v = args[i + 1]; if (!v || v.startsWith("--")) throw new Error(`Missing value for ${name}`); return v; };
if (args.includes("--help")) {
  console.log(`SVG paths and rotation on the RayNeo iO object cache (vector-cases.ts)
  bun vector-suite.ts [--list] [--dry-run] [--dump FILE]`);
  process.exit(0);
}
const steps = vectorCases();
if (args.includes("--list")) { console.log(steps.map((s) => s.name).join("\n")); process.exit(0); }

const dump = option("--dump");
if (dump) {
  const bytes: number[] = [];
  for (const step of steps) {
    const label = [...new TextEncoder().encode(step.name)];
    if (label.length > 255 || step.payload.length > 65535) throw new Error("Dump record exceeds format limit");
    bytes.push(3, label.length, ...label, 1, ...u16(step.payload.length), ...step.payload, 2, ...i32(Math.round(step.waitMs)));
    for (const [x, y, color] of step.probes ?? []) bytes.push(4, ...u16(x), ...u16(y), color);
    if (step.snapshot) bytes.push(5);
  }
  await Bun.write(dump, Uint8Array.from(bytes)); console.log(`wrote ${dump}: ${steps.length} steps`);
}
if (args.includes("--dry-run") || dump) {
  console.log(`offline: ${steps.length} steps, ${steps.reduce((n, s) => n + s.payload.length, 0)} encoded bytes; nothing sent`);
  process.exit(0);
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const bridge = await RayNeoBridge.connect(Number(process.env.G2_PORT ?? 7310));
let ident = bridge.hello && bridge.hello.revision > 0 ? bridge.hello : null;
if (!ident) ident = await bridge.query().catch(() => null);
if (!ident || ident.revision < REQUIRED_REVISION) {
  console.log(`needs the Glassly RayNeo iO CFW, revision ${REQUIRED_REVISION} or later (got ${ident ? `${ident.name} revision ${ident.revision}` : "no IDENT"})`);
  bridge.close();
  process.exit(1);
}
console.log(`CFW detected: ${ident.name} revision ${ident.revision}, ${ident.width}×${ident.height}`);
let page = await bridge.openPage();
for (let attempt = 0; !page.open && page.reason === 1 && attempt < 4; attempt++) { await sleep(2000); page = await bridge.openPage(); }
if (!page.open) throw new Error(`PAGE_OPEN refused, reason ${page.reason}`);
const renew = setInterval(() => bridge.control("keepalive"), 30_000);
const transport = new CfwTransport(bridge, 8000);
const link = new CacheLink(transport, 3, ident.maxChunk);
let failed = 0, sent = 0, worst = 0;
try {
  await link.reset();   /* one session epoch for both lenses; the fixtures' own reset step is skipped */
  console.log(`reset: epoch ${link.epoch}, store ${link.storeKiB} KiB`);
  for (const step of steps as VectorStep[]) {
    if (step.name === "reset") continue;
    const before = performance.now();
    let replies;
    try {
      replies = await link.send(step.payload, { allow: Object.values(REPLY) });
    } catch (e) {
      console.log(`FAIL ${step.name}: ${(e as Error).message}`);
      failed++;
      continue;
    }
    const ms = performance.now() - before;
    sent++; worst = Math.max(worst, ms);
    const bad = [...replies].filter(([, r]) => r.status !== REPLY.APPLIED);
    if (bad.length) failed++;
    const detail = bad.length ? bad.map(([lens, r]) => `${lens === 1 ? "L" : "R"} ${describeReply(r)}`).join(", ") : "applied";
    console.log(`${bad.length ? "FAIL" : "ok  "} ${step.name.padEnd(36)} ${String(step.payload.length).padStart(5)} B ${String(ms.toFixed(0)).padStart(5)} ms  ${detail}`);
    if (step.name !== "release" && step.waitMs > 0) await sleep(step.waitMs);
  }
} finally {
  clearInterval(renew);
  transport.close();
  bridge.control("close");
  await sleep(300);
  bridge.close();
}
console.log(`\n${sent - failed} applied, ${failed} failed; worst ACK ${worst.toFixed(0)} ms`);
process.exit(failed ? 1 : 0);
