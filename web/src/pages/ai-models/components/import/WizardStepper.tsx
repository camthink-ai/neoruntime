import { useTranslation } from 'react-i18next';
import { Check } from 'lucide-react';

export type WizardStepStatus = 'done' | 'active' | 'upcoming' | 'optional';

interface WizardStep {
  label: string;
  status: WizardStepStatus;
}

interface WizardStepperProps {
  steps: WizardStep[];
}

const CIRCLE_BASE =  'flex h-5 w-5 shrink-0 items-center justify-center rounded-full text-[10px] font-semibold';

const CIRCLE_BY_STATUS: Record<WizardStepStatus, string> = {
  done: 'bg-primary text-primary-foreground',
  active:
    'bg-primary text-primary-foreground ring-2 ring-primary/30 ring-offset-1 ring-offset-background',
  upcoming: 'border border-border text-muted-foreground',
  // Update mode may legitimately skip parsing (metadata-only edit).
  optional: 'border border-dashed border-border text-muted-foreground',
};

/**
 * Progress rail between the wizard title and body: upload → parse →
 * configure. Purely presentational — status derivation lives in the dialog.
 */
export default function WizardStepper({ steps }: WizardStepperProps) {
  const { t } = useTranslation();

  return (
    <ol
      className="flex items-center gap-2 px-4 pb-3 sm:px-6"
      aria-label={t('sys.ai_models.wizard.stepper_label', 'Import progress')}
    >
      {steps.map((step, i) => {
        const reachable = steps[i - 1]?.status === 'done';
        return (
          <li
            key={step.label}
            className="flex min-w-0 items-center gap-2"
            aria-current={step.status === 'active' ? 'step' : undefined}
          >
            {i > 0 && (
              <span
                className={`h-px w-4 shrink-0 sm:w-8 ${
                  reachable ? 'bg-primary/50' : 'bg-border'
                }`}
                aria-hidden="true"
              />
            )}
            <span className={`${CIRCLE_BASE} ${CIRCLE_BY_STATUS[step.status]}`}>
              {step.status === 'done' ? <Check className="h-3 w-3" /> : i + 1}
            </span>
            <span
              className={`truncate text-xs ${
                step.status === 'active'
                  ? 'font-medium text-foreground'
                  : 'text-muted-foreground'
              }`}
            >
              {step.label}
            </span>
          </li>
        );
      })}
    </ol>
  );
}
