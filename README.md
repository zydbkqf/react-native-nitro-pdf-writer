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

## Platform requirements

| | Minimum |
|---|---------|
| React Native | 0.78+ |
| iOS | 15.1+ |
| Android | API 24+ (minSdk 24) |
| Node.js | 18+ |

## Installation

```bash
npm install react-native-nitro-pdf-writer react-native-nitro-modules
# or
yarn add react-native-nitro-pdf-writer react-native-nitro-modules
```

### iOS

```bash
cd ios && pod install && cd ..
```

The podspec automatically downloads libharu during `pod install`.

### Android

No manual steps are required. Gradle runs `scripts/prepare-libharu.js` during the build to download libharu.

If you use ProGuard/R8, no extra rules are needed — the library is pure C++ with Nitro bindings.

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

## Quick Draw API

The `quickDraw` function provides a high-level, declarative API for generating PDFs. It supports both single document and batch generation modes, with automatic unit conversion.

### Single Document

```typescript
import { quickDraw } from 'react-native-nitro-pdf-writer';

const filePath = await quickDraw({
  unit: 'mm',
  output: '/path/to/output.pdf',
  operations: [
    { type: 'page', width: 210, height: 297 }, // A4 size
    {
      type: 'text',
      data: {
        content: 'Hello World',
        x: 20,
        y: 270,
        font: { name: 'Helvetica', size: 24 },
        color: { r: 0, g: 0, b: 0 },
      },
    },
    {
      type: 'rectangle',
      data: {
        x: 20,
        y: 240,
        width: 170,
        height: 20,
        fillColor: { r: 0.2, g: 0.6, b: 1.0 },
        fill: true,
        stroke: false,
      },
    },
  ],
});
// Returns: '/path/to/output.pdf'
```

If `output` is not provided, `quickDraw` returns a document handle (number) that you can use with the low-level API. Remember to call `freeDocument(handle)` when done.

```typescript
const docHandle = await quickDraw({
  operations: [
    { type: 'text', data: { content: 'Hello', x: 50, y: 700 } },
  ],
});
// Returns: document handle (number)
```

### Batch Generation

Pass an array of `DrawOptions` to generate multiple PDFs in a single call. This is more efficient than calling `quickDraw` multiple times because it shares the same native lock acquisition.

```typescript
const results = await quickDraw([
  {
    output: '/path/to/invoice1.pdf',
    unit: 'mm',
    operations: [
      { type: 'text', data: { content: 'Invoice #1', x: 20, y: 270, font: { name: 'Helvetica-Bold', size: 18 } } },
    ],
  },
  {
    output: '/path/to/invoice2.pdf',
    unit: 'cm',
    operations: [
      { type: 'text', data: { content: 'Invoice #2', x: 2, y: 27, font: { name: 'Helvetica', size: 14 } } },
    ],
  },
]);
// Returns: ['/path/to/invoice1.pdf', '/path/to/invoice2.pdf']
```

### Unit Conversion

All dimension values support automatic unit conversion. Specify the `unit` field to use:

| Unit | Description |
|------|-------------|
| `'mm'` | millimeters |
| `'cm'` | centimeters |
| `'in'` | inches |
| `'px'` | pixels (at 72 DPI by default, configurable via the `dpi` parameter) |
| `'pt'` | PDF points (default, 1/72 inch) |

```typescript
await quickDraw({
  unit: 'mm',
  operations: [
    { type: 'page', width: 210, height: 297 },
    { type: 'text', data: { content: 'Hello', x: 20, y: 270, font: { name: 'Helvetica', size: 12 } } },
  ],
});
```

### Error Handling in Batch Mode

Batch generation continues even if individual documents fail. Errors are returned as strings in the results array alongside successful file paths:

```typescript
const results = await quickDraw([
  { output: '/valid/path.pdf', operations: [...] },
  { output: '/invalid/path.pdf', operations: [...] },
]);

// results[0] = '/valid/path.pdf'   (success — a file path)
// results[1] = 'Error message...'  (failure — an error string)
```

To distinguish success from failure, check whether each result looks like a valid output path or starts with an error indicator (`"Missing"`, `"Failed"`, `"Invalid"`).

---

## Quick Draw Operation Reference

Each entry in the `operations` array is a discriminated union keyed by `type`. All coordinates use the unit specified in the top-level `unit` field (default `'pt'`).

> **Coordinate system**: Quick Draw uses the same **bottom-left origin** as the low-level API. `y = 0` is the bottom edge; `y` increases upward.

### `page`

Add a new page. If neither `width`/`height` nor `size` is provided, the default is A4 portrait.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `width` | `number` | A4 width | Custom page width |
| `height` | `number` | A4 height | Custom page height |
| `unit` | `Unit` | top-level `unit` | Unit for width/height |
| `size` | `string` | — | Named size: `'letter'`, `'legal'`, `'a3'`, `'a4'`, `'a5'`, `'b4'`, `'b5'` |
| `direction` | `'portrait' \| 'landscape'` | `'portrait'` | Page orientation |

### `text`

Draw text on the page. If `width` **and** `height` are both provided, text wraps inside the rectangle (via `textRect`). Otherwise, single-line text is drawn at `(x, y)` (via `textOut`).

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `content` | `string` | ✓ | Text string to draw |
| `x` | `number` | ✓ | X position |
| `y` | `number` | ✓ | Y position (baseline for `textOut`) |
| `width` | `number` | — | Bounding box width (enables textRect) |
| `height` | `number` | — | Bounding box height (enables textRect) |
| `unit` | `Unit` | — | Unit for x/y/width/height |
| `font` | `Font` | — | Font settings (see Font fields below) |
| `color` | `Color` | — | Text fill color |
| `align` | `'left' \| 'right' \| 'center' \| 'justify'` | `'left'` | Alignment (textRect only) |

#### Font fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | `string` | ✓* | Font name (e.g. `'Helvetica'`, `'Times-Roman'`) |
| `size` | `number` | ✓ | Font size |
| `unit` | `Unit` | — | Unit for size |
| `filePath` | `string` | — | Path to a custom TTF file (alternative to `name`) |
| `encoding` | `string` | — | Character encoding |
| `fontIndex` | `number` | `0` | Font index in a TTC collection |

*Either `name` or `filePath` is required.

#### Color fields

Provide **one** of the following:

| Mode | Fields | Range |
|------|--------|-------|
| RGB | `r`, `g`, `b` | 0.0 – 1.0 |
| CMYK | `c`, `m`, `y`, `k` | 0.0 – 1.0 |
| Grayscale | `gray` | 0.0 – 1.0 |

### `image`

Draw a PNG or JPEG image from a file path.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `source` | `string` | ✓ | Local file path to the image |
| `x` | `number` | ✓ | X position |
| `y` | `number` | ✓ | Y position |
| `width` | `number` | ✓ | Draw width on the page |
| `height` | `number` | ✓ | Draw height on the page |
| `unit` | `Unit` | — | Unit for x/y/width/height |
| `format` | `'png' \| 'jpeg'` | auto | Force format; auto-detected from file extension if omitted |

For binary image data (e.g. from a network request), use `loadPngImageFromBuffer` or `loadRawImageFromBuffer` from the low-level API instead.

### `line`

Draw a line between two points.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `x1`, `y1` | `number` | ✓ | Start point |
| `x2`, `y2` | `number` | ✓ | End point |
| `unit` | `Unit` | top-level `unit` | Unit for coordinates |
| `lineWidth` | `number` | `1.0` | Stroke width |
| `lineCap` | `'butt' \| 'round' \| 'projectingSquare'` | `'butt'` | Line cap style |
| `lineJoin` | `'miter' \| 'round' \| 'bevel'` | `'miter'` | Line join style |
| `color` | `Color` | black | Stroke color |

### `rectangle`

Draw a rectangle.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `x`, `y` | `number` | ✓ | Bottom-left corner |
| `width`, `height` | `number` | ✓ | Dimensions |
| `unit` | `Unit` | top-level `unit` | Unit |
| `lineWidth` | `number` | `1.0` | Stroke width |
| `fillColor` | `Color` | — | Fill color |
| `strokeColor` | `Color` | — | Stroke color |
| `fill` | `boolean` | `false` | Whether to fill |
| `stroke` | `boolean` | `true` | Whether to stroke |

> If you set `fillColor`, also set `fill: true` to actually paint the fill.

### `circle`

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `x`, `y` | `number` | ✓ | Center point |
| `radius` | `number` | ✓ | Radius |
| `unit` | `Unit` | top-level `unit` | Unit |
| `lineWidth` | `number` | `1.0` | Stroke width |
| `fillColor` | `Color` | — | Fill color |
| `strokeColor` | `Color` | — | Stroke color |
| `fill` | `boolean` | `false` | Whether to fill |
| `stroke` | `boolean` | `true` | Whether to stroke |

### `ellipse`

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `x`, `y` | `number` | ✓ | Center point |
| `xRadius` | `number` | ✓ | Horizontal radius |
| `yRadius` | `number` | ✓ | Vertical radius |
| `unit` | `Unit` | top-level `unit` | Unit |
| `lineWidth` | `number` | `1.0` | Stroke width |
| `fillColor` | `Color` | — | Fill color |
| `strokeColor` | `Color` | — | Stroke color |
| `fill` | `boolean` | `false` | Whether to fill |
| `stroke` | `boolean` | `true` | Whether to stroke |

### `path`

Draw a custom polyline/polygon path.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `points` | `Array<{x, y}>` | ✓ | Path vertices |
| `unit` | `Unit` | top-level `unit` | Unit for coordinates |
| `lineWidth` | `number` | `1.0` | Stroke width |
| `color` | `Color` | black | Stroke/fill color |
| `close` | `boolean` | `false` | Close the path (last point → first) |
| `fill` | `boolean` | `false` | Whether to fill |
| `stroke` | `boolean` | `true` | Whether to stroke |

### `font`

Set the current font for subsequent text operations.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | `string` | ✓* | Base-14 font name or loaded custom font name |
| `size` | `number` | ✓ | Font size |
| `filePath` | `string` | — | Path to TTF/TTC file (alternative to `name`) |
| `fontIndex` | `number` | `0` | Font index in a TTC collection |
| `encoding` | `string` | — | Character encoding |

### `color`

Set fill/stroke color for subsequent drawing operations.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `target` | `'fill' \| 'stroke' \| 'both'` | `'both'` | Which color to set |
| `r`, `g`, `b` | `number` | ✓** | RGB color (0.0 – 1.0) |
| `gray` | `number` | ✓** | Grayscale (0.0 – 1.0) |
| `c`, `m`, `y`, `k` | `number` | ✓** | CMYK color (0.0 – 1.0) |

**Provide one color mode.

### `rotate`

Set page rotation angle (0, 90, 180, or 270).

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `angle` | `number` | ✓ | Rotation angle in degrees |

### `gSave` / `gRestore`

Save/restore the graphics state (color, line width, transformation matrix, etc.). Useful for isolating drawing styles.

### `transform`

Apply a coordinate transformation matrix.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `a`, `b`, `c`, `d` | `number` | ✓ | Matrix coefficients |
| `x`, `y` | `number` | ✓ | Translation |
| `unit` | `Unit` | — | Unit for x/y translation |

---

## Units, coordinates, and page sizes

libharu uses **PDF points** as its native unit: **1 point = 1/72 inch**. All width, height, x, y, and radius values passed to the low-level API are in points.

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

| Method | Description |
|--------|-------------|
| `mmToPt(value)` | Millimeters to points |
| `cmToPt(value)` | Centimeters to points |
| `inchToPt(value)` | Inches to points |
| `pxToPt(value, dpi = 72)` | Pixels to points at the given DPI |

For convenience the package exports a `PaperSize` object with common ISO and US sizes already converted to points:

```typescript
import { PaperSize, HpdfPageSizes } from 'react-native-nitro-pdf-writer';

// Use predefined libharu sizes
await pdf.setPageSize(pageHandle, HpdfPageSizes.HPDF_PAGE_SIZE_A4);
await pdf.setSize(pageHandle, HpdfPageSizes.HPDF_PAGE_SIZE_A4, HpdfPageDirection.HPDF_PAGE_LANDSCAPE);

// Or use exact point dimensions
await pdf.setPageWidth(pageHandle, PaperSize.A4.width);
await pdf.setPageHeight(pageHandle, PaperSize.A4.height);
```

Available `HpdfPageSizes` values: `LETTER`, `LEGAL`, `A3`, `A4`, `A5`, `B4`, `B5`, `EXECUTIVE`, `US4x6`, `US4x8`, `US5x7`, `COMM10`.

`PaperSize` includes: `A0`, `A1`, `A2`, `A3`, `A4`, `A5`, `A6`, `B3`, `B4`, `B5`, `Letter`, `Legal`, `Executive`. Each has `{ width, height }` in PDF points.

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

Built-in (base-14) font names: `Courier`, `Courier-Bold`, `Courier-Oblique`, `Courier-BoldOblique`, `Helvetica`, `Helvetica-Bold`, `Helvetica-Oblique`, `Helvetica-BoldOblique`, `Times-Roman`, `Times-Bold`, `Times-Italic`, `Times-BoldItalic`, `Symbol`, `ZapfDingbats`.

You can also load custom fonts from files:

```typescript
// Load TTF font
const ttFont = await pdf.loadTTFontFromFile(docHandle, '/path/to/font.ttf', true);
await pdf.setFontAndSize(pageHandle, ttFont, 24);

// Load TTC (TrueType Collection) with a specific font index
const ttcFont = await pdf.loadTTFontFromFile2(docHandle, '/path/to/NotoSansCJK.ttc', 0, true);
await pdf.setFontAndSize(pageHandle, ttcFont, 18);

// Load Type 1 font (AFM + optional PFM)
const type1Font = await pdf.loadType1FontFromFile(docHandle, '/path/to/font.afm', '/path/to/font.pfm');
```

> ⚠️ **OTF fonts are not supported.** libharu only handles TrueType outlines. OTF fonts with CFF outlines (e.g. Source Han Sans `.otf`) must be converted to TTF first, or use the TTF/TTC variant (e.g. Noto Sans CJK `.ttc`).

> ⚠️ **Variable Fonts (`.ttf` with variable axes) are not supported.** Use a static-weight TTF/TTC instead.

> ⚠️ **TTC files** contain multiple fonts in one file. Use `loadTTFontFromFile2` with a `fontIndex` (0-based) to select which font to load.

> ⚠️ Font files must be **local file paths**. Remote URLs are **not supported**.

## Images

Only **JPEG** and **PNG** images are natively supported. Other formats (WebP, GIF, BMP, HEIF, etc.) must be decoded to raw pixel data by the caller before passing them to the library via `loadRawImageFromBuffer`.

> ⚠️ **Important**: Image files must be **local file paths**. Remote URLs are **not supported**.

- For native-loaded JPEG: CMYK JPEG may be preserved when the policy allows (for printing use-case).
- `loadRawImageFromBuffer` accepts RGB, Gray, or CMYK pixel buffers **without alpha**. If your source has an alpha channel (RGBA), separate the RGB data and alpha mask, then call `setImageMask` to attach the alpha mask manually.

### Vector graphics

> ⚠️ **This library does NOT support direct import of any external vector files.**

SVG cannot be loaded directly. To embed SVG, rasterize it to PNG/JPG on the JS side first. The result is a bitmap (zooming produces jagged/blurry output). Other vector formats (WMF, EMF, AI, EPS, CDR, DWG) are also not supported.

For true vector graphics, use the path drawing APIs (`moveTo`/`lineTo`/`curveTo`/`closePath` + `stroke`/`fill`) to construct shapes programmatically.

```typescript
// PNG or JPEG — load from file
const img = await pdf.loadPngImageFromFile(docHandle, '/path/to/photo.png');
const img2 = await pdf.loadJpegImageFromFile(docHandle, '/path/to/photo.jpg');
await pdf.drawImage(pageHandle, img, 50, 500, 200, 150);

// PNG — load from memory buffer (e.g. fetched from network)
const imgFromBuf = await pdf.loadPngImageFromBuffer(docHandle, pngArrayBuffer);
await pdf.drawImage(pageHandle, imgFromBuf, 50, 300, 200, 150);

// Other formats — decode to raw pixels yourself, then use drawRawImage
const rawPixels: ArrayBuffer = /* decoded pixel data */;
await pdf.drawRawImage(
  pageHandle, rawPixels,
  1024, 768,             // pixel dimensions
  0,                     // HPDF_CS_DEVICE_RGB
  50, 100, 300, 225,     // x, y, drawWidth, drawHeight (PDF points)
);
```

## Annotations

Annotations add interactive elements (notes, links) to a page. The `rect` parameter is `[left, top, right, bottom]` in PDF points.

```typescript
// Text annotation (sticky note)
await pdf.createTextAnnot(pageHandle, [50, 750, 250, 700], 'Review this section');

// Link to another page (destination)
const dest = await pdf.createDestination(targetPageHandle);
await pdf.setDestinationXYZ(dest, 0, 800, 0); // x, y, zoom (0 = keep current)
await pdf.createLinkAnnot(pageHandle, [50, 700, 200, 680], dest);

// URI link
await pdf.createURILinkAnnot(pageHandle, [50, 650, 250, 630], 'https://example.com');
```

## Destinations

Destinations control where a PDF viewer jumps to when following a link or outline entry.

```typescript
const dest = await pdf.createDestination(pageHandle);

await pdf.setDestinationXYZ(dest, 0, 800, 0);        // x, y, zoom
await pdf.setDestinationFit(dest);                    // fit entire page
await pdf.setDestinationFitH(dest, 750);              // fit width, top at 750
await pdf.setDestinationFitV(dest, 50);               // fit height, left at 50
await pdf.setDestinationFitR(dest, 50, 700, 400, 800); // fit rectangle
await pdf.setDestinationFitB(dest);                   // fit bounding box
await pdf.setDestinationFitBH(dest, 750);             // fit bbox height, top at 750
await pdf.setDestinationFitBV(dest, 50);              // fit bbox width, left at 50
```

## Outlines (Bookmarks)

Outlines create the bookmark sidebar in a PDF viewer.

```typescript
const root = await pdf.createOutline(docHandle, 0, 'Chapter 1'); // parent=0 for root
const child = await pdf.createOutline(docHandle, root, 'Section 1.1');
await pdf.setOpened(root, true); // expanded by default in viewer
```

- `parent = 0` creates a root-level outline.
- `parent = outlineHandle` creates a nested outline.
- An invalid `parent` handle throws an error.

## External Graphics State (ExtGState)

ExtGState controls transparency and blend modes.

```typescript
const gs = await pdf.createExtGState(docHandle);
await pdf.setAlphaFill(gs, 0.5);       // 50% fill opacity
await pdf.setAlphaStroke(gs, 0.8);     // 80% stroke opacity
await pdf.setBlendMode(gs, 0);         // Normal blend mode

await pdf.setExtGState(pageHandle, gs);
// All subsequent drawing uses these settings until changed
```

## Document Metadata

```typescript
import { HpdfInfoType } from 'react-native-nitro-pdf-writer';

await pdf.setInfoAttr(docHandle, HpdfInfoType.HPDF_INFO_TITLE, 'My Report');
await pdf.setInfoAttr(docHandle, HpdfInfoType.HPDF_INFO_AUTHOR, 'Jane Doe');
await pdf.setInfoAttr(docHandle, HpdfInfoType.HPDF_INFO_CREATOR, 'My App');

const title = await pdf.getInfoAttr(docHandle, HpdfInfoType.HPDF_INFO_TITLE);

// Dates use 'YYYY-MM-DD HH:MM:SS' format
await pdf.setInfoDateAttr(docHandle, HpdfInfoType.HPDF_INFO_CREATION_DATE, '2025-01-15 10:30:00');
```

Available `HpdfInfoType` values: `TITLE`, `AUTHOR`, `SUBJECT`, `KEYWORDS`, `CREATOR`, `PRODUCER`, `CREATION_DATE`, `MOD_DATE`.

## Password & Encryption

```typescript
await pdf.setPassword(docHandle, 'ownerPassword', 'userPassword');
await pdf.setPermission(docHandle, /* permission flags */ 4); // e.g. HPDF_ENABLE_PRINT
await pdf.setEncryptionMode(docHandle, /* mode */ 0, /* keyLen */ 40); // RC4, 40-bit
```

Set these **before** calling `saveToFile` or `saveToBuffer`.

## Error Handling

When libharu fails, the rejected `Promise` carries a readable error that includes the libharu constant name and a short explanation:

```
HPDF_PAGE_FONT_NOT_FOUND (0x104e): No font is set for the page. Call setFontAndSize() before drawing text.
```

Common errors:

| Error | Cause |
|-------|-------|
| `HPDF_PAGE_FONT_NOT_FOUND` | Text method called without `setFontAndSize()` |
| `HPDF_PAGE_INVALID_GMODE` | `fill()`/`stroke()` without a path, or `textOut()` outside `beginText()`/`endText()` |
| `HPDF_PAGE_OUT_OF_RANGE` | Numeric value out of range (e.g. RGB > 1.0) |
| `HPDF_ITEM_NOT_FOUND` | Invalid or stale handle |
| `HPDF_INVALID_DOCUMENT` | Document was already freed or never created |

For advanced error introspection:

```typescript
const errorCode = await pdf.getError(docHandle);
const detail = await pdf.getErrorDetail(docHandle);
await pdf.resetError(docHandle); // clear the error state
```

## Threading and Safety

- Every libharu operation runs on a dedicated background thread via `HaruWorker`.
- A global `std::mutex` (`HaruLock`) serializes all libharu calls, because libharu is not thread-safe.
- The JavaScript side receives promises and never blocks the React Native UI thread.
- Concurrent calls from JS are safe — they are queued and serialized natively.

## Memory Management

libharu objects are native resources. Always call `freeDocument(handle)` when done to release memory and file handles. All child handles (pages, fonts, images, destinations, outlines, annotations) are automatically invalidated when the document is freed.

```typescript
const docHandle = await pdf.createDocument();
try {
  // ... build the PDF ...
  await pdf.saveToFile(docHandle, '/path/to/output.pdf');
} finally {
  await pdf.freeDocument(docHandle);
}
```

## TypeScript Types

The package exports TypeScript interfaces for all Quick Draw operations:

```typescript
import type {
  Operation,    // Union of all operation types
  DrawOptions,  // { operations, unit?, output? }
  Font,
  Color,
  Text,
  Image,
  Line,
  Rectangle,
  Circle,
  Ellipse,
  Path,
  Unit,
} from 'react-native-nitro-pdf-writer';
```

Use these to type your operation arrays for editor autocompletion and compile-time safety:

```typescript
import { quickDraw, type Operation } from 'react-native-nitro-pdf-writer';

const operations: Operation[] = [
  { type: 'page', width: 210, height: 297 },
  { type: 'text', data: { content: 'Hello', x: 20, y: 270 } },
];

await quickDraw({ unit: 'mm', operations });
```

## API Reference

All methods return `Promise<T>`.

### Document Lifecycle

| Method | Signature |
|--------|-----------|
| `createDocument` | `() → Promise<number>` |
| `freeDocument` | `(doc: number) → Promise<void>` |
| `saveToFile` | `(doc: number, path: string) → Promise<void>` |
| `saveToBuffer` | `(doc: number) → Promise<ArrayBuffer>` |
| `hasDoc` | `(doc: number) → Promise<boolean>` |

### Error Handling

| Method | Signature |
|--------|-----------|
| `getError` | `(doc: number) → Promise<number>` |
| `getErrorDetail` | `(doc: number) → Promise<number>` |
| `resetError` | `(doc: number) → Promise<void>` |

### Document Properties

| Method | Signature |
|--------|-----------|
| `setCompressionMode` | `(doc: number, mode: number) → Promise<void>` |
| `setPassword` | `(doc: number, ownerPassword: string, userPassword: string) → Promise<void>` |
| `setPermission` | `(doc: number, permission: number) → Promise<void>` |
| `setEncryptionMode` | `(doc: number, mode: number, keyLen: number) → Promise<void>` |

### Pages

| Method | Signature |
|--------|-----------|
| `addPage` | `(doc: number) → Promise<number>` |
| `insertPage` | `(doc: number, page: number) → Promise<number>` |
| `getCurrentPage` | `(doc: number) → Promise<number>` |
| `setCurrentPage` | `(doc: number, page: number) → Promise<void>` |
| `getPageByIndex` | `(doc: number, index: number) → Promise<number>` |

### Page Sizes

| Method | Signature |
|--------|-----------|
| `setPageWidth` | `(page: number, width: number) → Promise<void>` |
| `setPageHeight` | `(page: number, height: number) → Promise<void>` |
| `setPageSize` | `(page: number, size: number) → Promise<void>` |
| `setSize` | `(page: number, size: number, direction: number) → Promise<void>` |
| `setRotate` | `(page: number, angle: number) → Promise<void>` |
| `getWidth` | `(page: number) → Promise<number>` |
| `getHeight` | `(page: number) → Promise<number>` |

### Fonts

| Method | Signature |
|--------|-----------|
| `getFont` | `(doc: number, fontName: string, encodingName?: string) → Promise<number>` |
| `getFont2` | `(doc: number, fontName: string, encodingName?: string) → Promise<number>` |
| `loadType1FontFromFile` | `(doc: number, afmPath: string, pfmPath?: string) → Promise<number>` |
| `loadTTFontFromFile` | `(doc: number, fileName: string, embedding?: boolean) → Promise<number>` |
| `loadTTFontFromFile2` | `(doc: number, fileName: string, index: number, embedding?: boolean) → Promise<number>` |
| `setFontAndSize` | `(page: number, font: number, size: number) → Promise<void>` |
| `getFontName` | `(font: number) → Promise<string>` |
| ~~`setCurrentFont`~~ | *Deprecated — throws. Use `setFontAndSize` instead.* |

### Text

| Method | Signature |
|--------|-----------|
| `beginText` | `(page: number) → Promise<void>` |
| `endText` | `(page: number) → Promise<void>` |
| `textOut` | `(page: number, x: number, y: number, text: string) → Promise<void>` |
| `textRect` | `(page: number, left: number, top: number, right: number, bottom: number, text: string, align: number) → Promise<void>` |
| `measureText` | `(page: number, text: string, width: number, wordwrap?: boolean) → Promise<number>` |

### Text State

| Method | Signature |
|--------|-----------|
| `setTextLeading` | `(page: number, leading: number) → Promise<void>` |
| `setTextRenderingMode` | `(page: number, mode: number) → Promise<void>` |
| `setTextRise` | `(page: number, rise: number) → Promise<void>` |
| `setCharSpace` | `(page: number, space: number) → Promise<void>` |
| `setWordSpace` | `(page: number, space: number) → Promise<void>` |
| `setHorizontalScalling` | `(page: number, scale: number) → Promise<void>` |

### Text Transformation

| Method | Signature |
|--------|-----------|
| `moveTextPos` | `(page: number, x: number, y: number) → Promise<void>` |
| `moveTextPos2` | `(page: number, x: number, y: number) → Promise<void>` |
| `setTextMatrix` | `(page: number, a: number, b: number, c: number, d: number, x: number, y: number) → Promise<void>` |

### Graphics State

| Method | Signature |
|--------|-----------|
| `setLineWidth` | `(page: number, width: number) → Promise<void>` |
| `setLineCap` | `(page: number, cap: number) → Promise<void>` |
| `setLineJoin` | `(page: number, join: number) → Promise<void>` |
| `setMiterLimit` | `(page: number, miterLimit: number) → Promise<void>` |
| `setDash` | `(page: number, dashPattern: number[], phase: number) → Promise<void>` |
| `setFlat` | `(page: number, flatness: number) → Promise<void>` |
| `setExtGState` | `(page: number, extGState: number) → Promise<void>` |
| `gSave` | `(page: number) → Promise<void>` |
| `gRestore` | `(page: number) → Promise<void>` |
| `concat` | `(page: number, a: number, b: number, c: number, d: number, x: number, y: number) → Promise<void>` |

### Colors

| Method | Signature |
|--------|-----------|
| `setRGBFill` | `(page: number, r: number, g: number, b: number) → Promise<void>` |
| `setRGBStroke` | `(page: number, r: number, g: number, b: number) → Promise<void>` |
| `setCMYKFill` | `(page: number, c: number, m: number, y: number, k: number) → Promise<void>` |
| `setCMYKStroke` | `(page: number, c: number, m: number, y: number, k: number) → Promise<void>` |
| `setGrayFill` | `(page: number, gray: number) → Promise<void>` |
| `setGrayStroke` | `(page: number, gray: number) → Promise<void>` |

### Path Construction

| Method | Signature |
|--------|-----------|
| `moveTo` | `(page: number, x: number, y: number) → Promise<void>` |
| `lineTo` | `(page: number, x: number, y: number) → Promise<void>` |
| `curveTo` | `(page: number, x1, y1, x2, y2, x3, y3) → Promise<void>` |
| `curveTo2` | `(page: number, x2, y2, x3, y3) → Promise<void>` |
| `curveTo3` | `(page: number, x1, y1, x3, y3) → Promise<void>` |
| `closePath` | `(page: number) → Promise<void>` |
| `rectangle` | `(page: number, x, y, width, height) → Promise<void>` |
| `arc` | `(page: number, x, y, radius, angle1, angle2) → Promise<void>` |
| `circle` | `(page: number, x, y, radius) → Promise<void>` |
| `ellipse` | `(page: number, x, y, xRadius, yRadius) → Promise<void>` |

### Path Painting

| Method | Signature |
|--------|-----------|
| `stroke` | `(page: number) → Promise<void>` |
| `closePathStroke` | `(page: number) → Promise<void>` |
| `fill` | `(page: number) → Promise<void>` |
| `eofill` | `(page: number) → Promise<void>` |
| `fillStroke` | `(page: number) → Promise<void>` |
| `eofillStroke` | `(page: number) → Promise<void>` |
| `closePathFillStroke` | `(page: number) → Promise<void>` |
| `closePathEofillStroke` | `(page: number) → Promise<void>` |
| `endPath` | `(page: number) → Promise<void>` |

### Images

| Method | Signature |
|--------|-----------|
| `loadPngImageFromFile` | `(doc: number, fileName: string) → Promise<number>` |
| `loadPngImageFromBuffer` | `(doc: number, buffer: ArrayBuffer) → Promise<number>` |
| `loadJpegImageFromFile` | `(doc: number, fileName: string) → Promise<number>` |
| `loadRawImageFromFile` | `(doc: number, fileName: string, width, height, colorSpace) → Promise<number>` |
| `loadRawImageFromBuffer` | `(doc: number, buffer: ArrayBuffer, width, height, colorSpace) → Promise<number>` |
| `setImageMask` | `(image: number, mask: number) → Promise<void>` |
| `drawImage` | `(page: number, image: number, x, y, width, height) → Promise<void>` |
| `drawRawImage` | `(page: number, buffer: ArrayBuffer, width, height, colorSpace, x, y, drawWidth, drawHeight) → Promise<void>` |

### Annotations

| Method | Signature |
|--------|-----------|
| `createTextAnnot` | `(page: number, rect: [number,number,number,number], text: string, encoder?: string) → Promise<number>` |
| `createLinkAnnot` | `(page: number, rect: [number,number,number,number], dst: number) → Promise<number>` |
| `createURILinkAnnot` | `(page: number, rect: [number,number,number,number], uri: string) → Promise<number>` |

### Destinations

| Method | Signature |
|--------|-----------|
| `createDestination` | `(page: number) → Promise<number>` |
| `setDestinationXYZ` | `(dst: number, x, y, zoom) → Promise<void>` |
| `setDestinationFit` | `(dst: number) → Promise<void>` |
| `setDestinationFitH` | `(dst: number, top: number) → Promise<void>` |
| `setDestinationFitV` | `(dst: number, left: number) → Promise<void>` |
| `setDestinationFitR` | `(dst: number, left, bottom, right, top) → Promise<void>` |
| `setDestinationFitB` | `(dst: number) → Promise<void>` |
| `setDestinationFitBH` | `(dst: number, top: number) → Promise<void>` |
| `setDestinationFitBV` | `(dst: number, left: number) → Promise<void>` |

### Outlines

| Method | Signature |
|--------|-----------|
| `createOutline` | `(doc: number, parent: number, title: string, encoder?: string) → Promise<number>` |
| `setOpened` | `(outline: number, opened: boolean) → Promise<void>` |

### External Graphics State

| Method | Signature |
|--------|-----------|
| `createExtGState` | `(doc: number) → Promise<number>` |
| `setAlphaStroke` | `(extGState: number, alpha: number) → Promise<void>` |
| `setAlphaFill` | `(extGState: number, alpha: number) → Promise<void>` |
| `setBlendMode` | `(extGState: number, mode: number) → Promise<void>` |

### Document Metadata

| Method | Signature |
|--------|-----------|
| `setInfoAttr` | `(doc: number, infoType: number, value: string) → Promise<void>` |
| `getInfoAttr` | `(doc: number, infoType: number) → Promise<string>` |
| `setInfoDateAttr` | `(doc: number, infoType: number, value: string) → Promise<void>` |

### Utility

| Method | Signature |
|--------|-----------|
| `pageTextWidth` | `(page: number, text: string) → Promise<number>` |
| `pageTextHeight` | `(page: number, text: string) → Promise<number>` |
| `pageMeasureText` | `(page: number, text: string, width: number, wordwrap?: boolean) → Promise<number>` |

### Quick Draw

| Method | Signature |
|--------|-----------|
| `quickDraw` | `(operations: AnyMap[], unit: string, outputPath?: string, dpi?: number) → Promise<string \| number>` |
| `quickBatchDraw` | `(items: AnyMap[], dpi?: number) → Promise<(string \| number)[]>` |
| `yuv2rgb` | `(buffer: ArrayBuffer, width: number, height: number, format: 'NV12' \| 'NV21' \| 'I420' \| 'YUV420P') → Promise<ArrayBuffer>` |

## Constants

The package re-exports libharu enums under friendly TypeScript names:

| Enum | Description |
|------|-------------|
| `HpdfPageSizes` | Predefined page sizes |
| `HpdfPageDirection` | Portrait / Landscape |
| `HpdfLineCap` | Butt / Round / ProjectingSquare |
| `HpdfLineJoin` | Miter / Round / Bevel |
| `HpdfColorSpace` | DeviceGray / DeviceRGB / DeviceCMYK |
| `HpdfTextRenderingMode` | Fill / Stroke / FillStroke / Invisible |
| `HpdfBlendMode` | Normal, Multiply, Screen, Overlay, etc. |
| `HpdfCompressionMode` | None / Text / Image / All |
| `HpdfEncryptionMode` | RC4 |
| `HpdfInfoType` | Title / Author / Subject / etc. |
| `HpdfAlign` | Left / Right / Center / Justify |

```typescript
import {
  NitroPdfWriterInstance,
  HpdfPageSizes,
  HpdfLineCap,
  HpdfCompressionMode,
} from 'react-native-nitro-pdf-writer';
```

## YUV to RGB Conversion

The `yuv2rgb` method converts YUV image data to RGB format. Useful for camera frames or video data.

### Supported Formats

| Format | Layout |
|--------|--------|
| `NV12` | Y plane + interleaved UV plane (most common for camera/video) |
| `NV21` | Y plane + interleaved VU plane (common in Android camera) |
| `I420` / `YUV420P` | Y plane, then U plane, then V plane (planar) |

### Usage

```typescript
import { NitroPdfWriterInstance as pdf } from 'react-native-nitro-pdf-writer';

const yuvBuffer: ArrayBuffer = /* camera frame data */;
const rgbBuffer = await pdf.yuv2rgb(yuvBuffer, 1920, 1080, 'NV12');

const pageHandle = await pdf.addPage(docHandle);
await pdf.drawRawImage(
  pageHandle,
  rgbBuffer,
  1920, 1080,
  0,                     // HPDF_CS_DEVICE_RGB
  0, 0, 612, 792,
);

// Result is always RGB (3 bytes per pixel), size = width * height * 3
```

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
