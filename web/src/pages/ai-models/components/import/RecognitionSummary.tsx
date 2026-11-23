import { useTranslation } from 'react-i18next';
import { FileSearch, Ruler, Settings2, Boxes } from 'lucide-react';

export interface RecognitionSummaryProps {
  /** Parse-suggested type id ('' when the parse had no opinion). */
  suggestedType: string;
  /** Currently chosen type id — differs from the suggestion only after the
   *  user overrode it, which the card surfaces as a small flag. */
  chosenType: string;
  /** Localized label of the chosen type (from the type options table). */
  chosenTypeLabel: string;
  /** Localized label of the suggested type, when resolvable. */
  suggestedTypeLabel?: string | null;
  /** Active postprocess profile select value (undefined when the type has
   *  no profile concept — hidden then). */
  profile?: string | undefined;
  /** Parsed network dimensions, when the HEF carried them. */
  dims?: { width?: number; height?: number } | null;
  /** Server classification of the compiled output: 'nms' | 'feature_map'
   *  | '' (unknown). */
  outputFormat: string;
}

/** One chip cell of the recognition summary. */
function Chip({
  icon: Icon,
  label,
  value,
  tone,
}: {
  icon: typeof Ruler;
  label: string;
  value: string;
  tone?: 'default' | 'mismatch';
}) {
  return (
    <div className="flex min-w-0 items-center gap-2">
      <Icon className="h-3.5 w-3.5 shrink-0 text-muted-foreground" />
      <div className="flex min-w-0 items-baseline gap-1.5">
        <span className="shrink-0 text-xs text-muted-foreground">{label}</span>
        <span
          className={`truncate text-xs font-medium ${
            tone === 'mismatch' ? 'text-amber-600 dark:text-amber-400' : ''
          }`}
          title={value}
        >
          {value}
        </span>
      </div>
    </div>
  );
}

/**
 * Recognition summary card at the top of the import wizard's configure
 * screen — the "identify" half of 导入=识别并建立. The parse already
 * answered what this file is; the card restates that answer (suggested
 * type, matched profile, network dimensions, output format) in one place
 * so confirming it is a glance instead of a page-by-page form audit. The
 * configure pages then only matter when the user disagrees with the
 * recognition.
 */
export default function RecognitionSummary({
  suggestedType,
  chosenType,
  chosenTypeLabel,
  suggestedTypeLabel,
  profile,
  dims,
  outputFormat,
}: RecognitionSummaryProps) {
  const { t } = useTranslation();
  const overridden = suggestedType !== '' && suggestedType !== chosenType;

  const formatText =    outputFormat === 'nms'
      ? t('sys.ai_models.wizard.summary_format_nms', 'NMS（平台可解码）')
      : outputFormat === 'feature_map'
        ? t('sys.ai_models.wizard.summary_format_feature_map', '特征图')
        : t('sys.ai_models.wizard.summary_format_unknown', '未识别');

  return (
    <div className="mb-5 rounded-lg border bg-muted/30 px-4 py-3">
      <div className="mb-2 flex items-center gap-2 text-xs font-medium text-foreground">
        <FileSearch className="h-4 w-4 text-primary" />
        {t('sys.ai_models.wizard.summary_title', '识别结果')}
      </div>
      <div className="grid grid-cols-1 gap-x-6 gap-y-2 sm:grid-cols-2 lg:grid-cols-4">
        <Chip
          icon={Settings2}
          label={t('sys.ai_models.wizard.summary_type', '类型')}
          value={
            overridden && suggestedTypeLabel
              ? `${chosenTypeLabel} ← ${suggestedTypeLabel}`
              : chosenTypeLabel
          }
          tone={overridden ? 'mismatch' : 'default'}
        />
        {profile !== undefined && profile !== '' && (
          <Chip
            icon={Boxes}
            label={t('sys.ai_models.wizard.summary_profile', '解码档案')}
            value={profile}
          />
        )}
        {dims && (dims.width != null || dims.height != null) && (
          <Chip
            icon={Ruler}
            label={t('sys.ai_models.wizard.summary_dims', '网络尺寸')}
            value={`${dims.width ?? '?'} × ${dims.height ?? '?'}`}
          />
        )}
        <Chip
          icon={FileSearch}
          label={t('sys.ai_models.wizard.summary_output', '输出形式')}
          value={formatText}
        />
      </div>
      {overridden && (
        <p className="mt-2 text-xs text-amber-600 dark:text-amber-400">
          {t(
            'sys.ai_models.wizard.summary_overridden',
            '已手动改写解析建议的类型——若非有意为之，请改回以获得匹配的后处理。'
          )}
        </p>
      )}
    </div>
  );
}
