export function redirectToLoginAfterReboot(): void {
  if (typeof window === 'undefined') return;
  window.location.assign('/login');
}
