import { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { Loader2, ShieldCheck, AlertTriangle, ArrowLeft } from 'lucide-react';
import { toast } from 'sonner';
import FileUpload from '@/components/file-upload';
import { Button } from '@/components/ui/button';
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog';
import { enterNetworkErrorToastSuppress } from '@/services/request';
import { startPolling, type PollingHandle } from '@/utils/polling';
import { systemApi } from '@/services/api/system';
import { cn } from '@/lib/utils';

export interface ImportFileTransferDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  title: string;
  description: string;
  /** react-dropzone accept map, e.g. { 'application/gzip': ['.tar.gz', '.tgz'] }. */
  accept: Record<string, string[]>;
  /** Max upload size in bytes. */
  maxSize: number;
  /** Impact disclosure bullets shown before confirm. */
  effects: string[];
  /** 'mid' = amber confirm, 'high' = destructive red confirm. */
  risk: 'mid' | 'high';
  /** Clone-only: items the target keeps (password / certs / device name / apps / models). */
  identityPreserved?: string[];
  /** Clone-only: platform-api self-restarts → suppress network toasts + health-poll reconnect. */
  criticalTransition?: boolean;
  /** Performs the multipart POST (backupApi.importMediaBundle / importClone). */
  onImport: (file: File) => Promise<void>;
  successMessage: string;
}

function formatSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

// Single-layer dialog: the confirm step is an in-dialog state change, NOT a
// second stacked portal. The previous design rendered a second AlertDialog
// portal (z-9999) as a sibling of the outer Radix Dialog; the stacked overlays
// could swallow the confirm-button click so runImport never fired
// ("import button does nothing, no request sent"). One dialog, two steps.
type Step = 'select' | 'confirm';

export default function ImportFileTransferDialog({
  open,
  onOpenChange,
  title,
  description,
  accept,
  maxSize,
  effects,
  risk,
  identityPreserved,
  criticalTransition = false,
  onImport,
  successMessage,
}: ImportFileTransferDialogProps) {
  const { t } = useTranslation();
  const [file, setFile] = useState<File | null>(null);
  const [importing, setImporting] = useState(false);
  const [step, setStep] = useState<Step>('select');

  const releaseRef = useRef<(() => void) | null>(null);
  const pollingRef = useRef<PollingHandle | null>(null);
  const startTimerRef = useRef<number | null>(null);

  const release = () => {
    releaseRef.current?.();
    releaseRef.current = null;
  };

  // Never leak a suppress transition or a dangling poll across mounts.
  useEffect(
    () => () => {
      pollingRef.current?.stop();
      pollingRef.current = null;
      if (startTimerRef.current !== null) {
        window.clearTimeout(startTimerRef.current);
        startTimerRef.current = null;
      }
      release();
    },
    []
  );

  // Reset to the file-pick step whenever the dialog closes.
  useEffect(() => {
    if (!open) {
      setFile(null);
      setStep('select');
    }
  }, [open]);

  const handleOpenChange = (next: boolean) => {
    if (!next && importing) return; // block dismiss mid-import
    onOpenChange(next);
  };

  const runImport = async () => {
    if (!file) return;
    setImporting(true);
    if (criticalTransition) {
      release();
      releaseRef.current = enterNetworkErrorToastSuppress();
    }
    try {
      await onImport(file);
      toast.success(successMessage);
      if (criticalTransition) {
        // platform-api self-restarts shortly after the response ships. Hold the
        // suppress shield and health-poll until it is back; the session token
        // survives the clone (platform-api.yaml is skipped) so no re-login.
        toast.info(
          t('maintenance.backup.reconnecting', 'Applying — reconnecting…')
        );
        startTimerRef.current = window.setTimeout(() => {
          pollingRef.current = startPolling({
            fn: () => systemApi.healthCheck({ silent: true }),
            onSuccess: () => {
              release();
              toast.success(t('maintenance.backup.reconnected', 'Reconnected'));
              return true;
            },
            interval: 3000,
            timeout: 120000,
            onTimeout: () => release(),
            onError: () => {
              /* still down — keep polling */
            },
          });
          startTimerRef.current = null;
        }, 5000);
      }
      setFile(null);
      setStep('select');
      onOpenChange(false);
    } catch (err) {
      if (criticalTransition) release();
      const msg =        (err as { response?: { data?: { message?: string } } })?.response?.data
          ?.message
        || (err as Error)?.message
        || t('common.error');
      toast.error(msg);
    } finally {
      setImporting(false);
    }
  };

  const isHigh = risk === 'high';

  return (
    <Dialog open={open} onOpenChange={handleOpenChange}>
      <DialogContent className="sm:max-w-lg">
        <DialogHeader>
          <DialogTitle>{title}</DialogTitle>
          <DialogDescription>{description}</DialogDescription>
        </DialogHeader>

        {step === 'select' ? (
          <div className="grid gap-4 py-1">
            <FileUpload
              single
              value={file ? [file] : []}
              onChange={files => setFile(files[0] ?? null)}
              accept={accept}
              maxSize={maxSize}
              loading={importing}
              disabled={importing}
              showFileList
              placeholder={t(
                'maintenance.backup.drop_hint',
                'Drag file here, or click to select'
              )}
            />

            {file && (
              <div className="rounded-lg border bg-muted/30 p-3 text-sm">
                <div className="flex items-center justify-between gap-4">
                  <span className="text-muted-foreground">
                    {t('maintenance.backup.file', 'File')}
                  </span>
                  <span className="truncate font-medium">{file.name}</span>
                </div>
                <div className="mt-1 flex items-center justify-between gap-4">
                  <span className="text-muted-foreground">
                    {t('maintenance.backup.size', 'Size')}
                  </span>
                  <span className="font-medium">{formatSize(file.size)}</span>
                </div>
              </div>
            )}

            {/* Impact disclosure */}
            <div className="rounded-lg border p-3">
              <p className="mb-2 text-xs font-medium uppercase tracking-wide text-muted-foreground">
                {t('maintenance.backup.impact', 'Impact')}
              </p>
              <ul className="space-y-1.5">
                {effects.map(e => (
                  <li
                    key={e}
                    className="flex items-start gap-2 text-sm text-muted-foreground"
                  >
                    <span
                      className={cn(
                        'mt-1.5 h-1.5 w-1.5 shrink-0 rounded-full',
                        isHigh ? 'bg-destructive' : 'bg-amber-500'
                      )}
                    />
                    <span>{e}</span>
                  </li>
                ))}
              </ul>
            </div>

            {/* Identity-preserved reassurance (clone only) */}
            {identityPreserved && identityPreserved.length > 0 && (
              <div className="rounded-lg border border-emerald-500/30 bg-emerald-500/[0.04] p-3">
                <p className="mb-2 flex items-center gap-1.5 text-xs font-medium text-emerald-600">
                  <ShieldCheck className="h-3.5 w-3.5" />
                  {t(
                    'maintenance.backup.identity_preserved',
                    'Preserved on target'
                  )}
                </p>
                <ul className="space-y-1">
                  {identityPreserved.map(e => (
                    <li key={e} className="text-sm text-muted-foreground">
                      • {e}
                    </li>
                  ))}
                </ul>
              </div>
            )}

            {isHigh && (
              <div className="flex items-start gap-2 rounded-lg border border-destructive/30 bg-destructive/5 p-3 text-sm text-destructive">
                <AlertTriangle className="mt-0.5 h-4 w-4 shrink-0" />
                <span>
                  {t(
                    'maintenance.backup.clone_disconnect_warn',
                    'This restarts platform-api — the UI will briefly disconnect, then reconnect automatically.'
                  )}
                </span>
              </div>
            )}
          </div>
        ) : (
          <div className="grid gap-4 py-1">
            <div
              className={cn(
                'flex items-start gap-2 rounded-lg p-3 text-sm',
                isHigh
                  ? 'border border-destructive/30 bg-destructive/5 text-destructive'
                  : 'border border-amber-500/30 bg-amber-500/[0.04] text-amber-700 dark:text-amber-500'
              )}
            >
              <AlertTriangle className="mt-0.5 h-4 w-4 shrink-0" />
              <span>
                {isHigh
                  ? t(
                      'maintenance.backup.confirm_high_desc',
                      'This overwrites the target device config and 4 state tables. Identity (password, certificates, device name, apps, models) is preserved. This action cannot be undone.'
                    )
                  : t(
                      'maintenance.backup.confirm_mid_desc',
                      'This overwrites the current media config and overlay images, then restarts the camera services. This action cannot be undone.'
                    )}
              </span>
            </div>

            {file && (
              <div className="rounded-lg border bg-muted/30 p-3 text-sm">
                <span className="text-muted-foreground">
                  {t('maintenance.backup.file', 'File')}:{' '}
                </span>
                <span className="font-medium">{file.name}</span>
                <span className="text-muted-foreground">
                  {' '}
                  ({formatSize(file.size)})
                </span>
              </div>
            )}
          </div>
        )}

        <DialogFooter>
          {step === 'select' ? (
            <>
              <Button
                variant="outline"
                onClick={() => handleOpenChange(false)}
                disabled={importing}
              >
                {t('common.cancel', 'Cancel')}
              </Button>
              <Button
                variant={isHigh ? 'destructive' : 'carbon'}
                onClick={() => setStep('confirm')}
                disabled={!file || importing}
              >
                {importing ? (
                  <Loader2 className="h-4 w-4 animate-spin" />
                ) : null}
                {t('maintenance.backup.next', 'Next')}
              </Button>
            </>
          ) : (
            <>
              <Button
                variant="outline"
                onClick={() => setStep('select')}
                disabled={importing}
              >
                <ArrowLeft className="h-4 w-4" />
                {t('common.back', 'Back')}
              </Button>
              <Button
                variant={isHigh ? 'destructive' : 'carbon'}
                onClick={runImport}
                disabled={importing}
                className={cn(
                  !isHigh
                    && 'bg-foreground text-background hover:bg-foreground/90'
                )}
              >
                {importing ? (
                  <Loader2 className="h-4 w-4 animate-spin" />
                ) : null}
                {t('maintenance.backup.confirm_action', 'Apply')}
              </Button>
            </>
          )}
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
