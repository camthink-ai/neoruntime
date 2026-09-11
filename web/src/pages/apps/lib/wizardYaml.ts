import type { WizardConfig } from '@/services/types';

/**
 * YAML preview for the wizard: a hand-rolled deterministic emitter for the
 * wizard-expressible manifest subset. Used by the YAML view when no app.yaml
 * was uploaded (registry / tar-only sources) — the editable YAML path
 * round-trips through upload-manifest instead, so this output is display and
 * copy only, never parsed back by the client.
 */

type YamlScalar = string | number | boolean;
type YamlValue =
  | YamlScalar
  | YamlValue[]
  | { [key: string]: YamlValue | undefined };

/** Values the YAML parser would read back as a non-string, or that break plain style. */
function needsQuotes(s: string): boolean {
  if (s === '') return true;
  if (/^\s|\s$/.test(s)) return true;
  // reads back as bool / null
  if (/^(?:true|false|null|yes|no|on|off|~)$/i.test(s)) return true;
  // reads back as a number (decimal, exponent, hex)
  if (!Number.isNaN(Number(s)) && s.trim() !== '') return true;
  if (/^0x[0-9a-fA-F]+$/.test(s)) return true;
  // indicator characters that are unsafe at the start or anywhere structural
  if (/^[-?:,[\]{}#&*!|>'"%@`]/.test(s)) return true;
  if (s.includes(': ') || s.includes(' #') || /[\n\r\t]/.test(s)) return true;
  return false;
}

function emitScalar(value: YamlScalar): string {
  if (typeof value === 'string' && needsQuotes(value)) {
    // JSON string escaping is valid YAML double-quoted scalar syntax.
    return JSON.stringify(value);
  }
  return String(value);
}

function isPlainObject(
  value: YamlValue
): value is { [key: string]: YamlValue | undefined } {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function emitMapping(
  obj: { [key: string]: YamlValue | undefined },
  indent: number
): string[] {
  const lines: string[] = [];
  for (const [key, value] of Object.entries(obj)) {
    if (value === undefined) continue;
    lines.push(...emitEntry(key, value, indent));
  }
  return lines;
}

function emitEntry(key: string, value: YamlValue, indent: number): string[] {
  const pad = '  '.repeat(indent);

  if (Array.isArray(value)) {
    if (value.length === 0) return [];
    const lines = [`${pad}${key}:`];
    for (const item of value) {
      if (isPlainObject(item)) {
        // env / volumes shape: inline first key ("- name: K"), the rest
        // aligned under it.
        const entries = Object.entries(item).filter(
          (entry): entry is [string, YamlValue] => entry[1] !== undefined
        );
        if (entries.length === 0) continue;
        const [firstKey, firstValue] = entries[0];
        if (isPlainObject(firstValue) || Array.isArray(firstValue)) {
          lines.push(`${pad}  -`);
          lines.push(...emitEntry(firstKey, firstValue, indent + 2));
        } else {
          lines.push(`${pad}  - ${firstKey}: ${emitScalar(firstValue)}`);
        }
        lines.push(
          ...emitMapping(Object.fromEntries(entries.slice(1)), indent + 2)
        );
      } else if (Array.isArray(item)) {
        // nested plain arrays do not occur in the manifest subset
        lines.push(`${pad}  -`);
        lines.push(...emitEntry('item', item, indent + 2));
      } else {
        lines.push(`${pad}  - ${emitScalar(item)}`);
      }
    }
    return lines;
  }

  if (isPlainObject(value)) {
    const inner = emitMapping(value, indent + 1);
    if (inner.length === 0) return [];
    return [`${pad}${key}:`, ...inner];
  }

  return [`${pad}${key}: ${emitScalar(value)}`];
}

/**
 * Deterministic YAML text for the current wizard config. Key order follows
 * the AppManifestDTO schema; undefined fields, empty arrays and objects that
 * prune down to nothing are omitted, so the preview only shows what the form
 * actually carries.
 */
export function wizardConfigToYaml(config: WizardConfig): string {
  const c = config ?? ({} as WizardConfig);
  const perms = c.permissions;

  const tree = {
    apiVersion: 'v1',
    kind: 'Application',
    metadata: {
      id: c.metadata?.id ?? '',
      name: c.metadata?.name ?? '',
      version: c.metadata?.version ?? '',
      ...(c.metadata?.description
        ? { description: c.metadata.description }
        : {}),
    },
    spec: {
      ...(c.image ? { image: c.image } : {}),
      models: c.models,
      resources: {
        cpu: c.resources?.cpu,
        memory: c.resources?.memory,
      },
      permissions: {
        video: perms?.video,
        inference: {
          max_qps: perms?.inference?.max_qps,
          max_concurrent: perms?.inference?.max_concurrent,
          allow_register_model: perms?.inference?.allow_register_model,
        },
        events: {
          publish: perms?.events?.publish,
          subscribe: perms?.events?.subscribe,
        },
        device: {
          light: perms?.device?.light,
          ir_cut: perms?.device?.ir_cut,
          ptz: perms?.device?.ptz,
          lens: perms?.device?.lens,
        },
        network: {
          mode: perms?.network?.mode,
          inbound: perms?.network?.inbound,
        },
      },
      env: c.env?.map(e => ({ name: e.name, value: e.value })),
      volumes: c.volumes?.map(v => ({
        host: v.host,
        container: v.container,
        readonly: v.readonly,
      })),
      autostart: c.autostart,
      restart_policy: c.restart_policy,
      security: {
        no_new_privileges: c.security?.no_new_privileges,
        readonly_rootfs: c.security?.readonly_rootfs,
      },
    },
  };

  const lines = emitMapping(tree, 0);
  return lines.length > 0 ? `${lines.join('\n')}\n` : '';
}

/**
 * Which mode the YAML view runs in: a real editable file only exists for
 * local imports with an uploaded app.yaml; every other source gets the
 * generated read-only preview.
 */
export function resolveYamlViewMode(input: {
  sourceType: 'registry' | 'local';
  hasManifest: boolean;
}): 'editable' | 'preview' {
  return input.sourceType === 'local' && input.hasManifest
    ? 'editable'
    : 'preview';
}
