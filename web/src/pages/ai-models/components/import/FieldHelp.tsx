import { useTranslation } from 'react-i18next';
import { CircleHelp } from 'lucide-react';

import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from '@/components/ui/popover';

interface FieldHelpProps {
  /** Schema field key — the explanation resolves from
   *  sys.ai_models.form.<fieldKey>_tooltip. Fields without a locale entry
   *  render no marker, the same missing-key contract as the hint texts. */
  fieldKey: string;
}

/**
 * Click-to-explain "?" beside a form label. Schema keys that surface as raw
 * English names (top_k, temperature…) carry their explanation behind this
 * marker instead of an always-on hint, keeping the form compact — the label
 * stays scannable, the definition one click away.
 */
export default function FieldHelp({ fieldKey }: FieldHelpProps) {
  const { t } = useTranslation();
  const text = t(`sys.ai_models.form.${fieldKey}_tooltip`, '');
  if (!text) return null;
  return (
    <Popover>
      <PopoverTrigger
        type="button"
        aria-label={t(
          'sys.ai_models.form.field_help_aria',
          'Explain this field'
        )}
        className="inline-flex h-4 w-4 shrink-0 items-center justify-center rounded-full text-muted-foreground transition-colors hover:text-foreground focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-1"
      >
        <CircleHelp className="h-3.5 w-3.5" />
      </PopoverTrigger>
      <PopoverContent className="w-72 text-xs leading-relaxed">
        {text}
      </PopoverContent>
    </Popover>
  );
}
