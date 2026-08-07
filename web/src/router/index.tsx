import { createBrowserRouter, type RouteObject, Navigate, Outlet, redirect } from 'react-router-dom'
import Login from '@/pages/login'
import Dashboard from '@/pages/dashboard'
import Image from '@/pages/image'
import Peripherals from '@/pages/peripherals'
import Apps from '@/pages/apps'
import AIModels from '@/pages/ai-models'
import Monitoring from '@/pages/media'
import Network from '@/pages/settings/network'
import Storage from '@/pages/settings/storage'
import DeviceInfo from '@/pages/settings/device-info'
import TimeSettings from '@/pages/settings/time'
import Logs from '@/pages/maintenance/logs'
import FileManagement from '@/pages/maintenance/files'
import Terminal from '@/pages/maintenance/terminal'
import Processes from '@/pages/maintenance/processes'
import Events from '@/pages/events'
import Layout from '@/layout'
import AuthGuard from './components/auth-guard'
import NotFound404 from '@/components/notFound/404'
import { enableAuth, redirectToLoginBeforeRender, useAuthStore, validateAuthSession } from '@/store/auth'

// Root redirect component that checks auth status
function RootRedirect() {
  const isValidateToken = useAuthStore(s => s.isValidateToken);
  return <Navigate to={isValidateToken ? '/dashboard' : '/login'} replace />;
}

// Redirect already-authenticated users away from /login
function LoginGuard() {
  const isValidateToken = useAuthStore((s) => s.isValidateToken)
  if (!enableAuth) return <Login />
  return isValidateToken ? <Navigate to="/dashboard" replace /> : <Login />
}

const baseRoutes: RouteObject[] = [
  {
    path: '/dashboard',
    element: <Dashboard />,
  },
  {
    path: '/image',
    element: <Image />,
  },
  {
    path: '/peripherals',
    element: <Peripherals />,
  },
  {
    path: '/apps',
    element: <Apps />,
  },
  {
    path: '/models',
    element: <AIModels />,
  },
  {
    path: '/events',
    element: <Events />,
  },
  {
    path: '/media',
    element: <Monitoring />,
  },
  {
    path: '/settings/time',
    element: <TimeSettings />,
  },
  {
    path: '/settings/network',
    element: <Network />,
  },
  {
    path: '/settings/storage',
    element: <Storage />,
  },
  {
    path: '/settings/device-info',
    element: <DeviceInfo />,
  },
  {
    path: '/maintenance/logs',
    element: <Logs />,
  },
  {
    path: '/maintenance/files',
    element: <FileManagement />,
  },
  {
    path: '/maintenance/terminal',
    element: <Terminal />,
  },
  {
    path: '/maintenance/processes',
    element: <Processes />,
  },
];

// Loader-level auth guard. Runs during route resolution, BEFORE any route
// element is committed to the URL or rendered. Unauthenticated users are
// redirected to /login without first landing on the protected route (no
// "/dashboard then /login" double-step, no empty-layout flash).
const requireAuth = async ({ request }: { request: Request }) => {
  if (enableAuth && !(await validateAuthSession(request.signal))) {
    return redirect('/login')
  }
  return null;
};

function ProtectedRoutes() {
  return (
    <AuthGuard>
      <Outlet />
    </AuthGuard>
  );
}

redirectToLoginBeforeRender()

// Route configuration
const router = createBrowserRouter([
  {
    path: '/',
    element: <Layout />,
    children: [
      {
        index: true,
        element: <RootRedirect />,
      },
      {
        path: '/login',
        element: <LoginGuard />,
      },
      {
        element: <ProtectedRoutes />,
        loader: requireAuth,
        shouldRevalidate: () => false,
        children: baseRoutes,
      },
    ],
  },
  {
    path: '*',
    element: <NotFound404 />,
  },
]);

export default router;
