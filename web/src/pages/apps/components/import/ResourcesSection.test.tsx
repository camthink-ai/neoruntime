import { useState } from 'react';
import { fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import type { WizardConfig } from '@/services/types';
import ResourcesSection from './ResourcesSection';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (_key: string, fallback?: string) => fallback ?? _key,
  }),
}));

const initialConfig: WizardConfig = {
  metadata: {
    id: 'cpu-format-regression',
    name: 'CPU Format Regression',
    version: '1.0.0',
    description: '',
  },
  image: 'example/cpu-format-regression:1.0.0',
  resources: { cpu: '0.5', memory: '256Mi' },
};

function Harness() {
  const [config, setConfig] = useState(initialConfig);
  return (
    <>
      <ResourcesSection config={config} onChange={setConfig} issues={[]} />
      <output data-testid="cpu-value">{config.resources?.cpu}</output>
    </>
  );
}

describe('ResourcesSection', () => {
  it('preserves manually entered CPU core and percentage formats', () => {
    render(<Harness />);

    const cpuInput = screen.getByPlaceholderText('0.5 / 50%');
    expect(cpuInput).toHaveValue('0.5');

    fireEvent.change(cpuInput, { target: { value: '1.5' } });
    expect(cpuInput).toHaveValue('1.5');
    expect(screen.getByTestId('cpu-value')).toHaveTextContent('1.5');

    fireEvent.change(cpuInput, { target: { value: '50%' } });
    expect(cpuInput).toHaveValue('50%');
    expect(screen.getByTestId('cpu-value')).toHaveTextContent('50%');
  });
});
