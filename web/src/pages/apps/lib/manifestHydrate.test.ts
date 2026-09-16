import { describe, expect, it } from 'vitest';

import type { AppManifestDTO, WizardConfig } from '@/services/types';
import {
  applyPatchFields,
  canonicalize,
  changedPatchFields,
  isDirty,
  manifestToWizardConfig,
  wizardConfigToPatchFields,
} from './manifestHydrate';

const manifest: AppManifestDTO = {
  apiVersion: 'v1',
  kind: 'Application',
  metadata: {
    id: 'demo-app',
    name: 'Demo',
    version: '1.2.3',
    description: 'a demo',
  },
  spec: {
    image: 'docker.io/library/alpine:latest',
    permissions: {
      video: ['camera_front'],
      inference: { models: ['clip_vit_b_32'], max_qps: 5 },
      device: { light: true },
      network: { mode: 'host', inbound: [8080] },
    },
    resources: { cpu: '50%', memory: '256Mi' },
    env: [{ name: 'A', value: '1' }],
    volumes: [{ host: '/data', container: '/data' }],
    autostart: true,
    restart_policy: 'on-failure',
    security: { no_new_privileges: true, readonly_rootfs: false },
    // declarative model dependencies — the wizard's model data source
    models: {
      detector: {
        id: 'yolov8s-640',
        path: '/opt/models/yolov8s.bin',
        required: true,
      },
      clip: { id: 'clip_vit_b_32' },
    },
  },
};

describe('canonicalize', () => {
  it('ignores key insertion order', () => {
    expect(canonicalize({ a: 1, b: 2 })).toBe(canonicalize({ b: 2, a: 1 }));
  });

  it('treats array order as significant', () => {
    expect(canonicalize([1, 2])).not.toBe(canonicalize([2, 1]));
  });

  it('drops undefined keys so sparse spread updates stay equal', () => {
    expect(canonicalize({ a: 1, b: undefined })).toBe(canonicalize({ a: 1 }));
  });
});

describe('manifestToWizardConfig', () => {
  it('maps every wizard field and deep-copies model dependencies', () => {
    const config = manifestToWizardConfig(manifest);
    expect(config.metadata.id).toBe('demo-app');
    expect(config.models).toEqual({
      detector: {
        id: 'yolov8s-640',
        path: '/opt/models/yolov8s.bin',
        required: true,
      },
      clip: { id: 'clip_vit_b_32' },
    });
    // the legacy authorization list is no longer form state
    expect(config.permissions?.inference?.models).toBeUndefined();
    expect(config.security).toEqual({
      no_new_privileges: true,
      readonly_rootfs: false,
    });
    expect(config.env).toEqual([{ name: 'A', value: '1' }]);

    // Independent copy: mutating the config must not touch the manifest.
    config.models!.detector.required = false;
    expect(manifest.spec.models?.detector.required).toBe(true);
  });

  it('keeps manifest-omitted fields undefined instead of defaulting', () => {
    const config = manifestToWizardConfig({
      metadata: { id: 'a', name: 'A', version: '1' },
      spec: {},
    });
    expect(config.resources?.cpu).toBeUndefined();
    expect(config.autostart).toBeUndefined();
    expect(config.security).toBeUndefined();
    expect(config.permissions?.video).toBeUndefined();
  });
});

describe('manifestToWizardConfig · server null marshalling', () => {
  // The Go backend marshals nil slices/maps/pointers as JSON null, so the
  // browser receives null where the DTO type says "omitted". Regression:
  // hydrating such a manifest must not throw, and null fields stay absent.
  const nullManifest = {
    ...manifest,
    spec: {
      image: 'docker.io/library/alpine:latest',
      permissions: {
        video: null,
        inference: { models: null, allow_register_model: null },
        events: { publish: null, subscribe: null },
        device: { light: null },
        network: { mode: null, inbound: null },
      },
      resources: { cpu: null, memory: null },
      env: null,
      volumes: null,
      autostart: null,
      restart_policy: null,
      security: null,
      models: null,
    },
  } as unknown as AppManifestDTO;

  it('hydrates a manifest whose absent fields arrived as null', () => {
    const config = manifestToWizardConfig(nullManifest);
    expect(config.metadata.id).toBe('demo-app');
    expect(config.image).toBe('docker.io/library/alpine:latest');
  });

  it('treats null arrays and scalars as omitted, not values', () => {
    const config = manifestToWizardConfig(nullManifest);
    expect(config.permissions?.video).toBeUndefined();
    expect(config.permissions?.inference?.models).toBeUndefined();
    expect(config.permissions?.inference?.allow_register_model).toBeUndefined();
    expect(config.permissions?.events?.publish).toBeUndefined();
    expect(config.permissions?.network?.inbound).toBeUndefined();
    expect(config.resources?.cpu).toBeUndefined();
    expect(config.env).toBeUndefined();
    expect(config.volumes).toBeUndefined();
    expect(config.autostart).toBeUndefined();
    expect(config.security).toBeUndefined();
    expect(config.models).toBeUndefined();
  });
});

describe('wizardConfigToPatchFields', () => {
  it('always emits spec.models but never id or image', () => {
    const fields = wizardConfigToPatchFields(manifestToWizardConfig(manifest));
    expect(Object.keys(fields)).not.toContain('metadata.id');
    expect(Object.keys(fields)).not.toContain('spec.image');
    expect(fields['spec.models']).toEqual({
      detector: {
        id: 'yolov8s-640',
        path: '/opt/models/yolov8s.bin',
        required: true,
      },
      clip: { id: 'clip_vit_b_32' },
    });
    expect(fields).not.toHaveProperty('spec.permissions.inference.models');
  });

  it('emits spec.models as null when there are no dependencies', () => {
    const fields = wizardConfigToPatchFields({
      metadata: { id: 'a', name: 'A', version: '1', description: '' },
      image: '',
    });
    expect(fields['spec.models']).toBeNull();
  });

  it('omits empty descriptions so absent fields stay absent', () => {
    const fields = wizardConfigToPatchFields({
      metadata: { id: 'a', name: 'A', version: '1', description: '' },
      image: '',
    });
    expect(fields).not.toHaveProperty('metadata.description');
  });
});

describe('dirty tracking', () => {
  const hydrated = manifestToWizardConfig(manifest);

  it('reports clean on the untouched snapshot', () => {
    expect(isDirty(hydrated, hydrated)).toBe(false);
    expect(Object.keys(changedPatchFields(hydrated, hydrated))).toHaveLength(0);
  });

  it('stays clean when only key order changed via spread updates', () => {
    // A spread-style setConfig that rebuilds permissions before metadata
    // produces a different key order but the same content.
    const reordered: WizardConfig = {
      permissions: { ...hydrated.permissions },
      metadata: { ...hydrated.metadata },
      image: hydrated.image,
      resources: { ...hydrated.resources },
      env: hydrated.env?.map(e => ({ ...e })),
      volumes: hydrated.volumes?.map(v => ({ ...v })),
      autostart: hydrated.autostart,
      restart_policy: hydrated.restart_policy,
      security: { ...hydrated.security },
      models: hydrated.models,
    };
    expect(isDirty(reordered, hydrated)).toBe(false);
  });

  it('reports exactly the edited field', () => {
    const edited: WizardConfig = {
      ...hydrated,
      metadata: { ...hydrated.metadata, name: 'Renamed' },
    };
    const changed = changedPatchFields(edited, hydrated);
    expect(Object.keys(changed)).toEqual(['metadata.name']);
    expect(changed['metadata.name']).toBe('Renamed');
  });

  it('detects an edited dependency mapping', () => {
    const edited: WizardConfig = {
      ...hydrated,
      models: {
        ...hydrated.models,
        detector: { ...hydrated.models!.detector, id: 'yolov8n-320' },
      },
    };
    const changed = changedPatchFields(edited, hydrated);
    expect(Object.keys(changed)).toEqual(['spec.models']);
    expect(changed['spec.models']).toHaveProperty('detector.id', 'yolov8n-320');
  });

  it('detects removed dependencies as a null clear', () => {
    const edited: WizardConfig = { ...hydrated, models: undefined };
    const changed = changedPatchFields(edited, hydrated);
    expect(changed['spec.models']).toBeNull();
  });

  it('is clean against a null snapshot (non-package sources)', () => {
    expect(isDirty(hydrated, null)).toBe(false);
    expect(changedPatchFields(hydrated, null)).toEqual({});
  });
});

describe('applyPatchFields', () => {
  const base = manifestToWizardConfig(manifest);

  it('overlays spec-prefixed edits onto the flushed manifest mapping', () => {
    // Arrange — form edits captured before the YAML flush
    const fields = {
      'metadata.name': 'Renamed',
      'spec.resources.cpu': '200%',
      'spec.permissions.inference.max_qps': 20,
    };

    // Act
    const merged = applyPatchFields(base, fields);

    // Assert
    expect(merged.metadata.name).toBe('Renamed');
    expect(merged.resources?.cpu).toBe('200%');
    expect(merged.permissions?.inference?.max_qps).toBe(20);
    // untouched fields come from the new manifest
    expect(merged.metadata.version).toBe('1.2.3');
    expect(merged.permissions?.device?.light).toBe(true);
  });

  it('replaces array-valued paths wholesale', () => {
    const merged = applyPatchFields(base, {
      'spec.env': [
        { name: 'A', value: '1' },
        { name: 'B', value: '2' },
      ],
    });

    expect(merged.env).toEqual([
      { name: 'A', value: '1' },
      { name: 'B', value: '2' },
    ]);
  });

  it('creates missing intermediate branches instead of crashing', () => {
    // Act — the flushed file dropped security entirely, the form still has
    // an edit for it
    const bare = applyPatchFields(base, {
      'spec.security.no_new_privileges': false,
    });

    // Assert
    expect(bare.security?.no_new_privileges).toBe(false);
  });

  it('does not mutate the base config', () => {
    const snapshot = JSON.stringify(base);

    applyPatchFields(base, {
      'metadata.name': 'Changed',
      'spec.resources.memory': '1Gi',
      'spec.volumes': [],
    });

    expect(JSON.stringify(base)).toBe(snapshot);
  });

  it('deletes the target key for a null value (dependency clear)', () => {
    // Arrange — a captured form edit that removed all dependencies
    // Act
    const merged = applyPatchFields(base, { 'spec.models': null });

    // Assert — null is the wire clear format; the config treats absence
    // as canonical, and the base config is untouched.
    expect(merged.models).toBeUndefined();
    expect('models' in merged).toBe(false);
    expect(base.models).toBeDefined();
  });

  it('round-trips with changedPatchFields: form edits survive a flush', () => {
    // Arrange — the user edits two form fields, then flushes new YAML whose
    // own mapping differs from the old snapshot
    const formEdited: WizardConfig = {
      ...base,
      metadata: { ...base.metadata, description: 'flush-safe' },
      permissions: {
        ...base.permissions,
        network: { ...base.permissions!.network, inbound: [9090] },
      },
    };
    const changed = changedPatchFields(formEdited, base);

    // Act — new manifest version arrives with different values everywhere
    const newManifest: AppManifestDTO = {
      ...manifest,
      metadata: { ...manifest.metadata, description: 'from yaml editor' },
      spec: {
        ...manifest.spec,
        permissions: {
          ...manifest.spec.permissions,
          network: { mode: 'bridge', inbound: [80, 443] },
        },
      },
    };
    const merged = applyPatchFields(
      manifestToWizardConfig(newManifest),
      changed
    );

    // Assert — form overrides win, YAML values fill the rest
    expect(merged.metadata.description).toBe('flush-safe');
    expect(merged.permissions?.network?.inbound).toEqual([9090]);
    expect(merged.permissions?.network?.mode).toBe('bridge');
  });

  it('returns an equal copy for an empty field set', () => {
    const merged = applyPatchFields(base, {});

    expect(canonicalize(merged)).toBe(canonicalize(base));
    expect(merged).not.toBe(base);
  });
});
