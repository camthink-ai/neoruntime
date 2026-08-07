import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery } from '@tanstack/react-query';
import { Download, Loader2, FileText } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { Card } from '@/components/ui/card';
import { toast } from 'sonner';
import { downloadDebugLogs, debugLogsApi } from '@/services/api/event_logs';

export default function DebugLogExport() {
  const { t } = useTranslation();
  const [isExporting, setIsExporting] = useState(false);

  const {
    data: servicesData,
    isLoading: servicesLoading,
    isError: servicesError,
  } = useQuery({
    queryKey: ['debug-logs', 'services'],
    queryFn: () => debugLogsApi.getServices(),
  });

  const {
    data: filesData,
    isLoading: filesLoading,
    isError: filesError,
  } = useQuery({
    queryKey: ['debug-logs', 'files'],
    queryFn: () => debugLogsApi.getFiles(),
  });

  const services = servicesData?.data.services ?? [];
  const files = filesData?.data.files ?? [];
  const isLoading = servicesLoading || filesLoading;
  const isError = servicesError || filesError;

  const handleExport = async () => {
    if (!services.length && !files.length) {
      toast.error(t('sys.debug_logs.error.export_failed', '日志导出失败'));
      return;
    }
    setIsExporting(true);
    try {
      await downloadDebugLogs({
        services: services.map(s => s.id),
        files: files.map(f => f.path),
        lines: 10000,
      });
      toast.success(t('sys.debug_logs.success.exported', '日志导出成功'));
    } catch {
      toast.error(t('sys.debug_logs.error.export_failed', '日志导出失败'));
    } finally {
      setIsExporting(false);
    }
  };

  return (
    <div className="flex items-center justify-center h-full bg-background p-6">
      <Card className="p-8 max-w-md w-full text-center">
        <div className="flex justify-center mb-4">
          <div className="p-4 rounded-full bg-primary/10">
            <FileText className="w-12 h-12 text-primary" />
          </div>
        </div>

        <h2 className="text-2xl font-bold text-foreground mb-2">
          {t('sys.debug_logs.title', '开发日志导出')}
        </h2>

        <p className="text-muted-foreground mb-6">
          {t(
            'sys.debug_logs.description',
            '导出完整的系统日志用于技术分析和故障排查'
          )}
        </p>

        <Button
          size="lg"
          onClick={handleExport}
          disabled={isExporting || isLoading || isError}
          className="w-full gap-2"
          variant="carbon"
        >
          {isExporting ? (
            <>
              <Loader2 className="w-5 h-5 animate-spin" />
              {t('sys.debug_logs.exporting', '正在导出...')}
            </>
          ) : (
            <>
              <Download className="w-5 h-5" />
              {t('sys.debug_logs.export', '导出全部日志')}
            </>
          )}
        </Button>
      </Card>
    </div>
  );
}
