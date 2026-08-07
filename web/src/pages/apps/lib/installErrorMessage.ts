import type { TFunction } from 'i18next';

interface ErrorMatcher {
  pattern: RegExp;
  translate: (match: RegExpMatchArray, t: TFunction) => string;
}

const ERROR_MATCHERS: ErrorMatcher[] = [
  {
    pattern: /^Failed to parse form:\s*(.+)$/is,
    translate: (m, t) => t('sys.apps.import.upload_errors.parse_form', {
        detail: m[1].trim(),
        defaultValue: 'Could not parse the upload request. {{detail}}',
      }),
  },
  {
    pattern: /^No file uploaded:\s*(.+)$/is,
    translate: (m, t) => t('sys.apps.import.upload_errors.no_file', {
        detail: m[1].trim(),
        defaultValue: 'No file was selected. {{detail}}',
      }),
  },
  {
    pattern: /^Only \.tar, \.tar\.gz or \.tgz files are allowed$/i,
    translate: (_m, t) => t('sys.apps.import.upload_errors.invalid_image_type', {
        defaultValue: 'Only .tar, .tar.gz or .tgz image files are supported.',
      }),
  },
  {
    pattern:
      /^Failed to (?:create upload directory|create file|save file|read file):\s*(.+)$/is,
    translate: (m, t) => t('sys.apps.import.upload_errors.save_failed', {
        detail: m[1].trim(),
        defaultValue:
          'Could not save the file. Check available storage and try again. {{detail}}',
      }),
  },
  {
    pattern: /^Only \.yaml or \.yml files are allowed$/i,
    translate: (_m, t) => t('sys.apps.import.upload_errors.invalid_manifest_type', {
        defaultValue: 'Only .yaml or .yml manifest files are supported.',
      }),
  },
  {
    pattern: /^Invalid YAML format:\s*(.+)$/is,
    translate: (m, t) => t('sys.apps.import.upload_errors.invalid_yaml', {
        detail: m[1].trim(),
        defaultValue:
          'Invalid manifest YAML. Please check the syntax. {{detail}}',
      }),
  },
  {
    pattern: /^manifest metadata\.id is required$/i,
    translate: (_m, t) => t('sys.apps.import.upload_errors.manifest_id_required', {
        defaultValue: 'app.yaml must include metadata.id.',
      }),
  },
  {
    pattern: /^Failed to load manifest:\s*(.+)$/is,
    translate: (m, t) => t('sys.apps.import.errors.load_manifest', {
        detail: m[1].trim(),
        defaultValue:
          'Could not read the app configuration file. Make sure app.yaml exists and is valid. {{detail}}',
      }),
  },
  {
    pattern: /^Invalid manifest:\s*(.+)$/is,
    translate: (m, t) => t('sys.apps.import.errors.invalid_manifest', {
        detail: m[1].trim(),
        defaultValue:
          'The app configuration is invalid. Please check app.yaml. {{detail}}',
      }),
  },
  {
    pattern: /^Invalid seccomp profile:\s*(.+)$/is,
    translate: (m, t) => t('sys.apps.import.errors.invalid_seccomp', {
        detail: m[1].trim(),
        defaultValue:
          'Device security policy check failed. Please try again later or contact support.',
      }),
  },
  {
    pattern: /^App (.+?) already exists\.\s*Use force=true to overwrite\./i,
    translate: (m, t) => t('sys.apps.import.errors.app_exists', {
        appId: m[1].trim(),
        defaultValue:
          'App "{{appId}}" is already installed. Uninstall it from the app list, then try installing again.',
      }),
  },
  {
    pattern: /^Failed to pull image:\s*(.+)$/is,
    translate: (m, t) => t('sys.apps.import.errors.pull_image', {
        detail: m[1].trim(),
        defaultValue:
          'Could not download the container image. Check the image address and network connection. {{detail}}',
      }),
  },
  {
    pattern: /^Failed to import image:\s*(.+)$/is,
    translate: (m, t) => t('sys.apps.import.errors.import_image', {
        detail: m[1].trim(),
        defaultValue:
          'Could not import the image file. Make sure the file is complete (.tar / .tar.gz) and try again. {{detail}}',
      }),
  },
  {
    pattern:
      /^Install verify failed: manifest\.image (.+?) is not resolvable after import.*$/is,
    translate: (m, t) => t('sys.apps.import.errors.install_verify_failed', {
        image: m[1].trim(),
        detail: m[0].trim(),
        defaultValue:
          'The image "{{image}}" in app.yaml could not be verified after import. Ensure the image tar matches the manifest.',
      }),
  },
  {
    pattern:
      /^No image tar was uploaded and manifest\.image "(.+?)" is not present in containerd\./is,
    translate: (m, t) => t('sys.apps.import.errors.image_missing_offline', {
        image: m[1].trim(),
        detail: m[0].trim(),
        defaultValue:
          'No image tar was uploaded and "{{image}}" is not on the device. Upload the image tar or import the image first.',
      }),
  },
  {
    pattern: /^Failed to register app:\s*(.+)$/is,
    translate: (m, t) => t('sys.apps.import.errors.register_app', {
        detail: m[1].trim(),
        defaultValue:
          'Could not register the app. Please try again later. {{detail}}',
      }),
  },
  {
    pattern: /^Failed to create instance directory:\s*(.+)$/is,
    translate: (m, t) => t('sys.apps.import.errors.create_instance_dir', {
        detail: m[1].trim(),
        defaultValue:
          'Could not create the app data folder. Check available storage on the device. {{detail}}',
      }),
  },
  {
    pattern: /^Installation failed:\s*(.+)$/is,
    translate: (m, t) => translateInstallError(m[1].trim(), t),
  },
  {
    pattern:
      /image.*not found|pull access denied|manifest unknown|name unknown/i,
    translate: (m, t) => t('sys.apps.import.errors.image_not_found', {
        detail: m[0].trim(),
        defaultValue:
          'Image not found or access denied. Please check the image name. {{detail}}',
      }),
  },
  {
    pattern: /timeout|timed out/i,
    translate: (_m, t) => t('sys.apps.import.errors.timeout', {
        defaultValue: 'Installation timed out. Please try again.',
      }),
  },
  {
    pattern: /disk.*full|no space/i,
    translate: (_m, t) => t('sys.apps.import.errors.no_space', {
        defaultValue: 'Not enough disk space to install the app.',
      }),
  },
];

function readApiErrorPayload(error: unknown): string | undefined {
  if (!error || typeof error !== 'object') {
    return typeof error === 'string' ? error : undefined;
  }

  const response = (error as { response?: { data?: Record<string, unknown> } })
    .response?.data;

  const detail = response?.error;
  if (detail && typeof detail === 'object' && 'detail' in detail) {
    const nested = (detail as { detail?: unknown }).detail;
    if (typeof nested === 'string' && nested.trim()) return nested.trim();
  }

  if (typeof response?.message === 'string' && response.message.trim()) {
    return response.message.trim();
  }

  if (error instanceof Error && error.message.trim()) {
    return error.message.trim();
  }

  return undefined;
}

export function translateInstallError(
  error: string | undefined | null,
  t: TFunction
): string {
  const raw = error?.trim();
  if (!raw) {
    return t('sys.apps.import.errors.empty', {
      defaultValue: 'An unknown error occurred during installation.',
    });
  }

  for (const matcher of ERROR_MATCHERS) {
    const match = raw.match(matcher.pattern);
    if (match) {
      return matcher.translate(match, t);
    }
  }

  return t('sys.apps.import.errors.unknown', {
    detail: raw,
    defaultValue: 'Installation failed: {{detail}}',
  });
}

export function resolveInstallApiError(error: unknown, t: TFunction): string {
  return translateInstallError(readApiErrorPayload(error), t);
}
