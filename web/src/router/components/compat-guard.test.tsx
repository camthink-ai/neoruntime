import { render, screen } from '@testing-library/react';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { beforeEach, describe, expect, it, vi } from 'vitest';

import CompatGuard from './compat-guard';
import { systemApi } from '@/services/api/system';

vi.mock('@/services/api/system', () => ({
  systemApi: {
    appCompatStatus: vi.fn(),
  },
}));

const mockCompat = systemApi.appCompatStatus as unknown as ReturnType<
  typeof vi.fn
>;

const INVALID_VERDICT = {
  code: 0,
  data: {
    valid: false,
    error_code: 'APP_OS_VERSION_UNSUPPORTED',
    os_version: '1.13.0',
    app_version: 'v1.1.0-test',
    app_min_os_version: '1.12.0',
    app_max_os_version: '1.12.0',
  },
};

// Render the guard around a marker page at the given path, with a takeover
// landing and an exempt route to observe redirects.
function renderGuardAt(path: string) {
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false } },
  });
  return render(
    <QueryClientProvider client={client}>
      <MemoryRouter initialEntries={[path]}>
        <Routes>
          <Route
            path="/dashboard"
            element={
              <CompatGuard>
                <div>dashboard-page</div>
              </CompatGuard>
            }
          />
          <Route
            path="/settings/device-info"
            element={
              <CompatGuard>
                <div>device-info-page</div>
              </CompatGuard>
            }
          />
          <Route
            path="/app-incompatible"
            element={<div>app-incompatible-page</div>}
          />
        </Routes>
      </MemoryRouter>
    </QueryClientProvider>
  );
}

describe('CompatGuard', () => {
  beforeEach(() => {
    mockCompat.mockReset();
  });

  it('redirects a business route to the takeover page when incompatible', async () => {
    mockCompat.mockResolvedValue(INVALID_VERDICT);

    renderGuardAt('/dashboard');

    expect(
      await screen.findByText('app-incompatible-page')
    ).toBeInTheDocument();
    expect(screen.queryByText('dashboard-page')).toBeNull();
  });

  it('keeps the exempt upgrade route reachable when incompatible', async () => {
    mockCompat.mockResolvedValue(INVALID_VERDICT);

    renderGuardAt('/settings/device-info');

    expect(await screen.findByText('device-info-page')).toBeInTheDocument();
    expect(screen.queryByText('app-incompatible-page')).toBeNull();
  });

  it('renders children when the installed app is compatible', async () => {
    mockCompat.mockResolvedValue({ code: 0, data: { valid: true } });

    renderGuardAt('/dashboard');

    expect(await screen.findByText('dashboard-page')).toBeInTheDocument();
  });

  it('fails open when the verdict cannot be fetched', async () => {
    mockCompat.mockRejectedValue(new Error('network down'));

    renderGuardAt('/dashboard');

    // The hook retries once with a ~1s backoff; tolerate it.
    expect(
      await screen.findByText('dashboard-page', {}, { timeout: 4000 })
    ).toBeInTheDocument();
  });
});
