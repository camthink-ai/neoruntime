import { useMemo, useRef, useState, useEffect } from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogTitle,
} from '@/components/ui/dialog';
import { toast } from 'sonner';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { Checkbox } from '@/components/ui/checkbox';
import { ScrollArea } from '@/components/ui/scroll-area';
import { Progress } from '@/components/ui/progress';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import {
  Globe,
  UploadCloud,
  CheckCircle2,
  Check,
  ArrowRight,
  ArrowLeft,
  Package,
  Settings,
  Shield,
  Eye,
  Wrench,
  X,
  Plus,
  Loader2,
  AlertCircle,
  FileText,
} from 'lucide-react';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { aiApi, streamsApi, appsApi, filesApi } from '@/services/api';
import { useWizardInstall, useInstallProgress } from '@/hooks';
import FileUpload from '@/components/file-upload';
import ImageUpload from './ImageUpload';
import type { WizardConfig } from '@/services/types';
import {
  translateInstallError,
  resolveInstallApiError,
} from '../lib/installErrorMessage';
import {
  translateInstallProgress,
  translateInstallPhase,
} from '../lib/installProgressMessage';

export interface ImportAppDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
}

const MEMORY_OPTIONS = ['128Mi', '256Mi', '512Mi', '1Gi', '2Gi'];

/** backing 仍为 `N%`，输入框仅展示数字 0–100 */
function cpuPercentToInputValue(cpu: string | undefined): string {
  if (!cpu?.trim()) return '';
  const t = cpu.trim();
  const m = t.match(/^(\d{1,3})\s*%$/);
  if (m) {
    const n = Math.min(100, Math.max(0, parseInt(m[1], 10)));
    return String(n);
  }
  const plain = t.match(/^(\d{1,3})$/);
  if (plain) {
    const n = Math.min(100, Math.max(0, parseInt(plain[1], 10)));
    return String(n);
  }
  return '';
}

function inputDigitsToCpuPercent(raw: string): string {
  const digits = raw.replace(/\D/g, '');
  if (digits === '') return '0%';
  let n = parseInt(digits, 10);
  if (Number.isNaN(n) || n < 0) n = 0;
  if (n > 100) n = 100;
  return `${n}%`;
}

/**
 * 容器镜像地址（Docker/OCI 风格）的表单校验：registry/仓库名[:tag] 或 @sha256:… digest
 */
function isValidContainerImageRef(ref: string): boolean {
  const s = ref.trim();
  if (s.length < 1 || s.length > 1024) return false;
  if (/\s/.test(s) || s.includes('://')) return false;
  if (s.startsWith('/') || s.endsWith('/') || s.includes('..')) return false;

  let remainder = s;
  if (remainder.includes('@')) {
    const at = remainder.lastIndexOf('@');
    const name = remainder.slice(0, at);
    const digest = remainder.slice(at + 1);
    if (!name || !/^sha256:[a-f0-9]{64}$/i.test(digest)) return false;
    remainder = name;
  }

  const parts = remainder.split('/');
  if (parts.some(p => !p)) return false;

  const segment = /^[a-zA-Z0-9][a-zA-Z0-9._-]*$/;
  const lastSegment = /^[a-zA-Z0-9][a-zA-Z0-9._-]*(?::[a-zA-Z0-9._-]{1,128})?$/;

  const isHostPort = (p: string): boolean => {
    const m = p.match(/^(.+):(\d{1,5})$/);
    if (!m) return false;
    const port = Number(m[2]);
    if (!Number.isFinite(port) || port < 1 || port > 65535) return false;
    const host = m[1];
    return (
      /^[a-zA-Z0-9]([a-zA-Z0-9.-]*[a-zA-Z0-9])?$/.test(host)
      || /^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(host)
    );
  };

  for (let i = 0; i < parts.length; i++) {
    const p = parts[i];
    const isLast = i === parts.length - 1;
    if (isLast) {
      if (!lastSegment.test(p)) return false;
    } else if (i === 0 && isHostPort(p)) {
      continue;
    } else if (!segment.test(p)) return false;
  }

  return true;
}

const defaultConfig: WizardConfig = {
  metadata: { id: '', name: '', version: '1.0.0', description: '' },
  image: '',
  image_path: '',
  resources: { cpu: '50%', memory: '256Mi' },
  permissions: {
    video: [],
    inference: {
      models: [],
      max_qps: 10,
      max_concurrent: 0,
      allow_register_model: false,
    },
    events: { publish: [], subscribe: [] },
    device: { light: false, ir_cut: false, ptz: false, lens: false },
    network: { mode: 'isolated' },
  },
  env: [],
  volumes: [],
  autostart: false,
  restart_policy: 'on-failure',
};

export default function ImportAppDialog({
  open,
  onOpenChange,
}: ImportAppDialogProps) {
  const { t } = useTranslation();
  const queryClient = useQueryClient();
  const navigate = useNavigate();
  const [step, setStep] = useState(0);
  const [sourceType, setSourceType] = useState<
    'registry' | 'upload' | 'package'
  >('registry');
  const [config, setConfig] = useState<WizardConfig>({ ...defaultConfig });
  const [isUploadingImage, setIsUploadingImage] = useState(false);
  const [taskId, setTaskId] = useState<string | null>(null);
  const [imageAddressError, setImageAddressError] = useState<string | null>(
    null
  );

  // Package mode state
  const [manifestPath, setManifestPath] = useState('');
  const [manifestMeta, setManifestMeta] = useState<{
    id: string;
    name: string;
    version: string;
    description: string;
  } | null>(null);
  const [packageImagePath, setPackageImagePath] = useState('');
  const [packageImageName, setPackageImageName] = useState('');
  const [isUploadingManifest, setIsUploadingManifest] = useState(false);

  const cancelRequestedRef = useRef(false);
  const uploadedImageRef = useRef({ path: '', name: '', size: 0 });

  const isStep0SourceReady =    sourceType === 'registry'
      ? !!config.image.trim() && isValidContainerImageRef(config.image)
      : sourceType === 'package'
        ? !!manifestPath
        : !!(config.image_path || uploadedImageRef.current.path);

  const steps = [
    {
      title: t('sys.apps.import.source'),
      icon: Package,
      validate: () => isStep0SourceReady,
    },
    {
      title: t('sys.apps.import.basic_info'),
      icon: Settings,
      validate: () => !!config.metadata.id.trim() && !!config.metadata.name.trim(),
    },
    {
      title: t('sys.apps.import.resources'),
      icon: Settings,
      validate: () => true,
    },
    {
      title: t('sys.apps.import.permissions'),
      icon: Shield,
      validate: () => true,
    },
    {
      title: t('sys.apps.import.advanced', 'Advanced Config'),
      icon: Wrench,
      validate: () => true,
    },
    { title: t('sys.apps.import.review'), icon: Eye, validate: () => true },
  ];

  const installMutation = useWizardInstall();
  const { data: progress } = useInstallProgress(taskId);

  const cleanupPaths = useMemo(() => {
    const paths = [manifestPath, packageImagePath, config.image_path].filter(
      Boolean
    ) as string[];
    return Array.from(new Set(paths));
  }, [manifestPath, packageImagePath, config.image_path]);

  const resetWizardState = () => {
    setStep(0);
    setSourceType('registry');
    setConfig({ ...defaultConfig });
    setIsUploadingImage(false);
    setTaskId(null);
    setImageAddressError(null);
    uploadedImageRef.current = { path: '', name: '', size: 0 };

    setManifestPath('');
    setManifestMeta(null);
    setPackageImagePath('');
    setPackageImageName('');
    setIsUploadingManifest(false);

    installMutation.reset();
  };

  const cleanupUploadedFiles = async (paths: string[]) => {
    const uniq = Array.from(new Set(paths.filter(Boolean)));
    if (uniq.length === 0) return;
    try {
      await filesApi.batchDelete(uniq);
    } catch {
      // best-effort cleanup
    }
  };

  const handleCancel = () => {
    cancelRequestedRef.current = true;

    // Stop async polling immediately (best effort; backend task may still run)
    setTaskId(null);

    // Cleanup uploaded artifacts (best effort)
    cleanupUploadedFiles(cleanupPaths);

    // Reset local wizard state and close
    resetWizardState();
    onOpenChange(false);

    // Return to apps main page
    navigate('/apps');
  };

  // Fetch existing apps for duplicate check
  const { data: existingAppsData } = useQuery({
    queryKey: ['apps'],
    queryFn: () => appsApi.list().then(res => res.data || {}),
    enabled: open,
  });
  const existingAppIds: Set<string> = new Set(
    (existingAppsData?.apps || []).map((a: any) => a.id || a.app_id)
  );

  // Fetch available models
  const { data: modelsData } = useQuery({
    queryKey: ['models'],
    queryFn: () => aiApi.list().then(res => res.data || {}),
    enabled: open,
  });
  const availableModels: Array<{ model_id: string; name?: string }> =    modelsData?.models || [];

  // Fetch available video streams
  const { data: streamsData } = useQuery({
    queryKey: ['streams'],
    queryFn: () => streamsApi.list().then(res => res.data || {}),
    enabled: open,
    retry: false,
    initialData: { streams: [] },
  });
  const availableStreams: Array<{
    stream_id: string;
    width?: number;
    height?: number;
    fps?: number;
    status?: string;
  }> = streamsData?.streams || [];

  // Reset on close
  useEffect(() => {
    if (open) {
      cancelRequestedRef.current = false;
      return;
    }

    setStep(0);
    setSourceType('registry');
    setConfig({ ...defaultConfig });
    setTaskId(null);
    setImageAddressError(null);
    uploadedImageRef.current = { path: '', name: '', size: 0 };
    setManifestPath('');
    setManifestMeta(null);
    setPackageImagePath('');
    setPackageImageName('');
    setIsUploadingManifest(false);
    installMutation.reset();
  }, [open]);

  const toggleModel = (modelId: string) => {
    const current = config.permissions?.inference?.models || [];
    const updated = current.includes(modelId)
      ? current.filter(m => m !== modelId)
      : [...current, modelId];
    setConfig({
      ...config,
      permissions: {
        ...config.permissions!,
        inference: { ...config.permissions!.inference!, models: updated },
      },
    });
  };

  const handleInstall = () => {
    if (sourceType === 'package') {
      // Package mode: use install-package endpoint
      appsApi
        .installPackage({
          manifest_path: manifestPath,
          image_path: packageImagePath || undefined,
        })
        .then((res: any) => {
          const tid = res?.data?.task_id;
          if (tid) {
            setTaskId(tid);
            setStep(steps.length);
          } else {
            toast.success(t('sys.apps.toast.installSuccess', 'App installed'));
            queryClient.invalidateQueries({ queryKey: ['apps'] });
            onOpenChange(false);
          }
        })
        .catch((error: unknown) => {
          toast.error(resolveInstallApiError(error, t));
        });
      return;
    }

    installMutation.mutate(config, {
      onSuccess: (data: any) => {
        const tid = data?.task_id;
        if (tid) {
          setTaskId(tid);
          setStep(steps.length); // Move to progress step
        } else {
          // Fallback: no task_id returned, treat as sync success
          toast.success(t('sys.apps.toast.installSuccess', 'App installed'));
          queryClient.invalidateQueries({ queryKey: ['store', 'apps'] });
          onOpenChange(false);
        }
      },
      onError: (error: unknown) => {
        toast.error(resolveInstallApiError(error, t));
      },
    });
  };

  // Watch progress for completion/error
  useEffect(() => {
    if (!progress || !taskId) return;
    if (progress.phase === 'complete') {
      toast.success(t('sys.apps.toast.installSuccess', 'App installed'));
      queryClient.invalidateQueries({ queryKey: ['apps'] });
      queryClient.invalidateQueries({ queryKey: ['store', 'apps'] });
      queryClient.invalidateQueries({ queryKey: ['containers'] });
      onOpenChange(false);
    }
    if (progress.phase === 'error') {
      queryClient.invalidateQueries({ queryKey: ['apps'] });
      queryClient.invalidateQueries({ queryKey: ['store', 'apps'] });
    }
  }, [progress?.phase]);

  const nextStep = () => {
    // Validate current step before proceeding
    if (!steps[step].validate()) {
      if (step === 0) {
        if (sourceType === 'registry') {
          const v = config.image.trim();
          if (!v) {
            setImageAddressError(
              t('sys.apps.import.image_required', 'Image address is required')
            );
          } else if (!isValidContainerImageRef(v)) {
            setImageAddressError(
              t(
                'sys.apps.import.invalid_image_ref',
                'Invalid image address. Use a valid registry path, e.g. docker.io/library/nginx:latest'
              )
            );
          } else {
            setImageAddressError(null);
          }
        } else if (sourceType === 'upload') {
          if (isUploadingImage) {
            toast.error(
              t(
                'sys.apps.import.image_uploading',
                'Image upload is still in progress, please wait'
              )
            );
          } else {
            toast.error(
              t(
                'sys.apps.import.image_upload_required',
                'Please upload an image file'
              )
            );
          }
        } else {
          toast.error(
            t('sys.apps.import.manifest_required', 'Please upload app.yaml')
          );
        }
      } else {
        toast.error(
          t(
            'sys.apps.import.validation_error',
            'Please fill in all required fields'
          )
        );
      }
      return;
    }

    if (step === 0) setImageAddressError(null);

    if (step === 0 && sourceType === 'upload') {
      const { path, name } = uploadedImageRef.current;
      if (path && path !== config.image_path) {
        setConfig(prev => ({
          ...prev,
          image_path: path,
          image: name || prev.image,
        }));
      }
    }

    // Package mode: install directly from step 0
    if (sourceType === 'package' && step === 0) {
      handleInstall();
      return;
    }

    if (step === steps.length - 1) {
      handleInstall();
    } else {
      setStep(step + 1);
    }
  };

  const prevStep = () => {
    if (step > 0) setStep(step - 1);
  };

  const renderStep = () => {
    switch (step) {
      case 0: // Source
        return (
          <div className="px-4 py-4 sm:px-6 sm:py-5 md:px-10 lg:px-12">
            <h2 className="mb-2 text-center text-xl font-bold text-foreground sm:text-2xl">
              {t('sys.apps.import.source_title')}
            </h2>
            <p className="mb-6 text-center text-muted-foreground sm:mb-8">
              {t('sys.apps.import.source_desc')}
            </p>

            <div className="mb-6 grid grid-cols-1 gap-4 sm:mb-8 sm:grid-cols-3 sm:gap-4">
              <div
                className={`relative flex cursor-pointer flex-col items-center rounded-xl border-2 p-4 transition-all sm:p-6 ${
                  sourceType === 'registry'
                    ? 'border-primary bg-primary/5'
                    : 'border-border hover:border-primary/50'
                }`}
                onClick={() => {
                  if (sourceType === 'registry') return;
                  setSourceType('registry');
                  setImageAddressError(null);
                  uploadedImageRef.current = { path: '', name: '', size: 0 };
                  // Avoid mixing states between different source modes
                  setConfig(prev => ({ ...prev, image_path: '' }));
                }}
              >
                {sourceType === 'registry' && (
                  <div className="absolute top-2 right-2 text-primary">
                    <CheckCircle2
                      className="w-5 h-5"
                      fill="currentColor"
                      stroke="white"
                    />
                  </div>
                )}
                <div className="w-12 h-12 bg-primary rounded-xl flex items-center justify-center text-primary-foreground mb-4">
                  <Globe className="w-6 h-6" />
                </div>
                <h3 className="font-semibold text-foreground mb-1">
                  {t('sys.apps.import.registry_title')}
                </h3>
                <p className="text-sm text-muted-foreground text-center">
                  {t('sys.apps.import.registry_desc')}
                </p>
              </div>

              <div
                className={`relative flex cursor-pointer flex-col items-center rounded-xl border-2 p-4 transition-all sm:p-6 ${
                  sourceType === 'upload'
                    ? 'border-primary bg-primary/5'
                    : 'border-border hover:border-primary/50'
                }`}
                onClick={() => {
                  if (sourceType === 'upload') return;
                  setSourceType('upload');
                  setImageAddressError(null);
                  uploadedImageRef.current = { path: '', name: '', size: 0 };
                  // Avoid mixing states between different source modes
                  setConfig(prev => ({ ...prev, image: '', image_path: '' }));
                }}
              >
                {sourceType === 'upload' && (
                  <div className="absolute top-2 right-2 text-primary">
                    <CheckCircle2
                      className="w-5 h-5"
                      fill="currentColor"
                      stroke="white"
                    />
                  </div>
                )}
                <div className="w-12 h-12 bg-primary rounded-xl flex items-center justify-center text-primary-foreground mb-4">
                  <UploadCloud className="w-6 h-6" />
                </div>
                <h3 className="font-semibold text-foreground mb-1">
                  {t('sys.apps.import.upload_title')}
                </h3>
                <p className="text-sm text-muted-foreground text-center">
                  {t('sys.apps.import.upload_desc')}
                </p>
              </div>

              <div
                className={`relative flex cursor-pointer flex-col items-center rounded-xl border-2 p-4 transition-all sm:p-6 ${
                  sourceType === 'package'
                    ? 'border-primary bg-primary/5'
                    : 'border-border hover:border-primary/50'
                }`}
                onClick={() => {
                  if (sourceType === 'package') return;
                  setSourceType('package');
                  setImageAddressError(null);
                  uploadedImageRef.current = { path: '', name: '', size: 0 };
                  // Avoid mixing states between different source modes
                  setConfig(prev => ({ ...prev, image: '', image_path: '' }));
                }}
              >
                {sourceType === 'package' && (
                  <div className="absolute top-2 right-2 text-primary">
                    <CheckCircle2
                      className="w-5 h-5"
                      fill="currentColor"
                      stroke="white"
                    />
                  </div>
                )}
                <div className="w-12 h-12 bg-primary rounded-xl flex items-center justify-center text-primary-foreground mb-4">
                  <FileText className="w-6 h-6" />
                </div>
                <h3 className="font-semibold text-foreground mb-1">
                  {t('sys.apps.import.package_title', 'Upload Package')}
                </h3>
                <p className="text-sm text-muted-foreground text-center">
                  {t(
                    'sys.apps.import.package_desc',
                    'Upload app.yaml + image for complete app configuration'
                  )}
                </p>
              </div>
            </div>

            {sourceType === 'registry' ? (
              <div>
                <Label>
                  {t('sys.apps.import.image_address')}
                  <span className="text-red-500 ml-1">*</span>
                </Label>
                <Input
                  placeholder="docker.io/library/nginx:latest"
                  value={config.image}
                  onChange={e => {
                    setConfig({ ...config, image: e.target.value });
                    setImageAddressError(null);
                  }}
                  className={`mt-2 ${imageAddressError ? 'border-red-500 focus-visible:ring-red-500' : ''}`}
                />
                {imageAddressError && (
                  <div className="mt-2 text-sm text-red-500">
                    {imageAddressError}
                  </div>
                )}
              </div>
            ) : sourceType === 'upload' ? (
              <ImageUpload
                onUploadSuccess={(path, imageName, size) => {
                  if (cancelRequestedRef.current) {
                    cleanupUploadedFiles([path]);
                    return;
                  }
                  uploadedImageRef.current = { path, name: imageName, size };
                  setConfig(prev => ({
                    ...prev,
                    image_path: path,
                    image: imageName,
                  }));
                }}
                onUploadingChange={setIsUploadingImage}
                onClear={(path?: string) => {
                  const pathToDelete =                    path || uploadedImageRef.current.path || config.image_path;
                  if (pathToDelete) cleanupUploadedFiles([pathToDelete]);
                  uploadedImageRef.current = { path: '', name: '', size: 0 };
                  setConfig(prev => ({ ...prev, image_path: '', image: '' }));
                }}
                initialFile={
                  config.image_path
                    ? {
                        path: config.image_path,
                        filename: config.image,
                        size: uploadedImageRef.current.size || undefined,
                      }
                    : undefined
                }
              />
            ) : (
              /* Package mode: upload app.yaml + image.tar */
              <div className="space-y-6">
                {/* Manifest upload */}
                <div>
                  <Label className="text-base font-semibold mb-2 block">
                    {t(
                      'sys.apps.import.manifest_file',
                      'App Manifest (app.yaml)'
                    )}
                    <span className="text-red-500 ml-1">*</span>
                  </Label>
                  {manifestPath ? (
                    <>
                      <div className="flex items-center justify-between gap-2 text-sm border rounded-lg p-3">
                        <div className="flex items-center gap-2 min-w-0">
                          <CheckCircle2 className="w-5 h-5 text-green-500 shrink-0" />
                          <span className="font-medium text-foreground truncate">
                            {manifestMeta?.id || 'app.yaml'}
                          </span>
                          <span className="text-muted-foreground truncate">
                            {manifestMeta?.name
                              && `(${manifestMeta.name} v${manifestMeta?.version || '1.0.0'})`}
                          </span>
                        </div>
                        <Button
                          variant="ghost"
                          size="sm"
                          onClick={() => {
                            setManifestPath('');
                            setManifestMeta(null);
                          }}
                        >
                          <X className="w-4 h-4" />
                        </Button>
                      </div>
                      {manifestMeta?.id
                        && existingAppIds.has(manifestMeta.id) && (
                          <p className="mt-1 text-sm text-red-500">
                            {t(
                              'sys.apps.import.duplicate_id_warning',
                              '此应用ID已存在，安装时将覆盖已有应用'
                            )}
                          </p>
                        )}
                    </>
                  ) : (
                    <FileUpload
                      single
                      loading={isUploadingManifest}
                      accept={{
                        'application/x-yaml': ['.yaml', '.yml'],
                        'text/yaml': ['.yaml', '.yml'],
                        'text/plain': ['.yaml', '.yml'],
                      }}
                      placeholder={t(
                        'sys.apps.import.click_upload_manifest',
                        'Click to upload app.yaml'
                      )}
                      onUpload={async files => {
                        const file = files[0];
                        if (!file) return;
                        setIsUploadingManifest(true);
                        try {
                          const res = await appsApi.uploadManifest(file);
                          const data = res?.data;
                          if (data?.path) {
                            setManifestPath(data.path);
                            setManifestMeta(data.metadata || null);
                          }
                        } catch (err: unknown) {
                          toast.error(
                            resolveInstallApiError(err, t)
                              || t(
                                'sys.apps.import.manifest_upload_failed',
                                'Manifest upload failed'
                              )
                          );
                        } finally {
                          setIsUploadingManifest(false);
                        }
                      }}
                    />
                  )}
                </div>

                {/* Image upload (optional — app.yaml may reference registry image) */}
                <div>
                  <Label className="text-base font-semibold mb-2 block">
                    {t('sys.apps.import.package_image', 'Container Image')}
                    <span className="text-xs text-muted-foreground ml-2 font-normal">
                      {t(
                        'sys.apps.import.package_image_hint',
                        'Optional if app.yaml uses registry image'
                      )}
                    </span>
                  </Label>
                  {packageImagePath ? (
                    <div className="flex items-center gap-2 text-sm border rounded-lg p-3">
                      <Package className="w-5 h-5 text-muted-foreground" />
                      <span className="font-medium text-foreground">
                        {packageImageName || 'image.tar'}
                      </span>
                      <Button
                        variant="ghost"
                        size="sm"
                        onClick={() => {
                          cleanupUploadedFiles([packageImagePath]);
                          setPackageImagePath('');
                          setPackageImageName('');
                        }}
                      >
                        <X className="w-4 h-4" />
                      </Button>
                    </div>
                  ) : (
                    <ImageUpload
                      onUploadSuccess={(path, imageName, size) => {
                        if (cancelRequestedRef.current) {
                          cleanupUploadedFiles([path]);
                          return;
                        }
                        setPackageImagePath(path);
                        setPackageImageName(imageName);
                        uploadedImageRef.current = {
                          path,
                          name: imageName,
                          size,
                        };
                      }}
                      onUploadingChange={setIsUploadingImage}
                      onClear={(path?: string) => {
                        const pathToDelete = path || packageImagePath;
                        if (pathToDelete) cleanupUploadedFiles([pathToDelete]);
                        setPackageImagePath('');
                        setPackageImageName('');
                      }}
                    />
                  )}
                </div>
              </div>
            )}
          </div>
        );

      case 1: // Basic Info
        return (
          <div className="px-4 py-4 sm:px-6 sm:py-5 md:px-10 lg:px-12 space-y-4">
            <div>
              <Label>{t('sys.apps.import.app_id')}</Label>
              <Input
                placeholder="my-app"
                value={config.metadata.id}
                onChange={e => setConfig({
                    ...config,
                    metadata: { ...config.metadata, id: e.target.value },
                  })}
                className={`mt-2 ${existingAppIds.has(config.metadata.id.trim()) ? 'border-red-500 focus-visible:ring-red-500' : ''}`}
              />
              {existingAppIds.has(config.metadata.id.trim()) && (
                <p className="mt-1 text-sm text-red-500">
                  {t(
                    'sys.apps.import.duplicate_id_warning',
                    '此应用ID已存在，安装时将覆盖已有应用'
                  )}
                </p>
              )}
            </div>

            <div>
              <Label>{t('sys.apps.import.app_name')}</Label>
              <Input
                placeholder="My Application"
                value={config.metadata.name}
                onChange={e => setConfig({
                    ...config,
                    metadata: { ...config.metadata, name: e.target.value },
                  })}
                className="mt-2"
              />
            </div>

            <div>
              <Label>{t('sys.apps.import.version')}</Label>
              <Input
                placeholder="1.0.0"
                value={config.metadata.version}
                onChange={e => setConfig({
                    ...config,
                    metadata: { ...config.metadata, version: e.target.value },
                  })}
                className="mt-2"
              />
            </div>

            <div>
              <Label>{t('sys.apps.import.description')}</Label>
              <Input
                placeholder="Application description"
                value={config.metadata.description}
                onChange={e => setConfig({
                    ...config,
                    metadata: {
                      ...config.metadata,
                      description: e.target.value,
                    },
                  })}
                className="mt-2"
              />
            </div>
          </div>
        );

      case 2: // Resources
        return (
          <div className="px-4 py-4 sm:px-6 sm:py-5 md:px-10 lg:px-12 space-y-6">
            <ScrollArea className="max-h-[400px] border rounded-lg">
              <div className="space-y-4 p-6">
                <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
                  <div>
                    <Label>{t('sys.apps.import.cpu_limit')}</Label>
                    <div className="mt-2 flex items-center gap-2">
                      <Input
                        inputMode="numeric"
                        autoComplete="off"
                        maxLength={3}
                        placeholder="50"
                        className="flex-1 min-w-0"
                        value={cpuPercentToInputValue(config.resources?.cpu)}
                        onChange={e => setConfig({
                            ...config,
                            resources: {
                              ...config.resources!,
                              cpu: inputDigitsToCpuPercent(e.target.value),
                            },
                          })}
                      />
                      <span className="shrink-0 text-sm text-muted-foreground tabular-nums">
                        %
                      </span>
                    </div>
                  </div>

                  <div>
                    <Label>{t('sys.apps.import.memory_limit')}</Label>
                    <Select
                      value={config.resources?.memory}
                      onValueChange={value => setConfig({
                          ...config,
                          resources: { ...config.resources!, memory: value },
                        })}
                    >
                      <SelectTrigger className="mt-2">
                        <SelectValue />
                      </SelectTrigger>
                      <SelectContent>
                        {MEMORY_OPTIONS.map(opt => (
                          <SelectItem key={opt} value={opt}>
                            {opt}
                          </SelectItem>
                        ))}
                      </SelectContent>
                    </Select>
                  </div>
                </div>

                <div>
                  <Label className="text-base font-semibold mb-3 block">
                    {t('sys.apps.import.runtime_options', 'Runtime Options')}
                  </Label>

                  <div className="flex items-center space-x-2">
                    <Checkbox
                      checked={config.autostart}
                      onCheckedChange={checked => setConfig({ ...config, autostart: !!checked })}
                    />
                    <Label className="font-normal">
                      {t('sys.apps.import.autostart', 'Auto Start')}
                    </Label>
                  </div>

                  <div className="mt-6">
                    <Label>
                      {t('sys.apps.import.restart_policy', 'Restart Policy')}
                    </Label>
                    <Select
                      value={config.restart_policy}
                      onValueChange={value => setConfig({ ...config, restart_policy: value })}
                    >
                      <SelectTrigger className="mt-2">
                        <SelectValue />
                      </SelectTrigger>
                      <SelectContent>
                        <SelectItem value="no">
                          {t('sys.apps.import.restart_no', 'No Restart')}
                        </SelectItem>
                        <SelectItem value="on-failure">
                          {t(
                            'sys.apps.import.restart_on_failure',
                            '失败时重启'
                          )}
                        </SelectItem>
                        <SelectItem value="always">
                          {t(
                            'sys.apps.import.restart_always',
                            'Always Restart'
                          )}
                        </SelectItem>
                      </SelectContent>
                    </Select>
                  </div>
                </div>
              </div>
            </ScrollArea>
          </div>
        );

      case 3: // Permissions
        return (
          <ScrollArea>
            <div className="px-4 py-4 sm:px-6 sm:py-5 md:px-10 lg:px-12 space-y-6 pr-2 sm:pr-4">
              {/* AI Models */}
              <div>
                <Label className="text-base font-semibold mb-3 block">
                  {t('sys.apps.import.ai_models')}
                </Label>
                <ScrollArea className="h-[200px] rounded-lg border">
                  <div className="space-y-2 p-3">
                    {availableModels.length > 0 ? (
                      availableModels.map(model => (
                        <div
                          key={model.model_id}
                          className="flex items-center space-x-2"
                        >
                          <Checkbox
                            checked={config.permissions?.inference?.models?.includes(
                              model.model_id
                            )}
                            onCheckedChange={() => toggleModel(model.model_id)}
                          />
                          <Label className="font-normal">
                            {model.name || model.model_id}
                          </Label>
                        </div>
                      ))
                    ) : (
                      <p className="text-sm text-muted-foreground">
                        {t('sys.apps.import.no_models')}
                      </p>
                    )}
                  </div>
                </ScrollArea>
              </div>

              {/* Max QPS */}
              <div>
                <Label>
                  {t('sys.apps.import.max_qps', 'Max Inference QPS')}
                </Label>
                <Input
                  type="number"
                  min={1}
                  max={1000}
                  value={config.permissions?.inference?.max_qps || 10}
                  onChange={e => setConfig({
                      ...config,
                      permissions: {
                        ...config.permissions!,
                        inference: {
                          ...config.permissions!.inference!,
                          max_qps: parseInt(e.target.value, 10) || 10,
                        },
                      },
                    })}
                  className="mt-2 w-full max-w-48 sm:w-32"
                />
              </div>

              {/* Max Concurrent */}
              <div>
                <Label>
                  {t(
                    'sys.apps.import.max_concurrent',
                    'Max Concurrent Inference'
                  )}
                </Label>
                <Input
                  type="number"
                  min={0}
                  max={100}
                  placeholder="0"
                  value={config.permissions?.inference?.max_concurrent || ''}
                  onChange={e => setConfig({
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
                    checked={
                      config.permissions?.inference?.allow_register_model
                    }
                    onCheckedChange={checked => setConfig({
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

              {/* Video Streams */}
              <div>
                <Label className="text-base font-semibold mb-3 block">
                  {t(
                    'sys.apps.import.video_streams',
                    'Video Stream Permissions'
                  )}
                </Label>
                <ScrollArea className="max-h-[200px] border rounded-lg p-3">
                  <div className="flex flex-wrap gap-2 pr-4">
                    {availableStreams.length > 0 ? (
                      availableStreams.map(stream => {
                        const streamValue = stream.stream_id;
                        return (
                          <label
                            key={stream.stream_id}
                            className={`inline-flex items-center space-x-2 px-3 py-1.5 rounded-full border cursor-pointer transition-all ${
                              config.permissions?.video?.includes(streamValue)
                                ? 'border-[#f24a00] bg-[#fff5f0] text-[#f24a00]'
                                : 'border-gray-200 hover:border-gray-300'
                            }`}
                          >
                            <Checkbox
                              checked={config.permissions?.video?.includes(
                                streamValue
                              )}
                              onCheckedChange={checked => {
                                const current = config.permissions?.video || [];
                                const updated = checked
                                  ? [...current, streamValue]
                                  : current.filter(v => v !== streamValue);
                                setConfig({
                                  ...config,
                                  permissions: {
                                    ...config.permissions!,
                                    video: updated,
                                  },
                                });
                              }}
                              className="sr-only"
                            />
                            <span className="text-sm">
                              {stream.stream_id}
                              {stream.width && stream.height && (
                                <span className="text-xs text-gray-400 ml-1">
                                  ({stream.width}x{stream.height}
                                  {stream.fps ? `@${stream.fps}` : ''})
                                </span>
                              )}
                            </span>
                          </label>
                        );
                      })
                    ) : (
                      <p className="text-sm text-gray-500">
                        {t(
                          'sys.apps.import.no_streams',
                          'No available video streams'
                        )}
                      </p>
                    )}
                  </div>
                </ScrollArea>
              </div>

              {/* Events */}
              <div>
                <Label className="text-base font-semibold mb-3 block">
                  {t('sys.apps.import.events', 'Event Permissions')}
                </Label>
                <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
                  <div>
                    <Label className="text-sm text-muted-foreground">
                      {t('sys.apps.import.events_publish', 'Publish Topics')}
                    </Label>
                    <Input
                      placeholder="app/output"
                      value={
                        config.permissions?.events?.publish?.join(', ') || ''
                      }
                      onChange={e => setConfig({
                          ...config,
                          permissions: {
                            ...config.permissions!,
                            events: {
                              ...config.permissions!.events!,
                              publish: e.target.value
                                .split(',')
                                .map(s => s.trim())
                                .filter(Boolean),
                            },
                          },
                        })}
                      className="mt-1"
                    />
                    <p className="text-xs text-muted-foreground mt-1">
                      {t(
                        'sys.apps.import.events_hint',
                        'Separate topics with commas'
                      )}
                    </p>
                  </div>
                  <div>
                    <Label className="text-sm text-muted-foreground">
                      {t(
                        'sys.apps.import.events_subscribe',
                        'Subscribe Topics'
                      )}
                    </Label>
                    <Input
                      placeholder="camera/*, sensor/#"
                      value={
                        config.permissions?.events?.subscribe?.join(', ') || ''
                      }
                      onChange={e => setConfig({
                          ...config,
                          permissions: {
                            ...config.permissions!,
                            events: {
                              ...config.permissions!.events!,
                              subscribe: e.target.value
                                .split(',')
                                .map(s => s.trim())
                                .filter(Boolean),
                            },
                          },
                        })}
                      className="mt-1"
                    />
                    <p className="text-xs text-muted-foreground mt-1">
                      {t(
                        'sys.apps.import.events_hint',
                        'Separate topics with commas'
                      )}
                    </p>
                  </div>
                </div>
              </div>

              {/* Network */}
              <div>
                <Label className="text-base font-semibold mb-3 block">
                  {t('sys.apps.import.network', 'Network Mode')}
                </Label>
                <Select
                  value={config.permissions?.network?.mode || 'isolated'}
                  onValueChange={value => setConfig({
                      ...config,
                      permissions: {
                        ...config.permissions!,
                        network: {
                          ...config.permissions!.network,
                          mode: value,
                        },
                      },
                    })}
                >
                  <SelectTrigger className="mt-2 w-full sm:w-48">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="isolated">
                      {t('sys.apps.import.network_isolated', 'Isolated')}
                    </SelectItem>
                    <SelectItem value="host">
                      {t('sys.apps.import.network_host', 'Host')}
                    </SelectItem>
                  </SelectContent>
                </Select>
                <p className="text-xs text-gray-400 mt-1">
                  {t(
                    'sys.apps.import.network_hint',
                    '隔离模式：无网络访问；主机模式：共享主机网络'
                  )}
                </p>
                {config.permissions?.network?.mode === 'host' && (
                  <div className="mt-3">
                    <Label className="text-sm text-muted-foreground">
                      {t('sys.apps.import.inbound_ports', '入站端口')}
                    </Label>
                    <Input
                      className="mt-1 w-full sm:w-48"
                      placeholder="e.g. 8889"
                      value={(config.permissions?.network?.inbound || []).join(
                        ', '
                      )}
                      onChange={e => {
                        const ports = e.target.value
                          .split(/[,\s]+/)
                          .map(s => parseInt(s.trim(), 10))
                          .filter(n => !Number.isNaN(n) && n > 0 && n < 65536);
                        setConfig({
                          ...config,
                          permissions: {
                            ...config.permissions!,
                            network: {
                              ...config.permissions!.network!,
                              inbound: ports,
                            },
                          },
                        });
                      }}
                    />
                    <p className="text-xs text-gray-400 mt-1">
                      {t(
                        'sys.apps.import.inbound_hint',
                        '应用对外暴露的端口，多个用逗号分隔'
                      )}
                    </p>
                  </div>
                )}
              </div>

              {/* Device Control */}
              <div>
                <Label className="text-base font-semibold mb-3 block">
                  {t('sys.apps.import.device_control')}
                </Label>
                <div className="space-y-2 pr-4 border rounded-lg p-3">
                  <div className="flex items-center space-x-2">
                    <Checkbox
                      checked={config.permissions?.device?.light}
                      onCheckedChange={checked => setConfig({
                          ...config,
                          permissions: {
                            ...config.permissions!,
                            device: {
                              ...config.permissions!.device!,
                              light: !!checked,
                            },
                          },
                        })}
                    />
                    <Label className="font-normal">
                      {t('sys.apps.import.light_control')}
                    </Label>
                  </div>
                  <div className="flex items-center space-x-2">
                    <Checkbox
                      checked={config.permissions?.device?.ir_cut}
                      onCheckedChange={checked => setConfig({
                          ...config,
                          permissions: {
                            ...config.permissions!,
                            device: {
                              ...config.permissions!.device!,
                              ir_cut: !!checked,
                            },
                          },
                        })}
                    />
                    <Label className="font-normal">
                      {t('sys.apps.import.ir_cut')}
                    </Label>
                  </div>
                  <div className="flex items-center space-x-2">
                    <Checkbox
                      checked={config.permissions?.device?.ptz}
                      onCheckedChange={checked => setConfig({
                          ...config,
                          permissions: {
                            ...config.permissions!,
                            device: {
                              ...config.permissions!.device!,
                              ptz: !!checked,
                            },
                          },
                        })}
                    />
                    <Label className="font-normal">
                      {t('sys.apps.import.ptz_control')}
                    </Label>
                  </div>
                  <div className="flex items-center space-x-2">
                    <Checkbox
                      checked={config.permissions?.device?.lens}
                      onCheckedChange={checked => setConfig({
                          ...config,
                          permissions: {
                            ...config.permissions!,
                            device: {
                              ...config.permissions!.device!,
                              lens: !!checked,
                            },
                          },
                        })}
                    />
                    <Label className="font-normal">
                      {t('sys.apps.import.lens_control', 'Lens Control')}
                    </Label>
                  </div>
                </div>
              </div>
            </div>
          </ScrollArea>
        );

      case 4: // Advanced (Env & Volumes)
        return (
          <div className="px-4 py-4 sm:px-6 sm:py-5 md:px-10 lg:px-12 space-y-6">
            {/* Environment Variables */}
            <div>
              <div className="mb-3 flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between">
                <Label className="text-base font-semibold">
                  {t('sys.apps.import.env_vars', 'Environment Variables')}
                </Label>
                <Button
                  variant="outline"
                  size="sm"
                  className="w-full shrink-0 sm:w-auto"
                  onClick={() => setConfig({
                      ...config,
                      env: [...(config.env || []), { name: '', value: '' }],
                    })}
                >
                  <Plus className="w-4 h-4 mr-1" />
                  {t('common.add', 'Add')}
                </Button>
              </div>
              <div className="space-y-2 border rounded-lg p-4">
                {config.env?.map((env, index) => (
                  <div
                    key={index}
                    className="flex flex-col gap-2 sm:flex-row sm:items-center"
                  >
                    <Input
                      placeholder="NAME"
                      value={env.name}
                      onChange={e => {
                        const newEnv = [...(config.env || [])];
                        newEnv[index] = {
                          ...newEnv[index],
                          name: e.target.value,
                        };
                        setConfig({ ...config, env: newEnv });
                      }}
                      className="flex-1"
                    />
                    <Input
                      placeholder="value"
                      value={env.value}
                      onChange={e => {
                        const newEnv = [...(config.env || [])];
                        newEnv[index] = {
                          ...newEnv[index],
                          value: e.target.value,
                        };
                        setConfig({ ...config, env: newEnv });
                      }}
                      className="flex-1"
                    />
                    <Button
                      variant="ghost"
                      size="icon"
                      onClick={() => {
                        const newEnv = config.env?.filter(
                          (_, i) => i !== index
                        );
                        setConfig({ ...config, env: newEnv });
                      }}
                    >
                      <X className="w-4 h-4" />
                    </Button>
                  </div>
                ))}
                {(!config.env || config.env.length === 0) && (
                  <p className="text-sm text-muted-foreground py-2">
                    {t(
                      'sys.apps.import.no_env_vars',
                      'No environment variables'
                    )}
                  </p>
                )}
              </div>
            </div>

            {/* Volumes */}
            <div>
              <div className="mb-3 flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between">
                <Label className="text-base font-semibold">
                  {t('sys.apps.import.volumes', 'Volume Mounts')}
                </Label>
                <Button
                  variant="outline"
                  size="sm"
                  className="w-full shrink-0 sm:w-auto"
                  onClick={() => setConfig({
                      ...config,
                      volumes: [
                        ...(config.volumes || []),
                        { host: '', container: '', readonly: false },
                      ],
                    })}
                >
                  <Plus className="w-4 h-4 mr-1" />
                  {t('common.add', 'Add')}
                </Button>
              </div>
              <div className="space-y-2 border rounded-lg p-4">
                {config.volumes?.map((vol, index) => (
                  <div
                    key={index}
                    className="flex flex-col gap-2 sm:flex-row sm:items-center"
                  >
                    <Input
                      placeholder={t('sys.apps.import.host_path', 'Host Path')}
                      value={vol.host}
                      onChange={e => {
                        const newVols = [...(config.volumes || [])];
                        newVols[index] = {
                          ...newVols[index],
                          host: e.target.value,
                        };
                        setConfig({ ...config, volumes: newVols });
                      }}
                      className="flex-1"
                    />
                    <Input
                      placeholder={t(
                        'sys.apps.import.container_path',
                        '容器路径'
                      )}
                      value={vol.container}
                      onChange={e => {
                        const newVols = [...(config.volumes || [])];
                        newVols[index] = {
                          ...newVols[index],
                          container: e.target.value,
                        };
                        setConfig({ ...config, volumes: newVols });
                      }}
                      className="flex-1"
                    />
                    <label className="flex items-center space-x-1 text-sm whitespace-nowrap">
                      <Checkbox
                        checked={vol.readonly}
                        onCheckedChange={checked => {
                          const newVols = [...(config.volumes || [])];
                          newVols[index] = {
                            ...newVols[index],
                            readonly: !!checked,
                          };
                          setConfig({ ...config, volumes: newVols });
                        }}
                      />
                      <span>RO</span>
                    </label>
                    <Button
                      variant="ghost"
                      size="icon"
                      onClick={() => {
                        const newVols = config.volumes?.filter(
                          (_, i) => i !== index
                        );
                        setConfig({ ...config, volumes: newVols });
                      }}
                    >
                      <X className="w-4 h-4" />
                    </Button>
                  </div>
                ))}
                {(!config.volumes || config.volumes.length === 0) && (
                  <p className="text-sm text-muted-foreground py-2">
                    {t('sys.apps.import.no_volumes', 'No volume mounts')}
                  </p>
                )}
              </div>
            </div>
          </div>
        );

      case 5: // Review
        return (
          <div className="px-4 py-4 sm:px-6 sm:py-5 md:px-10 lg:px-12">
            <h2 className="mb-4 text-center text-xl font-bold text-foreground sm:mb-6 sm:text-2xl">
              {t('sys.apps.import.review_title')}
            </h2>

            <div className="space-y-4 pr-0 sm:pr-4">
              {/* Basic Info */}
              <div className="border-b border-border pb-3">
                <p className="text-xs font-semibold text-muted-foreground uppercase mb-2">
                  {t('sys.apps.import.basic_info', 'Basic Info')}
                </p>
                <div className="grid grid-cols-1 gap-2 text-sm sm:grid-cols-2">
                  <div>
                    <span className="text-muted-foreground">ID:</span>
                    <span className="ml-2 font-medium text-foreground">
                      {config.metadata.id || '-'}
                    </span>
                  </div>
                  <div>
                    <span className="text-muted-foreground">
                      {t('sys.apps.import.app_name')}:
                    </span>
                    <span className="ml-2 font-medium text-foreground">
                      {config.metadata.name || '-'}
                    </span>
                  </div>
                  <div>
                    <span className="text-muted-foreground">
                      {t('sys.apps.import.version')}:
                    </span>
                    <span className="ml-2 text-foreground">
                      {config.metadata.version || '-'}
                    </span>
                  </div>
                </div>
              </div>

              {/* Image */}
              <div className="border-b border-border pb-3">
                <p className="text-xs font-semibold text-muted-foreground uppercase mb-2">
                  {t('sys.apps.import.source', 'Image')}
                </p>
                <p className="font-mono text-sm break-all text-foreground">
                  {config.image || config.image_path || '-'}
                </p>
              </div>

              {/* Resources */}
              <div className="border-b border-border pb-3">
                <p className="text-xs font-semibold text-muted-foreground uppercase mb-2">
                  {t('sys.apps.import.resources', 'Resources')}
                </p>
                <div className="grid grid-cols-1 gap-2 text-sm sm:grid-cols-2">
                  <div>
                    <span className="text-muted-foreground">CPU:</span>{' '}
                    <span className="text-foreground">
                      {config.resources?.cpu}
                    </span>
                  </div>
                  <div>
                    <span className="text-muted-foreground">Memory:</span>{' '}
                    <span className="text-foreground">
                      {config.resources?.memory}
                    </span>
                  </div>
                  <div>
                    <span className="text-muted-foreground">
                      {t('sys.apps.import.autostart', 'Auto Start')}:
                    </span>{' '}
                    <span className="text-foreground">
                      {config.autostart ? '✓' : '-'}
                    </span>
                  </div>
                  <div>
                    <span className="text-muted-foreground">
                      {t('sys.apps.import.restart_policy', 'Restart')}:
                    </span>{' '}
                    <span className="text-foreground">
                      {config.restart_policy}
                    </span>
                  </div>
                </div>
              </div>

              {/* Permissions */}
              <div className="border-b border-border pb-3">
                <p className="text-xs font-semibold text-muted-foreground uppercase mb-2">
                  {t('sys.apps.import.permissions', 'Permissions')}
                </p>
                <div className="space-y-1 text-sm">
                  <div className="flex flex-col gap-1 text-sm sm:flex-row sm:gap-4">
                    <span className="shrink-0 text-muted-foreground sm:w-20">
                      Models:
                    </span>
                    <span className="min-w-0 text-foreground">
                      {config.permissions?.inference?.models?.length || 0}{' '}
                      selected
                    </span>
                  </div>
                  <div className="flex flex-col gap-1 sm:flex-row sm:gap-4">
                    <span className="shrink-0 text-muted-foreground sm:w-20">
                      Max QPS:
                    </span>
                    <span className="min-w-0 text-foreground">
                      {config.permissions?.inference?.max_qps || 10}
                    </span>
                  </div>
                  <div className="flex flex-col gap-1 sm:flex-row sm:gap-4">
                    <span className="shrink-0 text-muted-foreground sm:w-20">
                      Concurrent:
                    </span>
                    <span className="min-w-0 text-foreground">
                      {config.permissions?.inference?.max_concurrent || '-'}
                    </span>
                  </div>
                  <div className="flex flex-col gap-1 sm:flex-row sm:gap-4">
                    <span className="shrink-0 text-muted-foreground sm:w-20">
                      Register:
                    </span>
                    <span className="min-w-0 text-foreground">
                      {config.permissions?.inference?.allow_register_model
                        ? 'Enabled'
                        : '-'}
                    </span>
                  </div>
                  <div className="flex flex-col gap-1 sm:flex-row sm:gap-4">
                    <span className="shrink-0 text-muted-foreground sm:w-20">
                      Video:
                    </span>
                    <span className="min-w-0 break-all text-foreground">
                      {config.permissions?.video?.join(', ') || '-'}
                    </span>
                  </div>
                  <div className="flex flex-col gap-1 sm:flex-row sm:gap-4">
                    <span className="shrink-0 text-muted-foreground sm:w-20">
                      Network:
                    </span>
                    <span className="min-w-0 text-foreground">
                      {config.permissions?.network?.mode || 'isolated'}
                    </span>
                  </div>
                  <div className="flex flex-col gap-1 sm:flex-row sm:gap-4">
                    <span className="shrink-0 text-muted-foreground sm:w-20">
                      Device:
                    </span>
                    <span className="min-w-0 text-foreground">
                      {[
                        config.permissions?.device?.light && 'Light',
                        config.permissions?.device?.ir_cut && 'IR-Cut',
                        config.permissions?.device?.ptz && 'PTZ',
                        config.permissions?.device?.lens && 'Lens',
                      ]
                        .filter(Boolean)
                        .join(', ') || '-'}
                    </span>
                  </div>
                  <div className="flex flex-col gap-1 sm:flex-row sm:gap-4">
                    <span className="shrink-0 text-muted-foreground sm:w-20">
                      Events:
                    </span>
                    <span className="min-w-0 break-all text-foreground">
                      {config.permissions?.events?.publish?.length || 0} pub /{' '}
                      {config.permissions?.events?.subscribe?.length || 0} sub
                    </span>
                  </div>
                </div>
              </div>

              {/* Env & Volumes */}
              <div>
                <p className="text-xs font-semibold text-muted-foreground uppercase mb-2">
                  {t('sys.apps.import.advanced', 'Advanced')}
                </p>
                <div className="space-y-1 text-sm">
                  <div className="flex flex-col gap-1 sm:flex-row sm:gap-4">
                    <span className="shrink-0 text-muted-foreground sm:w-20">
                      Env:
                    </span>
                    <span className="min-w-0 text-foreground">
                      {config.env?.length || 0} vars
                    </span>
                  </div>
                  <div className="flex flex-col gap-1 sm:flex-row sm:gap-4">
                    <span className="shrink-0 text-muted-foreground sm:w-20">
                      Volumes:
                    </span>
                    <span className="min-w-0 text-foreground">
                      {config.volumes?.length || 0} mounts
                    </span>
                  </div>
                </div>
              </div>
            </div>
          </div>
        );

      case steps.length: // Progress (installing)
        return (
          <div className="flex flex-col items-center px-4 py-8 sm:px-8 sm:py-10 md:px-12 lg:px-12">
            {progress?.phase === 'error' ? (
              <>
                <div className="w-16 h-16 bg-destructive/10 rounded-full flex items-center justify-center mb-4">
                  <AlertCircle className="w-8 h-8 text-destructive" />
                </div>
                <h2 className="text-xl font-bold text-foreground mb-2">
                  {t('sys.apps.import.install_failed', 'Install Failed')}
                </h2>
                <p className="text-sm text-muted-foreground mb-6 max-w-md text-center">
                  {translateInstallError(
                    progress?.error || progress?.message,
                    t
                  )}
                </p>
                <Button
                  variant="carbon"
                  onClick={() => {
                    setTaskId(null);
                    setStep(0);
                    installMutation.reset();
                  }}
                >
                  {t('common.retry', 'Retry')}
                </Button>
              </>
            ) : (
              <>
                <div className="w-16 h-16 bg-primary/10 rounded-full flex items-center justify-center mb-4">
                  <Loader2 className="w-8 h-8 text-primary animate-spin" />
                </div>
                <h2 className="text-xl font-bold text-foreground mb-2">
                  {t('sys.apps.import.installing_title', 'Installing app...')}
                </h2>
                <p className="text-sm text-muted-foreground mb-6">
                  {translateInstallProgress(progress, t)}
                </p>
                <div className="w-full max-w-md">
                  <Progress value={progress?.percent ?? 0} className="h-2" />
                  <div className="flex justify-between mt-2 text-xs text-muted-foreground">
                    <span>{translateInstallPhase(progress?.phase, t)}</span>
                    <span>{Math.round(progress?.percent ?? 0)}%</span>
                  </div>
                </div>
              </>
            )}
          </div>
        );

      default:
        return null;
    }
  };

  return (
    <Dialog
      open={open}
      onOpenChange={nextOpen => {
        if (nextOpen) {
          cancelRequestedRef.current = false;
          onOpenChange(true);
          return;
        }
        handleCancel();
      }}
    >
      <DialogContent
        className="flex max-h-[90vh] w-full max-w-[calc(100%-1rem)] flex-col overflow-hidden rounded-2xl border-none p-0 shadow-2xl max-sm:fixed max-sm:inset-0 max-sm:left-0 max-sm:top-0 max-sm:h-dvh max-sm:max-h-dvh max-sm:max-w-none max-sm:translate-x-0 max-sm:translate-y-0 max-sm:rounded-none sm:max-w-[800px]"
        onInteractOutside={e => e.preventDefault()}
      >
        <div className="p-4 pb-2 sm:p-6 sm:pb-2">
          <DialogTitle className="pr-10 text-lg font-bold text-foreground sm:mb-6 sm:text-xl">
            {t('sys.apps.import.wizard_title', 'Application Setup Wizard')}
          </DialogTitle>
          <DialogDescription className="hidden">
            {t('sys.apps.import.wizard_description', 'Setup Wizard')}
          </DialogDescription>

          {/* Step Indicator — scroll on narrow screens */}
          <div className="-mx-1 mt-6 mb-4 overflow-x-auto pb-1 sm:mx-0 sm:mt-0 sm:overflow-visible sm:pb-0">
            <div className="flex min-w-max items-center justify-center gap-1 px-1 sm:min-w-0 sm:gap-0 sm:space-x-2">
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
                      className={`max-w-17 truncate px-0.5 text-center text-[10px] sm:max-w-24 sm:text-xs ${i === step ? 'text-primary font-medium' : 'text-muted-foreground'}`}
                    >
                      {s.title}
                    </span>
                  </div>
                  {i < steps.length - 1 && (
                    <div
                      className={`-mt-4 h-px w-6 shrink-0 sm:-mt-5 sm:w-12 ${i < step ? 'bg-green-500' : 'bg-border'}`}
                    />
                  )}
                </div>
              ))}
            </div>
          </div>
        </div>
        <div className="flex-1 min-h-0 overflow-y-auto">{renderStep()}</div>

        {step < steps.length && (
          <div className="flex flex-col gap-3 border-t border-border bg-muted/20 px-4 py-3 sm:flex-row sm:items-center sm:justify-between sm:px-6 sm:py-4">
            <Button
              variant="outline"
              className="order-2 w-full text-muted-foreground hover:text-foreground sm:order-1 sm:w-auto"
              onClick={prevStep}
              disabled={step === 0 || installMutation.isPending}
            >
              <ArrowLeft className="mr-2 h-4 w-4" />
              {t('sys.apps.import.previous')}
            </Button>

            <div className="order-1 flex w-full flex-col gap-2 sm:order-2 sm:w-auto sm:flex-row sm:items-center sm:space-x-4 sm:space-y-0">
              <Button
                variant="outline"
                className="w-full text-muted-foreground hover:text-foreground sm:w-auto"
                onClick={handleCancel}
                disabled={installMutation.isPending}
              >
                {t('common.cancel')}
              </Button>
              <Button
                variant="carbon"
                className="w-full sm:w-auto"
                onClick={nextStep}
                disabled={
                  installMutation.isPending
                  || isUploadingImage
                  || isUploadingManifest
                  || (step === 0 && !isStep0SourceReady)
                }
              >
                {(sourceType === 'package' && step === 0)
                || step === steps.length - 1
                  ? installMutation.isPending
                    ? t('sys.apps.import.installing')
                    : t('common.install', 'Install')
                  : t('sys.apps.import.continue')}
                <ArrowRight className="ml-2 h-4 w-4" />
              </Button>
            </div>
          </div>
        )}
      </DialogContent>
    </Dialog>
  );
}
