import { describe, expect, it } from 'vitest';
import type { WizardConfig } from '@/services/types';
import { resolveYamlViewMode, wizardConfigToYaml } from './wizardYaml';

const emptyConfig: WizardConfig = {
  metadata: { id: '', name: '', version: '', description: '' },
  image: '',
};

describe('wizardConfigToYaml', () => {
  it('emits header and required metadata for an empty config', () => {
    // Arrange
    const config = emptyConfig;

    // Act
    const yaml = wizardConfigToYaml(config);

    // Assert
    expect(yaml).toBe(
      [
        'apiVersion: v1',
        'kind: Application',
        'metadata:',
        '  id: ""',
        '  name: ""',
        '  version: ""',
        '',
      ].join('\n')
    );
  });

  it('omits optional branches the config does not carry', () => {
    const yaml = wizardConfigToYaml({
      ...emptyConfig,
      metadata: {
        id: 'demo-app',
        name: 'Demo App',
        version: '1.0.0',
        description: '',
      },
      image: 'aipc/demo:0.1.0',
    });

    expect(yaml).toBe(
      [
        'apiVersion: v1',
        'kind: Application',
        'metadata:',
        '  id: demo-app',
        '  name: Demo App',
        '  version: 1.0.0', // 1.0.0 is not a valid number scalar — stays plain
        'spec:',
        '  image: aipc/demo:0.1.0',
        '',
      ].join('\n')
    );
    expect(yaml).not.toContain('resources');
    expect(yaml).not.toContain('permissions');
  });

  it('renders the full wizard-expressible subset in schema key order', () => {
    const yaml = wizardConfigToYaml({
      metadata: {
        id: 'gym-ops',
        name: 'GymOps Pose Coach',
        version: '0.1.0',
        description: 'Real-time gym pose coaching',
      },
      image: 'aipc/gym-ops:0.1.0',
      models: {
        detector: {
          id: 'yolov8s_pose',
          path: '/opt/models/yolov8s_pose.bin',
          required: true,
        },
        clip: { id: 'clip_vit_b_32' },
      },
      resources: { cpu: '300%', memory: '1Gi' },
      permissions: {
        video: ['/dev/video0'],
        inference: {
          max_qps: 10,
          max_concurrent: 2,
          allow_register_model: true,
        },
        events: { publish: ['app/zone'], subscribe: ['app/cmd'] },
        device: { light: true, ir_cut: false, ptz: true, lens: false },
        network: { mode: 'bridge', inbound: [8080, 9090] },
      },
      env: [{ name: 'MODE', value: 'live' }],
      volumes: [
        {
          host: '/data/aipc/models',
          container: '/opt/aipc/models',
          readonly: true,
        },
      ],
      autostart: true,
      restart_policy: 'on-failure',
      security: { no_new_privileges: true, readonly_rootfs: false },
    });

    expect(yaml).toBe(
      [
        'apiVersion: v1',
        'kind: Application',
        'metadata:',
        '  id: gym-ops',
        '  name: GymOps Pose Coach',
        '  version: 0.1.0',
        '  description: Real-time gym pose coaching',
        'spec:',
        '  image: aipc/gym-ops:0.1.0',
        '  models:',
        '    detector:',
        '      id: yolov8s_pose',
        '      path: /opt/models/yolov8s_pose.bin',
        '      required: true',
        '    clip:',
        '      id: clip_vit_b_32',
        '  resources:',
        '    cpu: 300%', // "300%" parses as a string — safe plain
        '    memory: 1Gi',
        '  permissions:',
        '    video:',
        '      - /dev/video0',
        '    inference:',
        '      max_qps: 10',
        '      max_concurrent: 2',
        '      allow_register_model: true',
        '    events:',
        '      publish:',
        '        - app/zone',
        '      subscribe:',
        '        - app/cmd',
        '    device:',
        '      light: true',
        '      ir_cut: false',
        '      ptz: true',
        '      lens: false',
        '    network:',
        '      mode: bridge',
        '      inbound:',
        '        - 8080',
        '        - 9090',
        '  env:',
        '    - name: MODE',
        '      value: live',
        '  volumes:',
        '    - host: /data/aipc/models',
        '      container: /opt/aipc/models',
        '      readonly: true',
        '  autostart: true',
        '  restart_policy: on-failure',
        '  security:',
        '    no_new_privileges: true',
        '    readonly_rootfs: false',
        '',
      ].join('\n')
    );
  });

  it('quotes strings that would parse back as numbers or booleans', () => {
    const yaml = wizardConfigToYaml({
      ...emptyConfig,
      resources: { cpu: '2', memory: '512' },
      restart_policy: 'no',
    });

    expect(yaml).toContain('cpu: "2"');
    expect(yaml).toContain('memory: "512"');
    expect(yaml).toContain('restart_policy: "no"');
  });

  it('quotes strings with structural indicator characters', () => {
    const yaml = wizardConfigToYaml({
      ...emptyConfig,
      metadata: {
        id: 'app-1',
        name: 'App: The #1 coach',
        version: '1.0.0',
        description: 'has: colon-space and # hash',
      },
    });

    expect(yaml).toContain('name: "App: The #1 coach"');
    expect(yaml).toContain('description: "has: colon-space and # hash"');
  });

  it('omits empty arrays and objects that prune to nothing', () => {
    const yaml = wizardConfigToYaml({
      ...emptyConfig,
      permissions: {
        video: [],
        inference: {},
        events: { publish: [], subscribe: [] },
        device: { light: false },
      },
      models: {},
      env: [],
      volumes: [],
      security: {},
    });

    expect(yaml).toContain('device:'); // device carries `light: false`
    expect(yaml).not.toContain('video');
    expect(yaml).not.toContain('inference');
    expect(yaml).not.toContain('models');
    expect(yaml).not.toContain('events');
    expect(yaml).not.toContain('env');
    expect(yaml).not.toContain('volumes');
    expect(yaml).not.toContain('security');
  });

  it('emits readonly only when the volume sets it', () => {
    const yaml = wizardConfigToYaml({
      ...emptyConfig,
      volumes: [
        { host: '/a', container: '/b' },
        { host: '/c', container: '/d', readonly: true },
      ],
    });

    expect(yaml).toContain('- host: /a');
    expect(yaml).not.toContain('readonly: false');
    expect(yaml).toContain('readonly: true');
  });
});

describe('resolveYamlViewMode', () => {
  it('returns editable for a local import with an uploaded app.yaml', () => {
    expect(
      resolveYamlViewMode({ sourceType: 'local', hasManifest: true })
    ).toBe('editable');
  });

  it('returns preview for local imports without app.yaml', () => {
    expect(
      resolveYamlViewMode({ sourceType: 'local', hasManifest: false })
    ).toBe('preview');
  });

  it('returns preview for the registry source regardless of manifest', () => {
    expect(
      resolveYamlViewMode({ sourceType: 'registry', hasManifest: true })
    ).toBe('preview');
  });
});
