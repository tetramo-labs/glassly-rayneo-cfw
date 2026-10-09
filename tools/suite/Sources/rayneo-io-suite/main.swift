//
//  main.swift
//
//  rayneo-io-suite: the on-device acceptance run for the Glassly CFW, in the
//  spirit of the G2 shapes/vector suites. Flash first, then:
//
//    swift run rayneo-io-suite                 # every case, in order
//    swift run rayneo-io-suite cfw page        # by group
//    swift run rayneo-io-suite ident stale     # by id (connect is implied)
//    swift run rayneo-io-suite --list
//
//  Options: --name <BLE name>  --verbose  --hold S (stability seconds, 15)
//           --revision N (expected minimum, default the SDK's)  --out results.json
//

import Foundation

var filters: [String] = []
var name: String?
var verbose = false
var hold: TimeInterval = 15
var revision = RayNeoIOFirmwareCatalog.cfwRevision
var out: String?
var list = false

var argv = Array(CommandLine.arguments.dropFirst())
var i = 0
while i < argv.count {
    let a = argv[i]
    func value() -> String {
        i += 1
        return i < argv.count ? argv[i] : ""
    }
    switch a {
    case "--name": name = value()
    case "--verbose", "-v": verbose = true
    case "--hold": hold = TimeInterval(value()) ?? hold
    case "--revision": revision = Int(value()) ?? revision
    case "--out": out = value()
    case "--list": list = true
    case "--help", "-h":
        print("usage: rayneo-io-suite [ids or groups…] [--list] [--name N] [--verbose] [--hold S] [--revision N] [--out FILE]")
        exit(0)
    default: filters.append(a)
    }
    i += 1
}

Task { @MainActor in
    var linkOptions = Link.Options()
    linkOptions.name = name
    linkOptions.verbose = verbose
    let link = Link(options: linkOptions)
    let watcher = CfwWatcher(link: link)
    link.onFrame = { watcher.handle($0) }
    var suiteOptions = Suite.Options()
    suiteOptions.hold = hold
    suiteOptions.expectedRevision = revision
    let suite = Suite(link: link, watcher: watcher, options: suiteOptions)

    if list {
        for c in suite.cases {
            print(String(format: "%-12@ %-6@ %@", c.id as NSString, c.group as NSString, c.title as NSString))
        }
        exit(0)
    }
    // A filtered run still has to connect first.
    if !filters.isEmpty, !filters.contains("connect"), !filters.contains("link") {
        filters.insert("connect", at: 0)
    }
    let ok = await suite.run(only: filters)
    link.disconnect()

    let passed = suite.results.filter(\.passed).count
    print("\n\(passed)/\(suite.results.count) passed")
    if let out {
        let rows = suite.results.map { r -> [String: Any] in
            ["id": r.id, "group": r.group, "passed": r.passed, "detail": r.detail, "seconds": r.seconds]
        }
        let doc: [String: Any] = ["date": ISO8601DateFormatter().string(from: Date()), "results": rows, "passed": passed, "total": suite.results.count]
        if let data = try? JSONSerialization.data(withJSONObject: doc, options: [.prettyPrinted, .sortedKeys]) {
            try? data.write(to: URL(fileURLWithPath: out))
            print("wrote \(out)")
        }
    }
    exit(ok ? 0 : 1)
}

dispatchMain()
