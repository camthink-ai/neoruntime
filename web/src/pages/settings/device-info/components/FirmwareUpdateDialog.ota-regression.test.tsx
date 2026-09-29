import {
  act,
  fireEvent,
  render,
  screen,
  waitFor,
} from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { FirmwareUpdateDialog } from './FirmwareUpdateDialog';

const mocks = vi.hoisted(() => {
  // One release fn per enterNetworkErrorToastSuppress() call, so tests can
  // assert the error paths hand the global toast suppress back.
  const releases: Array<ReturnType<typeof vi.fn>> = [];
  return {
    releases,
    enterNetworkErrorToastSuppress: vi.fn(() => {
      const release = vi.fn();
      releases.push(release);
      return release;
    }),
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
  };
});

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    // Supports both t(key, 'fallback') and t(key, { defaultValue, ...interp })
    // so interpolated compatibility-range strings render like production i18next.
    t: (key: string, opts?: unknown) => {
      if (typeof opts === 'string') return opts;
      if (opts !== null && typeof opts === 'object' && 'defaultValue' in opts) {
        const record = opts as Record<string, string>;
        return Object.entries(record)
          .filter(([name]) => name !== 'defaultValue')
          .reduce(
            (text, [name, value]) => text.split(`{{${name}}}`).join(String(value)),
            String(record.defaultValue)
          );
      }
      return key;
    },
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
  Button: ({
    children,
    className: _className,
    variant: _variant,
    ...props
  }: any) => <button {...props}>{children}</button>,
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
  Label: ({ children, htmlFor }: any) => (
    <label htmlFor={htmlFor}>{children}</label>
  ),
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

describe('FirmwareUpdateDialog parse compatibility gate', () => {
  const startUpgrade = async () => {
    render(<FirmwareUpdateDialog open onOpenChange={vi.fn()} />);

    fireEvent.click(screen.getByText('select firmware'));
    fireEvent.click(screen.getByLabelText('ack'));
    await act(async () => {
      fireEvent.click(screen.getByText('确认升级'));
    });
  };

  beforeEach(() => {
    vi.clearAllMocks();
    mocks.polling = undefined;
    mocks.releases.length = 0;
    mocks.otaParse.mockResolvedValue({
      data: {
        firmware_path: '/tmp/ota_firmware_pending.tar.gz',
        compatibility: { valid: true },
      },
    });
    mocks.otaInstallFromPath.mockResolvedValue({
      data: { job_id: 'ota-regression' },
    });
  });

  it('proceeds to install when parse reports a compatible package', async () => {
    await startUpgrade();

    await waitFor(() => expect(mocks.startPolling).toHaveBeenCalledTimes(1));
    expect(mocks.otaInstallFromPath).toHaveBeenCalledWith(
      '/tmp/ota_firmware_pending.tar.gz'
    );
  });

  it('stops before install when parse reports an incompatible package', async () => {
    mocks.otaParse.mockResolvedValue({
      data: {
        firmware_path: '/tmp/ota_firmware_pending.tar.gz',
        compatibility: {
          valid: false,
          error_code: 'APP_OS_VERSION_UNSUPPORTED',
          message: 'current OS 1.12.0 is outside the app range 1.10.0-1.11.0',
          os_version: '1.12.0',
          app_min_os_version: '1.10.0',
          app_max_os_version: '1.11.0',
        },
      },
    });

    await startUpgrade();

    await waitFor(() => expect(screen.getByTestId('system-mask')).toHaveTextContent(
        '应用包与当前系统不兼容'
      ));
    // The backend reason and the supported range stay visible in the mask.
    expect(screen.getByTestId('system-mask')).toHaveTextContent(
      '当前 OS 1.12.0，应用支持范围 1.10.0–1.11.0'
    );
    expect(screen.getByTestId('system-mask')).toHaveAttribute(
      'data-error',
      'true'
    );
    expect(mocks.otaInstallFromPath).not.toHaveBeenCalled();
    expect(mocks.startPolling).not.toHaveBeenCalled();
    expect(mocks.releases).toHaveLength(1);
    expect(mocks.releases[0]).toHaveBeenCalled();
  });

  it('surfaces the mutual-exclusion message when install is rejected during an OS upgrade', async () => {
    mocks.otaInstallFromPath.mockRejectedValue({
      data: {
        error: {
          detail:
            'an OS upgrade is in progress; retry after it finishes or is cancelled',
        },
      },
    });

    await startUpgrade();

    await waitFor(() => expect(screen.getByTestId('system-mask')).toHaveTextContent(
        'OS 升级正在进行中，应用固件安装被拒绝'
      ));
    expect(mocks.startPolling).not.toHaveBeenCalled();
    expect(mocks.releases[0]).toHaveBeenCalled();
  });

  it('falls back to the generic install failure for unrelated rejections', async () => {
    mocks.otaInstallFromPath.mockRejectedValue({
      data: { error: { detail: 'deploy script exited with status 1' } },
    });

    await startUpgrade();

    await waitFor(() => expect(screen.getByTestId('system-mask')).toHaveTextContent(
        '启动升级失败，请重试'
      ));
    expect(mocks.releases[0]).toHaveBeenCalled();
  });
});
