import { useState, useEffect } from 'react';
import { useTranslation } from 'react-i18next';
import { Check, X } from 'lucide-react';
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
  DialogFooter,
} from '@/components/ui/dialog';
import { Input } from '@/components/ui/input';
import { Button } from '@/components/ui/button';
import type { FileInfo } from '../../hooks/useFiles';

interface RenameDialogProps {
  file: FileInfo | null;
  open: boolean;
  onOpenChange: (open: boolean) => void;
  onRename: (file: FileInfo, newName: string) => void;
  isPending: boolean;
}

export function RenameDialog({
  file,
  open,
  onOpenChange,
  onRename,
  isPending,
}: RenameDialogProps) {
  const { t } = useTranslation();
  const [name, setName] = useState('');

  // Sync name when target file changes
  useEffect(() => {
    if (file) setName(file.name);
  }, [file]);

  const handleRename = () => {
    if (!file || !name.trim() || name === file.name) return;
    onRename(file, name.trim());
  };

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="sm:max-w-sm">
        <DialogHeader>
          <DialogTitle>{t('sys.file_management.rename', '重命名')}</DialogTitle>
        </DialogHeader>
        <div className="py-2">
          <Input
            autoFocus
            placeholder={t(
              'sys.file_management.new_name_placeholder',
              '请输入新名称'
            )}
            value={name}
            onChange={e => setName(e.target.value)}
            onKeyDown={e => e.key === 'Enter' && handleRename()}
          />
        </div>
        <DialogFooter>
          <Button variant="outline" onClick={() => onOpenChange(false)}>
            <X className="h-4 w-4 mr-1" />
            {t('common.cancel', '取消')}
          </Button>
          <Button
            onClick={handleRename}
            disabled={!name.trim() || name === file?.name || isPending}
          >
            <Check className="h-4 w-4 mr-1" />
            {t('common.confirm', '确认')}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
