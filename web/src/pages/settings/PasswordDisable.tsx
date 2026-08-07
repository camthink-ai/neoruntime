import { useState } from 'react';
import { Button } from '@/components/ui/button';
import { useTranslation } from 'react-i18next';
import { Lock, Unlock, AlertTriangle } from 'lucide-react';
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '@/components/ui/alert-dialog';

export default function PasswordDisable() {
  const { t } = useTranslation();
  const [passwordEnabled, setPasswordEnabled] = useState(true);
  const [showDialog, setShowDialog] = useState(false);

  const handleTogglePassword = () => {
    setShowDialog(true);
  };

  const handleConfirm = () => {
    setPasswordEnabled(prev => !prev);
    setShowDialog(false);
  };

  return (
    <div className="space-y-4 max-w-2xl">
      <h2 className="text-xl font-semibold mb-4">
        {t('sys.password_settings.title', 'Password Lock')}
      </h2>
      <div className="space-y-6">
        <div className="flex items-center justify-between">
          <div className="flex items-center space-x-3">
            {passwordEnabled ? (
              <Lock className="h-5 w-5 text-green-600" />
            ) : (
              <Unlock className="h-5 w-5 text-red-600" />
            )}
            <div>
              <div className="font-medium">
                {t('sys.password_settings.status', 'Password Status')}
              </div>
              <div className="text-sm text-muted-foreground">
                {passwordEnabled
                  ? t('common.enabled', 'Enabled')
                  : t('common.disabled', 'Disabled')}
              </div>
            </div>
          </div>
          <Button
            variant={passwordEnabled ? 'destructive' : 'default'}
            onClick={handleTogglePassword}
          >
            {passwordEnabled
              ? t('sys.password_settings.disable', 'Disable Password')
              : t('sys.password_settings.enable', 'Enable Password')}
          </Button>
        </div>

        {!passwordEnabled && (
          <div className="flex items-start space-x-3 p-4 bg-destructive/10 border border-destructive/20 rounded-lg">
            <AlertTriangle className="h-5 w-5 text-destructive flex-shrink-0 mt-0.5" />
            <p className="text-sm text-destructive">
              {t(
                'sys.password_settings.warning',
                'Disabling the password allows anyone to access the device, which poses a security risk'
              )}
            </p>
          </div>
        )}
      </div>

      <AlertDialog open={showDialog} onOpenChange={setShowDialog}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>
              {passwordEnabled
                ? t('sys.password_settings.disable', 'Disable Password')
                : t('sys.password_settings.enable', 'Enable Password')}
            </AlertDialogTitle>
            <AlertDialogDescription>
              {passwordEnabled
                ? t(
                    'sys.password_settings.warning',
                    'Disabling the password allows anyone to access the device, which poses a security risk'
                  )
                : '确定要启用设备密码吗？启用后需要密码才能访问设备。'}
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('common.cancel')}</AlertDialogCancel>
            <AlertDialogAction onClick={handleConfirm}>
              {t('common.confirm')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </div>
  );
}
