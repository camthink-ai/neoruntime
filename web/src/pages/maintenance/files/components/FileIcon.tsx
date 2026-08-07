import type { FileInfo } from '../hooks/useFiles';
import { getFileExtension } from '../hooks/useFiles';
import {
  Folder,
  FileText,
  FileImage,
  FileVideo,
  FileCode,
  File,
} from 'lucide-react';

// ── File Icon ──────────────────────────────────────────────────────────────────
export function FileIcon({
  file,
  className = 'h-5 w-5',
  onClick,
}: {
  file: FileInfo;
  className?: string;
  onClick?: (e: any) => void;
}) {
  if (file.is_dir) return <Folder className={`${className} text-primary`} onClick={onClick} />;
  const ext = getFileExtension(file.name).toLowerCase();
  if (['png', 'jpg', 'jpeg', 'gif', 'webp', 'svg', 'bmp'].includes(ext)) {
 return (
      <FileImage className={`${className} text-primary`} onClick={onClick} />
    ); 
}
  if (['mp4', 'mkv', 'avi', 'mov', 'webm'].includes(ext)) {
 return (
      <FileVideo className={`${className} text-purple-500`} onClick={onClick} />
    ); 
}
  if (
    [
      'yml',
      'yaml',
      'json',
      'ts',
      'tsx',
      'js',
      'jsx',
      'py',
      'sh',
      'bash',
      'conf',
      'cfg',
      'toml',
      'ini',
    ].includes(ext)
  ) {
 return (
      <FileCode className={`${className} text-green-500`} onClick={onClick} />
    ); 
}
  if (['log', 'txt', 'md'].includes(ext)) {
 return (
      <FileText className={`${className} text-slate-500`} onClick={onClick} />
    ); 
}
  return <File className={`${className} text-slate-400`} onClick={onClick} />;
}

// ── Type Badge ─────────────────────────────────────────────────────────────────
const EXT_COLORS: Record<string, string> = {
  LOG: 'bg-blue-100 text-blue-600 dark:bg-blue-950 dark:text-blue-400',
  ISO: 'bg-slate-100 text-slate-600 dark:bg-slate-800 dark:text-slate-400',
  PNG: 'bg-green-100 text-green-600 dark:bg-green-950 dark:text-green-400',
  JPG: 'bg-green-100 text-green-600 dark:bg-green-950 dark:text-green-400',
  JPEG: 'bg-green-100 text-green-600 dark:bg-green-950 dark:text-green-400',
  YAML: 'bg-purple-100 text-purple-600 dark:bg-purple-950 dark:text-purple-400',
  YML: 'bg-purple-100 text-purple-600 dark:bg-purple-950 dark:text-purple-400',
  JSON: 'bg-yellow-100 text-yellow-700 dark:bg-yellow-950 dark:text-yellow-400',
  MP4: 'bg-pink-100 text-pink-600 dark:bg-pink-950 dark:text-pink-400',
  SH: 'bg-teal-100 text-teal-600 dark:bg-teal-950 dark:text-teal-400',
};

export function FileTypeBadge({ file }: { file: FileInfo }) {
  if (file.is_dir) {
    return (
      <span className="inline-flex items-center rounded px-2 py-0.5 text-[11px] font-semibold bg-orange-100 text-[#F24A00] dark:bg-orange-950 dark:text-orange-400">
        文件夹
      </span>
    );
  }
  const ext = getFileExtension(file.name);
  const cls =    EXT_COLORS[ext]
    ?? 'bg-slate-100 text-slate-600 dark:bg-slate-800 dark:text-slate-400';
  return (
    <span
      className={`inline-flex items-center rounded px-2 py-0.5 text-[11px] font-semibold ${cls}`}
    >
      {ext}
    </span>
  );
}

// ── Date formatter ─────────────────────────────────────────────────────────────
export function formatDate(isoDate: string): string {
  try {
    const d = new Date(isoDate);
    const y = d.getFullYear();
    const mo = String(d.getMonth() + 1).padStart(2, '0');
    const dd = String(d.getDate()).padStart(2, '0');
    const h = String(d.getHours()).padStart(2, '0');
    const mi = String(d.getMinutes()).padStart(2, '0');
    return `${y}-${mo}-${dd}\n${h}:${mi}`;
  } catch {
    return isoDate;
  }
}
