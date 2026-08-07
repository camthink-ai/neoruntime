import terser from '@rollup/plugin-terser';

export default terser({
  compress: {
    // Temporarily disable console removal for debugging
    // drop_console: ['log', 'warn', 'info', 'debug', 'trace', 'error'],
    drop_debugger: true,
  },
  mangle: true,
  output: { comments: false },
});
