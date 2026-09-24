/**
 * PDF drawing interfaces and types for high-level PDF generation.
 */

export type Unit = 'mm' | 'cm' | 'in' | 'px' | 'pt';

export interface Font {
  /** Font family / PostScript name (e.g. `'Helvetica'`). Alias: `family`. */
  name?: string;
  /** Preferred alias for `name`. Provide one of `family` | `name` | `filePath` | `media`. */
  family?: string;
  size: number;
  unit?: Unit;
  filePath?: string;
  /**
   * Media handle from `loadFontFromFile` / `loadFontFromBuffer`.
   * Preferred over `filePath` when the font was pre-loaded (and optionally cached).
   */
  media?: number;
  encoding?: string;
  fontIndex?: number;
  /**
   * Embed the font in the PDF (custom TTF/TTC only).
   * Defaults: quickDraw `font`/`text.font` and `attachFont` → true;
   * one-step `loadTTFontFromFile` / `loadTTFontFromFile2` → false.
   */
  embedding?: boolean;
}

export interface Color {
  r?: number;
  g?: number;
  b?: number;
  gray?: number;
  c?: number;
  m?: number;
  y?: number;
  k?: number;
}

export interface Text {
  content: string;
  x: number;
  y: number;
  width?: number;
  height?: number;
  unit?: Unit;
  /** `media` / `filePath` / `name` — see Font fields. */
  font?: Font;
  color?: Color;
  align?: 'left' | 'right' | 'center' | 'justify';
}

export interface Image {
  /**
   * Path to the image file. Omit when `media` is set.
   * For binary data use `loadImageFromBuffer` + `media` (or the one-step sugar APIs).
   */
  source?: string;
  /**
   * Media handle from `loadImageFromFile` / `loadImageFromBuffer`.
   * Preferred when the image was pre-loaded / cached and reused across documents.
   */
  media?: number;
  x: number;
  y: number;
  width: number;
  height: number;
  unit?: Unit;
  /** Force image format. If omitted, detected from file extension. */
  format?: 'png' | 'jpeg';
  /**
   * Share the loaded image bytes through the media cache (default false).
   * Only applies when loading from `source`; ignored when `media` is set.
   * Prefer leaving this off unless you actually reuse the source.
   */
  useCache?: boolean;
}

export interface Line {
  x1: number;
  y1: number;
  x2: number;
  y2: number;
  unit?: Unit;
  lineWidth?: number;
  lineCap?: 'butt' | 'round' | 'projectingSquare';
  lineJoin?: 'miter' | 'round' | 'bevel';
  color?: Color;
}

export interface Rectangle {
  x: number;
  y: number;
  width: number;
  height: number;
  unit?: Unit;
  lineWidth?: number;
  fillColor?: Color;
  strokeColor?: Color;
  fill?: boolean;
  stroke?: boolean;
}

export interface Circle {
  x: number;
  y: number;
  radius: number;
  unit?: Unit;
  lineWidth?: number;
  fillColor?: Color;
  strokeColor?: Color;
  fill?: boolean;
  stroke?: boolean;
}

export interface Ellipse {
  x: number;
  y: number;
  xRadius: number;
  yRadius: number;
  unit?: Unit;
  lineWidth?: number;
  fillColor?: Color;
  strokeColor?: Color;
  fill?: boolean;
  stroke?: boolean;
}

export interface Path {
  points: Array<{ x: number; y: number }>;
  unit?: Unit;
  lineWidth?: number;
  color?: Color;
  close?: boolean;
  fill?: boolean;
  stroke?: boolean;
}

export type Operation =
  | { type: 'text'; data: Text }
  | { type: 'image'; data: Image }
  | { type: 'line'; data: Line }
  | { type: 'rectangle'; data: Rectangle }
  | { type: 'circle'; data: Circle }
  | { type: 'ellipse'; data: Ellipse }
  | { type: 'path'; data: Path }
  | { type: 'font'; data: Font }
  | { type: 'color'; data: Color; target: 'fill' | 'stroke' | 'both' }
  | { type: 'page'; width?: number; height?: number; unit?: Unit; size?: string; direction?: 'portrait' | 'landscape' }
  | { type: 'rotate'; angle: number }
  | { type: 'gSave' }
  | { type: 'gRestore' }
  | { type: 'transform'; a: number; b: number; c: number; d: number; x: number; y: number; unit?: Unit };

export interface DrawOptions {
  operations: Operation[];
  unit?: Unit;
  output?: string;
}
