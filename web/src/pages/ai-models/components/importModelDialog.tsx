import { useMemo, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { Check } from 'lucide-react';
import { useNavigate } from 'react-router-dom';
import {
  useCapabilities,
  useParseModel,
  useRegisterModelV2,
  type ModelTypeDef,
  type ModelFieldDef,
} from '@/hooks/useModels';
import { useModels } from '@/hooks/useModels';
import { useToast } from '@/hooks/use-toast';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { Badge } from '@/components/ui/badge';
import FileUpload from '@/components/file-upload';
import {
  Dialog,
  DialogContent,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog';
import { filesApi } from '@/services/api';

interface ImportModelDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  onSuccess?: () => unknown;
}

interface ParseResult {
  file_hash: string;
  file_path: string;
  file_size: number;
  filename: string;
  network_name: string;
  vstream_info: string;
  suggested_type: string;
  format: string;
  input_width?: number;
  input_height?: number;
}

interface FormState {
  modelId: string;
  modelType: string;
  variant: string;
  config: Record<string, unknown>;
}

const initialFormState: FormState = {
  modelId: '',
  modelType: '',
  variant: '',
  config: {},
};

function formatFileSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function sanitizeModelId(name: string): string {
  return name
    .toLowerCase()
    .replace(/[^a-z0-9_-]/g, '_')
    .replace(/_+/g, '_')
    .replace(/^_|_$/g, '');
}

function fieldDefaultToState(fields: ModelFieldDef[]): Record<string, unknown> {
  const config: Record<string, unknown> = {};
  for (const f of fields) {
    if (f.default !== undefined) {
      config[f.key] = f.default;
    }
  }
  return config;
}

export default function ImportModelDialog({
  open,
  onOpenChange,
  onSuccess,
}: ImportModelDialogProps) {
  const { t } = useTranslation();
  const { toast } = useToast();
  const navigate = useNavigate();

  const cancelRequestedRef = useRef(false);

  const [step, setStep] = useState(0);
  const [file, setFile] = useState<File | null>(null);
  const [parseResult, setParseResult] = useState<ParseResult | null>(null);
  const [form, setForm] = useState<FormState>(initialFormState);
  const [errors, setErrors] = useState<Record<string, string>>({});
  const [touched, setTouched] = useState<Record<string, boolean>>({});

  const { data: capabilities } = useCapabilities();
  const { data: existingModels = [], isSuccess: modelsReady } = useModels();
  const parseMutation = useParseModel();
  const registerMutation = useRegisterModelV2();

  // 每次重新打开对话框时，重置取消标记，避免新一轮上传被当作“已取消”而丢弃 parseResult
  if (open && cancelRequestedRef.current) {
    cancelRequestedRef.current = false;
  }

  const cleanupUploadedFiles = async (
    paths: Array<string | undefined | null>
  ) => {
    const uniq = Array.from(new Set(paths.filter(Boolean))) as string[];
    if (uniq.length === 0) return;
    try {
      await filesApi.batchDelete(uniq);
    } catch {
      // best-effort cleanup
    }
  };

  const modelTypeOptions = useMemo(() => {
    if (!capabilities?.model_types) return [];
    return capabilities.model_types.map((mt: ModelTypeDef) => ({
      value: mt.id,
      label: t(`sys.ai_models.model_type.${mt.id}`, mt.label),
      fields: mt.fields,
    }));
  }, [capabilities, t]);

  const acceptFormats = useMemo(() => {
    if (!capabilities?.formats) return { 'application/octet-stream': ['.hef'] };
    const map: Record<string, string[]> = {};
    for (const f of capabilities.formats) {
      if (!map[f.mime_type]) map[f.mime_type] = [];
      map[f.mime_type].push(f.extension);
    }
    return map;
  }, [capabilities]);

  const formatHint = useMemo(() => {
    if (!capabilities?.formats) return t('sys.ai_models.form.file_hint', 'Only .hef format is supported');
    return capabilities.formats.map(f => f.extension).join(', ');
  }, [capabilities, t]);

  const existingModelIdSet = useMemo(
    () => new Set(
        (existingModels || [])
          .map((m: any) => (typeof m?.model_id === 'string'
              ? m.model_id.trim().toLowerCase()
              : ''))
          .filter(Boolean)
      ),
    [existingModels]
  );

  const isDuplicateModelId = (raw: string) => {
    if (!modelsReady) return false;
    const normalized = raw.trim().toLowerCase();
    return normalized !== '' && existingModelIdSet.has(normalized);
  };

  // Currently selected type's fields
  const currentFields = useMemo(() => {
    const opt = modelTypeOptions.find(o => o.value === form.modelType);
    return opt?.fields ?? [];
  }, [modelTypeOptions, form.modelType]);

  const steps = useMemo(
    () => [
      { title: t('sys.ai_models.wizard.step_upload', 'Upload') },
      { title: t('sys.ai_models.wizard.step_confirm', 'Configure') },
    ],
    [t]
  );

  // Handlers
  const handleFileChange = (files: File[]) => {
    const next = files[0] || null;
    setFile(next);
    setParseResult(null);

    if (!next) return;

    const formData = new FormData();
    formData.append('model', next);

    parseMutation.mutate(formData, {
      onSuccess: (data: any) => {
        const result = data as ParseResult;
        if (cancelRequestedRef.current) {
          cleanupUploadedFiles([result?.file_path]);
          return;
        }
        setParseResult(result);

        const suggestedType = result.suggested_type || '';
        const typeOpt = modelTypeOptions.find(o => o.value === suggestedType);
        const configDefaults = typeOpt
          ? fieldDefaultToState(typeOpt.fields)
          : {};

        setForm({
          modelId: sanitizeModelId(
            result.network_name
              || result.filename?.replace(/\.[^.]+$/, '')
              || ''
          ),
          modelType: suggestedType,
          variant: '',
          config: configDefaults,
        });
      },
      onError: (error: any) => {
        toast({
          title: t(
            'sys.ai_models.wizard.parse_failed',
            'Failed to parse model'
          ),
          description: error?.response?.data?.message || error?.message,
          variant: 'destructive',
        });
      },
    });
  };

  const handleNext = () => {
    setTouched({});
    setErrors({});
    setStep(1);
  };

  const validate = (): boolean => {
    const newErrors: Record<string, string> = {};
    if (!form.modelId.trim()) newErrors.modelId = t('sys.ai_models.form.required');
    if (!newErrors.modelId && isDuplicateModelId(form.modelId)) {
      newErrors.modelId = t(
        'sys.ai_models.form.model_id_exists',
        'Model ID already exists'
      );
    }
    if (!form.modelType) newErrors.modelType = t('sys.ai_models.form.required');

    // Validate dynamic fields
    for (const f of currentFields) {
      const value = form.config[f.key];
      if (f.required && (value === undefined || value === '')) {
        newErrors[`config_${f.key}`] = t('sys.ai_models.form.required');
        continue;
      }

      if (f.type === 'number' && value !== undefined && value !== '') {
        const n = typeof value === 'number' ? value : Number(value);
        if (!Number.isFinite(n)) {
          newErrors[`config_${f.key}`] = t(
            'sys.ai_models.form.invalid_number',
            'Please enter a valid number'
          );
          continue;
        }

        // Special validation for common detection thresholds
        if (f.key === 'threshold' && (n < 0 || n > 1)) {
          newErrors[`config_${f.key}`] = t(
            'sys.ai_models.form.threshold_range',
            'Threshold must be between 0 and 1'
          );
          continue;
        }
        if (f.key === 'nms_threshold' && (n < 0 || n > 1)) {
          newErrors[`config_${f.key}`] = t(
            'sys.ai_models.form.nms_threshold_range',
            'NMS threshold must be between 0 and 1'
          );
          continue;
        }

        // Generic min/max validation if provided by capability schema
        if (typeof f.min === 'number' && n < f.min) {
          newErrors[`config_${f.key}`] = t(
            'sys.ai_models.form.number_min',
            'Value must be ≥ {{min}}',
            { min: f.min }
          );
          continue;
        }
        if (typeof f.max === 'number' && n > f.max) {
          newErrors[`config_${f.key}`] = t(
            'sys.ai_models.form.number_max',
            'Value must be ≤ {{max}}',
            { max: f.max }
          );
        }
      }
    }

    setErrors(newErrors);
    return Object.keys(newErrors).length === 0;
  };

  const handleModelTypeChange = (value: string) => {
    const typeOpt = modelTypeOptions.find(o => o.value === value);
    const configDefaults = typeOpt ? fieldDefaultToState(typeOpt.fields) : {};
    setForm(prev => ({
      ...prev,
      modelType: value,
      config: configDefaults,
    }));
    if (touched.modelType) {
      setErrors(prev => {
        const next = { ...prev };
        delete next.modelType;
        return next;
      });
    }
  };

  const updateConfig = (key: string, value: unknown) => {
    setForm(prev => ({
      ...prev,
      config: { ...prev.config, [key]: value },
    }));
    const errKey = `config_${key}`;
    if (touched[errKey]) {
      setErrors(prev => {
        const next = { ...prev };
        delete next[errKey];
        return next;
      });
    }
  };

  const handleRegister = () => {
    const newTouched: Record<string, boolean> = {
      modelId: true,
      modelType: true,
    };
    for (const f of currentFields) {
      newTouched[`config_${f.key}`] = true;
    }
    setTouched(newTouched);

    if (!validate() || !parseResult) return;

    registerMutation.mutate(
      {
        file_hash: parseResult.file_hash,
        model_id: form.modelId.trim(),
        model_type: form.modelType,
        model_variant: form.variant.trim(),
        config: form.config,
        file_size: parseResult.file_size,
        network_name: parseResult.network_name,
        vstream_info: parseResult.vstream_info,
        input_width: parseResult.input_width ?? 0,
        input_height: parseResult.input_height ?? 0,
      },
      {
        onSuccess: async () => {
          toast({
            title: t(
              'sys.ai_models.message.import_success',
              'Model imported successfully'
            ),
          });
          handleReset();
          onOpenChange(false);
          await onSuccess?.();
        },
        onError: (error: any) => {
          toast({
            title: t('common.error', 'Error'),
            description: error?.response?.data?.message || error?.message,
            variant: 'destructive',
          });
        },
      }
    );
  };

  const handleReset = () => {
    setStep(0);
    setFile(null);
    setParseResult(null);
    setForm(initialFormState);
    setErrors({});
    setTouched({});
  };

  const handleCancel = () => {
    cancelRequestedRef.current = true
    cleanupUploadedFiles([parseResult?.file_path])
    handleReset()
    onOpenChange(false)
    navigate('/models')
  }

  const handleOpenChange = (nextOpen: boolean) => {
    if (nextOpen) {
      cancelRequestedRef.current = false;
      onOpenChange(true);
      return;
    }
    handleCancel();
  };

  const ph = t('sys.ai_models.form.placeholder', 'Please enter');
  const isLoading = parseMutation.isPending || registerMutation.isPending;

  // Render a dynamic field based on its schema definition
  const renderField = (field: ModelFieldDef) => {
    const errKey = `config_${field.key}`;
    const hasError = touched[errKey] && errors[errKey];

    switch (field.type) {
      case 'number': {
        const val =          form.config[field.key] !== undefined
            ? String(form.config[field.key])
            : '';
        const effectiveMin =          field.key === 'threshold' || field.key === 'nms_threshold'
            ? 0
            : field.min;
        const effectiveMax =          field.key === 'threshold' || field.key === 'nms_threshold'
            ? 1
            : field.max;
        const effectiveStep =          field.key === 'threshold' || field.key === 'nms_threshold'
            ? (field.step ?? 0.01)
            : (field.step ?? 1);
        return (
          <div className="grid gap-2" key={field.key}>
            <Label htmlFor={field.key}>
              {t(`sys.ai_models.form.${field.key}`, field.key)}
            </Label>
            <Input
              id={field.key}
              type="number"
              step={effectiveStep}
              min={effectiveMin}
              max={effectiveMax}
              value={val}
              onChange={e => {
                const v =                  e.target.value === ''
                    ? undefined
                    : parseFloat(e.target.value);
                updateConfig(field.key, v);
              }}
              onBlur={() => {
                setTouched(prev => ({ ...prev, [errKey]: true }));
                const raw = form.config[field.key];
                if (raw === undefined || raw === '') return;
                const n = typeof raw === 'number' ? raw : Number(raw);
                if (!Number.isFinite(n)) {
                  setErrors(prev => ({
                    ...prev,
                    [errKey]: t(
                      'sys.ai_models.form.invalid_number',
                      'Please enter a valid number'
                    ),
                  }));
                  return;
                }
                if (field.key === 'threshold' && (n < 0 || n > 1)) {
                  setErrors(prev => ({
                    ...prev,
                    [errKey]: t(
                      'sys.ai_models.form.threshold_range',
                      'Threshold must be between 0 and 1'
                    ),
                  }));
                  return;
                }
                if (field.key === 'nms_threshold' && (n < 0 || n > 1)) {
                  setErrors(prev => ({
                    ...prev,
                    [errKey]: t(
                      'sys.ai_models.form.nms_threshold_range',
                      'NMS threshold must be between 0 and 1'
                    ),
                  }));
                  return;
                }
                if (typeof field.min === 'number' && n < field.min) {
                  setErrors(prev => ({
                    ...prev,
                    [errKey]: t(
                      'sys.ai_models.form.number_min',
                      'Value must be ≥ {{min}}',
                      { min: field.min }
                    ),
                  }));
                  return;
                }
                if (typeof field.max === 'number' && n > field.max) {
                  setErrors(prev => ({
                    ...prev,
                    [errKey]: t(
                      'sys.ai_models.form.number_max',
                      'Value must be ≤ {{max}}',
                      { max: field.max }
                    ),
                  }));
                }
              }}
              placeholder={ph}
              disabled={isLoading}
            />
            {hasError && (
              <p className="text-sm text-destructive">{errors[errKey]}</p>
            )}
          </div>
        );
      }
      case 'select': {
        const val = String(form.config[field.key] ?? '');
        return (
          <div className="grid gap-2" key={field.key}>
            <Label htmlFor={field.key}>
              {t(`sys.ai_models.form.${field.key}`, field.key)}
            </Label>
            <Select
              value={val}
              onValueChange={v => updateConfig(field.key, v)}
              disabled={isLoading}
            >
              <SelectTrigger id={field.key}>
                <SelectValue
                  placeholder={t(
                    'sys.ai_models.form.select_type',
                    'Please select'
                  )}
                />
              </SelectTrigger>
              <SelectContent>
                {(field.options ?? []).map(opt => (
                  <SelectItem key={opt.value} value={opt.value}>
                    {t(
                      `sys.ai_models.form.${field.key}_${opt.value}`,
                      opt.label
                    )}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </div>
        );
      }
      case 'boolean': {
        const checked = Boolean(form.config[field.key]);
        return (
          <div className="flex items-center gap-2" key={field.key}>
            <input
              type="checkbox"
              id={field.key}
              checked={checked}
              onChange={e => updateConfig(field.key, e.target.checked)}
              disabled={isLoading}
              className="h-4 w-4"
            />
            <Label htmlFor={field.key} className="font-normal">
              {t(`sys.ai_models.form.${field.key}`, field.key)}
            </Label>
          </div>
        );
      }
      default:
        return null;
    }
  };

  return (
    <Dialog open={open} onOpenChange={handleOpenChange}>
      <DialogContent
        onInteractOutside={e => e.preventDefault()}
        className="sm:max-w-xl"
      >
        <DialogHeader>
          <DialogTitle>
            {step === 0
              ? t('sys.ai_models.action.import', 'Import Model')
              : t('sys.ai_models.wizard.step_confirm', 'Configure Model')}
          </DialogTitle>
        </DialogHeader>

        {/* Step indicator — scroll on narrow screens */}
        <div className="-mx-1 mb-4 mt-2 overflow-x-auto pb-1 sm:mx-0 sm:overflow-visible sm:pb-0">
          <div className="flex min-w-max items-center justify-center px-1 sm:min-w-0">
            {steps.map((s, i) => (
              <div key={i} className="flex items-center">
                <div className="flex flex-col items-center">
                  <div
                    className={`mb-1 flex h-7 w-7 shrink-0 items-center justify-center rounded-full border-2 text-xs transition-all sm:h-8 sm:w-8 sm:text-sm ${
                      i === step
                        ? 'border-primary bg-primary text-primary-foreground'
                        : i < step
                          ? 'border-green-500 bg-green-500 text-white'
                          : 'border-border bg-background text-muted-foreground'
                    }`}
                  >
                    {i < step ? (
                      <Check className="h-4 w-4 sm:h-5 sm:w-5" />
                    ) : (
                      i + 1
                    )}
                  </div>
                  <span
                    className={`max-w-17 truncate px-0.5 text-center text-[10px] sm:max-w-24 sm:text-xs ${i === step ? 'font-medium text-primary' : 'text-muted-foreground'}`}
                  >
                    {s.title}
                  </span>
                </div>
                {i < steps.length - 1 && (
                  <div
                    className={`-mt-4 mx-0.5 h-px w-10 shrink-0 sm:-mt-5 sm:mx-1 sm:w-14 ${i < step ? 'bg-green-500' : 'bg-border'}`}
                  />
                )}
              </div>
            ))}
          </div>
        </div>

        {step === 0 ? (
          /* Step 0: Upload + Parse */
          <div className="grid gap-4 py-2">
            <FileUpload
              single
              value={file ? [file] : []}
              onChange={handleFileChange}
              loading={parseMutation.isPending}
              disabled={isLoading}
              showFileList
              accept={acceptFormats}
              placeholder={t(
                'sys.ai_models.form.file_placeholder',
                'Drag and drop model file here'
              )}
              hint={formatHint}
            />

            {parseMutation.isPending && (
              <p className="text-sm text-muted-foreground animate-pulse">
                {t('sys.ai_models.wizard.parsing', 'Parsing model...')}
              </p>
            )}

            {parseResult && (
              <div className="rounded-lg border bg-muted/30 p-3 space-y-2">
                <div className="space-y-2 text-sm">
                  <div className="flex items-center justify-between gap-4">
                    <Label className="shrink-0 text-muted-foreground">
                      {t('sys.ai_models.wizard.preview_title', 'Model Preview')}
                    </Label>
                    <Badge variant="secondary">
                      {t('sys.ai_models.wizard.suggested_type', 'Suggested')}:{' '}
                      {t(
                        `sys.ai_models.model_type.${parseResult.suggested_type}`,
                        parseResult.suggested_type
                      )}
                    </Badge>
                  </div>
                  <div className="flex items-center justify-between gap-4">
                    <Label className="shrink-0 text-muted-foreground">
                      {t('sys.ai_models.wizard.preview_network', 'Network')}
                    </Label>
                    <div className="min-w-0 flex-1 truncate text-right">
                      {parseResult.network_name || '—'}
                    </div>
                  </div>

                  <div className="flex items-center justify-between gap-4">
                    <Label className="shrink-0 text-muted-foreground">
                      {t('sys.ai_models.wizard.preview_file', 'File')}
                    </Label>
                    <div className="min-w-0 flex-1 truncate text-right">
                      {parseResult.filename} (
                      {formatFileSize(parseResult.file_size)})
                    </div>
                  </div>

                  <div className="flex items-center justify-between gap-4">
                    <Label className="shrink-0 text-muted-foreground">
                      {t('sys.ai_models.wizard.preview_input', 'Input')}
                    </Label>
                    <div className="min-w-0 flex-1 text-right">
                      {parseResult.input_width && parseResult.input_height
                        ? `${parseResult.input_width}x${parseResult.input_height}`
                        : '—'}
                    </div>
                  </div>
                </div>
              </div>
            )}
          </div>
        ) : (
          /* Step 1: Configure + Register */
          <div className="grid gap-4 py-2">
            {/* Model ID */}
            <div className="grid gap-2">
              <Label htmlFor="model-id">
                {t('sys.ai_models.form.model_id', 'Model ID')} *
              </Label>
              <Input
                id="model-id"
                value={form.modelId}
                onChange={e => {
                  setForm(prev => ({ ...prev, modelId: e.target.value }));
                  if (touched.modelId) {
                    setErrors(prev => {
                      const next = { ...prev };
                      delete next.modelId;
                      return next;
                    });
                  }
                }}
                onBlur={() => {
                  setTouched(prev => ({ ...prev, modelId: true }));
                  if (!form.modelId.trim()) {
                    setErrors(prev => ({
                      ...prev,
                      modelId: t('sys.ai_models.form.required'),
                    }));
                  } else if (isDuplicateModelId(form.modelId)) {
                    setErrors(prev => ({
                      ...prev,
                      modelId: t(
                        'sys.ai_models.form.model_id_exists',
                        'Model ID already exists'
                      ),
                    }));
                  }
                }}
                placeholder={ph}
                disabled={isLoading}
              />
              {touched.modelId && errors.modelId && (
                <p className="text-sm text-destructive">{errors.modelId}</p>
              )}
            </div>

            {/* Model Type */}
            <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
              <div className="grid gap-2">
                <Label htmlFor="model-type">
                  {t('sys.ai_models.form.model_type', 'Model Type')} *
                </Label>
                <Select
                  value={form.modelType}
                  onValueChange={handleModelTypeChange}
                  disabled={isLoading}
                >
                  <SelectTrigger
                    id="model-type"
                    className={
                      touched.modelType && errors.modelType
                        ? 'border-destructive'
                        : ''
                    }
                  >
                    <SelectValue
                      placeholder={t(
                        'sys.ai_models.form.select_type',
                        'Please select'
                      )}
                    />
                  </SelectTrigger>
                  <SelectContent>
                    {modelTypeOptions.map(option => (
                      <SelectItem key={option.value} value={option.value}>
                        {option.label}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
                {touched.modelType && errors.modelType && (
                  <p className="text-sm text-destructive">{errors.modelType}</p>
                )}
              </div>

              {/* Variant */}
              <div className="grid gap-2">
                <Label htmlFor="variant">
                  {t('sys.ai_models.form.variant', 'Variant')}
                </Label>
                <Input
                  id="variant"
                  value={form.variant}
                  onChange={e => setForm(prev => ({ ...prev, variant: e.target.value }))}
                  placeholder={ph}
                  disabled={isLoading}
                />
              </div>
            </div>

            {/* Dynamic fields from schema */}
            {currentFields.length > 0 && (
              <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
                {currentFields.map(field => renderField(field))}
              </div>
            )}
          </div>
        )}

        <DialogFooter>
          {step === 0 ? (
            <>
              <Button
                variant="outline"
                onClick={handleCancel}
                disabled={isLoading}
              >
                {t('common.cancel', 'Cancel')}
              </Button>
              <Button
                variant="carbon"
                onClick={handleNext}
                disabled={!parseResult || isLoading}
              >
                {t('common.next', 'Next')}
              </Button>
            </>
          ) : (
            <>
              <Button
                variant="outline"
                onClick={() => setStep(0)}
                disabled={isLoading}
              >
                {t('common.back', 'Back')}
              </Button>
              <Button
                variant="carbon"
                onClick={handleRegister}
                disabled={isLoading}
              >
                {registerMutation.isPending
                  ? t('common.loading', 'Loading...')
                  : t('sys.ai_models.wizard.confirm_register', 'Register')}
              </Button>
            </>
          )}
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
