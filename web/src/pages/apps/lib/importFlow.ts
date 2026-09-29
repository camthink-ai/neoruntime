import type { WizardConfig } from '@/services/types';

/**
 * Pure helpers for the merged import flow (registry + local-upload). The
 * local source covers both old modes: yaml present → the manifest is the
 * source of truth (edits PATCH back onto the file); tar only → the wizard
 * form generates the manifest. Kept pure so it is testable without React.
 */

/** How a local import should be installed. `null` when nothing is uploaded. */
export type LocalMode = 'manifest' | 'image-only';

export function resolveLocalMode(input: {
  manifestPath: string;
  imageTarPath: string;
}): LocalMode | null {
  if (input.manifestPath) return 'manifest';
  if (input.imageTarPath) return 'image-only';
  return null;
}

/**
 * 容器镜像地址（Docker/OCI 风格）的表单校验：registry/仓库名[:tag] 或 @sha256:… digest
 */
export function isValidContainerImageRef(ref: string): boolean {
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

/**
 * 模型依赖别名（spec.models 的键）的表单校验，镜像后端 manifest.go
 * modelAliasPattern：字母或下划线开头，仅字母/数字/下划线。
 */
export function isValidModelAlias(alias: string): boolean {
  return /^[A-Za-z_][A-Za-z0-9_]*$/.test(alias);
}

/** Pages of the paginated import form, in nav order. */
export type ImportSectionId =
  | 'basic_info'
  | 'resources'
  | 'models'
  | 'permissions'
  | 'advanced';

export type InstallIssueReason =
  | 'app_id_required'
  | 'app_id_invalid'
  | 'app_name_required'
  | 'app_version_required'
  | 'invalid_image_ref'
  | 'local_source_required'
  | 'cpu_limit_invalid'
  | 'cpu_limit_high'
  | 'memory_limit_invalid'
  | 'memory_limit_high'
  | 'invalid_model_alias'
  | 'reserved_model_alias'
  | 'model_id_required'
  | 'model_path_invalid'
  | 'model_path_conflict'
  | 'model_unavailable_required'
  | 'inference_quota_invalid'
  | 'event_topic_invalid'
  | 'event_topic_duplicate'
  | 'video_stream_unavailable'
  | 'network_mode_invalid'
  | 'network_inbound_invalid'
  | 'network_inbound_duplicate'
  | 'network_inbound_requires_host'
  | 'host_network_warning'
  | 'dynamic_model_registration_warning'
  | 'env_name_invalid'
  | 'env_name_duplicate'
  | 'volume_path_invalid'
  | 'volume_destination_duplicate'
  | 'restart_policy_invalid'
  | 'writable_rootfs_warning';

export type InstallIssueSeverity = 'error' | 'warning';

export type InstallIssueField =
  | 'metadata.id'
  | 'metadata.name'
  | 'metadata.version'
  | 'image'
  | 'resources.cpu'
  | 'resources.memory'
  | 'models'
  | 'permissions.inference'
  | 'permissions.events'
  | 'permissions.video'
  | 'permissions.network'
  | 'env'
  | 'volumes'
  | 'restart_policy'
  | 'security';

export interface InstallIssue {
  /** The form page the problem belongs to — jump here on failure. */
  section: ImportSectionId;
  /** i18n key suffix under `sys.apps.import.` for the toast message. */
  reason: InstallIssueReason;
  /** Exact form control/group to annotate inline. */
  field: InstallIssueField;
  /** Errors block install; warnings require explicit confirmation. */
  severity: InstallIssueSeverity;
}

const SAFE_APP_ID = /^[A-Za-z0-9][A-Za-z0-9._-]*$/;
const ENV_NAME = /^[A-Za-z_][A-Za-z0-9_]*$/;
const RESERVED_MODEL_ALIASES = new Set([
  'HOST_PREFIX',
  'APP_ID',
  'APP_ROLE',
  'CONTAINER_NAME',
]);

function isAbsoluteCleanPath(value: string): boolean {
  if (!value.startsWith('/') || value.includes('//')) return false;
  return !value.split('/').some(part => part === '.' || part === '..');
}

function parseCPUQuota(value: string | undefined): number | null {
  const raw = value?.trim();
  if (!raw) return null;
  const percent = raw.match(/^([+]?(?:\d+(?:\.\d*)?|\.\d+))%$/);
  const parsed = Number(percent ? percent[1] : raw);
  if (!Number.isFinite(parsed) || parsed <= 0) return Number.NaN;
  return percent ? parsed / 100 : parsed;
}

function parseMemoryBytes(value: string | undefined): number | null {
  const raw = value?.trim();
  if (!raw) return null;
  const match = raw.match(/^(\d+)(Ki|Mi|Gi|K|M|G)?$/);
  if (!match) return Number.NaN;
  const multipliers: Record<string, number> = {
    '': 1,
    K: 1000,
    M: 1000 ** 2,
    G: 1000 ** 3,
    Ki: 1024,
    Mi: 1024 ** 2,
    Gi: 1024 ** 3,
  };
  const bytes = Number(match[1]) * multipliers[match[2] ?? ''];
  return Number.isSafeInteger(bytes) && bytes > 0 ? bytes : Number.NaN;
}

function hasDuplicate<T>(values: T[]): boolean {
  return new Set(values).size !== values.length;
}

/**
 * Full validation run before install. The form reports the first issue
 * back to its page.
 */
export function collectInstallIssues(
  config: WizardConfig,
  opts: {
    sourceType: 'registry' | 'local';
    /** Registry image ref already valid, or a local file uploaded. */
    sourceReady: boolean;
    /**
     * Model ids the device currently knows (runtime + platform DB). Passed
     * as undefined while the model list query is loading/unavailable — the
     * availability check is skipped then (the backend still fast-fails at
     * install time), instead of flagging every dependency as missing.
     */
    availableModelIds?: string[];
    /** Stream ids currently exposed by the device; undefined skips warning. */
    availableStreamIds?: string[];
  }
): InstallIssue[] {
  const issues: InstallIssue[] = [];

  const add = (
    section: ImportSectionId,
    field: InstallIssueField,
    reason: InstallIssueReason,
    severity: InstallIssueSeverity = 'error'
  ) => issues.push({ section, field, reason, severity });

  if (!config.metadata?.id?.trim()) {
    add('basic_info', 'metadata.id', 'app_id_required');
  } else if (!SAFE_APP_ID.test(config.metadata.id.trim())) {
    add('basic_info', 'metadata.id', 'app_id_invalid');
  }
  if (!config.metadata?.name?.trim()) {
    add('basic_info', 'metadata.name', 'app_name_required');
  }
  if (!config.metadata?.version?.trim()) {
    add('basic_info', 'metadata.version', 'app_version_required');
  }

  if (opts.sourceType === 'registry') {
    if (!isValidContainerImageRef((config.image || '').trim())) {
      // No review page anymore — the image ref lives on 基础信息.
      add('basic_info', 'image', 'invalid_image_ref');
    }
  } else if (!opts.sourceReady) {
    add('basic_info', 'image', 'local_source_required');
  }

  const cpuQuota = parseCPUQuota(config.resources?.cpu);
  if (Number.isNaN(cpuQuota)) {
    add('resources', 'resources.cpu', 'cpu_limit_invalid');
  } else if (cpuQuota != null && cpuQuota > 1) {
    add('resources', 'resources.cpu', 'cpu_limit_high', 'warning');
  }
  const memoryBytes = parseMemoryBytes(config.resources?.memory);
  if (Number.isNaN(memoryBytes)) {
    add('resources', 'resources.memory', 'memory_limit_invalid');
  } else if (memoryBytes != null && memoryBytes > 4 * 1024 ** 3) {
    add('resources', 'resources.memory', 'memory_limit_high', 'warning');
  }

  // Model dependencies (spec.models): a draft row sits on the '' alias until
  // named, and a row without a selected model cannot install. Reported at
  // most once per reason — the toast shows the first issue only.
  const models = config.models ?? {};
  if (Object.keys(models).some(alias => !isValidModelAlias(alias))) {
    add('models', 'models', 'invalid_model_alias');
  }
  if (Object.keys(models).some(alias => RESERVED_MODEL_ALIASES.has(alias))) {
    add('models', 'models', 'reserved_model_alias');
  }
  if (Object.values(models).some(m => !m?.id?.trim())) {
    add('models', 'models', 'model_id_required');
  }
  // Mirror of the backend manifest validation: a declared bundling path must
  // point at an AMPK model package — bare .hef bundling is no longer
  // supported on the install side, so it must not pass the form either.
  if (
    Object.values(models).some(m => {
      const p = m?.path?.trim();
      return !!p && (!isAbsoluteCleanPath(p) || !p.endsWith('.bin'));
    })
  ) {
    add('models', 'models', 'model_path_invalid');
  }
  const pathByModel = new Map<string, string>();
  let pathConflict = false;
  for (const mapping of Object.values(models)) {
    const id = mapping?.id?.trim();
    if (!id) continue;
    const path = mapping.path?.trim() ?? '';
    if (pathByModel.has(id) && pathByModel.get(id) !== path) {
      pathConflict = true;
      break;
    }
    pathByModel.set(id, path);
  }
  if (pathConflict) {
    add('models', 'models', 'model_path_conflict');
  }

  // Mirror of the backend resolveModelDependencies fast-fail: a required
  // dependency whose id is neither on the device nor declared with a bundled
  // path fails the install before the image pull — surface it in the form
  // instead of letting the user discover it at install time. Optional misses
  // only warn server-side, so they stay installable here.
  if (opts.availableModelIds) {
    const known = new Set(opts.availableModelIds);
    if (
      Object.values(models).some(
        m => !!m?.id?.trim()
          && m.required
          && !m.path?.trim()
          && !known.has(m.id.trim())
      )
    ) {
      add('models', 'models', 'model_unavailable_required');
    }
  }

  const inference = config.permissions?.inference;
  if (
    (inference?.max_qps != null && inference.max_qps < 0)
    || (inference?.max_concurrent != null && inference.max_concurrent < 0)
  ) {
    add('models', 'permissions.inference', 'inference_quota_invalid');
  }
  if (inference?.allow_register_model) {
    add(
      'models',
      'permissions.inference',
      'dynamic_model_registration_warning',
      'warning'
    );
  }

  const events = config.permissions?.events;
  const topics = [...(events?.publish ?? []), ...(events?.subscribe ?? [])];
  if (topics.some(topic => !topic.trim())) {
    add('permissions', 'permissions.events', 'event_topic_invalid');
  }
  if (
    hasDuplicate(events?.publish ?? [])
    || hasDuplicate(events?.subscribe ?? [])
  ) {
    add('permissions', 'permissions.events', 'event_topic_duplicate');
  }

  const network = config.permissions?.network;
  const mode = network?.mode ?? '';
  if (mode !== '' && mode !== 'isolated' && mode !== 'host') {
    add('permissions', 'permissions.network', 'network_mode_invalid');
  }
  const inbound = network?.inbound ?? [];
  if (
    inbound.some(port => !Number.isInteger(port) || port < 1 || port > 65535)
  ) {
    add('permissions', 'permissions.network', 'network_inbound_invalid');
  }
  if (hasDuplicate(inbound)) {
    add('permissions', 'permissions.network', 'network_inbound_duplicate');
  }
  if (mode !== 'host' && inbound.length > 0) {
    add('permissions', 'permissions.network', 'network_inbound_requires_host');
  }
  if (mode === 'host') {
    add(
      'permissions',
      'permissions.network',
      'host_network_warning',
      'warning'
    );
  }

  if (opts.availableStreamIds) {
    const knownStreams = new Set(opts.availableStreamIds);
    if ((config.permissions?.video ?? []).some(id => !knownStreams.has(id))) {
      add(
        'permissions',
        'permissions.video',
        'video_stream_unavailable',
        'warning'
      );
    }
  }

  const env = config.env ?? [];
  if (env.some(item => !ENV_NAME.test(item.name))) {
    add('advanced', 'env', 'env_name_invalid');
  }
  if (hasDuplicate(env.map(item => item.name))) {
    add('advanced', 'env', 'env_name_duplicate');
  }

  const volumes = config.volumes ?? [];
  if (
    volumes.some(
      volume => !isAbsoluteCleanPath(volume.host)
        || !isAbsoluteCleanPath(volume.container)
    )
  ) {
    add('advanced', 'volumes', 'volume_path_invalid');
  }
  if (hasDuplicate(volumes.map(volume => volume.container))) {
    add('advanced', 'volumes', 'volume_destination_duplicate');
  }

  const restart = config.restart_policy?.trim().toLowerCase() ?? '';
  if (
    !['', 'always', 'on-failure', 'on_failure', 'no', 'never'].includes(restart)
  ) {
    add('advanced', 'restart_policy', 'restart_policy_invalid');
  }
  if (config.security?.readonly_rootfs === false) {
    add('advanced', 'security', 'writable_rootfs_warning', 'warning');
  }

  return issues;
}

/** Compatibility helper for callers interested only in blocking issues. */
export function collectInstallErrors(
  config: WizardConfig,
  opts: Parameters<typeof collectInstallIssues>[1]
): InstallIssue[] {
  return collectInstallIssues(config, opts).filter(
    issue => issue.severity === 'error'
  );
}
