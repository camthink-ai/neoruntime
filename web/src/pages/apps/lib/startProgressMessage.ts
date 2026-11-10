import type { TFunction } from 'i18next';
import type { InstallProgress } from '@/hooks/useApps';

const PHASES: Record<string, { key: string; fallback: string }> = {
  checking_image: {
    key: 'sys.apps.start_progress.checking_image',
    fallback: 'Checking application image...',
  },
  restoring_image: {
    key: 'sys.apps.start_progress.restoring_image',
    fallback: 'Restoring application image...',
  },
  pulling_image: {
    key: 'sys.apps.start_progress.pulling_image',
    fallback: 'Downloading application image...',
  },
  image_ready: {
    key: 'sys.apps.start_progress.image_ready',
    fallback: 'Application image is ready',
  },
  preloading_models: {
    key: 'sys.apps.start_progress.preloading_models',
    fallback: 'Preloading AI models...',
  },
  creating_container: {
    key: 'sys.apps.start_progress.creating_container',
    fallback: 'Creating application container...',
  },
  starting_container: {
    key: 'sys.apps.start_progress.starting_container',
    fallback: 'Starting application container...',
  },
  complete: {
    key: 'sys.apps.start_progress.complete',
    fallback: 'Application started',
  },
  error: {
    key: 'sys.apps.start_progress.error',
    fallback: 'Application start failed',
  },
};

export function translateStartProgress(
  progress: Pick<InstallProgress, 'phase'> | null | undefined,
  t: TFunction
): string {
  const phase = progress?.phase ?? 'checking_image';
  const entry = PHASES[phase] ?? PHASES.checking_image;
  return t(entry.key, entry.fallback);
}
