import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Check,
  Download,
  Images,
  Info,
  Loader2,
  Server,
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
  /** Tailwind classes for the icon tile. */
  iconClass: string;
  /** Body copy under the header, one paragraph per line. */
  descLines: string[];
  /** ✓ checklist: what the tier covers. */
  includes?: string[];
  /** Note under the checklist: what a backup excludes. */
  excludesNote?: string;
  importLabel: string;
  exportLabel: string;
  exporting: boolean;
  onExport: () => void;
  onImport: () => void;
}

// One export/import tier. Structure mirrors the risk semantics used inside
// ImportFileTransferDialog: sky = low (config), destructive = high (clone).
// The footer sits on mt-auto so both cards' action rows align regardless of
// the optional checklist block. The import (risky) button leads, the export
// (safe) one stays visually primary.
function TierCard({
  icon: Icon,
  title,
  iconClass,
  descLines,
  includes,
  excludesNote,
  importLabel,
  exportLabel,
  exporting,
  onExport,
  onImport,
}: TierCardProps) {
  const { t } = useTranslation();

  return (
    <Card className="transition-shadow hover:shadow-md">
      <div className="flex flex-1 flex-col gap-5 p-6">
        <div className="flex items-center gap-3.5">
          <div
            className={cn(
              'flex h-11 w-11 shrink-0 items-center justify-center rounded-xl',
              iconClass
            )}
          >
            <Icon className="h-5 w-5" />
          </div>
          <h2 className="text-base font-semibold text-foreground">{title}</h2>
        </div>

        <div className="flex flex-col gap-1">
          {descLines.map(line => (
            <p
              key={line}
              className="text-sm leading-relaxed text-muted-foreground"
            >
              {line}
            </p>
          ))}
        </div>

        {includes && includes.length > 0 && (
          <ul className="flex flex-col gap-2">
            {includes.map(item => (
              <li
                key={item}
                className="flex items-center gap-2.5 text-sm text-foreground"
              >
                <Check className="h-4 w-4 shrink-0 text-emerald-600 dark:text-emerald-400" />
                {item}
              </li>
            ))}
          </ul>
        )}

        {excludesNote && (
          <div className="flex items-center gap-2 text-xs text-muted-foreground">
            <Info className="h-3.5 w-3.5 shrink-0" />
            {excludesNote}
          </div>
        )}

        <div className="mt-auto flex justify-end gap-3 pt-1">
          <Button variant="secondary" className="gap-2" onClick={onImport}>
            <Upload className="h-4 w-4" />
            {importLabel}
          </Button>
          <Button
            variant="carbon"
            className="gap-2"
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
              : exportLabel}
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
  // the card surface. Wording follows the backend: media import restarts
  // camera-daemon + device-control (media_config_io.go); clone import keeps
  // the target's identity (clone.go) and restarts the config-consuming
  // services. The UI-disconnect warning is shown once, in the dialog's
  // dedicated high-risk banner — not repeated here.
  const configEffects = [
    t(
      'maintenance.backup.tier.config.effect1',
      '用导入文件替换本机的媒体配置（图像、编码、音频、镜头等）'
    ),
    t(
      'maintenance.backup.tier.config.effect2',
      '相机与镜头服务将重启，实时画面短暂中断'
    ),
  ];
  const cloneEffects = [
    t(
      'maintenance.backup.tier.clone.effect1',
      '用导入文件替换整机的全部配置（设备身份除外）'
    ),
    t('maintenance.backup.tier.clone.effect2', '重启核心服务（含本控制台）'),
  ];
  const identityPreserved = [
    t('maintenance.backup.identity.password', 'Admin password'),
    t('maintenance.backup.identity.certificates', 'TLS certificates'),
    t('maintenance.backup.identity.device_name', 'Device name'),
    t('maintenance.backup.identity.apps', 'Installed apps'),
    t('maintenance.backup.identity.models', 'AI models'),
    t('maintenance.backup.identity.network', 'Network identity'),
  ];

  const configDesc = [
    t(
      'maintenance.backup.tier.config.desc',
      '导出或导入图像与媒体相关配置，适用于快速复制媒体设置。'
    ),
  ];
  const cloneDesc = [
    t('maintenance.backup.tier.clone.desc', '备份或恢复当前设备的完整配置。'),
  ];
  // Checklist wording follows the backend payload (media_config_io.go):
  // base YAML (encoders/audio/lens/autofocus/infrared) + image JSONs
  // (isp/transform/privacy_mask/osd/profile).
  const configIncludes = [
    t('maintenance.backup.tier.config.includes.image', '图像与 ISP 配置'),
    t('maintenance.backup.tier.config.includes.video', '视频编码与流'),
    t('maintenance.backup.tier.config.includes.audio', '音频配置'),
    t('maintenance.backup.tier.config.includes.lens', '镜头与红外'),
  ];
  // Clone scope per clone.go: the /data/aipc/etc tree + the 4 config DB
  // tables. App/model registries (AppInstall/AIModel) are deliberately NOT
  // cloned — only their runtime yaml configs ride along in the etc tree.
  const cloneIncludes = [
    t('maintenance.backup.tier.clone.includes.device', '设备配置'),
    t('maintenance.backup.tier.clone.includes.network', '网络配置'),
    t('maintenance.backup.tier.clone.includes.runtime', '应用与 AI 运行配置'),
  ];

  return (
    <div className="flex h-full flex-col p-4 md:p-6">
      <div className="mx-auto flex w-full max-w-5xl flex-1 flex-col">
        <h1 className="text-2xl font-bold tracking-tight text-foreground">
          {t('maintenance.backup.title', '备份与迁移')}
        </h1>

        <div className="mt-6 grid gap-6 lg:grid-cols-2">
          {/* Media config (JSON) — low risk, no identity change, no disconnect. */}
          <TierCard
            icon={Images}
            title={t('maintenance.backup.tier.config.title', '媒体配置')}
            iconClass="bg-sky-500/10 text-sky-600 dark:text-sky-400"
            descLines={configDesc}
            includes={configIncludes}
            importLabel={t('maintenance.backup.import_config', '导入配置')}
            exportLabel={t('maintenance.backup.export_config', '导出配置')}
            exporting={exportingConfig}
            onExport={runExportConfig}
            onImport={() => setConfigImportOpen(true)}
          />

          {/* Full device clone — high risk, regenerates identity, reconnect flow. */}
          <TierCard
            icon={Server}
            title={t('maintenance.backup.tier.clone.title', '整机克隆')}
            iconClass="bg-destructive/10 text-destructive"
            descLines={cloneDesc}
            includes={cloneIncludes}
            excludesNote={t(
              'maintenance.backup.tier.clone.excludes',
              '不包含设备身份信息、安全凭证及已安装的应用与 AI 模型'
            )}
            importLabel={t('maintenance.backup.restore', '恢复克隆')}
            exportLabel={t('maintenance.backup.create_clone', '创建克隆')}
            exporting={exportingClone}
            onExport={runExportClone}
            onImport={() => setCloneImportOpen(true)}
          />
        </div>
      </div>

      {/* Low risk: media config JSON. The client parses the file before POSTing;
          a malformed file throws SyntaxError → friendly invalid_json prompt. */}
      <ImportFileTransferDialog
        open={configImportOpen}
        onOpenChange={setConfigImportOpen}
        title={t('maintenance.backup.importTiers.config.title', '导入媒体配置')}
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
