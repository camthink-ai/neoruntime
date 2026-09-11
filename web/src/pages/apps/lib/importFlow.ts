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
  | 'app_name_required'
  | 'invalid_image_ref'
  | 'local_source_required'
  | 'invalid_model_alias'
  | 'model_id_required'
  | 'model_path_invalid'
  | 'model_unavailable_required';

export interface InstallIssue {
  /** The form page the problem belongs to — jump here on failure. */
  section: ImportSectionId;
  /** i18n key suffix under `sys.apps.import.` for the toast message. */
  reason: InstallIssueReason;
}

/**
 * Full validation run before install. The form reports the first issue
 * back to its page.
 */
export function collectInstallErrors(
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
  }
): InstallIssue[] {
  const issues: InstallIssue[] = [];

  if (!config.metadata?.id?.trim()) {
    issues.push({ section: 'basic_info', reason: 'app_id_required' });
  }
  if (!config.metadata?.name?.trim()) {
    issues.push({ section: 'basic_info', reason: 'app_name_required' });
  }

  if (opts.sourceType === 'registry') {
    if (!isValidContainerImageRef((config.image || '').trim())) {
      // No review page anymore — the image ref lives on 基础信息.
      issues.push({ section: 'basic_info', reason: 'invalid_image_ref' });
    }
  } else if (!opts.sourceReady) {
    issues.push({ section: 'basic_info', reason: 'local_source_required' });
  }

  // Model dependencies (spec.models): a draft row sits on the '' alias until
  // named, and a row without a selected model cannot install. Reported at
  // most once per reason — the toast shows the first issue only.
  const models = config.models ?? {};
  if (Object.keys(models).some(alias => !isValidModelAlias(alias))) {
    issues.push({ section: 'models', reason: 'invalid_model_alias' });
  }
  if (Object.values(models).some(m => !m?.id?.trim())) {
    issues.push({ section: 'models', reason: 'model_id_required' });
  }
  // Mirror of the backend manifest validation: a declared bundling path must
  // point at an AMPK model package — bare .hef bundling is no longer
  // supported on the install side, so it must not pass the form either.
  if (
    Object.values(models).some(m => {
      const p = m?.path?.trim();
      return !!p && !p.toLowerCase().endsWith('.bin');
    })
  ) {
    issues.push({ section: 'models', reason: 'model_path_invalid' });
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
      issues.push({ section: 'models', reason: 'model_unavailable_required' });
    }
  }

  return issues;
}
