import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { FirmwareUpdateDialog } from './FirmwareUpdateDialog';

const mocks = vi.hoisted(() => ({
  enterNetworkErrorToastSuppress: vi.fn(() => vi.fn()),
  otaInstallFromPath: vi.fn(),
  otaParse: vi.fn(),
  polling: undefined as any,
  startPolling: vi.fn((options: any) => {
    mocks.polling = options;
    return { stop: vi.fn() };
  }),
  otaRedirect: {
    redirectToLoginAfterOTASuccess: vi.fn(),
    stashOTASuccessLoginMessage: vi.fn(),
  },
  toast: {
    error: vi.fn(),
    success: vi.fn(),
  },
}));

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (_key: string, fallback?: string) => fallback ?? _key,
  }),
}));

vi.mock('sonner', () => ({
  toast: mocks.toast,
}));

vi.mock('@/services/request', () => ({
  enterNetworkErrorToastSuppress: mocks.enterNetworkErrorToastSuppress,
}));

vi.mock('@/utils/otaLoginRedirect', () => mocks.otaRedirect);

vi.mock('@/services/api/system', () => ({
  systemApi: {
    otaInstallFromPath: mocks.otaInstallFromPath,
    otaParse: mocks.otaParse,
    otaStatus: vi.fn(),
  },
}));

vi.mock('@/utils/polling', () => ({
  startPolling: mocks.startPolling,
}));

vi.mock('@/components/file-upload', () => ({
  default: ({ disabled, onChange }: any) => (
    <button
      disabled={disabled}
      type="button"
      onClick={() => onChange([
          new File(['firmware'], 'aipc-test.tar.gz', {
            type: 'application/gzip',
          }),
        ])}
    >
      select firmware
    </button>
  ),
}));

vi.mock('@/components/system-loading-mask', () => ({
  default: ({
    actionLabel,
    error,
    errorMessage,
    hint,
    message,
    onAction,
    open,
  }: any) => (open ? (
      <div data-error={String(!!error)} data-testid="system-mask">
        <span>{message}</span>
        {hint && <span>{hint}</span>}
        {errorMessage && <span>{errorMessage}</span>}
        {actionLabel && (
          <button type="button" onClick={onAction}>
            {actionLabel}
          </button>
        )}
      </div>
    ) : null),
}));

vi.mock('@/components/ui/button', () => ({
  Button: ({ children, className: _className, variant: _variant, ...props }: any) => (
    <button {...props}>{children}</button>
  ),
}));

vi.mock('@/components/ui/checkbox', () => ({
  Checkbox: ({ checked, onCheckedChange, ...props }: any) => (
    <input
      {...props}
      aria-label="ack"
      checked={!!checked}
      type="checkbox"
      onChange={() => onCheckedChange?.(!checked)}
    />
  ),
}));

vi.mock('@/components/ui/dialog', () => ({
  Dialog: ({ children, open }: any) => (open ? <div>{children}</div> : null),
  DialogContent: ({ children }: any) => <div>{children}</div>,
  DialogFooter: ({ children }: any) => <div>{children}</div>,
  DialogHeader: ({ children }: any) => <div>{children}</div>,
  DialogTitle: ({ children }: any) => <h2>{children}</h2>,
}));

vi.mock('@/components/ui/label', () => ({
  Label: ({ children, htmlFor }: any) => <label htmlFor={htmlFor}>{children}</label>,
}));

const otaStatus = (patch: Record<string, unknown>) => ({
  error: '',
  finished_at: 1784800802,
  job_id: 'ota-regression',
  message: 'Firmware upgrade completed; rebooting',
  progress: 100,
  reboot_needed: false,
  start_time: 1784800577,
  status: 'success',
  version: 'v-test',
  ...patch,
});

describe('FirmwareUpdateDialog OTA reboot polling', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    mocks.polling = undefined;
    mocks.otaParse.mockResolvedValue({
      data: { firmware_path: '/tmp/ota_firmware_pending.tar.gz' },
    });
    mocks.otaInstallFromPath.mockResolvedValue({
      data: { job_id: 'ota-regression' },
    });
  });

  it('ignores deploy hot-swap outage and finishes only after post-success reboot outage', async () => {
    render(<FirmwareUpdateDialog open onOpenChange={vi.fn()} />);

    fireEvent.click(screen.getByText('select firmware'));
    fireEvent.click(screen.getByLabelText('ack'));
    await act(async () => {
      fireEvent.click(screen.getByText('确认升级'));
    });

    await waitFor(() => expect(mocks.startPolling).toHaveBeenCalledTimes(1));

    act(() => {
      mocks.polling.onError(new Error('platform-api hot-swap'));
    });

    expect(screen.getByTestId('system-mask')).toHaveTextContent('正在写入固件');
    expect(mocks.toast.success).not.toHaveBeenCalled();

    let done = false;
    act(() => {
      done = mocks.polling.onSuccess({
        status: otaStatus({
          boot_id: 'same-boot',
          reboot_confirmed: true,
          reboot_needed: false,
        }),
      });
    });

    expect(done).toBe(false);
    expect(screen.getByTestId('system-mask')).toHaveTextContent('设备正在重启');
    expect(
      mocks.otaRedirect.redirectToLoginAfterOTASuccess
    ).not.toHaveBeenCalled();

    act(() => {
      done = mocks.polling.onSuccess({
        status: otaStatus({
          boot_id: 'same-boot',
          reboot_confirmed: true,
          reboot_needed: false,
        }),
      });
    });

    expect(done).toBe(false);
    expect(
      mocks.otaRedirect.redirectToLoginAfterOTASuccess
    ).not.toHaveBeenCalled();

    act(() => {
      mocks.polling.onError(new Error('device rebooting'));
    });
    act(() => {
      done = mocks.polling.onSuccess({
        status: otaStatus({
          reboot_confirmed: true,
          reboot_needed: false,
        }),
      });
    });

    expect(done).toBe(true);
    expect(mocks.otaRedirect.stashOTASuccessLoginMessage).toHaveBeenCalledWith(
      '固件升级完成，请重新登录'
    );
    expect(
      mocks.otaRedirect.redirectToLoginAfterOTASuccess
    ).toHaveBeenCalledTimes(1);
  });

  it('accepts backend boot-id proof when the reboot happened between polls', async () => {
    render(<FirmwareUpdateDialog open onOpenChange={vi.fn()} />);

    fireEvent.click(screen.getByText('select firmware'));
    fireEvent.click(screen.getByLabelText('ack'));
    await act(async () => {
      fireEvent.click(screen.getByText('确认升级'));
    });

    await waitFor(() => expect(mocks.startPolling).toHaveBeenCalledTimes(1));

    let done = false;
    act(() => {
      done = mocks.polling.onSuccess({
        status: otaStatus({
          boot_id: 'previous-boot',
          current_boot_id: 'current-boot',
          reboot_confirmed: true,
          reboot_needed: false,
        }),
      });
    });

    expect(done).toBe(true);
    expect(mocks.otaRedirect.stashOTASuccessLoginMessage).toHaveBeenCalledWith(
      '固件升级完成，请重新登录'
    );
    expect(
      mocks.otaRedirect.redirectToLoginAfterOTASuccess
    ).toHaveBeenCalledTimes(1);
  });
});
