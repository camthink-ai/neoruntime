import { parseDocument, type Document } from 'yaml';
import type { WizardConfig } from '@/services/types';
import { manifestToWizardConfig } from './manifestHydrate';

/**
 * Real-time form ↔ YAML synchronization on the uploaded app.yaml text.
 *
 * The manifest file text is kept as an editable document (comments, unknown
 * fields and key order survive), and form state is projected onto it by
 * editing only the managed paths — the same whitelist the server PATCH
 * accepts. The reverse direction parses the text client-side; the server
 * stays authoritative at flush/install time.
 */

export type YamlSyncResult = { text: string } | { error: string };

/**
 * Recursively drop `undefined`-valued keys so optional form fields (e.g.
 * `path: undefined` on a model mapping) are omitted instead of serialized
 * as `path:` nulls.
 */
const stripUndefined = (value: unknown): unknown => {
  if (Array.isArray(value)) return value.map(stripUndefined);
  if (value && typeof value === 'object') {
    const out: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(value as Record<string, unknown>)) {
      if (v !== undefined) out[k] = stripUndefined(v);
    }
    return out;
  }
  return value;
};

const docErrors = (doc: Document): string[] => doc.errors.map(e => String(e.message || e));

/**
 * Project the wizard form state onto the YAML text via AST editing.
 * Only managed paths are touched; comments, unknown fields and formatting
 * elsewhere in the document are preserved byte-for-byte.
 */
export function applyConfigToYamlText(
  text: string,
  config: WizardConfig
): YamlSyncResult {
  const doc = parseDocument(text);
  const errors = docErrors(doc);
  if (errors.length > 0) return { error: errors[0] };

  /** Set a value, or remove the key when the form has nothing for it. */
  const setOrDelete = (path: string[], value: unknown) => {
    if (value === undefined || value === null) {
      // deleteIn throws when an intermediate collection is absent — that
      // just means there is nothing to remove.
      try {
        doc.deleteIn(path);
      } catch {
        /* absent path — no-op */
      }
    } else {
      doc.setIn(path, stripUndefined(value));
    }
  };
  const setOrDeleteWhenEmpty = (path: string[], value: unknown) => {
    const isEmpty =      value === undefined
      || value === null
      || (Array.isArray(value) && value.length === 0);
    setOrDelete(path, isEmpty ? undefined : value);
  };

  setOrDelete(['metadata', 'name'], config.metadata?.name || undefined);
  setOrDelete(['metadata', 'version'], config.metadata?.version || undefined);
  // Empty descriptions are omitted rather than written as "" so a manifest
  // without a description does not grow an empty field.
  setOrDelete(
    ['metadata', 'description'],
    config.metadata?.description || undefined
  );

  setOrDelete(['spec', 'image'], config.image || undefined);

  const models =    config.models && Object.keys(config.models).length > 0
      ? config.models
      : undefined;
  setOrDelete(['spec', 'models'], models);

  setOrDelete(['spec', 'resources', 'cpu'], config.resources?.cpu);
  setOrDelete(['spec', 'resources', 'memory'], config.resources?.memory);
  setOrDelete(['spec', 'autostart'], config.autostart);
  setOrDelete(['spec', 'restart_policy'], config.restart_policy);

  const perms = config.permissions;
  setOrDeleteWhenEmpty(['spec', 'permissions', 'video'], perms?.video);
  setOrDelete(
    ['spec', 'permissions', 'inference', 'max_qps'],
    perms?.inference?.max_qps
  );
  setOrDelete(
    ['spec', 'permissions', 'inference', 'max_concurrent'],
    perms?.inference?.max_concurrent
  );
  setOrDelete(
    ['spec', 'permissions', 'inference', 'allow_register_model'],
    perms?.inference?.allow_register_model
  );
  setOrDeleteWhenEmpty(
    ['spec', 'permissions', 'events', 'publish'],
    perms?.events?.publish
  );
  setOrDeleteWhenEmpty(
    ['spec', 'permissions', 'events', 'subscribe'],
    perms?.events?.subscribe
  );
  setOrDelete(['spec', 'permissions', 'device', 'light'], perms?.device?.light);
  setOrDelete(
    ['spec', 'permissions', 'device', 'ir_cut'],
    perms?.device?.ir_cut
  );
  setOrDelete(['spec', 'permissions', 'device', 'ptz'], perms?.device?.ptz);
  setOrDelete(['spec', 'permissions', 'device', 'lens'], perms?.device?.lens);
  setOrDelete(['spec', 'permissions', 'network', 'mode'], perms?.network?.mode);
  setOrDeleteWhenEmpty(
    ['spec', 'permissions', 'network', 'inbound'],
    perms?.network?.inbound
  );

  setOrDeleteWhenEmpty(['spec', 'env'], config.env);
  setOrDeleteWhenEmpty(['spec', 'volumes'], config.volumes);

  setOrDelete(
    ['spec', 'security', 'no_new_privileges'],
    config.security?.no_new_privileges
  );
  setOrDelete(
    ['spec', 'security', 'readonly_rootfs'],
    config.security?.readonly_rootfs
  );

  return { text: doc.toString() };
}

/**
 * Parse edited YAML text into wizard form state (client-side mirror of the
 * upload-manifest hydration). A parse failure or a non-object document is
 * an error — callers keep the last good form state and surface the message.
 */
export function parseYamlToConfig(
  text: string
): { config: WizardConfig } | { error: string } {
  if (!text.trim()) return { error: 'document is empty' };
  let doc: Document;
  try {
    doc = parseDocument(text);
  } catch (e) {
    return { error: String((e as Error)?.message || e) };
  }
  const errors = docErrors(doc);
  if (errors.length > 0) return { error: errors[0] };

  let js: unknown;
  try {
    js = doc.toJS();
  } catch (e) {
    return { error: String((e as Error)?.message || e) };
  }
  if (!js || typeof js !== 'object' || Array.isArray(js)) {
    return { error: 'document is not a YAML mapping' };
  }
  return { config: manifestToWizardConfig(js as never) };
}
