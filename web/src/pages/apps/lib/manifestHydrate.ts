import type { AppManifestDTO, WizardConfig } from '@/services/types';

/**
 * mode-3 (upload app.yaml) hydration: the server returns the full parsed
 * manifest, these pure functions map it onto the wizard form state and back
 * into PATCH-able field edits. Kept pure so it can be tested without React.
 */

/**
 * Deterministic JSON-ish string: objects get keys sorted recursively, arrays
 * keep order (order changes in arrays are real changes). Two configs that
 * differ only in key insertion order canonicalize identically, so spread-style
 * setConfig updates never report a false dirty.
 */
export function canonicalize(value: unknown): string {
  if (Array.isArray(value)) {
    return `[${value.map(canonicalize).join(',')}]`;
  }
  if (value !== null && typeof value === 'object') {
    const record = value as Record<string, unknown>;
    const body = Object.keys(record)
      .filter(key => record[key] !== undefined)
      .sort()
      .map(key => `${JSON.stringify(key)}:${canonicalize(record[key])}`)
      .join(',');
    return `{${body}}`;
  }
  return JSON.stringify(value) ?? 'null';
}

/** True when the current wizard config differs from the hydrated snapshot. */
export function isDirty(
  current: WizardConfig,
  hydrated: WizardConfig | null
): boolean {
  if (!hydrated) return false;
  return canonicalize(current) !== canonicalize(hydrated);
}

/**
 * Map a parsed manifest onto the wizard form. Fields the manifest omits stay
 * undefined (not defaulted) so an untouched walk-through of the wizard is
 * byte-identical to the uploaded file — defaults would fake edits.
 *
 * The Go backend marshals nil slices/maps/pointers as JSON `null`, so absent
 * fields arrive as null rather than undefined; the guards below treat the
 * two the same.
 */
export function manifestToWizardConfig(m: AppManifestDTO): WizardConfig {
  const spec = m.spec ?? ({} as AppManifestDTO['spec']);
  const perms = spec.permissions ?? {};
  const inf = perms.inference ?? {};
  const events = perms.events ?? {};
  const device = perms.device ?? {};
  const network = perms.network ?? {};
  const meta = m.metadata;

  return {
    metadata: {
      id: meta?.id ?? '',
      name: meta?.name ?? '',
      version: meta?.version ?? '',
      description: meta?.description ?? '',
    },
    image: spec.image ?? '',
    ...(spec.models != null
      && typeof spec.models === 'object' && {
        models: Object.fromEntries(
          Object.entries(spec.models).map(([alias, mapping]) => [
            alias,
            { ...mapping },
          ])
        ),
      }),
    resources: {
      ...(spec.resources?.cpu != null && { cpu: spec.resources.cpu }),
      ...(spec.resources?.memory != null && {
        memory: spec.resources.memory,
      }),
    },
    permissions: {
      ...(Array.isArray(perms.video) && { video: [...perms.video] }),
      inference: {
        // models deliberately NOT hydrated here: the wizard's model editor is
        // rebound to spec.models (declarative dependencies). The legacy
        // permissions.inference.models authorization list is no longer form
        // state — PATCH never writes it, so it survives untouched on disk.
        ...(inf.max_qps != null && { max_qps: inf.max_qps }),
        ...(inf.max_concurrent != null && {
          max_concurrent: inf.max_concurrent,
        }),
        ...(inf.allow_register_model != null && {
          allow_register_model: inf.allow_register_model,
        }),
      },
      events: {
        ...(Array.isArray(events.publish) && {
          publish: [...events.publish],
        }),
        ...(Array.isArray(events.subscribe) && {
          subscribe: [...events.subscribe],
        }),
      },
      device: {
        ...(device.light != null && { light: device.light }),
        ...(device.ir_cut != null && { ir_cut: device.ir_cut }),
        ...(device.ptz != null && { ptz: device.ptz }),
        ...(device.lens != null && { lens: device.lens }),
      },
      network: {
        ...(network.mode != null && { mode: network.mode }),
        ...(Array.isArray(network.inbound) && {
          inbound: [...network.inbound],
        }),
      },
    },
    ...(Array.isArray(spec.env) && { env: spec.env.map(e => ({ ...e })) }),
    ...(Array.isArray(spec.volumes) && {
      volumes: spec.volumes.map(v => ({ ...v })),
    }),
    ...(spec.autostart != null && { autostart: spec.autostart }),
    ...(spec.restart_policy != null && {
      restart_policy: spec.restart_policy,
    }),
    ...(spec.security != null && {
      security: {
        ...(typeof spec.security.no_new_privileges === 'boolean' && {
          no_new_privileges: spec.security.no_new_privileges,
        }),
        ...(typeof spec.security.readonly_rootfs === 'boolean' && {
          readonly_rootfs: spec.security.readonly_rootfs,
        }),
      },
    }),
  };
}

/**
 * Build PATCH /apps/manifest field edits from the wizard config. Only
 * server-whitelisted paths are emitted; metadata.id (directory binding) and
 * image (tar RepoTag reconciliation) are excluded by construction. Undefined
 * fields are omitted so an untouched config produces an empty object — the
 * caller installs the original file without patching.
 */
export function wizardConfigToPatchFields(
  config: WizardConfig
): Record<string, unknown> {
  const fields: Record<string, unknown> = {};
  const set = (path: string, value: unknown) => {
    if (value !== undefined) fields[path] = value;
  };

  set('metadata.name', config.metadata?.name);
  set('metadata.version', config.metadata?.version);
  // Empty descriptions are omitted rather than written as "" so a manifest
  // without a description does not grow an empty field.
  if (config.metadata?.description) {
    set('metadata.description', config.metadata.description);
  }

  set('spec.resources.cpu', config.resources?.cpu);
  set('spec.resources.memory', config.resources?.memory);
  set('spec.autostart', config.autostart);
  set('spec.restart_policy', config.restart_policy);

  // Model dependencies replace the whole spec.models map in one op. Always
  // emitted — null (not undefined) clears it, which is the only way a
  // dependency removal is detectable by changedPatchFields (set() skips
  // undefined, and an absent key cannot be diffed against a present one).
  fields['spec.models'] =    config.models && Object.keys(config.models).length > 0
      ? config.models
      : null;

  set('spec.permissions.video', config.permissions?.video);
  set(
    'spec.permissions.inference.max_qps',
    config.permissions?.inference?.max_qps
  );
  set(
    'spec.permissions.inference.max_concurrent',
    config.permissions?.inference?.max_concurrent
  );
  set(
    'spec.permissions.inference.allow_register_model',
    config.permissions?.inference?.allow_register_model
  );
  set('spec.permissions.events.publish', config.permissions?.events?.publish);
  set(
    'spec.permissions.events.subscribe',
    config.permissions?.events?.subscribe
  );
  set('spec.permissions.device.light', config.permissions?.device?.light);
  set('spec.permissions.device.ir_cut', config.permissions?.device?.ir_cut);
  set('spec.permissions.device.ptz', config.permissions?.device?.ptz);
  set('spec.permissions.device.lens', config.permissions?.device?.lens);
  set('spec.permissions.network.mode', config.permissions?.network?.mode);
  set('spec.permissions.network.inbound', config.permissions?.network?.inbound);

  set('spec.env', config.env);
  set('spec.volumes', config.volumes);

  set('spec.security.no_new_privileges', config.security?.no_new_privileges);
  set('spec.security.readonly_rootfs', config.security?.readonly_rootfs);

  return fields;
}

/**
 * The effective edit set: fields whose value actually changed relative to the
 * hydrated snapshot. Drives both the "N edits" review hint and the PATCH body
 * (patching only what changed keeps the diff on disk minimal).
 */
export function changedPatchFields(
  current: WizardConfig,
  hydrated: WizardConfig | null
): Record<string, unknown> {
  if (!hydrated) return {};
  const all = wizardConfigToPatchFields(current);
  const before = wizardConfigToPatchFields(hydrated);
  const changed: Record<string, unknown> = {};
  for (const [path, value] of Object.entries(all)) {
    if (canonicalize(before[path]) !== canonicalize(value)) {
      changed[path] = value;
    }
  }
  return changed;
}

/**
 * Overlay of PATCH-style field edits onto a wizard config — the flush-merge
 * half of `changedPatchFields`. When the YAML editor applies a new manifest
 * version the form is re-hydrated from it, but unflushed form edits (captured
 * as changed fields relative to the previous snapshot) must survive as
 * overrides on top. Paths use the PATCH vocabulary (`spec.`-prefixed manifest
 * paths); the `spec.` segment maps onto the config root, `metadata.*` maps
 * onto config.metadata unchanged. Returns a new config; base is not mutated.
 */
export function applyPatchFields(
  base: WizardConfig,
  fields: Record<string, unknown>
): WizardConfig {
  const merged = clonePlain(base) as unknown as Record<string, unknown>;
  for (const [path, value] of Object.entries(fields)) {
    const segments = path.split('.');
    if (segments[0] === 'spec') segments.shift();
    if (segments.length === 0) continue;

    let node = merged;
    for (let i = 0; i < segments.length - 1; i += 1) {
      const key = segments[i];
      const child = node[key];
      node[key] = child !== null && typeof child === 'object' ? child : {};
      node = node[key] as Record<string, unknown>;
    }
    const last = segments[segments.length - 1];
    if (value === null) {
      // null is the wire format for "clear this field" — the config treats
      // absence as canonical (matches the backend's null-deletes-key patch
      // semantics).
      delete node[last];
    } else {
      node[last] = value;
    }
  }
  return merged as unknown as WizardConfig;
}

/** Recursive clone for the JSON-safe shapes the wizard config uses. */
function clonePlain<T>(value: T): T {
  if (Array.isArray(value)) {
    return value.map(item => clonePlain(item)) as unknown as T;
  }
  if (value !== null && typeof value === 'object') {
    const out: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(value as Record<string, unknown>)) {
      if (v !== undefined) out[k] = clonePlain(v);
    }
    return out as unknown as T;
  }
  return value;
}
