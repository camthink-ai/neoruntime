import { useState } from 'react';
import { fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import type { WizardConfig } from '@/services/types';
import ModelsSection from './ModelsSection';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (_key: string, fallback?: string) => fallback ?? _key,
  }),
}));

const initialConfig: WizardConfig = {
  metadata: {
    id: 'alias-regression',
    name: 'Alias Regression',
    version: '1.0.0',
    description: '',
  },
  image: 'example/alias-regression:1.0.0',
  models: { '': { id: '' } },
};

function Harness() {
  const [config, setConfig] = useState(initialConfig);
  return (
    <ModelsSection
      config={config}
      onChange={setConfig}
      availableModels={[]}
      issues={[]}
    />
  );
}

describe('ModelsSection', () => {
  it('keeps the alias input mounted and focused while typing multiple characters', () => {
    render(<Harness />);

    const aliasInput = screen.getByPlaceholderText('Alias');
    aliasInput.focus();

    let typed = '';
    for (const char of 'detector') {
      typed += char;
      fireEvent.change(aliasInput, { target: { value: typed } });

      expect(screen.getByPlaceholderText('Alias')).toBe(aliasInput);
      expect(aliasInput).toHaveValue(typed);
      expect(aliasInput).toHaveFocus();
    }
  });
});
