export type ModelVariantKind = 'default' | 'preset' | 'custom' | 'invalid';

export interface ModelVariantDisplay {
  kind: ModelVariantKind;
  /** Trimmed stored value; empty for the platform-default state. */
  value: string;
}

/**
 * Classify the stored model_variant for presentation.
 *
 * The runtime treats a `{`-leading value as a full postprocess override and
 * everything else as a named variant/profile. Keep the UI classification on
 * that same boundary so it never describes a value differently from the
 * loader. Invalid override JSON is surfaced explicitly instead of dumping the
 * malformed payload into model cards and tables.
 */
export function classifyModelVariant(
  variant: string | null | undefined
): ModelVariantDisplay {
  const value = variant?.trim() ?? '';
  if (!value) return { kind: 'default', value: '' };
  if (!value.startsWith('{')) return { kind: 'preset', value };

  try {
    const parsed: unknown = JSON.parse(value);
    if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
      return { kind: 'custom', value };
    }
  } catch {
    // Fall through to the explicit invalid state.
  }
  return { kind: 'invalid', value };
}
