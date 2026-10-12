import { fireEvent, render, screen } from '@testing-library/react';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { beforeEach, describe, expect, it, vi } from 'vitest';

import AppIncompatible from './index';
import { systemApi } from '@/services/api/system';
import { changeLanguage } from '@/i18n/utils';

vi.mock('react-i18next', () => ({
  // initReactI18next stub: the @/hooks barrel transitively loads the real
  // i18n config, which imports it from this module.
  initReactI18next: { type: '3rdParty', init: () => {} },
  useTranslation: () => ({
    i18n: { language: 'en' },
    // Same fallback + interpolation contract as the real t(): string
    // fallback wins, options carry defaultValue and {{var}} substitution.
    t: (key: string, opts?: unknown) => {
      if (typeof opts === 'string') return opts;
      const options = (opts ?? {}) as Record<string, string>;
      const base = options.defaultValue ?? key;
      return Object.entries(options).reduce(
        (text, [name, value]) =>
          typeof value === 'string'
            ? text.replaceAll(`{{${name}}}`, value)
            : text,
        base
      );
    },
  }),
}));

vi.mock('@/i18n/utils', () => ({
  changeLanguage: vi.fn(),
}));

vi.mock('lucide-react', () => ({
  ShieldAlert: () => <span data-testid="shield-alert" />,
  HardDriveUpload: () => <span data-testid="icon-upload" />,
  Globe: () => <span data-testid="icon-globe" />,
  ChevronDown: () => <span data-testid="icon-chevron" />,
}));

vi.mock('@/services/api/system', () => ({
  systemApi: {
    appCompatStatus: vi.fn(),
  },
}));

// The rescue dialog is the device-info page's own component with its own
// test coverage; stub it to a marker that reflects its open state.
vi.mock('@/pages/settings/device-info/components/FirmwareUpdateDialog', () => ({
  FirmwareUpdateDialog: ({ open }: { open: boolean }) =>
    open ? <div>firmware-dialog-open</div> : null,
}));

const mockCompat = systemApi.appCompatStatus as unknown as ReturnType<
  typeof vi.fn
>;
const mockChangeLanguage = vi.mocked(changeLanguage);

const INVALID_VERDICT = {
  code: 0,
  data: {
    valid: false,
    error_code: 'APP_OS_VERSION_UNSUPPORTED',
    message: 'current OS 1.13.0 is above the app maximum 1.12.0',
    os_version: '1.13.0',
    app_version: 'v1.1.0-test',
    app_min_os_version: '1.12.0',
    app_max_os_version: '1.12.0',
  },
};

function renderPage() {
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false } },
  });
  return render(
    <QueryClientProvider client={client}>
      <MemoryRouter initialEntries={['/app-incompatible']}>
        <Routes>
          <Route path="/app-incompatible" element={<AppIncompatible />} />
          <Route path="/dashboard" element={<div>dashboard-page</div>} />
        </Routes>
      </MemoryRouter>
    </QueryClientProvider>
  );
}

describe('AppIncompatible', () => {
  beforeEach(() => {
    mockCompat.mockReset();
    mockChangeLanguage.mockReset();
  });

  it('renders the warning, version table and only the firmware action', async () => {
    mockCompat.mockResolvedValue(INVALID_VERDICT);

    renderPage();

    expect(
      await screen.findByText('当前应用与设备 OS 不兼容，业务功能已停用')
    ).toBeInTheDocument();
    // Await the first verdict-dependent element so the query has settled
    // before the synchronous assertions below.
    expect(await screen.findByText('应用版本')).toBeInTheDocument();
    expect(screen.getByText('v1.1.0-test')).toBeInTheDocument();
    expect(screen.getByText('支持 OS 范围')).toBeInTheDocument();
    expect(screen.getByText('1.12.0 – 1.12.0')).toBeInTheDocument();
    expect(screen.getByText('当前设备 OS')).toBeInTheDocument();
    expect(screen.getByText('1.13.0')).toBeInTheDocument();
    expect(
      screen.getByText('current OS 1.13.0 is above the app maximum 1.12.0')
    ).toBeInTheDocument();
    expect(
      screen.getByRole('button', { name: '固件升级' })
    ).toBeInTheDocument();
    // Password change, reboot and the device-info link were removed by
    // design: firmware OTA is the only rescue path on this page.
    expect(screen.queryByRole('button', { name: '更改密码' })).toBeNull();
    expect(screen.queryByRole('button', { name: '系统重启' })).toBeNull();
    expect(screen.queryByText('查看设备信息页')).toBeNull();
  });

  it('opens the firmware dialog from its button', async () => {
    mockCompat.mockResolvedValue(INVALID_VERDICT);

    renderPage();
    await screen.findByText('应用版本');

    fireEvent.click(screen.getByRole('button', { name: '固件升级' }));
    expect(await screen.findByText('firmware-dialog-open')).toBeInTheDocument();
  });

  it('switches language via the login-style dropdown', async () => {
    mockCompat.mockResolvedValue(INVALID_VERDICT);

    renderPage();
    await screen.findByText('应用版本');

    // Trigger shows the current language (mocked en); the menu with both
    // languages opens from it, like the login page's control.
    fireEvent.click(screen.getByRole('button', { name: 'English' }));
    expect(screen.getByRole('menu')).toBeInTheDocument();
    expect(
      screen.getByRole('menuitem', { name: '简体中文' })
    ).toBeInTheDocument();

    // Current language is en: switching to zh goes through changeLanguage;
    // picking English again is a no-op. Both close the menu.
    fireEvent.click(screen.getByRole('menuitem', { name: '简体中文' }));
    expect(mockChangeLanguage).toHaveBeenCalledWith('zh');
    expect(screen.queryByRole('menu')).toBeNull();

    fireEvent.click(screen.getByRole('button', { name: 'English' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'English' }));
    expect(mockChangeLanguage).not.toHaveBeenCalledWith('en');
  });

  it('returns to the dashboard once the verdict turns valid', async () => {
    mockCompat.mockResolvedValue({ code: 0, data: { valid: true } });

    renderPage();

    expect(await screen.findByText('dashboard-page')).toBeInTheDocument();
  });
});
