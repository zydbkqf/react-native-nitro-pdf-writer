# Vendored dependencies

`libpng` 1.6.59 and `libharu` 2.4.6 are vendored here and compiled from source on
every platform (Android via `android/CMakeLists.txt`, iOS via
`NitroPdfWriter.podspec`, and SwiftPM via `Package.swift`). Nothing is fetched at
build time.

The trees are **pruned to the files the build actually uses**. The published npm
tarball has a hard size limit, and the upstream release tarballs are dominated by
test suites, contrib examples, autotools scaffolding and documentation that no
consumer needs.

## What is kept

`libpng/`:

- the 15 library sources (`png.c`, `pngerror.c`, `pngget.c`, `pngmem.c`,
  `pngpread.c`, `pngread.c`, `pngrio.c`, `pngrtran.c`, `pngrutil.c`, `pngset.c`,
  `pngtrans.c`, `pngwio.c`, `pngwrite.c`, `pngwtran.c`, `pngwutil.c`)
- the headers `png.h`, `pngconf.h`, `pnglibconf.h` (generated from
  `scripts/pnglibconf.h.prebuilt`), `pngpriv.h`, `pngstruct.h`, `pnginfo.h`,
  `pngdebug.h`
- `LICENSE`, `LICENSES`, `AUTHORS`, `README`

Removed: `contrib/`, `scripts/`, `ci/`, `projects/`, `tests/`, the arch-specific
directories (`arm/`, `intel/`, `mips/`, `powerpc/`, `riscv/`, `loongarch/`),
`pngtest.c`/`pngtest.png`, `example.c`, autotools files (`configure`,
`Makefile.*`, `ltmain.sh`, …), and the documentation (`CHANGES`,
`libpng-manual.txt`, `libpng.3`, …).

`libharu/`:

- `src/` (library sources and private headers) and `include/` (public headers,
  including the generated `hpdf_config.h`)
- `LICENSE`, `CHANGES`, `README.md`

Removed: `demo/`, `bindings/`, `doc/`, `cmake/`, `win32/`, `script/`,
`CMakeLists.txt`, `INSTALL`, `README_cmake`, `NOTICE`.

## Updating

`scripts/update-vendored-deps.js` does the whole flow: download, verify the
version reported by the extracted sources, regenerate `pnglibconf.h` /
`hpdf_config.h`, prune with the same keep-lists as above, and only then swap the
tree into place (a failed assertion restores the previous tree instead of leaving
an unverified checkout behind).
