import compression from 'vite-plugin-compression';

export default compression({
  algorithm: 'gzip', // Use gzip algorithm
  ext: '.gz', // File extension
  deleteOriginFile: false, // Delete original file
  threshold: 1024, // Only compress files larger than 1KB
  filter: /\.(js|css|html|svg)$/, // Only compress these file types
});
