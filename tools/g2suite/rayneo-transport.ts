// Business-0xF0 record transport for the RayNeo iO, over the Mac harness's
// `rayneo-io bridge` socket (glassly/mobile/modules/bluetooth-sdk/tools/
// rayneo-io-mac). Same record layout as the G2 demos' cfw-transport.ts: one
// record per message `[flags][len:u16][crc16][body]`, deflated on its own
// with RESET_CONTEXT; the bridge packetizes a stream and tells us its id.
// Revision 5 firmware keeps a scene per lens and answers each targeted lens
// with its own REPLY and ACK, exactly like the two G2 lenses, so CacheLink
// is the G2 one unchanged.
import { deflateSync, constants } from "node:zlib";
import { control, describeReply, isCacheMessage, parseReply, REPLY, withEpoch, type Reply } from "./object-cache";

export const LENS_LEFT = 1;
export const LENS_RIGHT = 2;
export const LENS_BOTH = 3;
const FLAG_COMPRESSED = 4;
const FLAG_RESET_CONTEXT = 8;

const hex = (b: Uint8Array) => Array.from(b, (v) => v.toString(16).padStart(2, "0")).join("");
const unhex = (s: string) => Uint8Array.from(s.match(/../g) ?? [], (h) => parseInt(h, 16));

export function crc16(bytes: Uint8Array): number {
  let crc = 0xffff;
  for (const b of bytes) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

/** One stream record: `[flags][len:u16][crc16 of the plain message][body]`. */
export function record(message: Uint8Array, targets = LENS_BOTH): Uint8Array {
  if (message.length > 0xffff) throw new Error(`message of ${message.length} bytes exceeds the 65535-byte record limit`);
  const crc = crc16(message);
  let body: Uint8Array = new Uint8Array(deflateSync(message, { finishFlush: constants.Z_SYNC_FLUSH }));
  let flags = targets | FLAG_RESET_CONTEXT | FLAG_COMPRESSED;
  if (body.length > 0xffff) { body = message; flags = targets | FLAG_RESET_CONTEXT; }
  const out = new Uint8Array(5 + body.length);
  out[0] = flags; out[1] = body.length & 0xff; out[2] = body.length >> 8; out[3] = crc & 0xff; out[4] = crc >> 8;
  out.set(body, 5);
  return out;
}

export interface Ident { name: string; revision: number; width: number; height: number; maxChunk: number; flags: number }
export interface Ack { success: boolean; streamId: number; ordinal: number; lens: number; size: number; crc: number }
export interface CacheReplyFrame { streamId: number; ordinal: number; lens: number; reply: Reply }
export interface Page { open: boolean; reason: number }

/** `[01|03][stream][ordinal:u16][lens][size:u16][crc:u16]` */
export function parseAck(p: Uint8Array): Ack | null {
  if (p.length < 9 || (p[0] !== 1 && p[0] !== 3) || (p[4] !== 1 && p[4] !== 2)) return null;
  return { success: p[0] === 1, streamId: p[1]!, ordinal: p[2]! | (p[3]! << 8), lens: p[4]!, size: p[5]! | (p[6]! << 8), crc: p[7]! | (p[8]! << 8) };
}

/** `[05][stream][ordinal:u16][lens][reply…]` */
export function parseCacheReply(p: Uint8Array): CacheReplyFrame | null {
  if (p.length < 15 || p[0] !== 5 || (p[4] !== 1 && p[4] !== 2)) return null;
  const reply = parseReply(p.subarray(5));
  return reply ? { streamId: p[1]!, ordinal: p[2]! | (p[3]! << 8), lens: p[4]!, reply } : null;
}

/** `[11][ver][w:u16][h:u16][maxChunk:u16][storeKiB:u16][revision:u16][flags]name` */
export function parseIdent(p: Uint8Array): Ident | null {
  if (p.length < 13 || p[0] !== 0x11) return null;
  const u16 = (i: number) => p[i]! | (p[i + 1]! << 8);
  return { name: new TextDecoder().decode(p.subarray(13)).replace(/\0+$/, ""), revision: u16(10), width: u16(2), height: u16(4), maxChunk: u16(6), flags: p[12]! };
}

/** The bridge socket: lines in, lines out, replies fanned out to listeners. */
export class RayNeoBridge {
  private socket!: Awaited<ReturnType<typeof Bun.connect>>;
  private pending = "";
  private listeners = new Set<(payload: Uint8Array) => void>();
  private lineWaiters: Array<(line: string) => void> = [];
  hello: Ident | null = null;
  dropped = false;

  static async connect(port = 7310): Promise<RayNeoBridge> {
    const b = new RayNeoBridge();
    const helloLine = new Promise<string>((resolve) => b.lineWaiters.push(resolve));
    b.socket = await Bun.connect({
      hostname: "127.0.0.1", port,
      socket: {
        data: (_s, data) => b.onData(data),
        close: () => { b.dropped = true; },
        error: () => { b.dropped = true; },
      },
    });
    const hello = (await helloLine).split(" ");
    if (hello[0] === "hello" && hello.length >= 7) {
      b.hello = { name: hello[1]!, revision: Number(hello[2]), width: Number(hello[3]), height: Number(hello[4]), maxChunk: Number(hello[5]), flags: Number(hello[6]) };
    }
    return b;
  }

  private onData(data: Buffer) {
    this.pending += data.toString("utf8");
    let nl: number;
    while ((nl = this.pending.indexOf("\n")) >= 0) {
      const line = this.pending.slice(0, nl).trim();
      this.pending = this.pending.slice(nl + 1);
      if (line.startsWith("reply ")) {
        const payload = unhex(line.slice(6));
        for (const l of this.listeners) l(payload);
      } else if (line === "dropped") {
        this.dropped = true;
      } else {
        const w = this.lineWaiters.shift();
        w?.(line);
      }
    }
  }

  onReply(cb: (payload: Uint8Array) => void): () => void {
    this.listeners.add(cb);
    return () => this.listeners.delete(cb);
  }

  private line(cmd: string): void { this.socket.write(cmd + "\n"); }

  /** A non-reply answer line (`sent N`, `error …`). */
  private answer(cmd: string): Promise<string> {
    return new Promise((resolve) => { this.lineWaiters.push(resolve); this.line(cmd); });
  }

  control(verb: "query" | "open" | "close" | "keepalive"): void { this.line(verb); }

  /** Send one record stream; resolves with the stream id the bridge used. */
  async stream(bytes: Uint8Array): Promise<number> {
    const a = await this.answer(`stream ${hex(bytes)}`);
    const m = /^sent (\d+)$/.exec(a);
    if (!m) throw new Error(`bridge: ${a}`);
    return Number(m[1]);
  }

  /** Wait for one business-0xF0 payload the predicate accepts. */
  waitReply<T>(pick: (payload: Uint8Array) => T | null, timeoutMs: number): Promise<T> {
    return new Promise((resolve, reject) => {
      const off = this.onReply((payload) => {
        const v = pick(payload);
        if (v === null) return;
        clearTimeout(timer); off(); resolve(v);
      });
      const timer = setTimeout(() => { off(); reject(new Error("timeout")); }, timeoutMs);
    });
  }

  async query(timeoutMs = 3000): Promise<Ident> {
    const p = this.waitReply(parseIdent, timeoutMs);
    this.control("query");
    return p;
  }

  async openPage(timeoutMs = 3000): Promise<Page> {
    const p = this.waitReply((x) => (x[0] === 0x13 && x.length >= 3 ? { open: x[1] === 1, reason: x[2]! } : null), timeoutMs);
    this.control("open");
    return p;
  }

  close(): void {
    this.line("quit");
    this.socket.end();
  }
}

/** ACK-gated sender: one stream per payload, every targeted lens must ack. */
export class CfwTransport {
  private waiter: ((ack: Ack) => void) | undefined;
  private readonly off: () => void;

  constructor(private readonly bridge: RayNeoBridge, private readonly ackTimeoutMs = 8000) {
    this.off = bridge.onReply((raw) => {
      const ack = parseAck(raw);
      if (ack) { this.waiter?.(ack); return; }
      const reply = parseCacheReply(raw);
      if (reply && reply.streamId === this.currentStream && reply.ordinal === 0) this.lastReplies.set(reply.lens, reply.reply);
    });
  }

  lastOutcome = "";
  lastReplies = new Map<number, Reply>();
  private currentStream = -1;

  async send(payload: Uint8Array, targets = LENS_BOTH): Promise<boolean> {
    const rec = record(payload, targets);
    this.lastReplies = new Map();
    let acked = 0;
    let streamId = -1;
    const name = (lens: number) => (lens === 1 ? "L" : "R");
    const done = new Promise<boolean>((resolve) => {
      const timer = setTimeout(() => {
        this.waiter = undefined;
        this.lastOutcome = acked === 1 ? "timeout: L acked, R silent" : acked === 2 ? "timeout: R acked, L silent" : `timeout after ${this.ackTimeoutMs} ms, acked by neither lens`;
        resolve(false);
      }, this.ackTimeoutMs + payload.length / 8);
      this.waiter = (ack) => {
        if (ack.streamId !== streamId || ack.ordinal !== 0) return;
        if (!ack.success) { clearTimeout(timer); this.waiter = undefined; this.lastOutcome = `NACK from ${name(ack.lens)}`; resolve(false); return; }
        acked |= ack.lens;
        if ((acked & targets) === targets) { clearTimeout(timer); this.waiter = undefined; this.lastOutcome = targets === LENS_BOTH ? "acked by both" : `acked by ${name(targets)}`; resolve(true); }
      };
    });
    streamId = await this.bridge.stream(rec);
    this.currentStream = streamId;
    return done;
  }

  close(): void { this.off(); }
}

export class StaleEpochError extends Error {
  constructor(public readonly lens: number, public readonly epoch: number) { super(`lens ${lens === 1 ? "L" : "R"} is at cache epoch ${epoch}`); }
}

/** Object-cache session over the transport (the G2 demos' CacheLink, unchanged). */
export class CacheLink {
  epoch = 0;
  storeKiB = 0;
  private request = 1;
  constructor(private readonly transport: CfwTransport, public targets = LENS_BOTH, private readonly chunk = 240) {}

  nextRequest(): number { const r = this.request; this.request = this.request >= 0xffff ? 1 : this.request + 1; return r; }

  async reset(): Promise<void> {
    this.epoch = 1 + Math.floor(Math.random() * 0xfffe);
    const replies = await this.send(control.reset(this.nextRequest(), this.chunk + 10), { expectEpoch: false });
    for (const [lens, r] of replies) if (r.epoch !== this.epoch) throw new Error(`lens ${lens} kept epoch ${r.epoch} after a reset to ${this.epoch}`);
    this.storeKiB = Math.min(...[...replies.values()].map((r) => r.storeKiB ?? 0));
  }

  async send(message: Uint8Array, o: { expectEpoch?: boolean; allow?: number[] } = {}): Promise<Map<number, Reply>> {
    const cache = isCacheMessage(message);
    const wire = cache ? withEpoch(message, this.epoch, this.nextRequest()) : message;
    if (!(await this.transport.send(wire, this.targets))) throw new Error(`message (mode ${message[0]}, ${message.length} B): ${this.transport.lastOutcome}`);
    if (!cache) return new Map();
    const replies = this.transport.lastReplies;
    const ok = new Set(o.allow ?? [REPLY.APPLIED, REPLY.UNCHANGED, REPLY.DELTA, REPLY.SNAPSHOT]);
    for (const [lens, r] of replies) {
      if (r.status === REPLY.STALE && o.expectEpoch !== false) throw new StaleEpochError(lens, r.epoch);
      if (!ok.has(r.status)) throw new Error(`mode ${message[0]} refused by lens ${lens === 1 ? "L" : "R"}: ${describeReply(r)}`);
    }
    const expected = this.targets === LENS_BOTH ? 2 : 1;
    if (replies.size < expected && !(o.allow && o.allow.length === 0)) throw new Error(`mode ${message[0]}: cache reply missing from ${replies.size ? (replies.has(1) ? "R" : "L") : "both lenses"}`);
    return replies;
  }
}
