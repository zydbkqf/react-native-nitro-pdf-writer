import { HpdfAlign, HpdfBlendMode, HpdfColorSpace, HpdfCompressionMode, HpdfEncryptionMode, HpdfInfoType, HpdfLineCap, HpdfLineJoin, HpdfPageDirection, HpdfPageSizes, HpdfTextRenderingMode, PaperSize } from '../constants';

describe('libharu constants', () => {
  test('page sizes start at 0 and increment', () => {
    expect(HpdfPageSizes.HPDF_PAGE_SIZE_LETTER).toBe(0);
    expect(HpdfPageSizes.HPDF_PAGE_SIZE_A4).toBe(3);
    expect(HpdfPageSizes.HPDF_PAGE_SIZE_EOF).toBeGreaterThan(0);
  });

  test('page directions are preserved', () => {
    expect(HpdfPageDirection.HPDF_PAGE_PORTRAIT).toBe(0);
    expect(HpdfPageDirection.HPDF_PAGE_LANDSCAPE).toBe(1);
  });

  test('line caps and joins match libharu values', () => {
    expect(HpdfLineCap.HPDF_BUTT_END).toBe(0);
    expect(HpdfLineCap.HPDF_PROJECTING_SQUARE_END).toBe(2);
    expect(HpdfLineJoin.HPDF_BEVEL_JOIN).toBe(2);
  });

  test('color spaces and compression modes are exported', () => {
    expect(HpdfColorSpace.HPDF_CS_DEVICE_GRAY).toBe(0);
    expect(HpdfColorSpace.HPDF_CS_DEVICE_CMYK).toBe(2);
    expect(HpdfCompressionMode.HPDF_COMP_ALL).toBe(15);
  });

  test('encryption, info, align and blend enums are defined', () => {
    expect(HpdfEncryptionMode.HPDF_ENCRYPT_R2).toBe(2);
    expect(HpdfEncryptionMode.HPDF_ENCRYPT_R3).toBe(3);
    expect(HpdfInfoType.HPDF_INFO_AUTHOR).toBe(2);
    expect(HpdfAlign.HPDF_TALIGN_CENTER).toBe(2);
    expect(HpdfBlendMode.HPDF_BM_NORMAL).toBe(0);
    expect(HpdfBlendMode.HPDF_BM_EXCLUSHON).toBe(11);
  });

  test('text rendering modes are ordered', () => {
    expect(HpdfTextRenderingMode.HPDF_FILL).toBe(0);
    expect(HpdfTextRenderingMode.HPDF_CLIPPING).toBe(7);
  });

  test('PaperSize helpers are defined in points', () => {
    expect(PaperSize.A4.width).toBeCloseTo(595.28, 1);
    expect(PaperSize.A4.height).toBeCloseTo(841.89, 1);
    expect(PaperSize.B3.width).toBeGreaterThan(PaperSize.A4.width);
    expect(PaperSize.Letter.width).toBe(612);
  });
});
