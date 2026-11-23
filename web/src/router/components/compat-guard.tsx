import { Navigate, useLocation } from 'react-router-dom';

import { useAppCompat } from '@/hooks';
import Loading from '@/components/loading';

interface CompatGuardProps {
  children: React.ReactNode;
}

// Routes that stay reachable while the installed app is incompatible:
// the upgrade page itself is the rescue channel.
const COMPAT_EXEMPT_PATHS = ['/settings/device-info'];

// Route-level takeover for the post-OS-upgrade degraded state: when the
// installed app is incompatible with the running OS, business pages are
// held down server-side, so they are redirected to the dedicated status
// page instead of rendering their own broken states. Fails open: an
// unreachable verdict must never lock the operator out.
export default function CompatGuard({ children }: CompatGuardProps) {
  const location = useLocation();
  const { data: verdict, isLoading, isError } = useAppCompat();

  if (COMPAT_EXEMPT_PATHS.includes(location.pathname)) {
    return children;
  }

  // Gate only on the first fetch of the session — the verdict is cached
  // for the session afterwards, so navigation never flashes a loading
  // placeholder.
  if (isLoading) {
    return <Loading fullHeight />;
  }

  if (isError || !verdict || verdict.valid !== false) {
    return children;
  }

  return <Navigate to="/app-incompatible" replace />;
}
