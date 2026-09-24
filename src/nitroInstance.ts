import { NitroModules } from 'react-native-nitro-modules';
import type { NitroPdfWriter } from './specs/NitroPdfWriter.nitro';

export const NitroPdfWriterInstance = NitroModules.createHybridObject<NitroPdfWriter>('NitroPdfWriter');
