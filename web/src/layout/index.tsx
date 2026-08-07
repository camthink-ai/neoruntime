import { useEffect, useState } from 'react';
import { Navigate, Outlet, useLocation, useNavigation } from 'react-router-dom';

import PCMenu from './pc/menu';
import MobileMenuDrawer from './mobile/menu-drawer';
import MobileHeader from './mobile/mobile-header';

import { enableAuth, hasValidAuthToken, isLoginPath, useAuthStore } from '@/store/auth';
import { useIsMobile } from '@/hooks/use-mobile';
import { cn } from '@/lib/utils';
import Loading from '@/components/loading';

export default function Layout() {
  const { isValidateToken } = useAuthStore();
  const isMobile = useIsMobile();
  const location = useLocation();
  const navigation = useNavigation();
  const isLoginPage = isLoginPath(location.pathname);
  const [mobileMenuOpen, setMobileMenuOpen] = useState(false);

  // On first entry, the requireAuth route loader fires a session check
  // before the protected page is allowed to render. During that window the
  // right pane would be blank. Show a centered placeholder only for that
  // initial load — once the first route settles, each page owns its own
  // loading state, so we never gate the Outlet again.
  const [bootstrapped, setBootstrapped] = useState(false);
  useEffect(() => {
    if (navigation.state === 'idle') setBootstrapped(true);
  }, [navigation.state]);
  const showInitialLoading =    !bootstrapped && navigation.state === 'loading';

  if (enableAuth && !isLoginPage && !hasValidAuthToken()) {
    return <Navigate to="/login" replace />;
  }

  const showMobileChrome = isMobile && isValidateToken && !isLoginPage;

  return (
    <div className="flex flex-col h-screen w-screen">
      {/* Main Layout with Sidebar */}
      <div className="flex flex-1 overflow-hidden bg-background">
        {/* PC Navigation menu */}
        {isValidateToken && !isLoginPage && !isMobile && <PCMenu />}

        {/* Mobile top bar: logo + menu */}
        {showMobileChrome && (
          <MobileHeader onMenuClick={() => setMobileMenuOpen(true)} />
        )}

        {/* Mobile Menu Drawer */}
        {isValidateToken && !isLoginPage && isMobile && (
          <MobileMenuDrawer
            open={mobileMenuOpen}
            onClose={() => setMobileMenuOpen(false)}
          />
        )}

        <main
          className={cn(
            'flex-1 overflow-auto w-full relative bg-background',
            showMobileChrome
              && 'mt-[calc(3.5rem+env(safe-area-inset-top,0px))] h-[calc(100dvh-(3.5rem+env(safe-area-inset-top,0px)))]'
          )}
        >
          {showInitialLoading ? <Loading fullHeight /> : <Outlet />}
        </main>
      </div>
    </div>
  );
}
