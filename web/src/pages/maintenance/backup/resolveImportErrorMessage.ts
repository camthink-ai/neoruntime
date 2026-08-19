import type { TFunction } from 'i18next';

// Backend business codes that mean "the uploaded file is not a valid backup".
// These abort BEFORE any device state is touched (handlers validate
// multipart → manifest → schema → version → sha256 → inner JSON before
// applying), so masking their raw detail (e.g. a JSON parse error) with a
// friendly prompt hides no device-side failure. Apply/DB/network errors keep
// their real detail.
export const INVALID_BACKUP_CODES = new Set([1001, 1002, 1004]);

export type ApiErrorBody = {
  code?: number;
  message?: string;
  error?: { type?: string; detail?: string };
};

// Map an import error to a user-facing toast message. Bad-backup-file errors
// (validation / integrity / invalid request·JSON·parameter / extract failure),
// and a client-side JSON parse failure before the request even ships, show the
// friendly invalid-file prompt; server-side failures surface their specific
// detail so the real reason isn't masked.
export function resolveImportErrorMessage(err: unknown, t: TFunction): string {
  // Config-tier import parses the .json client-side before POSTing. A malformed
  // file throws SyntaxError here (no axios response), so classify it the same
  // way as a server-side bad-file rejection.
  if (err instanceof SyntaxError) {
    return t(
      'maintenance.backup.invalid_json',
      'The backup file is invalid or corrupted.'
    );
  }

  const data = (err as { response?: { data?: ApiErrorBody } })?.response?.data;
  const code = data?.code;
  const type = data?.error?.type;
  const isBadBackupFile =    type === 'validation'
    || type === 'integrity'
    || (code !== undefined && INVALID_BACKUP_CODES.has(code))
    || (code === 4003 && !type); // extract/staging (FailMsg) — not apply/import (FailTyped)
  if (isBadBackupFile) {
    return t(
      'maintenance.backup.invalid_json',
      'The backup file is invalid or corrupted.'
    );
  }
  return (
    data?.error?.detail
    || data?.message
    || (err as Error)?.message
    || t('common.error')
  );
}
