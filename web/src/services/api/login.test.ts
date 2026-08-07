import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import request from '@/services/request';
import { authApi } from '@/services/api/login';
import { encryptPassword, invalidatePublicKeyCache } from '@/utils/crypto';

vi.mock('@/services/request', () => ({
  default: {
    post: vi.fn(),
  },
}));

vi.mock('@/utils/crypto', () => ({
  encryptPassword: vi.fn(),
  invalidatePublicKeyCache: vi.fn(),
}));

const mockedPost = vi.mocked(request.post);
const mockedEncryptPassword = vi.mocked(encryptPassword);
const mockedInvalidatePublicKeyCache = vi.mocked(invalidatePublicKeyCache);

describe('authApi.login', () => {
  beforeEach(() => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date('2026-07-23T08:00:00Z'));
    vi.clearAllMocks();
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('refreshes the device time source and retries when login timestamp is outside the allowed window', async () => {
    const timestampError = {
      data: {
        code: 1005,
        message: 'Request timestamp out of allowed window',
      },
    };
    const loginResponse = {
      code: 0,
      message: 'Success',
      data: {
        token: 'Bearer token',
        username: 'admin',
      },
    };

    mockedEncryptPassword
      .mockResolvedValueOnce({ ciphertext: 'ciphertext-1', timestamp: 100 })
      .mockResolvedValueOnce({ ciphertext: 'ciphertext-2', timestamp: 200 });
    mockedPost
      .mockRejectedValueOnce(timestampError)
      .mockResolvedValueOnce(loginResponse);

    await expect(
      authApi.login({ username: 'admin', password: 'password' })
    ).resolves.toBe(loginResponse);

    expect(mockedPost).toHaveBeenNthCalledWith(
      1,
      '/api/login',
      { username: 'admin', password: 'ciphertext-1', timestamp: 100 },
      { silent: true }
    );
    expect(mockedPost).toHaveBeenNthCalledWith(
      2,
      '/api/login',
      { username: 'admin', password: 'ciphertext-2', timestamp: 200 },
      { silent: true }
    );
    expect(mockedPost).toHaveBeenCalledTimes(2);
    expect(mockedEncryptPassword).toHaveBeenCalledTimes(2);
    expect(mockedEncryptPassword).toHaveBeenNthCalledWith(1, 'password', {
      forcePublicKey: true,
    });
    expect(mockedEncryptPassword).toHaveBeenNthCalledWith(2, 'password', {
      forcePublicKey: true,
    });
    expect(mockedInvalidatePublicKeyCache).toHaveBeenCalledTimes(1);
  });

  it('does not sync device time before login if the refreshed timestamp still fails', async () => {
    const timestampError = {
      data: {
        code: 1005,
        message: 'Request timestamp out of allowed window',
      },
    };

    mockedEncryptPassword
      .mockResolvedValueOnce({ ciphertext: 'ciphertext-1', timestamp: 100 })
      .mockResolvedValueOnce({ ciphertext: 'ciphertext-2', timestamp: 200 });
    mockedPost
      .mockRejectedValueOnce(timestampError)
      .mockRejectedValueOnce(timestampError);

    await expect(
      authApi.login({ username: 'admin', password: 'password' })
    ).rejects.toBe(timestampError);

    expect(mockedPost).toHaveBeenNthCalledWith(
      1,
      '/api/login',
      { username: 'admin', password: 'ciphertext-1', timestamp: 100 },
      { silent: true }
    );
    expect(mockedPost).toHaveBeenNthCalledWith(
      2,
      '/api/login',
      { username: 'admin', password: 'ciphertext-2', timestamp: 200 },
      { silent: true }
    );
    expect(mockedPost).toHaveBeenCalledTimes(2);
    expect(mockedInvalidatePublicKeyCache).toHaveBeenCalledTimes(1);
  });

  it('does not sync time or retry for ordinary login failures', async () => {
    const invalidCredentials = {
      data: {
        code: 2000,
        message: 'Unauthorized',
      },
    };

    mockedEncryptPassword.mockResolvedValueOnce({
      ciphertext: 'ciphertext',
      timestamp: 100,
    });
    mockedPost.mockRejectedValueOnce(invalidCredentials);

    await expect(
      authApi.login({ username: 'admin', password: 'wrong' })
    ).rejects.toBe(invalidCredentials);

    expect(mockedPost).toHaveBeenCalledTimes(1);
    expect(mockedEncryptPassword).toHaveBeenCalledWith('wrong', {
      forcePublicKey: true,
    });
    expect(mockedInvalidatePublicKeyCache).not.toHaveBeenCalled();
  });
});
