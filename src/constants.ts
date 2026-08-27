/**
 * libHaru constants mirrored from hpdf_consts.h.
 * Keep these in sync with the native libharu headers.
 */

export enum HpdfPageSizes {
  HPDF_PAGE_SIZE_LETTER = 0,
  HPDF_PAGE_SIZE_LEGAL,
  HPDF_PAGE_SIZE_A3,
  HPDF_PAGE_SIZE_A4,
  HPDF_PAGE_SIZE_A5,
  HPDF_PAGE_SIZE_B4,
  HPDF_PAGE_SIZE_B5,
  HPDF_PAGE_SIZE_EXECUTIVE,
  HPDF_PAGE_SIZE_US4x6,
  HPDF_PAGE_SIZE_US4x8,
  HPDF_PAGE_SIZE_US5x7,
  HPDF_PAGE_SIZE_COMM10,
  HPDF_PAGE_SIZE_EOF,
}

export enum HpdfPageDirection {
  HPDF_PAGE_PORTRAIT = 0,
  HPDF_PAGE_LANDSCAPE = 1,
}

/**
 * Common paper sizes in PDF points (1/72 inch).
 * Use these with setPageWidth/setPageHeight, or use HpdfPageSizes with setPageSize/setSize.
 */
export const PaperSize = {
  A0: { width: 2383.94, height: 3370.39 },
  A1: { width: 1683.78, height: 2383.94 },
  A2: { width: 1190.55, height: 1683.78 },
  A3: { width: 841.89, height: 1190.55 },
  A4: { width: 595.28, height: 841.89 },
  A5: { width: 419.53, height: 595.28 },
  A6: { width: 297.64, height: 419.53 },
  B3: { width: 1000.63, height: 1417.32 },
  B4: { width: 708.66, height: 1000.63 },
  B5: { width: 498.9, height: 708.66 },
  Letter: { width: 612, height: 792 },
  Legal: { width: 612, height: 1008 },
  Executive: { width: 522, height: 756 },
} as const;

export enum HpdfLineCap {
  HPDF_BUTT_END = 0,
  HPDF_ROUND_END,
  HPDF_PROJECTING_SQUARE_END,
}

export enum HpdfLineJoin {
  HPDF_MITER_JOIN = 0,
  HPDF_ROUND_JOIN,
  HPDF_BEVEL_JOIN,
}

export enum HpdfTextRenderingMode {
  HPDF_FILL = 0,
  HPDF_STROKE,
  HPDF_FILL_THEN_STROKE,
  HPDF_INVISIBLE,
  HPDF_FILL_CLIPPING,
  HPDF_STROKE_CLIPPING,
  HPDF_FILL_STROKE_CLIPPING,
  HPDF_CLIPPING,
}

export enum HpdfColorSpace {
  HPDF_CS_DEVICE_GRAY = 0,
  HPDF_CS_DEVICE_RGB,
  HPDF_CS_DEVICE_CMYK,
  HPDF_CS_CAL_GRAY,
  HPDF_CS_CAL_RGB,
  HPDF_CS_LAB,
  HPDF_CS_ICC_BASED,
  HPDF_CS_SEPARATION,
  HPDF_CS_DEVICE_N,
  HPDF_CS_INDEXED,
  HPDF_CS_PATTERN,
  HPDF_CS_EOF,
}

export enum HpdfCompressionMode {
  HPDF_COMP_NONE = 0,
  HPDF_COMP_TEXT = 1,
  HPDF_COMP_IMAGE = 2,
  HPDF_COMP_METADATA = 4,
  HPDF_COMP_ALL = 15,
}

export enum HpdfEncryptionMode {
  HPDF_ENCRYPT_R2 = 2,
  HPDF_ENCRYPT_R3 = 3,
}

export enum HpdfInfoType {
  HPDF_INFO_CREATION_DATE = 0,
  HPDF_INFO_MOD_DATE,
  HPDF_INFO_AUTHOR,
  HPDF_INFO_CREATOR,
  HPDF_INFO_PRODUCER,
  HPDF_INFO_TITLE,
  HPDF_INFO_SUBJECT,
  HPDF_INFO_KEYWORDS,
}

export enum HpdfAlign {
  HPDF_TALIGN_LEFT = 0,
  HPDF_TALIGN_RIGHT,
  HPDF_TALIGN_CENTER,
  HPDF_TALIGN_JUSTIFY,
}

export enum HpdfBlendMode {
  HPDF_BM_NORMAL = 0,
  HPDF_BM_MULTIPLY,
  HPDF_BM_SCREEN,
  HPDF_BM_OVERLAY,
  HPDF_BM_DARKEN,
  HPDF_BM_LIGHTEN,
  HPDF_BM_COLOR_DODGE,
  HPDF_BM_COLOR_BUM,
  HPDF_BM_HARD_LIGHT,
  HPDF_BM_SOFT_LIGHT,
  HPDF_BM_DIFFERENCE,
  HPDF_BM_EXCLUSHON,
}
