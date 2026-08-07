import { createProdMockServer } from 'vite-plugin-mock/client';
// import deviceToolMock from './device-tool-mock'
// import modelVerificationMock from './model-verification-mock'
import loginMock from './loginMock';

export function setupProdMockServer() {
  createProdMockServer([...loginMock]);
}
