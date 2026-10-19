#!/usr/bin/env bun
// First render and subsequent screen-update latency. Start rayneo-io bridge,
// then run `bun scene-timing.ts`; --dry-run only checks/prints the payloads.
import { CacheLink, CfwTransport, RayNeoBridge, record as wireRecord } from "./rayneo-transport";
import { inlineText, putObject, record, show, T, REPLY } from "./object-cache";

function screen(version: number, lines: number, width = 540): Uint8Array {
  const puts = [putObject(1, version, record(T.RECT, 15, 2, 2, 2, width - 4, 216, 8))];
  const refs = [{ id: 1, version }];
  for (let i = 0; i < lines; i++) {
    const id = i + 2;
    puts.push(putObject(id, version, inlineText(31, 12, 8 + i * 25, width - 24, 25,
      `Update ${version}, row ${i + 1}: the quick brown fox jumps`)));
    refs.push({ id, version });
  }
  return show({ request: version, puts, refs });
}
if (process.argv.includes("--dry-run")) {
  for (const lines of [0, 8]) {
    const m = screen(1, lines), w = wireRecord(m);
    console.log(`${lines} text rows: ${m.length} message bytes, ${w.length} wire bytes, ${Math.ceil(w.length / 240)} packets`);
  }
  process.exit(0);
}
const bridge = await RayNeoBridge.connect(Number(process.env.G2_PORT ?? 7310));
const ident = await bridge.query();
if (!ident || ident.revision < 5) throw new Error("requires RayNeo CFW revision 5+");
console.log(`${ident.name}: ${ident.width}x${ident.height}`);
const link = new CacheLink(new CfwTransport(bridge, 15_000), 3, ident.maxChunk);
const u16 = (p: Uint8Array, i: number) => p[i]! | p[i + 1]! << 8;
const u32 = (p: Uint8Array, i: number) => (u16(p, i) | u16(p, i + 2) << 16) >>> 0;
const unsubscribe = bridge.onReply((p) => {
  if (p[0] !== 0x15 || p[1] !== 2 || p.length < 51) return;
  console.log(`  firmware totals: ${u32(p, 39)} renders, ${u32(p, 43)} other-eye copies; last/max render ${u16(p, 47)}/${u16(p, 49)} ms`);
});
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
let opened = false;
try {
  const start = performance.now(), page = await bridge.openPage();
  if (!page.open) throw new Error(`page refused: ${page.reason}`);
  opened = true;
  console.log(`PAGE_OPEN round trip: ${(performance.now() - start).toFixed(0)} ms`);
  const resetAt = performance.now(); await link.reset();
  console.log(`RESET round trip: ${(performance.now() - resetAt).toFixed(0)} ms`);
  let version = 1;
  for (const lines of [0, 8]) {
    const times: number[] = [];
    for (let sample = 0; sample < 6; sample++) {
      const t = performance.now();
      const replies = await link.send(screen(version++, lines, ident.width));
      for (const [lens, reply] of replies) if (reply.status !== REPLY.APPLIED) throw new Error(`SHOW lens ${lens}: ${reply.status}`);
      times.push(performance.now() - t);
      await sleep(250);
    }
    const updates = times.slice(1).sort((a, b) => a - b);
    console.log(`${lines} text rows: first SHOW ${times[0]!.toFixed(0)} ms, updates median ${updates[2]!.toFixed(0)} ms, max ${updates.at(-1)!.toFixed(0)} ms (through firmware ACK; not optical presentation)`);
    await bridge.query(); await sleep(300);
  }
} finally {
  unsubscribe();
  if (opened) { bridge.control("close"); await sleep(200); }
  bridge.close();
}
