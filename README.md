# react-native-nitro-pdf-writer

Fast PDF generation for React Native, powered by [libharu](https://github.com/libharu/libharu) and [react-native-nitro-modules](https://github.com/mrousavy/react-native-nitro-modules).

This library exposes libharu's PDF creation API to JavaScript/TypeScript through Nitro Modules, keeping all rendering work off the UI thread and guarding libharu with a global lock because libharu itself is **not thread-safe**.

## Why libharu?

Built-in alternatives such as Android's `android.graphics.pdf.PdfDocument` and iOS `CPDFDocument` / `UIGraphicsPDFRenderer` are convenient for small documents, but they build the entire PDF structure in memory before it is written out.

For medium or large PDFs — for example reports with hundreds of pages, invoices with embedded images, or photo books — that approach causes memory usage to grow roughly in proportion to the number of pages. On a mobile device this can quickly lead to memory pressure, app slowdowns, and `OutOfMemoryError` / OOM crashes.

libharu takes a different approach:

- It writes PDF content incrementally and keeps only the currently needed objects in memory.
- Pages and streams can be flushed, so adding the 500th page does not keep the first 499 pages resident.
- Memory usage stays low and predictable even for very large documents.

This makes `react-native-nitro-pdf-writer` a safer choice when you need to generate multi-page or image-heavy PDFs on mobile hardware.

## Installation

```bash
npm install react-native-nitro-pdf-writer
# or
yarn add react-native-nitro-pdf-writer
```

You also need a compatible version of `react-native-nitro-modules` in your app:

```bash
npm install react-native-nitro-modules
```

### iOS

```bash
cd ios && pod install && cd ..
```

The podspec automatically downloads libharu during `pod install`.

### Android

No manual steps are required. Gradle runs `scripts/prepare-libharu.js` during the build to download libharu.

## Usage

```typescript
import {
  NitroPdfWriterInstance as pdf,
  HpdfPageSizes,
  HpdfPageDirection,
  HpdfCompressionMode,
} from 'react-native-nitro-pdf-writer';

async function createDocument() {
  const docHandle = await pdf.createDocument();

  await pdf.setCompressionMode(docHandle, HpdfCompressionMode.HPDF_COMP_ALL);

  const pageHandle = await pdf.addPage(docHandle);
  // Portrait A4 using the convenience helper
  await pdf.setPageSize(pageHandle, HpdfPageSizes.HPDF_PAGE_SIZE_A4);

  const fontHandle = await pdf.getFont(docHandle, 'Helvetica');
  await pdf.beginText(pageHandle);
  await pdf.setFontAndSize(pageHandle, fontHandle, 24);
  await pdf.textOut(pageHandle, 50, 700, 'Hello from react-native-nitro-pdf-writer!');
  await pdf.endText(pageHandle);

  const buffer = await pdf.saveToBuffer(docHandle);
  await pdf.freeDocument(docHandle);

  return buffer; // ArrayBuffer
}
```

### Save to a file path

```typescript
import RNFS from 'react-native-fs';

const path = RNFS.DocumentDirectoryPath + '/example.pdf';
await pdf.saveToFile(docHandle, path);
```

### Memory management

libharu objects are native resources. Call `freeDocument(handle)` when you are done to release memory and file handles.

## Units, coordinates, and page sizes

libharu uses **PDF points** as its native unit: **1 point = 1/72 inch**. All width, height, x, y, and radius values passed to the API are in points.

The page coordinate system starts at the **bottom-left corner** of the page:

- `x = 0` is the left edge.
- `y = 0` is the bottom edge.
- `y` increases upward, so `textOut(page, 50, 800, "...")` places text near the top of an A4 page.

For unit conversions, use the exported `PdfUnits` helper:

```typescript
import { PdfUnits, NitroPdfWriterInstance as pdf } from 'react-native-nitro-pdf-writer';

const leftMargin = PdfUnits.mmToPt(20);   // 20 mm
const topMargin = PdfUnits.cmToPt(2.5);   // 2.5 cm
const width = PdfUnits.pxToPt(1024, 300); // 1024 px @ 300 DPI
await pdf.textOut(page, leftMargin, topMargin, 'Hello');
```

- `mmToPt(value)` — millimeters to points
- `cmToPt(value)` — centimeters to points
- `inchToPt(value)` — inches to points
- `pxToPt(value, dpi = 72)` — pixels to points at the given DPI

For convenience the package exports a `PaperSize` object with common ISO and US sizes already converted to points:

```typescript
import { PaperSize, HpdfPageSizes } from 'react-native-nitro-pdf-writer';

// Use predefined libharu sizes with setPageSize / setSize
await pdf.setPageSize(pageHandle, HpdfPageSizes.HPDF_PAGE_SIZE_A4);
await pdf.setSize(pageHandle, HpdfPageSizes.HPDF_PAGE_SIZE_A4, HpdfPageDirection.HPDF_PAGE_LANDSCAPE);

// Or use exact point dimensions
await pdf.setPageWidth(pageHandle, PaperSize.A4.width);
await pdf.setPageHeight(pageHandle, PaperSize.A4.height);
```

Available `HpdfPageSizes` values: `LETTER`, `LEGAL`, `A3`, `A4`, `A5`, `B4`, `B5`, `EXECUTIVE`, `US4x6`, `US4x8`, `US5x7`, `COMM10`.

`PaperSize` includes additional helpers such as `A0`, `A1`, `A2`, `A6`, `B3`, `Letter`, `Legal`, and `Executive`.

## Colors

All color values in libharu are **normalized to the range 0.0 – 1.0**, not 0 – 255.

```typescript
await pdf.setRGBFill(pageHandle, 0.9, 0.2, 0.2); // light red
await pdf.setGrayFill(pageHandle, 0.5);          // 50% gray
await pdf.setCMYKFill(pageHandle, 0.0, 1.0, 1.0, 0.0); // red in CMYK
```

Passing a value outside `0..1` raises `HPDF_PAGE_OUT_OF_RANGE`.

## Fonts

libharu does not have a default font. You must obtain a font handle and set it on the page **before** calling `textOut`, `textRect`, `measureText`, or similar text methods.

```typescript
const fontHandle = await pdf.getFont(docHandle, 'Helvetica');
await pdf.setFontAndSize(pageHandle, fontHandle, 24);
```

Built-in (base-14) font names include: `Courier`, `Courier-Bold`, `Courier-Oblique`, `Courier-BoldOblique`, `Helvetica`, `Helvetica-Bold`, `Helvetica-Oblique`, `Helvetica-BoldOblique`, `Times-Roman`, `Times-Bold`, `Times-Italic`, `Times-BoldItalic`, `Symbol`, and `ZapfDingbats`.

You can also load custom fonts from files:

```typescript
const ttFont = await pdf.loadTTFontFromFile(docHandle, '/path/to/font.ttf', true);
await pdf.setFontAndSize(pageHandle, ttFont, 24);
```

## Images

Only **JPEG** and **PNG** images are natively supported. Other formats (WebP, GIF, BMP, HEIF, etc.) must be decoded to raw pixel data (RGB/RGBA) by the caller before passing them to the library.

### Vector graphics

> ⚠️ Important limitation:
This library **does NOT support direct‑import of any external vector files**.

Common vector format SVG cannot be loaded directly.
- If you need to embed SVG into PDF: you must rasterize SVG to PNG/JPG bitmap **on the JS‑side first**.
- After rasterization the graphic will become a bitmap inside PDF (it loses vector property; zoom‑in will produce jaggy/blurry result).

Other vector formats (wmf/emf etc.) are also not supported.
> Professional source files such as ai, eps, cdr, dwg are out‑of‑scope and not supported.

If you truly want true vector graphics inside output PDF, you need to use the drawing path APIs to construct shapes programmatically.

```typescript
// PNG or JPEG — load directly from file
const img = await pdf.loadPngImageFromFile(docHandle, '/path/to/photo.png');
const img2 = await pdf.loadJpegImageFromFile(docHandle, '/path/to/photo.jpg');
await pdf.drawImage(pageHandle, img, 50, 500, 200, 150);

// PNG — load from memory buffer (e.g. fetched from network)
const imgFromBuf = await pdf.loadPngImageFromBuffer(docHandle, pngArrayBuffer);
await pdf.drawImage(pageHandle, imgFromBuf, 50, 300, 200, 150);

// Other formats — decode to raw pixels yourself, then use drawRawImage
// (RGB, RGBA, or grayscale, 8 bits per component)
const rawPixels: ArrayBuffer = /* decoded pixel data */;
await pdf.drawRawImage(
  pageHandle, rawPixels,
  1024, 768,             // pixel dimensions
  0,                     // HPDF_CS_DEVICE_RGB
  50, 100, 300, 225,     // x, y, drawWidth, drawHeight (PDF points)
);
```

## Error handling

When libharu fails, the rejected `Promise` carries a readable error that includes the libharu constant name and a short explanation. For example:

```
HPDF_PAGE_FONT_NOT_FOUND (0x104e): No font is set for the page. Call setFontAndSize() before drawing text.
```

Common errors you may see:

- `HPDF_PAGE_FONT_NOT_FOUND` — you called a text method without `setFontAndSize()`.
- `HPDF_PAGE_INVALID_GMODE` — you called `fill()`/`stroke()` without first creating a path, or you called `textOut()` outside `beginText()`/`endText()`.
- `HPDF_PAGE_OUT_OF_RANGE` — a numeric value is out of range, most often an RGB component greater than `1.0`.
- `HPDF_ITEM_NOT_FOUND` — a required object or resource is missing, often an invalid handle.

## Threading and safety

- Every libharu operation runs on a dedicated background thread via `HaruWorker`.
- A global `std::mutex` (`HaruLock`) serializes all libharu calls, because libharu is not thread-safe.
- The JavaScript side receives promises and never blocks the React Native UI thread.

## Constants

The package re-exports libharu enums under friendly TypeScript names:

- `HpdfPageSizes`
- `HpdfPageDirection`
- `HpdfLineCap`
- `HpdfLineJoin`
- `HpdfColorSpace`
- `HpdfTextRenderingMode`
- `HpdfBlendMode`
- `HpdfCompressionMode`
- `HpdfEncryptionMode`
- `HpdfInfoType`
- `HpdfAlign`

Import them together with the writer instance:

```typescript
import {
  NitroPdfWriterInstance,
  HpdfPageSizes,
} from 'react-native-nitro-pdf-writer';
```

## API overview

The module exposes libharu's core methods, including but not limited to:

- Document creation: `createDocument`, `freeDocument`, `hasDoc`, `setCompressionMode`, `setInfoAttr`
- Pages: `addPage`, `insertPage`, `setPageSize`, `setSize`, `setPageWidth`, `setPageHeight`, `setRotate`, `getWidth`, `getHeight`
- Fonts: `getFont`, `getFont2`, `loadTypeFontFromFile`, `loadTypeFontFromFile2`, `setCurrentFont`, `getFontName`
- Text: `beginText`, `endText`, `textOut`, `textRect`, `showText`, `showTextNextLine`, `textWidth`
- Graphics: `moveTo`, `lineTo`, `curveTo`, `rectangle`, `circle`, `ellipse`, `stroke`, `fill`, `setLineWidth`, `setRGBFill`, `setRGBStroke`
- Images: `loadPngImageFromFile`, `loadJpegImageFromFile`, `loadRawImageFromBuffer`, `drawRawImage`, `drawImage`
- Output: `saveToBuffer`, `saveToFile`

All methods are asynchronous and return `Promise<T>`.

## Running tests

```bash
# TypeScript / Jest tests
yarn test

# C++ unit tests
yarn test:cpp
```

## Dependencies

- [libharu](https://github.com/libharu/libharu) — open-source PDF generation library
- [libpng](http://www.libpng.org/pub/png/libpng.html) — PNG image support (bundled for both iOS and Android)
- [react-native-nitro-modules](https://github.com/mrousavy/react-native-nitro-modules) — fast native bindings for React Native

## Resources

- [libharu API Reference](http://libharu.org/) — official documentation for the underlying C library

## License

MIT
