import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { Download, Upload, Loader2, Server } from 'lucide-react';
import { toast } from 'sonner';
import { Button } from '@/components/ui/button';
import { Card } from '@/components/ui/card';
import ImportFileTransferDialog from './ImportFileTransferDialog';
import { backupApi } from '@/services/api/backup';

// tar.gz upload: react-dropzone accepts both the official and the x-gzip mime.
const ACCEPT_GZIP = {
  'application/gzip': ['.tar.gz', '.tgz'],
  'application/x-gzip': ['.tar.gz', '.tgz'],
};
const CLONE_MAX = 256 * 1024 * 1024;

export default function BackupMigrate() {
  const { t } = useTranslation();
  const [exporting, setExporting] = useState(false);
  const [importOpen, setImportOpen] = useState(false);

  const runExport = async () => {
    setExporting(true);
    try {
      await backupApi.exportClone();
      toast.success(
        t('maintenance.backup.toast.export_clone', '整机备份已导出')
      );
    } catch {
      toast.error(t('maintenance.backup.export_failed', '导出失败'));
    } finally {
      setExporting(false);
    }
  };

  // Impact disclosure lives inside the import dialog (confirm step), not on the
  // card surface — keeps the page concise and matches the debug-log export style.
  const cloneEffects = [
    t(
      'maintenance.backup.tier.clone.effect1',
      '覆盖这台设备的几乎所有配置与运行状态'
    ),
    t(
      'maintenance.backup.tier.clone.effect2',
      '重新生成设备身份并重启多个服务（含本控制台，会短暂断开）'
    ),
  ];
  const identityPreserved = [
    t('maintenance.backup.identity.password', 'Admin password'),
    t('maintenance.backup.identity.certificates', 'TLS certificates'),
    t('maintenance.backup.identity.device_name', 'Device name'),
    t('maintenance.backup.identity.apps', 'Installed apps'),
    t('maintenance.backup.identity.models', 'AI models'),
    t('maintenance.backup.identity.network', 'Network identity'),
  ];

  return (
    <div className="flex h-full min-h-0 flex-col p-4 md:p-6">
      <div className="flex flex-1 items-center justify-center">
        <Card className="w-full max-w-md p-8 text-center">
          <div className="mb-4 flex justify-center">
            <div className="rounded-full bg-primary/10 p-4">
              <Server className="h-12 w-12 text-primary" />
            </div>
          </div>

          <h2 className="mb-2 text-2xl font-bold text-foreground">
            {t('maintenance.backup.title', '整机备份')}
          </h2>

          <p className="mb-6 text-muted-foreground">
            {t(
              'maintenance.backup.subtitle',
              '导出整机备份，或从备份文件恢复到本机 / 同型号设备。'
            )}
          </p>

          <div className="flex gap-3">
            <Button
              variant="carbon"
              className="flex-1 gap-2"
              onClick={runExport}
              disabled={exporting}
            >
              {exporting ? (
                <Loader2 className="h-5 w-5 animate-spin" />
              ) : (
                <Download className="h-5 w-5" />
              )}
              {exporting
                ? t('maintenance.backup.exporting', '导出中…')
                : t('maintenance.backup.export', '导出备份')}
            </Button>
            <Button
              variant="secondary"
              className="flex-1 gap-2"
              onClick={() => setImportOpen(true)}
            >
              <Upload className="h-5 w-5" />
              {t('maintenance.backup.import', '导入恢复')}
            </Button>
          </div>

          <p className="mt-4 text-xs text-muted-foreground">
            {t(
              'maintenance.backup.footnote',
              '导入会覆盖本机配置并重启服务，请谨慎操作。'
            )}
          </p>
        </Card>
      </div>

      {/* High risk: full clone. criticalTransition → suppress + health-poll. */}
      <ImportFileTransferDialog
        open={importOpen}
        onOpenChange={setImportOpen}
        title={t('maintenance.backup.import.clone.title', '导入整机克隆')}
        description={t(
          'maintenance.backup.import.clone.desc',
          '选择之前导出的 .tar.gz 克隆文件'
        )}
        accept={ACCEPT_GZIP}
        maxSize={CLONE_MAX}
        effects={cloneEffects}
        risk="high"
        identityPreserved={identityPreserved}
        criticalTransition
        onImport={async file => {
          await backupApi.importClone(file);
        }}
        successMessage={t(
          'maintenance.backup.toast.import_clone',
          '整机克隆已导入'
        )}
      />
    </div>
  );
}
