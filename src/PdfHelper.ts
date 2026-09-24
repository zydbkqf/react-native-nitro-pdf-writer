import { NitroPdfWriterInstance } from './nitroInstance';
import type { DrawOptions, Operation } from './PdfTypes';

// Strip keys with undefined values so they are not serialized into AnyMap
function stripUndefined(obj: Record<string, any>): Record<string, any> {
  const out: Record<string, any> = {};
  for (const [k, v] of Object.entries(obj)) {
    if (v !== undefined) out[k] = v;
  }
  return out;
}

// Helper function to convert Operation to AnyMap format
function operationToAnyMap(op: Operation): Record<string, any> {
  const result: Record<string, any> = { type: op.type };

  if ('data' in op) {
    const data = stripUndefined(op.data as Record<string, any>);
    if (data.font && typeof data.font === 'object') {
      data.font = stripUndefined(data.font);
    }
    if (data.color && typeof data.color === 'object') {
      data.color = stripUndefined(data.color);
    }
    result.data = data;
  }
  if ('target' in op) {
    result.target = op.target;
  }
  if (op.type === 'page') {
    if (op.width !== undefined) result.width = op.width;
    if (op.height !== undefined) result.height = op.height;
    if (op.unit) result.unit = op.unit;
    if (op.size) result.size = op.size;
    if (op.direction) result.direction = op.direction;
  }
  if (op.type === 'rotate') {
    result.angle = op.angle;
  }
  if (op.type === 'transform') {
    result.a = op.a;
    result.b = op.b;
    result.c = op.c;
    result.d = op.d;
    result.x = op.x;
    result.y = op.y;
    if (op.unit) result.unit = op.unit;
  }

  return result;
}

// Helper function to convert DrawOptions to AnyMap format
function drawOptionsToAnyMap(item: DrawOptions): Record<string, any> {
  const result: Record<string, any> = {
    operations: item.operations.map(operationToAnyMap),
  };
  if (item.output) result.output = item.output;
  if (item.unit) result.unit = item.unit;
  return result;
}

export async function quickDraw(
  options: DrawOptions | DrawOptions[],
  dpi: number = 72,
): Promise<string | number | Array<string | number>> {
  if (Array.isArray(options)) {
    // Batch mode - convert items to AnyMap format
    // Note: quickBatchDraw results may contain error strings (e.g. "Missing…",
    // "Failed…", "Invalid…") which are returned as-is; inspect the results
    // yourself if you need to detect failures.
    const items = options.map(drawOptionsToAnyMap);
    return await NitroPdfWriterInstance.quickBatchDraw(items, dpi);
  } else {
    // Single mode - convert operations to AnyMap format
    const operations = options.operations.map(operationToAnyMap);
    const unit = options.unit || 'pt';
    return await NitroPdfWriterInstance.quickDraw(operations, unit, options.output, dpi);
  }
}
