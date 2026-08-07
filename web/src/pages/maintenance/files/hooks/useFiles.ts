import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { filesApi } from '@/services/api';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';

export interface FileInfo {
  name: string;
  path: string;
  size: number;
  mode: string;
  mod_time: string;
  is_dir: boolean;
}

export const formatBytes = (bytes: number): string => {
  if (bytes === 0) return '—';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return `${parseFloat((bytes / k ** i).toFixed(2))} ${sizes[i]}`;
};

export const getFileExtension = (name: string): string => {
  const parts = name.split('.');
  if (parts.length > 1) return parts[parts.length - 1].toUpperCase();
  return 'FILE';
};

export function useFiles(
  path: string,
  onAccessDenied?: (deniedPath: string) => void,
  enabled = true
) {
  return useQuery<FileInfo[]>({
    queryKey: ['files', path],
    enabled: !!path && enabled,
    queryFn: async () => {
      try {
        const res = await filesApi.listSilent(path);
        return res.data?.files || [];
      } catch (err: any) {
        const code = err?.data?.code ?? err?.response?.data?.code;
        if (code === 8004 || code === 4003) {
          onAccessDenied?.(path);
        }
        throw err;
      }
    },
    retry: false,
  });
}
export function useCreateDirectory() {
  const queryClient = useQueryClient();
  const { t } = useTranslation();
  return useMutation({
    mutationFn: (path: string) => filesApi.createDirectory(path),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['files'] });
      toast.success(t('sys.file_management.mkdir_success', '文件夹创建成功'));
    },
    onError: (err: any) => {
      toast.error(
        err.response?.data?.message
          || t('sys.file_management.mkdir_error', '创建文件夹失败')
      );
    },
  });
}

export function useDeleteFile() {
  const queryClient = useQueryClient();
  const { t } = useTranslation();
  return useMutation({
    mutationFn: (path: string) => filesApi.delete(path),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['files'] });
      toast.success(t('sys.file_management.delete_success', '删除成功'));
    },
    onError: (err: any) => {
      toast.error(
        err.response?.data?.message
          || t('sys.file_management.delete_error', '删除失败')
      );
    },
  });
}

export function useBatchDeleteFiles() {
  const queryClient = useQueryClient();
  const { t } = useTranslation();
  return useMutation({
    mutationFn: (paths: string[]) => filesApi.batchDelete(paths),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['files'] });
      toast.success(
        t('sys.file_management.batch_delete_success', '批量删除成功')
      );
    },
    onError: (err: any) => {
      toast.error(
        err.response?.data?.message
          || t('sys.file_management.batch_delete_error', '批量删除失败')
      );
    },
  });
}

export function useRenameFile() {
  const queryClient = useQueryClient();
  const { t } = useTranslation();
  return useMutation({
    mutationFn: ({ oldPath, newPath }: { oldPath: string; newPath: string }) => filesApi.rename(oldPath, newPath),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['files'] });
      toast.success(t('sys.file_management.rename_success', '重命名成功'));
    },
    onError: (err: any) => {
      toast.error(
        err.response?.data?.message
          || t('sys.file_management.rename_error', '重命名失败')
      );
    },
  });
}

export function useUploadFile() {
  const queryClient = useQueryClient();
  const { t } = useTranslation();
  return useMutation({
    mutationFn: ({ path, file }: { path: string; file: File }) => filesApi.upload(path, file),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['files'] });
      toast.success(t('sys.file_management.upload_success', '上传成功'));
    },
    onError: (err: any) => {
      toast.error(
        err.response?.data?.message
          || t('sys.file_management.upload_error', '上传失败')
      );
    },
  });
}
