import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';
import { monitorApi, storageApi } from '@/services/api/system';
import { RefreshCw } from 'lucide-react';
import { useState } from 'react';
import {
  AlertDialog,
  AlertDialogContent,
  AlertDialogHeader,
  AlertDialogTitle,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogCancel,
  AlertDialogAction,
} from '@/components/ui/alert-dialog';
import { toast } from 'sonner';
import { StorageSkeleton } from './components/StorageSkeleton';
import InternalStorageCard from './components/InternalStorageCard';
import RemovableStorageCard from './components/RemovableStorageCard';
import EmptySlotCard from './components/EmptySlotCard';
import { groupPartitionsByDevice } from './lib/groupDevices';
import ErrorState from '@/components/ErrorState';
import type { StorageDevice } from '@/services/types';

export default function Storage() {
  const { t } = useTranslation();
  const queryClient = useQueryClient();

  const [selectedDevice, setSelectedDevice] = useState<StorageDevice | null>(
    null
  );
  const [unmountDialogOpen, setUnmountDialogOpen] = useState(false);
  const [formatDialogOpen, setFormatDialogOpen] = useState(false);
  const [isRefreshing, setIsRefreshing] = useState(false);

  const unmountMutation = useMutation({
    mutationFn: (target: string) => storageApi.unmountDisk(target),
    onSuccess: () => {
      toast.success(t('sys.storage.unmount_success', '卸载成功'));
      setUnmountDialogOpen(false);
      queryClient.invalidateQueries({ queryKey: ['diskInfo'] });
    },
    onError: (error: unknown) => {
      const msg = error instanceof Error ? error.message : String(error);
      toast.error(t('sys.storage.unmount_error', '卸载失败') + msg);
    },
  });

  const formatMutation = useMutation({
    mutationFn: ({
      device,
      fstype,
    }: {
      device: string;
      fstype: 'ext4' | 'vfat' | 'fat32';
    }) => storageApi.formatDisk(device, fstype),
    onSuccess: () => {
      toast.success(t('sys.storage.format_success', '格式化成功'));
      setFormatDialogOpen(false);
      queryClient.invalidateQueries({ queryKey: ['diskInfo'] });
    },
    onError: (error: unknown) => {
      const msg = error instanceof Error ? error.message : String(error);
      toast.error(t('sys.storage.format_error', '格式化失败') + msg);
    },
  });

  const handleUnmount = () => {
    if (!selectedDevice) return;
    const mounted = selectedDevice.partitions.find(
      p => p.mountpoint && !p.is_protected
    );
    if (mounted) unmountMutation.mutate(mounted.mountpoint);
  };

  const handleFormat = () => {
    if (!selectedDevice) return;
    const target = selectedDevice.partitions.reduce(
      (a, b) => (a.total > b.total ? a : b),
      selectedDevice.partitions[0]
    );
    if (target) formatMutation.mutate({ device: target.device, fstype: 'ext4' });
  };

  const handleRefresh = async () => {
    try {
      setIsRefreshing(true);
      await queryClient.invalidateQueries({ queryKey: ['diskInfo'] });
      await refetch();
      toast.success(t('sys.storage.refresh_success', '存储信息已刷新'));
    } finally {
      setIsRefreshing(false);
    }
  };

  const {
    data: diskData,
    isLoading,
    error,
    refetch,
  } = useQuery({
    queryKey: ['diskInfo'],
    queryFn: async () => {
      const response = await monitorApi.getDisk();
      const raw = response as any;
      return raw?.data?.data || raw?.data || raw;
    },
  });

  const partitions = diskData?.partitions || [];
  const devices = groupPartitionsByDevice(partitions);

  if (isLoading) return <StorageSkeleton />;

  if (error) {
    return (
      <div className="p-8 mx-auto w-full min-h-screen bg-background">
        <div className="mx-auto flex h-64 max-w-xl items-center justify-center">
          <ErrorState actionLoading={isRefreshing} onAction={handleRefresh} />
        </div>
      </div>
    );
  }

  return (
    <div className="p-5 md:p-10 max-w-4xl w-full min-h-screen bg-background mx-auto">
      {/* Header */}
      <div className="flex items-center justify-between mb-6">
        <div>
          <h2 className="text-lg font-bold">
            {t('sys.storage.title', '存储')}
          </h2>
        </div>
        <Button
          variant="outline"
          size="sm"
          onClick={handleRefresh}
          disabled={isRefreshing}
        >
          <RefreshCw
            className={`w-4 h-4 mr-2 ${isRefreshing ? 'animate-spin' : ''}`}
          />
          {t('sys.storage.refresh', '刷新')}
        </Button>
      </div>

      {/* Device Cards */}
      <div className="flex flex-col gap-4">
        {devices.map(device => {
          if (device.type === 'internal') {
            return <InternalStorageCard key={device.id} device={device} />;
          }
          if (device.partitions.length === 0) {
            return <EmptySlotCard key={device.id} type={device.type} />;
          }
          return (
            <RemovableStorageCard
              key={device.id}
              device={device}
              onUnmount={() => {
                setSelectedDevice(device);
                setUnmountDialogOpen(true);
              }}
              onFormat={() => {
                setSelectedDevice(device);
                setFormatDialogOpen(true);
              }}
            />
          );
        })}
      </div>

      {/* Unmount Dialog */}
      <AlertDialog open={unmountDialogOpen} onOpenChange={setUnmountDialogOpen}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>
              {t('sys.storage.unmount_confirm_title', '确认卸载')}
            </AlertDialogTitle>
            <AlertDialogDescription>
              {t(
                'sys.storage.unmount_confirm_desc',
                '确定要卸载此存储设备吗？请确保没有正在进行的读写操作。'
              )}
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel disabled={unmountMutation.isPending}>
              {t('common.cancel')}
            </AlertDialogCancel>
            <AlertDialogAction
              onClick={handleUnmount}
              disabled={unmountMutation.isPending}
            >
              {unmountMutation.isPending
                ? t('common.processing')
                : t('sys.storage.unmount', '卸载')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>

      {/* Format Dialog */}
      <AlertDialog open={formatDialogOpen} onOpenChange={setFormatDialogOpen}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>
              {t('sys.storage.format_confirm_title', '确认格式化')}
            </AlertDialogTitle>
            <AlertDialogDescription>
              <span className="text-destructive">
                {t(
                  'sys.storage.format_confirm_desc',
                  '警告：格式化将清除存储设备上的所有数据，此操作无法撤销！'
                )}
              </span>
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel disabled={formatMutation.isPending}>
              {t('common.cancel')}
            </AlertDialogCancel>
            <AlertDialogAction
              onClick={handleFormat}
              disabled={formatMutation.isPending}
              className="bg-destructive text-destructive-foreground hover:bg-destructive/90"
            >
              {formatMutation.isPending
                ? t('common.processing')
                : t('sys.storage.format', '格式化')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </div>
  );
}
