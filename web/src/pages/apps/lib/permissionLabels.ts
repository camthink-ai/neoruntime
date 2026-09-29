// Raw permission identifiers (stream ids, network modes) → i18n keys.
// Unknown values surface as-is at call sites rather than being guessed.
// Stream ids are bare ("main"/"sub"/"third", per streamsApi); the ".raw"
// aliases are kept for older permission payloads.
const STREAM_LABEL_KEYS = {
  main: 'sys.media_settings.main_stream',
  sub: 'sys.media_settings.sub_stream',
  third: 'sys.media_settings.third_stream',
  'main.raw': 'sys.media_settings.main_stream',
  'sub.raw': 'sys.media_settings.sub_stream',
  'third.raw': 'sys.media_settings.third_stream',
} as const;

const NETWORK_MODE_LABEL_KEYS = {
  host: 'sys.apps.import.network_host',
  isolated: 'sys.apps.import.network_isolated',
} as const;

export function videoStreamLabelKey(stream: string) {
  return STREAM_LABEL_KEYS[stream as keyof typeof STREAM_LABEL_KEYS];
}

export function networkModeLabelKey(mode: string) {
  return NETWORK_MODE_LABEL_KEYS[mode as keyof typeof NETWORK_MODE_LABEL_KEYS];
}
