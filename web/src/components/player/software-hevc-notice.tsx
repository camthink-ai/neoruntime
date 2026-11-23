import { useCallback, useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { AlertTriangle, X } from 'lucide-react';
import { getItem, setItem } from '@/utils/storage';

const DISMISSED_KEY = 'softwareHevcNoticeDismissed';
const DISMISSED_EVENT = 'aipc:software-hevc-notice-dismissed';

export function useSoftwareHevcNotice() {
  const [active, setActive] = useState(false);
  const [dismissed, setDismissed] = useState(() => getItem<boolean>(DISMISSED_KEY) === true);
  const onChange = useCallback((value: boolean) => {
    setActive(value);
  }, []);
  useEffect(() => {
    const syncDismissed = () => setDismissed(getItem<boolean>(DISMISSED_KEY) === true);
    const onDismissed = () => setDismissed(true);
    window.addEventListener(DISMISSED_EVENT, onDismissed);
    window.addEventListener('storage', syncDismissed);
    syncDismissed();
    return () => {
      window.removeEventListener(DISMISSED_EVENT, onDismissed);
      window.removeEventListener('storage', syncDismissed);
    };
  }, []);
  const dismiss = useCallback(() => {
    setItem(DISMISSED_KEY, true);
    setDismissed(true);
    window.dispatchEvent(new Event(DISMISSED_EVENT));
  }, []);
  return { visible: active && !dismissed, onChange, dismiss };
}

export function SoftwareHevcNotice({ onDismiss }: { onDismiss: () => void }) {
  const { t } = useTranslation();
  return (
    <div role="status" className="mt-2 mb-2 flex w-full shrink-0 items-start gap-2 rounded-md bg-amber-50 px-3 py-2 text-sm text-amber-800 dark:bg-amber-950 dark:text-amber-200">
      <AlertTriangle className="mt-0.5 size-4 shrink-0" aria-hidden="true" />
      <span className="flex-1">{t('sys.media_player.software_hevc_warning')}</span>
      <button type="button" onClick={onDismiss} aria-label={t('sys.media_player.dismiss_warning')} className="shrink-0 rounded hover:bg-black/10">
        <X className="size-4" aria-hidden="true" />
      </button>
    </div>
  );
}
