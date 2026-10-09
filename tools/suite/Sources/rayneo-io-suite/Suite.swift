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
            try await s.link.connect(timeout: 60)
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
            guard ident.width == 540, ident.height == 180 else { throw SuiteError.failed("\(ident.width)x\(ident.height)") }
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
            var wire = Data([0x01, UInt8(message.count & 0xFF), UInt8(message.count >> 8), UInt8(crc & 0xFF), UInt8(crc >> 8)])
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
            try s.sendMessage(s.showMessage(puts: [], refs: [(40, 1)], flags: 0x05, ops: [op]), compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(reply.summary) }
            let t0 = Date()
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
            try s.sendMessage(s.showMessage(puts: [], refs: [(40, 1)], flags: 0x05, ops: [op]), compress: false)
            let reply = try await s.cacheReply(timeout: 3)
            guard reply.status == G2CfwTransport.CacheReply.applied else { throw SuiteError.failed(reply.summary) }
            let t0 = Date()
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

    private func settled(timeout: TimeInterval) async throws -> UInt16 {
        guard await link.waitUntil(timeout: timeout, { !self.watcher.settled.isEmpty }), let tag = watcher.settled.first
        else { throw SuiteError.failed("no SETTLED in \(timeout) s") }
        return tag
    }

    private func page(timeout: TimeInterval) async throws -> RayNeoIOCfwLink.Page {
        guard await link.waitUntil(timeout: timeout, { !self.watcher.pages.isEmpty }), let p = watcher.pages.first else { throw SuiteError.failed("no PAGE reply in \(timeout) s") }
        return p
    }

    private func ack(timeout: TimeInterval) async throws -> G2CfwTransport.Ack {
        guard await link.waitUntil(timeout: timeout, { !self.watcher.acks.isEmpty }), let a = watcher.acks.first else { throw SuiteError.failed("no ACK/NACK in \(timeout) s") }
        return a
    }

    /// The firmware answers a handled message with REPLY then ACK; wait for
    /// both so a trailing ACK never counts for the next case.
    private func cacheReply(timeout: TimeInterval) async throws -> G2CfwTransport.CacheReply {
        guard await link.waitUntil(timeout: timeout, { !self.watcher.cacheReplies.isEmpty }), let r = watcher.cacheReplies.first
        else { throw SuiteError.failed("no REPLY in \(timeout) s" + (watcher.acks.first.map { " (\($0.success ? "ACK" : "NACK") arrived)" } ?? "")) }
        guard await link.waitUntil(timeout: timeout, { !self.watcher.acks.isEmpty }) else { throw SuiteError.failed("REPLY without ACK") }
        return r
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

    private func control(_ c: RayNeoIOCfwLink.Control) {
        link.sendFrame(business: RayNeoIOBusiness.cfw, payload: RayNeoIOCfwLink.controlPacket(c))
    }

    /// Frame one L2 message as a record (the phone's encoder) and stream it.
    private func sendMessage(_ message: Data, compress: Bool) throws {
        guard let record = encoder.record(message, targets: RayNeoIOCfwLink.lens, allLenses: RayNeoIOCfwLink.lens, compress: compress)
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
        var r = Data([17, 0, 0x0F, 0])
        for v in [x, y, w, h] {
            r.append(le16(v))
        }
        r.append(UInt8(bytes.count))
        r.append(contentsOf: bytes)
        return r
    }

    func le16(_ v: Int16) -> Data {
        let u = UInt16(bitPattern: v)
        return Data([UInt8(u & 0xFF), UInt8(u >> 8)])
    }

    func le32(_ v: UInt32) -> Data {
        Data([UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF), UInt8((v >> 16) & 0xFF), UInt8(v >> 24)])
    }
}
