/**
 * Backend error envelope → display text.
 *
 * The API wraps failures as {code, message, error: {type, detail}} where
 * `message` is the generic per-code text and `error.detail` carries the
 * specific reason (e.g. a humanized model-load explanation). Detail wins;
 * axios's own message covers transport errors. Mirrors the precedence the
 * request.ts interceptor already uses.
 */
export function apiErrorText(error: unknown, fallback = ''): string {
  const data = (error as any)?.response?.data;
  return (
    data?.error?.detail || data?.message || (error as any)?.message || fallback
  );
}

/**
 * Business code for "the row committed, but loading it on the NPU failed"
 * (surfaced with HTTP 500). Callers branch on it to report partial success.
 */
export const MODEL_LOAD_FAILED_CODE = 5001;

/** Business error code from the API envelope, when present. */
export function apiErrorCode(error: unknown): number | undefined {
  return (error as any)?.response?.data?.code;
}
