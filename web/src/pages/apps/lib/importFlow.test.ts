import { describe, expect, it } from 'vitest';
import type { WizardConfig } from '@/services/types';
import {
  collectInstallErrors,
  collectInstallIssues,
  isValidContainerImageRef,
  isValidModelAlias,
  resolveLocalMode,
} from './importFlow';

const errorIssue = (section: string, field: string, reason: string) => ({
  section,
  field,
  reason,
  severity: 'error',
});

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
      errorIssue('basic_info', 'metadata.id', 'app_id_required'),
      errorIssue('basic_info', 'metadata.name', 'app_name_required'),
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
      errorIssue('basic_info', 'image', 'invalid_image_ref'),
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
    expect(issues).toContainEqual(
      errorIssue('basic_info', 'image', 'local_source_required')
    );
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
      errorIssue('models', 'models', 'invalid_model_alias'),
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
      errorIssue('models', 'models', 'model_id_required'),
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
      errorIssue('models', 'models', 'invalid_model_alias'),
      errorIssue('models', 'models', 'model_id_required'),
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
      errorIssue('models', 'models', 'model_unavailable_required'),
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
      errorIssue('models', 'models', 'model_path_invalid'),
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

  it('validates version, safe app id, and positive resource limits', () => {
    const config: WizardConfig = {
      ...completeConfig,
      metadata: { ...completeConfig.metadata, id: '../bad', version: ' ' },
      resources: { cpu: '0%', memory: '-1Mi' },
    };

    const issues = collectInstallErrors(config, {
      sourceType: 'local',
      sourceReady: true,
    });

    expect(issues).toEqual([
      errorIssue('basic_info', 'metadata.id', 'app_id_invalid'),
      errorIssue('basic_info', 'metadata.version', 'app_version_required'),
      errorIssue('resources', 'resources.cpu', 'cpu_limit_invalid'),
      errorIssue('resources', 'resources.memory', 'memory_limit_invalid'),
    ]);
  });

  it('accepts a non-empty non-SemVer version for compatibility', () => {
    const config: WizardConfig = {
      ...completeConfig,
      metadata: { ...completeConfig.metadata, version: '2026.09-release' },
    };

    expect(
      collectInstallErrors(config, {
        sourceType: 'local',
        sourceReady: true,
      })
    ).toEqual([]);
  });

  it('validates permission ports, quotas, event topics, and network mode', () => {
    const config: WizardConfig = {
      ...completeConfig,
      permissions: {
        ...completeConfig.permissions,
        inference: {
          ...completeConfig.permissions?.inference,
          max_qps: -1,
          max_concurrent: 0,
        },
        events: { publish: ['app/event', 'app/event'], subscribe: [''] },
        network: { mode: 'isolated', inbound: [8080, 8080, 70000] },
      },
    };

    const reasons = collectInstallErrors(config, {
      sourceType: 'local',
      sourceReady: true,
    }).map(issue => issue.reason);

    expect(reasons).toEqual([
      'inference_quota_invalid',
      'event_topic_invalid',
      'event_topic_duplicate',
      'network_inbound_invalid',
      'network_inbound_duplicate',
      'network_inbound_requires_host',
    ]);
  });

  it('validates environment names, volume paths, and restart policy', () => {
    const config: WizardConfig = {
      ...completeConfig,
      env: [
        { name: 'BAD-NAME', value: 'x' },
        { name: 'BAD-NAME', value: 'y' },
      ],
      volumes: [
        { host: 'relative', container: '/app/data' },
        { host: '/data/other', container: '/app/data' },
      ],
      restart_policy: 'sometimes',
    };

    const reasons = collectInstallErrors(config, {
      sourceType: 'local',
      sourceReady: true,
    }).map(issue => issue.reason);

    expect(reasons).toEqual([
      'env_name_invalid',
      'env_name_duplicate',
      'volume_path_invalid',
      'volume_destination_duplicate',
      'restart_policy_invalid',
    ]);
  });

  it('keeps zero quotas and legacy restart aliases compatible', () => {
    const config: WizardConfig = {
      ...completeConfig,
      permissions: {
        ...completeConfig.permissions,
        inference: { max_qps: 0, max_concurrent: 0 },
      },
      restart_policy: 'on_failure',
    };

    expect(
      collectInstallErrors(config, {
        sourceType: 'local',
        sourceReady: true,
      })
    ).toEqual([]);
  });

  it('returns risk checks as warnings without treating them as errors', () => {
    const config: WizardConfig = {
      ...completeConfig,
      resources: { cpu: '150%', memory: '8Gi' },
      permissions: {
        ...completeConfig.permissions,
        video: ['missing-stream'],
        inference: { allow_register_model: true },
        network: { mode: 'host' },
      },
      security: { readonly_rootfs: false },
    };
    const opts = {
      sourceType: 'local' as const,
      sourceReady: true,
      availableStreamIds: [] as string[],
    };

    expect(collectInstallErrors(config, opts)).toEqual([]);
    expect(
      collectInstallIssues(config, opts).map(issue => issue.reason)
    ).toEqual([
      'cpu_limit_high',
      'memory_limit_high',
      'dynamic_model_registration_warning',
      'host_network_warning',
      'video_stream_unavailable',
      'writable_rootfs_warning',
    ]);
  });
});
