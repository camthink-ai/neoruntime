import { useState, useCallback, useRef, useEffect } from 'react';
import { useTranslation } from 'react-i18next';
import { Loader2, ShieldAlert } from 'lucide-react';
import { toast } from 'sonner';
import { Button } from '@/components/ui/button';
import {
  Dialog,
  DialogContent,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog';
import { Checkbox } from '@/components/ui/checkbox';
import { Label } from '@/components/ui/label';
import FileUpload from '@/components/file-upload';
import SystemLoadingMask from '@/components/system-loading-mask';
import { systemApi, type OTAStatusResponse } from '@/services/api/system';
import { enterNetworkErrorToastSuppress } from '@/services/request';
import { useAuthStore } from '@/store/auth';
import {
  redirectToLoginAfterOTASuccess,
  stashOTASuccessLoginMessage,
} from '@/utils/otaLoginRedirect';
import { startPolling, type PollingHandle } from '@/utils/polling';

export interface FirmwareUpdateDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
}

type MaskPhase =
  | 'uploading'
  | 'upgrading'
  | 'rebooting'
  | 'offline'
  | 'error'
  | null;

const unwrapOTAStatus = (response: any): OTAStatusResponse | undefined => response?.data?.data ?? response?.data ?? response;

export function FirmwareUpdateDialog({
  open,
  onOpenChange,
}: FirmwareUpdateDialogProps) {
  const { t } = useTranslation();

  const [file, setFile] = useState<File | null>(null);
  const [uploading, setUploading] = useState(false);
  const [confirmed, setConfirmed] = useState(false);
  const [maskPhase, setMaskPhase] = useState<MaskPhase>(null);
  const [maskErrorMessage, setMaskErrorMessage] = useState<string>('');
  const pollingRef = useRef<PollingHandle | null>(null);
  const releaseNetworkErrorSuppressRef = useRef<(() => void) | null>(null);
  const rebootArmedRef = useRef(false);
  const rebootObservedOfflineRef = useRef(false);
  const rebootExpectedRef = useRef(false);
  // Last OTA job id — kept so the offline "重新检测" action can re-poll the
  // same job after the reboot window times out.
  const jobIdRef = useRef('');

  const releaseNetworkErrorSuppress = useCallback(() => {
    releaseNetworkErrorSuppressRef.current?.();
    releaseNetworkErrorSuppressRef.current = null;
  }, []);

  const cleanupPolling = useCallback(() => {
    pollingRef.current?.stop();
    pollingRef.current = null;
  }, []);

  useEffect(
    () => () => {
      cleanupPolling();
      releaseNetworkErrorSuppress();
    },
    [cleanupPolling, releaseNetworkErrorSuppress]
  );

  const finishUpgrade = useCallback(() => {
    cleanupPolling();
    releaseNetworkErrorSuppress();
    setMaskPhase(null);
    setUploading(false);
    stashOTASuccessLoginMessage(
      t('sys.device_info.ota_complete_login_prompt', '固件升级完成，请重新登录')
    );
    useAuthStore.getState().clearToken();
    redirectToLoginAfterOTASuccess();
  }, [cleanupPolling, releaseNetworkErrorSuppress, t]);

  const startOTAStatusPolling = useCallback(
    (jobId?: string, preserveRebootState = false) => {
      rebootExpectedRef.current = true;
      if (!preserveRebootState) {
        rebootArmedRef.current = false;
        rebootObservedOfflineRef.current = false;
      }

      pollingRef.current = startPolling({
        fn: async () => {
          const response: any = await systemApi.otaStatus(jobId, true);
          const status = unwrapOTAStatus(response);
          return { status };
        },
        onSuccess: ({ status }) => {
          if (!status) return false;
          const terminalStatus = status.status === 'success' || status.status === 'failed';
          if (
            jobId
            && status.job_id
            && status.job_id !== jobId
            && !terminalStatus
          ) {
            return false;
          }

          if (status.status === 'failed') {
            cleanupPolling();
            releaseNetworkErrorSuppress();
            setMaskErrorMessage(
              status.error
                || status.message
                || t('sys.device_info.ota_install_failed', '固件升级失败')
            );
            setMaskPhase('error');
            setUploading(false);
            return true;
          }

          if (status.status === 'success') {
            if (rebootExpectedRef.current) {
              const rebootConfirmedByBootID = Boolean(
                status.reboot_confirmed
                && status.boot_id
                && status.current_boot_id
                && status.boot_id !== status.current_boot_id
              );
              // deploy.sh can write success, restart platform-api, and keep
              // serving 200 for a short window before the board reboot starts.
              // Treat success as the point where reboot evidence may begin;
              // do not trust a single pre-reboot 200 response as completion.
              rebootArmedRef.current = true;
              setMaskPhase('rebooting');
              if (rebootObservedOfflineRef.current || rebootConfirmedByBootID) {
                finishUpgrade();
                return true;
              }
              return false;
            }
            finishUpgrade();
            return true;
          }

          if (status.status === 'uploading' || status.status === 'preparing') {
            setMaskPhase('uploading');
          } else {
            setMaskPhase('upgrading');
          }
          return false;
        },
        interval: 2000,
        timeout: 600000,
        onTimeout: () => {
          cleanupPolling();
          releaseNetworkErrorSuppress();
          // Device never confirmed back online within the window. Surface an
          // actionable offline state with a manual re-detect instead of the
          // old dead-end "升级超时".
          setMaskErrorMessage('');
          setMaskPhase('offline');
          setUploading(false);
        },
        onError: () => {
          // platform-api can disappear briefly during deploy hot-swap. Count an
          // outage as reboot evidence only after the OTA reached success.
          if (rebootArmedRef.current) {
            rebootObservedOfflineRef.current = true;
            setMaskPhase('rebooting');
            return;
          }
          setMaskPhase('upgrading');
        },
      });
    },
    [cleanupPolling, finishUpgrade, releaseNetworkErrorSuppress, t]
  );

  const reset = useCallback(() => {
    setFile(null);
    setUploading(false);
    setConfirmed(false);
    setMaskErrorMessage('');
  }, []);

  const handleOpenChange = (nextOpen: boolean) => {
    if (!nextOpen) reset();
    onOpenChange(nextOpen);
  };

  const handleFileChange = (files: File[]) => {
    if (files.length === 0) {
      setFile(null);
      setConfirmed(false);
      return;
    }
    const selected = files[0];
    if (!selected.name.endsWith('.tar.gz')) {
      toast.error(
        t(
          'sys.device_info.invalid_firmware',
          '固件文件格式错误，请上传 .tar.gz 文件'
        )
      );
      return;
    }
    setFile(selected);
    setConfirmed(false);
  };

  const handleUpgrade = async () => {
    if (!file || !confirmed) return;

    // Ensure we don't have leftover polling/timer from previous runs
    cleanupPolling();
    releaseNetworkErrorSuppress();
    releaseNetworkErrorSuppressRef.current = enterNetworkErrorToastSuppress();

    setUploading(true);
    onOpenChange(false);

    // Phase 1: upload
    setMaskPhase('uploading');
    let firmwarePath = '';
    try {
      const parseResp: any = await systemApi.otaParse(file);
      firmwarePath = parseResp?.data?.firmware_path || '';
      if (!firmwarePath) {
        throw new Error('missing firmware_path');
      }
    } catch {
      releaseNetworkErrorSuppress();
      setMaskErrorMessage(
        t('sys.device_info.ota_upload_failed', '固件上传失败，请重试')
      );
      setMaskPhase('error');
      setUploading(false);
      return;
    }

    // Phase 2: trigger upgrade
    setMaskPhase('upgrading');
    try {
      const installResp: any = await systemApi.otaInstallFromPath(firmwarePath);
      const jobId = installResp?.data?.job_id || '';
      jobIdRef.current = jobId;
      startOTAStatusPolling(jobId);
    } catch {
      releaseNetworkErrorSuppress();
      setMaskErrorMessage(
        t('sys.device_info.ota_install_failed', '启动升级失败，请重试')
      );
      setMaskPhase('error');
      setUploading(false);
    }
  };

  const handleDismissError = () => {
    cleanupPolling();
    releaseNetworkErrorSuppress();
    setMaskPhase(null);
    setUploading(false);
    setMaskErrorMessage('');
  };

  const handleRetryDetect = () => {
    if (!jobIdRef.current) {
      handleDismissError();
      return;
    }
    setMaskErrorMessage('');
    setMaskPhase('rebooting');
    releaseNetworkErrorSuppressRef.current = enterNetworkErrorToastSuppress();
    startOTAStatusPolling(jobIdRef.current, true);
  };

  const maskMessage = () => {
    switch (maskPhase) {
      case 'uploading':
        return t('sys.device_info.ota_uploading', '正在上传固件');
      case 'upgrading':
        return t('sys.device_info.ota_upgrading', '正在写入固件');
      case 'rebooting':
        return t('sys.device_info.ota_rebooting', '设备正在重启');
      case 'error':
        return t('sys.device_info.ota_failed', '固件更新失败');
      case 'offline':
        return t('sys.device_info.ota_offline_title', '设备未上线');
      default:
        return '';
    }
  };

  const maskHint = () => {
    switch (maskPhase) {
      case 'uploading':
        return t(
          'sys.device_info.ota_uploading_hint',
          '请勿关闭页面或断开电源'
        );
      case 'upgrading':
        return t('sys.device_info.ota_upgrading_hint', '固件写入中，请勿断电');
      case 'rebooting':
        return t(
          'sys.device_info.ota_rebooting_hint',
          '请稍候，正在等待设备上线'
        );
      default:
        return undefined;
    }
  };

  return (
    <>
      <Dialog open={open} onOpenChange={handleOpenChange}>
        <DialogContent className="max-w-md bg-card border-border shadow-xl">
          <DialogHeader>
            <DialogTitle className="text-lg font-bold">
              {t('sys.device_info.firmware_update', '固件升级')}
            </DialogTitle>
          </DialogHeader>

          <div className="py-2">
            <FileUpload
              single
              value={file ? [file] : []}
              onChange={handleFileChange}
              accept={{
                'application/gzip': ['.tar.gz'],
                'application/x-gzip': ['.tar.gz'],
              }}
              placeholder={t(
                'sys.device_info.upload_hint',
                '点击或拖拽文件上传'
              )}
              hint={t(
                'sys.device_info.upload_support',
                '支持 .tar.gz 固件包格式'
              )}
              disabled={uploading}
              showFileList
            />
          </div>

          {file && (
            <div className="flex items-start space-x-2 rounded-md border border-amber-500/30 bg-amber-500/5 p-3">
              <ShieldAlert className="w-5 h-5 text-amber-500 shrink-0 mt-0.5" />
              <div className="flex-1 space-y-1">
                <Label
                  htmlFor="firmware-confirm"
                  className="text-sm font-medium cursor-pointer leading-tight"
                >
                  {t(
                    'sys.device_info.firmware_confirm_text',
                    '固件升级期间设备将自动重启，请确保供电稳定，中断可能导致系统无法启动'
                  )}
                </Label>
                <div className="flex items-center space-x-2 pt-1">
                  <Checkbox
                    id="firmware-confirm"
                    checked={confirmed}
                    onCheckedChange={checked => setConfirmed(!!checked)}
                    disabled={uploading}
                  />
                  <Label
                    htmlFor="firmware-confirm"
                    className="text-xs text-muted-foreground cursor-pointer"
                  >
                    {t(
                      'sys.device_info.firmware_confirm_ack',
                      '确认并继续升级'
                    )}
                  </Label>
                </div>
              </div>
            </div>
          )}

          <DialogFooter>
            <Button
              variant="outline"
              onClick={() => handleOpenChange(false)}
              disabled={uploading}
              className="border-border font-medium"
            >
              {t('common.cancel', '取消')}
            </Button>
            <Button
              onClick={handleUpgrade}
              disabled={!file || !confirmed || uploading}
              className="bg-foreground hover:bg-foreground/90 text-background font-medium"
            >
              {uploading && <Loader2 className="w-4 h-4 mr-2 animate-spin" />}
              {t('sys.device_info.confirm_upgrade_btn', '确认升级')}
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <SystemLoadingMask
        open={maskPhase !== null}
        message={maskMessage()}
        hint={maskHint()}
        error={maskPhase === 'error' || maskPhase === 'offline'}
        errorMessage={
          maskErrorMessage
          || (maskPhase === 'offline'
            ? t(
                'sys.device_info.ota_offline_desc',
                '设备未在预期时间内重新上线，请检查网络连接与供电后重试'
              )
            : t('sys.device_info.ota_failed', '固件更新失败'))
        }
        actionLabel={
          maskPhase === 'offline'
            ? t('sys.device_info.ota_retry_detect', '重新检测')
            : undefined
        }
        onAction={maskPhase === 'offline' ? handleRetryDetect : undefined}
        onClick={
          maskPhase === 'error' || maskPhase === 'offline'
            ? handleDismissError
            : undefined
        }
      />
    </>
  );
}
