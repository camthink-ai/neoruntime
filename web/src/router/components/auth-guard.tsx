import { Navigate, useLocation } from 'react-router-dom';
import { enableAuth, useAuthStore } from '@/store/auth';

interface AuthGuardProps {
  children: React.ReactNode;
}

export default function AuthGuard({ children }: AuthGuardProps) {
  const isValidateToken = useAuthStore(s => s.isValidateToken);
  const location = useLocation();

  if (!isValidateToken && enableAuth) {
    return <Navigate to="/login" state={{ from: location }} replace />;
  }

  return children;
}
