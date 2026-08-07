import { FileText } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogHeader,
  DialogTitle,
  DialogTrigger,
} from '@/components/ui/dialog';
import { Card } from '@/components/ui/card';
import { Empty } from '@/components/ui/empty';

interface LogsDialogProps {
  children?: React.ReactNode;
  open?: boolean;
  onOpenChange?: (open: boolean) => void;
}

export default function LogsDialog({
  children,
  open,
  onOpenChange,
}: LogsDialogProps) {
  const { t } = useTranslation();

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogTrigger asChild>{children}</DialogTrigger>
      <DialogContent className="max-w-3xl max-h-[80vh] overflow-auto">
        <DialogHeader>
          <DialogTitle className="flex items-center gap-2">
            <FileText className="h-5 w-5" />
            {t('common.logs')}
          </DialogTitle>
          <DialogDescription>{t('common.view_system_logs')}</DialogDescription>
        </DialogHeader>
        <Card className="mt-4">
          <Empty description={t('common.no_logs', '暂无日志')} />
        </Card>
      </DialogContent>
    </Dialog>
  );
}
