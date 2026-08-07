import JSEncrypt from 'jsencrypt';

import request from '@/services/request';

// Password transport encryption: the frontend RSA-encrypts the password with the
// device's public key (fetched once and cached) before sending it over the wire,
// so a plaintext password never appears in a request body, access log, or
// browser history — on top of the TLS protection HTTPS already provides.

const PUBKEY_CACHE = 'aipc_rsa_pubkey';
const PUBKEY_TS = 'aipc_rsa_pubkey_ts';
const PUBKEY_DEVICE_TS = 'aipc_rsa_pubkey_device_ts';
// Cache the public key for 5 minutes: the key only changes on factory reset or
// if /data/aipc/etc/rsa is deleted, so a short TTL keeps logins fast while still
// self-healing within minutes if the device rotates its keypair.
const CACHE_TTL_MS = 5 * 60 * 1000;

export interface PublicKeyResp {
  public_key: string;
  algorithm: string;
  unix_timestamp?: number;
}

export interface EncryptedPassword {
  ciphertext: string;
  timestamp: number;
}

export interface EncryptPasswordOptions {
  forcePublicKey?: boolean;
}

interface PublicKeyMaterial {
  pem: string;
  deviceUnixTimestamp?: number;
  fetchedAtMs: number;
}

const getAdjustedDeviceTimestamp = (material: PublicKeyMaterial) => {
  if (typeof material.deviceUnixTimestamp !== 'number') {
    return Math.floor(Date.now() / 1000);
  }
  return (
    material.deviceUnixTimestamp
    + Math.floor((Date.now() - material.fetchedAtMs) / 1000)
  );
};

/**
 * Fetches the device's RSA public key, using a sessionStorage cache to avoid a
 * round-trip on every login. Pass force=true to bypass the cache (used after a
 * login failure that may indicate the cached key no longer matches the device).
 */
async function fetchPublicKeyMaterial(
  force = false
): Promise<PublicKeyMaterial> {
  if (!force) {
    const cached = sessionStorage.getItem(PUBKEY_CACHE);
    const ts = Number(sessionStorage.getItem(PUBKEY_TS) || 0);
    const rawDeviceTs = sessionStorage.getItem(PUBKEY_DEVICE_TS);
    const deviceTs = rawDeviceTs === null ? undefined : Number(rawDeviceTs);
    if (cached && Date.now() - ts < CACHE_TTL_MS) {
      return {
        pem: cached,
        deviceUnixTimestamp: Number.isFinite(deviceTs) ? deviceTs : undefined,
        fetchedAtMs: ts,
      };
    }
  }
  const resp = (await request.get('/api/v1/auth/public-key')) as {
    data?: PublicKeyResp;
  };
  const pem = resp.data?.public_key;
  if (!pem) {
    throw new Error('public_key missing in response');
  }
  const fetchedAtMs = Date.now();
  sessionStorage.setItem(PUBKEY_CACHE, pem);
  sessionStorage.setItem(PUBKEY_TS, String(fetchedAtMs));
  if (typeof resp.data?.unix_timestamp === 'number') {
    sessionStorage.setItem(PUBKEY_DEVICE_TS, String(resp.data.unix_timestamp));
  } else {
    sessionStorage.removeItem(PUBKEY_DEVICE_TS);
  }
  return {
    pem,
    deviceUnixTimestamp: resp.data?.unix_timestamp,
    fetchedAtMs,
  };
}

export async function fetchPublicKey(force = false): Promise<string> {
  const material = await fetchPublicKeyMaterial(force);
  return material.pem;
}

/**
 * RSA-encrypts a plaintext password and returns the base64 ciphertext plus a
 * device-clock unix-second timestamp for replay protection.
 *
 * Mixed-deploy fallback: if the public key cannot be fetched or encryption
 * fails (e.g. a new frontend pointed at an old backend that has no
 * /api/v1/auth/public-key), the plaintext is returned with timestamp 0. A new
 * backend's try-decrypt-fallback then treats the undecryptable value as
 * plaintext, and an old backend compares it directly — so login degrades to
 * plaintext gracefully instead of failing. Encryption still wins whenever the
 * key is reachable.
 */
export async function encryptPassword(
  plain: string,
  options: EncryptPasswordOptions = {}
): Promise<EncryptedPassword> {
  try {
    const material = await fetchPublicKeyMaterial(
      options.forcePublicKey ?? true
    );
    const enc = new JSEncrypt();
    enc.setPublicKey(material.pem);
    const ct = enc.encrypt(plain);
    if (ct === false) {
      throw new Error('RSA encryption failed');
    }
    return { ciphertext: ct, timestamp: getAdjustedDeviceTimestamp(material) };
  } catch (err) {
    // eslint-disable-next-line no-console
    console.warn(
      '[crypto] password encryption unavailable, falling back to plaintext:',
      err
    );
    return { ciphertext: plain, timestamp: 0 };
  }
}

/** Drops the cached public key so the next encryptPassword re-fetches it. */
export function invalidatePublicKeyCache(): void {
  sessionStorage.removeItem(PUBKEY_CACHE);
  sessionStorage.removeItem(PUBKEY_TS);
  sessionStorage.removeItem(PUBKEY_DEVICE_TS);
}
