#!/usr/bin/env bun
// Time the firmware's animation clock on a RayNeo iO: a SHOW with a tagged
// glide of N frames should settle N x 33 ms after it lands. Runs the glide
// over a light scene (one disc) and a dashboard-like scene (border, eight
// text lines, a few shapes) so a render-bound frame loop shows up as a settle
// time well past the nominal duration. Prints one line per run.
//
//     rayneo-io bridge                        # in another terminal
//     bun anim-timing.ts
import { CacheLink, CfwTransport, RayNeoBridge } from "./rayneo-transport";
import { control, EASE, hide, inlineText, ops, putObject, record, show, T, REPLY } from "./object-cache";

const RPL_SETTLED = 0x09;
const FRAME_MS = 33;
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

const bridge = await RayNeoBridge.connect(Number(process.env.G2_PORT ?? 7310));
let ident = bridge.hello && bridge.hello.revision > 0 ? bridge.hello : null;
if (!ident) ident = await bridge.query().catch(() => null);
if (!ident || ident.revision < 5) {
  console.log(`needs the Glassly RayNeo iO CFW revision 5+ (got ${ident ? `${ident.name} revision ${ident.revision}` : "no IDENT"})`);
  bridge.close();
  process.exit(1);
}
console.log(`CFW detected: ${ident.name} revision ${ident.revision}, canvas ${ident.width}x${ident.height}`);
let page = await bridge.openPage();
for (let attempt = 0; !page.open && page.reason === 1 && attempt < 4; attempt++) { await sleep(2000); page = await bridge.openPage(); }
if (!page.open) throw new Error(`PAGE_OPEN refused, reason ${page.reason}`);
const renew = setInterval(() => bridge.control("keepalive"), 30_000);
const transport = new CfwTransport(bridge, 15_000);
const link = new CacheLink(transport, 3, ident.maxChunk);

/** Settled replies as they arrive: tag, arrival ms and (revision 8) the firmware's own statistics. */
const settled: { tag: number; at: number; stats?: string }[] = [];
const u16 = (p: Uint8Array, o: number) => p[o]! | (p[o + 1]! << 8);
const offReply = bridge.onReply((p) => {
  if (p[0] !== RPL_SETTLED || p.length < 3) return;
  let stats = p.length >= 17
    ? `fw: ${u16(p, 3)} ms, ${u16(p, 5)} ticks, ${u16(p, 7)} frames, ${u16(p, 9)} renders, ${u16(p, 11)} pans, ${u16(p, 13)} presented, render max ${p[15]} ms, tick gap max ${p[16]} ms`
    : undefined;
  if (stats && p.length >= 22) stats += `, gate 0x${p[17]!.toString(16)}, compose max ${p[18]} ms, update max ${p[19]} ms, skipped ${u16(p, 20)}`;
  settled.push({ tag: u16(p, 1), at: performance.now(), stats });
});

const W = ident.width, H = ident.height;
const BALL = 1;
const ball = (x: number) => record(T.CIRCLE_FILL, 15, 0, x, Math.floor(H / 2), 25);

/** Static objects besides the ball: none (light) or a dashboard-like set (heavy). */
function scenery(heavy: boolean): { puts: number[][]; refs: { id: number; version: number }[] } {
  if (!heavy) return { puts: [], refs: [] };
  const puts: number[][] = [];
  const refs: { id: number; version: number }[] = [];
  let id = 100;
  const add = (rec: number[]) => { puts.push(putObject(id, 1, rec)); refs.push({ id, version: 1 }); id++; };
  add(record(T.RECT, 15, 2, 2, 2, W - 4, H - 4, 8));
  for (let i = 0; i < 8; i++) add(inlineText(0x10 | 15, 12, 8 + i * 26, W - 24, 24, `Line ${i + 1}: the quick brown fox jumps over the lazy dog`));
  add(record(T.CIRCLE, 12, 2, W - 60, 40, 30));
  add(record(T.TRI_FILL, 9, 0, W - 120, H - 20, W - 80, H - 60, W - 40, H - 20));
  add(record(T.ARC, 15, 3, W - 60, H - 60, 28, 20, 300));
  return { puts, refs };
}

async function run(heavy: boolean, frames: number): Promise<string> {
  const base = scenery(heavy);
  let tag = 1000 + frames + (heavy ? 500 : 0);
  // Show the start scene, settled at once (no motion).
  let replies = await link.send(show({ request: tag, tag: true, puts: [...base.puts, putObject(BALL, 1, ball(40))], refs: [...base.refs, { id: BALL, version: 1 }] }));
  for (const [lens, r] of replies) if (r.status !== REPLY.APPLIED) throw new Error(`SHOW lens ${lens}: status ${r.status}`);
  await sleep(300);
  settled.length = 0;
  // The glide: tagged, so each lens reports SETTLED when the motion ends.
  tag += 1;
  const t0 = performance.now();
  replies = await link.send(show({ request: tag, tag: true, refs: [...base.refs, { id: BALL, version: 1 }], ops: [ops.glide(BALL, W - 130, 0, frames, EASE.inOut)] }));
  const landed = performance.now();
  for (const [lens, r] of replies) if (r.status !== REPLY.APPLIED) throw new Error(`glide SHOW lens ${lens}: status ${r.status}`);
  const nominal = frames * FRAME_MS;
  const deadline = landed + nominal * 4 + 2000;
  while (settled.filter((s) => s.tag === tag).length < 2 && performance.now() < deadline) await sleep(5);
  const mine = settled.filter((s) => s.tag === tag).map((s) => s.at - landed);
  const ack = (landed - t0).toFixed(0);
  const times = mine.length ? mine.map((t) => t.toFixed(0)).join("/") : "none";
  const worst = mine.length ? Math.max(...mine) : NaN;
  const ratio = Number.isFinite(worst) ? (worst / nominal).toFixed(2) : "-";
  const stats = settled.find((s) => s.tag === tag && s.stats)?.stats;
  await sleep(400);
  return `${heavy ? "heavy" : "light"} scene, ${frames} frames (${nominal} ms nominal): SHOW acked in ${ack} ms, SETTLED after ${times} ms (${mine.length}/2 lenses, x${ratio})${stats ? `\n    ${stats}` : ""}`;
}

/** Tuning masks to compare (revision 9 CONTROL sub 3); older firmware runs the defaults once. */
const TUNINGS: [number, string][] = ident.revision >= 9
  ? [[0, "defaults"], [0x01, "presents gated on the power flag"], [0x02, "no UI wake"], [0x04, "render every frame"]]
  : [[0, "defaults"]];
let failed = 0;
try {
  await link.reset();
  for (const [mask, label] of TUNINGS) {
    if (ident.revision >= 9) await link.send(control.tuning(mask));
    console.log(`-- tuning 0x${mask.toString(16)}: ${label}`);
    for (const [heavy, frames] of [[false, 15], [false, 60], [true, 30]] as [boolean, number][]) {
      try {
        console.log(run.name, await run(heavy, frames));
      } catch (e) {
        failed++;
        console.log(`FAIL ${heavy ? "heavy" : "light"} ${frames}: ${(e as Error).message}`);
      }
    }
  }
  if (ident.revision >= 9) await link.send(control.tuning(0));
  await link.send(hide());
} finally {
  offReply();
  clearInterval(renew);
  bridge.control("close");
  await sleep(500);
  bridge.close();
}
console.log(failed ? `${failed} run(s) failed` : "all runs reported");
process.exit(failed ? 1 : 0);
