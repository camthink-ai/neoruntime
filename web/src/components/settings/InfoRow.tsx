import { motion } from 'motion/react';

interface InfoRowProps {
  label: string;
  value: string | number;
  icon?: React.ReactNode;
  copyable?: boolean;
}

export default function InfoRow({
  label,
  value,
  icon,
  copyable,
}: InfoRowProps) {
  const handleCopy = () => {
    if (copyable && typeof value === 'string') {
      navigator.clipboard.writeText(value);
    }
  };

  return (
    <motion.div
      initial={{ opacity: 0, y: 10 }}
      animate={{ opacity: 1, y: 0 }}
      className="flex items-center justify-between py-3 border-b last:border-b-0"
    >
      <div className="flex items-center gap-3">
        {icon && <div className="text-muted-foreground">{icon}</div>}
        <span className="text-sm text-muted-foreground">{label}</span>
      </div>
      <div className="flex items-center gap-2">
        <span className="text-sm font-medium text-foreground">{value}</span>
        {copyable && (
          <button
            onClick={handleCopy}
            className="text-xs text-primary hover:text-primary/80 transition-colors"
          >
            Copy
          </button>
        )}
      </div>
    </motion.div>
  );
}
