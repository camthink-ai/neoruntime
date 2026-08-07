import { motion } from 'motion/react';

interface StorageBarProps {
  label: string;
  used: number;
  total: number;
  color?: string;
}

export default function StorageBar({
  label,
  used,
  total,
  color = '#1890ff',
}: StorageBarProps) {
  const percentage = (used / total) * 100;
  const formatSize = (bytes: number) => {
    if (bytes >= 1024 * 1024 * 1024) {
      return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
    }
    if (bytes >= 1024 * 1024) {
      return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
    }
    return `${(bytes / 1024).toFixed(2)} KB`;
  };

  return (
    <div className="space-y-2">
      <div className="flex items-center justify-between">
        <span className="text-sm font-medium text-foreground">{label}</span>
        <span className="text-sm text-muted-foreground">
          {formatSize(used)} / {formatSize(total)}
        </span>
      </div>
      <div className="w-full h-3 bg-border rounded-full overflow-hidden">
        <motion.div
          initial={{ width: 0 }}
          animate={{ width: `${percentage}%` }}
          transition={{ duration: 1, ease: 'easeOut' }}
          className="h-full rounded-full"
          style={{ backgroundColor: color }}
        />
      </div>
      <div className="flex items-center justify-between text-xs text-muted-foreground">
        <span>{percentage.toFixed(1)}% used</span>
        <span>{formatSize(total - used)} available</span>
      </div>
    </div>
  );
}
