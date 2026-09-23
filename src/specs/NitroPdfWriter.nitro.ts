import type { HybridObject } from 'react-native-nitro-modules';

/**
 * NitroPdfWriter exposes libHaru functionality through Nitro Modules.
 * All methods return Promises so that work can be off-loaded from the UI thread.
 * libHaru itself is not thread-safe; the native implementation serializes
 * every call behind a single global lock and a dedicated worker thread.
 */
export interface NitroPdfWriter extends HybridObject<{ ios: 'c++'; android: 'c++' }> {
  // ---------------------------------------------------------------------------
  // Document lifecycle
  // ---------------------------------------------------------------------------
  createDocument(): Promise<number>;
  freeDocument(doc: number): Promise<void>;
  saveToFile(doc: number, path: string): Promise<void>;
  saveToBuffer(doc: number): Promise<ArrayBuffer>;
  hasDoc(doc: number): Promise<boolean>;

  // ---------------------------------------------------------------------------
  // Error handling
  // ---------------------------------------------------------------------------
  getError(doc: number): Promise<number>;
  getErrorDetail(doc: number): Promise<number>;
  resetError(doc: number): Promise<void>;

  // ---------------------------------------------------------------------------
  // Document properties / compression / encryption
  // ---------------------------------------------------------------------------
  setCompressionMode(doc: number, mode: number): Promise<void>;
  setPassword(doc: number, ownerPassword: string, userPassword: string): Promise<void>;
  setPermission(doc: number, permission: number): Promise<void>;
  setEncryptionMode(doc: number, mode: number, keyLen: number): Promise<void>;

  // ---------------------------------------------------------------------------
  // Pages
  // ---------------------------------------------------------------------------
  addPage(doc: number): Promise<number>;
  getCurrentPage(doc: number): Promise<number>;
  setCurrentPage(doc: number, page: number): Promise<void>;
  insertPage(doc: number, page: number): Promise<number>;
  getPageByIndex(doc: number, index: number): Promise<number>;

  // ---------------------------------------------------------------------------
  // Page sizes
  // ---------------------------------------------------------------------------
  setPageWidth(page: number, width: number): Promise<void>;
  setPageHeight(page: number, height: number): Promise<void>;
  setPageSize(page: number, size: number): Promise<void>;
  setSize(page: number, size: number, direction: number): Promise<void>;
  setRotate(page: number, angle: number): Promise<void>;

  // ---------------------------------------------------------------------------
  // Fonts
  // ---------------------------------------------------------------------------
  getFont(doc: number, fontName: string, encodingName?: string): Promise<number>;
  loadType1FontFromFile(doc: number, afmPath: string, pfmPath?: string): Promise<number>;
  loadTTFontFromFile(doc: number, fileName: string, embedding?: boolean): Promise<number>;
  loadTTFontFromFile2(doc: number, fileName: string, index: number, embedding?: boolean): Promise<number>;
  setCurrentFont(doc: number, font: number): Promise<void>;
  setFontAndSize(page: number, font: number, size: number): Promise<void>;
  getFontName(font: number): Promise<string>;
  measureText(page: number, text: string, width: number, wordwrap?: boolean): Promise<number>;

  // ---------------------------------------------------------------------------
  // Text state
  // ---------------------------------------------------------------------------
  beginText(page: number): Promise<void>;
  endText(page: number): Promise<void>;
  textOut(page: number, x: number, y: number, text: string): Promise<void>;
  textRect(
    page: number,
    left: number,
    top: number,
    right: number,
    bottom: number,
    text: string,
    align: number,
  ): Promise<void>;
  setTextLeading(page: number, leading: number): Promise<void>;
  setTextRenderingMode(page: number, mode: number): Promise<void>;
  setTextRise(page: number, rise: number): Promise<void>;
  setCharSpace(page: number, space: number): Promise<void>;
  setWordSpace(page: number, space: number): Promise<void>;
  setHorizontalScalling(page: number, scale: number): Promise<void>;

  // ---------------------------------------------------------------------------
  // Text transformation
  // ---------------------------------------------------------------------------
  moveTextPos(page: number, x: number, y: number): Promise<void>;
  moveTextPos2(page: number, x: number, y: number): Promise<void>;
  setTextMatrix(page: number, a: number, b: number, c: number, d: number, x: number, y: number): Promise<void>;

  // ---------------------------------------------------------------------------
  // Graphics state
  // ---------------------------------------------------------------------------
  setLineWidth(page: number, width: number): Promise<void>;
  setLineCap(page: number, cap: number): Promise<void>;
  setLineJoin(page: number, join: number): Promise<void>;
  setMiterLimit(page: number, miterLimit: number): Promise<void>;
  setDash(page: number, dashPattern: number[], phase: number): Promise<void>;
  setFlat(page: number, flatness: number): Promise<void>;
  setExtGState(page: number, extGState: number): Promise<void>;

  // ---------------------------------------------------------------------------
  // Colors
  // ---------------------------------------------------------------------------
  setRGBFill(page: number, r: number, g: number, b: number): Promise<void>;
  setRGBStroke(page: number, r: number, g: number, b: number): Promise<void>;
  setCMYKFill(page: number, c: number, m: number, y: number, k: number): Promise<void>;
  setCMYKStroke(page: number, c: number, m: number, y: number, k: number): Promise<void>;
  setGrayFill(page: number, gray: number): Promise<void>;
  setGrayStroke(page: number, gray: number): Promise<void>;

  // ---------------------------------------------------------------------------
  // Path construction
  // ---------------------------------------------------------------------------
  moveTo(page: number, x: number, y: number): Promise<void>;
  lineTo(page: number, x: number, y: number): Promise<void>;
  curveTo(
    page: number,
    x1: number,
    y1: number,
    x2: number,
    y2: number,
    x3: number,
    y3: number,
  ): Promise<void>;
  curveTo2(page: number, x2: number, y2: number, x3: number, y3: number): Promise<void>;
  curveTo3(page: number, x1: number, y1: number, x3: number, y3: number): Promise<void>;
  closePath(page: number): Promise<void>;
  rectangle(page: number, x: number, y: number, width: number, height: number): Promise<void>;
  arc(page: number, x: number, y: number, radius: number, angle1: number, angle2: number): Promise<void>;
  ellipse(page: number, x: number, y: number, xRadius: number, yRadius: number): Promise<void>;
  circle(page: number, x: number, y: number, radius: number): Promise<void>;

  // ---------------------------------------------------------------------------
  // Path painting
  // ---------------------------------------------------------------------------
  stroke(page: number): Promise<void>;
  closePathStroke(page: number): Promise<void>;
  fill(page: number): Promise<void>;
  eofill(page: number): Promise<void>;
  fillStroke(page: number): Promise<void>;
  eofillStroke(page: number): Promise<void>;
  closePathFillStroke(page: number): Promise<void>;
  closePathEofillStroke(page: number): Promise<void>;
  endPath(page: number): Promise<void>;

  // ---------------------------------------------------------------------------
  // Images
  // ---------------------------------------------------------------------------
  loadPngImageFromFile(doc: number, fileName: string): Promise<number>;
  loadPngImageFromBuffer(doc: number, buffer: ArrayBuffer): Promise<number>;
  loadJpegImageFromFile(doc: number, fileName: string): Promise<number>;
  loadRawImageFromFile(
    doc: number,
    fileName: string,
    width: number,
    height: number,
    colorSpace: number,
  ): Promise<number>;
  loadRawImageFromBuffer(
    doc: number,
    buffer: ArrayBuffer,
    width: number,
    height: number,
    colorSpace: number,
  ): Promise<number>;
  setImageMask(image: number, mask: number): Promise<void>;
  drawImage(page: number, image: number, x: number, y: number, width: number, height: number): Promise<void>;
  /**
   * Convenience: load raw pixel data and draw it onto the page in one call.
   * Equivalent to loadRawImageFromBuffer() + drawImage() but avoids exposing
   * the intermediate image handle.
   *
   * @param page       Page handle.
   * @param buffer     Raw pixel data (RGB, RGBA, or grayscale, 8 bits per component).
   * @param width      Image width in pixels.
   * @param height     Image height in pixels.
   * @param colorSpace HPDF_ColorSpace value (e.g. HPDF_CS_DEVICE_RGB = 0).
   * @param x          X position on the page (PDF points).
   * @param y          Y position on the page (PDF points).
   * @param drawWidth  Width to draw on the page (PDF points).
   * @param drawHeight Height to draw on the page (PDF points).
   */
  drawRawImage(
    page: number,
    buffer: ArrayBuffer,
    width: number,
    height: number,
    colorSpace: number,
    x: number,
    y: number,
    drawWidth: number,
    drawHeight: number,
  ): Promise<void>;

  // ---------------------------------------------------------------------------
  // Coordinate transforms
  // ---------------------------------------------------------------------------
  gSave(page: number): Promise<void>;
  gRestore(page: number): Promise<void>;
  concat(page: number, a: number, b: number, c: number, d: number, x: number, y: number): Promise<void>;

  // ---------------------------------------------------------------------------
  // Annotations
  // ---------------------------------------------------------------------------
  createTextAnnot(page: number, rect: number[], text: string, encoder?: string): Promise<number>;
  createLinkAnnot(page: number, rect: number[], dst: number): Promise<number>;
  createURILinkAnnot(page: number, rect: number[], uri: string): Promise<number>;

  // ---------------------------------------------------------------------------
  // Destinations
  // ---------------------------------------------------------------------------
  createDestination(page: number): Promise<number>;
  setDestinationXYZ(dst: number, x: number, y: number, zoom: number): Promise<void>;
  setDestinationFit(dst: number): Promise<void>;
  setDestinationFitH(dst: number, top: number): Promise<void>;
  setDestinationFitV(dst: number, left: number): Promise<void>;
  setDestinationFitR(
    dst: number,
    left: number,
    bottom: number,
    right: number,
    top: number,
  ): Promise<void>;
  setDestinationFitB(dst: number): Promise<void>;
  setDestinationFitBH(dst: number, top: number): Promise<void>;
  setDestinationFitBV(dst: number, left: number): Promise<void>;

  // ---------------------------------------------------------------------------
  // Outlines
  // ---------------------------------------------------------------------------
  createOutline(doc: number, parent: number, title: string, encoder?: string): Promise<number>;
  setOpened(outline: number, opened: boolean): Promise<void>;

  // ---------------------------------------------------------------------------
  // External graphics state
  // ---------------------------------------------------------------------------
  createExtGState(doc: number): Promise<number>;
  setAlphaStroke(extGState: number, alpha: number): Promise<void>;
  setAlphaFill(extGState: number, alpha: number): Promise<void>;
  setBlendMode(extGState: number, mode: number): Promise<void>;

  // ---------------------------------------------------------------------------
  // Document info
  // ---------------------------------------------------------------------------
  setInfoAttr(doc: number, infoType: number, value: string): Promise<void>;
  getInfoAttr(doc: number, infoType: number): Promise<string>;
  setInfoDateAttr(doc: number, infoType: number, value: string): Promise<void>;

  // ---------------------------------------------------------------------------
  // Utility
  // ---------------------------------------------------------------------------
  pageTextWidth(page: number, text: string): Promise<number>;
  pageTextHeight(page: number, text: string): Promise<number>;
  pageMeasureText(page: number, text: string, width: number, wordwrap?: boolean): Promise<number>;
}
