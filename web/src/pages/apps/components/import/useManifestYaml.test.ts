import { act, renderHook, waitFor } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { appsApi } from '@/services/api';
import { useManifestYaml } from './useManifestYaml';

vi.mock('@/services/api', () => ({
  appsApi: { uploadManifest: vi.fn() },
}));

const uploadManifest = vi.mocked(appsApi.uploadManifest);

const fileOf = (text: string) => new File([text], 'app.yaml', { type: 'application/x-yaml' });

describe('useManifestYaml', () => {
  beforeEach(() => {
    uploadManifest.mockReset();
  });

  it('flush is a no-op when no manifest was ever attached (preview pollution guard)', async () => {
    // Preview mode (tar-only / registry source) shows form-generated YAML but
    // has no server-side file. If its text ever leaks into manifestText, a
    // flush must still do nothing — there is nothing to re-upload.
    const onApplied = vi.fn();
    const { result } = renderHook(() => useManifestYaml({ onApplied }));

    act(() => {
      result.current.setManifestText('apiVersion: v1\nkind: Application\n');
    });
    expect(result.current.yamlDirty).toBe(true);

    const res = await result.current.flushYaml();

    expect(res).toEqual({ ok: true });
    expect(uploadManifest).not.toHaveBeenCalled();
    expect(onApplied).not.toHaveBeenCalled();
  });

  it('re-uploads dirty text after an upload was attached and applies the response', async () => {
    const onApplied = vi.fn();
    uploadManifest.mockResolvedValue({
      data: {
        path: '/data/aipc/apps/manifests/demo/app.yaml',
        metadata: { id: 'demo' },
      },
    } as never);
    const { result } = renderHook(() => useManifestYaml({ onApplied }));

    act(() => {
      result.current.attachUpload(fileOf('id: demo\nversion: 1\n'));
    });
    await waitFor(() => expect(result.current.manifestText).toContain('demo'));
    expect(result.current.yamlDirty).toBe(false);

    act(() => {
      result.current.setManifestText('id: demo\nversion: 2\n');
    });
    expect(result.current.yamlDirty).toBe(true);

    let res: { ok: boolean; path?: string } | undefined;
    await act(async () => {
      res = await result.current.flushYaml();
    });

    expect(res).toEqual({
      ok: true,
      path: '/data/aipc/apps/manifests/demo/app.yaml',
    });
    expect(uploadManifest).toHaveBeenCalledTimes(1);
    const sent = uploadManifest.mock.calls[0][0];
    expect(sent).toBeInstanceOf(File);
    await expect(sent.text()).resolves.toBe('id: demo\nversion: 2\n');
    expect(onApplied).toHaveBeenCalledWith(
      expect.objectContaining({
        path: '/data/aipc/apps/manifests/demo/app.yaml',
      }),
      { source: 'flush' }
    );
    expect(result.current.yamlDirty).toBe(false);
  });

  it('reports failure and stays dirty when the server rejects the text', async () => {
    const onApplied = vi.fn();
    uploadManifest.mockRejectedValue({
      response: { data: { message: 'Invalid manifest: bad' } },
    });
    const { result } = renderHook(() => useManifestYaml({ onApplied }));

    act(() => {
      result.current.attachUpload(fileOf('id: demo\n'));
    });
    await waitFor(() => expect(result.current.manifestText).toBe('id: demo\n'));
    act(() => {
      result.current.setManifestText('id: demo\nbroken: [\n');
    });

    let res: { ok: boolean } | undefined;
    await act(async () => {
      res = await result.current.flushYaml();
    });

    expect(res).toEqual({ ok: false });
    expect(result.current.yamlDirty).toBe(true);
    expect(onApplied).not.toHaveBeenCalled();
  });

  it('reset clears upload tracking — flush becomes a no-op again', async () => {
    const { result } = renderHook(() => useManifestYaml({ onApplied: vi.fn() }));

    act(() => {
      result.current.attachUpload(fileOf('id: demo\n'));
    });
    await waitFor(() => expect(result.current.manifestText).toBe('id: demo\n'));
    act(() => {
      result.current.reset();
    });
    act(() => {
      result.current.setManifestText('polluted\n');
    });

    const res = await result.current.flushYaml();

    expect(res).toEqual({ ok: true });
    expect(uploadManifest).not.toHaveBeenCalled();
  });
});

describe('useManifestYaml live sync', () => {
  beforeEach(() => {
    uploadManifest.mockReset();
  });

  const attach = (result: { current: ReturnType<typeof useManifestYaml> }) => {
    act(() => {
      result.current.attachUpload(
        fileOf('metadata:\n  id: demo\n  name: Demo\n')
      );
    });
  };

  it('hydrates the form from valid edited text (debounced)', async () => {
    // Arrange
    const onLiveParse = vi.fn();
    const { result } = renderHook(() => useManifestYaml({ onApplied: vi.fn(), onLiveParse, parseDelayMs: 0 }));
    attach(result);
    await waitFor(() => expect(result.current.manifestText).toContain('demo'));

    // Act — user edit in the YAML editor
    act(() => {
      result.current.setManifestText(
        'metadata:\n  id: demo\n  name: Renamed\n'
      );
    });

    // Assert
    await waitFor(() => expect(onLiveParse).toHaveBeenCalledTimes(1));
    const config = onLiveParse.mock.calls[0][0];
    expect(config.metadata.name).toBe('Renamed');
    expect(result.current.yamlError).toBeNull();
  });

  it('flags invalid text without touching the form', async () => {
    // Arrange
    const onLiveParse = vi.fn();
    const { result } = renderHook(() => useManifestYaml({ onApplied: vi.fn(), onLiveParse, parseDelayMs: 0 }));
    attach(result);
    await waitFor(() => expect(result.current.manifestText).toContain('demo'));

    // Act
    act(() => {
      result.current.setManifestText('metadata:\n  id: [broken\n');
    });

    // Assert — form keeps the last good state, error surfaces
    await waitFor(() => expect(result.current.yamlError).toBeTruthy());
    expect(onLiveParse).not.toHaveBeenCalled();
  });

  it('projects form state onto the text (form → YAML)', async () => {
    // Arrange
    const { result } = renderHook(() => useManifestYaml({ onApplied: vi.fn(), parseDelayMs: 0 }));
    attach(result);
    await waitFor(() => expect(result.current.manifestText).toContain('demo'));

    // Act
    act(() => {
      result.current.applyConfig({
        metadata: { id: 'demo', name: 'Renamed', version: '', description: '' },
        image: '',
        permissions: { inference: {}, events: {}, device: {}, network: {} },
      });
    });

    // Assert — the edit landed and marks the text dirty for the next flush
    expect(result.current.manifestText).toContain('name: Renamed');
    expect(result.current.yamlDirty).toBe(true);
  });

  it('never rewrites the text while the editor is focused', async () => {
    // Arrange
    const { result } = renderHook(() => useManifestYaml({ onApplied: vi.fn(), parseDelayMs: 0 }));
    attach(result);
    await waitFor(() => expect(result.current.manifestText).toContain('demo'));

    // Act — user is typing in the YAML pane
    act(() => {
      result.current.setFocused(true);
    });
    act(() => {
      result.current.applyConfig({
        metadata: { id: 'demo', name: 'Renamed', version: '', description: '' },
        image: '',
        permissions: { inference: {}, events: {}, device: {}, network: {} },
      });
    });

    // Assert
    expect(result.current.manifestText).not.toContain('Renamed');
  });

  it('does not echo its own projection back into the form', async () => {
    // Arrange
    const onLiveParse = vi.fn();
    const { result } = renderHook(() => useManifestYaml({ onApplied: vi.fn(), onLiveParse, parseDelayMs: 0 }));
    attach(result);
    await waitFor(() => expect(result.current.manifestText).toContain('demo'));
    act(() => {
      result.current.applyConfig({
        metadata: { id: 'demo', name: 'Renamed', version: '', description: '' },
        image: '',
        permissions: { inference: {}, events: {}, device: {}, network: {} },
      });
    });
    const projected = result.current.manifestText;

    // Act — the editor fires onChange with the text we just wrote
    act(() => {
      result.current.setManifestText(projected);
    });

    // Assert — recognized as our own write; no hydration round-trip
    await new Promise(r => { setTimeout(r, 10); });
    expect(onLiveParse).not.toHaveBeenCalled();
  });
});
