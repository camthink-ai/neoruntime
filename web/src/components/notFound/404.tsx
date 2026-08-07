import { useTranslation } from 'react-i18next';

export default function NotFound404() {
  const { i18n } = useTranslation();

  return (
    <div className="flex items-center justify-center min-h-screen">
      <div className="text-center">
        <h1 className="text-5xl font-bold mb-4 text-foreground">
          {i18n.t('common.not_found_title')}
        </h1>
        <p className="mb-4 text-base text-muted-foreground">
          {i18n.t('common.not_found_message')}
        </p>
        <a href="/" className="text-primary hover:text-primary/80 underline">
          {i18n.t('common.not_found_back_home')}
        </a>
      </div>
    </div>
  );
}
