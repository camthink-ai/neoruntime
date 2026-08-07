import { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { Loader2 } from 'lucide-react';
import { toast } from 'sonner';
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '@/components/ui/alert-dialog';
import SystemLoadingMask from '@/components/system-loading-mask';
import { systemApi } from '@/services/api/system';
import { useAuthStore } from '@/store/auth';
import { redirectToLoginAfterReboot } from '@/utils/rebootLoginRedirect';
import { enterNetworkErrorToastSuppress } from '@/services/request';
import { startPolling, type PollingHandle } from '@/utils/polling';

export interface RebootDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
}

export function RebootDialog({ open, onOpenChange }: RebootDialogProps) {
  const { t } = useTranslation();
  const [isRebooting, setIsRebooting] = useState(false);
  const [maskError, setMaskError] = useState(false);
  const pollingRef = useRef<PollingHandle | null>(null);
  const startPollTimerRef = useRef<number | null>(null);
  const releaseNetworkErrorSuppressRef = useRef<(() => void) | null>(null);

  const releaseNetworkErrorSuppress = () => {
    releaseNetworkErrorSuppressRef.current?.();
    releaseNetworkErrorSuppressRef.current = null;
  };

  // One-shot pagehide handler that releases the suppress transition only once
  // the /login hard navigation actually begins to unload the document. See
  // onSuccess for why the release must be deferred past the navigation window.
  const releaseOnHideRef = useRef<(() => void) | null>(null);

  useEffect(() => () => {
    const onHide = releaseOnHideRef.current;
    if (onHide) {
      window.removeEventListener('pagehide', onHide);
      releaseOnHideRef.current = null;
    }
    releaseNetworkErrorSuppress();
  }, []);

  const handleReboot = async () => {
    releaseNetworkErrorSuppress();
    releaseNetworkErrorSuppressRef.current = enterNetworkErrorToastSuppress();

    setIsRebooting(true);
    setMaskError(false);
    onOpenChange(false);

    pollingRef.current?.stop();
    pollingRef.current = null;
    if (startPollTimerRef.current !== null) {
      window.clearTimeout(startPollTimerRef.current);
      startPollTimerRef.current = null;
    }

    try {
      await systemApi.restart();
    } catch {
      // restart() may throw because the device cuts the connection — that's expected
    }

    // Wait for device to go down before polling
    startPollTimerRef.current = window.setTimeout(() => {
      pollingRef.current = startPolling({
        fn: () => systemApi.healthCheck({ silent: true }),
        onSuccess: () => {
          // Clear the session and kick off the /login navigation FIRST; keep
          // the network-error-suppress transition + the loading mask up for
          // the entire navigation window. Releasing them synchronously here
          // (the old order) dropped the shield before the hard navigation
          // committed — reopening the device-info query to a reconnect
          // refetch against a half-woken device, the race that flips the
          // /settings/device-info page to ErrorState mid-reboot. The release
          // is deferred to `pagehide` (fires as the document unloads), so the
          // shield covers the whole window while the persisted flag is still
          // cleared before the next session loads.
          useAuthStore.getState().clearToken();
          redirectToLoginAfterReboot();
          toast.success(t('sys.device_info.reboot_complete', '系统重启完成'));

          if (typeof window !== 'undefined' && !releaseOnHideRef.current) {
            const onHide = () => {
              releaseNetworkErrorSuppress();
              releaseOnHideRef.current = null;
            };
            releaseOnHideRef.current = onHide;
            window.addEventListener('pagehide', onHide, { once: true });
          }

          return true;
        },
        interval: 3000,
        timeout: 180000,
        onTimeout: () => {
          setMaskError(true);
        },
        onError: () => {
          // device still offline — keep polling
        },
      });
      startPollTimerRef.current = null;
    }, 30000);
  };

  const handleDismissError = () => {
    pollingRef.current?.stop();
    pollingRef.current = null;
    if (startPollTimerRef.current !== null) {
      window.clearTimeout(startPollTimerRef.current);
      startPollTimerRef.current = null;
    }
    releaseNetworkErrorSuppress();
    setIsRebooting(false);
    setMaskError(false);
  };

  return (
    <>
      <AlertDialog open={open} onOpenChange={onOpenChange}>
        <AlertDialogContent className="max-w-md bg-card border-border shadow-xl">
          <AlertDialogHeader>
            <AlertDialogTitle className="text-lg font-bold">
              {t('sys.device_info.reboot_title', '重启系统')}
            </AlertDialogTitle>
            <AlertDialogDescription className="text-muted-foreground text-xs">
              {t(
                'sys.device_info.reboot_desc',
                '您确定要重启设备吗？设备重启期间将无法访问系统服务。'
              )}
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter className="mt-4">
            <AlertDialogCancel className="border-border font-medium">
              {t('common.cancel', '取消')}
            </AlertDialogCancel>
            <AlertDialogAction
              onClick={handleReboot}
              disabled={isRebooting}
              className="bg-foreground hover:bg-foreground/90 text-background font-medium"
            >
              {isRebooting ? (
                <Loader2 className="w-4 h-4 mr-2 animate-spin" />
              ) : null}
              {t('common.confirm', '确认重启')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>

      <SystemLoadingMask
        open={isRebooting}
        message={
          maskError
            ? t('sys.device_info.reboot_timeout_title', '重启超时')
            : t('sys.device_info.rebooting', '系统正在重启')
        }
        hint={t('sys.device_info.rebooting_hint', '请稍候，正在等待设备上线')}
        error={maskError}
        errorMessage={t(
          'sys.device_info.reboot_timeout_desc',
          '设备未能在预期时间内重启，请检查设备状态'
        )}
        onClick={maskError ? handleDismissError : undefined}
      />
    </>
  );
}
