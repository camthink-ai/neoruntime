import 'react-i18next';

// 导入语言资源类型
import zhCom from './locales/zh/com.json';
import zhErrors from './locales/zh/errors.json';
import zhSys from './locales/zh/sys.json';
import enCom from './locales/en/com.json';
import enErrors from './locales/en/errors.json';
import enSys from './locales/en/sys.json';

declare module 'react-i18next' {
  interface CustomTypeOptions {
    defaultNS: 'com';
    resources: {
      com: typeof zhCom & typeof enCom;
      errors: typeof zhErrors & typeof enErrors;
      sys: typeof zhSys & typeof enSys;
    };
  }
}
