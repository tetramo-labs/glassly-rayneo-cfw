#!/usr/bin/env bun
// Replay the phone driver's own raster messages on a RayNeo iO (revision 7):
// the mode-6 keyframe and the mode-3/8 delta the SDK's G2CfwRenderer encodes
// for the RayNeo panel, dumped by its G2CfwRendererTests with
// RAYNEO_RASTER_DUMP set. Checks the ACKs, that a scene SHOW takes the panel
// back, and that a raster frame takes it over again. Watch the glasses: the
// keyframe is a gray ramp with a white disc near the top left, the delta
// moves it 24 px to the right, the scene is a white rectangle outline.
//
//     cd mobile/modules/bluetooth-sdk && xcodebuild test -scheme BluetoothSDK \
//       -destination 'platform=iOS Simulator,id=…' -only-testing:BluetoothSDKTests/G2CfwRendererTests \
//       TEST_RUNNER_RAYNEO_RASTER_DUMP=/tmp/raster-dump
//     rayneo-io bridge                        # in another terminal
//     bun raster-replay.ts /tmp/raster-dump
import { CacheLink, CfwTransport, RayNeoBridge } from "./rayneo-transport";
import { hide, record, putObject, show, REPLY } from "./object-cache";

const REQUIRED_REVISION = 7;
const IDENT_FLAG_RASTER = 0x10;
const dir = process.argv[2];
if (!dir) { console.log("usage: bun raster-replay.ts <dump dir with keyframe.bin and delta.bin>"); process.exit(2); }
const keyframe = new Uint8Array(await Bun.file(`${dir}/keyframe.bin`).arrayBuffer());
const delta = new Uint8Array(await Bun.file(`${dir}/delta.bin`).arrayBuffer());
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

const bridge = await RayNeoBridge.connect(Number(process.env.G2_PORT ?? 7310));
let ident = bridge.hello && bridge.hello.revision > 0 ? bridge.hello : null;
if (!ident) ident = await bridge.query().catch(() => null);
if (!ident || ident.revision < REQUIRED_REVISION || !(ident.flags & IDENT_FLAG_RASTER)) {
  console.log(`needs the Glassly RayNeo iO CFW revision ${REQUIRED_REVISION} with the raster flag (got ${ident ? `${ident.name} revision ${ident.revision} flags 0x${ident.flags.toString(16)}` : "no IDENT"})`);
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

const describe = (m: Uint8Array) => `mode ${m[0] === 0x83 ? "0x83" : m[0]}, ${m.length} B`;
/** A raster message: ACKed by both lenses, no cache reply. */
async function raster(name: string, message: Uint8Array): Promise<string> {
  const t0 = performance.now();
  if (!(await transport.send(message, 3))) throw new Error(`${name} (${describe(message)}): ${transport.lastOutcome}`);
  return `${describe(message)} ${transport.lastOutcome} in ${(performance.now() - t0).toFixed(0)} ms`;
}

const checks: [string, () => Promise<string>][] = [
  ["keyframe-mode-6", () => raster("keyframe", keyframe)],
  ["delta-after-keyframe", () => raster("delta", delta)],
  ["scene-takes-the-panel-back", async () => {
    await link.reset();
    const replies = await link.send(show({ puts: [putObject(1, 1, record(3, 15, 2, 20, 20, 200, 60))], refs: [{ id: 1, version: 1 }] }));
    for (const [lens, r] of replies) if (r.status !== REPLY.APPLIED) throw new Error(`SHOW lens ${lens}: status ${r.status}`);
    return "SHOW applied: a white outline replaces the image";
  }],
  ["raster-takes-over-again", () => raster("keyframe", keyframe)],
  ["delta-again", () => raster("delta", delta)],
  ["hide-blanks-the-raster", async () => {
    await link.send(hide());
    return "HIDE applied";
  }],
];

let failed = 0;
try {
  for (const [name, run] of checks) {
    try {
      const detail = await run();
      console.log(`ok   ${name.padEnd(28)} ${detail}`);
    } catch (e) {
      failed++;
      console.log(`FAIL ${name.padEnd(28)} ${(e as Error).message}`);
    }
    await sleep(2000);
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
