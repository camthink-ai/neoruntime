import { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import type {
  OsdTextOverlay,
  OsdDateTimeOverlay,
  OsdImageOverlay,
} from '@/services/media';
import { osdFontUrl } from '@/services/media';
import type { StreamName } from '../hooks/useOsdConfig';

// The exact font the camera-daemon bakes text/datetime OSD with. Fallbacks are
// only used before the @font-face (injected on mount) finishes loading or if
// the device font endpoint is unreachable. Liberation Mono / Courier New are
// metric-compatible stand-ins.
const OSD_MONO_STACK =  "'OSD Mono', 'Liberation Mono', 'Courier New', monospace";

// HAL alignment enums — mirror hal_v2/include/media/hal_osd.h (0/1/2).
const HALIGN_LEFT = 0;
const HALIGN_CENTER = 1;
const HALIGN_RIGHT = 2;
const VALIGN_TOP = 0;
const VALIGN_CENTER = 1;
const VALIGN_BOTTOM = 2;

type CornerKey = 'tl' | 'tr' | 'bl' | 'br';
export const CORNERS: {
  key: CornerKey;
  x: number;
  y: number;
  hAlign: number;
  vAlign: number;
}[] = [
  { key: 'tl', x: 0.02, y: 0.02, hAlign: HALIGN_LEFT, vAlign: VALIGN_TOP },
  { key: 'tr', x: 0.98, y: 0.02, hAlign: HALIGN_RIGHT, vAlign: VALIGN_TOP },
  { key: 'bl', x: 0.02, y: 0.98, hAlign: HALIGN_LEFT, vAlign: VALIGN_BOTTOM },
  { key: 'br', x: 0.98, y: 0.98, hAlign: HALIGN_RIGHT, vAlign: VALIGN_BOTTOM },
];

const clamp = (n: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, n));

type Geometry = {
  renderedW: number;
  renderedH: number;
  offX: number;
  offY: number;
  scale: number;
};

// object-contain letterbox math: where the stream actually paints inside the
// container, so overlay handles land on the baked OSD, not the letterbox bars.
function computeGeometry(
  cW: number,
  cH: number,
  vw: number,
  vh: number,
  objectFit: 'contain' | 'cover' = 'contain'
): Geometry {
  const scale = cW > 0 && cH > 0
    ? objectFit === 'cover'
      ? Math.max(cW / vw, cH / vh)
      : Math.min(cW / vw, cH / vh)
    : 0;
  const renderedW = vw * scale;
  const renderedH = vh * scale;
  return {
    renderedW,
    renderedH,
    offX: (cW - renderedW) / 2,
    offY: (cH - renderedH) / 2,
    scale,
  };
}

type DragState =
  | {
      kind: 'move';
      overlayType: 'text' | 'image';
      id: string;
      startPX: number;
      startPY: number;
      startOX: number;
      startOY: number;
      // Normalized box size so the whole box (not just the anchor) stays
      // inside the rendered video area while dragging.
      boxWN: number;
      boxHN: number;
      // HAL alignment of the overlay being moved — the anchor x/y is
      // alignment-independent, but the clamp bounds depend on which edge of the
      // box sits on the anchor.
      hAlign: number;
      vAlign: number;
    }
  | {
      kind: 'resize-text';
      id: string;
      originX: number;
      originY: number;
      startDist: number;
      startFontSize: number;
    }
  | {
      kind: 'resize-image';
      id: string;
      startPX: number;
      startPY: number;
      startW: number;
      startH: number;
    };

interface OsdOverlayLayerProps {
  activeStream: StreamName;
  textOverlays: OsdTextOverlay[];
  datetimeOverlays: OsdDateTimeOverlay[];
  imageOverlays: OsdImageOverlay[];
  streamWidth?: number;
  streamHeight?: number;
  selectedId: string | null;
  onSelect: (id: string | null) => void;
  onUpdateText: (id: string, patch: Partial<OsdTextOverlay>) => void;
  onUpdateDateTime: (id: string, patch: Partial<OsdDateTimeOverlay>) => void;
  onUpdateImage: (id: string, patch: Partial<OsdImageOverlay>) => void;
}

const HANDLE = 12; // px, resize handle edge length

/**
 * Interaction layer rendered directly inside the live-video container (a
 * `relative` parent in `media/index.tsx`). The visible OSD is baked into the
 * H.264 pixels by camera-daemon, so it is not clickable — these HTML handles
 * are the proxy the user drags/resizes. object-contain letterbox math keeps
 * handles on the baked OSD.
 */
export default function OsdOverlayLayer({
  activeStream,
  textOverlays,
  // Datetime overlays are configured entirely from the sidebar (corner/anchor
  // buttons) — no on-video frame or hotspots — so these two are unused here,
  // but kept in the prop contract for the parent.
  datetimeOverlays: _datetimeOverlays,
  imageOverlays,
  streamWidth,
  streamHeight,
  selectedId,
  onSelect,
  onUpdateText,
  onUpdateDateTime: _onUpdateDateTime,
  onUpdateImage,
}: OsdOverlayLayerProps) {
  const { t } = useTranslation();
  const rootRef = useRef<HTMLDivElement | null>(null);
  const geoRef = useRef<Geometry>({
    renderedW: 0,
    renderedH: 0,
    offX: 0,
    offY: 0,
    scale: 0,
  });
  const [geo, setGeo] = useState<Geometry>(geoRef.current);
  geoRef.current = geo;
  const dragRef = useRef<DragState | null>(null);

  // Text overlay currently being edited inline (double-click → type in place).
  const [editingId, setEditingId] = useState<string | null>(null);
  const taRef = useRef<HTMLTextAreaElement | null>(null);

  // Inject the device OSD font (@font-face 'OSD Mono') once, so the text proxy
  // renders in the identical font the stream is baked with. Idempotent: a single
  // global <style> tag is created/updated regardless of how often the layer mounts.
  useEffect(() => {
    const id = 'osd-mono-fontface';
    const css = `@font-face { font-family: 'OSD Mono'; src: url('${osdFontUrl()}') format('truetype'); font-display: block; }`;
    let el = document.getElementById(id) as HTMLStyleElement | null;
    if (!el) {
      el = document.createElement('style');
      el.id = id;
      document.head.appendChild(el);
    }
    el.textContent = css;
  }, []);

  // Track the exact geometry used by the sibling <video>. Transform updates
  // can change videoWidth/videoHeight without changing this container, and
  // Player may resolve adaptive object-fit to cover. Reading the live video
  // avoids drawing frames with stale media-status dimensions until refresh.
  useEffect(() => {
    const el = rootRef.current;
    if (!el) return;
    const video = el.parentElement?.querySelector('video') ?? null;
    const update = () => {
      const rect = el.getBoundingClientRect();
      const vw = video && video.videoWidth > 0
        ? video.videoWidth
        : streamWidth && streamWidth > 0
          ? streamWidth
          : 1920;
      const vh = video && video.videoHeight > 0
        ? video.videoHeight
        : streamHeight && streamHeight > 0
          ? streamHeight
          : 1080;
      const fit = video && getComputedStyle(video).objectFit === 'cover'
        ? 'cover'
        : 'contain';
      setGeo(computeGeometry(rect.width, rect.height, vw, vh, fit));
    };
    update();
    const ro = new ResizeObserver(update);
    ro.observe(el);
    if (video) {
      ro.observe(video);
      video.addEventListener('loadedmetadata', update);
      video.addEventListener('resize', update);
    }
    const onTransformUpdated = (event: Event) => {
      const { detail } = event as CustomEvent<{ active?: boolean }>;
      if (detail?.active === false) requestAnimationFrame(update);
    };
    window.addEventListener(
      'aipc:media-transform-updating',
      onTransformUpdated
    );
    return () => {
      ro.disconnect();
      video?.removeEventListener('loadedmetadata', update);
      video?.removeEventListener('resize', update);
      window.removeEventListener(
        'aipc:media-transform-updating',
        onTransformUpdated
      );
    };
  }, [streamWidth, streamHeight]);

  // Editing/selection is per-stream; clear when the edited stream changes.
  useEffect(() => {
    setEditingId(null);
  }, [activeStream]);

  // Focus the inline editor as soon as it mounts.
  useEffect(() => {
    if (editingId && taRef.current) {
      taRef.current.focus();
      taRef.current.select();
    }
  }, [editingId]);

  // Exit the inline edit on any pointer-down outside the textarea (clicking the
  // video, the letterbox, or another overlay). The textarea's onBlur already
  // commits + clears editingId, but this guarantees the player leaves the edit
  // state even when the click lands on a pointer-events-none area that would
  // not steal focus from the textarea on its own.
  useEffect(() => {
    if (!editingId) return;
    const onDown = (e: PointerEvent) => {
      const ta = taRef.current;
      if (!ta) return;
      if (e.target instanceof Node && ta.contains(e.target)) return;
      ta.blur();
    };
    window.addEventListener('pointerdown', onDown, true);
    return () => window.removeEventListener('pointerdown', onDown, true);
  }, [editingId]);

  // Escape clears the current selection (only when not mid-drag/edit).
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && dragRef.current === null) {
        setEditingId(null);
        onSelect(null);
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [onSelect]);

  // ---- unified pointer handlers (dispatch on drag kind) ----
  const onHandlePointerMove = (e: React.PointerEvent) => {
    const d = dragRef.current;
    if (!d) return;
    if (d.kind === 'move') {
      const g = geoRef.current;
      if (g.renderedW <= 0 || g.renderedH <= 0) return;
      // Clamp the anchor so the WHOLE box stays inside the rendered video for
      // the overlay's alignment (LEFT/TOP/CENTER/RIGHT/BOTTOM bounds differ).
      const [minX, maxX] = anchorBounds(d.hAlign, d.boxWN);
      const [minY, maxY] = anchorBounds(d.vAlign, d.boxHN);
      const nx = clamp(
        d.startOX + (e.clientX - d.startPX) / g.renderedW,
        minX,
        maxX
      );
      const ny = clamp(
        d.startOY + (e.clientY - d.startPY) / g.renderedH,
        minY,
        maxY
      );
      if (d.overlayType === 'text') onUpdateText(d.id, { x: nx, y: ny });
      else onUpdateImage(d.id, { x: nx, y: ny });
    } else if (d.kind === 'resize-text') {
      const dist = Math.hypot(e.clientX - d.originX, e.clientY - d.originY);
      const ratio = dist / (d.startDist || 1);
      onUpdateText(d.id, {
        font_size: clamp(Math.round(d.startFontSize * ratio), 8, 72),
      });
    } else {
      const g = geoRef.current;
      if (g.renderedW <= 0 || g.renderedH <= 0) return;
      const nw = clamp(
        d.startW + (e.clientX - d.startPX) / g.renderedW,
        0.02,
        1
      );
      const nh = clamp(
        d.startH + (e.clientY - d.startPY) / g.renderedH,
        0.02,
        1
      );
      onUpdateImage(d.id, { width: nw, height: nh });
    }
  };

  const onHandlePointerUp = (e: React.PointerEvent) => {
    if (dragRef.current) {
      try {
        (e.currentTarget as Element).releasePointerCapture(e.pointerId);
      } catch {
        /* noop */
      }
    }
    dragRef.current = null;
  };

  const startMove = (
    e: React.PointerEvent,
    overlayType: 'text' | 'image',
    id: string,
    ox: number,
    oy: number,
    boxWN: number,
    boxHN: number,
    hAlign: number,
    vAlign: number
  ) => {
    e.stopPropagation();
    (e.currentTarget as Element).setPointerCapture(e.pointerId);
    dragRef.current = {
      kind: 'move',
      overlayType,
      id,
      startPX: e.clientX,
      startPY: e.clientY,
      startOX: ox,
      startOY: oy,
      boxWN,
      boxHN,
      hAlign,
      vAlign,
    };
    onSelect(id);
  };

  const startTextResize = (
    e: React.PointerEvent,
    id: string,
    originX: number,
    originY: number,
    fontSize: number
  ) => {
    e.stopPropagation();
    (e.currentTarget as Element).setPointerCapture(e.pointerId);
    dragRef.current = {
      kind: 'resize-text',
      id,
      originX,
      originY,
      startDist: Math.hypot(e.clientX - originX, e.clientY - originY) || 1,
      startFontSize: fontSize,
    };
    onSelect(id);
  };

  const startImageResize = (
    e: React.PointerEvent,
    id: string,
    w: number,
    h: number
  ) => {
    e.stopPropagation();
    (e.currentTarget as Element).setPointerCapture(e.pointerId);
    dragRef.current = {
      kind: 'resize-image',
      id,
      startPX: e.clientX,
      startPY: e.clientY,
      startW: w,
      startH: h,
    };
    onSelect(id);
  };

  const screenX = (nx: number) => geo.offX + nx * geo.renderedW;
  const screenY = (ny: number) => geo.offY + ny * geo.renderedH;

  // HAL treats (x, y) as the alignment ANCHOR: h_align/v_align decide which
  // edge/center of the overlay sits on it (LEFT/TOP → top-left, CENTER →
  // middle, RIGHT/BOTTOM → bottom-right). camera-daemon bakes accordingly, so
  // the proxy box must offset from the anchor the same way — otherwise a
  // non-LEFT/TOP overlay (e.g. an imported/API-created one) renders its frame
  // offset from the baked pixels. The web UI only creates LEFT/TOP overlays,
  // but this keeps the proxy WYSIWYG for any alignment.
  const alignedBoxTopLeft = (
    anchorNX: number,
    anchorNY: number,
    hAlign: number,
    vAlign: number,
    boxW: number,
    boxH: number
  ): { left: number; top: number } => {
    const ax = screenX(anchorNX);
    const ay = screenY(anchorNY);
    const left =      hAlign === HALIGN_RIGHT
        ? ax - boxW
        : hAlign === HALIGN_CENTER
          ? ax - boxW / 2
          : ax; // HALIGN_LEFT
    const top =      vAlign === VALIGN_BOTTOM
        ? ay - boxH
        : vAlign === VALIGN_CENTER
          ? ay - boxH / 2
          : ay; // VALIGN_TOP
    return { left, top };
  };

  // Valid normalized anchor range keeping the WHOLE box inside [0,1] for the
  // given alignment (used to clamp dragging). LEFT: anchor is the left edge so
  // max = 1 - boxN; CENTER: anchor is the middle; RIGHT: anchor is the right
  // edge so min = boxN. Returns [min, max].
  const anchorBounds = (align: number, boxN: number): [number, number] => {
    if (align === HALIGN_RIGHT || align === VALIGN_BOTTOM) return [boxN, 1];
    if (align === HALIGN_CENTER || align === VALIGN_CENTER) return [boxN / 2, 1 - boxN / 2];
    return [0, 1 - boxN]; // LEFT / TOP
  };

  return (
    <div
      ref={rootRef}
      className="pointer-events-none absolute inset-0 z-20 select-none"
    >
      {geo.scale > 0 && (
        <>
          {/* Image overlays (behind) */}
          {imageOverlays
            .filter(o => o.enabled)
            .map(o => {
              const w = o.width ?? 0.12;
              const h = o.height ?? 0.08;
              const hAlign = o.h_align ?? HALIGN_LEFT;
              const vAlign = o.v_align ?? VALIGN_TOP;
              const boxW = w * geo.renderedW;
              const boxH = h * geo.renderedH;
              const { left, top } = alignedBoxTopLeft(
                o.x ?? 0,
                o.y ?? 0,
                hAlign,
                vAlign,
                boxW,
                boxH
              );
              const selected = selectedId === o.id;
              return (
                <div
                  key={o.id}
                  className={`pointer-events-auto absolute cursor-move rounded-sm border transition-shadow ${
                    selected
                      ? 'border-sky-400 shadow-[0_0_0_1px_rgba(56,189,248,0.6)]'
                      : 'border-amber-400/60 hover:border-amber-300'
                  }`}
                  style={{ left, top, width: boxW, height: boxH }}
                  onPointerDown={e => startMove(
                      e,
                      'image',
                      o.id,
                      o.x ?? 0,
                      o.y ?? 0,
                      w,
                      h,
                      hAlign,
                      vAlign
                    )}
                  onPointerMove={onHandlePointerMove}
                  onPointerUp={onHandlePointerUp}
                >
                  {o.image_path ? null : (
                    <span className="pointer-events-none flex h-full w-full items-center justify-center text-[10px] italic text-amber-200/70">
                      {t('sys.media_settings.osd_upload_image', 'Upload Image')}
                    </span>
                  )}
                  {selected && (
                    <span
                      className="pointer-events-auto absolute -bottom-1 -right-1 cursor-nwse-resize rounded-sm border border-white bg-amber-400 shadow-md transition-transform hover:scale-125"
                      style={{ width: HANDLE, height: HANDLE }}
                      onPointerDown={e => startImageResize(
                          e,
                          o.id,
                          o.width ?? 0.12,
                          o.height ?? 0.08
                        )}
                      onPointerMove={onHandlePointerMove}
                      onPointerUp={onHandlePointerUp}
                    />
                  )}
                </div>
              );
            })}

          {/* Text overlays — the baked OSD is always visible on the stream
              (never suppressed), so the frame is a TRANSPARENT drag handle, not
              a proxy. Dragging the frame moves the anchor; the baked text lags
              by the debounce + encode delay then settles exactly on the frame.
              Double-click to type in place. */}
          {textOverlays
            .filter(o => o.enabled)
            .map(o => {
              const fontSize = o.font_size ?? 32;
              const screenFontPx = fontSize * geo.scale;
              const text = o.text || '';
              // text_color is ARGB (a=hi byte). Match the baked OSD color so the
              // live typed text reads over the video without an opaque fill.
              const tc = o.text_color ?? 0xffffffff;
              const textColorCss = `rgba(${(tc >> 16) & 0xff}, ${(tc >> 8) & 0xff}, ${tc & 0xff}, ${(((tc >>> 24) & 0xff) / 255).toFixed(3)})`;
              const hAlign = o.h_align ?? HALIGN_LEFT;
              const vAlign = o.v_align ?? VALIGN_TOP;
              const selected = selectedId === o.id;
              const editing = editingId === o.id;
              // Vertical breathing room so the frame surrounds the baked text
              // instead of its top border crossing the glyphs. HAL places the
              // text's top edge on the y anchor; the old boxH (1.3×) made the
              // frame taller than the text while pinning its top to the anchor,
              // so the top border sat right on the text. padY lifts the visible
              // frame above the anchor — purely visual, the anchor geometry
              // (drag + exit-no-jump) is unchanged.
              const padY = screenFontPx * 0.35;
              const boxW = editing
                ? Math.max(text.length * 0.6 * screenFontPx, 168)
                : Math.max(text.length * 0.6 * screenFontPx, 3 * screenFontPx);
              const boxH = Math.max(screenFontPx + 2 * padY, 16);
              const { left, top } = alignedBoxTopLeft(
                o.x ?? 0,
                o.y ?? 0,
                hAlign,
                vAlign,
                boxW,
                boxH
              );
              // Center the baked text inside the padded frame: TOP shifts the
              // frame up by padY, BOTTOM down by padY, CENTER stays put.
              const visualOffsetY =                vAlign === VALIGN_TOP
                  ? -padY
                  : vAlign === VALIGN_BOTTOM
                    ? padY
                    : 0;
              const visualTop = top + visualOffsetY;
              // Normalized box size, used to clamp dragging inside the video.
              const boxWN =                geo.renderedW > 0 ? clamp(boxW / geo.renderedW, 0, 1) : 0;
              const boxHN =                geo.renderedH > 0 ? clamp(boxH / geo.renderedH, 0, 1) : 0;
              return (
                <div
                  key={o.id}
                  className={`pointer-events-auto absolute flex items-center rounded-sm border ${
                    editing
                      ? 'border-sky-400 bg-transparent shadow-[0_0_0_2px_rgba(56,189,248,0.7)]'
                      : selected
                        ? 'cursor-move border-sky-400 bg-transparent shadow-[0_0_0_1px_rgba(56,189,248,0.5)]'
                        : 'cursor-move border-white/30 bg-transparent hover:border-white/70'
                  }`}
                  style={{
                    left,
                    top: visualTop,
                    width: boxW,
                    height: boxH,
                    fontSize: screenFontPx,
                  }}
                  onPointerDown={
                    editing
                      ? undefined
                      : e => startMove(
                            e,
                            'text',
                            o.id,
                            o.x ?? 0,
                            o.y ?? 0,
                            boxWN,
                            boxHN,
                            hAlign,
                            vAlign
                          )
                  }
                  onPointerMove={editing ? undefined : onHandlePointerMove}
                  onPointerUp={editing ? undefined : onHandlePointerUp}
                  onDoubleClick={e => {
                    e.stopPropagation();
                    onSelect(o.id);
                    setEditingId(o.id);
                  }}
                >
                  {editing ? (
                    <textarea
                      ref={taRef}
                      defaultValue={text}
                      rows={1}
                      className="absolute inset-0 h-full w-full resize-none border-0 bg-transparent px-1 leading-none outline-none"
                      style={{
                        fontSize: screenFontPx,
                        fontFamily: OSD_MONO_STACK,
                        color: textColorCss,
                        caretColor: textColorCss,
                      }}
                      onPointerDown={e => e.stopPropagation()}
                      onBlur={e => {
                        onUpdateText(o.id, { text: e.currentTarget.value });
                        setEditingId(null);
                      }}
                      onKeyDown={e => {
                        e.stopPropagation();
                        if (e.key === 'Enter' && !e.shiftKey) {
                          e.preventDefault();
                          e.currentTarget.blur();
                        } else if (e.key === 'Escape') {
                          e.currentTarget.blur();
                        }
                      }}
                    />
                  ) : (
                    <>
                      {/* The frame is a transparent drag handle — the real text is
                          the always-baked OSD showing through. Only render a faint
                          placeholder for empty overlays so the (invisible) frame
                          stays findable + the double-click target stays obvious. */}
                      {!text && (
                        <span className="pointer-events-none truncate px-1 text-[10px] italic text-white/55">
                          {t(
                            'sys.media_settings.osd_double_click_edit',
                            'double-click to edit'
                          )}
                        </span>
                      )}
                      {selected && (
                        <span
                          className="pointer-events-auto absolute -bottom-1 -right-1 cursor-nwse-resize rounded-sm border border-white bg-sky-400 shadow-md transition-transform hover:scale-125"
                          style={{ width: HANDLE, height: HANDLE }}
                          onPointerDown={e => startTextResize(e, o.id, left, top, fontSize)}
                          onPointerMove={onHandlePointerMove}
                          onPointerUp={onHandlePointerUp}
                        />
                      )}
                    </>
                  )}
                </div>
              );
            })}

          {/* DateTime overlays have no on-video frame — the baked stream renders
              the live time. The corner/anchor is set from the sidebar, not via
              on-video hotspots. */}
        </>
      )}
    </div>
  );
}
