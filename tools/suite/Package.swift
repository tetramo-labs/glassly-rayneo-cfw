// swift-tools-version: 5.9
import PackageDescription

// On-device test suite for the Glassly CFW. The protocol, flasher and CFW
// transport are the Glassly App SDK's own sources, and the BLE driver is the
// SDK's Mac harness, both reached through symlinks into a sibling `glassly`
// checkout (../../../../glassly). See README.md.
let package = Package(
  name: "rayneo-io-suite",
  platforms: [
    .macOS(.v14)
  ],
  targets: [
    .executableTarget(
      name: "rayneo-io-suite",
      path: "Sources/rayneo-io-suite",
      linkerSettings: [
        .linkedFramework("CoreBluetooth")
      ]
    )
  ]
)
