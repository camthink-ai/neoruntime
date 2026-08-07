import { describe, expect, it } from 'vitest';
import { translateInstallError } from '@/pages/apps/lib/installErrorMessage';

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
});
