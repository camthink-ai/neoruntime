// @refresh reset
import { useCallback, useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import {
  fetchPrivacyMaskConfig,
  updatePrivacyMaskConfig,
  type PrivacyMaskConfig,
  type PrivacyMaskRegion,
} from '@/services/media';
import { useSaveStatus, type SaveStatus } from './useSaveStatus';

// Coarse-vs-fine mutual exclusion: a coarse target already covers its fine
// counterpart (face ⊂ person, license_plate ⊂ vehicle), so enabling one
// disables the other — exactly one granularity per pair.
const DPM_MUTEX: Record<string, string> = {
  person: 'face',
  face: 'person',
  vehicle: 'license_plate',
  license_plate: 'vehicle',
};

// The HAL does not persist region ids (HalPrivacyMaskItem.id is a non-owning
// const char*; the daemon only keeps it alive for the duration of the SET
// call). After a reload/daemon restart, GET often returns every region with an
// empty (or duplicated) id — and handleDeleteRegion filters by id, so deleting
// one region would remove all empty-id regions at once. Normalize on load:
// give every region a stable-within-session unique id, leaving already-unique
// ids untouched. Ids are ephemeral client keys (the device identifies regions
// by polygon, not id), so regenerating them across reloads is safe.
const ensureUniqueIds = (regions: PrivacyMaskRegion[]): PrivacyMaskRegion[] => {
  const seen = new Set<string>();
  return regions.map((r, i) => {
    if (r.id && !seen.has(r.id)) {
      seen.add(r.id);
      return r;
    }
    let id = `region_${Date.now()}_${i}`;
    while (seen.has(id)) id += '_x';
    seen.add(id);
    return { ...r, id };
  });
};

export interface PendingPoint {
  x: number;
  y: number;
}

export interface PrivacyMaskController {
  config: PrivacyMaskConfig | null;
  loading: boolean;
  saveStatus: SaveStatus;
  drawingRegion: boolean;
  activeRegion: string | null;
  pendingPoints: PendingPoint[];
  setActiveRegion: (id: string | null) => void;
  // Static-mask config controls
  handleToggleEnabled: (enabled: boolean) => void;
  handleBlurRadiusChange: (value: number[]) => void;
  handleColorChange: (e: React.ChangeEvent<HTMLInputElement>) => void;
  handleDeleteRegion: (id: string) => void;
  handleToggleRegion: (id: string, enabled: boolean) => void;
  // DPM controls
  handleDpmToggle: (dpm_enabled: boolean) => void;
  handleDpmModeChange: (dpm_mode: string) => void;
  handleDpmLabelToggle: (label: string) => void;
  handleDpmColorChange: (e: React.ChangeEvent<HTMLInputElement>) => void;
  // Drawing controls (normalized 0..1 over the full frame)
  startDrawing: () => void;
  addPoint: (nx: number, ny: number) => void;
  closePolygon: () => void;
  moveVertex: (
    regionId: string,
    vertexIdx: number,
    nx: number,
    ny: number
  ) => void;
  // Commit the in-flight vertex drag to the backend. moveVertex only updates
  // local UI during the drag; this fires one PUT on pointer up.
  commitVertexMove: () => void;
  cancelDrawing: () => void;
}

/**
 * Owns privacy-mask state shared by the live-video interaction layer
 * (`PrivacyMaskOverlayLayer`) and the sidebar form (`PrivacyMaskSettings`).
 * Lifted to the common parent (`media/index.tsx`) so the polygon interaction
 * renders directly over the `Player` — no portal, no separate canvas preview.
 *
 * Fetches only while the privacy tab is active (`enabled`). Saves are
 * debounced (400ms) and coalesced into one PUT; a burst that touched DPM
 * arm-state refetches to re-sync the toggle honestly (#3 — the device may
 * refuse the arm and leave dpm_enabled false server-side).
 */
export function usePrivacyMaskConfig(enabled: boolean): PrivacyMaskController {
  const { t } = useTranslation();
  const [config, setConfig] = useState<PrivacyMaskConfig | null>(null);
  const [loading, setLoading] = useState(true);
  const [drawingRegion, setDrawingRegion] = useState(false);
  const [activeRegion, setActiveRegion] = useState<string | null>(null);
  const [pendingPoints, setPendingPoints] = useState<PendingPoint[]>([]);
  const { saveStatus, markSaving, markSaved, markError } = useSaveStatus();

  // Coalesced debounced PUT payload + a flag for whether the current burst
  // touched DPM arm-state. Only a burst that changed dpm_enabled/dpm_labels
  // can leave the optimistic local state out of sync with the device, so only
  // then do we refetch to re-sync the toggle honestly (#3).
  const pendingUpdateRef = useRef<Partial<PrivacyMaskConfig>>({});
  const dpmTouchedRef = useRef(false);
  const debounceRef = useRef<ReturnType<typeof setTimeout>>(undefined);

  // The PUT payload currently in flight (set when the debounced save fires,
  // cleared once the round — PUT + optional re-sync — settles). A GET request
  // issued before the toggle (initial mount, tab re-entry, window focus) can
  // resolve *while* the PUT is still arming the device, returning the pre-arm
  // dpm_enabled=false. Blindly applying that stale response reverts the
  // optimistic ON → the "打开先关闭再打开" flicker. Only the authoritative
  // post-PUT re-sync (loadConfig(true)) may correct arm-state; every other GET
  // must overlay the pending + in-flight optimistic edits on the fetched data.
  const inFlightRef = useRef<Partial<PrivacyMaskConfig> | null>(null);

  // Latest config mirror so commitVertexMove can read the post-drag regions
  // without depending on `config` (which would re-create the callback every
  // move and defeat the drag's stable listener closure).
  const configRef = useRef<PrivacyMaskConfig | null>(null);
  configRef.current = config;

  // Apply a fetched config. `authoritative` marks the post-PUT re-sync — the
  // only GET allowed to correct DPM arm-state (it reflects the device's actual
  // post-arm dpm_enabled_). Non-authoritative GETs (initial mount, tab
  // re-entry) overlay pending + in-flight optimistic edits so a stale
  // pre-arm response can never revert a toggle the user just flipped.
  const applyFetchedConfig = useCallback(
    (data: PrivacyMaskConfig, authoritative: boolean) => {
      const merged: PrivacyMaskConfig = {
        ...data,
        regions: ensureUniqueIds(data.regions ?? []),
      };
      if (!authoritative) {
        const pending = pendingUpdateRef.current;
        const inFlight = inFlightRef.current;
        if (pending) Object.assign(merged, pending);
        if (inFlight) Object.assign(merged, inFlight);
      } else {
        // The re-sync has spoken; the in-flight intent is resolved.
        inFlightRef.current = null;
      }
      setConfig(merged);
    },
    []
  );

  const loadConfig = useCallback(
    async (authoritative = false) => {
      try {
        const data = await fetchPrivacyMaskConfig();
        applyFetchedConfig(data, authoritative);
      } catch {
        toast.error(t('sys.media_settings.privacy_mask_save_failed'));
      } finally {
        setLoading(false);
      }
    },
    [applyFetchedConfig, t]
  );

  // Fetch only while the privacy tab is active.
  useEffect(() => {
    if (!enabled) return;
    let cancelled = false;
    (async () => {
      try {
        const data = await fetchPrivacyMaskConfig();
        if (cancelled) return;
        applyFetchedConfig(data, false);
      } catch {
        if (!cancelled) toast.error(t('sys.media_settings.privacy_mask_save_failed'));
      } finally {
        if (!cancelled) setLoading(false);
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [enabled, applyFetchedConfig, t]);

  // Flush any pending debounced PUT on unmount / tab switch.
  useEffect(
    () => () => {
      if (debounceRef.current) clearTimeout(debounceRef.current);
      const payload = pendingUpdateRef.current;
      if (Object.keys(payload).length > 0) {
        pendingUpdateRef.current = {};
        dpmTouchedRef.current = false;
        markSaving();
        updatePrivacyMaskConfig(payload)
          .then(markSaved)
          .catch(async () => {
            markError();
            await loadConfig(true);
            toast.error(t('sys.media_settings.privacy_mask_save_failed'));
          });
      }
    },
    [enabled, loadConfig, markError, markSaved, markSaving, t]
  );

  const saveConfig = useCallback(
    (updated: Partial<PrivacyMaskConfig>) => {
      if (debounceRef.current) clearTimeout(debounceRef.current);
      // Merge into the pending payload so a rapid burst coalesces into one PUT.
      // Track whether the burst touched DPM arm-state — those are the only
      // changes whose effective result can differ from the optimistic local state.
      pendingUpdateRef.current = { ...pendingUpdateRef.current, ...updated };
      if ('dpm_enabled' in updated || 'dpm_labels' in updated) {
        dpmTouchedRef.current = true;
      }
      debounceRef.current = setTimeout(async () => {
        const payload = pendingUpdateRef.current;
        const dpmTouched = dpmTouchedRef.current;
        pendingUpdateRef.current = {};
        dpmTouchedRef.current = false;
        // Record the in-flight payload so a stale non-authoritative GET
        // resolving during this round overlays these values instead of
        // reverting them. Cleared by the authoritative re-sync (or finally).
        inFlightRef.current = payload;
        markSaving();
        try {
          await updatePrivacyMaskConfig(payload);
          if (dpmTouched) {
            // Re-sync honestly (#3): the device may have refused the arm (HEF
            // contention / thermal restriction) → dpm_enabled_ is false
            // server-side. This authoritative GET is the only one allowed to
            // correct arm-state — it applies the device's truth (which may
            // differ from the optimistic request) and clears the in-flight
            // intent. A plain (non-authoritative) GET would have been blocked
            // from reverting the toggle by applyFetchedConfig.
            await loadConfig(true);
          }
          markSaved();
        } catch {
          markError();
          await loadConfig(true);
          toast.error(t('sys.media_settings.privacy_mask_save_failed'));
        } finally {
          inFlightRef.current = null;
        }
      }, 400);
    },
    [t, loadConfig]
  );

  const handleToggleEnabled = useCallback(
    (on: boolean) => {
      setConfig(prev => (prev ? { ...prev, enabled: on } : prev));
      saveConfig({ enabled: on });
    },
    [saveConfig]
  );

  const handleBlurRadiusChange = useCallback(
    (value: number[]) => {
      const blur_radius = value[0];
      setConfig(prev => (prev ? { ...prev, blur_radius } : prev));
      saveConfig({ blur_radius });
    },
    [saveConfig]
  );

  const handleColorChange = useCallback(
    (e: React.ChangeEvent<HTMLInputElement>) => {
      const hex = e.target.value.replace('#', '');
      const color =        (parseInt(hex.substring(0, 2), 16) << 16)
        | (parseInt(hex.substring(2, 4), 16) << 8)
        | parseInt(hex.substring(4, 6), 16);
      setConfig(prev => (prev ? { ...prev, color } : prev));
      saveConfig({ color });
    },
    [saveConfig]
  );

  const handleDeleteRegion = useCallback(
    (id: string) => {
      if (!config) return;
      const regions = config.regions.filter(r => r.id !== id);
      setConfig({ ...config, regions });
      saveConfig({ regions });
      if (activeRegion === id) setActiveRegion(null);
    },
    [config, saveConfig, activeRegion]
  );

  const handleToggleRegion = useCallback(
    (id: string, on: boolean) => {
      if (!config) return;
      const regions = config.regions.map(r => (r.id === id ? { ...r, enabled: on } : r));
      setConfig({ ...config, regions });
      saveConfig({ regions });
    },
    [config, saveConfig]
  );

  const handleDpmToggle = useCallback(
    (dpm_enabled: boolean) => {
      setConfig(prev => (prev ? { ...prev, dpm_enabled } : prev));
      saveConfig({ dpm_enabled });
    },
    [saveConfig]
  );

  const handleDpmModeChange = useCallback(
    (dpm_mode: string) => {
      setConfig(prev => (prev ? { ...prev, dpm_mode } : prev));
      saveConfig({ dpm_mode });
    },
    [saveConfig]
  );

  const handleDpmLabelToggle = useCallback(
    (label: string) => {
      setConfig(prev => {
        if (!prev) return prev;
        const current = (prev.dpm_labels || '')
          .split(',')
          .map(s => s.trim())
          .filter(Boolean);
        let next: string[];
        if (current.includes(label)) {
          next = current.filter(l => l !== label);
        } else {
          const partner = DPM_MUTEX[label];
          next = partner ? current.filter(l => l !== partner) : current.slice();
          next.push(label);
        }
        const csv = next.join(',');
        saveConfig({ dpm_labels: csv });
        return { ...prev, dpm_labels: csv };
      });
    },
    [saveConfig]
  );

  const handleDpmColorChange = useCallback(
    (e: React.ChangeEvent<HTMLInputElement>) => {
      const hex = e.target.value.replace('#', '');
      const dpm_color =        (parseInt(hex.substring(0, 2), 16) << 16)
        | (parseInt(hex.substring(2, 4), 16) << 8)
        | parseInt(hex.substring(4, 6), 16);
      setConfig(prev => (prev ? { ...prev, dpm_color } : prev));
      saveConfig({ dpm_color });
    },
    [saveConfig]
  );

  // ---- Drawing controls (normalized 0..1 over the full frame) ----
  const startDrawing = useCallback(() => {
    setDrawingRegion(true);
    setPendingPoints([]);
  }, []);

  const addPoint = useCallback((nx: number, ny: number) => {
    setPendingPoints(prev => [...prev, { x: nx, y: ny }]);
  }, []);

  const closePolygon = useCallback(() => {
    if (!drawingRegion || pendingPoints.length < 3 || !config) return;
    // Guarantee a unique id even if two polygons close in the same millisecond
    // or an existing region already holds this timestamp — keeps delete-by-id
    // exact (one id ↔ one region).
    const existing = new Set(config.regions.map(r => r.id));
    let newId = `region_${Date.now()}`;
    while (existing.has(newId)) newId += '_x';
    const newRegion: PrivacyMaskRegion = {
      id: newId,
      name: `#${config.regions.length + 1}`,
      enabled: true,
      points_x: pendingPoints.map(p => p.x),
      points_y: pendingPoints.map(p => p.y),
    };
    const regions = [...config.regions, newRegion];
    setConfig({ ...config, regions });
    saveConfig({ regions });
    setDrawingRegion(false);
    setPendingPoints([]);
    setActiveRegion(newId);
  }, [drawingRegion, pendingPoints, config, saveConfig]);

  const moveVertex = useCallback(
    (regionId: string, vertexIdx: number, nx: number, ny: number) => {
      // Local UI only — do NOT PUT during the drag. Previously each mousemove
      // called saveConfig, which both churned React (every move rebuilt the
      // config object → the whole overlay re-rendered) and queued a debounced
      // PUT that the backend honored by re-running MediaLibrary
      // set_override_parameters on each coalesced flush — surfacing as preview
      // stutter on the /image page. The drag is committed once on pointer up
      // via commitVertexMove(). Functional setConfig keeps this callback
      // identity-stable (no `config` dep) so the drag's window listeners
      // never see a stale moveVertex, and only the dragged region object is
      // rebuilt (other regions keep referential identity → skip re-render).
      setConfig(prev => {
        if (!prev) return prev;
        const cx = Math.max(0, Math.min(1, nx));
        const cy = Math.max(0, Math.min(1, ny));
        const regions = prev.regions.map(r => {
          if (r.id !== regionId) return r;
          if (vertexIdx < 0 || vertexIdx >= r.points_x.length) return r;
          const newPx = [...r.points_x];
          const newPy = [...r.points_y];
          newPx[vertexIdx] = cx;
          newPy[vertexIdx] = cy;
          return { ...r, points_x: newPx, points_y: newPy };
        });
        return { ...prev, regions };
      });
    },
    []
  );

  // Fire a single PUT for the regions after a drag ends. Reads from
  // configRef so it always commits the latest geometry regardless of which
  // render's closure the pointer-up listener captured.
  const commitVertexMove = useCallback(() => {
    const cur = configRef.current;
    if (!cur) return;
    saveConfig({ regions: cur.regions });
  }, [saveConfig]);

  const cancelDrawing = useCallback(() => {
    setDrawingRegion(false);
    setPendingPoints([]);
  }, []);

  return {
    config,
    loading,
    saveStatus,
    drawingRegion,
    activeRegion,
    pendingPoints,
    setActiveRegion,
    handleToggleEnabled,
    handleBlurRadiusChange,
    handleColorChange,
    handleDeleteRegion,
    handleToggleRegion,
    handleDpmToggle,
    handleDpmModeChange,
    handleDpmLabelToggle,
    handleDpmColorChange,
    startDrawing,
    addPoint,
    closePolygon,
    moveVertex,
    commitVertexMove,
    cancelDrawing,
  };
}
