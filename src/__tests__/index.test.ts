jest.mock('react-native-nitro-modules', () => ({
  NitroModules: {
    createHybridObject: jest.fn(() => ({})),
  },
}));

import { NitroModules } from 'react-native-nitro-modules';
import { NitroPdfWriterInstance } from '../index';

describe('NitroPdfWriter entry point', () => {
  test('creates the hybrid object with the correct name', () => {
    expect(NitroModules.createHybridObject).toHaveBeenCalledWith('NitroPdfWriter');
    expect(NitroPdfWriterInstance).toBeDefined();
  });
});
