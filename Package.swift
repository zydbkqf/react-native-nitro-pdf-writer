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
        // thirdparty/ is vendored down to the files the build actually compiles
        // (see thirdparty/README.md), so nothing here needs a directory exclude
        // beyond the docs that must not become SwiftPM sources.
        "thirdparty/libpng/README",
        "thirdparty/libharu/README.md",
      ],
      sources: [
        "cpp",
        "ios",
        "thirdparty/libharu/src",
        // Explicit list instead of "thirdparty/libpng": SwiftPM collects that
        // directory recursively, subdirectories included.
        "thirdparty/libpng/png.c",
        "thirdparty/libpng/pngerror.c",
        "thirdparty/libpng/pngget.c",
        "thirdparty/libpng/pngmem.c",
        "thirdparty/libpng/pngpread.c",
        "thirdparty/libpng/pngread.c",
        "thirdparty/libpng/pngrio.c",
        "thirdparty/libpng/pngrtran.c",
        "thirdparty/libpng/pngrutil.c",
        "thirdparty/libpng/pngset.c",
        "thirdparty/libpng/pngtrans.c",
        "thirdparty/libpng/pngwio.c",
        "thirdparty/libpng/pngwrite.c",
        "thirdparty/libpng/pngwtran.c",
        "thirdparty/libpng/pngwutil.c",
        "nitrogen/generated/shared/c++",
        "nitrogen/generated/ios/c++",
        "nitrogen/generated/ios/swift",
      ],
      publicHeadersPath: "ios",
      cSettings: [
        .headerSearchPath("cpp"),
        .headerSearchPath("thirdparty/libharu/include"),
        .headerSearchPath("thirdparty/libpng"),
        .headerSearchPath("nitrogen/generated/shared/c++"),
        .headerSearchPath("nitrogen/generated/ios/c++"),
        .define("HPDF_NOPACKPROTOCOL", to: "1"),
        .define("PNG_ARM_NEON_OPT", to: "0"),
      ],
      cxxSettings: [
        .headerSearchPath("cpp"),
        .headerSearchPath("thirdparty/libharu/include"),
        .headerSearchPath("thirdparty/libpng"),
        .headerSearchPath("nitrogen/generated/shared/c++"),
        .headerSearchPath("nitrogen/generated/ios/c++"),
        .define("HPDF_NOPACKPROTOCOL", to: "1"),
        .define("PNG_ARM_NEON_OPT", to: "0"),
      ],
      linkerSettings: [
        .linkedLibrary("z"),
        // Platform image decoding for HEIC/WebP/AVIF (see ios/NitroPdfWriterImageTranscoder.mm).
        .linkedFramework("ImageIO"),
        .linkedFramework("CoreGraphics"),
        // Used only to bake EXIF orientation when ImageIO falls back off the
        // thumbnail path.
        .linkedFramework("CoreImage"),
      ]
    ),
  ]
)
