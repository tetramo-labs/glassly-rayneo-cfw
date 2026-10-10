#!/usr/bin/env bun
// The object cache's journal on a RayNeo iO (revision 6): STATE (mode 40)
// answers UNCHANGED / DELTA / SNAPSHOT / STALE as the G2 scene.c does, DROP
// (CONTROL sub 1) frees inactive objects and refuses active ones, PERIOD
// (sub 2) is range-checked. Each check is one or two messages over the
// bridge; every lens must answer alike.
//
//     rayneo-io bridge                        # first, in another terminal
//     bun cache-state.ts
import { CacheLink, CfwTransport, RayNeoBridge } from "./rayneo-transport";
import { control, describeReply, hash31, put, putObject, record, REPLY, show, state, type Reply } from "./object-cache";

const REQUIRED_REVISION = 6;
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const bridge = await RayNeoBridge.connect(Number(process.env.G2_PORT ?? 7310));
let ident = bridge.hello && bridge.hello.revision > 0 ? bridge.hello : null;
if (!ident) ident = await bridge.query().catch(() => null);
if (!ident || ident.revision < REQUIRED_REVISION) {
  console.log(`needs the Glassly RayNeo iO CFW, revision ${REQUIRED_REVISION} or later (got ${ident ? `${ident.name} revision ${ident.revision}` : "no IDENT"})`);
  bridge.close();
  process.exit(1);
}
console.log(`CFW detected: ${ident.name} revision ${ident.revision}`);
let page = await bridge.openPage();
for (let attempt = 0; !page.open && page.reason === 1 && attempt < 4; attempt++) { await sleep(2000); page = await bridge.openPage(); }
if (!page.open) throw new Error(`PAGE_OPEN refused, reason ${page.reason}`);
const renew = setInterval(() => bridge.control("keepalive"), 30_000);
const transport = new CfwTransport(bridge, 8000);
const link = new CacheLink(transport, 3, ident.maxChunk);

const ALL = Object.values(REPLY);
const rect = (x: number) => record(3, 15, 0, x, 20, 40, 30);
/** Send a cache message; every lens's reply must have `status` and satisfy `check`. Returns the replies. */
async function expect(name: string, message: Uint8Array, status: number, check?: (r: Reply) => string | undefined): Promise<Map<number, Reply>> {
  const replies = await link.send(message, { allow: ALL });
  if (replies.size !== 2) throw new Error(`${name}: ${replies.size} replies`);
  for (const [lens, r] of replies) {
    const why = r.status !== status ? `status ${describeReply(r)}, expected ${status}` : check?.(r);
    if (why) throw new Error(`${name} lens ${lens === 1 ? "L" : "R"}: ${why} (revision ${r.revision})`);
  }
  return replies;
}
/** A message with its own epoch/request bytes, sent as is (STATE with epoch 0, a bad epoch, a bad period). */
async function raw(name: string, message: Uint8Array, status: number | "nack", check?: (r: Reply) => string | undefined) {
  const ok = await transport.send(message, 3);
  if (status === "nack") {
    if (ok || !/NACK/.test(transport.lastOutcome)) throw new Error(`${name}: expected a NACK, got ${transport.lastOutcome}`);
    return;
  }
  if (!ok) throw new Error(`${name}: ${transport.lastOutcome}`);
  for (const [lens, r] of transport.lastReplies) {
    const why = r.status !== status ? `status ${describeReply(r)}, expected ${status}` : check?.(r);
    if (why) throw new Error(`${name} lens ${lens === 1 ? "L" : "R"}: ${why}`);
  }
}
const withEpoch = (m: Uint8Array, epoch: number) => { const out = Uint8Array.from(m); out[1] = epoch & 0xff; out[2] = (epoch >>> 8) & 0xff; return out; };
const revisionOf = (replies: Map<number, Reply>) => Math.min(...[...replies.values()].map((r) => r.revision));

const checks: [string, () => Promise<string>][] = [
  ["state-unchanged-after-reset", async () => {
    await expect("state", state(0), REPLY.UNCHANGED, (r) => (r.revision === 0 ? undefined : `revision ${r.revision}`));
    return "revision 0";
  }],
  ["put-advances-revision", async () => {
    const r = await expect("put", put([putObject(1, 7, rect(10))]), REPLY.APPLIED, (r) => (r.revision === 1 ? undefined : `revision ${r.revision}`));
    return `revision ${revisionOf(r)}`;
  }],
  ["state-delta-names-the-put", async () => {
    await expect("state", state(0), REPLY.DELTA, (r) => {
      const e = r.entries ?? [];
      return e.length === 1 && !e[0]!.asset && e[0]!.id === 1 && e[0]!.version === 7 ? undefined : `entries ${JSON.stringify(e)}`;
    });
    return "1 entry: object 1 v7";
  }],
  ["state-unchanged-at-revision", async () => { await expect("state", state(1), REPLY.UNCHANGED); return "revision 1"; }],
  ["state-snapshot-with-epoch-0", async () => {
    await raw("snapshot", state(0, 0), REPLY.SNAPSHOT, (r) => (r.total === 1 && r.page === 0 && r.entries?.length === 1 && r.entries[0]!.id === 1 ? undefined : `total ${r.total} entries ${JSON.stringify(r.entries)}`));
    return "1 object";
  }],
  ["state-stale-on-wrong-epoch", async () => { await raw("stale", withEpoch(state(1), link.epoch ^ 0x5555), REPLY.STALE); return "stale"; }],
  ["drop-inactive-then-delta", async () => {
    await expect("drop", control.drop([1]), REPLY.APPLIED, (r) => (r.revision === 2 ? undefined : `revision ${r.revision}`));
    await expect("state", state(1), REPLY.DELTA, (r) => {
      const e = r.entries ?? [];
      return e.length === 1 && e[0]!.id === 1 && e[0]!.version === 0 ? undefined : `entries ${JSON.stringify(e)}`;
    });
    return "object 1 gone";
  }],
  ["drop-active-refused", async () => {
    const v = hash31("r");
    await expect("show", show({ puts: [putObject(2, v, rect(60))], refs: [{ id: 2, version: v }] }), REPLY.APPLIED);
    await expect("drop", control.drop([2]), REPLY.REFUSED, (r) => (r.reason === 5 ? undefined : `reason ${r.reason}`));
    return "reason 5 (active)";
  }],
  ["period-range", async () => {
    await expect("period", control.period(50), REPLY.APPLIED);
    await raw("period 5", withEpoch(Uint8Array.from([41, 0, 0, 0, 0, 2, 5]), link.epoch), "nack");
    await expect("period", control.period(33), REPLY.APPLIED);
    return "50 ok, 5 nacked, 33 ok";
  }],
  ["journal-overflow-snapshot", async () => {
    const base = revisionOf(await expect("put", put([putObject(100, 1, rect(100))]), REPLY.APPLIED));
    for (let i = 1; i <= 66; i++) await expect(`put ${i}`, put([putObject(100 + i, 1, rect(110))]), REPLY.APPLIED);
    await expect("state", state(base), REPLY.SNAPSHOT, (r) => ((r.total ?? 0) >= 67 ? undefined : `total ${r.total}`));
    await expect("state (recent)", state(base + 60), REPLY.DELTA);
    return "67 puts: old revision needs a snapshot, a recent one gets a delta";
  }],
];

let failed = 0;
try {
  await link.reset();
  console.log(`reset: epoch ${link.epoch}, store ${link.storeKiB} KiB`);
  for (const [name, run] of checks) {
    const t0 = performance.now();
    try {
      const detail = await run();
      console.log(`ok   ${name.padEnd(32)} ${String((performance.now() - t0).toFixed(0)).padStart(5)} ms  ${detail}`);
    } catch (e) {
      failed++;
      console.log(`FAIL ${name.padEnd(32)} ${(e as Error).message}`);
    }
  }
} finally {
  clearInterval(renew);
  transport.close();
  bridge.control("close");
  await sleep(300);
  bridge.close();
}
console.log(`\n${checks.length - failed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
