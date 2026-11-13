import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import MediaSettings from './MediaSettings';
import { getMediaConfig, setRtspEnabled } from '@/services/media';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({ t: (_key: string, fallback: string) => fallback }),
}));
vi.mock('@/services/request', () => ({ default: {} }));
vi.mock('@/components/ui/scroll-area', () => ({
  ScrollArea: ({ children }: { children: React.ReactNode }) => <div>{children}</div>,
}));
vi.mock('@/hooks/useMediaStatus', () => ({
  useMediaStatus: () => ({ data: { streams: [] }, isLoading: false }),
}));
vi.mock('@/services/media', async importOriginal => ({
  ...await importOriginal<typeof import('@/services/media')>(),
  getMediaConfig: vi.fn(),
  setRtspEnabled: vi.fn(),
}));

const renderSettings = (client: QueryClient) => render(
  <QueryClientProvider client={client}><MediaSettings /></QueryClientProvider>
);

describe('RTSP settings', () => {
  let client: QueryClient;
  beforeEach(() => {
    vi.resetAllMocks();
    client = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  });
  afterEach(() => { cleanup(); client.clear(); });

  it('loads the saved state again on re-entry and updates it after a successful toggle', async () => {
    vi.mocked(getMediaConfig).mockResolvedValue({ data: { rtsp: { enabled: false } } } as any);
    vi.mocked(setRtspEnabled).mockResolvedValue({} as any);
    const view = renderSettings(client);
    const toggle = await screen.findByRole('switch', { name: 'Enable RTSP Stream' });
    await waitFor(() => expect(toggle).toBeEnabled());
    expect(toggle).not.toBeChecked();
    fireEvent.click(toggle);
    await waitFor(() => expect(toggle).toBeChecked());
    expect(setRtspEnabled).toHaveBeenCalledWith(true);
    view.unmount();
    vi.mocked(getMediaConfig).mockResolvedValue({ data: { rtsp: { enabled: false } } } as any);
    renderSettings(client);
    await waitFor(() => expect(screen.getByRole('switch', { name: 'Enable RTSP Stream' })).not.toBeChecked());
    expect(getMediaConfig).toHaveBeenCalledTimes(2);
  });

  it('keeps the saved state when a toggle fails', async () => {
    vi.mocked(getMediaConfig).mockResolvedValue({ data: { rtsp: { enabled: true } } } as any);
    vi.mocked(setRtspEnabled).mockRejectedValue(new Error('failed'));
    renderSettings(client);
    const toggle = await screen.findByRole('switch', { name: 'Enable RTSP Stream' });
    await waitFor(() => expect(toggle).toBeEnabled());
    fireEvent.click(toggle);
    await waitFor(() => expect(setRtspEnabled).toHaveBeenCalledWith(false));
    await waitFor(() => expect(toggle).toBeEnabled());
    expect(toggle).toBeChecked();
  });

  it('disables the switch and offers retry when the config cannot be read', async () => {
    vi.mocked(getMediaConfig).mockRejectedValueOnce(new Error('failed'))
      .mockResolvedValueOnce({ data: { rtsp: { enabled: true } } } as any);
    renderSettings(client);
    expect(await screen.findByRole('alert')).toBeInTheDocument();
    expect(screen.getByRole('switch', { name: 'Enable RTSP Stream' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
    await waitFor(() => expect(screen.getByRole('switch', { name: 'Enable RTSP Stream' })).toBeChecked());
  });
});
