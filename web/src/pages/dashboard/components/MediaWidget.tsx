import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import { Video } from 'lucide-react';

export default function MediaWidget() {
  const { t } = useTranslation();
  const navigate = useNavigate();

  return (
    <button
      type="button"
      onClick={() => navigate('/media')}
      className="bg-card rounded-2xl p-6 shadow-sm border border-border hover:shadow-md hover:border-primary/50 transition-all duration-200 aspect-square flex flex-col items-center justify-center gap-3 group"
    >
      <div className="w-16 h-16 rounded-xl bg-primary/10 flex items-center justify-center group-hover:bg-primary/20 transition-colors">
        <Video className="w-8 h-8 text-primary" />
      </div>
      <div className="text-center">
        <p className="text-foreground font-semibold text-base">
          {t('sys.media.title', 'Media')}
        </p>
        <p className="text-muted-foreground text-xs mt-1">
          {t('sys.media.view_streams', 'Live Streams')}
        </p>
      </div>
    </button>
  );
}
