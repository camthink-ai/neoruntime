import { render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import type { ModelFieldDef } from '@/hooks/useModels';
import ModelSchemaField from './ModelSchemaField';

// Only the keys the tests assert on; everything else takes the caller's
// fallback (non-empty string) or '' — mirroring i18next's missing-key
// behavior that hides the hint/note elements.
const strings: Record<string, string> = {
  'sys.ai_models.form.postprocess_profile_facial_landmarks_note':
    'facial hardcoded note',
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

const baseField: ModelFieldDef = { key: 'threshold', type: 'number' };
const noop = () => {};

const renderField = (field: ModelFieldDef, value?: unknown) => render(
    <ModelSchemaField
      field={field}
      value={value}
      onChange={noop}
      onBlur={noop}
    />
  );

describe('ModelSchemaField effect badges', () => {
  it('renders an advisory badge when the field does not reach the decoder', () => {
    renderField({ ...baseField, effect: 'advisory' });
    expect(screen.getByText('no effect')).toBeInTheDocument();
  });

  it('renders a metadata badge for consumer-side metadata fields', () => {
    renderField({
      ...baseField,
      key: 'labels',
      type: 'text',
      effect: 'metadata',
    });
    expect(screen.getByText('metadata only')).toBeInTheDocument();
  });

  it('renders no badge for consumed or unspecified effects', () => {
    const { rerender } = renderField(baseField);
    expect(screen.queryByText('no effect')).not.toBeInTheDocument();
    expect(screen.queryByText('metadata only')).not.toBeInTheDocument();

    rerender(
      <ModelSchemaField
        field={{ ...baseField, effect: 'consumed' }}
        value={undefined}
        onChange={noop}
        onBlur={noop}
      />
    );
    expect(screen.queryByText('no effect')).not.toBeInTheDocument();
  });
});

describe('ModelSchemaField select option note card', () => {
  const profileField: ModelFieldDef = {
    key: 'postprocess_profile',
    type: 'select',
    options: [
      { value: 'facial_landmarks', label: 'Facial landmarks (default)' },
      { value: 'yolov8_pose', label: 'YOLOv8 pose (COCO-17)' },
    ],
  };

  it('shows the note card for the active option when a note key exists', () => {
    renderField(profileField, 'facial_landmarks');
    expect(screen.getByText('facial hardcoded note')).toBeInTheDocument();
  });

  it('hides the note card when the active option has no note', () => {
    renderField(profileField, 'yolov8_pose');
    expect(screen.queryByText('facial hardcoded note')).not.toBeInTheDocument();
  });
});
