import { describe, expect, it } from 'vitest';
import type { WizardConfig } from '@/services/types';
import { applyConfigToYamlText, parseYamlToConfig } from './yamlSync';

const baseManifest = `apiVersion: v1
kind: AIPCApp
# app identity lives in metadata
metadata:
  id: demo-app # directory binding
  name: Demo App
  version: 1.0.0
spec:
  image: docker.io/library/nginx:latest
  # models the app depends on
  models:
    detector:
      id: yolov8s-640
      required: true
  resources:
    cpu: "50%"
    memory: 256Mi
  autostart: false
  unknown_field: keep-me # must survive form edits
`;

const baseConfig: WizardConfig = {
  metadata: {
    id: 'demo-app',
    name: 'Demo App',
    version: '1.0.0',
    description: '',
  },
  image: 'docker.io/library/nginx:latest',
  models: {
    detector: { id: 'yolov8s-640', required: true },
  },
  resources: { cpu: '50%', memory: '256Mi' },
  // manifestToWizardConfig always materializes the permissions sub-objects,
  // even for a manifest without a permissions block
  permissions: { inference: {}, events: {}, device: {}, network: {} },
  autostart: false,
};

describe('parseYamlToConfig', () => {
  it('hydrates a manifest the same way the upload flow does', () => {
    // Act
    const result = parseYamlToConfig(baseManifest);

    // Assert
    expect(result).toEqual({ config: baseConfig });
  });

  it('reports a syntax error instead of throwing', () => {
    // Act
    const result = parseYamlToConfig('spec:\n  image: [unclosed');

    // Assert
    expect('error' in result && result.error).toBeTruthy();
  });

  it('rejects an empty document', () => {
    // Act
    const result = parseYamlToConfig('   \n');

    // Assert
    expect(result).toEqual({ error: 'document is empty' });
  });

  it('rejects a top-level sequence', () => {
    // Act
    const result = parseYamlToConfig('- a\n- b\n');

    // Assert
    expect(result).toEqual({ error: 'document is not a YAML mapping' });
  });
});

describe('applyConfigToYamlText', () => {
  it('preserves comments and unknown fields while applying form edits', () => {
    // Arrange — form edits: rename, custom model with a bundled path, quota
    const config: WizardConfig = {
      ...baseConfig,
      metadata: { ...baseConfig.metadata, name: 'Renamed App' },
      models: {
        detector: {
          id: 'custom-yolo',
          path: '/opt/models/yolo.bin',
          required: true,
        },
      },
      permissions: {
        inference: { max_concurrent: 0, allow_register_model: false },
      },
    };

    // Act
    const result = applyConfigToYamlText(baseManifest, config);

    // Assert
    expect('text' in result).toBe(true);
    if (!('text' in result)) return;
    const { text } = result;
    expect(text).toContain('# app identity lives in metadata');
    expect(text).toContain('# models the app depends on');
    expect(text).toContain('unknown_field: keep-me');
    expect(text).toContain('name: Renamed App');
    expect(text).toContain('id: custom-yolo');
    expect(text).toContain('path: /opt/models/yolo.bin');
    // the manifest-level model type is gone — bundled models take it from
    // the AMPK package metadata at install time
    expect(text).not.toContain('type:');
    // false / 0 are meaningful values — they must be written, not dropped
    expect(text).toContain('autostart: false');
    expect(text).toContain('max_concurrent: 0');
    expect(text).toContain('allow_register_model: false');
  });

  it('omits undefined optional fields instead of writing nulls', () => {
    // Arrange — mapping carries no path/type; only id + required
    const config: WizardConfig = {
      ...baseConfig,
      models: { clip: { id: 'clip_vit_b_32', required: undefined } },
    };

    // Act
    const result = applyConfigToYamlText(baseManifest, config);

    // Assert
    expect('text' in result).toBe(true);
    if (!('text' in result)) return;
    expect(result.text).toContain('id: clip_vit_b_32');
    expect(result.text).not.toContain('path:');
    expect(result.text).not.toContain('type:');
    expect(result.text).not.toContain('required:');
  });

  it('removes spec.models and spec.image when the form clears them', () => {
    // Arrange
    const config: WizardConfig = {
      ...baseConfig,
      image: '',
      models: undefined,
    };

    // Act
    const result = applyConfigToYamlText(baseManifest, config);

    // Assert
    expect('text' in result).toBe(true);
    if (!('text' in result)) return;
    expect(result.text).not.toContain('image:');
    expect(result.text).not.toContain('models:');
    // untouched siblings survive
    expect(result.text).toContain('cpu: "50%"');
  });

  it('reports an error when the current text does not parse', () => {
    // Arrange — the editor holds broken yaml; form edits must not destroy it
    // Act
    const result = applyConfigToYamlText('a: [oops', baseConfig);

    // Assert
    expect('error' in result).toBe(true);
  });

  it('is byte-stable: reapplying the parsed config changes nothing', () => {
    // Arrange
    const parsed = parseYamlToConfig(baseManifest);
    expect('config' in parsed).toBe(true);
    if (!('config' in parsed)) return;

    // Act
    const once = applyConfigToYamlText(baseManifest, parsed.config);
    expect('text' in once).toBe(true);
    if (!('text' in once)) return;
    const reparsed = parseYamlToConfig(once.text);
    expect('config' in reparsed).toBe(true);
    if (!('config' in reparsed)) return;
    const twice = applyConfigToYamlText(once.text, reparsed.config);

    // Assert — the second pass is a no-op, so live sync reaches a fixed point
    expect(twice).toEqual(once);
    expect(reparsed.config).toEqual(parsed.config);
  });
});
