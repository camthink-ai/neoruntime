// import compression from './compression'
// import visualizer from './visualizer
import mock from './mock';
import svgIcons from './svgIcons';
import cleanCode from './cleanCode';
import version from './version';

// 使用非强类型避免与不同版本的 Vite 插件类型冲突
const plugins = [
  mock,
  // compression,
  // visualizer,
  cleanCode,
  svgIcons,
  version(), // Call function to return plugin object
];

export default plugins;
