import { useTranslation } from 'react-i18next';
import { Card, CardContent } from '@/components/ui/card';
import { Badge } from '@/components/ui/badge';
import { Info } from 'lucide-react';
import type { StorageDevice } from '@/services/types';
import { formatSize, getStrokeColor } from '../lib/formatUtils';
import PartitionBreakdown from './PartitionBreakdown';

export default function InternalStorageCard({
  device,
}: {
  device: StorageDevice;
}) {
  const { t } = useTranslation();
  const percent = Math.min(100, Math.max(0, device.usagePercent));
  const strokeColor = getStrokeColor(percent);
  const circumference = 2 * Math.PI * 42;
  const offset = circumference - (percent / 100) * circumference;

  return (
    <Card>
      <CardContent className="py-6 space-y-5">
        <div className="flex items-center gap-6">
          {/* Circular SVG progress */}
          <div className="relative flex-shrink-0">
            <svg width="112" height="112" className="-rotate-90">
              <circle
                cx="56"
                cy="56"
                r="42"
                fill="none"
                stroke="currentColor"
                className="text-secondary/30"
                strokeWidth="8"
              />
              <circle
                cx="56"
                cy="56"
                r="42"
                fill="none"
                strokeWidth="8"
                strokeLinecap="round"
                stroke={strokeColor}
                strokeDasharray={circumference}
                strokeDashoffset={offset}
                style={{ transition: 'stroke-dashoffset 0.6s ease' }}
              />
            </svg>
            <div className="absolute inset-0 flex flex-col items-center justify-center">
              <span className="text-xl font-bold tabular-nums">
                {percent.toFixed(0)}%
              </span>
              <span className="text-[10px] text-muted-foreground">
                {t('sys.storage.used', '已用')}
              </span>
            </div>
          </div>

          {/* Device info */}
          <div className="flex-1 min-w-0">
            <div className="flex items-center gap-2 mb-1">
              <span className="text-sm font-semibold">
                {t('sys.storage.internal_label', '内部存储')}
              </span>
              <Badge variant="secondary" className="text-[11px] px-2 py-0.5">
                {t('sys.storage.builtin', '内置')}
              </Badge>
            </div>
            <p className="text-sm text-muted-foreground">
              {formatSize(device.usedBytes)}{' '}
              <span className="text-muted-foreground/40">/</span>{' '}
              {formatSize(device.totalBytes)} {t('sys.storage.used', '已用')}
            </p>
          </div>
        </div>

        {/* Partition breakdown */}
        <PartitionBreakdown partitions={device.partitions} />

        {/* System notice */}
        <div className="flex items-start gap-2 rounded-lg bg-muted/40 px-3 py-2">
          <Info className="w-3.5 h-3.5 text-muted-foreground mt-0.5 flex-shrink-0" />
          <p className="text-xs text-muted-foreground leading-relaxed">
            {t(
              'sys.storage.system_notice',
              '系统分区为设备运行必需，无法修改或格式化。'
            )}
          </p>
        </div>
      </CardContent>
    </Card>
  );
}
