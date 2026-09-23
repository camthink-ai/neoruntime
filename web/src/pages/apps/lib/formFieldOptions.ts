/**
 * Select option lists for the import form's Radix dropdowns.
 *
 * Manifest values are free-form server-side (any `N[.M]Ki|Mi|Gi|Ti` memory,
 * any restart_policy string the backend accepts), but the dropdowns offer a
 * fixed preset list. Radix `SelectValue` renders EMPTY for a value with no
 * matching `SelectItem`, so an out-of-preset manifest value (e.g. 64Mi /
 * "never") would display as a blank dropdown. These pure helpers extend the
 * presets with the current value so it stays visible and selectable.
 */

export const MEMORY_PRESETS = [
  '128Mi',
  '256Mi',
  '512Mi',
  '1Gi',
  '2Gi',
] as const;

export const RESTART_POLICY_PRESETS = ['no', 'on-failure', 'always'] as const;

/** Byte size of a Kubernetes-style memory string ("64Mi", "1.5Gi", "1024").
 * Case-insensitive; unparseable input yields NaN. */
export function memoryToBytes(value: string): number {
  const m = value.trim().match(/^(\d+(?:\.\d+)?)\s*(Ki|Mi|Gi|Ti)?$/i);
  if (!m) return Number.NaN;
  const units: Record<string, number> = {
    '': 1,
    ki: 1024,
    mi: 1024 ** 2,
    gi: 1024 ** 3,
    ti: 1024 ** 4,
  };
  return parseFloat(m[1]) * units[(m[2] ?? '').toLowerCase()];
}

/** Memory presets extended with the manifest's current value, inserted in
 * ascending byte order; unparseable values are appended at the end. */
export function memoryOptionsFor(value: string | undefined): string[] {
  const options: string[] = [...MEMORY_PRESETS];
  if (!value || options.includes(value)) return options;
  const bytes = memoryToBytes(value);
  if (Number.isNaN(bytes)) return [...options, value];
  const at = options.findIndex(opt => memoryToBytes(opt) > bytes);
  if (at === -1) return [...options, value];
  options.splice(at, 0, value);
  return options;
}

/** Restart presets extended with an out-of-preset policy appended at the
 * end (rendered with its raw value — it reflects what the file says). */
export function restartPoliciesFor(value: string | undefined): string[] {
  const presets: string[] = [...RESTART_POLICY_PRESETS];
  if (!value || presets.includes(value)) return presets;
  return [...presets, value];
}
