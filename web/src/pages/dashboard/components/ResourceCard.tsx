interface ResourceCardProps {
  title: string;
  value: string | number;
  progressPercent: number;
  progressColorClass?: string;
  subtext: string;
  icon?: React.ReactNode;
}

export default function ResourceCard({
  title,
  value,
  progressPercent,
  progressColorClass = 'text-[#f24a00]',
  subtext,
  icon,
}: ResourceCardProps) {
  const percent = Math.min(100, Math.max(0, progressPercent));
  const radius = 42;
  const circumference = 2 * Math.PI * radius;
  const strokeDashoffset = circumference - (percent / 100) * circumference;

  return (
    <div className="bg-card rounded-2xl p-4 shadow-sm border border-border flex flex-col items-center justify-center">
      <h3 className="text-base font-bold text-foreground mb-4 flex items-center gap-2">
        {icon}
        {title}
      </h3>

      {/* 扇形圆环图 */}
      <div className="relative w-28 h-28 my-2">
        <svg className="w-28 h-28 -rotate-90" viewBox="0 0 100 100">
          <circle
            cx="50"
            cy="50"
            r={radius}
            fill="none"
            stroke="currentColor"
            strokeWidth="6"
            className="text-secondary/30"
          />
          <circle
            cx="50"
            cy="50"
            r={radius}
            fill="none"
            stroke="currentColor"
            strokeWidth="6"
            strokeLinecap="round"
            className={progressColorClass}
            style={{
              strokeDasharray: circumference,
              strokeDashoffset,
            }}
          />
        </svg>
        <div className="absolute inset-0 flex items-center justify-center">
          <span className="text-lg font-bold text-foreground">
            {percent.toFixed(1)}%
          </span>
        </div>
      </div>

      {/* 底部信息 */}
      <div className="text-center">
        <span className="text-xl font-bold text-foreground">{value}</span>
        <p className="text-xs text-muted-foreground mt-0.5">{subtext}</p>
      </div>
    </div>
  );
}
