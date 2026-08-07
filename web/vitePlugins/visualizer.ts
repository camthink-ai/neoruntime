import { visualizer } from 'rollup-plugin-visualizer';

export default visualizer({
  filename: 'dist/stats.html', // Analysis file output path
  open: true, // Automatically open browser after build completes
  gzipSize: true, // Show gzip compressed size
  brotliSize: true, // Show brotli compressed size
  template: 'treemap', // Use treemap template
});
