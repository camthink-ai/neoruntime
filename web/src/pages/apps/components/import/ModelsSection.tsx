import { useState } from 'react';
import { AlertTriangle, Package, Plus, X } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';
import { Checkbox } from '@/components/ui/checkbox';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import type { WizardConfig, WizardModelMapping } from '@/services/types';
import { isValidModelAlias } from '@/pages/apps/lib/importFlow';

/** Select value that switches a row into custom (free-id) mode. */
const CUSTOM_MODEL_VALUE = '__custom__';

export interface ModelsSectionProps {
  config: WizardConfig;
  onChange: (next: WizardConfig) => void;
  availableModels: Array<{ model_id: string; name?: string }>;
}

/**
 * 模型配置 page of the paginated import form: declarative model
 * dependencies (spec.models) plus inference quotas — QPS / concurrency and
 * dynamic model registration. Dependencies write config.models (alias →
 * mapping); quotas write config.permissions.inference.*. The legacy
 * permissions.inference.models authorization list is server-side only and is
 * deliberately not edited here.
 *
 * Each dependency row mirrors the backend resolveModelDependencies chain:
 * an id the device knows renders compact (system model); an unknown id is a
 * custom declaration that expands to edit path / required — a declared
 * in-image .bin package path makes it a bundled model (installable), a
 * missing path on a required dependency is flagged as install-blocking, on
 * an optional one as a warn-only miss.
 */
export default function ModelsSection({
  config,
  onChange,
  availableModels,
}: ModelsSectionProps) {
  const { t } = useTranslation();
  const models = config.models ?? {};
  const entries = Object.entries(models);
  /**
   * Aliases switched to custom (free-text id) mode via the 自定义… select
   * entry. UI-only state: the mapping itself records path/required. Hydrated
   * unknown ids enter custom mode without needing an entry here.
   */
  const [customDrafts, setCustomDrafts] = useState<string[]>([]);

  const rebuild = (
    alias: string,
    fn: (mapping: WizardModelMapping) => WizardModelMapping,
    aliasTo = alias
  ) => {
    const next: typeof models = {};
    for (const [a, mapping] of entries) {
      next[a === alias ? aliasTo : a] = a === alias ? fn(mapping) : mapping;
    }
    onChange({ ...config, models: next });
  };

  /** New rows land on the reserved '' alias slot: Record keys make it the
   * only draft that can exist at once, and it survives page switches in
   * config state instead of component-local state. */
  const addDependency = () => {
    if ('' in models) return;
    onChange({
      ...config,
      models: { ...models, '': { id: '' } },
    });
  };

  /** Alias edits rename the record key as typed — invalid text stays in
   * state (red border + install blocked) rather than snapping back. Only a
   * collision with an existing alias is refused, to never silently merge. */
  const renameAlias = (alias: string, next: string) => {
    if (next === alias) return;
    if (next in models) return;
    setCustomDrafts(drafts => drafts.map(a => (a === alias ? next : a)));
    rebuild(alias, m => m, next);
  };

  const setMappingId = (alias: string, modelId: string) => {
    // The alias mirrors the picked model id while it is still "auto": the
    // unnamed draft ('') or one equal to the current mapping id (the user
    // hasn't typed a custom alias). Re-picking follows the new model; a
    // hand-typed alias is never clobbered, and a collision with another row
    // keeps the current alias. path/required ride along untouched.
    const isAutoAlias = alias === '' || alias === models[alias]?.id;
    const aliasToUse = isAutoAlias && !(modelId in models) ? modelId : alias;
    if (modelId === CUSTOM_MODEL_VALUE) {
      setCustomDrafts(drafts => [...drafts, alias]);
      return;
    }
    setCustomDrafts(drafts => (alias === '' ? drafts : drafts.filter(a => a !== alias)));
    rebuild(alias, m => ({ ...m, id: modelId }), aliasToUse);
  };

  const patchMapping = (alias: string, patch: Partial<WizardModelMapping>) => rebuild(alias, m => ({ ...m, ...patch }));

  const removeDependency = (alias: string) => {
    setCustomDrafts(drafts => drafts.filter(a => a !== alias));
    const next = Object.fromEntries(entries.filter(([a]) => a !== alias));
    onChange({
      ...config,
      // undefined (not {}) — an emptied dependency map reads as "cleared"
      models: Object.keys(next).length > 0 ? next : undefined,
    });
  };

  const rowState = (alias: string, mapping: WizardModelMapping) => {
    const id = mapping.id?.trim() ?? '';
    const isSystem =      id !== '' && availableModels.some(m => m.model_id === mapping.id);
    // Custom mode is either chosen (自定义… select entry) or derived (an
    // id the device does not know / a declared bundled path).
    const isCustom =      customDrafts.includes(alias)
      || (!isSystem && (id !== '' || mapping.path?.trim()));
    const hasPath = !!mapping.path?.trim();
    return { isSystem, isCustom, hasPath, id };
  };

  return (
    <div className="space-y-6 pr-2 sm:pr-4">
      {/* Model Dependencies (spec.models) */}
      <div>
        <div className="mb-3 flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between">
          <Label className="text-base font-semibold">
            {t('sys.apps.import.ai_models', 'Model Dependencies')}
          </Label>
          <Button
            variant="outline"
            size="sm"
            className="w-full shrink-0 sm:w-auto"
            onClick={addDependency}
          >
            <Plus className="w-4 h-4 mr-1" />
            {t('sys.apps.import.model_dep_add', 'Add Dependency')}
          </Button>
        </div>
        <p className="text-xs text-muted-foreground mb-3">
          {t(
            'sys.apps.import.model_dep_hint',
            'Declare the models the app depends on. Each alias is injected as an AIPC_MODEL_<alias> at runtime.'
          )}
        </p>
        <div className="space-y-2 border rounded-lg p-4">
          {entries.map(([alias, mapping]) => {
            const aliasInvalid = !isValidModelAlias(alias);
            const idMissing = !mapping.id?.trim();
            const { isSystem, isCustom, hasPath } = rowState(alias, mapping);
            const missingRequired = isCustom && !hasPath && mapping.required;
            const missingOptional = isCustom && !hasPath && !mapping.required;
            return (
              <div
                key={alias || '__draft__'}
                className="rounded-md border border-border/60 p-2.5 space-y-2"
              >
                <div className="flex flex-col gap-2 sm:flex-row sm:items-center">
                  <Input
                    placeholder={t('sys.apps.import.model_alias', 'Alias')}
                    value={alias}
                    onChange={e => renameAlias(alias, e.target.value)}
                    aria-invalid={aliasInvalid || undefined}
                    className="flex-1"
                  />
                  {isCustom ? (
                    <Input
                      placeholder={t(
                        'sys.apps.import.model_id_label',
                        'Model ID'
                      )}
                      value={mapping.id}
                      onChange={e => patchMapping(alias, { id: e.target.value })}
                      aria-invalid={idMissing || undefined}
                      className="flex-1 font-mono text-sm"
                    />
                  ) : (
                    <Select
                      value={mapping.id || undefined}
                      onValueChange={v => setMappingId(alias, v)}
                    >
                      <SelectTrigger
                        aria-invalid={idMissing || undefined}
                        className="flex-1"
                      >
                        <SelectValue
                          placeholder={t(
                            'sys.apps.import.model_select',
                            'Select model'
                          )}
                        />
                      </SelectTrigger>
                      <SelectContent>
                        {availableModels.map(model => (
                          <SelectItem
                            key={model.model_id}
                            value={model.model_id}
                          >
                            {model.name || model.model_id}
                          </SelectItem>
                        ))}
                        <SelectItem value={CUSTOM_MODEL_VALUE}>
                          {t(
                            'sys.apps.import.model_custom_add',
                            'Custom model…'
                          )}
                        </SelectItem>
                      </SelectContent>
                    </Select>
                  )}
                  {isSystem && (
                    <span className="shrink-0 rounded-md bg-emerald-500/10 px-2 py-1 text-xs text-emerald-600 dark:text-emerald-400">
                      {t('sys.apps.import.model_badge_system', '系统模型')}
                    </span>
                  )}
                  {isCustom && hasPath && (
                    <span className="shrink-0 inline-flex items-center gap-1 rounded-md bg-amber-500/10 px-2 py-1 text-xs text-amber-600 dark:text-amber-400">
                      <Package className="h-3 w-3" />
                      {t('sys.apps.import.model_badge_bundled', '自带模型')}
                    </span>
                  )}
                  {missingRequired && (
                    <span className="shrink-0 inline-flex items-center gap-1 rounded-md bg-destructive/10 px-2 py-1 text-xs text-destructive">
                      <AlertTriangle className="h-3 w-3" />
                      {t(
                        'sys.apps.import.model_badge_missing_required',
                        '缺失 · 必需'
                      )}
                    </span>
                  )}
                  {missingOptional && (
                    <span className="shrink-0 inline-flex items-center gap-1 rounded-md bg-amber-500/10 px-2 py-1 text-xs text-amber-600 dark:text-amber-400">
                      <AlertTriangle className="h-3 w-3" />
                      {t(
                        'sys.apps.import.model_badge_missing_optional',
                        '未注册 · 可选'
                      )}
                    </span>
                  )}
                  <Button
                    variant="ghost"
                    size="icon"
                    className="shrink-0"
                    onClick={() => removeDependency(alias)}
                  >
                    <X className="w-4 h-4" />
                  </Button>
                </div>

                {isCustom && (
                  <div className="grid gap-2 rounded-md bg-muted/40 p-2.5 sm:grid-cols-2">
                    <div>
                      <Label className="text-xs text-muted-foreground">
                        {t('sys.apps.import.model_path', '模型包路径 (path)')}
                      </Label>
                      <Input
                        value={mapping.path ?? ''}
                        onChange={e => patchMapping(alias, {
                            path: e.target.value || undefined,
                          })}
                        placeholder="/opt/models/yolo.bin"
                        className="mt-1 font-mono text-sm"
                      />
                    </div>
                    <div className="flex items-end pb-1">
                      <div className="flex items-center space-x-2">
                        <Checkbox
                          id={`model-required-${alias || 'draft'}`}
                          checked={!!mapping.required}
                          onCheckedChange={checked => patchMapping(alias, {
                              required: !!checked || undefined,
                            })}
                        />
                        <Label
                          htmlFor={`model-required-${alias || 'draft'}`}
                          className="text-xs font-normal"
                        >
                          {t(
                            'sys.apps.import.model_required_row',
                            '必需（缺失时安装失败）'
                          )}
                        </Label>
                      </div>
                    </div>
                    <p className="text-xs text-muted-foreground sm:col-span-2">
                      {t(
                        'sys.apps.import.model_custom_hint',
                        '声明镜像内自带、未注册到系统的模型：id 不在系统列表时，需提供镜像内 AMPK 模型包（.bin）路径才能通过安装校验。'
                      )}
                    </p>
                  </div>
                )}
              </div>
            );
          })}
          {entries.length === 0 && (
            <p className="text-sm text-muted-foreground py-2">
              {t(
                'sys.apps.import.no_model_deps',
                'No model dependencies declared'
              )}
            </p>
          )}
        </div>
      </div>

      {/* Max QPS */}
      <div>
        <Label>{t('sys.apps.import.max_qps', 'Max Inference QPS')}</Label>
        <Input
          type="number"
          min={1}
          max={1000}
          value={config.permissions?.inference?.max_qps ?? ''}
          onChange={e => {
            // Empty clears the override (backend default applies); typed
            // values clamp to 1–1000 without snapping back to a default.
            const n = parseInt(e.target.value, 10);
            onChange({
              ...config,
              permissions: {
                ...config.permissions!,
                inference: {
                  ...config.permissions!.inference!,
                  max_qps: Number.isNaN(n)
                    ? undefined
                    : Math.min(1000, Math.max(1, n)),
                },
              },
            });
          }}
          className="mt-2 w-full max-w-48 sm:w-32"
        />
      </div>

      {/* Max Concurrent */}
      <div>
        <Label>
          {t('sys.apps.import.max_concurrent', 'Max Concurrent Inference')}
        </Label>
        <Input
          type="number"
          min={0}
          max={100}
          placeholder="0"
          value={config.permissions?.inference?.max_concurrent || ''}
          onChange={e => onChange({
              ...config,
              permissions: {
                ...config.permissions!,
                inference: {
                  ...config.permissions!.inference!,
                  max_concurrent: parseInt(e.target.value, 10) || 0,
                },
              },
            })}
          className="mt-2 w-full max-w-48 sm:w-32"
        />
      </div>

      {/* Allow Register Model */}
      <div>
        <div className="flex items-center space-x-2">
          <Checkbox
            checked={config.permissions?.inference?.allow_register_model}
            onCheckedChange={checked => onChange({
                ...config,
                permissions: {
                  ...config.permissions!,
                  inference: {
                    ...config.permissions!.inference!,
                    allow_register_model: !!checked,
                  },
                },
              })}
          />
          <Label className="font-normal">
            {t(
              'sys.apps.import.allow_register_model',
              'Allow Dynamic Model Registration'
            )}
          </Label>
        </div>
        <p className="text-xs text-muted-foreground mt-1">
          {t(
            'sys.apps.import.allow_register_model_hint',
            'Allow app to discover and register models at runtime'
          )}
        </p>
      </div>
    </div>
  );
}
