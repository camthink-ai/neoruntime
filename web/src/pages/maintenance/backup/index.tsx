import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Download,
  FileJson,
  Info,
  Loader2,
  Server,
  ShieldCheck,
  Upload,
} from 'lucide-react';
import type { LucideIcon } from 'lucide-react';
import { toast } from 'sonner';
import { cn } from '@/lib/utils';
import { Button } from '@/components/ui/button';
import { Card } from '@/components/ui/card';
import ImportFileTransferDialog from './ImportFileTransferDialog';
import { backupApi } from '@/services/api/backup';

// tar.gz upload: react-dropzone accepts both the official and the x-gzip mime.
const ACCEPT_GZIP = {
  'application/gzip': ['.tar.gz', '.tgz'],
  'application/x-gzip': ['.tar.gz', '.tgz'],
};
// JSON upload for the media-config tier (config only, no image bytes).
const ACCEPT_JSON = { 'application/json': ['.json'] };
const CLONE_MAX = 256 * 1024 * 1024;
const CONFIG_MAX = 16 * 1024 * 1024;

interface TierCardProps {
  icon: LucideIcon;
  title: string;
  desc: string;
  riskLabel: string;
  /** Tailwind classes for the top accent bar, icon tile and risk pill. */
  accentClass: string;
  iconClass: string;
  riskClass: string;
  /** Compact facts row (format / size limit / service restart). */
  meta: { label: string; value: string }[];
  /** Clone-only reassurance strip: items the target keeps through an import. */
  preserved?: string[];
  exporting: boolean;
  onExport: () => void;
  onImport: () => void;
}

// One export/import tier. Structure mirrors the risk semantics used inside
// ImportFileTransferDialog: sky = low (config), destructive = high (clone).
// The footer sits on mt-auto so both cards' action rows align regardless of
// the optional preserved strip.
function TierCard({
  icon: Icon,
  title,
  desc,
  riskLabel,
  accentClass,
  iconClass,
  riskClass,
  meta,
  preserved,
  exporting,
  onExport,
  onImport,
}: TierCardProps) {
  const { t } = useTranslation();

  return (
    <Card className="gap-0 overflow-hidden transition-shadow hover:shadow-md">
      <div className={cn('h-1 shrink-0', accentClass)} />
      <div className="flex flex-1 flex-col gap-5 p-6">
        <div className="flex items-start justify-between gap-3">
          <div className="flex items-center gap-3.5">
            <div
              className={cn(
                'flex h-11 w-11 shrink-0 items-center justify-center rounded-xl',
                iconClass
              )}
            >
              <Icon className="h-5 w-5" />
            </div>
            <div>
              <h2 className="text-base font-semibold text-foreground">
                {title}
              </h2>
              <p className="mt-0.5 text-sm text-muted-foreground">{desc}</p>
            </div>
          </div>
          <span
            className={cn(
              'inline-flex shrink-0 items-center gap-1.5 rounded-full px-2.5 py-1 text-xs font-medium',
              riskClass
            )}
          >
            <span className="h-1.5 w-1.5 rounded-full bg-current" />
            {riskLabel}
          </span>
        </div>

        <div className="grid grid-cols-3 gap-3 rounded-xl border border-border/60 bg-muted/30 px-4 py-3">
          {meta.map(m => (
            <div key={m.label} className="flex min-w-0 flex-col gap-0.5">
              <span className="truncate text-xs text-muted-foreground">
                {m.label}
              </span>
              <span className="truncate text-sm font-medium text-foreground">
                {m.value}
              </span>
            </div>
          ))}
        </div>

        {preserved && preserved.length > 0 && (
          <div className="flex items-start gap-2 rounded-lg border border-emerald-500/30 bg-emerald-500/[0.04] px-3 py-2.5">
            <ShieldCheck className="mt-0.5 h-4 w-4 shrink-0 text-emerald-600 dark:text-emerald-400" />
            <p className="text-xs leading-relaxed text-muted-foreground">
              <span className="font-medium text-emerald-700 dark:text-emerald-400">
                {t('maintenance.backup.identity_preserved', '目标机保留')}:
              </span>{' '}
              {preserved.join(' · ')}
            </p>
          </div>
        )}

        <div className="mt-auto flex gap-3 pt-1">
          <Button
            variant="carbon"
            className="flex-1 gap-2"
            onClick={onExport}
            disabled={exporting}
          >
            {exporting ? (
              <Loader2 className="h-4 w-4 animate-spin" />
            ) : (
              <Download className="h-4 w-4" />
            )}
            {exporting
              ? t('maintenance.backup.exporting', '导出中…')
              : t('common.export', '导出')}
          </Button>
          <Button
            variant="secondary"
            className="flex-1 gap-2"
            onClick={onImport}
          >
            <Upload className="h-4 w-4" />
            {t('common.import', '导入')}
          </Button>
        </div>
      </div>
    </Card>
  );
}

export default function BackupMigrate() {
  const { t } = useTranslation();
  const [exportingConfig, setExportingConfig] = useState(false);
  const [exportingClone, setExportingClone] = useState(false);
  const [configImportOpen, setConfigImportOpen] = useState(false);
  const [cloneImportOpen, setCloneImportOpen] = useState(false);

  const runExportConfig = async () => {
    setExportingConfig(true);
    try {
      await backupApi.exportMediaConfig();
      toast.success(
        t('maintenance.backup.toast.export_config', '媒体配置已导出')
      );
    } catch {
      toast.error(t('maintenance.backup.export_failed', '导出失败'));
    } finally {
      setExportingConfig(false);
    }
  };

  const runExportClone = async () => {
    setExportingClone(true);
    try {
      await backupApi.exportClone();
      toast.success(
        t('maintenance.backup.toast.export_clone', '整机备份已导出')
      );
    } catch {
      toast.error(t('maintenance.backup.export_failed', '导出失败'));
    } finally {
      setExportingClone(false);
    }
  };

  // Impact disclosure lives inside each import dialog (confirm step), not on
  // the card surface — the card only carries the quick facts (meta row).
  const configEffects = [
    t(
      'maintenance.backup.tier.config.effect1',
      '用导入文件替换本机的相机图像与媒体设置'
    ),
    t(
      'maintenance.backup.tier.config.effect2',
      '相机服务将重启（约 10 秒），实时画面短暂中断'
    ),
  ];
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

  const configMeta = [
    {
      label: t('maintenance.backup.meta.format', '文件格式'),
      value: '.json',
    },
    {
      label: t('maintenance.backup.meta.size_limit', '大小上限'),
      value: `${CONFIG_MAX / (1024 * 1024)} MB`,
    },
    {
      label: t('maintenance.backup.meta.restart', '服务重启'),
      value: t('maintenance.backup.meta.restart_config', '相机服务'),
    },
  ];
  const cloneMeta = [
    {
      label: t('maintenance.backup.meta.format', '文件格式'),
      value: '.tar.gz',
    },
    {
      label: t('maintenance.backup.meta.size_limit', '大小上限'),
      value: `${CLONE_MAX / (1024 * 1024)} MB`,
    },
    {
      label: t('maintenance.backup.meta.restart', '服务重启'),
      value: t('maintenance.backup.meta.restart_clone', '全部服务'),
    },
  ];

  return (
    <div className="flex h-full flex-col p-4 md:p-6">
      <div className="mx-auto flex w-full max-w-5xl flex-1 flex-col">
        <h1 className="text-2xl font-bold tracking-tight text-foreground">
          {t('maintenance.backup.title', '备份与迁移')}
        </h1>
        <p className="mt-1.5 text-sm text-muted-foreground">
          {t(
            'maintenance.backup.subtitle',
            '导出/导入设备配置，以在相同型号设备间复制设置或执行整机克隆。'
          )}
        </p>

        <div className="mt-6 grid gap-6 lg:grid-cols-2">
          {/* Media config (JSON) — low risk, no identity change, no disconnect. */}
          <TierCard
            icon={FileJson}
            title={t('maintenance.backup.tier.config.title', '媒体配置 (JSON)')}
            desc={t(
              'maintenance.backup.tier.config.desc',
              '仅相机图像与媒体设置（不含叠加图）'
            )}
            riskLabel={t('maintenance.backup.risk.low', '低风险')}
            accentClass="bg-gradient-to-r from-sky-500 to-sky-400"
            iconClass="bg-sky-500/10 text-sky-600 dark:text-sky-400"
            riskClass="bg-sky-500/10 text-sky-700 dark:text-sky-400"
            meta={configMeta}
            exporting={exportingConfig}
            onExport={runExportConfig}
            onImport={() => setConfigImportOpen(true)}
          />

          {/* Full device clone — high risk, regenerates identity, reconnect flow. */}
          <TierCard
            icon={Server}
            title={t('maintenance.backup.tier.clone.title', '整机克隆')}
            desc={t('maintenance.backup.tier.clone.desc', '整机全部配置')}
            riskLabel={t('maintenance.backup.risk.high', '高风险')}
            accentClass="bg-gradient-to-r from-destructive to-destructive/60"
            iconClass="bg-destructive/10 text-destructive"
            riskClass="bg-destructive/10 text-destructive"
            meta={cloneMeta}
            preserved={identityPreserved}
            exporting={exportingClone}
            onExport={runExportClone}
            onImport={() => setCloneImportOpen(true)}
          />
        </div>

        <div className="mt-6 flex items-center gap-2 text-xs text-muted-foreground">
          <Info className="h-3.5 w-3.5 shrink-0" />
          {t(
            'maintenance.backup.footnote',
            '导入会覆盖本机配置并重启服务，请谨慎操作。'
          )}
        </div>
      </div>

      {/* Low risk: media config JSON. The client parses the file before POSTing;
          a malformed file throws SyntaxError → friendly invalid_json prompt. */}
      <ImportFileTransferDialog
        open={configImportOpen}
        onOpenChange={setConfigImportOpen}
        title={t('maintenance.backup.importTiers.config.title', '导入媒体配置')}
        description={t(
          'maintenance.backup.importTiers.config.desc',
          '选择此前导出的 .json 文件'
        )}
        accept={ACCEPT_JSON}
        maxSize={CONFIG_MAX}
        effects={configEffects}
        risk="low"
        onImport={async file => {
          const envelope = JSON.parse(await file.text());
          await backupApi.importMediaConfig(envelope);
        }}
        successMessage={t(
          'maintenance.backup.toast.import_config',
          '媒体配置已导入'
        )}
      />

      {/* High risk: full clone. criticalTransition → suppress + health-poll. */}
      <ImportFileTransferDialog
        open={cloneImportOpen}
        onOpenChange={setCloneImportOpen}
        title={t('maintenance.backup.importTiers.clone.title', '导入整机克隆')}
        description={t(
          'maintenance.backup.importTiers.clone.desc',
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
