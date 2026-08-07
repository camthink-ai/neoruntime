import type { MockMethod } from 'vite-plugin-mock';
// import deviceToolMock from './device-tool-mock'
// import modelVerificationMock from './model-verification-mock'
import loginMock from './loginMock';
import aiStatusMock from './aiStatusMock';
import logMock from './logMock';
import mediaMock from './mediaMock';
import aiModelsMock from './aiModelsMock';

export default [
  // ...deviceToolMock,
  // ...modelVerificationMock,
  ...loginMock,
  ...aiStatusMock,
  ...logMock,
  ...mediaMock,
  ...aiModelsMock,
] as MockMethod[];
