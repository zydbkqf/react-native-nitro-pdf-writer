/**
 * PDF drawing interfaces and types for high-level PDF generation.
 */

export type Unit = 'mm' | 'cm' | 'in' | 'px' | 'pt';

export interface Font {
  name?: string;
  size: number;
  unit?: Unit;
  filePath?: string;
  encoding?: string;
  fontIndex?: number;
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
  font?: Font;
  color?: Color;
  align?: 'left' | 'right' | 'center' | 'justify';
}

export interface Image {
  /** Path to the image file. For binary data use loadPngImageFromBuffer/loadRawImageFromBuffer instead. */
  source: string;
  x: number;
  y: number;
  width: number;
  height: number;
  unit?: Unit;
  /** Force image format. If omitted, detected from file extension. */
  format?: 'png' | 'jpeg';
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
