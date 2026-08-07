import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const requestMocks = vi.hoisted(() => ({
  get: vi.fn(),
}));

const jsencryptMocks = vi.hoisted(() => ({
  encrypt: vi.fn(),
  setPublicKey: vi.fn(),
}));

vi.mock('@/services/request', () => ({
  default: {
    get: requestMocks.get,
  },
}));

vi.mock('jsencrypt', () => {
  function MockJSEncrypt() {
    return {
      encrypt: jsencryptMocks.encrypt,
      setPublicKey: jsencryptMocks.setPublicKey,
    };
  }

  return {
    default: vi.fn(MockJSEncrypt),
  };
});

describe('encryptPassword', () => {
  beforeEach(() => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date('2026-07-27T08:00:00Z'));
    sessionStorage.clear();
    requestMocks.get.mockReset();
    jsencryptMocks.encrypt.mockReset();
    jsencryptMocks.setPublicKey.mockReset();
    jsencryptMocks.encrypt.mockReturnValue('ciphertext');
  });

  afterEach(() => {
    vi.useRealTimers();
    sessionStorage.clear();
  });

  it('uses the device timestamp from the public-key response', async () => {
    const { encryptPassword } = await import('@/utils/crypto');

    requestMocks.get.mockResolvedValueOnce({
      data: {
        public_key: 'device-pem',
        algorithm: 'RSA-2048/PKCS1v15',
        unix_timestamp: 946684800,
      },
    });

    await expect(encryptPassword('password')).resolves.toEqual({
      ciphertext: 'ciphertext',
      timestamp: 946684800,
    });

    expect(requestMocks.get).toHaveBeenCalledWith('/api/v1/auth/public-key');
    expect(jsencryptMocks.setPublicKey).toHaveBeenCalledWith('device-pem');
    expect(jsencryptMocks.encrypt).toHaveBeenCalledWith('password');
  });

  it('can reuse cached device time adjusted by elapsed browser time', async () => {
    const { encryptPassword, fetchPublicKey } = await import('@/utils/crypto');

    requestMocks.get.mockResolvedValueOnce({
      data: {
        public_key: 'cached-pem',
        algorithm: 'RSA-2048/PKCS1v15',
        unix_timestamp: 1000,
      },
    });

    await expect(fetchPublicKey(false)).resolves.toBe('cached-pem');
    vi.advanceTimersByTime(2500);

    await expect(
      encryptPassword('password', { forcePublicKey: false })
    ).resolves.toEqual({
      ciphertext: 'ciphertext',
      timestamp: 1002,
    });

    expect(requestMocks.get).toHaveBeenCalledTimes(1);
  });
});
