import { useTranslation } from 'react-i18next';
import { Plus } from 'lucide-react';
import type { AppTemplate } from '@/services/types';
import TemplateCard from './TemplateCard';

interface AppCardGridProps {
  apps: AppTemplate[];
  onAppClick: (app: AppTemplate) => void;
  onStart: (app: AppTemplate) => void;
  onStop: (app: AppTemplate) => void;
  onRestart: (app: AppTemplate) => void;
  onUninstall: (app: AppTemplate) => void;
  onOpenLogs: (app: AppTemplate) => void;
  onOpenTerminal: (app: AppTemplate) => void;
  onImportClick: () => void;
  pendingActions: Set<string>;
}

export default function AppCardGrid({
  apps,
  onAppClick,
  onStart,
  onStop,
  onRestart,
  onUninstall,
  onOpenLogs,
  onOpenTerminal,
  onImportClick,
  pendingActions,
}: AppCardGridProps) {
  const { t } = useTranslation();

  return (
    <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-3 2xl:grid-cols-4 gap-4 mt-6">
      <div
        className="flex flex-col items-center justify-center p-6 border-2 border-dashed border-border rounded-lg hover:border-primary hover:bg-primary/5 transition-all cursor-pointer min-h-[220px]"
        onClick={onImportClick}
      >
        <div className="w-16 h-16 rounded-full bg-muted flex items-center justify-center mb-4">
          <Plus className="w-8 h-8 text-muted-foreground" />
        </div>
        <h3 className="text-lg font-semibold text-foreground mb-2">
          {t('sys.apps.action.import', '导入应用')}
        </h3>
        <p className="text-sm text-muted-foreground text-center">
          {t('sys.apps.action.import_desc')}
        </p>
      </div>

      {apps.map(app => (
        <TemplateCard
          key={app.id}
          app={app}
          onCardClick={onAppClick}
          onStart={onStart}
          onStop={onStop}
          onRestart={onRestart}
          onUninstall={onUninstall}
          onOpenLogs={onOpenLogs}
          onOpenTerminal={onOpenTerminal}
          pendingActions={pendingActions}
        />
      ))}
    </div>
  );
}
