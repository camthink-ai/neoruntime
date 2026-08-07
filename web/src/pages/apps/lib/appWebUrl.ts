import type { AppTemplate } from '@/services/types';

type AppWebLocation = Pick<Location, 'hostname' | 'origin' | 'port'>;

function directPortUrl(
  app: AppTemplate,
  path: string,
  location: AppWebLocation
): string | null {
  const inbound = app.permissions?.network?.inbound;
  if (!inbound || inbound.length === 0) return null;
  return `http://${location.hostname}:${inbound[0]}${path}`;
}

function shouldUseDirectPortFallback(location: AppWebLocation): boolean {
  return (
    location.port !== '' && location.port !== '80' && location.port !== '443'
  );
}

export function getAppWebUrl(
  app: AppTemplate,
  location: AppWebLocation = window.location
): string | null {
  if (app.state !== 'running' || !app.web_url) return null;
  const path = app.web_url.startsWith('/') ? app.web_url : `/${app.web_url}`;
  if (shouldUseDirectPortFallback(location)) {
    return directPortUrl(app, path, location);
  }
  return `${location.origin}/apps/${encodeURIComponent(String(app.id))}${path}`;
}
