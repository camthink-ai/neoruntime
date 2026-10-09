import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { ChevronDown, ChevronRight, AlertTriangle, Zap } from 'lucide-react';
import { Button } from '@/components/ui/button';
import type { ModelFieldDef, ModelTypeDef } from '@/hooks/useModels';
import {
  effectivePostprocess,
  fieldKeyToPoseVariantKey,
  fieldKeyToVariantKey,
  filterFieldsByProfile,
  variantOverridesProfile,
} from '../../lib/modelImportFlow';
import { EffectBadge } from './ModelSchemaField';

export interface EffectiveConfigPanelProps {
  modelType: string;
  outputMode: string;
  variant: string;
  /** Stored config (already parsed to an object by the dialog). */
  config: Record<string, unknown>;
  inputWidth?: number;
  inputHeight?: number;
  /** The type's schema fields + per-profile effect table, from the
   *  capabilities model type options. */
  fields: ModelFieldDef[];
  profileParamEffects?: ModelTypeDef['profile_param_effects'];
}

/** Blob-dialect key for a field key under the given type (presentation
 *  bridge — mirrors the Go registry table). */
function blobKeyFor(modelType: string, fieldKey: string): string | undefined {
  if (modelType === 'keypoint') {
    return fieldKeyToPoseVariantKey[fieldKey];
  }
  return fieldKeyToVariantKey[fieldKey];
}

/** Human-friendly rendering of one effective value. */
function ValueText({ value }: { value: unknown }) {
  if (value === undefined || value === null) {
    return <span className="text-xs text-muted-foreground">-</span>;
  }
  const text =    typeof value === 'string'
      ? value
      : Array.isArray(value)
        ? value.join(', ')
        : JSON.stringify(value);
  return (
    <span
      className="block truncate font-mono text-xs text-foreground"
      title={text}
    >
      {text}
    </span>
  );
}

/**
 * 详情页的"生效配置"面板 — the detail page's answer to "运行时到底收到了
 * 什么". It projects the stored row through the same resolution order the
 * load-time composer uses (raw mode → nothing; `{`-variant → verbatim
 * override; else composed from config + profile), so the gap between
 * 存储 and 生效 (escape-hatch override, silently-dropped keys) is visible
 * here instead of only in the journal.
 */
export default function EffectiveConfigPanel({
  modelType,
  outputMode,
  variant,
  config,
  inputWidth,
  inputHeight,
  fields,
  profileParamEffects,
}: EffectiveConfigPanelProps) {
  const { t } = useTranslation();
  const [payloadOpen, setPayloadOpen] = useState(false);

  const effective = effectivePostprocess({
    modelType,
    outputMode,
    variant,
    config,
    inputWidth,
    inputHeight,
  });

  const profile =    typeof config.postprocess_profile === 'string'
      ? config.postprocess_profile
      : undefined;
  const profileEffects = profileParamEffects?.[profile ?? ''];
  const visible = filterFieldsByProfile(fields, profile);

  // Field-dialect rows: each knob the type exposes, with its per-profile
  // effect badge and the value the runtime actually received for it.
  const rows = visible
    .map(f => {
      const effect = profileEffects?.[f.key] ?? f.effect;
      const blobKey = blobKeyFor(modelType, f.key);
      const value =        effective.blob && blobKey && blobKey in effective.blob
          ? effective.blob[blobKey]
          : f.key === 'postprocess_profile'
            ? profile
            : undefined;
      return { f, effect, value };
    })
    .filter(
      r => r.f.key === 'postprocess_profile'
        || r.value !== undefined
        || r.effect === 'advisory'
        || r.effect === 'metadata'
    );

  const hasEscape = variantOverridesProfile(variant);
  const payloadJson =    effective.source === 'custom-blob' || effective.source === 'composed'
      ? JSON.stringify(effective.blob ?? variant, null, 2)
      : null;

  return (
    <section className="rounded-lg border">
      <header className="flex items-center gap-2 border-b border-border px-4 py-2.5">
        <Zap className="h-4 w-4 text-primary" />
        <h4 className="text-sm font-medium text-foreground">
          {t('sys.ai_models.detail.effective_title', '生效配置（运行时）')}
        </h4>
      </header>
      <div className="space-y-3 px-4 py-3">
        <p className="text-xs text-muted-foreground">
          {t(
            'sys.ai_models.detail.effective_hint',
            '加载时运行时实际收到的后处理参数——按与装载器相同的裁决顺序投影（自定义 blob 优先于档案组合）。'
          )}
        </p>

        {effective.source === 'none-raw' && (
          <div className="rounded-md border bg-muted/40 p-3 text-xs text-muted-foreground">
            {t(
              'sys.ai_models.detail.effective_raw_note',
              'raw 模式：输出为裸张量直传，后处理配置不参与运行时。'
            )}
          </div>
        )}

        {hasEscape && effective.source === 'custom-blob' && (
          <div className="flex items-start gap-2 rounded-md border border-amber-500/60 bg-amber-500/10 p-3 text-xs text-amber-700 dark:text-amber-400">
            <AlertTriangle className="mt-0.5 h-4 w-4 shrink-0" />
            <span>
              {effective.blob
                ? t(
                    'sys.ai_models.detail.effective_escape',
                    '自定义 variant blob 正在覆盖档案组合——上方"后处理参数"中的表单值不生效。'
                  )
                : t(
                    'sys.ai_models.detail.effective_escape_broken',
                    '存储的 variant 不是合法 JSON 对象——装载将被拒绝，请修正或清空。'
                  )}
            </span>
          </div>
        )}

        {effective.source === 'passthrough-name' && (
          <div className="rounded-md border bg-muted/40 p-3 text-xs text-muted-foreground">
            {t('sys.ai_models.detail.effective_passthrough', '裸名 variant：')}
            <code className="font-mono">{variant.trim()}</code>
            {modelType === 'detection'
              ? t(
                  'sys.ai_models.detail.effective_passthrough_detection',
                  ' 作为后端函数路由，其余键由配置合成。'
                )
              : t(
                  'sys.ai_models.detail.effective_passthrough_keypoint',
                  ' 将被档案组合覆盖（裸名不参与姿态解码）。'
                )}
          </div>
        )}

        {effective.source === 'none' && outputMode !== 'raw' && (
          <div className="rounded-md border bg-muted/40 p-3 text-xs text-muted-foreground">
            {t(
              'sys.ai_models.detail.effective_none',
              '该档案无运行时后处理参数——解码器全部行为在编译期固定。'
            )}
          </div>
        )}

        {(effective.source === 'composed'
          || effective.source === 'custom-blob')
          && effective.blob
          && rows.length > 0 && (
            <div className="divide-y divide-border rounded-md border">
              {rows.map(({ f, effect, value }) => (
                <div key={f.key} className="flex items-center gap-3 px-3 py-2">
                  <span className="w-32 shrink-0 truncate text-xs text-muted-foreground">
                    {f.key}
                  </span>
                  <div className="min-w-0 flex-1">
                    <ValueText value={value} />
                  </div>
                  <EffectBadge effect={effect} />
                </div>
              ))}
            </div>
          )}

        {payloadJson && (
          <div>
            <Button
              type="button"
              variant="ghost"
              size="sm"
              className="h-7 gap-1 px-2 text-xs text-muted-foreground"
              onClick={() => setPayloadOpen(prev => !prev)}
            >
              {payloadOpen ? (
                <ChevronDown className="h-3.5 w-3.5" />
              ) : (
                <ChevronRight className="h-3.5 w-3.5" />
              )}
              {t(
                'sys.ai_models.detail.effective_payload',
                '运行时载荷 JSON（逐字）'
              )}
            </Button>
            {payloadOpen && (
              <pre className="mt-1.5 overflow-x-auto rounded-md border bg-muted/40 p-3 font-mono text-xs leading-relaxed text-foreground">
                {payloadJson}
              </pre>
            )}
          </div>
        )}
      </div>
    </section>
  );
}
