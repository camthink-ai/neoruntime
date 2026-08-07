import type { TFunction } from 'i18next';
import type { InstallProgress } from '@/hooks/useApps';

const EXACT_MESSAGE_KEYS: Record<string, { key: string; fallback: string }> = {
  'Preparing installation...': {
    key: 'sys.apps.import.progress.preparing_install',
    fallback: 'Preparing installation...',
  },
  'Validating manifest...': {
    key: 'sys.apps.import.progress.validating_manifest',
    fallback: 'Validating manifest...',
  },
  'Validation complete': {
    key: 'sys.apps.import.progress.validation_complete',
    fallback: 'Validation complete',
  },
  'Removing existing app for overwrite...': {
    key: 'sys.apps.import.progress.removing_existing',
    fallback: 'Removing existing app for overwrite...',
  },
  'Starting image pull...': {
    key: 'sys.apps.import.progress.starting_pull',
    fallback: 'Starting image pull...',
  },
  'Image pull complete': {
    key: 'sys.apps.import.progress.pull_complete',
    fallback: 'Image pull complete',
  },
  'Importing local image...': {
    key: 'sys.apps.import.progress.importing_local',
    fallback: 'Importing local image...',
  },
  'Image import complete': {
    key: 'sys.apps.import.progress.import_complete',
    fallback: 'Image import complete',
  },
  'Image already present': {
    key: 'sys.apps.import.progress.image_present',
    fallback: 'Image already present',
  },
  'No image to pull': {
    key: 'sys.apps.import.progress.no_image',
    fallback: 'No image to pull',
  },
  'Registering application...': {
    key: 'sys.apps.import.progress.registering_app',
    fallback: 'Registering application...',
  },
  'Saving app registry...': {
    key: 'sys.apps.import.progress.saving_registry',
    fallback: 'Saving app registry...',
  },
  'Registering permissions...': {
    key: 'sys.apps.import.progress.registering_permissions',
    fallback: 'Registering permissions...',
  },
  'Installation complete': {
    key: 'sys.apps.import.progress.complete',
    fallback: 'Installation complete',
  },
};

const PHASE_FALLBACKS: Record<string, string> = {
  validating: 'Validating...',
  pulling: 'Processing image...',
  registering: 'Registering application...',
  complete: 'Installation complete',
  error: 'Installation failed',
};

const PHASE_LABEL_FALLBACKS: Record<string, string> = {
  validating: 'Validating',
  pulling: 'Image',
  registering: 'Registering',
  complete: 'Complete',
  error: 'Error',
};

export function translateInstallPhase(
  phase: string | undefined,
  t: TFunction
): string {
  const key = phase || 'validating';
  return t(`sys.apps.import.phase.${key}`, PHASE_LABEL_FALLBACKS[key] ?? key);
}

export function translateInstallProgress(
  progress: Pick<InstallProgress, 'phase' | 'message'> | null | undefined,
  t: TFunction
): string {
  if (!progress) {
    return t('sys.apps.import.preparing', 'Preparing...');
  }

  const message = progress.message?.trim();
  if (message) {
    const exact = EXACT_MESSAGE_KEYS[message];
    if (exact) {
      return t(exact.key, exact.fallback);
    }

    const downloading = message.match(/^Downloading ([\d.]+)% \(([^)]+)\)/);
    if (downloading) {
      return t(
        'sys.apps.import.progress.downloading',
        'Downloading {{percent}}% ({{size}})',
        {
          percent: downloading[1],
          size: downloading[2],
        }
      );
    }

    if (message.startsWith('Pulled ')) {
      return t('sys.apps.import.progress.pulled', 'Image pulled successfully');
    }

    if (message.startsWith('Failed:')) {
      return t('sys.apps.import.progress.pull_failed', 'Image pull failed');
    }

    if (message.startsWith('Installation failed:')) {
      return t('sys.apps.import.install_failed', 'Install Failed');
    }
  }

  const phase = progress.phase || 'validating';
  return t(
    `sys.apps.import.progress.${phase}`,
    PHASE_FALLBACKS[phase] ?? t('sys.apps.import.preparing', 'Preparing...')
  );
}
