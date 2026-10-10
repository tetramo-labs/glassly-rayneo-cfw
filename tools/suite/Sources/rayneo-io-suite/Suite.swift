//
//  Suite.swift
//
//  The cases. Each one drives the glasses through the SDK's own codecs and
//  judges the replies; the order matters (page cases leave the page open for
//  the scene cases and close it at the end). Groups: link, cfw, page, scene.
//

import Foundation

struct CaseResult {
    let id: String
    let group: String
    let passed: Bool
    let detail: String
    let seconds: Double
}

enum SuiteError: Error, CustomStringConvertible {
    case failed(String)
    /// The firmware on the glasses predates what the case exercises.
    case skipped(String)
    var description: String {
        switch self {
        case let .failed(why): why
        case let .skipped(why): "skipped: \(why)"
        }
    }
}

@MainActor
final class Suite {
    struct Options {
        var hold: TimeInterval = 15
        var expectedRevision = RayNeoIOFirmwareCatalog.cfwRevision
        var showHold: TimeInterval = 2
        /// The glasses advertise slowly when dozing; scan this long.
        var connectTimeout: TimeInterval = 180
    }

    typealias Body = (Suite) async throws -> String

    struct Case {
        let id: String
        let group: String
        let title: String
        let body: Body
    }

    let link: Link
    let watcher: CfwWatcher
    let options: Options
    private(set) var results: [CaseResult] = []
    /// Stream epoch the scene cases agree on after RESET.
    private var epoch: UInt16 = 0
    private var request: UInt16 = 0
    private var encoder = G2CfwTransport.Encoder()

    init(link: Link, watcher: CfwWatcher, options: Options) {
        self.link = link
        self.watcher = watcher
        self.options = options
    }

    // MARK: - Case list

    lazy var cases: [Case] = [
        Case(id: "connect", group: "link", title: "scan, connect, AA55 handshake") { s in
            try await s.link.connect(timeout: s.options.connectTimeout)
            return "serial \(s.link.serial)"
        },
        Case(id: "version", group: "link", title: "OTA version reply is confirmed and idle") { s in
            s.watcher.clear()
            s.link.requestVersionInfo()
            let reply = try await s.envelope(business: RayNeoIOBusiness.ota, type: RayNeoIOOta.typeVersion, timeout: 5)
            let json = reply.jsonObject ?? [:]
            let version = json["OsVersion"] as? String ?? ""
            let state = (json["OtaValidationState"] as? String ?? "").lowercased()
            let confirmed = RayNeoIOJson.bool(json["OtaValidationConfirmed"]) ?? false
            guard version.contains(RayNeoIOFirmwareCatalog.stockVersion) else { throw SuiteError.failed("OsVersion \(version)") }
            guard confirmed, state == "idle" else { throw SuiteError.failed("state \(state) confirmed \(confirmed)") }
            return "\(version), confirmed, idle"
        },
        Case(id: "battery", group: "link", title: "launcher general status answers with a battery level") { s in
            let reply: [String: Any]? = await withCheckedContinuation { c in
                s.link.sendLauncher(type: RayNeoIOLauncher.typeGeneralStatus, json: RayNeoIOLauncher.generalStatusRequest()) { c.resume(returning: $0) }
            }
            guard let reply, let status = RayNeoIOLauncher.parseGeneralStatus(RayNeoIOJson.encode(reply)), let battery = status.battery
            else { throw SuiteError.failed(reply == nil ? "timeout" : "no battery field") }
            return "\(battery)%"
        },

        Case(id: "ident", group: "cfw", title: "QUERY answered by IDENT within 2 s") { s in
            s.watcher.clear()
            s.query()
            let ident = try await s.ident(timeout: 2)
            guard ident.isGlassly else { throw SuiteError.failed("name \(ident.name)") }
            guard ident.revision >= s.options.expectedRevision else { throw SuiteError.failed("revision \(ident.revision) < \(s.options.expectedRevision)") }
            guard ident.acceptsRawStreams, ident.acceptsCompressedRecords else { throw SuiteError.failed("flags \(ident.flags)") }
            guard ident.hasContext else { throw SuiteError.failed("no context at IDENT time (flags \(ident.flags))") }
            // revision 5 owns the panel's 220-row window; revision 4 pages were 540x180
            guard ident.width == 540, ident.height == (ident.flags & 0x08 != 0 ? 220 : 180) else { throw SuiteError.failed("\(ident.width)x\(ident.height)") }
            return "\(ident.name) chunk \(ident.maxChunk) store \(ident.storeKiB) KiB flags 0x\(String(ident.flags, radix: 16))"
        },
        Case(id: "diag-rx", group: "cfw", title: "DIAG from the RX callback names the registry entry and a context") { s in
            let d = try await s.diag(source: 1, timeout: 2)
            guard d.entry >= 0x1987_C780, d.entry < 0x1987_C780 + 24 * 0x2C, (d.entry - 0x1987_C780) % 0x2C == 0
            else { throw SuiteError.failed("entry \(String(format: "0x%08x", d.entry))") }
            guard d.ctx != 0, d.ctx & 7 == 0 else { throw SuiteError.failed("ctx \(String(format: "0x%08x", d.ctx))") }
            return "entry #\((d.entry - 0x1987_C780) / 0x2C) ctx \(String(format: "0x%08x", d.ctx))"
        },
        Case(id: "diag-ui", group: "cfw", title: "DIAG from the LVGL handler: UI module registered, timer created") { s in
            let d = try await s.diag(source: 2, timeout: 3)
            guard d.uiModule == 5 || d.uiModule == 3 else { throw SuiteError.failed("uiModule \(d.uiModule)") }
            guard d.regRc == 0, d.sendRc == 0 else { throw SuiteError.failed("regRc \(d.regRc) sendRc \(d.sendRc)") }
            guard d.timer != 0 else { throw SuiteError.failed("no service timer") }
            return "module \(d.uiModule) timer \(String(format: "0x%08x", d.timer))"
        },
        Case(id: "singleton", group: "cfw", title: "three more QUERYs reuse the same context") { s in
            let first = s.watcher.diags.first?.ctx ?? 0
            var ctxs: [UInt32] = []
            for _ in 0 ..< 3 {
                s.watcher.clear()
                s.query()
                _ = try await s.ident(timeout: 2)
                let d = try await s.diag(source: 2, timeout: 3)
                ctxs.append(d.ctx)
                try? await Task.sleep(nanoseconds: 500_000_000)
            }
            guard ctxs.allSatisfy({ $0 == first }) else { throw SuiteError.failed("ctx changed: \(ctxs.map { String(format: "0x%08x", $0) })") }
            return "ctx \(String(format: "0x%08x", first)) x4"
        },
        Case(id: "stability", group: "cfw", title: "link stays up after QUERY; tick advances with wall time") { s in
            guard let start = s.watcher.diags.last else { throw SuiteError.failed("no DIAG to start from") }
            let t0 = Date()
            let dropped = await s.link.waitUntil(timeout: s.options.hold) { s.link.phase != .ready }
            if dropped {
                throw SuiteError.failed("link dropped after \(Int(-t0.timeIntervalSinceNow)) s (the revision-3 double free looked like this)")
            }
            s.watcher.clear()
            s.query()
            let end = try await s.diag(source: 1, timeout: 2)
            let wall = Date().timeIntervalSince(t0) * 1000
            let ticks = Double(end.tick &- start.tick)
            guard ticks > wall * 0.7, ticks < wall * 1.3 else { throw SuiteError.failed("tick advanced \(Int(ticks)) ms over \(Int(wall)) ms") }
            return "up \(Int(s.options.hold)) s, tick +\(Int(ticks)) ms over \(Int(wall)) ms"
        },

        Case(id: "nack-closed", group: "scene", title: "a cache message while the page is closed is NACKed") { s in
            s.watcher.clear()
            try s.sendMessage(s.resetMessage(), compress: false)
            let ack = try await s.ack(timeout: 3)
            guard !ack.success else { throw SuiteError.failed("ACKed with the page closed") }
            return "NACK stream \(ack.current.streamId)"
        },

        Case(id: "open", group: "page", title: "PAGE_OPEN is granted (retrying while the launcher is busy)") { s in
            var last: RayNeoIOCfwLink.Page?
            for attempt in 0 ..< 4 {
                if attempt > 0 {
                    try? await Task.sleep(nanoseconds: 2_000_000_000)
                }
                s.watcher.clear()
                s.control(.pageOpen)
                let page = try await s.page(timeout: 3)
                last = page
                if page.open {
                    return "open after \(attempt + 1) attempt(s)"
                }
                guard page.retryable else { break }
            }
            throw SuiteError.failed("refused, reason \(last?.reason ?? 255)")
        },
        Case(id: "disp-probe", group: "disp", title: "DISPLAY_PROBE reports the fbdev driver geometry (revision 5)") { s in
            guard let ident = s.watcher.lastIdent, ident.revision >= 5 else { throw SuiteError.skipped("revision 5 firmware") }
            s.watcher.clear()
            s.displayProbe(mode: 0)
            let report = try await s.dispReport(timeout: 3)
            return report.summary
        },
        Case(id: "keepalive", group: "page", title: "KEEPALIVE renews the lease") { s in
            s.watcher.clear()
            s.control(.keepAlive)
            let page = try await s.page(timeout: 3)
            guard page.open, page.reason == 0 else { throw SuiteError.failed("page \(page.open) reason \(page.reason)") }
            return "lease renewed"
        },

        Case(id: "reset", group: "scene", title: "RESET applies and is ACKed") { s in
            s.watcher.clear()
            try s.sendMessage(s.resetMessage(), compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.applied, reply.mode == 41 else { throw SuiteError.failed("reply \(reply.summary) mode \(reply.mode)") }
            guard s.watcher.acks.first?.success == true else { throw SuiteError.failed("NACK") }
            return "applied, epoch \(reply.epoch), store \(reply.storeBytes / 1024) KiB"
        },
        Case(id: "show-rect", group: "scene", title: "SHOW with an inline filled rectangle (visible for a moment)") { s in
            s.watcher.clear()
            let record = s.rectRecord(filled: true, x: 20, y: 20, w: 200, h: 100, gray: 15)
            try s.sendMessage(s.showMessage(puts: [s.putObject(id: 1, version: 1, record: record)], refs: [(1, 1)]), compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(reply.summary) }
            try? await Task.sleep(nanoseconds: UInt64(s.options.showHold * 1_000_000_000))
            return "applied"
        },
        Case(id: "show-text", group: "scene", title: "SHOW with inline text next to the rectangle") { s in
            s.watcher.clear()
            let text = s.textRecord(x: 240, y: 40, w: 280, h: 60, text: "Glassly CFW suite")
            try s.sendMessage(s.showMessage(puts: [s.putObject(id: 2, version: 1, record: text)], refs: [(1, 1), (2, 1)]), compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(reply.summary) }
            try? await Task.sleep(nanoseconds: UInt64(s.options.showHold * 1_000_000_000))
            return "applied"
        },
        Case(id: "stereo", group: "scene", title: "a SHOW per lens: the square sits at x 100 in the left eye, x 160 in the right (revision 5)") { s in
            guard s.lensCount == 2 else { throw SuiteError.skipped("single-lens firmware") }
            for (lens, x) in [(G2CfwTransport.lensLeft, Int16(100)), (G2CfwTransport.lensRight, Int16(160))] {
                s.watcher.clear()
                let rect = s.rectRecord(filled: true, x: x, y: 80, w: 60, h: 60, gray: 15)
                let frame = s.rectRecord(filled: false, x: 20, y: 20, w: 500, h: 180, gray: 15)
                let show = s.showMessage(puts: [s.putObject(id: 3, version: 1, record: rect), s.putObject(id: 4, version: 1, record: frame)], refs: [(3, 1), (4, 1)])
                guard let record = s.encoder.record(show, targets: lens, allLenses: s.lenses, compress: false) else { throw SuiteError.failed("framing") }
                s.sendStream(record)
                guard await s.link.waitUntil(timeout: 3, { self.watcher.cacheReplies.contains { $0.lens == lens } }),
                      let reply = s.watcher.cacheReplies.first(where: { $0.lens == lens })
                else { throw SuiteError.failed("no REPLY from lens \(lens)") }
                guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed("lens \(lens): \(reply.summary)") }
                guard await s.link.waitUntil(timeout: 3, { self.watcher.acks.contains { $0.lens == lens } }) else { throw SuiteError.failed("lens \(lens): no ACK") }
            }
            try? await Task.sleep(nanoseconds: UInt64(s.options.showHold * 1_000_000_000))
            return "left and right applied"
        },
        Case(id: "geometry-fill", group: "scene", title: "filled circle, triangle, quad, pie and rounded rect (visible)") { s in
            s.watcher.clear()
            let puts = [
                s.putObject(id: 10, version: 1, record: s.geomRecord(5, width: 0, [60, 90, 40])), // CIRCLE_FILL
                s.putObject(id: 11, version: 1, record: s.geomRecord(7, width: 0, [120, 150, 200, 150, 160, 40])), // TRI_FILL
                s.putObject(id: 12, version: 1, record: s.geomRecord(9, width: 0, [220, 60, 300, 40, 310, 140, 230, 150])), // QUAD_FILL
                s.putObject(id: 13, version: 1, record: s.geomRecord(13, width: 0, [380, 90, 45, -90, 150])), // PIE
                s.putObject(id: 14, version: 1, record: s.geomRecord(3, width: 0, [440, 30, 90, 120, 16])), // RECT_FILL r16
            ]
            try s.sendMessage(s.showMessage(puts: puts, refs: [(10, 1), (11, 1), (12, 1), (13, 1), (14, 1)]), compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(reply.summary) }
            try? await Task.sleep(nanoseconds: UInt64(s.options.showHold * 1_000_000_000))
            return "applied"
        },
        Case(id: "geometry-stroke", group: "scene", title: "lines, ring, outlines, beziers, arc and a rounded border (visible)") { s in
            s.watcher.clear()
            let puts = [
                s.putObject(id: 20, version: 1, record: s.geomRecord(1, width: 1, [10, 10, 530, 170])), // LINE hairline
                s.putObject(id: 21, version: 1, record: s.geomRecord(1, width: 5, [10, 170, 530, 10])), // LINE wide
                s.putObject(id: 22, version: 1, record: s.geomRecord(4, width: 3, [60, 90, 40])), // CIRCLE ring
                s.putObject(id: 23, version: 1, record: s.geomRecord(6, width: 1, [120, 150, 200, 150, 160, 40])), // TRI
                s.putObject(id: 24, version: 1, record: s.geomRecord(8, width: 2, [220, 60, 300, 40, 310, 140, 230, 150])), // QUAD
                s.putObject(id: 25, version: 1, record: s.geomRecord(10, width: 2, [330, 160, 380, 20, 430, 160])), // BEZIER2
                s.putObject(id: 26, version: 1, record: s.geomRecord(11, width: 1, [330, 90, 360, 10, 400, 170, 430, 90])), // BEZIER3
                s.putObject(id: 27, version: 1, record: s.geomRecord(12, width: 2, [380, 90, 45, 180, 360])), // ARC
                s.putObject(id: 28, version: 1, record: s.geomRecord(2, width: 2, [440, 30, 90, 120, 12])), // RECT r12 border
            ]
            try s.sendMessage(s.showMessage(puts: puts, refs: (20 ... 28).map { (UInt32($0), 1) }), compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(reply.summary) }
            try? await Task.sleep(nanoseconds: UInt64(s.options.showHold * 1_000_000_000))
            return "applied"
        },
        Case(id: "compressed", group: "scene", title: "a zlib-compressed SHOW is inflated and applied") { s in
            s.watcher.clear()
            let record = s.rectRecord(filled: false, x: 10, y: 10, w: 520, h: 160, gray: 15)
            try s.sendMessage(s.showMessage(puts: [s.putObject(id: 3, version: 1, record: record)], refs: [(1, 1), (2, 1), (3, 1)]), compress: true)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(reply.summary) }
            try? await Task.sleep(nanoseconds: UInt64(s.options.showHold * 1_000_000_000))
            return "applied"
        },
        Case(id: "missing", group: "scene", title: "SHOW referencing an unknown object reports MISSING") { s in
            s.watcher.clear()
            try s.sendMessage(s.showMessage(puts: [], refs: [(99, 1)]), compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.missing else { throw SuiteError.failed(reply.summary) }
            return reply.summary
        },
        Case(id: "stale", group: "scene", title: "a message with the wrong epoch reports STALE") { s in
            s.watcher.clear()
            try s.sendMessage(s.hideMessage(epoch: s.epoch &+ 1), compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.stale else { throw SuiteError.failed(reply.summary) }
            return "stale"
        },
        Case(id: "bad-crc", group: "scene", title: "a record with a wrong CRC is NACKed") { s in
            s.watcher.clear()
            let message = s.hideMessage(epoch: s.epoch)
            let crc = BinaryCodec.crc16CCITT(message) ^ 0x5A5A
            var wire = Data([s.lenses, UInt8(message.count & 0xFF), UInt8(message.count >> 8), UInt8(crc & 0xFF), UInt8(crc >> 8)])
            wire.append(message)
            s.sendStream(wire)
            let ack = try await s.ack(timeout: 3)
            guard !ack.success else { throw SuiteError.failed("ACKed a bad CRC") }
            return "NACK"
        },
        Case(id: "settle-now", group: "anim", title: "a TAG-flagged SHOW without motion reports SETTLED at once") { s in
            try s.requireAnimation()
            s.watcher.clear()
            let record = s.rectRecord(filled: true, x: 40, y: 40, w: 60, h: 60, gray: 15)
            try s.sendMessage(s.showMessage(puts: [s.putObject(id: 40, version: 1, record: record)], refs: [(40, 1)], flags: 0x05), compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(reply.summary) }
            let tag = try await s.settled(timeout: 2)
            guard tag == s.request else { throw SuiteError.failed("settled tag \(tag) != request \(s.request)") }
            return "settled \(tag)"
        },
        Case(id: "glide", group: "anim", title: "GLIDE moves the rectangle over 15 frames, then SETTLED (visible)") { s in
            try s.requireAnimation()
            s.watcher.clear()
            var op = Data([4]); op.append(s.le32(40)); op.append(s.le16(380)); op.append(s.le16(40)); op.append(15); op.append(contentsOf: [0, 0, 255, 255])
            let t0 = Date()
            try s.sendMessage(s.showMessage(puts: [], refs: [(40, 1)], flags: 0x05, ops: [op]), compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(reply.summary) }
            let tag = try await s.settled(timeout: 4)
            let ms = Int(Date().timeIntervalSince(t0) * 1000)
            guard tag == s.request else { throw SuiteError.failed("settled tag \(tag)") }
            guard ms >= 300 else { throw SuiteError.failed("settled after \(ms) ms: no frames ran") }
            return "settled after \(ms) ms"
        },
        Case(id: "tween", group: "anim", title: "TWEEN grows the rectangle with ease-in-out, then SETTLED (visible)") { s in
            try s.requireAnimation()
            s.watcher.clear()
            var op = Data([5]); op.append(s.le32(40)); op.append(s.le16(0x1F)); op.append(30); op.append(contentsOf: [107, 0, 148, 255])
            for v in [60, 20, 420, 140, 20] as [Int16] {
                op.append(s.le16(v))
            }
            let t0 = Date()
            try s.sendMessage(s.showMessage(puts: [], refs: [(40, 1)], flags: 0x05, ops: [op]), compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(reply.summary) }
            let tag = try await s.settled(timeout: 5)
            let ms = Int(Date().timeIntervalSince(t0) * 1000)
            guard tag == s.request, ms >= 700 else { throw SuiteError.failed("tag \(tag) after \(ms) ms") }
            return "settled after \(ms) ms"
        },
        Case(id: "visible", group: "anim", title: "VISIBLE hides and shows the object (KEEP, ops only)") { s in
            try s.requireAnimation()
            for on: UInt8 in [0, 1] {
                s.watcher.clear()
                var op = Data([2]); op.append(s.le32(40)); op.append(on)
                try s.sendMessage(s.showMessage(puts: [], refs: [], flags: 0x03, ops: [op]), compress: false)
                let reply = try await s.cacheReply(timeout: 3)
                guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed("visible=\(on): \(reply.summary)") }
                try? await Task.sleep(nanoseconds: 700_000_000)
            }
            return "hidden, shown"
        },
        Case(id: "bad-op", group: "anim", title: "an op on an object outside the active set is REFUSED") { s in
            try s.requireAnimation()
            s.watcher.clear()
            var op = Data([3]); op.append(s.le32(99)); op.append(s.le16(1)); op.append(s.le16(1))
            try s.sendMessage(s.showMessage(puts: [], refs: [(40, 1)], flags: 0x01, ops: [op]), compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.refused else { throw SuiteError.failed(reply.summary) }
            return reply.summary
        },
        Case(id: "hide-tagged", group: "anim", title: "a TAG-flagged HIDE reports SETTLED") { s in
            try s.requireAnimation()
            s.watcher.clear()
            var m = s.hideMessage(epoch: s.epoch)
            m[m.startIndex + 5] = 0x05
            try s.sendMessage(m, compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(reply.summary) }
            let tag = try await s.settled(timeout: 2)
            guard tag == s.request else { throw SuiteError.failed("settled tag \(tag)") }
            return "settled"
        },
        Case(id: "pingpong", group: "demo", title: "3D ping pong: the ball glides between a near and a far paddle (stereo, revision 5)") { s in
            try s.requireAnimation()
            guard s.lensCount == 2 else { throw SuiteError.skipped("single-lens firmware") }
            // depth is a disparity: the left lens draws at x + depth, the right at x - depth
            func lensX(_ x: Int16, _ depth: Int16, _ lens: UInt8) -> Int16 { lens == G2CfwTransport.lensLeft ? x + depth : x - depth }
            let near: Int16 = 5, far: Int16 = -5
            let startX: Int16 = 80, endX: Int16 = 440
            s.watcher.clear()
            try s.sendMessage(s.resetMessage(), compress: false)
            _ = try await s.cacheReply(timeout: 3)
            for lens in [G2CfwTransport.lensLeft, G2CfwTransport.lensRight] {
                s.watcher.clear()
                let table = s.rectRecord(filled: false, x: 20, y: 20, w: 500, h: 180, gray: 6)
                let net = s.rectRecord(filled: true, x: 268, y: 20, w: 4, h: 180, gray: 4)
                let left = s.rectRecord(filled: true, x: lensX(40, near, lens), y: 70, w: 10, h: 80, gray: 15)
                let right = s.rectRecord(filled: true, x: lensX(490, far, lens), y: 70, w: 10, h: 80, gray: 15)
                let ball = s.geomRecord(5, width: 0, [lensX(startX, near, lens), 60, 10]) // CIRCLE_FILL x y r
                let puts = [s.putObject(id: 60, version: 1, record: table), s.putObject(id: 61, version: 1, record: net),
                            s.putObject(id: 62, version: 1, record: left), s.putObject(id: 63, version: 1, record: right),
                            s.putObject(id: 64, version: 1, record: ball)]
                try s.sendMessage(s.showMessage(puts: puts, refs: [(60, 1), (61, 1), (62, 1), (63, 1), (64, 1)]), compress: false, targets: lens)
                try await s.lensApplied(lens, what: "scene")
            }
            // six crossings: the ball glides from the near paddle to the far one and back,
            // its disparity sliding from +8 to -8, so it travels away and comes back
            var x = startX, depth = near, y: Int16 = 60
            let ys: [Int16] = [60, 150, 90, 160, 70, 140]
            for i in 0 ..< 6 {
                let toX = x == startX ? endX : startX, toDepth = depth == near ? far : near, toY = ys[i]
                // both lens records in one stream: the firmware applies them in the
                // same handler call, so the two glides start on the same frame tick
                s.watcher.clear()
                var stream = Data()
                for lens in [G2CfwTransport.lensLeft, G2CfwTransport.lensRight] {
                    let dx = lensX(toX, toDepth, lens) - lensX(x, depth, lens)
                    var op = Data([4]); op.append(s.le32(64)); op.append(s.le16(dx)); op.append(s.le16(toY - y)); op.append(24); op.append(contentsOf: [0, 0, 255, 255])
                    guard let record = s.encoder.record(s.showMessage(puts: [], refs: [], flags: 0x03, ops: [op]), targets: lens, allLenses: s.lenses, compress: false)
                    else { throw SuiteError.failed("framing") }
                    stream.append(record)
                }
                s.sendStream(stream)
                for lens in [G2CfwTransport.lensLeft, G2CfwTransport.lensRight] {
                    try await s.lensApplied(lens, what: "crossing \(i)")
                }
                x = toX; depth = toDepth; y = toY
                try? await Task.sleep(nanoseconds: 900_000_000)
            }
            try? await Task.sleep(nanoseconds: UInt64(s.options.showHold * 1_000_000_000))
            return "6 crossings"
        },
        Case(id: "path", group: "vector", title: "PATH: an even-odd ring filled by the firmware's compiled edges (revision 6, visible)") { s in
            try s.requireVector()
            s.watcher.clear()
            let ring = s.pathCommands([(0, 80, 40), (1, 240, 40), (1, 240, 200), (1, 80, 200), (4, 0, 0),
                                       (0, 120, 80), (1, 200, 80), (1, 200, 160), (1, 120, 160), (4, 0, 0)])
            let put = s.putObject(id: 80, version: 1, record: s.pathRecord(rule: 1, x: 0, y: 0, scale: 256, commands: ring))
            try s.sendMessage(s.showMessage(puts: [put], refs: [(80, 1)]), compress: false)
            let r = try await s.cacheReply(timeout: 3)
            guard r.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(r.summary) }
            try? await Task.sleep(nanoseconds: UInt64(s.options.showHold * 1_000_000_000))
            return "ring applied, revision \(r.revision)"
        },
        Case(id: "path-open-refused", group: "vector", title: "a PATH whose contour never closes is REFUSED (reason 2)") { s in
            try s.requireVector()
            s.watcher.clear()
            let open = s.pathCommands([(0, 10, 10), (1, 60, 10), (1, 60, 40)])
            let put = s.putObject(id: 81, version: 1, record: s.pathRecord(rule: 0, x: 0, y: 0, scale: 256, commands: open))
            try s.sendMessage(s.showMessage(puts: [put], refs: [(81, 1)]), compress: false)
            let r = try await s.cacheReply(timeout: 3)
            guard r.status == G2CfwTransport.CacheReply.refused, r.extra.first == 2 else { throw SuiteError.failed(r.summary) }
            return r.summary
        },
        Case(id: "rotate", group: "vector", title: "ROTATE turns a rectangle a quarter turn over 600 ms, then SETTLED (visible)") { s in
            try s.requireVector()
            s.watcher.clear()
            let put = s.putObject(id: 82, version: 1, record: s.rectRecord(filled: true, x: 200, y: 80, w: 120, h: 50, gray: 15))
            try s.sendMessage(s.showMessage(puts: [put], refs: [(82, 1)], flags: 0x05, ops: [s.rotateOp(id: 82, degrees: 90, px: 260, py: 105, ms: 600)]), compress: false)
            let r = try await s.cacheReply(timeout: 3)
            guard r.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(r.summary) }
            let t0 = Date()
            _ = try await s.settled(timeout: 4)
            let ms = Int(Date().timeIntervalSince(t0) * 1000)
            guard ms >= 400 else { throw SuiteError.failed("settled after \(ms) ms, expected the 600 ms turn") }
            try? await Task.sleep(nanoseconds: UInt64(s.options.showHold * 1_000_000_000))
            return "settled after \(ms) ms"
        },
        Case(id: "rotate-text-refused", group: "vector", title: "ROTATE on inline text is REFUSED (reason 3)") { s in
            try s.requireVector()
            s.watcher.clear()
            let put = s.putObject(id: 83, version: 1, record: s.textRecord(x: 20, y: 20, w: 200, h: 24, text: "no spin"))
            try s.sendMessage(s.showMessage(puts: [put], refs: [(83, 1)], ops: [s.rotateOp(id: 83, degrees: 45, px: 100, py: 30, ms: 0)]), compress: false)
            let r = try await s.cacheReply(timeout: 3)
            guard r.status == G2CfwTransport.CacheReply.refused, r.extra.first == 3 else { throw SuiteError.failed(r.summary) }
            return r.summary
        },
        Case(id: "state", group: "journal", title: "STATE: UNCHANGED at the revision, DELTA after a PUT, SNAPSHOT with epoch 0, STALE on a wrong epoch") { s in
            try s.requireVector()
            s.watcher.clear()
            var m = s.header(37, epoch: s.epoch)
            m.append(s.putObject(id: 84, version: 3, record: s.rectRecord(filled: true, x: 10, y: 10, w: 20, h: 20, gray: 8)))
            try s.sendMessage(m, compress: false)
            let putReply = try await s.cacheReply(timeout: 3)
            guard putReply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed("put: \(putReply.summary)") }
            let rev = putReply.revision
            s.watcher.clear()
            try s.sendMessage(s.stateMessage(epoch: s.epoch, revision: rev, page: 0), compress: false)
            let same = try await s.cacheReply(timeout: 3)
            guard same.status == G2CfwTransport.CacheReply.unchanged else { throw SuiteError.failed("at revision \(rev): \(same.summary)") }
            s.watcher.clear()
            try s.sendMessage(s.stateMessage(epoch: s.epoch, revision: rev - 1, page: 0), compress: false)
            let delta = try await s.cacheReply(timeout: 3)
            guard delta.status == G2CfwTransport.CacheReply.delta, delta.extra.count >= 10, delta.extra[0] >= 1 else { throw SuiteError.failed("below revision: \(delta.summary) extra \(delta.extra)") }
            let ids = stride(from: 1, to: delta.extra.count - 8, by: 9).map { i in UInt32(delta.extra[i + 1]) | UInt32(delta.extra[i + 2]) << 8 | UInt32(delta.extra[i + 3]) << 16 | UInt32(delta.extra[i + 4]) << 24 }
            guard ids.contains(84) else { throw SuiteError.failed("delta names \(ids), not 84") }
            s.watcher.clear()
            try s.sendMessage(s.stateMessage(epoch: 0, revision: 0, page: 0), compress: false)
            let snap = try await s.cacheReply(timeout: 3)
            guard snap.status == G2CfwTransport.CacheReply.snapshot, snap.extra.count >= 5, snap.extra[4] >= 1 else { throw SuiteError.failed("epoch 0: \(snap.summary)") }
            s.watcher.clear()
            try s.sendMessage(s.stateMessage(epoch: s.epoch ^ 0x5555, revision: rev, page: 0), compress: false)
            let stale = try await s.cacheReply(timeout: 3)
            guard stale.status == G2CfwTransport.CacheReply.stale else { throw SuiteError.failed("wrong epoch: \(stale.summary)") }
            return "revision \(rev): unchanged, delta \(delta.extra[0]) entries, snapshot \(snap.extra[4]) entries, stale"
        },
        Case(id: "drop", group: "journal", title: "DROP frees an inactive object (journaled as gone) and refuses an active one (reason 5)") { s in
            try s.requireVector()
            s.watcher.clear()
            var m = s.header(37, epoch: s.epoch)
            m.append(s.putObject(id: 85, version: 1, record: s.rectRecord(filled: true, x: 10, y: 10, w: 20, h: 20, gray: 8)))
            try s.sendMessage(m, compress: false)
            let before = try await s.cacheReply(timeout: 3)
            guard before.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed("put: \(before.summary)") }
            s.watcher.clear()
            try s.sendMessage(s.dropMessage(ids: [85]), compress: false)
            let dropped = try await s.cacheReply(timeout: 3)
            guard dropped.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed("drop: \(dropped.summary)") }
            s.watcher.clear()
            try s.sendMessage(s.stateMessage(epoch: s.epoch, revision: before.revision, page: 0), compress: false)
            let delta = try await s.cacheReply(timeout: 3)
            guard delta.status == G2CfwTransport.CacheReply.delta, delta.extra.count >= 10 else { throw SuiteError.failed("after drop: \(delta.summary)") }
            let gone = stride(from: 1, to: delta.extra.count - 8, by: 9).contains { i in
                UInt32(delta.extra[i + 1]) == 85 && delta.extra[i + 2] == 0 && delta.extra[i + 5] == 0 && delta.extra[i + 6] == 0 && delta.extra[i + 7] == 0 && delta.extra[i + 8] == 0
            }
            guard gone else { throw SuiteError.failed("delta does not list 85 as gone: \(delta.extra)") }
            s.watcher.clear()
            try s.sendMessage(s.showMessage(puts: [s.putObject(id: 86, version: 1, record: s.rectRecord(filled: true, x: 300, y: 40, w: 40, h: 40, gray: 15))], refs: [(86, 1)]), compress: false)
            guard try await s.cacheReply(timeout: 3).status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed("show 86") }
            s.watcher.clear()
            try s.sendMessage(s.dropMessage(ids: [86]), compress: false)
            let refused = try await s.cacheReply(timeout: 3)
            guard refused.status == G2CfwTransport.CacheReply.refused, refused.extra.first == 5 else { throw SuiteError.failed("drop active: \(refused.summary)") }
            return "85 dropped and journaled, 86 refused (active)"
        },
        Case(id: "image-options", group: "vector", title: "IMAGE asset drawn through the texture options: ink 15 transparent, ink 8 opaque, inverse; then a colour tween fades it (revision 6, visible)") { s in
            try s.requireVector()
            s.watcher.clear()
            // 32x24: left half gray 15, right half gray 0 (the transparent half)
            var pixels: [UInt8] = []
            for _ in 0 ..< 24 { pixels.append(contentsOf: [UInt8](repeating: 15, count: 16)); pixels.append(contentsOf: [UInt8](repeating: 0, count: 16)) }
            let image = s.rleImage(pixels, width: 32, height: 24)
            var put = s.header(37, epoch: s.epoch)
            put.append(s.putAsset(id: 90, version: 1, kind: 0, data: image))
            try s.sendMessage(put, compress: false)
            let a = try await s.cacheReply(timeout: 3)
            guard a.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed("asset: \(a.summary)") }
            s.watcher.clear()
            let puts = [
                s.putObject(id: 91, version: 1, record: s.imageRecord(options: 0x1F, x: 40, y: 120, asset: 90)),
                s.putObject(id: 92, version: 1, record: s.imageRecord(options: 0x08, x: 120, y: 120, asset: 90)),
                s.putObject(id: 93, version: 1, record: s.imageRecord(options: 0x2F, x: 200, y: 120, asset: 90)),
            ]
            // fade the first copy: colour byte 0x1F -> 0x10 over 20 frames, tagged
            var tween = Data([5]); tween.append(s.le32(91)); tween.append(s.le16(0x100)); tween.append(20); tween.append(contentsOf: [0, 0, 255, 255]); tween.append(s.le16(0x10))
            try s.sendMessage(s.showMessage(puts: puts, refs: [(91, 1), (92, 1), (93, 1)], flags: 0x05, ops: [tween]), compress: false)
            let r = try await s.cacheReply(timeout: 3)
            guard r.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(r.summary) }
            _ = try await s.settled(timeout: 4)
            try? await Task.sleep(nanoseconds: UInt64(s.options.showHold * 1_000_000_000))
            return "three draws applied, fade settled"
        },
        Case(id: "hide", group: "scene", title: "HIDE clears the page") { s in
            s.watcher.clear()
            try s.sendMessage(s.hideMessage(epoch: s.epoch), compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(reply.summary) }
            return "applied"
        },

        Case(id: "close", group: "page", title: "PAGE_CLOSE reports closed with reason 0") { s in
            s.watcher.clear()
            s.control(.pageClose)
            let page = try await s.page(timeout: 3)
            guard !page.open, page.reason == 0 else { throw SuiteError.failed("page \(page.open) reason \(page.reason)") }
            return "closed"
        },
        Case(id: "reopen", group: "page", title: "the page opens again after a close, then closes") { s in
            s.watcher.clear()
            s.control(.pageOpen)
            let page = try await s.page(timeout: 3)
            guard page.open else { throw SuiteError.failed("refused, reason \(page.reason)") }
            s.watcher.clear()
            s.control(.pageClose)
            let closed = try await s.page(timeout: 3)
            guard !closed.open else { throw SuiteError.failed("still open") }
            return "open, closed"
        },
        Case(id: "settle", group: "cfw", title: "link still up after the page cases") { s in
            let dropped = await s.link.waitUntil(timeout: 5) { s.link.phase != .ready }
            if dropped {
                throw SuiteError.failed("link dropped")
            }
            return "up"
        },
    ]

    // MARK: - Runner

    func run(only filters: [String]) async -> Bool {
        let selected = cases.filter { filters.isEmpty || filters.contains($0.id) || filters.contains($0.group) }
        var allPassed = true
        for c in selected {
            if c.id != "connect", link.phase != .ready {
                results.append(CaseResult(id: c.id, group: c.group, passed: false, detail: "not connected (\(link.phase))", seconds: 0))
                allPassed = false
                continue
            }
            let start = Date()
            do {
                let detail = try await c.body(self)
                results.append(CaseResult(id: c.id, group: c.group, passed: true, detail: detail, seconds: Date().timeIntervalSince(start)))
                print(String(format: "PASS  %-12@ %@", c.id as NSString, detail as NSString))
            } catch let SuiteError.skipped(why) {
                results.append(CaseResult(id: c.id, group: c.group, passed: true, detail: "skipped: \(why)", seconds: Date().timeIntervalSince(start)))
                print(String(format: "SKIP  %-12@ %@", c.id as NSString, why as NSString))
            } catch {
                results.append(CaseResult(id: c.id, group: c.group, passed: false, detail: "\(error)", seconds: Date().timeIntervalSince(start)))
                print(String(format: "FAIL  %-12@ %@", c.id as NSString, "\(error)" as NSString))
                allPassed = false
            }
        }
        return allPassed
    }

    // MARK: - Waiting on the watcher

    private func ident(timeout: TimeInterval) async throws -> RayNeoIOCfwLink.Ident {
        guard await link.waitUntil(timeout: timeout, { self.watcher.ident != nil }), let ident = watcher.ident else { throw SuiteError.failed("no IDENT in \(timeout) s") }
        return ident
    }

    private func diag(source: UInt8, timeout: TimeInterval) async throws -> CfwDiag {
        guard await link.waitUntil(timeout: timeout, { self.watcher.diags.contains { $0.source == source } }),
              let d = watcher.diags.first(where: { $0.source == source })
        else { throw SuiteError.failed("no DIAG source \(source) in \(timeout) s") }
        return d
    }

    private func requireAnimation() throws {
        guard let ident = watcher.lastIdent else { throw SuiteError.skipped("no IDENT") }
        guard ident.revision >= 4 else { throw SuiteError.skipped("needs revision 4, glasses run \(ident.revision)") }
    }

    /// Every lens reports SETTLED; wait for all of them so a straggler never
    /// counts for the next case, and return the last (they carry one tag).
    private func settled(timeout: TimeInterval) async throws -> UInt16 {
        let n = lensCount
        guard await link.waitUntil(timeout: timeout, { self.watcher.settled.count >= n }), let tag = watcher.settled.last
        else { throw SuiteError.failed("no SETTLED from \(n) lens(es) in \(timeout) s (got \(watcher.settled.count))") }
        return tag
    }

    private func page(timeout: TimeInterval) async throws -> RayNeoIOCfwLink.Page {
        guard await link.waitUntil(timeout: timeout, { !self.watcher.pages.isEmpty }), let p = watcher.pages.first else { throw SuiteError.failed("no PAGE reply in \(timeout) s") }
        return p
    }

    /// Lens targets of the records this suite sends: both on stereo firmware
    /// (IDENT flag 0x08, revision 5), the single left lens before.
    private var lenses: UInt8 {
        (watcher.lastIdent?.flags ?? 0) & 0x08 != 0 ? G2CfwTransport.lensBoth : RayNeoIOCfwLink.lens
    }

    private var lensCount: Int {
        lenses == G2CfwTransport.lensBoth ? 2 : 1
    }

    /// Every targeted lens ACKs (or NACKs); wait for all of them so a late one
    /// never counts for the next case. Returns the first.
    private func ack(timeout: TimeInterval) async throws -> G2CfwTransport.Ack {
        let n = lensCount
        guard await link.waitUntil(timeout: timeout, { self.watcher.acks.count >= n }), let a = watcher.acks.first
        else { throw SuiteError.failed("no ACK/NACK from \(n) lens(es) in \(timeout) s (got \(watcher.acks.count))") }
        return a
    }

    /// The firmware answers a handled message with REPLY then ACK per lens;
    /// wait for all so a trailing one never counts for the next case. The
    /// first lens's REPLY is returned; a lens that disagrees fails the case.
    private func cacheReply(timeout: TimeInterval) async throws -> G2CfwTransport.CacheReply {
        let n = lensCount
        guard await link.waitUntil(timeout: timeout, { self.watcher.cacheReplies.count >= n }), let r = watcher.cacheReplies.first
        else { throw SuiteError.failed("no REPLY from \(n) lens(es) in \(timeout) s" + (watcher.acks.first.map { " (\($0.success ? "ACK" : "NACK") arrived)" } ?? "")) }
        guard await link.waitUntil(timeout: timeout, { self.watcher.acks.count >= n }) else { throw SuiteError.failed("REPLY without ACK") }
        if let other = watcher.cacheReplies.first(where: { $0.status != r.status }) {
            throw SuiteError.failed("lenses disagree: \(r.summary) vs \(other.summary)")
        }
        return r
    }

    /// One lens's applied REPLY and its ACK for the last record sent to it.
    private func lensApplied(_ lens: UInt8, what: String) async throws {
        guard await link.waitUntil(timeout: 3, { self.watcher.cacheReplies.contains { $0.lens == lens } }),
              let reply = watcher.cacheReplies.first(where: { $0.lens == lens })
        else { throw SuiteError.failed("\(what): no REPLY from lens \(lens)") }
        guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed("\(what) lens \(lens): \(reply.summary)") }
        guard await link.waitUntil(timeout: 3, { self.watcher.acks.contains { $0.lens == lens } }) else { throw SuiteError.failed("\(what) lens \(lens): no ACK") }
    }

    private func envelope(business: UInt8, type: Int, timeout: TimeInterval) async throws -> RayNeoIOEnvelope {
        let match = { (e: (business: UInt8, envelope: RayNeoIOEnvelope)) in e.business == business && e.envelope.type == type }
        guard await link.waitUntil(timeout: timeout, { self.watcher.envelopes.contains(where: match) }), let e = watcher.envelopes.first(where: match)
        else { throw SuiteError.failed("no business \(business) type \(type) reply in \(timeout) s") }
        return e.envelope
    }

    // MARK: - Sending

    private func query() {
        link.sendFrame(business: RayNeoIOBusiness.cfw, payload: RayNeoIOCfwLink.controlPacket(.query))
    }

    /// Revision 5 display probe: `[0x10][0][5][mode]`.
    func displayProbe(mode: UInt8) {
        link.sendFrame(business: RayNeoIOBusiness.cfw, payload: Data([0x10, 0, 5, mode]))
    }

    private func dispReport(timeout: TimeInterval) async throws -> DispReport {
        guard await link.waitUntil(timeout: timeout, { self.watcher.unparsed.contains { $0.first == DispReport.kind } }),
              let raw = watcher.unparsed.first(where: { $0.first == DispReport.kind }), let r = DispReport(raw)
        else { throw SuiteError.failed("no DISP report in \(timeout) s") }
        return r
    }

    private func control(_ c: RayNeoIOCfwLink.Control) {
        link.sendFrame(business: RayNeoIOBusiness.cfw, payload: RayNeoIOCfwLink.controlPacket(c))
    }

    /// Frame one L2 message as a record (the phone's encoder) and stream it
    /// to `targets` (default: every lens the firmware has).
    private func sendMessage(_ message: Data, compress: Bool, targets: UInt8? = nil) throws {
        guard let record = encoder.record(message, targets: targets ?? lenses, allLenses: lenses, compress: compress)
        else { throw SuiteError.failed("record framing failed") }
        sendStream(record)
    }

    private func sendStream(_ stream: Data) {
        let packets = RayNeoIOCfwLink.packetize(stream: stream, streamId: link.nextStreamId, maxChunk: watcher.ident?.maxChunk ?? RayNeoIOCfwLink.defaultMaxChunk)
        for p in packets {
            link.sendFrame(business: RayNeoIOBusiness.cfw, payload: p)
        }
    }

    // MARK: - L2 messages (docs/PROTOCOL.md)

    private func header(_ mode: UInt8, epoch: UInt16) -> Data {
        request &+= 1
        return Data([mode, UInt8(epoch & 0xFF), UInt8(epoch >> 8), UInt8(request & 0xFF), UInt8(request >> 8)])
    }

    private func resetMessage() -> Data {
        epoch &+= 1
        var m = header(41, epoch: epoch)
        m.append(contentsOf: [0x00, 64])
        return m
    }

    private func hideMessage(epoch: UInt16) -> Data {
        var m = header(39, epoch: epoch)
        m.append(contentsOf: [0x00, 0x00])
        return m
    }

    private func showMessage(puts: [Data], refs: [(UInt32, UInt32)], flags: UInt8 = 0x01, ops: [Data] = []) -> Data {
        var m = header(38, epoch: epoch)
        m.append(contentsOf: [flags, 0x00, UInt8(puts.count)])
        for p in puts {
            m.append(p) // back to back, no length prefix (G2CfwScene.showPayload)
        }
        m.append(contentsOf: [UInt8(refs.count & 0xFF), UInt8(refs.count >> 8)])
        for (id, ver) in refs {
            m.append(le32(id))
            m.append(le32(ver))
        }
        for op in ops {
            m.append(op)
        }
        return m
    }

    private func putObject(id: UInt32, version: UInt32, record: Data) -> Data {
        var e = Data([0x01])
        e.append(le32(id))
        e.append(le32(version))
        e.append(record)
        return e
    }

    /// `RECT(2)` / `RECT_FILL(3)`: 20 bytes, options/gray at [2], x y w h r as s16 from [4].
    private func rectRecord(filled: Bool, x: Int16, y: Int16, w: Int16, h: Int16, gray: UInt8) -> Data {
        var r = Data([filled ? 3 : 2, 0, gray & 0x0F, 0])
        for v in [x, y, w, h, 0] {
            r.append(le16(v))
        }
        r.append(contentsOf: [UInt8](repeating: 0, count: 6))
        return r
    }

    /// Geometry record (types 1..13): `[type][1][gray][width]` + eight i16 params (G2CfwScene.Slot pads to 8).
    private func geomRecord(_ type: UInt8, width: UInt8, _ params: [Int16]) -> Data {
        var r = Data([type, 1, 0x0F, width])
        for i in 0 ..< 8 {
            r.append(le16(i < params.count ? params[i] : 0))
        }
        return r
    }

    /// `TEXT_INLINE(17)`: 13 + len bytes, x y w h at [4], length at [12].
    private func textRecord(x: Int16, y: Int16, w: Int16, h: Int16, text: String) -> Data {
        let bytes = Array(text.utf8.prefix(64))
        var r = Data([17, 0, 0x1F, 0])   // options: ink 15, transparent (the phone's builtin text options)
        for v in [x, y, w, h] {
            r.append(le16(v))
        }
        r.append(UInt8(bytes.count))
        r.append(contentsOf: bytes)
        return r
    }

    private func requireVector() throws {
        guard let ident = watcher.lastIdent else { throw SuiteError.skipped("no IDENT") }
        guard ident.revision >= 6 else { throw SuiteError.skipped("needs revision 6, glasses run \(ident.revision)") }
    }

    /// Path commands in Q4: (op, x, y) with 0 M, 1 L, 4 Z (coordinates in pixels).
    private func pathCommands(_ cmds: [(UInt8, Int, Int)]) -> Data {
        var d = Data()
        for (op, x, y) in cmds {
            d.append(op)
            if op != 4 { d.append(le16(Int16(x * 16))); d.append(le16(Int16(y * 16))) }
        }
        return d
    }

    /// `PATH(18)`: [18][1][gray][rule][x][y][scale Q8][len][commands] (G2 VECTOR_PROTOCOL.md).
    private func pathRecord(rule: UInt8, x: Int16, y: Int16, scale: UInt16, commands: Data, gray: UInt8 = 15) -> Data {
        var r = Data([18, 1, gray & 0x0F, rule])
        r.append(le16(x)); r.append(le16(y))
        r.append(le16(Int16(bitPattern: scale)))
        r.append(le16(Int16(commands.count)))
        r.append(commands)
        return r
    }

    /// `[9][id][angle deg*256 i32][pivot x][pivot y][ms u16][curve x4]`, linear easing.
    private func rotateOp(id: UInt32, degrees: Int32, px: Int16, py: Int16, ms: UInt16) -> Data {
        var op = Data([9])
        op.append(le32(id))
        op.append(le32(UInt32(bitPattern: degrees * 256)))
        op.append(le16(px)); op.append(le16(py))
        op.append(le16(Int16(bitPattern: ms)))
        op.append(contentsOf: [0, 0, 255, 255])
        return op
    }

    /// `[40][epoch][revision u32][page u16]`: no request field (the reply echoes the page).
    private func stateMessage(epoch: UInt16, revision: UInt32, page: UInt16) -> Data {
        var m = Data([40, UInt8(epoch & 0xFF), UInt8(epoch >> 8)])
        m.append(le32(revision))
        m.append(le16(Int16(bitPattern: page)))
        return m
    }

    /// CONTROL sub 1: `[41][epoch][request][1][n][id u32 x n]`.
    private func dropMessage(ids: [UInt32]) -> Data {
        var m = header(41, epoch: epoch)
        m.append(contentsOf: [1, UInt8(ids.count)])
        for id in ids { m.append(le32(id)) }
        return m
    }

    /// `PUT_ASSET`: `[00][id][ver][kind][total u32][offset u32][len u16][data]`, one chunk.
    private func putAsset(id: UInt32, version: UInt32, kind: UInt8, data: Data) -> Data {
        var e = Data([0x00])
        e.append(le32(id)); e.append(le32(version)); e.append(kind)
        e.append(le32(UInt32(data.count))); e.append(le32(0))
        e.append(le16(Int16(data.count)))
        e.append(data)
        return e
    }

    /// `IMAGE(14)`: `[14][1][options][0][x][y][asset u32]`; options = [inverse 0x20][transparent 0x10][ink].
    private func imageRecord(options: UInt8, x: Int16, y: Int16, asset: UInt32) -> Data {
        var r = Data([14, 1, options, 0])
        r.append(le16(x)); r.append(le16(y)); r.append(le32(asset))
        return r
    }

    /// `[w][h][4bpp RLE]`: runs `(n<<4)|c` for n <= 15, `[c][n]` for n <= 255, `[c][0][n u16]` beyond.
    private func rleImage(_ gray: [UInt8], width: Int, height: Int) -> Data {
        var out = Data([UInt8(width), UInt8(height)])
        var i = 0
        while i < gray.count {
            let c = gray[i] & 15
            var n = 1
            while i + n < gray.count, gray[i + n] & 15 == c, n < 65535 { n += 1 }
            if n <= 15 { out.append(UInt8(n << 4) | c) }
            else if n <= 255 { out.append(c); out.append(UInt8(n)) }
            else { out.append(c); out.append(0); out.append(UInt8(n & 0xFF)); out.append(UInt8(n >> 8)) }
            i += n
        }
        return out
    }

    func le16(_ v: Int16) -> Data {
        let u = UInt16(bitPattern: v)
        return Data([UInt8(u & 0xFF), UInt8(u >> 8)])
    }

    func le32(_ v: UInt32) -> Data {
        Data([UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF), UInt8((v >> 16) & 0xFF), UInt8(v >> 24)])
    }
}
