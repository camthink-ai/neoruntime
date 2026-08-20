import { Minus, Plus } from 'lucide-react';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Slider } from '@/components/ui/slider';

const STEP_PERCENT = 1; // 1% of range per +/- click

const clamp = (value: number, min: number, max: number) => Math.min(max, Math.max(min, value));
const clamp01 = (value: number) => clamp(value, 0, 1);

interface MotorAxisControlProps {
  label: string;
  displayValue: string;
  level: number; // 0-100
  onLevelChange: (level: number) => void;
  onCommit: (normalizedLevel: number) => Promise<void>;
  busy: boolean;
  canDecrement: boolean;
  canIncrement: boolean;
  disabled?: boolean;
  /** Granularity of the +/- buttons and slider drag, in percent of range. */
  stepPercent?: number;
}

export default function MotorAxisControl({
  label,
  displayValue,
  level,
  onLevelChange,
  onCommit,
  busy,
  canDecrement,
  canIncrement,
  disabled,
  stepPercent = STEP_PERCENT,
}: MotorAxisControlProps) {
  const isDisabled = disabled || busy;

  const handleStep = async (direction: -1 | 1) => {
    const next = clamp(level + direction * stepPercent, 0, 100);
    onLevelChange(next);
    await onCommit(clamp01(next / 100));
  };

  return (
    <div className="space-y-2">
      <div className="flex items-center justify-between text-sm text-muted-foreground">
        <span>{label}</span>
        <Badge variant="secondary" className="text-[10px] px-4 py-1 font-mono">
          {displayValue}
        </Badge>
      </div>
      <div className="flex items-center gap-2">
        <Button
          type="button"
          variant="outline"
          className="shrink-0 w-10 h-10 rounded-xl"
          disabled={isDisabled || !canDecrement}
          onClick={() => handleStep(-1)}
        >
          <Minus className="w-4 h-4" />
        </Button>
        <Slider
          value={[level]}
          min={0}
          max={100}
          step={stepPercent}
          disabled={isDisabled}
          onValueChange={values => {
            const next =              Array.isArray(values) && Number.isFinite(values[0])
                ? values[0]
                : level;
            onLevelChange(clamp(Math.round(next), 0, 100));
          }}
          onValueCommit={values => {
            const raw =              Array.isArray(values) && Number.isFinite(values[0])
                ? values[0]
                : level;
            const normalized = clamp(Math.round(raw), 0, 100);
            onLevelChange(normalized);
            onCommit(clamp01(normalized / 100));
          }}
        />
        <Button
          type="button"
          variant="outline"
          className="shrink-0 w-10 h-10 rounded-xl"
          disabled={isDisabled || !canIncrement}
          onClick={() => handleStep(1)}
        >
          <Plus className="w-4 h-4" />
        </Button>
      </div>
    </div>
  );
}
