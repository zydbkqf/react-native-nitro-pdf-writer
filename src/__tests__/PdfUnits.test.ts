import { PdfUnits } from '../PdfUnits';

describe('PdfUnits', () => {
  test('mmToPt uses 72/25.4', () => {
    expect(PdfUnits.mmToPt(25.4)).toBeCloseTo(72.0, 4);
    expect(PdfUnits.mmToPt(0)).toBe(0);
  });

  test('cmToPt uses 72/2.54', () => {
    expect(PdfUnits.cmToPt(2.54)).toBeCloseTo(72.0, 4);
  });

  test('inchToPt', () => {
    expect(PdfUnits.inchToPt(1)).toBe(72.0);
    expect(PdfUnits.inchToPt(0)).toBe(0);
  });

  test('pxToPt defaults to 72 dpi', () => {
    expect(PdfUnits.pxToPt(72)).toBe(72.0);
    expect(PdfUnits.pxToPt(144)).toBe(144.0);
  });

  test('pxToPt respects custom dpi', () => {
    expect(PdfUnits.pxToPt(300, 300)).toBe(72.0);
    expect(PdfUnits.pxToPt(600, 300)).toBe(144.0);
  });
});
