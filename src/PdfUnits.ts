/**
 * PDF uses points (1/72 inch) as its native unit.
 * These helpers convert common units to PDF points.
 */
export class PdfUnits {
  /** Convert millimeters to PDF points. */
  static mmToPt(mm: number): number {
    return mm * (72.0 / 25.4);
  }

  /** Convert centimeters to PDF points. */
  static cmToPt(cm: number): number {
    return cm * (72.0 / 2.54);
  }

  /** Convert inches to PDF points. */
  static inchToPt(inch: number): number {
    return inch * 72.0;
  }

  /**
   * Convert pixels to PDF points using the specified DPI.
   * Defaults to 72 DPI (1:1 with points).
   */
  static pxToPt(px: number, dpi: number = 72): number {
    return (px / dpi) * 72.0;
  }
}
