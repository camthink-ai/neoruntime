import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import { Progress } from '@/components/ui/progress';
import { FolderOpen } from 'lucide-react';
import type { DiskPartition } from '@/services/types';
import { formatSize } from '../lib/formatUtils';

function getPartitionLabel(p: DiskPartition): string {
  if (p.mountpoint === '/') return 'partition_system';
  if (p.mountpoint === '/data') return 'partition_data';
  if (p.mountpoint === '/boot' || p.mountpoint === '/boot/firmware') return 'partition_boot';
  return '';
}

function guessPartitionRole(p: DiskPartition): string {
  if (p.fstype === 'vfat' && p.total < 500 * 1024 * 1024) return 'partition_boot';
  return 'partition_data';
}

export default function PartitionBreakdown({
  partitions,
}: {
  partitions: DiskPartition[];
}) {
  const { t } = useTranslation();
  const navigate = useNavigate();

  return (
    <div className="grid grid-cols-1 gap-2 sm:grid-cols-2">
      {partitions.map(p => {
        const labelKey = getPartitionLabel(p) || guessPartitionRole(p);
        const fallback =          p.mountpoint_label
          || p.mountpoint
          || p.device.split('/').pop()
          || p.device;
        const label = labelKey
          ? t(`sys.storage.${labelKey}`, { defaultValue: fallback })
          : fallback;

        const canBrowse = !!p.mountpoint;

        return (
          <div
            key={p.device}
            className={`rounded-lg bg-muted/30 px-3 py-2.5 ${canBrowse ? 'cursor-pointer hover:bg-muted/60 transition-colors group' : ''}`}
            onClick={() => {
              if (canBrowse && p.mountpoint) {
                navigate(
                  `/maintenance/files?path=${encodeURIComponent(p.mountpoint)}`
                );
              }
            }}
          >
            <div className="flex items-center justify-between text-xs mb-1.5">
              <span className="text-muted-foreground font-medium flex items-center gap-1">
                {label}
                {canBrowse && (
                  <FolderOpen className="w-3 h-3 opacity-0 group-hover:opacity-100 transition-opacity" />
                )}
              </span>
              <span className="text-muted-foreground/70 text-[11px] tabular-nums">
                {formatSize(p.used)} / {formatSize(p.total)}
              </span>
            </div>
            <Progress value={p.usage_percent} className="h-1.5" />
          </div>
        );
      })}
    </div>
  );
}
