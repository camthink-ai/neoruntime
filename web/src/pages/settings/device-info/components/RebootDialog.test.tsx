import { act, fireEvent, render, screen } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { RebootDialog } from './RebootDialog';

const mocks = vi.hoisted(() => ({
  clearToken: vi.fn(),
  enterNetworkErrorToastSuppress: vi.fn(() => vi.fn()),
  polling: undefined as any,
  redirectToLoginAfterReboot: vi.fn(),
  restart: vi.fn(),
  startPolling: vi.fn((options: any) => {
    mocks.polling = options;
    return { stop: vi.fn() };
  }),
}));

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (_key: string, fallback?: string) => fallback ?? _key,
  }),
}));

vi.mock('lucide-react', () => ({
  Loader2: () => <span data-testid="loader" />,
}));

vi.mock('sonner', () => ({
  toast: { success: vi.fn() },
}));

vi.mock('@/services/request', () => ({
  enterNetworkErrorToastSuppress: mocks.enterNetworkErrorToastSuppress,
}));

vi.mock('@/services/api/system', () => ({
  systemApi: {
    healthCheck: vi.fn(),
    restart: mocks.restart,
  },
}));

vi.mock('@/store/auth', () => ({
  useAuthStore: {
    getState: () => ({ clearToken: mocks.clearToken }),
  },
}));

vi.mock('@/utils/polling', () => ({
  startPolling: mocks.startPolling,
}));
vi.mock('@/utils/rebootLoginRedirect', () => ({
  redirectToLoginAfterReboot: mocks.redirectToLoginAfterReboot,
}));

vi.mock('@/components/system-loading-mask', () => ({
  default: ({ message, open }: any) => (open ? <div data-testid="system-mask">{message}</div> : null),
}));

vi.mock('@/components/ui/alert-dialog', () => ({
  AlertDialog: ({ children, open }: any) => (open ? <div>{children}</div> : null),
  AlertDialogAction: ({ children, ...props }: any) => (
    <button type="button" {...props}>
      {children}
    </button>
  ),
  AlertDialogCancel: ({ children, ...props }: any) => (
    <button type="button" {...props}>
      {children}
    </button>
  ),
  AlertDialogContent: ({ children }: any) => <div>{children}</div>,
  AlertDialogDescription: ({ children }: any) => <div>{children}</div>,
  AlertDialogFooter: ({ children }: any) => <div>{children}</div>,
  AlertDialogHeader: ({ children }: any) => <div>{children}</div>,
  AlertDialogTitle: ({ children }: any) => <h2>{children}</h2>,
}));

describe('RebootDialog', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers();
    mocks.polling = undefined;
    mocks.restart.mockResolvedValue(undefined);
  });

  it('clears the stale session and redirects to login when the device is online', async () => {
    render(<RebootDialog open onOpenChange={vi.fn()} />);
    fireEvent.click(screen.getByRole('button', { name: '确认重启' }));

    await act(async () => {});
    vi.advanceTimersByTime(30_000);
    act(() => {});
    expect(mocks.startPolling).toHaveBeenCalledTimes(1);

    let shouldStop: boolean | undefined;
    act(() => {
      shouldStop = mocks.polling.onSuccess();
    });

    expect(shouldStop).toBe(true);
    expect(mocks.clearToken).toHaveBeenCalledTimes(1);
    expect(mocks.redirectToLoginAfterReboot).toHaveBeenCalledTimes(1);
    // The loading mask stays up through the navigation window — dropping it
    // synchronously would reopen the device-info query mid-reboot.
    expect(screen.queryByTestId('system-mask')).toBeInTheDocument();

    // The suppress transition is NOT released at onSuccess — only once the
    // document unloads for the /login navigation (pagehide), so the shield
    // covers the whole redirect window.
    const release = mocks.enterNetworkErrorToastSuppress.mock
      .results[0].value as ReturnType<typeof vi.fn>;
    expect(release).not.toHaveBeenCalled();

    act(() => {
      window.dispatchEvent(new Event('pagehide'));
    });
    expect(release).toHaveBeenCalledTimes(1);
  });
});
