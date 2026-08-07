export function formatSize(bytes: number): string {
  const gb = bytes / (1024 * 1024 * 1024);
  if (gb >= 1024) return `${(gb / 1024).toFixed(1)} TB`;
  return `${gb.toFixed(1)} GB`;
}

export function getProgressColor(percent: number): string {
  if (percent >= 80) return 'bg-red-500';
  if (percent >= 50) return 'bg-yellow-500';
  return 'bg-emerald-500';
}

export function getStrokeColor(percent: number): string {
  if (percent >= 80) return '#ef4444';
  if (percent >= 50) return '#eab308';
  return '#10b981';
}

export function getProgressTextColor(percent: number): string {
  if (percent >= 80) return 'text-red-500';
  if (percent >= 50) return 'text-yellow-500';
  return 'text-emerald-500';
}
