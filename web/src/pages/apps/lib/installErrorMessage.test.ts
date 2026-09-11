import { describe, expect, it } from 'vitest';
import {
  resolveInstallApiError,
  resolveKnownApiError,
  translateInstallError,
} from '@/pages/apps/lib/installErrorMessage';

const t = ((
  key: string,
  options?: {
    defaultValue?: string;
    detail?: string;
    appId?: string;
    image?: string;
  }
) => {
  if (key === 'sys.apps.import.errors.app_exists') {
    return `应用「${options?.appId}」已安装`;
  }
  if (key === 'sys.apps.import.errors.import_image') {
    return `镜像导入失败：${options?.detail}`;
  }
  if (key === 'sys.apps.import.errors.invalid_image_tar') {
    return `镜像文件不合法：${options?.detail}`;
  }
  if (key === 'sys.apps.import.upload_errors.image_too_large') {
    return '镜像文件超过 2GB 上限';
  }
  if (key === 'sys.apps.import.errors.empty') {
    return options?.defaultValue ?? key;
  }
  if (key === 'sys.apps.import.errors.unknown') {
    return `未知错误：${options?.detail}`;
  }
  return options?.defaultValue ?? key;
}) as never;

describe('translateInstallError', () => {
  it('maps backend app exists error', () => {
    expect(
      translateInstallError(
        'App demo-app already exists. Use force=true to overwrite.',
        t
      )
    ).toBe('应用「demo-app」已安装');
  });

  it('maps backend import image error', () => {
    expect(
      translateInstallError('Failed to import image: invalid tar header', t)
    ).toBe('镜像导入失败：invalid tar header');
  });

  it('unwraps installation failed prefix', () => {
    expect(
      translateInstallError(
        'Installation failed: Failed to import image: corrupt archive',
        t
      )
    ).toBe('镜像导入失败：corrupt archive');
  });

  it('returns empty fallback', () => {
    expect(translateInstallError('', t)).toContain('unknown error');
  });

  it('maps manifest yaml upload error', () => {
    expect(
      translateInstallError('Invalid YAML format: yaml: line 2', t)
    ).toContain('Invalid manifest YAML');
  });

  it('maps upload-time invalid tar rejection', () => {
    expect(
      translateInstallError(
        'Invalid image tar: manifest entry 0: layer "sha256:deadbeef" not found in archive',
        t
      )
    ).toBe(
      '镜像文件不合法：manifest entry 0: layer "sha256:deadbeef" not found in archive'
    );
  });

  it('maps upload-time size limit rejection', () => {
    expect(
      translateInstallError(
        'Image exceeds the maximum allowed size (2147483648 bytes)',
        t
      )
    ).toBe('镜像文件超过 2GB 上限');
  });

  it('maps upload-time no-space rejection to no_space', () => {
    expect(
      translateInstallError(
        'No space left for upload: need 5368709120 bytes (with headroom), 1073741824 free',
        t
      )
    ).toContain('Not enough disk space');
  });
});

const apiError = (message: string) => ({
  response: { data: { message } },
});

describe('resolveKnownApiError', () => {
  it('translates a matched error with its specific copy', () => {
    const msg = resolveKnownApiError(
      apiError('Invalid YAML format: yaml: line 2'),
      t,
      'FALLBACK'
    );
    // the fake t returns defaultValue verbatim (no {{detail}} interpolation
    // — that is i18next's job), so assert the matched copy won over the
    // fallback rather than the interpolated detail.
    expect(msg).toContain('Invalid manifest YAML');
    expect(msg).not.toBe('FALLBACK');
  });

  it('falls back to the caller context message for unmatched payloads', () => {
    expect(
      resolveKnownApiError(apiError('something odd happened'), t, 'FALLBACK')
    ).toBe('FALLBACK');
  });

  it('falls back when there is no readable payload at all', () => {
    expect(resolveKnownApiError(undefined, t, 'FALLBACK')).toBe('FALLBACK');
    expect(resolveKnownApiError(new Error(''), t, 'FALLBACK')).toBe('FALLBACK');
  });
});

describe('resolveInstallApiError (unchanged anchor)', () => {
  it('still translates matched errors for install contexts', () => {
    expect(
      resolveInstallApiError(apiError('Failed to import image: bad tar'), t)
    ).toBe('镜像导入失败：bad tar');
  });

  it('still returns the install-flavored generic for unmatched payloads', () => {
    expect(resolveInstallApiError(apiError('something odd'), t)).toContain(
      '未知错误'
    );
  });
});
