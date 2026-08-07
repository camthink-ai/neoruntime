import { useTranslation } from 'react-i18next';
import { Card, CardContent } from '@/components/ui/card';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { HardDrive, Usb, LogOut, PaintRoller, CheckCircle } from 'lucide-react';
import type { StorageDevice } from '@/services/types';
import { formatSize, getProgressColor } from '../lib/formatUtils';
import PartitionBreakdown from './PartitionBreakdown';
import { toast } from 'sonner';
import { storageApi } from '@/services/api/system';
import { useQueryClient } from '@tanstack/react-query';

export default function RemovableStorageCard({
  device,
  onUnmount,
  onFormat,
}: {
  device: StorageDevice;
  onUnmount: () => void;
  onFormat: () => void;
}) {
  const { t } = useTranslation();
  const queryClient = useQueryClient();
  const isSdCard = device.type === 'sd_card';
  const Icon = isSdCard ? HardDrive : Usb;
  const percent = Math.min(100, Math.max(0, device.usagePercent));
  const barColor = getProgressColor(percent);
  const hasMountedPartitions = device.partitions.some(p => p.mountpoint);
  const unmountedPartitions = device.partitions.filter(p => !p.mountpoint);

  const handleMount = async () => {
    try {
      const target = unmountedPartitions.reduce(
        (a, b) => (a.total > b.total ? a : b),
        unmountedPartitions[0]
      );
      if (target) {
        await storageApi.mountDisk(target.device);
        toast.success(t('sys.storage.mount_success', '挂载成功'));
        queryClient.invalidateQueries({ queryKey: ['diskInfo'] });
      }
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : String(err);
      toast.error(t('sys.storage.mount_error', '挂载失败') + msg);
    }
  };

  return (
    <Card>
      <CardContent className="py-6 space-y-5">
        {/* Header */}
        <div className="flex items-center justify-between">
          <div className="flex items-center gap-3">
            <div className="rounded-lg bg-primary/10 p-2.5">
              <Icon className="h-5 w-5 text-primary" />
            </div>
            <div>
              <div className="flex items-center gap-2">
                <span className="text-sm font-semibold">
                  {isSdCard
                    ? t('sys.storage.sd_card_label', 'SD 卡')
                    : t('sys.storage.usb_label', 'USB 存储')}
                </span>
                {hasMountedPartitions ? (
                  <Badge className="bg-emerald-500/10 text-emerald-600 border-emerald-500/20 hover:bg-emerald-500/20 text-[11px] px-2 py-0.5">
                    {t('sys.storage.mounted', '已挂载')}
                  </Badge>
                ) : (
                  <Badge
                    variant="outline"
                    className="text-yellow-600 border-yellow-500/30 text-[11px] px-2 py-0.5"
                  >
                    {t('sys.storage.unmounted', '未挂载')}
                  </Badge>
                )}
              </div>
              <p className="text-sm text-muted-foreground mt-0.5">
                {formatSize(device.totalBytes)}
                {device.usedBytes > 0 && (
                  <>
                    {' '}
                    <span className="text-muted-foreground/40">·</span>{' '}
                    <span
                      className={
                        percent >= 80
                          ? 'text-red-500'
                          : percent >= 50
                            ? 'text-yellow-500'
                            : 'text-emerald-500'
                      }
                    >
                      {percent.toFixed(1)}%
                    </span>
                  </>
                )}
              </p>
            </div>
          </div>
        </div>

        {/* Usage progress bar */}
        {device.usedBytes > 0 && (
          <div className="h-2 w-full overflow-hidden rounded-full bg-secondary/30">
            <div
              className={`h-full rounded-full transition-all ${barColor}`}
              style={{ width: `${percent}%` }}
            />
          </div>
        )}

        {/* Partition breakdown */}
        <PartitionBreakdown partitions={device.partitions} />

        {/* Actions */}
        <div className="flex items-center gap-2 pt-1">
          {hasMountedPartitions ? (
            <>
              <Button
                variant="outline"
                size="sm"
                className="h-8 text-xs"
                onClick={onUnmount}
                disabled={!device.canUnmount}
              >
                <LogOut className="w-3.5 h-3.5 mr-1.5" />
                {t('sys.storage.unmount', '卸载')}
              </Button>
              <Button
                variant="outline"
                size="sm"
                className="h-8 text-xs text-destructive hover:text-destructive"
                onClick={onFormat}
                disabled={!device.canFormat}
              >
                <PaintRoller className="w-3.5 h-3.5 mr-1.5" />
                {t('sys.storage.format', '格式化')}
              </Button>
            </>
          ) : (
            <Button size="sm" className="h-8 text-xs" onClick={handleMount}>
              <CheckCircle className="w-3.5 h-3.5 mr-1.5" />
              {t('sys.storage.mount', '挂载')}
            </Button>
          )}
        </div>
      </CardContent>
    </Card>
  );
}
