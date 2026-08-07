import i18n from 'i18next';
import { initReactI18next } from 'react-i18next';
import { getItem } from '@/utils/storage';
import zhCom from './locales/zh/com.json';
import zhErrors from './locales/zh/errors.json';
import zhSys from './locales/zh/sys.json';
import enCom from './locales/en/com.json';
import enErrors from './locales/en/errors.json';
import enSys from './locales/en/sys.json';

// 从本地存储获取保存的语言设置，默认为英文
const savedLanguage = (getItem<string>('i18nextLng') as string | null) || 'en';

i18n.use(initReactI18next).init({
  resources: {
    zh: {
      com: zhCom,
      errors: zhErrors,
      sys: zhSys,
    },
    en: {
      com: enCom,
      errors: enErrors,
      sys: enSys,
    },
  },
  lng: savedLanguage,
  fallbackLng: 'en',
  defaultNS: 'com',
  ns: ['com', 'errors', 'sys'],
  // 允许在所有命名空间中查找键
  fallbackNS: ['com', 'errors', 'sys'],
  interpolation: {
    escapeValue: false, // React 已经转义了
  },
  react: {
    useSuspense: false, // 避免在 SSR 或某些场景下的问题
  },
});

export default i18n;
