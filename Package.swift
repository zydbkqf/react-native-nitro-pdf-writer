// swift-tools-version:5.9
import PackageDescription

let package = Package(
  name: "NitroPdfWriter",
  platforms: [
    .iOS(.v15),
  ],
  products: [
    .library(
      name: "NitroPdfWriter",
      targets: ["NitroPdfWriter"]
    ),
  ],
  dependencies: [
    .package(url: "https://github.com/mrousavy/react-native-nitro-modules.git", from: "0.35.8"),
  ],
  targets: [
    .target(
      name: "NitroPdfWriter",
      dependencies: [
        .product(name: "NitroModules", package: "react-native-nitro-modules"),
      ],
      path: ".",
      exclude: [
        "scripts",
        "android",
        "src",
        "package.json",
        "tsconfig.json",
        "nitro.json",
        "NitroPdfWriter.podspec",
        "README.md",
        "LICENSE",
        "NOTICE",
      ],
      sources: [
        "cpp",
        "ios",
        "libharu/src",
        "nitrogen/generated/shared/c++",
        "nitrogen/generated/ios/c++",
        "nitrogen/generated/ios/swift",
      ],
      publicHeadersPath: "ios",
      cSettings: [
        .headerSearchPath("cpp"),
        .headerSearchPath("libharu/include"),
        .headerSearchPath("nitrogen/generated/shared/c++"),
        .headerSearchPath("nitrogen/generated/ios/c++"),
        .define("HPDF_NOPACKPROTOCOL", to: "1"),
      ],
      cxxSettings: [
        .headerSearchPath("cpp"),
        .headerSearchPath("libharu/include"),
        .headerSearchPath("nitrogen/generated/shared/c++"),
        .headerSearchPath("nitrogen/generated/ios/c++"),
        .define("HPDF_NOPACKPROTOCOL", to: "1"),
      ],
      linkerSettings: [
        .linkedLibrary("z"),
      ]
    ),
  ]
)
