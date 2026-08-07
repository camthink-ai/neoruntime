import i18n from './config';
import { setItem } from '@/utils/storage';

/**
 * 切换语言
 * @param lng 语言代码，如 'zh' 或 'en'
 */
export const changeLanguage = (lng: string) => {
  i18n.changeLanguage(lng);
  setItem('i18nextLng', lng);
};

/**
 * 获取当前语言
 */
export const getCurrentLanguage = () => i18n.language || 'en';

/**
 * 获取所有支持的语言
 */
export const getSupportedLanguages = () => [
  { code: 'zh', name: '中文' },
  { code: 'en', name: 'English' },
];
