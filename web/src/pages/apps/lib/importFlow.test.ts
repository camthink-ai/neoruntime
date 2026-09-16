import { describe, expect, it } from 'vitest';
import type { WizardConfig } from '@/services/types';
import {
  collectInstallErrors,
  isValidContainerImageRef,
  isValidModelAlias,
  resolveLocalMode,
} from './importFlow';

const completeConfig: WizardConfig = {
  metadata: {
    id: 'demo-app',
    name: 'Demo App',
    version: '1.0.0',
    description: '',
  },
  image: 'docker.io/library/nginx:latest',
  image_path: '',
  resources: { cpu: '50%', memory: '256Mi' },
  permissions: {
    video: [],
    inference: {
      models: [],
      max_qps: 10,
      max_concurrent: 0,
      allow_register_model: false,
    },
    events: { publish: [], subscribe: [] },
    device: { light: false, ir_cut: false, ptz: false, lens: false },
    network: { mode: 'isolated' },
  },
  env: [],
  volumes: [],
  autostart: false,
  restart_policy: 'on-failure',
};

describe('resolveLocalMode', () => {
  it('returns manifest when both app.yaml and tar are uploaded', () => {
    // Arrange
    const input = {
      manifestPath: '/tmp/aipc/app.yaml',
      imageTarPath: '/tmp/aipc/img.tar',
    };

    // Act
    const mode = resolveLocalMode(input);

    // Assert
    expect(mode).toBe('manifest');
  });

  it('returns image-only when only the tar is uploaded', () => {
    const mode = resolveLocalMode({
      manifestPath: '',
      imageTarPath: '/tmp/img.tar',
    });
    expect(mode).toBe('image-only');
  });

  it('returns null when neither slot has a file', () => {
    const mode = resolveLocalMode({ manifestPath: '', imageTarPath: '' });
    expect(mode).toBeNull();
  });
});

describe('isValidContainerImageRef', () => {
  it.each([
    'nginx:latest',
    'docker.io/library/nginx:latest',
    'registry.local:5000/team/app:v1.0',
    `docker.io/aipc/api-tour@sha256:${'a'.repeat(64)}`,
  ])('accepts %s', ref => {
    expect(isValidContainerImageRef(ref)).toBe(true);
  });

  it.each([
    '',
    'http://docker.io/nginx', // scheme
    '/leading/slash',
    'has space/nginx',
    'docker.io/aipc/api-tour@sha256:short', // bad digest
  ])('rejects %s', ref => {
    expect(isValidContainerImageRef(ref)).toBe(false);
  });
});

describe('isValidModelAlias', () => {
  it.each(['detector', 'clip_vit_b_32', '_internal', 'A1'])(
    'accepts %s',
    alias => {
      expect(isValidModelAlias(alias)).toBe(true);
    }
  );

  it.each(['', '1bad', 'has space', 'kebab-case', '中文'])(
    'rejects %s',
    alias => {
      expect(isValidModelAlias(alias)).toBe(false);
    }
  );
});

describe('collectInstallErrors', () => {
  it('returns no issues for a complete registry config', () => {
    // Arrange
    const config = completeConfig;

    // Act
    const issues = collectInstallErrors(config, {
      sourceType: 'registry',
      sourceReady: true,
    });

    // Assert
    expect(issues).toEqual([]);
  });

  it('reports missing id and name as basic_info issues', () => {
    // Arrange
    const config: WizardConfig = {
      ...completeConfig,
      metadata: { ...completeConfig.metadata, id: '  ', name: '' },
    };

    // Act
    const issues = collectInstallErrors(config, {
      sourceType: 'registry',
      sourceReady: true,
    });

    // Assert
    expect(issues).toEqual([
      { section: 'basic_info', reason: 'app_id_required' },
      { section: 'basic_info', reason: 'app_name_required' },
    ]);
  });

  it('reports an invalid registry image ref', () => {
    // Arrange
    const config: WizardConfig = { ...completeConfig, image: 'not a ref!' };

    // Act
    const issues = collectInstallErrors(config, {
      sourceType: 'registry',
      sourceReady: true,
    });

    // Assert
    expect(issues).toEqual([
      { section: 'basic_info', reason: 'invalid_image_ref' },
    ]);
  });

  it('reports local source missing when no file was uploaded', () => {
    // Arrange
    const config: WizardConfig = { ...completeConfig, image: '' };

    // Act
    const issues = collectInstallErrors(config, {
      sourceType: 'local',
      sourceReady: false,
    });

    // Assert
    expect(issues).toContainEqual({
      section: 'basic_info',
      reason: 'local_source_required',
    });
  });

  it('returns no source issue for local image-only once a tar is uploaded', () => {
    // Arrange
    const config: WizardConfig = { ...completeConfig, image: '' };

    // Act
    const issues = collectInstallErrors(config, {
      sourceType: 'local',
      sourceReady: true,
    });

    // Assert
    expect(issues).toEqual([]);
  });

  it('returns no issues for well-formed model dependencies', () => {
    // Arrange
    const config: WizardConfig = {
      ...completeConfig,
      models: {
        detector: { id: 'yolov8s-640', required: true },
        clip: { id: 'clip_vit_b_32' },
      },
    };

    // Act
    const issues = collectInstallErrors(config, {
      sourceType: 'local',
      sourceReady: true,
    });

    // Assert
    expect(issues).toEqual([]);
  });

  it('reports a draft row (empty alias) as an invalid model alias', () => {
    // Arrange — the editor reserves '' for an added-but-unnamed row
    const config: WizardConfig = {
      ...completeConfig,
      models: { '': { id: 'yolov8s-640' } },
    };

    // Act
    const issues = collectInstallErrors(config, {
      sourceType: 'local',
      sourceReady: true,
    });

    // Assert
    expect(issues).toEqual([
      { section: 'models', reason: 'invalid_model_alias' },
    ]);
  });

  it('reports a dependency whose model was never selected', () => {
    // Arrange
    const config: WizardConfig = {
      ...completeConfig,
      models: { detector: { id: '' } },
    };

    // Act
    const issues = collectInstallErrors(config, {
      sourceType: 'local',
      sourceReady: true,
    });

    // Assert
    expect(issues).toEqual([
      { section: 'models', reason: 'model_id_required' },
    ]);
  });

  it('reports each dependency problem at most once regardless of row count', () => {
    // Arrange — two broken rows of each kind
    const config: WizardConfig = {
      ...completeConfig,
      models: {
        '1bad': { id: '' },
        'also bad': { id: '  ' },
      },
    };

    // Act
    const issues = collectInstallErrors(config, {
      sourceType: 'local',
      sourceReady: true,
    });

    // Assert
    expect(issues).toEqual([
      { section: 'models', reason: 'invalid_model_alias' },
      { section: 'models', reason: 'model_id_required' },
    ]);
  });

  it('blocks a required dependency that is unregistered and has no path', () => {
    // Arrange — mirrors the backend fast-fail in resolveModelDependencies
    const config: WizardConfig = {
      ...completeConfig,
      models: {
        detector: {
          id: 'yolo_world_540',
          required: true,
        },
      },
    };

    // Act
    const issues = collectInstallErrors(config, {
      sourceType: 'local',
      sourceReady: true,
      availableModelIds: ['clip_vit_b_32'],
    });

    // Assert
    expect(issues).toEqual([
      { section: 'models', reason: 'model_unavailable_required' },
    ]);
  });

  it('allows an unregistered required dependency that declares a bundled path', () => {
    // Arrange — the user-declared custom model case (id + .bin package path)
    const config: WizardConfig = {
      ...completeConfig,
      models: {
        detector: {
          id: 'yolo_world_540',
          path: '/opt/models/yolo.bin',
          required: true,
        },
      },
    };

    // Act
    const issues = collectInstallErrors(config, {
      sourceType: 'local',
      sourceReady: true,
      availableModelIds: [],
    });

    // Assert
    expect(issues).toEqual([]);
  });

  it('rejects a bundled path that is not an AMPK .bin package', () => {
    // Arrange — mirror of the backend manifest validation: bare .hef
    // bundling is no longer supported on the install side
    const config: WizardConfig = {
      ...completeConfig,
      models: {
        detector: {
          id: 'yolo_world_540',
          path: '/opt/models/yolo.hef',
          required: true,
        },
      },
    };

    // Act
    const issues = collectInstallErrors(config, {
      sourceType: 'local',
      sourceReady: true,
      availableModelIds: [],
    });

    // Assert
    expect(issues).toEqual([
      { section: 'models', reason: 'model_path_invalid' },
    ]);
  });

  it('warns nothing for an optional unregistered dependency without a path', () => {
    // Arrange — optional misses only warn server-side, never block
    const config: WizardConfig = {
      ...completeConfig,
      models: {
        detector: { id: 'yolo_world_540' },
      },
    };

    // Act
    const issues = collectInstallErrors(config, {
      sourceType: 'local',
      sourceReady: true,
      availableModelIds: ['clip_vit_b_32'],
    });

    // Assert
    expect(issues).toEqual([]);
  });

  it('skips the availability check while the model list is not loaded', () => {
    // Arrange — availableModelIds undefined (query loading): no false alarms
    const config: WizardConfig = {
      ...completeConfig,
      models: {
        detector: { id: 'yolo_world_540', required: true },
      },
    };

    // Act
    const issues = collectInstallErrors(config, {
      sourceType: 'local',
      sourceReady: true,
    });

    // Assert
    expect(issues).toEqual([]);
  });
});
