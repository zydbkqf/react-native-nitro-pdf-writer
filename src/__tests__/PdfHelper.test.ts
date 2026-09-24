import { quickDraw } from '../PdfHelper';
import type { DrawOptions } from '../PdfTypes';

// Mock the NitroPdfWriterInstance
jest.mock('../index', () => ({
  NitroPdfWriterInstance: {
    quickDraw: jest.fn().mockImplementation(async (_operations: any[], _unit: string, outputPath?: string, _dpi?: number) => {
      if (outputPath) return outputPath;
      return 1;
    }),
    quickBatchDraw: jest.fn().mockImplementation(async (items: any[], _dpi?: number) => {
      return items.map((item: any, index: number) => {
        if (item.output) return item.output;
        return index + 1;
      });
    }),
  },
}));

describe('PdfHelper', () => {
  describe('quickDraw', () => {
    it('should call quickDraw with correct parameters for single mode', async () => {
      const { NitroPdfWriterInstance } = require('../index');
      const options: DrawOptions = {
        unit: 'mm',
        output: '/tmp/test.pdf',
        operations: [
          { type: 'text', data: { content: 'Hello', x: 10, y: 10 } },
        ],
      };

      const result = await quickDraw(options);

      expect(result).toBe('/tmp/test.pdf');
      expect(NitroPdfWriterInstance.quickDraw).toHaveBeenCalledWith(
        options.operations,
        'mm',
        '/tmp/test.pdf',
        72
      );
    });

    it('should return document handle when no output path', async () => {
      const { NitroPdfWriterInstance } = require('../index');
      const options: DrawOptions = {
        operations: [
          { type: 'text', data: { content: 'Hello', x: 10, y: 10 } },
        ],
      };

      const result = await quickDraw(options);

      expect(result).toBe(1);
      expect(NitroPdfWriterInstance.quickDraw).toHaveBeenCalledWith(
        options.operations,
        'pt',
        undefined,
        72
      );
    });

    it('should call quickBatchDraw for batch mode', async () => {
      const { NitroPdfWriterInstance } = require('../index');
      const items: DrawOptions[] = [
        { output: '/tmp/test1.pdf', operations: [{ type: 'text', data: { content: 'File 1', x: 10, y: 10 } }] },
        { output: '/tmp/test2.pdf', operations: [{ type: 'text', data: { content: 'File 2', x: 10, y: 10 } }] },
      ];

      const result = await quickDraw(items);

      expect(result).toEqual(['/tmp/test1.pdf', '/tmp/test2.pdf']);
      expect(NitroPdfWriterInstance.quickBatchDraw).toHaveBeenCalled();
    });

    it('should return document handles when no output in batch', async () => {
      const { NitroPdfWriterInstance } = require('../index');
      const items: DrawOptions[] = [
        { operations: [{ type: 'text', data: { content: 'Doc 1', x: 10, y: 10 } }] },
        { operations: [{ type: 'text', data: { content: 'Doc 2', x: 10, y: 10 } }] },
      ];

      const result = await quickDraw(items);

      expect(result).toEqual([1, 2]);
      expect(NitroPdfWriterInstance.quickBatchDraw).toHaveBeenCalled();
    });
  });
});
