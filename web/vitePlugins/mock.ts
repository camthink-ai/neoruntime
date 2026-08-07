import { viteMockServe } from 'vite-plugin-mock';

export default viteMockServe({
  mockPath: 'src/mock',
  enable: true,
  logger: true, // Enable logging
});
