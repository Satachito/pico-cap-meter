// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "CapMeter",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(name: "CapMeter", path: "Sources/CapMeter")
    ]
)
