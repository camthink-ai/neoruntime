import { useState, useEffect, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import { Check, X, FilePen, Maximize2, Minimize2 } from 'lucide-react';
import screenfull from 'screenfull';
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
  DialogFooter,
} from '@/components/ui/dialog';
import { Button } from '@/components/ui/button';
import { filesApi } from '@/services/api';
import type { FileInfo } from '../../hooks/useFiles';
import { useUploadFile } from '../../hooks/useFiles';
import MonacoEditor from '../MonacoEditor';
import Loading from '@/components/loading';

interface ViewEditDialogProps {
  file: FileInfo | null;
  open: boolean;
  initialEditMode?: boolean;
  onOpenChange: (open: boolean) => void;
}

export function ViewEditDialog({
  file,
  open,
  initialEditMode = false,
  onOpenChange,
}: ViewEditDialogProps) {
  const { t } = useTranslation();
  const uploadFile = useUploadFile();
  const dialogRef = useRef<HTMLDivElement>(null);

  const [loading, setLoading] = useState(false);
  const [content, setContent] = useState('');
  const [editMode, setEditMode] = useState(initialEditMode);
  const [editContent, setEditContent] = useState('');
  const [isFullscreen, setIsFullscreen] = useState(false);

  // 监听全屏变化
  useEffect(() => {
    if (!screenfull.isEnabled) return;

    const handleChange = () => {
      setIsFullscreen(screenfull.isFullscreen);
    };

    screenfull.on('change', handleChange);
    return () => {
      screenfull.off('change', handleChange);
    };
  }, []);

  // 切换全屏
  const toggleFullscreen = () => {
    if (!screenfull.isEnabled || !dialogRef.current) return;

    if (screenfull.isFullscreen) {
      screenfull.exit();
    } else {
      screenfull.request(dialogRef.current);
    }
  };

  // Whenever the dialog opens, fetch content
  useEffect(() => {
    if (!open || !file) return;
    setEditMode(initialEditMode);
    setContent('');
    setLoading(true);
    filesApi
      .readContent(file.path)
      .then(res => {
        const c = res.data?.content ?? '';
        setContent(c);
        setEditContent(c);
      })
      .catch(() => {
        const msg = t('sys.file_management.view_error', '无法读取文件内容');
        setContent(msg);
        setEditContent(msg);
      })
      .finally(() => setLoading(false));
  }, [open, file, initialEditMode, t]);

  const handleSave = () => {
    if (!file) return;
    const blob = new Blob([editContent], { type: 'text/plain' });
    const f = Object.assign(blob, {
      name: file.name,
      lastModified: Date.now(),
    }) as File;
    const dir = file.path.substring(0, file.path.lastIndexOf('/'));
    uploadFile.mutate(
      { path: dir, file: f },
      {
        onSuccess: () => {
          setContent(editContent);
          setEditMode(false);
        },
      }
    );
  };

  return (
    <Dialog
      open={open}
      onOpenChange={o => {
        onOpenChange(o);
        if (!o) {
          setEditMode(false);
          // 退出全屏
          if (screenfull.isEnabled && screenfull.isFullscreen) {
            screenfull.exit();
          }
        }
      }}
    >
      <DialogContent
        ref={dialogRef}
        className="sm:max-w-3xl max-h-[80vh] flex flex-col gap-0 p-0"
      >
        {/* Header */}
        <DialogHeader className="flex flex-row items-center gap-3 px-6 py-4 shrink-0 border-b border-border">
          <DialogTitle className="flex-1 truncate text-base">
            <span className="text-muted-foreground font-normal mr-1">
              {editMode
                ? `${t('sys.file_management.editing', '编辑')}:`
                : `${t('sys.file_management.viewing', '查看')}:`}
            </span>
            <span className="font-mono">{file?.name}</span>
          </DialogTitle>
          <div className="flex items-center gap-2">
            {screenfull.isEnabled && (
              <Button
                variant="ghost"
                size="icon-sm"
                className="h-8 w-8"
                onClick={toggleFullscreen}
                title={isFullscreen ? '退出全屏' : '全屏'}
              >
                {isFullscreen ? (
                  <Minimize2 className="h-4 w-4" />
                ) : (
                  <Maximize2 className="h-4 w-4" />
                )}
              </Button>
            )}
            {!editMode && (
              <Button
                variant="outline"
                size="sm"
                onClick={() => {
                  setEditMode(true);
                  setEditContent(content);
                }}
              >
                <FilePen className="h-3.5 w-3.5 mr-1.5" />
                {t('sys.file_management.edit', '编辑')}
              </Button>
            )}
          </div>
        </DialogHeader>

        {/* Body */}
        <div className="flex-1 overflow-hidden min-h-0">
          {loading ? (
            <Loading
              fullHeight={false}
              className="h-full bg-[#1e1e1e]"
              size="sm"
            />
          ) : editMode ? (
            <MonacoEditor
              value={editContent}
              onChange={setEditContent}
              filename={file?.name}
              readOnly={false}
            />
          ) : (
            <MonacoEditor value={content} filename={file?.name} readOnly />
          )}
        </div>

        {/* Footer */}
        <DialogFooter className="px-6 py-4 shrink-0 border-t border-border">
          {editMode ? (
            <>
              <Button
                variant="outline"
                onClick={() => {
                  setEditMode(false);
                  setEditContent(content);
                }}
              >
                <X className="h-4 w-4 mr-1" />
                {t('common.cancel', '取消')}
              </Button>
              <Button onClick={handleSave} disabled={uploadFile.isPending}>
                <Check className="h-4 w-4 mr-1" />
                {t('common.save', '保存')}
              </Button>
            </>
          ) : (
            <Button variant="outline" onClick={() => onOpenChange(false)}>
              {t('common.close', '关闭')}
            </Button>
          )}
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
