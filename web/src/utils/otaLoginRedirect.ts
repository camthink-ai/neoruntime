const OTA_SUCCESS_LOGIN_MESSAGE_KEY = 'aipc.ota.successLoginMessage';
const OTA_SUCCESS_QUERY_KEY = 'ota';
const OTA_SUCCESS_QUERY_VALUE = 'success';

const canUseWindow = () => typeof window !== 'undefined';

export function stashOTASuccessLoginMessage(message: string): void {
  if (!canUseWindow()) return;
  try {
    window.sessionStorage.setItem(OTA_SUCCESS_LOGIN_MESSAGE_KEY, message);
  } catch {
    // The login URL query parameter is still enough to show a fallback prompt.
  }
}

export function redirectToLoginAfterOTASuccess(): void {
  if (!canUseWindow()) return;
  window.location.assign(
    `/login?${OTA_SUCCESS_QUERY_KEY}=${OTA_SUCCESS_QUERY_VALUE}`
  );
}

export function consumeOTASuccessLoginMessage(
  fallbackMessage: string
): string | null {
  if (!canUseWindow()) return null;

  const params = new URLSearchParams(window.location.search);
  const hasSuccessQuery = params.get(OTA_SUCCESS_QUERY_KEY) === OTA_SUCCESS_QUERY_VALUE;
  let storedMessage = '';
  try {
    storedMessage = window.sessionStorage.getItem(OTA_SUCCESS_LOGIN_MESSAGE_KEY) || '';
    window.sessionStorage.removeItem(OTA_SUCCESS_LOGIN_MESSAGE_KEY);
  } catch {
    storedMessage = '';
  }

  if (!hasSuccessQuery && !storedMessage) {
    return null;
  }

  if (hasSuccessQuery) {
    params.delete(OTA_SUCCESS_QUERY_KEY);
    const nextSearch = params.toString();
    const cleanPath = `${window.location.pathname}${nextSearch ? `?${nextSearch}` : ''}${window.location.hash}`;
    window.history.replaceState(null, '', cleanPath);
  }

  return storedMessage || fallbackMessage;
}
