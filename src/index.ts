import { NitroModules } from 'react-native-nitro-modules';
import type { NitroPdfWriter } from './specs/NitroPdfWriter.nitro';

export type * from './specs/NitroPdfWriter.nitro';
export * from './constants';
export * from './PdfUnits';

export const NitroPdfWriterInstance = NitroModules.createHybridObject<NitroPdfWriter>('NitroPdfWriter');
