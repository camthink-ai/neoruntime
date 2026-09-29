import { describe, expect, it } from 'vitest';
import type { TFunction } from 'i18next';
import { resolveImportErrorMessage } from './resolveImportErrorMessage';

// Deterministic stub: return the key string so assertions can tell which
// branch fired. The real detail-passthrough branches bypass `t` entirely
// (they return data.error.detail directly), so those assert on the detail.
const mockT = ((key: string) => key) as unknown as TFunction;

// Helper to fake an axios-style error carrying our APIResponse envelope.
function apiError(body: {
  code?: number;
  message?: string;
  error?: { type?: string; detail?: string };
}): unknown {
  return { response: { data: body } };
}

describe('resolveImportErrorMessage', () => {
  it('maps invalid-json business codes to the friendly prompt', () => {
    // Arrange — code 1002 (CodeInvalidJSON) aborts before any apply.
    const err = apiError({ code: 1002, message: 'invalid json payload' });

    // Act
    const msg = resolveImportErrorMessage(err, mockT);

    // Assert
    expect(msg).toBe('maintenance.backup.invalid_json');
  });

  it('maps an integrity error type to the friendly prompt', () => {
    // Arrange — sha256 mismatch surfaces as type=integrity (a bad file).
    const err = apiError({
      code: 4003,
      error: { type: 'integrity', detail: 'checksum mismatch' },
    });

    // Act
    const msg = resolveImportErrorMessage(err, mockT);

    // Assert
    expect(msg).toBe('maintenance.backup.invalid_json');
  });

  it('maps an invalid-parameter code to the friendly prompt even with a type', () => {
    // Arrange — code 1004 (CodeInvalidParameter) is in the bad-file set.
    const err = apiError({
      code: 1004,
      error: { type: 'something-else', detail: 'raw detail' },
    });

    // Act
    const msg = resolveImportErrorMessage(err, mockT);

    // Assert — the code membership dominates the type.
    expect(msg).toBe('maintenance.backup.invalid_json');
  });

  it('surfaces the real detail for an apply-stage failure (code 4003 + type)', () => {
    // Arrange — type=apply means state write failed AFTER validation passed;
    // the real reason must not be masked.
    const err = apiError({
      code: 4003,
      error: { type: 'apply', detail: 'apply failed: db locked' },
    });

    // Act
    const msg = resolveImportErrorMessage(err, mockT);

    // Assert
    expect(msg).toBe('apply failed: db locked');
  });

  it('surfaces the real detail for an import-stage failure', () => {
    // Arrange
    const err = apiError({
      code: 3004,
      error: { type: 'import', detail: 'partial import aborted at row 7' },
    });

    // Act
    const msg = resolveImportErrorMessage(err, mockT);

    // Assert
    expect(msg).toBe('partial import aborted at row 7');
  });

  it('maps a client-side JSON parse failure (SyntaxError) to the friendly prompt', () => {
    // Arrange — config-tier import JSON.parses the file before POSTing; a
    // malformed file throws here with no axios response attached.
    const err: unknown = new SyntaxError('Unexpected token < in JSON');

    // Act
    const msg = resolveImportErrorMessage(err, mockT);

    // Assert
    expect(msg).toBe('maintenance.backup.invalid_json');
  });

  it('falls back to the error message for a plain network error', () => {
    // Arrange — no response body at all (offline / request never landed).
    const err: unknown = new Error('Network Error');

    // Act
    const msg = resolveImportErrorMessage(err, mockT);

    // Assert
    expect(msg).toBe('Network Error');
  });
});
