import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { Loader2 } from 'lucide-react';
import { toast } from 'sonner';
import { Input } from '@/components/ui/input';
import { Button } from '@/components/ui/button';
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog';
import { systemApi } from '@/services/api/system';
import { useLogout } from '@/hooks';

export interface PasswordDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
}

export function PasswordDialog({ open, onOpenChange }: PasswordDialogProps) {
  const { t } = useTranslation();
  const logout = useLogout();
  const [oldPassword, setOldPassword] = useState('');
  const [newPassword, setNewPassword] = useState('');
  const [confirmPassword, setConfirmPassword] = useState('');
  const [isChangingPassword, setIsChangingPassword] = useState(false);

  const handlePasswordChange = async () => {
    if (!oldPassword || !newPassword || !confirmPassword) {
      toast.error(t('sys.login.password_error'));
      return;
    }

    if (newPassword.length < 8 || newPassword.length > 32) {
      toast.error(t('sys.login.password_length_error'));
      return;
    }

    if (newPassword.includes(' ')) {
      toast.error(t('sys.login.password_illegal_error'));
      return;
    }

    if (newPassword !== confirmPassword) {
      toast.error(
        t('sys.device_info.password_mismatch', '两次输入的新密码不匹配')
      );
      return;
    }

    setIsChangingPassword(true);
    try {
      await systemApi.updatePassword({
        old_password: oldPassword,
        new_password: newPassword,
      });
      toast.success(
        t(
          'sys.device_info.password_changed_success',
          '密码修改成功，请重新登录'
        )
      );
      onOpenChange(false);
      setTimeout(() => {
        logout();
      }, 1500);
    } catch {
      toast.error(t('sys.device_info.password_changed_failed', '密码修改失败'));
    } finally {
      setIsChangingPassword(false);
    }
  };

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-lg bg-card border-border shadow-xl">
        <DialogHeader>
          <DialogTitle className="text-lg font-bold">
            {t('sys.device_info.change_password_title', '修改密码')}
          </DialogTitle>
          <DialogDescription className="text-muted-foreground text-xs">
            {t(
              'sys.device_info.change_password_desc',
              '为了您的设备安全，请定期修改密码'
            )}
          </DialogDescription>
        </DialogHeader>

        <div className="space-y-4 py-2">
          <div className="space-y-1.5 grid gap-0.5">
            <label className="text-xs font-semibold text-foreground">
              {t('sys.device_info.old_password', '原密码')}
            </label>
            <Input
              type="password"
              placeholder={t(
                'sys.device_info.old_password_placeholder',
                '请输入原密码'
              )}
              value={oldPassword}
              onChange={e => setOldPassword(e.target.value)}
              className="h-10 border-border text-sm"
            />
          </div>
          <div className="space-y-1.5 grid gap-0.5">
            <label className="text-xs font-semibold text-foreground">
              {t('sys.device_info.new_password', '新密码')}
            </label>
            <Input
              type="password"
              placeholder={t(
                'sys.device_info.new_password_placeholder',
                '请输入新密码'
              )}
              value={newPassword}
              onChange={e => setNewPassword(e.target.value)}
              className="h-10 border-border text-sm"
            />
          </div>
          <div className="space-y-1.5 grid gap-0.5">
            <label className="text-xs font-semibold text-foreground">
              {t('sys.device_info.confirm_password', '确认新密码')}
            </label>
            <Input
              type="password"
              placeholder={t(
                'sys.device_info.confirm_password_placeholder',
                '请再次输入新密码'
              )}
              value={confirmPassword}
              onChange={e => setConfirmPassword(e.target.value)}
              className="h-10 border-border text-sm"
            />
          </div>
        </div>

        <DialogFooter>
          <Button
            variant="outline"
            onClick={() => onOpenChange(false)}
            disabled={isChangingPassword}
            className="border-border font-medium"
          >
            {t('common.cancel', '取消')}
          </Button>
          <Button
            onClick={handlePasswordChange}
            disabled={isChangingPassword}
            className="bg-foreground hover:bg-foreground/90 text-background font-medium"
          >
            {isChangingPassword ? (
              <Loader2 className="w-4 h-4 mr-2 animate-spin" />
            ) : null}
            {t('common.confirm', '确认')}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
