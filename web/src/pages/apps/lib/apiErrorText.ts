/**
 * Backend error envelope → display text for app actions.
 *
 * The request interceptor rejects with the axios *response* (not the error),
 * so the envelope {code, message, error: {detail}} sits at err.data; the
 * err.response.data and err.message shapes cover plain-axios rejections and
 * thrown Errors (e.g. the install precheck in useApps).
 */
export function apiErrorText(error: unknown): string {
  const err = error as any;
  const data = err?.data ?? err?.response?.data;
  return data?.error?.detail || data?.message || err?.message || '';
}
