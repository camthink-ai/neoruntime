import { render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import type { ModelFieldDef } from '@/hooks/useModels';
import FieldHelp from './FieldHelp';
import ModelSchemaField from './ModelSchemaField';

// Only the keys the tests assert on; everything else takes the caller's
// fallback (non-empty string) or '' — mirroring i18next's missing-key
// behavior that hides the marker entirely.
const strings: Record<string, string> = {
  'sys.ai_models.form.temperature_tooltip': 'randomness of generation',
  'sys.ai_models.form.field_help_aria': 'Explain this field',
};

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, opts?: unknown) => {
      if (strings[key] !== undefined) return strings[key];
      if (typeof opts === 'string') return opts;
      return '';
    },
  }),
}));

describe('FieldHelp', () => {
  it('renders no marker when the field has no tooltip entry', () => {
    const { container } = render(<FieldHelp fieldKey="top_k" />);
    expect(container.querySelector('button')).toBeNull();
  });

  it('renders a labeled trigger when the tooltip entry exists', () => {
    render(<FieldHelp fieldKey="temperature" />);
    expect(
      screen.getByRole('button', { name: 'Explain this field' })
    ).toBeInTheDocument();
  });
});

describe('ModelSchemaField help marker integration', () => {
  const noop = () => {};

  it('shows the marker next to the label for a field with a tooltip', () => {
    const field: ModelFieldDef = { key: 'temperature', type: 'number' };
    render(
      <ModelSchemaField
        field={field}
        value={0.7}
        onChange={noop}
        onBlur={noop}
      />
    );
    expect(
      screen.getByRole('button', { name: 'Explain this field' })
    ).toBeInTheDocument();
  });

  it('renders no marker for a field without a tooltip entry', () => {
    const field: ModelFieldDef = { key: 'threshold', type: 'number' };
    render(
      <ModelSchemaField
        field={field}
        value={0.25}
        onChange={noop}
        onBlur={noop}
      />
    );
    expect(screen.queryByRole('button')).toBeNull();
  });
});
