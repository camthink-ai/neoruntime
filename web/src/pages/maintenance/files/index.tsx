import { useState, useCallback, useEffect, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import { useQueryClient } from '@tanstack/react-query';
import { useSearchParams } from 'react-router-dom';
import { AlertCircle } from 'lucide-react';
import { toast } from 'sonner';

import { filesApi } from '@/services/api';
import { fetchDeviceInfo } from '@/services/settings';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import {
  Dialog,
  DialogContent,
  DialogTitle,
  DialogDescription,
  DialogFooter,
  DialogHeader,
} from '@/components/ui/dialog';
import FileUpload from '@/components/file-upload';
import { getItem, setItem } from '@/utils/storage';
import { Alert, AlertDescription } from '@/components/ui/alert';

import {
  useFiles,
  useCreateDirectory,
  useDeleteFile,
  useBatchDeleteFiles,
  useRenameFile,
  type FileInfo,
} from './hooks/useFiles';

import { FileTable } from './components/FileTable';
import FilePreviewDialog from './components/FilePreviewDialog';
import { MkdirDialog } from './components/dialogs/MkdirDialog';
import { RenameDialog } from './components/dialogs/RenameDialog';
import { DeleteDialog } from './components/dialogs/DeleteDialog';
import { FileBreadcrumb } from './components/FileBreadcrumb';
import { NavigationButtons } from './components/NavigationButtons';
import { ActionButtons } from './components/ActionButtons';
import Pagination from '@/components/Pagination';

const FILE_PATH_STORAGE_KEY = 'file-management-current-path';
const DEFAULT_ROOT = '/data/aipc';

export default function FileManagement() {
  const { t } = useTranslation();
  const queryClient = useQueryClient();
  const [searchParams] = useSearchParams();

  // ── Dynamic root path from device info ────────────────────────────────────
  const [rootPath, setRootPath] = useState(DEFAULT_ROOT);
  const [resolved, setResolved] = useState(false);
  useEffect(() => {
    // Check for ?path= query param first (from storage page partition click)
    const queryPath = searchParams.get('path');
    fetchDeviceInfo()
      .then(info => {
        const prefix = info.install_prefix || DEFAULT_ROOT;
        setRootPath(prefix);

        let initialPath: string;
        if (queryPath) {
          initialPath = queryPath;
        } else {
          const savedPath = getItem<string>(FILE_PATH_STORAGE_KEY);
          initialPath =            savedPath && savedPath.startsWith(prefix) ? savedPath : prefix;
        }

        setCurrentPath(initialPath);
        setPathHistory([initialPath]);
        setHistoryIndex(0);
        setResolved(true);
      })
      .catch(() => {
        if (queryPath) {
          setCurrentPath(queryPath);
          setPathHistory([queryPath]);
          setHistoryIndex(0);
        }
        setResolved(true);
      });
  }, []);

  // ── Navigation ─────────────────────────────────────────────────────────────
  const [currentPath, setCurrentPath] = useState(DEFAULT_ROOT);
  const [pathHistory, setPathHistory] = useState<string[]>([DEFAULT_ROOT]);
  const [historyIndex, setHistoryIndex] = useState(0);

  // Save path to localStorage whenever it changes
  useEffect(() => {
    if (resolved) setItem(FILE_PATH_STORAGE_KEY, currentPath);
  }, [currentPath, resolved]);

  const navigateTo = useCallback(
    (path: string) => {
      setCurrentPath(prev => {
        if (prev === path) return prev;
        setPathHistory(h => {
          const next = [...h.slice(0, historyIndex + 1), path];
          setHistoryIndex(next.length - 1);
          return next;
        });
        return path;
      });
      setSelectedPaths(new Set());
      setCurrentPage(1);
    },
    [historyIndex]
  );

  const goBack = () => {
    if (historyIndex <= 0) return;
    const idx = historyIndex - 1;
    setHistoryIndex(idx);
    setCurrentPath(pathHistory[idx]);
    setSelectedPaths(new Set());
    setCurrentPage(1);
  };

  const goForward = () => {
    if (historyIndex >= pathHistory.length - 1) return;
    const idx = historyIndex + 1;
    setHistoryIndex(idx);
    setCurrentPath(pathHistory[idx]);
    setSelectedPaths(new Set());
    setCurrentPage(1);
  };

  // ── Search & Pagination ────────────────────────────────────────────────────
  const [currentPage, setCurrentPage] = useState(1);
  const [pageSize, setPageSize] = useState(20);

  // ── 地址栏状态 ─────────────────────────────────────────────────────────────
  const [addressFocused, setAddressFocused] = useState(false);
  const [addressInput, setAddressInput] = useState(currentPath);
  const addressInputRef = useRef<HTMLInputElement>(null);

  // ── Selection ──────────────────────────────────────────────────────────────
  const [selectedPaths, setSelectedPaths] = useState<Set<string>>(new Set());

  const toggleSelect = (path: string) => setSelectedPaths(prev => {
      const next = new Set(prev);
      if (next.has(path)) {
        next.delete(path);
      } else {
        next.add(path);
      }
      return next;
    });

  // ── Data ───────────────────────────────────────────────────────────────────
  const {
    data: files = [],
    isLoading,
    isFetching,
  } = useFiles(
    currentPath,
    _deniedPath => {
      toast.error(
        t(
          'sys.file_management.path_error',
          'Path does not exist or access denied'
        )
      );
      if (historyIndex > 0) {
        goBack();
      } else {
        setCurrentPath(rootPath);
      }
    },
    resolved
  );
  const createDir = useCreateDirectory();
  const deleteFile = useDeleteFile();
  const batchDelete = useBatchDeleteFiles();
  const renameFile = useRenameFile();

  // ── Filtered / paged files ─────────────────────────────────────────────────
  const filtered = files;
  const totalFiles = filtered.length;
  const paged = filtered.slice(
    (currentPage - 1) * pageSize,
    currentPage * pageSize
  );

  const allCurrentSelected =    paged.length > 0 && paged.every(f => selectedPaths.has(f.path));
  const toggleSelectAll = () => {
    if (allCurrentSelected) {
      setSelectedPaths(prev => {
        const n = new Set(prev);
        paged.forEach(f => n.delete(f.path));
        return n;
      });
    } else {
      setSelectedPaths(prev => {
        const n = new Set(prev);
        paged.forEach(f => n.add(f.path));
        return n;
      });
    }
  };

  // ── File name click or double click (navigate into directory or preview files) ──
  const handleFileOpen = (file: FileInfo) => {
    if (file.is_dir) {
      navigateTo(file.path);
    } else if (isPreviewable(file.name)) {
      openPreview(file);
    } else {
      toast.info(
        t(
          'sys.file_management.preview_not_supported',
          'Preview not supported for this file type, please download instead'
        )
      );
    }
  };

  // ── Download ────────────────────────────────────────────────────────────────
  const handleDownload = async (file: FileInfo) => {
    try {
      const blob = await filesApi.download(file.path);
      const url = window.URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = url;
      link.setAttribute('download', file.name);
      document.body.appendChild(link);
      link.click();
      link.remove();
      window.URL.revokeObjectURL(url);
    } catch {
      /* silent */
    }
  };

  // ── Dialogs state ──────────────────────────────────────────────────────────
  const [mkdirOpen, setMkdirOpen] = useState(false);
  const [renameOpen, setRenameOpen] = useState(false);
  const [renameTarget, setRenameTarget] = useState<FileInfo | null>(null);
  const [deleteOpen, setDeleteOpen] = useState(false);
  const [deleteTarget, setDeleteTarget] = useState<FileInfo | null>(null);
  const [batchDeleteOpen, setBatchDeleteOpen] = useState(false);
  const [previewOpen, setPreviewOpen] = useState(false);
  const [previewFile, setPreviewFile] = useState<FileInfo | null>(null);
  const [uploadOpen, setUploadOpen] = useState(false);
  const [uploading, setUploading] = useState(false);
  const [conflictDialog, setConflictDialog] = useState<{
    open: boolean;
    file: File | null;
    existingName: string;
    newName: string;
  }>({
    open: false,
    file: null,
    existingName: '',
    newName: '',
  });

  // 判断文件是否支持预览
  const isPreviewable = (filename: string): boolean => {
    const ext = filename.split('.').pop()?.toLowerCase() || '';
    const previewableExts = [
      // 图片
      'jpg',
      'jpeg',
      'png',
      'gif',
      'bmp',
      'webp',
      'svg',
      // 视频
      'mp4',
      'webm',
      'ogg',
      'mov',
      'avi',
      // 音频
      'mp3',
      'wav',
      'ogg',
      'flac',
      'm4a',
      // PDF
      'pdf',
      // 代码
      'js',
      'jsx',
      'ts',
      'tsx',
      'py',
      'java',
      'c',
      'cpp',
      'h',
      'hpp',
      'go',
      'rs',
      'php',
      'rb',
      'sh',
      'bash',
      'css',
      'scss',
      'html',
      'xml',
      'json',
      'yaml',
      'yml',
      'toml',
      'ini',
      'conf',
      // 文本
      'txt',
      'md',
      'log',
      'csv',
    ];
    return previewableExts.includes(ext);
  };

  // ── Dialog handlers ────────────────────────────────────────────────────────
  const openRename = (file: FileInfo) => {
    setRenameTarget(file);
    setRenameOpen(true);
  };
  const openDelete = (file: FileInfo) => {
    setDeleteTarget(file);
    setDeleteOpen(true);
  };
  const openPreview = (file: FileInfo) => {
    if (!isPreviewable(file.name)) {
      toast.info(
        t(
          'sys.file_management.preview_not_supported',
          'Preview not supported for this file type, please download instead'
        )
      );
      return;
    }
    setPreviewFile(file);
    setPreviewOpen(true);
  };

  const handleMkdir = (name: string) => createDir.mutate(`${currentPath}/${name}`, {
      onSuccess: () => setMkdirOpen(false),
    });

  const handleRename = (file: FileInfo, newName: string) => {
    const dir = file.path.substring(0, file.path.lastIndexOf('/'));
    renameFile.mutate(
      { oldPath: file.path, newPath: `${dir}/${newName}` },
      {
        onSuccess: () => {
          setRenameOpen(false);
          setRenameTarget(null);
        },
      }
    );
  };

  const handleDelete = () => {
    if (!deleteTarget) return;
    deleteFile.mutate(deleteTarget.path, {
      onSuccess: () => {
        setDeleteOpen(false);
        setDeleteTarget(null);
        setSelectedPaths(prev => {
          const n = new Set(prev);
          n.delete(deleteTarget.path);
          return n;
        });
      },
    });
  };

  const handleBatchDelete = () => {
    if (selectedPaths.size === 0) return;
    const pathsArray = Array.from(selectedPaths);
    batchDelete.mutate(pathsArray, {
      onSuccess: () => {
        setBatchDeleteOpen(false);
        setSelectedPaths(new Set());
      },
    });
  };

  const handleBatchDownload = async () => {
    if (selectedPaths.size === 0) return;

    // 获取选中的文件（排除文件夹）
    const selectedFiles = files.filter(
      f => selectedPaths.has(f.path) && !f.is_dir
    );

    if (selectedFiles.length === 0) {
      toast.warning(
        t('sys.file_management.no_files_to_download', 'No files to download')
      );
      return;
    }

    // 如果只有一个文件，直接下载
    if (selectedFiles.length === 1) {
      handleDownload(selectedFiles[0]);
      return;
    }

    // 多个文件，打包成 ZIP 下载
    try {
      toast.info(
        t('sys.file_management.preparing_zip', {
          count: selectedFiles.length,
          defaultValue: 'Packaging {{count}} files...',
        })
      );

      const paths = selectedFiles.map(f => f.path);
      const zipBlob = await filesApi.batchDownload(paths);

      // 生成文件名：files_YYYYMMDD_HHMMSS.zip
      const now = new Date();
      const timestamp = now.toISOString().replace(/[:.]/g, '-').slice(0, 19);
      const zipName = `files_${timestamp}.zip`;

      // 触发下载
      const url = window.URL.createObjectURL(zipBlob);
      const link = document.createElement('a');
      link.href = url;
      link.setAttribute('download', zipName);
      document.body.appendChild(link);
      link.click();
      link.remove();
      window.URL.revokeObjectURL(url);

      toast.success(
        t('sys.file_management.download_complete', {
          count: selectedFiles.length,
          defaultValue: 'Successfully downloaded {{count}} files',
        })
      );
    } catch (error) {
      console.error('Batch download failed:', error);
      toast.error(
        t('sys.file_management.batch_download_failed', 'Batch download failed')
      );
    }
  };

  // ── Upload ─────────────────────────────────────────────────────────────────
  // 生成新的文件名（如果存在同名文件）
  const generateUniqueFileName = (
    fileName: string,
    existingFiles: FileInfo[]
  ): string => {
    // 如果文件名不存在，直接返回
    if (!existingFiles.some(f => f.name === fileName)) {
      return fileName;
    }

    // 提取文件名和扩展名
    const lastDotIndex = fileName.lastIndexOf('.');
    const name =      lastDotIndex !== -1 ? fileName.substring(0, lastDotIndex) : fileName;
    const ext = lastDotIndex !== -1 ? fileName.substring(lastDotIndex) : '';

    // 查找下一个可用的数字
    let counter = 1;
    let newName: string;
    const checkExists = (targetName: string) => existingFiles.some(f => f.name === targetName);

    do {
      newName = `${name}(${counter})${ext}`;
      counter++;
    } while (checkExists(newName));

    return newName;
  };

  const handleUpload = async (uploadFiles: File[]) => {
    if (uploadFiles.length === 0) return;
    const file = uploadFiles[0];

    // 检查是否存在同名文件
    const existingFile = files.find(f => f.name === file.name);

    if (existingFile && !existingFile.is_dir) {
      // 存在同名文件，显示冲突对话框
      const newName = generateUniqueFileName(file.name, files);
      setConflictDialog({
        open: true,
        file,
        existingName: file.name,
        newName,
      });
      return;
    }

    // 没有冲突，直接上传
    await doUpload(file);
  };

  const doUpload = async (file: File, newFileName?: string) => {
    setUploading(true);
    try {
      // 如果指定了新文件名，重命名文件
      const fileToUpload = newFileName
        ? new File([file], newFileName, { type: file.type })
        : file;

      await filesApi.upload(currentPath, fileToUpload);
      queryClient.invalidateQueries({ queryKey: ['files'] });
      setUploadOpen(false);
      toast.success(t('sys.file_management.upload_success', 'File uploaded'));
    } catch (error) {
      console.error('Upload failed:', error);
      toast.error(t('sys.file_management.upload_failed', 'File upload failed'));
    } finally {
      setUploading(false);
    }
  };

  const handleConflictReplace = () => {
    if (!conflictDialog.file) return;
    setConflictDialog({
      open: false,
      file: null,
      existingName: '',
      newName: '',
    });
    doUpload(conflictDialog.file);
  };

  const handleConflictRename = () => {
    if (!conflictDialog.file || !conflictDialog.newName) return;
    const { file, newName } = conflictDialog;
    setConflictDialog({
      open: false,
      file: null,
      existingName: '',
      newName: '',
    });
    doUpload(file, newName);
  };

  return (
    <div className="flex flex-col h-full p-4 md:p-6">
      {/* ── Header ──────────────────────────────────────────────────────────── */}
      <div className="z-10 mb-4 flex flex-col gap-4 lg:flex-row lg:items-center lg:justify-between">
        <div className="flex w-full min-w-0 flex-1 flex-col gap-3 sm:flex-row sm:items-center sm:gap-4">
          <NavigationButtons
            historyIndex={historyIndex}
            pathHistoryLength={pathHistory.length}
            currentPath={currentPath}
            rootPath={rootPath}
            onBack={goBack}
            onForward={goForward}
            onGoUp={() => {
              if (currentPath === rootPath) return;
              const parentPath =                currentPath.substring(0, currentPath.lastIndexOf('/'))
                || rootPath;
              navigateTo(parentPath);
            }}
          />
          <div
            className="relative flex h-9 w-full min-w-0 items-center overflow-hidden rounded border border-border/50 bg-background transition-colors focus-within:border-primary/50 cursor-text sm:w-[400px] sm:shrink-0 md:w-[500px]"
            onClick={() => {
              if (!addressFocused) {
                setAddressInput(currentPath);
                setAddressFocused(true);
                setTimeout(() => {
                  addressInputRef.current?.select();
                }, 0);
              }
            }}
          >
            {addressFocused ? (
              <Input
                ref={addressInputRef}
                value={addressInput}
                onChange={e => setAddressInput(e.target.value)}
                onBlur={() => {
                  setAddressFocused(false);
                }}
                onKeyDown={e => {
                  if (e.key === 'Enter') {
                    const path = addressInput.trim() || rootPath;
                    navigateTo(path);
                    setAddressFocused(false);
                    (e.target as HTMLInputElement).blur();
                  } else if (e.key === 'Escape') {
                    setAddressInput(currentPath);
                    setAddressFocused(false);
                    (e.target as HTMLInputElement).blur();
                  }
                }}
                className="h-full w-full rounded-md border-0 focus-visible:ring-0 shadow-none px-2.5 text-sm bg-transparent font-mono"
              />
            ) : (
              <div className="flex items-center pl-1.5 h-full w-full overflow-hidden">
                <FileBreadcrumb
                  currentPath={currentPath}
                  onNavigate={setCurrentPath}
                  rootPath={rootPath}
                />
              </div>
            )}
          </div>
        </div>

        <div className="w-full min-w-0 lg:w-auto lg:shrink-0">
          <ActionButtons
            selectedCount={selectedPaths.size}
            isFetching={isFetching}
            onBatchDelete={() => setBatchDeleteOpen(true)}
            onBatchDownload={handleBatchDownload}
            onUpload={() => setUploadOpen(true)}
            onCreateFolder={() => setMkdirOpen(true)}
            onRefresh={() => queryClient.invalidateQueries({ queryKey: ['files'] })}
          />
        </div>
      </div>

      <div className="flex flex-1 min-h-0 flex-col bg-white dark:bg-card rounded-xl shadow-[0_2px_12px_rgba(0,0,0,0.04)] border border-border/40 overflow-hidden">
        {/* ── Table ───────────────────────────────────────────────────────────── */}
        <div className="flex-1 min-h-0 overflow-hidden">
          <FileTable
            files={paged}
            isLoading={isLoading}
            searchText=""
            selectedPaths={selectedPaths}
            allCurrentSelected={allCurrentSelected}
            onToggleSelectAll={toggleSelectAll}
            onToggleSelect={toggleSelect}
            onFileOpen={handleFileOpen}
            onDownload={handleDownload}
            onRename={openRename}
            onDelete={openDelete}
            isPreviewable={isPreviewable}
          />
        </div>

        {/* ── Footer ──────────────────────────────────────────────────────────── */}
        <Pagination
          currentPage={currentPage}
          pageSize={pageSize}
          total={totalFiles}
          onPageChange={setCurrentPage}
          pageSizeOptions={[20, 50, 100]}
          onPageSizeChange={size => {
            setPageSize(size);
            setCurrentPage(1);
          }}
        />
      </div>

      {/* ── Dialogs ─────────────────────────────────────────────────────────── */}
      <MkdirDialog
        open={mkdirOpen}
        onOpenChange={setMkdirOpen}
        onCreate={handleMkdir}
        isPending={createDir.isPending}
      />

      <RenameDialog
        file={renameTarget}
        open={renameOpen}
        onOpenChange={setRenameOpen}
        onRename={handleRename}
        isPending={renameFile.isPending}
      />

      <DeleteDialog
        file={deleteTarget}
        open={deleteOpen}
        onOpenChange={setDeleteOpen}
        onDelete={handleDelete}
      />

      {/* 批量删除对话框 */}
      <Dialog open={batchDeleteOpen} onOpenChange={setBatchDeleteOpen}>
        <DialogContent className="sm:max-w-[480px] w-[95vw]">
          <DialogHeader>
            <DialogTitle className="flex items-center gap-2">
              <AlertCircle className="h-5 w-5 text-destructive" />
              {t(
                'sys.file_management.batch_delete_confirm',
                'Batch Delete Confirmation'
              )}
            </DialogTitle>
            <DialogDescription>
              {t('sys.file_management.batch_delete_warning', {
                count: selectedPaths.size,
                defaultValue:
                  'Are you sure you want to delete {{count}} selected files/folders? This cannot be undone.',
              })}
            </DialogDescription>
          </DialogHeader>
          <div className="max-h-[300px] overflow-y-auto py-4">
            <div className="space-y-2">
              {Array.from(selectedPaths).map(path => {
                const fileName = path.split('/').pop() || path;
                return (
                  <div
                    key={path}
                    className="flex items-center gap-2 px-3 py-2 bg-muted/50 rounded-md text-sm"
                  >
                    <span className="font-mono text-muted-foreground truncate">
                      {fileName}
                    </span>
                  </div>
                );
              })}
            </div>
          </div>
          <DialogFooter className="gap-2">
            <Button
              variant="outline"
              onClick={() => setBatchDeleteOpen(false)}
              disabled={batchDelete.isPending}
            >
              {t('sys.file_management.cancel', 'Cancel')}
            </Button>
            <Button
              variant="destructive"
              onClick={handleBatchDelete}
              disabled={batchDelete.isPending}
            >
              {batchDelete.isPending
                ? t('sys.file_management.deleting', 'Deleting...')
                : t('sys.file_management.confirm_delete', 'Confirm Delete')}
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <FilePreviewDialog
        file={previewFile}
        open={previewOpen}
        onOpenChange={setPreviewOpen}
      />

      <Dialog open={uploadOpen} onOpenChange={setUploadOpen}>
        <DialogContent className="sm:max-w-[500px] w-[95vw]">
          <DialogTitle>
            {t('sys.file_management.upload', 'Upload File')}
          </DialogTitle>
          <DialogDescription className="hidden">
            Upload files to current directory
          </DialogDescription>
          <div className="py-4">
            <FileUpload
              single
              onUpload={handleUpload}
              loading={uploading}
              placeholder={t(
                'sys.file_management.upload_placeholder',
                'Drag files here or click to select'
              )}
            />
          </div>
        </DialogContent>
      </Dialog>

      {/* 文件冲突对话框 */}
      <Dialog
        open={conflictDialog.open}
        onOpenChange={open => {
          if (!open) {
            setConflictDialog({
              open: false,
              file: null,
              existingName: '',
              newName: '',
            });
          }
        }}
      >
        <DialogContent className="sm:max-w-[480px] w-[95vw]">
          <DialogHeader>
            <DialogTitle className="flex items-center gap-2">
              <AlertCircle className="h-5 w-5 text-yellow-500" />
              {t('sys.file_management.file_conflict', 'File Conflict')}
            </DialogTitle>
          </DialogHeader>
          <div className="py-4">
            <Alert>
              <AlertCircle className="h-4 w-4" />
              <AlertDescription>
                {t('sys.file_management.conflict_description', {
                  name: conflictDialog.existingName,
                  defaultValue:
                    'File "{{name}}" already exists. Replace or rename?',
                })}
              </AlertDescription>
            </Alert>

            {conflictDialog.newName && (
              <div className="mt-4 p-3 bg-muted/50 rounded-lg">
                <p className="text-sm text-muted-foreground mb-1">
                  {t('sys.file_management.suggested_name', 'Suggested name: ')}
                </p>
                <p className="text-sm font-medium font-mono">
                  {conflictDialog.newName}
                </p>
              </div>
            )}
          </div>
          <DialogFooter className="gap-2">
            <Button
              variant="outline"
              onClick={() => setConflictDialog({
                  open: false,
                  file: null,
                  existingName: '',
                  newName: '',
                })}
            >
              {t('sys.file_management.cancel', 'Cancel')}
            </Button>
            <Button variant="default" onClick={handleConflictReplace}>
              {t('sys.file_management.replace', 'Replace')}
            </Button>
            <Button variant="secondary" onClick={handleConflictRename}>
              {t('sys.file_management.rename_upload', 'Rename & Upload')}
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </div>
  );
}
