import { useState, useCallback, useRef, useEffect, useMemo } from 'react';
import { useTranslation } from 'react-i18next';
import { HardDriveUpload, Loader2, ShieldAlert } from 'lucide-react';
import { toast } from 'sonner';
import FileUpload from '@/components/file-upload';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Checkbox } from '@/components/ui/checkbox';
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogContent,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '@/components/ui/alert-dialog';
import {
  Dialog,
  DialogContent,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog';
import { Label } from '@/components/ui/label';
import { Progress } from '@/components/ui/progress';
import SystemLoadingMask from '@/components/system-loading-mask';
import {
  systemApi,
  type OSUpgradeStrategyOption,
  type OSUpgradeStrategy,
  type OSUpgradeStatus,
  type OSUpgradeUpdateModeOption,
} from '@/services/api/system';
import { enterNetworkErrorToastSuppress } from '@/services/request';
import { getItem, removeItem, setItem } from '@/utils/storage';
import { startPolling, type PollingHandle } from '@/utils/polling';

export interface OsUpgradeDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
}

type MaskPhase =
  | 'installing'
  | 'rebooting'
  | 'verifying'
  | 'offline'
  | 'error'
  | null;

// Job states treated as a hard failure in both the install and reboot phases.
const installFailureStates = ['failed', 'cancelled', 'rollback'];
const terminalStates: OSUpgradeStatus['status'][] = [
  'success',
  'failed',
  'cancelled',
];
const persistedMaskKey = 'aipc.os-upgrade.mask';
const dualRebootTimeoutMs = 10 * 60 * 1000;
const singleRecoveryRebootTimeoutMs = 30 * 60 * 1000;
const persistedMaskMaxAgeMs = 2 * 60 * 60 * 1000;
const rebootRetryIntervalMs = 15 * 1000;

type PersistedMaskState = {
  jobId: string;
  phase: Exclude<MaskPhase, null>;
  upgradeMode?: OSUpgradeStatus['upgrade_mode'];
  rebootTriggered?: boolean;
  savedAt: number;
};

const pollingTimeoutFor = (upgradeMode?: OSUpgradeStatus['upgrade_mode']) => (upgradeMode === 'single-recovery'
    ? singleRecoveryRebootTimeoutMs
    : dualRebootTimeoutMs);

const maskPhaseForActiveStatus = (
  state?: OSUpgradeStatus['status']
): MaskPhase => {
  switch (state) {
    case 'installing':
    case 'installed':
      return 'installing';
    case 'awaiting_reboot':
    case 'rebooting':
      return 'rebooting';
    case 'verifying':
      return 'verifying';
    default:
      return null;
  }
};

const isTerminalState = (state?: OSUpgradeStatus['status']) => !!state && terminalStates.includes(state);

const getStrategyOption = (
  st: OSUpgradeStatus,
  strategy: OSUpgradeStrategy
): OSUpgradeStrategyOption | undefined => st.available_upgrade_strategies?.find(item => item.strategy === strategy);

const getUpdateModeOptions = (
  st: OSUpgradeStatus
): OSUpgradeUpdateModeOption[] => st.available_update_mode_options
  ?? (st.available_update_modes ?? []).map(mode => ({
    mode,
    supported: true,
  }));

const getUpdateModeOption = (
  st: OSUpgradeStatus,
  mode: string
): OSUpgradeUpdateModeOption | undefined => getUpdateModeOptions(st).find(item => item.mode === mode);

const getDefaultUpdateMode = (st: OSUpgradeStatus) => {
  const options = getUpdateModeOptions(st);
  return (
    options.find(item => item.default && item.supported)?.mode
    || options.find(item => item.mode === 'copy-a' && item.supported)?.mode
    || options.find(item => item.supported)?.mode
    || ''
  );
};

const hasStrategyCapabilities = (st: OSUpgradeStatus) => !!st.available_upgrade_strategies?.length
  || st.supports_standard_upgrade !== undefined
  || st.supports_full_upgrade !== undefined;

const isStrategySupported = (
  st: OSUpgradeStatus,
  strategy: OSUpgradeStrategy
) => {
  const option = getStrategyOption(st, strategy);
  if (option) return option.supported;
  if (strategy === 'standard') return !!st.supports_standard_upgrade;
  return hasStrategyCapabilities(st) ? !!st.supports_full_upgrade : true;
};

export function OsUpgradeDialog({ open, onOpenChange }: OsUpgradeDialogProps) {
  const { t } = useTranslation();

  // Backend job state — driven by upload/validate responses and the open
  // refresh, so the echoed package survives close/reopen.
  const [jobId, setJobId] = useState('');
  const [status, setStatus] = useState<OSUpgradeStatus>({ status: 'idle' });
  const [file, setFile] = useState<File | null>(null);
  const [uploadProgress, setUploadProgress] = useState(0);
  // True only while the on-select upload+validate is in flight. The mask has
  // its own phase and runs with the dialog closed.
  const [busy, setBusy] = useState(false);
  const [confirmed, setConfirmed] = useState(false);
  const [advancedMode, setAdvancedMode] = useState(false);
  const [selectedUpdateMode, setSelectedUpdateMode] = useState('');
  const [advancedConfirmOpen, setAdvancedConfirmOpen] = useState(false);

  const [maskPhase, setMaskPhase] = useState<MaskPhase>(null);
  const [maskErrorMessage, setMaskErrorMessage] = useState('');
  const pollingRef = useRef<PollingHandle | null>(null);
  // Guards against re-triggering the reboot call across polls while the
  // status lingers on awaiting_reboot before the device actually drops.
  const rebootTriggeredRef = useRef(false);
  const lastRebootAttemptAtRef = useRef(0);
  const restoredPersistedMaskRef = useRef(false);
  const releaseNetworkErrorSuppressRef = useRef<(() => void) | null>(null);

  const releaseNetworkErrorSuppress = useCallback(() => {
    releaseNetworkErrorSuppressRef.current?.();
    releaseNetworkErrorSuppressRef.current = null;
  }, []);

  const cleanupPolling = useCallback(() => {
    pollingRef.current?.stop();
    pollingRef.current = null;
  }, []);

  const clearPersistedMask = useCallback(() => {
    removeItem(persistedMaskKey);
  }, []);

  const resetPackageState = useCallback(() => {
    setFile(null);
    setJobId('');
    setConfirmed(false);
    setAdvancedMode(false);
    setSelectedUpdateMode('');
    setStatus({ status: 'idle' });
    clearPersistedMask();
  }, [clearPersistedMask]);

  const cleanupTerminalPackage = useCallback((id: string) => {
    if (!id) return;
    systemApi.osUpgradeDeletePackage(id, true).catch(() => {});
  }, []);

  const persistMask = useCallback(
    (phase: Exclude<MaskPhase, null>, id = jobId) => {
      if (!id) return;
      const previous = getItem<PersistedMaskState>(persistedMaskKey);
      setItem<PersistedMaskState>(persistedMaskKey, {
        jobId: id,
        phase,
        upgradeMode: status.upgrade_mode || previous?.upgradeMode,
        rebootTriggered: rebootTriggeredRef.current,
        savedAt: Date.now(),
      });
    },
    [jobId, status.upgrade_mode]
  );

  useEffect(() => {
    if (!maskPhase || !jobId) return;
    persistMask(maskPhase);
  }, [jobId, maskPhase, persistMask]);

  useEffect(
    () => () => {
      cleanupPolling();
      releaseNetworkErrorSuppress();
    },
    [cleanupPolling, releaseNetworkErrorSuppress]
  );

  // Always query the active job — used both on open (to echo any staged
  // package) and after a failed upload/validate (to sync the backend state).
  const refresh = useCallback(async (silent = true) => {
    try {
      const resp: any = await systemApi.osUpgradeStatus(undefined, silent);
      const st: OSUpgradeStatus | undefined = resp?.data;
      if (st) {
        if (st.status === 'success') {
          cleanupTerminalPackage(st.job_id || '');
          resetPackageState();
          return;
        }
        setStatus(st);
        setJobId(st.job_id || '');
        if (st.status === 'ready') {
          setSelectedUpdateMode(current => current || getDefaultUpdateMode(st));
        }
      }
    } catch {
      // Device unreachable / no active job — leave the current status.
    }
  }, [cleanupTerminalPackage, resetPackageState]);

  useEffect(() => {
    if (open) {
      setConfirmed(false);
      setBusy(false);
      if (!maskPhase) {
        rebootTriggeredRef.current = false;
      }
      refresh(true);
    }
  }, [maskPhase, open, refresh]);

  // Cancel then delete while the package is only staged. Once the job is
  // terminal, the backend intentionally rejects cancel; just remove the package.
  const teardownPackage = useCallback(
    async (id: string, state?: OSUpgradeStatus['status']) => {
      if (!id) return;
      if (!isTerminalState(state)) {
        try {
          await systemApi.osUpgradeCancel(id, true);
        } catch {
          // Not cancellable in the current state — DELETE below is best-effort.
        }
      }
      try {
        await systemApi.osUpgradeDeletePackage(id, true);
      } catch {
        // Best-effort cleanup; the package may be active or already removed.
      }
    },
    []
  );

  const removePackage = useCallback(async () => {
    const id = jobId;
    const state = status.status;
    resetPackageState();
    await teardownPackage(id, state);
  }, [jobId, resetPackageState, status.status, teardownPackage]);

  const uploadAndValidate = useCallback(
    async (target: File, prevId?: string) => {
      setBusy(true);
      setStatus({ status: 'uploading' });
      setUploadProgress(0);
      // Free the backend slot from a previous package before uploading a new
      // one — the backend rejects a second upload while a job is active.
      if (prevId) {
        setJobId('');
        await teardownPackage(prevId);
      }
      let id = '';
      try {
        const uploadResp: any = await systemApi.osUpgradeUpload(
          target,
          setUploadProgress
        );
        const uploaded: OSUpgradeStatus | undefined = uploadResp?.data;
        id = uploaded?.job_id || '';
        if (!uploaded || !id) throw new Error('missing job_id');
        setJobId(id);
        setStatus(uploaded);
        const validateResp: any = await systemApi.osUpgradeValidate(id);
        const validated: OSUpgradeStatus | undefined = validateResp?.data;
        if (!validated) throw new Error('validate failed');
        setStatus(validated);
        setAdvancedMode(false);
        setSelectedUpdateMode(getDefaultUpdateMode(validated));
      } catch {
        if (id) {
          // The backend failed the job — sync its recorded error/message.
          await refresh(true);
        } else {
          setStatus({
            status: 'failed',
            error: t(
              'sys.device_info.os_upgrade_upload_failed',
              '升级包上传失败，请重试'
            ),
          });
        }
      } finally {
        setBusy(false);
      }
    },
    [refresh, t, teardownPackage]
  );

  const handleFileChange = (files: File[]) => {
    if (files.length === 0) {
      // X clicked — tear down the backend package so the slot is free.
      removePackage();
      return;
    }
    const selected = files[0];
    if (!selected.name.toLowerCase().endsWith('.swu')) {
      toast.error(t('sys.device_info.invalid_os_package', '请选择 .swu 文件'));
      return;
    }
    setFile(selected);
    setConfirmed(false);
    setAdvancedMode(false);
    setSelectedUpdateMode('');
    uploadAndValidate(selected, jobId);
  };

  // 取消 (footer) keeps the backend package so it can be echoed on reopen;
  // only the local selection is cleared. X on the file is the delete path.
  const handleOpenChange = (nextOpen: boolean) => {
    if (!nextOpen) {
      setFile(null);
      setConfirmed(false);
      setUploadProgress(0);
    }
    onOpenChange(nextOpen);
  };

  const requestRebootOnce = useCallback((id: string) => {
    const now = Date.now();
    if (now - lastRebootAttemptAtRef.current < rebootRetryIntervalMs) {
      return;
    }
    lastRebootAttemptAtRef.current = now;
    // Fire-and-forget. If this request is lost because the device starts going
    // down, the status poll retries while the backend remains awaiting_reboot.
    systemApi.osUpgradeReboot(id).catch(() => {});
  }, []);

  // Poll the authoritative backend job status through the whole flow:
  // install → awaiting_reboot → (trigger reboot) → rebooting → verifying →
  // success/failed. The terminal state is keyed off st.status ONLY — never
  // off /health — because /health can stay non-200 across the entire verify
  // window (slow camera-daemon start, TLS regen, or an empty .network) which
  // is exactly what stuck the mask on "rebooting" in the first place. The
  // backend only sets 'success' after its own ~60s stability check, so when
  // it reports success the device is genuinely online.
  const startInstallRebootPolling = useCallback(
    (id: string, upgradeMode?: OSUpgradeStatus['upgrade_mode']) => {
      pollingRef.current = startPolling({
        fn: async () => {
          try {
            const resp: any = await systemApi.osUpgradeStatus(id, true);
            return {
              online: true,
              st: resp?.data as OSUpgradeStatus | undefined,
            };
          } catch {
            // Device unreachable (reboot in progress / briefly offline).
            // Absorb and keep polling; status is authoritative once it is
            // back — never treat a network blip as a terminal failure.
            return { online: false, st: undefined };
          }
        },
        onSuccess: ({ online, st }) => {
          // --- Install phase: before we've triggered reboot. ---
          if (!rebootTriggeredRef.current) {
            if (st && installFailureStates.includes(st.status)) {
              cleanupPolling();
              releaseNetworkErrorSuppress();
              setMaskErrorMessage(
                st.error
                  || st.message
                  || t('sys.device_info.os_upgrade_failed', '系统升级失败')
              );
              setMaskPhase('error');
              return true;
            }
            // Backend reached success without needing a reboot (no-op /
            // already on target). Close the loop immediately.
            if (st?.status === 'success') {
              cleanupPolling();
              releaseNetworkErrorSuppress();
              cleanupTerminalPackage(id);
              resetPackageState();
              setMaskPhase(null);
              toast.success(
                t(
                  'sys.device_info.os_upgrade_complete',
                  '系统升级完成，设备已上线'
                )
              );
              return true;
            }
            if (st?.status === 'awaiting_reboot') {
              rebootTriggeredRef.current = true;
              setMaskPhase('rebooting');
              requestRebootOnce(id);
            }
            return false;
          }

          // --- Reboot phase: st.status is authoritative. ---
          if (!online || !st) {
            // Still rebooting / offline — keep waiting for it to come back.
            setMaskPhase('rebooting');
            return false;
          }
          if (st.status === 'success') {
            cleanupPolling();
            releaseNetworkErrorSuppress();
            cleanupTerminalPackage(id);
            resetPackageState();
            setMaskPhase(null);
            toast.success(
              t(
                'sys.device_info.os_upgrade_complete',
                '系统升级完成，设备已上线'
              )
            );
            return true;
          }
          if (installFailureStates.includes(st.status)) {
            // rollback/failed/cancelled after reboot — surface the backend
            // reason instead of misreporting success (the old health-only
            // path reported a rolled-back device as "upgrade complete").
            cleanupPolling();
            releaseNetworkErrorSuppress();
            setMaskErrorMessage(
              st.error
                || st.message
                || t('sys.device_info.os_upgrade_failed', '系统升级失败')
            );
            setMaskPhase('error');
            return true;
          }
          if (st.status === 'verifying') {
            setMaskPhase('verifying');
            return false;
          }
          if (st.status === 'awaiting_reboot') {
            // The first reboot request may have been dropped by a transient
            // network close. Retry at a low rate while the backend still says
            // it has not entered rebooting.
            requestRebootOnce(id);
          }
          // rebooting / awaiting_reboot / installed / installing — still
          // cycling through the reboot, keep the rebooting mask.
          setMaskPhase('rebooting');
          return false;
        },
        interval: 3000,
        timeout: pollingTimeoutFor(upgradeMode),
        onTimeout: () => {
          cleanupPolling();
          releaseNetworkErrorSuppress();
          // Device did not reach a terminal state in the expected window.
          // Offer a manual re-detect instead of a dead-end "timeout".
          setMaskPhase('offline');
        },
        onError: () => {
          // fn absorbs all errors above; this is belt-and-braces only.
        },
      });
    },
    [
      cleanupPolling,
      cleanupTerminalPackage,
      releaseNetworkErrorSuppress,
      requestRebootOnce,
      resetPackageState,
      t,
    ]
  );

  useEffect(() => {
    if (restoredPersistedMaskRef.current) return;
    restoredPersistedMaskRef.current = true;

    const persisted = getItem<PersistedMaskState>(persistedMaskKey);
    if (!persisted?.jobId || !persisted.phase) return;
    if (Date.now() - persisted.savedAt > persistedMaskMaxAgeMs) {
      clearPersistedMask();
      return;
    }

    setJobId(persisted.jobId);
    setMaskPhase(persisted.phase);
    rebootTriggeredRef.current =      !!persisted.rebootTriggered
      || persisted.phase === 'rebooting'
      || persisted.phase === 'verifying'
      || persisted.phase === 'offline';
    lastRebootAttemptAtRef.current = 0;
    releaseNetworkErrorSuppressRef.current = enterNetworkErrorToastSuppress();
    startInstallRebootPolling(persisted.jobId, persisted.upgradeMode);
  }, [clearPersistedMask, startInstallRebootPolling]);

  useEffect(() => {
    if (maskPhase || !jobId) return;
    const phase = maskPhaseForActiveStatus(status.status);
    if (!phase) return;

    setMaskPhase(phase);
    rebootTriggeredRef.current =      status.status === 'awaiting_reboot'
      || status.status === 'rebooting'
      || status.status === 'verifying';
    lastRebootAttemptAtRef.current = 0;
    releaseNetworkErrorSuppressRef.current = enterNetworkErrorToastSuppress();
    startInstallRebootPolling(jobId, status.upgrade_mode);
  }, [
    jobId,
    maskPhase,
    startInstallRebootPolling,
    status.status,
    status.upgrade_mode,
  ]);

  const canStartUpgrade = () => {
    const selectedModeOption = getUpdateModeOption(status, selectedUpdateMode);
    const advancedModeSelected = !!selectedModeOption;
    const strategySupported = advancedMode
      ? advancedModeSelected
      : isStrategySupported(status, 'standard');
    return (
      !!jobId
      && confirmed
      && status.status === 'ready'
      && strategySupported
      && (!advancedMode || !!selectedUpdateMode)
    );
  };

  const startUpgrade = async () => {
    if (!canStartUpgrade()) {
      return;
    }

    cleanupPolling();
    releaseNetworkErrorSuppress();
    clearPersistedMask();
    releaseNetworkErrorSuppressRef.current = enterNetworkErrorToastSuppress();
    rebootTriggeredRef.current = false;
    lastRebootAttemptAtRef.current = 0;

    // Upload + validate already happened on file select; the mask only
    // covers install -> reboot.
    setMaskPhase('installing');
    onOpenChange(false);

    try {
      await systemApi.osUpgradeInstall(
        jobId,
        advancedMode ? undefined : 'standard',
        advancedMode ? selectedUpdateMode : undefined
      );
    } catch {
      releaseNetworkErrorSuppress();
      setMaskErrorMessage(
        t('sys.device_info.os_upgrade_install_failed', '启动升级失败，请重试')
      );
      setMaskPhase('error');
      return;
    }
    startInstallRebootPolling(jobId, status.upgrade_mode);
  };

  const handleUpgrade = () => {
    if (!canStartUpgrade()) {
      return;
    }
    if (advancedMode) {
      setAdvancedConfirmOpen(true);
      return;
    }
    startUpgrade();
  };

  const handleAdvancedConfirm = () => {
    setAdvancedConfirmOpen(false);
    startUpgrade();
  };

  const handleDismissError = () => {
    cleanupPolling();
    releaseNetworkErrorSuppress();
    clearPersistedMask();
    setMaskPhase(null);
    setMaskErrorMessage('');
  };

  // "重新检测": re-arm polling with a fresh deadline after the device failed
  // to come back online within the window. The job id is still valid across
  // the reboot window, so we resume reading its authoritative status.
  const handleRetryDetect = () => {
    if (!jobId) {
      handleDismissError();
      return;
    }
    cleanupPolling();
    releaseNetworkErrorSuppress();
    setMaskErrorMessage('');
    setMaskPhase('rebooting');
    releaseNetworkErrorSuppressRef.current = enterNetworkErrorToastSuppress();
    startInstallRebootPolling(jobId, status.upgrade_mode);
  };

  // Echo a previously uploaded package (e.g. after close/reopen) so the file
  // is visible and its X button can delete it from the backend.
  const echoFile = useMemo(
    () => (status.file_name
        ? ({
            name: status.file_name,
            size: status.file_size ?? 0,
          } as unknown as File)
        : null),
    [status.file_name, status.file_size]
  );
  const displayFile = file ?? echoFile;
  const packageReady = status.status === 'ready' && !!jobId;
  const standardStrategyOption = getStrategyOption(status, 'standard');
  const standardUpgradeSupported = isStrategySupported(status, 'standard');
  const updateModeOptions = getUpdateModeOptions(status);
  const selectedModeOption = getUpdateModeOption(status, selectedUpdateMode);
  const selectedStrategySupported = advancedMode
    ? !!selectedModeOption
    : standardUpgradeSupported;
  const ready = packageReady && selectedStrategySupported;

  const maskMessage = () => {
    switch (maskPhase) {
      case 'installing':
        return t('sys.device_info.os_upgrade_installing', '正在写入系统');
      case 'rebooting':
        return t('sys.device_info.os_upgrade_rebooting', '设备正在重启');
      case 'verifying':
        return t('sys.device_info.os_upgrade_verifying', '正在验收新系统');
      case 'offline':
        return t('sys.device_info.os_upgrade_offline_title', '设备未上线');
      case 'error':
        return t('sys.device_info.os_upgrade_failed', '系统升级失败');
      default:
        return '';
    }
  };

  const maskHint = () => {
    switch (maskPhase) {
      case 'installing':
        return t(
          'sys.device_info.os_upgrade_installing_hint',
          '系统写入中，请勿断电'
        );
      case 'rebooting':
        return t(
          'sys.device_info.os_upgrade_rebooting_hint',
          '请稍候，正在等待设备上线'
        );
      case 'verifying':
        return t(
          'sys.device_info.os_upgrade_verifying_hint',
          '正在校验新系统稳定性，请稍候'
        );
      case 'offline':
        return t(
          'sys.device_info.os_upgrade_offline_desc',
          '设备未在预期时间内重新上线，请检查网络连接与供电后重试'
        );
      default:
        return undefined;
    }
  };

  return (
    <>
      <Dialog open={open} onOpenChange={handleOpenChange}>
        <DialogContent
          className="max-w-md max-h-[85vh] flex flex-col gap-0 bg-card border-border shadow-xl p-0"
          // 升级流程敏感，禁止点击遮罩/四周关闭，避免误触中断
          onInteractOutside={e => e.preventDefault()}
          onPointerDownOutside={e => e.preventDefault()}
        >
          <DialogHeader className="p-6 pb-4">
            <DialogTitle className="flex items-center gap-2 text-lg font-bold">
              <HardDriveUpload className="h-5 w-5" />
              {t('sys.device_info.os_upgrade_title', '系统 OS 升级')}
            </DialogTitle>
          </DialogHeader>

          <div className="overflow-y-auto px-6 pb-2 flex-1 min-h-0 space-y-4">
            <FileUpload
              single
              value={displayFile ? [displayFile] : []}
              onChange={handleFileChange}
              accept={{ 'application/octet-stream': ['.swu'] }}
              placeholder={t(
                'sys.device_info.os_upgrade_upload_hint',
                '点击或拖拽选择 SWUpdate 镜像'
              )}
              hint={t(
                'sys.device_info.os_upgrade_upload_support',
                '仅支持适用于 hailo15-ne503 的 .swu 包'
              )}
              disabled={busy}
              showFileList
            />

            {status.status === 'uploading' && (
              <div className="space-y-1.5">
                <Progress value={uploadProgress} />
                <div className="flex justify-between text-xs text-muted-foreground">
                  <span>
                    {t('sys.device_info.os_upgrade_uploading', '正在上传')}
                  </span>
                  <span>{uploadProgress}%</span>
                </div>
              </div>
            )}

            {status.status === 'validating' && (
              <div className="flex items-center gap-2 text-xs text-muted-foreground">
                <Loader2 className="h-3.5 w-3.5 animate-spin" />
                <span>
                  {t('sys.device_info.os_upgrade_validating', '正在校验升级包')}
                </span>
              </div>
            )}

            {status.error && (
              <div className="flex gap-2 rounded-md border border-destructive/30 bg-destructive/5 p-3 text-sm text-destructive">
                <ShieldAlert className="h-5 w-5 shrink-0" />
                <span>{status.error}</span>
              </div>
            )}

            {displayFile && packageReady && (
              <div className="rounded-md border border-border bg-muted/20 p-3 space-y-3">
                <div className="space-y-0.5">
                  <div className="text-sm font-medium">
                    {t('sys.device_info.os_upgrade_strategy_title', '升级方式')}
                  </div>
                  <div className="text-xs text-muted-foreground">
                    {t(
                      'sys.device_info.os_upgrade_strategy_hint',
                      '默认使用推荐模式；需要维护/工厂步骤时再打开高级。'
                    )}
                  </div>
                </div>

                <div
                  className={`flex items-start gap-2 rounded-md border p-3 transition-colors ${
                    standardUpgradeSupported
                      ? 'border-foreground bg-background'
                      : 'cursor-not-allowed opacity-50'
                  }`}
                >
                  <HardDriveUpload className="mt-0.5 h-4 w-4 shrink-0 text-muted-foreground" />
                  <span className="space-y-0.5">
                    <span className="block text-sm font-medium">
                      {t(
                        'sys.device_info.os_upgrade_strategy_standard',
                        '推荐升级'
                      )}
                    </span>
                    {!standardUpgradeSupported
                      && standardStrategyOption?.reason && (
                        <span className="block text-xs text-destructive">
                          {standardStrategyOption.reason}
                        </span>
                      )}
                  </span>
                </div>

                {updateModeOptions.length > 0 && (
                  <div className="rounded-md border border-border/70 bg-background/50 p-3 space-y-3">
                    <div className="flex items-start gap-2">
                      <Checkbox
                        id="os-upgrade-advanced-mode"
                        checked={advancedMode}
                        onCheckedChange={checked => {
                          const enabled = !!checked;
                          setAdvancedMode(enabled);
                          if (enabled && !selectedUpdateMode) {
                            setSelectedUpdateMode(getDefaultUpdateMode(status));
                          }
                        }}
                        disabled={busy}
                      />
                      <div className="space-y-0.5">
                        <Label
                          htmlFor="os-upgrade-advanced-mode"
                          className="cursor-pointer text-sm font-medium"
                        >
                          {t(
                            'sys.device_info.os_upgrade_advanced_mode',
                            '高级：手动选择更新模式'
                          )}
                        </Label>
                        <div className="text-xs text-muted-foreground">
                          {t(
                            'sys.device_info.os_upgrade_advanced_mode_hint',
                            '仅限熟悉操作者使用，误操作可能导致无法启动，后果自负。'
                          )}
                        </div>
                      </div>
                    </div>

                    {advancedMode && (
                      <div className="space-y-2">
                        {updateModeOptions.map(option => (
                          <label
                            key={option.mode}
                            className={`flex items-start gap-2 rounded-md border p-2 transition-colors ${
                              busy
                                ? 'cursor-not-allowed opacity-70'
                                : 'cursor-pointer hover:bg-muted/40'
                            } ${
                              selectedUpdateMode === option.mode
                                ? 'border-foreground bg-background'
                                : 'border-border'
                            }`}
                          >
                            <input
                              type="radio"
                              name="os-upgrade-update-mode"
                              className="mt-1"
                              checked={selectedUpdateMode === option.mode}
                              disabled={busy}
                              onChange={() => setSelectedUpdateMode(option.mode)}
                            />
                            <span className="min-w-0 space-y-0.5">
                              <span className="flex flex-wrap items-center gap-1">
                                <Badge
                                  variant={
                                    option.default ? 'default' : 'secondary'
                                  }
                                  className="rounded-sm px-1.5 py-0 font-mono text-[10px]"
                                >
                                  {option.mode}
                                </Badge>
                                {option.default && (
                                  <span className="text-[10px] text-muted-foreground">
                                    {t('common.default', '默认')}
                                  </span>
                                )}
                              </span>
                              {!option.supported && option.reason && (
                                <span className="block text-xs text-destructive">
                                  {option.reason}
                                </span>
                              )}
                            </span>
                          </label>
                        ))}
                      </div>
                    )}
                  </div>
                )}
              </div>
            )}

            {displayFile && packageReady && (
              <div className="flex items-start space-x-2 rounded-md border border-amber-500/30 bg-amber-500/5 p-3">
                <ShieldAlert className="w-5 h-5 text-amber-500 shrink-0 mt-0.5" />
                <div className="flex-1 space-y-1">
                  <Label
                    htmlFor="os-upgrade-confirm"
                    className="text-sm font-medium cursor-pointer leading-tight"
                  >
                    {t(
                      'sys.device_info.os_upgrade_confirm_text',
                      '系统升级期间设备将自动重启，请确保供电稳定，中断可能导致系统无法启动'
                    )}
                  </Label>
                  <div className="flex items-center space-x-2 pt-1">
                    <Checkbox
                      id="os-upgrade-confirm"
                      checked={confirmed}
                      onCheckedChange={checked => setConfirmed(!!checked)}
                      disabled={busy}
                    />
                    <Label
                      htmlFor="os-upgrade-confirm"
                      className="text-xs text-muted-foreground cursor-pointer"
                    >
                      {t(
                        'sys.device_info.os_upgrade_confirm_ack',
                        '确认并继续升级'
                      )}
                    </Label>
                  </div>
                </div>
              </div>
            )}
          </div>

          <DialogFooter className="p-6 pt-4 mt-0">
            <Button
              variant="outline"
              onClick={() => handleOpenChange(false)}
              disabled={busy}
              className="border-border font-medium"
            >
              {t('common.cancel', '取消')}
            </Button>
            <Button
              onClick={handleUpgrade}
              disabled={!ready || !confirmed || busy}
              className="bg-foreground hover:bg-foreground/90 text-background font-medium"
            >
              {busy && <Loader2 className="w-4 h-4 mr-2 animate-spin" />}
              {t('sys.device_info.os_upgrade_confirm_update', '确认更新')}
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <AlertDialog
        open={advancedConfirmOpen}
        onOpenChange={setAdvancedConfirmOpen}
      >
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>
              {t(
                'sys.device_info.os_upgrade_advanced_confirm_title',
                '确认执行高级模式？'
              )}
            </AlertDialogTitle>
            <div className="space-y-2 text-sm text-muted-foreground">
              <div>
                {t(
                  'sys.device_info.os_upgrade_advanced_confirm_desc',
                  '将执行所选 SWU 更新模式，请确认后继续。'
                )}
              </div>
              {selectedUpdateMode && (
                <div className="flex items-center gap-2 text-foreground">
                  <span>
                    {t(
                      'sys.device_info.os_upgrade_advanced_selected_mode',
                      '已选择'
                    )}
                  </span>
                  <Badge className="rounded-sm px-1.5 py-0 font-mono text-[10px]">
                    {selectedUpdateMode}
                  </Badge>
                </div>
              )}
            </div>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <Button
              type="button"
              variant="outline"
              onClick={() => setAdvancedConfirmOpen(false)}
            >
              {t('common.cancel', '取消')}
            </Button>
            <AlertDialogAction
              type="button"
              variant="destructive"
              onClick={handleAdvancedConfirm}
            >
              {t(
                'sys.device_info.os_upgrade_advanced_confirm_action',
                '确认执行'
              )}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>

      <SystemLoadingMask
        open={maskPhase !== null}
        message={maskMessage()}
        hint={maskHint()}
        error={maskPhase === 'error' || maskPhase === 'offline'}
        errorMessage={
          maskErrorMessage
          || (maskPhase === 'offline'
            ? t(
                'sys.device_info.os_upgrade_offline_desc',
                '设备未在预期时间内重新上线，请检查网络连接与供电后重试'
              )
            : t('sys.device_info.os_upgrade_failed', '系统升级失败'))
        }
        actionLabel={
          maskPhase === 'offline'
            ? t('sys.device_info.os_upgrade_retry_detect', '重新检测')
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
