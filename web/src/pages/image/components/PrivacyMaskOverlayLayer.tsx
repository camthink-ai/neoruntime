import {
  useEffect,
  useRef,
  useState,
  memo,
  type MouseEvent as ReactMouseEvent,
} from 'react';
import type { PrivacyMaskController } from '../hooks/usePrivacyMaskConfig';
import type { PrivacyMaskRegion } from '@/services/media';

// object-contain letterbox math: where the stream actually paints inside the
// container, so polygon handles land on the video, not the letterbox bars.
type Geometry = {
  renderedW: number;
  renderedH: number;
  offX: number;
  offY: number;
  scale: number;
};

function computeGeometry(
  cW: number,
  cH: number,
  vw: number,
  vh: number
): Geometry {
  const scale = cW > 0 && cH > 0 ? Math.min(cW / vw, cH / vh) : 0;
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

const clamp01 = (n: number) => Math.max(0, Math.min(1, n));

type Props = PrivacyMaskController & {
  streamWidth?: number;
  streamHeight?: number;
};

/**
 * Draws privacy-mask polygons directly over the live `Player`. Mirrors
 * `OsdOverlayLayer`: a pointer-events-none root with pointer-events-auto
 * shapes, letterbox geometry so normalized 0..1 frame coords map to the
 * rendered video rect. SVG (no canvas) — polygons + vertex handles.
 *
 * Click an existing polygon to select it; drag its vertex handles to fine-tune.
 * While drawing (after "Add Region"), click to place vertices and double-click
 * to close. Esc cancels.
 */
export default function PrivacyMaskOverlayLayer({
  config,
  drawingRegion,
  pendingPoints,
  activeRegion,
  setActiveRegion,
  addPoint,
  closePolygon,
  moveVertex,
  commitVertexMove,
  cancelDrawing,
  streamWidth,
  streamHeight,
}: Props) {
  const rootRef = useRef<HTMLDivElement>(null);
  const [geo, setGeo] = useState<Geometry>({
    renderedW: 0,
    renderedH: 0,
    offX: 0,
    offY: 0,
    scale: 0,
  });
  const geoRef = useRef(geo);
  geoRef.current = geo;
  const dragRef = useRef<{ regionId: string; vertexIdx: number } | null>(null);

  // Track container size; recompute letterbox geometry on resize.
  useEffect(() => {
    const el = rootRef.current;
    if (!el) return;
    // Fall back to 16:9 when the stream dims are unknown.
    const vw = streamWidth && streamWidth > 0 ? streamWidth : 16;
    const vh = streamHeight && streamHeight > 0 ? streamHeight : 9;
    const update = () => {
      const rect = el.getBoundingClientRect();
      setGeo(computeGeometry(rect.width, rect.height, vw, vh));
    };
    update();
    const ro = new ResizeObserver(update);
    ro.observe(el);
    return () => ro.disconnect();
  }, [streamWidth, streamHeight]);

  // Esc cancels drawing (only when not mid-drag).
  useEffect(() => {
    if (!drawingRegion) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && dragRef.current === null) cancelDrawing();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [drawingRegion, cancelDrawing]);

  // Convert a mouse position to normalized frame coords (0..1 over the video).
  const eventToNorm = (e: { clientX: number; clientY: number }) => {
    const el = rootRef.current;
    const g = geoRef.current;
    if (!el || g.scale === 0) return null;
    const rect = el.getBoundingClientRect();
    const mx = e.clientX - rect.left;
    const my = e.clientY - rect.top;
    return {
      nx: clamp01((mx - g.offX) / g.renderedW),
      ny: clamp01((my - g.offY) / g.renderedH),
    };
  };

  // Vertex drag: imperative window listeners so movement outside the shape
  // still tracks. moveVertex only updates local UI (no PUT); the drag is
  // committed once on pointer up via commitVertexMove(). moveVertex is
  // identity-stable (functional setConfig, no `config` dep), so the closure
  // captured here never goes stale during the drag.
  const onVertexMouseDown = (
    e: ReactMouseEvent,
    regionId: string,
    vertexIdx: number
  ) => {
    e.stopPropagation();
    e.preventDefault();
    setActiveRegion(regionId);
    dragRef.current = { regionId, vertexIdx };
    const onMove = (ev: MouseEvent) => {
      const n = eventToNorm(ev);
      if (n) moveVertex(regionId, vertexIdx, n.nx, n.ny);
    };
    const onUp = () => {
      dragRef.current = null;
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
      // Commit the drag once, instead of PUT-ing on every mousemove.
      commitVertexMove();
    };
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
  };

  const px = (nx: number) => geo.offX + nx * geo.renderedW;
  const py = (ny: number) => geo.offY + ny * geo.renderedH;

  // Master switch gates rendering: when the privacy-mask master is off, draw no
  // polygons over the video (the device isn't baking, so the edit overlay should
  // not show stale masks). config.regions is still kept verbatim in the hook
  // state — only the rendered set is empty here, so re-enabling restores them.
  const regions = config?.enabled ? (config?.regions ?? []) : [];

  return (
    <div
      ref={rootRef}
      className="pointer-events-none absolute inset-0 z-20 select-none"
    >
      {geo.scale > 0 && (
        <svg className="absolute inset-0 h-full w-full">
          {/* Existing regions. Memoized per-region so a vertex drag only
              re-renders the dragged polygon (its region object is rebuilt
              with a new reference on each move); unaffected regions keep
              referential identity and skip re-render. */}
          {regions.map(r => (
            <RegionPolygon
              key={r.id}
              region={r}
              geo={geo}
              selected={activeRegion === r.id}
              onSelect={setActiveRegion}
              onVertexMouseDown={onVertexMouseDown}
            />
          ))}

          {/* Pending polygon while drawing */}
          {drawingRegion && pendingPoints.length > 0 && (
            <g>
              {pendingPoints.length >= 2 && (
                <polyline
                  points={pendingPoints.map(p => `${px(p.x)},${py(p.y)}`).join(' ')}
                  fill="none"
                  stroke="#22c55e"
                  strokeWidth={2}
                  strokeDasharray="5 5"
                />
              )}
              {pendingPoints.map((p, i) => (
                <circle key={i} cx={px(p.x)} cy={py(p.y)} r={5} fill="#22c55e" />
              ))}
            </g>
          )}

          {/* Drawing capture surface — on top so clicks place vertices */}
          {drawingRegion && (
            <rect
              x={geo.offX}
              y={geo.offY}
              width={geo.renderedW}
              height={geo.renderedH}
              fill="transparent"
              style={{ pointerEvents: 'all', cursor: 'crosshair' }}
              onPointerDown={e => e.stopPropagation()}
              onClick={e => {
                const n = eventToNorm(e);
                if (n) addPoint(n.nx, n.ny);
              }}
              onDoubleClick={e => {
                e.preventDefault();
                closePolygon();
              }}
            />
          )}
        </svg>
      )}
    </div>
  );
}

// Memoized per-region polygon + vertex handles. `geo` is stable across a
// drag (it only changes on resize), `region` keeps its reference unless it
// was the one dragged, and `selected` flips at most two regions. So during
// a vertex drag only this region's instance re-renders; the others bail
// out on the memo comparison and avoid recomputing their point strings.
type RegionPolygonProps = {
  region: PrivacyMaskRegion;
  geo: Geometry;
  selected: boolean;
  onSelect: (id: string) => void;
  onVertexMouseDown: (
    e: ReactMouseEvent,
    regionId: string,
    vertexIdx: number
  ) => void;
};

const RegionPolygon = memo(RegionPolygonImpl);

function RegionPolygonImpl({
  region,
  geo,
  selected,
  onSelect,
  onVertexMouseDown,
}: RegionPolygonProps) {
  if (!region.enabled || region.points_x.length < 3) return null;
  const stroke = selected ? '#3b82f6' : '#ef4444';
  const px = (nx: number) => geo.offX + nx * geo.renderedW;
  const py = (ny: number) => geo.offY + ny * geo.renderedH;
  const pts = region.points_x
    .map((x, i) => `${px(x)},${py(region.points_y[i])}`)
    .join(' ');
  // The active (editing) region is outline + handles only — no large
  // semi-transparent fill covering the WebCodecs canvas during the drag.
  // Inactive regions keep a light fill so the masked area stays readable.
  const fill = selected ? 'none' : 'rgba(0,0,0,0.22)';
  return (
    <g>
      <polygon
        points={pts}
        fill={fill}
        stroke={stroke}
        strokeWidth={2}
        style={{ pointerEvents: 'auto', cursor: 'pointer' }}
        onPointerDown={e => e.stopPropagation()}
        onClick={() => onSelect(region.id)}
      />
      {selected
        && region.points_x.map((x, i) => (
          <circle
            key={i}
            cx={px(x)}
            cy={py(region.points_y[i])}
            r={6}
            fill={stroke}
            stroke="#fff"
            strokeWidth={1}
            style={{ pointerEvents: 'auto', cursor: 'grab' }}
            onPointerDown={e => e.stopPropagation()}
            onMouseDown={e => onVertexMouseDown(e, region.id, i)}
          />
        ))}
    </g>
  );
}
