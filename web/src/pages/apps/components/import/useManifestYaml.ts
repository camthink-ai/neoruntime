import { useCallback, useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { appsApi } from '@/services/api';
import type { AppManifestDTO, WizardConfig } from '@/services/types';
import { resolveKnownApiError } from '../../lib/installErrorMessage';
import { applyConfigToYamlText, parseYamlToConfig } from '../../lib/yamlSync';

/** Successful POST /apps/upload-manifest body (subset the dialog consumes). */
export interface ManifestUploadResult {
  path: string;
  metadata?: {
    id: string;
    name?: string;
    version?: string;
    description?: string;
  } | null;
  manifest?: AppManifestDTO;
  multi_container?: boolean;
  /** upload-package only: original app.yaml text inside the .neoapp bundle. */
  manifest_yaml?: string;
}

export interface UseManifestYamlOptions {
  /**
   * Re-apply an upload-manifest response to the dialog state (path, meta,
   * form hydration). `flush` (YAML editor apply) and `upload` (a fresh file)
   * both replace the form outright — with live sync the text always carries
   * the current form state, so there is nothing left to merge over it.
   */
  onApplied: (
    data: ManifestUploadResult,
    ctx: { source: 'upload' | 'flush' }
  ) => void;
  /**
   * Live YAML → form hydration: the editor text parsed cleanly (debounced)
   * and maps onto this config. The shell should adopt it as both form state
   * and hydration snapshot.
   */
  onLiveParse?: (config: WizardConfig) => void;
  /** Debounce for YAML → form parsing (tests pass 0). */
  parseDelayMs?: number;
}

/**
 * Text-side state of the YAML editor view, with real-time form ↔ YAML sync.
 * The manifest file on the server stays the source of truth at flush/install
 * time; between flushes the text and the form are kept equivalent:
 *
 * - form → text: `applyConfig` AST-projects the form onto the current text,
 *   touching only managed paths so comments, unknown fields and key order
 *   survive. Suppressed while the editor is focused (never rewrite under
 *   the user's cursor) and when the text does not parse (keep their text).
 * - text → form: `setManifestText` schedules a debounced client-side parse;
 *   valid text hydrates the form via `onLiveParse`, invalid text only sets
 *   `yamlError` — the form keeps the last good state.
 *
 * Echo control: `lastSyncedTextRef` records the text this hook itself last
 * wrote (form projection or attach). A parse of exactly that text is skipped
 * — it would only re-produce the config the projection came from.
 *
 * Three texts are kept apart:
 *
 * - `manifestText`  — what the editor currently shows (may be unflushed)
 * - `appliedText`   — the text of the server-side file `manifestPath` points
 *                     to; dirty check compares against this
 * - `originalText`  — the very first upload; the diff view baseline
 */
export function useManifestYaml({
  onApplied,
  onLiveParse,
  parseDelayMs = 500,
}: UseManifestYamlOptions) {
  const { t } = useTranslation();
  const [manifestText, setManifestTextState] = useState('');
  const [appliedText, setAppliedText] = useState('');
  const [originalText, setOriginalText] = useState('');
  const [isFlushing, setIsFlushing] = useState(false);
  const [yamlError, setYamlError] = useState<string | null>(null);
  const [isFocused, setIsFocused] = useState(false);
  /** Every manifest version ever uploaded (initial + each flush) — cancel
   * cleanup must delete them all or re-uploads leak temp files. */
  const uploadedPathsRef = useRef<string[]>([]);
  /** True once a manifest file was actually attached. Preview sources
   * (tar-only / registry) show form-generated YAML with no server-side
   * file — flush must stay a no-op for them even if the text state gets
   * polluted (e.g. a controlled editor firing onChange with the generated
   * text), or leaving the YAML view would try to "re-upload" a file that
   * was never uploaded and fail on every guarded action. */
  const hasUploadRef = useRef(false);
  /** Mirror of `manifestText` for callbacks that must read the latest text
   * without re-subscribing on every keystroke. */
  const textRef = useRef('');
  /** Focus lock: while the editor is focused, form → text projection waits. */
  const focusedRef = useRef(false);
  /** Echo guard: the text this hook last wrote itself. */
  const lastSyncedTextRef = useRef('');
  const parseTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  // A pending debounce firing after unmount would set state on a dead hook.
  useEffect(
    () => () => {
      if (parseTimerRef.current) clearTimeout(parseTimerRef.current);
    },
    []
  );

  const yamlDirty = manifestText !== appliedText;
  const yamlEdited =    originalText !== '' && appliedText !== '' && appliedText !== originalText;

  const recordPath = useCallback((path: string) => {
    if (!path) return;
    uploadedPathsRef.current = [...uploadedPathsRef.current, path];
  }, []);

  const setTextState = useCallback((text: string) => {
    textRef.current = text;
    setManifestTextState(text);
  }, []);

  const scheduleParse = useCallback(
    (text: string) => {
      if (parseTimerRef.current) clearTimeout(parseTimerRef.current);
      parseTimerRef.current = setTimeout(() => {
        parseTimerRef.current = null;
        // Our own projection (or the attached upload): parsing it back would
        // only echo the config it was built from.
        if (text === lastSyncedTextRef.current) {
          setYamlError(null);
          return;
        }
        const res = parseYamlToConfig(text);
        if ('config' in res) {
          setYamlError(null);
          onLiveParse?.(res.config);
        } else {
          setYamlError(res.error);
        }
      }, parseDelayMs);
    },
    [onLiveParse, parseDelayMs]
  );

  /**
   * Editor text changed (user typing, or a controlled-value echo). Records
   * the text and schedules the debounced form hydration.
   */
  const setManifestText = useCallback(
    (text: string) => {
      setTextState(text);
      scheduleParse(text);
    },
    [scheduleParse, setTextState]
  );

  /** Focus lock for form → text projection; wired to the pane's focus root. */
  const setFocused = useCallback((focused: boolean) => {
    focusedRef.current = focused;
    setIsFocused(focused);
  }, []);

  /**
   * Project the current form state onto the editor text (AST edit of managed
   * paths only). No-op while focused, while flushing, without an uploaded
   * manifest, or when the text does not currently parse — in all those cases
   * the user's text wins and the form edit lands on the next projection.
   */
  const applyConfig = useCallback(
    (config: WizardConfig) => {
      if (!hasUploadRef.current || focusedRef.current || isFlushing) return;
      const res = applyConfigToYamlText(textRef.current, config);
      if ('error' in res) return;
      if (res.text === textRef.current) return;
      lastSyncedTextRef.current = res.text;
      setTextState(res.text);
    },
    [isFlushing, setTextState]
  );

  /** Capture the text of a manifest file the shell just uploaded. */
  const attachUpload = useCallback(
    (file: File) => {
      hasUploadRef.current = true;
      let alive = true;
      file.text().then(text => {
        if (!alive) return;
        lastSyncedTextRef.current = text;
        setYamlError(null);
        setTextState(text);
        setAppliedText(text);
        setOriginalText(text);
      });
      return () => {
        alive = false;
      };
    },
    [setTextState]
  );

  /**
   * Upload the current editor text and make it the applied version.
   * Returns the new server path on success — callers must use it in place
   * of the (stale) `manifestPath` state within the same event handler.
   * Returns ok:false (and keeps the user on the YAML view) when the server
   * rejects the text — parse/validation errors surface through the toast.
   */
  const flushYaml = useCallback(
    async (opts?: {
      silent?: boolean;
    }): Promise<{ ok: boolean; path?: string }> => {
      // Preview sources never attached a file — nothing server-side to
      // re-apply, whatever the text state says. Checked before the dirty
      // check on purpose: pollution must not trigger an upload.
      if (!hasUploadRef.current) return { ok: true };
      if (!yamlDirty || isFlushing) return { ok: true };
      setIsFlushing(true);
      try {
        const file = new File([manifestText], 'app.yaml', {
          type: 'application/x-yaml',
        });
        const res = await appsApi.uploadManifest(file);
        const data = res?.data;
        if (!data?.path) throw new Error('upload-manifest returned no path');
        uploadedPathsRef.current = [...uploadedPathsRef.current, data.path];
        setAppliedText(manifestText);
        setYamlError(null);
        onApplied(data, { source: 'flush' });
        if (!opts?.silent) {
          toast.success(
            t('sys.apps.import.yaml_applied', 'YAML changes applied')
          );
        }
        return { ok: true, path: data.path };
      } catch (err: unknown) {
        // A recognized backend error (e.g. invalid YAML) keeps its specific
        // translated copy; anything else gets the flush-context message —
        // NOT the install-flavored generic from resolveInstallApiError.
        const message = resolveKnownApiError(
          err,
          t,
          t('sys.apps.import.yaml_flush_failed', 'Failed to apply YAML changes')
        );
        toast.error(message);
        // Persist the server-side rejection inline so it stays visible
        // after the toast is gone.
        setYamlError(message);
        return { ok: false };
      } finally {
        setIsFlushing(false);
      }
    },
    [yamlDirty, isFlushing, manifestText, onApplied, t]
  );

  /** Drop all text state (dialog reset / manifest cleared). */
  const reset = useCallback(() => {
    if (parseTimerRef.current) {
      clearTimeout(parseTimerRef.current);
      parseTimerRef.current = null;
    }
    setTextState('');
    setAppliedText('');
    setOriginalText('');
    setYamlError(null);
    hasUploadRef.current = false;
    uploadedPathsRef.current = [];
    lastSyncedTextRef.current = '';
  }, [setTextState]);

  return {
    manifestText,
    setManifestText,
    originalText,
    yamlDirty,
    yamlEdited,
    isFlushing,
    yamlError,
    isFocused,
    applyConfig,
    setFocused,
    attachUpload,
    recordPath,
    flushYaml,
    reset,
    uploadedPathsRef,
  };
}
