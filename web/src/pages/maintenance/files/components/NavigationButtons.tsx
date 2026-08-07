import { ChevronLeft, ChevronRight, ArrowUp } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { useTranslation } from 'react-i18next';

interface NavigationButtonsProps {
  historyIndex: number;
  pathHistoryLength: number;
  currentPath: string;
  rootPath: string;
  onBack: () => void;
  onForward: () => void;
  onGoUp: () => void;
}

export function NavigationButtons({
  historyIndex,
  pathHistoryLength,
  currentPath,
  rootPath,
  onBack,
  onForward,
  onGoUp,
}: NavigationButtonsProps) {
  const { t } = useTranslation();
  const canGoUp = currentPath !== rootPath;

  return (
    <div className="flex items-center gap-2">
      <Button
        variant="outline"
        size="icon-sm"
        className="h-8 w-8 rounded-md border-border/50 text-muted-foreground bg-background hover:bg-muted"
        onClick={onBack}
        disabled={historyIndex <= 0}
        title={t('sys.file_management.nav_back', '后退')}
      >
        <ChevronLeft className="h-4 w-4" />
      </Button>
      <Button
        variant="outline"
        size="icon-sm"
        className="h-8 w-8 rounded-md border-border/50 text-muted-foreground bg-background hover:bg-muted"
        onClick={onForward}
        disabled={historyIndex >= pathHistoryLength - 1}
        title={t('sys.file_management.nav_forward', '前进')}
      >
        <ChevronRight className="h-4 w-4" />
      </Button>
      <Button
        variant="outline"
        size="icon-sm"
        className="h-8 w-8 rounded-md border-border/50 text-muted-foreground bg-background hover:bg-muted"
        onClick={onGoUp}
        disabled={!canGoUp}
        title={t('sys.file_management.nav_up', '返回上级')}
      >
        <ArrowUp className="h-4 w-4" />
      </Button>
    </div>
  );
}
