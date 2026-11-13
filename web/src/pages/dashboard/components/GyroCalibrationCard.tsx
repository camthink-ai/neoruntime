import { useEffect, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import { Compass } from 'lucide-react';
import { GyroSSE } from '@/lib/gyroStream/gyroSSE';
import {
  Scene,
  PerspectiveCamera,
  WebGLRenderer,
  Clock,
  PlaneGeometry,
  BoxGeometry,
  CylinderGeometry,
  SphereGeometry,
  MeshStandardMaterial,
  Mesh,
  Group,
  CanvasTexture,
  AmbientLight,
  DirectionalLight,
  GridHelper,
  LineSegments,
  EdgesGeometry,
  LineBasicMaterial,
  Quaternion,
  Vector3,
  MathUtils,
  SRGBColorSpace,
  DoubleSide,
  type Material,
  type BufferGeometry,
} from 'three';

/**
 * 陀螺仪校准可视化：仅 pitch/roll（无 yaw）。
 * +X 前 / +Y 右 / +Z 上；数据来自 GyroSSE，不可用时可选 MockGyro。
 */

const LEVEL_TOLERANCE_DEG = 2;
const MAX_TILT_DEG = 90;

type Disposable = { dispose: () => void };

function makeLabelTexture(
  label: string,
  bg: string,
  opts?: { w?: number; h?: number; fontPx?: number; fg?: string }
) {
  const w = opts?.w ?? 512;
  const h = opts?.h ?? 512;
  const cvs = document.createElement('canvas');
  cvs.width = w;
  cvs.height = h;
  const ctx = cvs.getContext('2d')!;
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, w, h);
  ctx.fillStyle = opts?.fg ?? 'rgba(255,255,255,0.92)';
  ctx.font = `bold ${opts?.fontPx ?? 120}px sans-serif`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(label, w / 2, h / 2);
  const tex = new CanvasTexture(cvs);
  tex.anisotropy = 4;
  tex.colorSpace = SRGBColorSpace;
  return tex;
}

/** NE503 bullet IPC：机身 + 顶雨罩 + 水平颈/球头 + 壁挂底板（俯视 T）。 */
function buildIpcDevice(): { root: Group; disposables: Disposable[] } {
  const root = new Group();
  const disposables: Disposable[] = [];
  const track = (...items: Disposable[]) => {
    disposables.push(...items);
  };
  const mat = (
    color: number,
    roughness?: number,
    metalness?: number,
    side?: typeof DoubleSide
  ) => {
    const m = new MeshStandardMaterial({
      color,
      roughness: roughness ?? 0.45,
      metalness: metalness ?? 0.28,
      side,
    });
    track(m);
    return m;
  };
  const add = (
    parent: Group,
    geo: BufferGeometry,
    material: Material | Material[],
    opts?: { x?: number; y?: number; z?: number; rx?: number; ry?: number; rz?: number }
  ) => {
    const mesh = new Mesh(geo, material);
    if (opts?.x != null) mesh.position.x = opts.x;
    if (opts?.y != null) mesh.position.y = opts.y;
    if (opts?.z != null) mesh.position.z = opts.z;
    if (opts?.rx != null) mesh.rotation.x = opts.rx;
    if (opts?.ry != null) mesh.rotation.y = opts.ry;
    if (opts?.rz != null) mesh.rotation.z = opts.rz;
    parent.add(mesh);
    track(geo);
    return mesh;
  };

  const shell = mat(0x4a535e, 0.5, 0.22);
  const shellDark = mat(0x3a424c, 0.52, 0.18);
  const hoodMat = mat(0x3a424c, 0.52, 0.18, DoubleSide);
  const metal = mat(0x6a727c, 0.32, 0.55);
  const screwSilver = mat(0xd0d5dc, 0.22, 0.8);
  const black = mat(0x14181e, 0.4, 0.25);

  const barrelR = 0.56;
  const barrelLen = 2.15;
  const baseFace = 0.98;
  const baseDepth = 0.42;
  const ballR = 0.14;

  const baseX = -1.95;
  const baseFrontX = baseX + baseDepth / 2;
  const stubLen = 0.14;
  const ballX = baseFrontX + stubLen + ballR * 0.35;
  const armLen = 0.28;
  const armMidX = ballX + ballR * 0.55 + armLen / 2;
  const barrelRearX = ballX + ballR * 0.55 + armLen;
  const barrelX = barrelRearX + barrelLen / 2;

  const base = new Group();
  base.position.set(baseX, 0, 0);
  root.add(base);
  const baseBox = add(base, new BoxGeometry(baseDepth, baseFace, baseFace), shell);
  {
    const eg = new EdgesGeometry(baseBox.geometry);
    const lines = new LineSegments(
      eg,
      new LineBasicMaterial({ color: 0x6a727c, transparent: true, opacity: 0.28 })
    );
    base.add(lines);
    track(eg, lines.material);
  }
  {
    const tex = makeLabelTexture('TOP', '#3a424c', {
      w: 256,
      h: 128,
      fontPx: 72,
      fg: 'rgba(255,255,255,0.9)',
    });
    track(tex);
    const m = new MeshStandardMaterial({ map: tex, roughness: 0.6, metalness: 0.1 });
    track(m);
    add(base, new PlaneGeometry(0.22, 0.12), m, { z: baseFace / 2 + 0.002 });
  }
  add(base, new BoxGeometry(0.06, 0.04, 0.1), metal, {
    y: -baseFace / 2 - 0.015,
    z: 0.1,
  });
  add(base, new BoxGeometry(0.06, 0.04, 0.1), metal, {
    y: -baseFace / 2 - 0.015,
    z: -0.1,
  });
  add(base, new BoxGeometry(0.06, baseFace * 0.92, baseFace * 0.92), shellDark, {
    x: baseDepth / 2 - 0.02,
  });

  add(root, new CylinderGeometry(0.13, 0.15, stubLen, 20), shellDark, {
    x: baseFrontX + stubLen / 2,
    rz: Math.PI / 2,
  });
  add(root, new SphereGeometry(ballR, 22, 16), metal, { x: ballX });
  add(root, new CylinderGeometry(0.12, 0.14, armLen, 20), shell, {
    x: armMidX,
    rz: Math.PI / 2,
  });
  add(root, new CylinderGeometry(0.2, 0.22, 0.1, 20), shellDark, {
    x: barrelRearX,
    rz: Math.PI / 2,
  });

  add(root, new CylinderGeometry(barrelR, barrelR, barrelLen, 48), shell, {
    x: barrelX,
    rz: Math.PI / 2,
  });
  add(root, new CylinderGeometry(barrelR * 0.98, barrelR * 0.98, 0.05, 36), shellDark, {
    x: barrelRearX + 0.03,
    rz: Math.PI / 2,
  });

  // Thin top hood; front rim arched (crown forward, sides pull back)
  const hoodR = barrelR * 1.08;
  const hoodSpan = Math.PI * 1.02;
  const hoodStart = -hoodSpan / 2;
  const hoodLen = barrelLen + 0.14;
  const hoodX = barrelRearX + hoodLen / 2 - 0.02;
  {
    const geo = new CylinderGeometry(hoodR, hoodR, hoodLen, 56, 14, true, hoodStart, hoodSpan);
    geo.rotateZ(Math.PI / 2);
    const pos = geo.attributes.position;
    const half = hoodLen / 2;
    const zone = 0.4;
    for (let i = 0; i < pos.count; i++) {
      const x = pos.getX(i);
      const z = pos.getZ(i);
      if (x < half - zone) continue;
      const t = (x - (half - zone)) / zone;
      const elev = Math.max(0, Math.min(1, z / hoodR));
      pos.setX(i, x - t * t * (1 - elev * elev) * 0.22);
    }
    pos.needsUpdate = true;
    geo.computeVertexNormals();
    const mesh = new Mesh(geo, hoodMat);
    mesh.position.set(hoodX, 0, 0);
    root.add(mesh);
    track(geo);
  }

  const screwX = barrelX - 0.05;
  add(root, new BoxGeometry(0.16, 0.13, 0.022), shellDark, {
    x: screwX,
    z: hoodR,
  });
  add(root, new CylinderGeometry(0.048, 0.048, 0.012, 18), screwSilver, {
    x: screwX,
    z: hoodR + 0.008,
    rx: Math.PI / 2,
  });
  add(root, new BoxGeometry(0.052, 0.006, 0.003), black, {
    x: screwX,
    z: hoodR + 0.015,
  });

  const frontX = barrelRearX + barrelLen - 0.02;
  add(root, new CylinderGeometry(barrelR * 0.96, barrelR * 0.96, 0.05, 40), black, {
    x: frontX,
    rz: Math.PI / 2,
  });
  {
    const glassMat = new MeshStandardMaterial({
      color: 0x151c28,
      roughness: 0.15,
      metalness: 0.8,
      emissive: 0x081018,
      emissiveIntensity: 0.3,
    });
    track(glassMat);
    add(root, new BoxGeometry(0.04, 0.44, 0.3), black, { x: frontX + 0.02 });
    add(root, new BoxGeometry(0.03, 0.36, 0.22), glassMat, { x: frontX + 0.035 });
  }

  root.position.set(0.35, 0, 0.08);
  return { root, disposables };
}

/** pitch>0 前倾，roll>0 右倾；yaw 锁死。 */
const AXIS_Y = new Vector3(0, 1, 0);
const AXIS_X = new Vector3(1, 0, 0);
function tiltToQuaternion(pitchDeg: number, rollDeg: number, out: Quaternion) {
  const qPitch = new Quaternion().setFromAxisAngle(AXIS_Y, MathUtils.degToRad(pitchDeg));
  const qRoll = new Quaternion().setFromAxisAngle(AXIS_X, -MathUtils.degToRad(rollDeg));
  out.multiplyQuaternions(qRoll, qPitch);
  return out;
}

class MockGyro {
  onTilt: (pitch: number, roll: number) => void;

  running = false;

  private t = 0;

  private raf: number | null = null;

  constructor(onTilt: (pitch: number, roll: number) => void) {
    this.onTilt = onTilt;
  }

  start() {
    if (this.running) return;
    this.running = true;
    this.t = 0;
    this.loop();
  }

  stop() {
    this.running = false;
    if (this.raf != null) cancelAnimationFrame(this.raf);
    this.raf = null;
  }

  private loop = () => {
    if (!this.running) return;
    this.t += 0.01;
    this.onTilt(Math.sin(this.t * 0.7) * 15, Math.cos(this.t * 0.5) * 12);
    this.raf = requestAnimationFrame(this.loop);
  };
}

export default function GyroCalibrationCard() {
  const { t } = useTranslation();
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const pitchRef = useRef<HTMLSpanElement>(null);
  const rollRef = useRef<HTMLSpanElement>(null);
  const statusTextRef = useRef<HTMLSpanElement>(null);
  const statusDotRef = useRef<HTMLSpanElement>(null);
  const tRef = useRef(t);

  useEffect(() => {
    tRef.current = t;
  });

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const container = canvas.parentElement!;

    const renderer = new WebGLRenderer({ canvas, antialias: true, alpha: true });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    const w = container.clientWidth || 1;
    const h = container.clientHeight || 1;
    renderer.setSize(w, h, false);
    renderer.outputColorSpace = SRGBColorSpace;

    const scene = new Scene();
    scene.background = null;

    const camera = new PerspectiveCamera(40, w / h, 0.1, 100);
    camera.up.set(0, 0, 1);
    const orbit = {
      radius: 7.2,
      theta: MathUtils.degToRad(68),
      phi: MathUtils.degToRad(48),
    };
    const applyOrbit = () => {
      const { radius, theta, phi } = orbit;
      camera.position.set(
        radius * Math.sin(theta) * Math.cos(phi),
        radius * Math.sin(theta) * Math.sin(phi),
        radius * Math.cos(theta),
      );
      camera.lookAt(0, 0, 0);
    };
    applyOrbit();

    let dragging = false;
    let lastX = 0;
    let lastY = 0;
    const onPointerDown = (e: PointerEvent) => {
      dragging = true;
      lastX = e.clientX;
      lastY = e.clientY;
      canvas.style.cursor = 'grabbing';
      try {
        canvas.setPointerCapture(e.pointerId);
      } catch {
        /* noop */
      }
    };
    const onPointerMove = (e: PointerEvent) => {
      if (!dragging) return;
      const dx = e.clientX - lastX;
      const dy = e.clientY - lastY;
      lastX = e.clientX;
      lastY = e.clientY;
      orbit.phi -= dx * 0.006;
      orbit.theta = MathUtils.clamp(
        orbit.theta - dy * 0.006,
        MathUtils.degToRad(5),
        MathUtils.degToRad(89),
      );
      applyOrbit();
    };
    const onPointerUp = (e: PointerEvent) => {
      dragging = false;
      canvas.style.cursor = 'grab';
      try {
        canvas.releasePointerCapture(e.pointerId);
      } catch {
        /* noop */
      }
    };
    canvas.style.cursor = 'grab';
    canvas.addEventListener('pointerdown', onPointerDown);
    canvas.addEventListener('pointermove', onPointerMove);
    canvas.addEventListener('pointerup', onPointerUp);
    canvas.addEventListener('pointercancel', onPointerUp);

    scene.add(new AmbientLight(0xffffff, 1.05));
    const key = new DirectionalLight(0xffffff, 1.35);
    key.position.set(6, 2, 4);
    scene.add(key);
    const fill = new DirectionalLight(0xc8d0dc, 0.65);
    fill.position.set(-5, -4, -3);
    scene.add(fill);
    const rim = new DirectionalLight(0xffffff, 0.55);
    rim.position.set(-2, 8, 4);
    scene.add(rim);

    const { root: plate, disposables: ipcDisposables } = buildIpcDevice();
    plate.scale.setScalar(1.35);
    scene.add(plate);

    const grid = new GridHelper(20, 20, 0x30363d, 0x21262d);
    grid.rotation.x = Math.PI / 2;
    grid.position.z = -2.4;
    scene.add(grid);

    let targetPitch = 0;
    let targetRoll = 0;
    let currentPitch = 0;
    let currentRoll = 0;
    const deviceQuat = new Quaternion();
    const deviceUp = new Vector3();

    const setOrientation = (pitchDeg: number, rollDeg: number) => {
      targetPitch = MathUtils.clamp(pitchDeg, -MAX_TILT_DEG, MAX_TILT_DEG);
      targetRoll = MathUtils.clamp(rollDeg, -MAX_TILT_DEG, MAX_TILT_DEG);
    };

    const updateHud = (pitchDeg: number, rollDeg: number) => {
      tiltToQuaternion(pitchDeg, rollDeg, deviceQuat);
      deviceUp.set(0, 0, 1).applyQuaternion(deviceQuat);
      const tiltDeg = MathUtils.radToDeg(Math.acos(MathUtils.clamp(deviceUp.z, -1, 1)));
      const level = tiltDeg <= LEVEL_TOLERANCE_DEG;
      const signed = (deg: number) => (deg >= 0 ? '+' : '') + deg.toFixed(2);
      if (pitchRef.current) pitchRef.current.textContent = signed(pitchDeg);
      if (rollRef.current) rollRef.current.textContent = signed(rollDeg);
      if (statusTextRef.current) {
        statusTextRef.current.textContent = level
          ? tRef.current('sys.gyro.level', '水平')
          : tRef.current('sys.gyro.tilted', '未水平');
      }
      if (statusDotRef.current) {
        statusDotRef.current.style.backgroundColor = level ? '#10b981' : '#f85149';
      }
    };

    const clock = new Clock();
    let rafId = 0;
    const animate = () => {
      rafId = requestAnimationFrame(animate);
      const dt = clock.getDelta();
      const k = 1 - 0.001 ** dt;
      currentPitch += (targetPitch - currentPitch) * k;
      currentRoll += (targetRoll - currentRoll) * k;
      tiltToQuaternion(currentPitch, currentRoll, deviceQuat);
      plate.quaternion.copy(deviceQuat);
      updateHud(currentPitch, currentRoll);
      renderer.render(scene, camera);
    };
    animate();

    // SSE first; MockGyro only in DEV with ?gyroMock=1 after grace period
    const enableMock = (
      import.meta.env.DEV
      && new URLSearchParams(window.location.search).get('gyroMock') === '1'
    );
    const mock = new MockGyro(setOrientation);
    let sseDelivered = false;
    let usingMock = false;

    const useMock = () => {
      if (!enableMock || usingMock) return;
      usingMock = true;
      mock.start();
    };

    const fallbackTimer = window.setTimeout(() => {
      if (!sseDelivered) useMock();
    }, 3000);

    const sse = new GyroSSE({
      onAttitude: a => {
        if (!sseDelivered) {
          sseDelivered = true;
          window.clearTimeout(fallbackTimer);
        }
        if (usingMock) {
          usingMock = false;
          mock.stop();
        }
        setOrientation(a.pitch, a.roll);
      },
      onError: () => {
        if (sse.closed) useMock();
      },
    });
    sse.start();

    const ro = new ResizeObserver(() => {
      const cw = container.clientWidth || 1;
      const ch = container.clientHeight || 1;
      camera.aspect = cw / ch;
      camera.updateProjectionMatrix();
      renderer.setSize(cw, ch, false);
    });
    ro.observe(container);

    return () => {
      cancelAnimationFrame(rafId);
      window.clearTimeout(fallbackTimer);
      sse.stop();
      mock.stop();
      ro.disconnect();
      canvas.removeEventListener('pointerdown', onPointerDown);
      canvas.removeEventListener('pointermove', onPointerMove);
      canvas.removeEventListener('pointerup', onPointerUp);
      canvas.removeEventListener('pointercancel', onPointerUp);
      for (const d of ipcDisposables) d.dispose();
      grid.dispose();
      renderer.dispose();
    };
  }, []);

  return (
    <div className="bg-card rounded-2xl p-5 shadow-sm border border-border h-full max-lg:max-h-80 flex flex-col overflow-hidden">
      <div className="flex items-center justify-between mb-4 shrink-0">
        <h3 className="text-base font-bold text-foreground flex items-center gap-2">
          <Compass className="w-4 h-4 text-primary" />
          {t('sys.gyro.title', '陀螺仪校准')}
        </h3>
      </div>

      <div className="relative flex-1 min-h-0 rounded-xl overflow-hidden bg-secondary/20">
        <canvas ref={canvasRef} className="block w-full h-full" style={{ touchAction: 'none' }} />

        <div className="absolute top-2 left-2 flex flex-col gap-1.5">
          <div className="flex items-center gap-1.5 px-2 py-1.5 rounded-lg border border-border bg-card/80 backdrop-blur-sm">
            <span className="text-muted-foreground text-[11px]">
              {t('sys.gyro.frontBack', '前后')}
            </span>
            <span ref={pitchRef} className="text-[11px] tabular-nums font-semibold">
              +0.00
            </span>
            <span className="text-muted-foreground text-[11px]">°</span>
          </div>
          <div className="flex items-center gap-1.5 px-2 py-1.5 rounded-lg border border-border bg-card/80 backdrop-blur-sm">
            <span className="text-muted-foreground text-[11px]">
              {t('sys.gyro.leftRight', '左右')}
            </span>
            <span ref={rollRef} className="text-[11px] tabular-nums font-semibold">
              +0.00
            </span>
            <span className="text-muted-foreground text-[11px]">°</span>
          </div>
        </div>

        <div className="absolute top-2 right-2 flex items-center gap-1.5 px-2 py-1.5 rounded-lg border border-border bg-card/80 backdrop-blur-sm">
          <span
            ref={statusDotRef}
            className="w-2 h-2 rounded-full"
            style={{ backgroundColor: '#10b981' }}
          />
          <span ref={statusTextRef} className="text-[11px] font-semibold">
            {t('sys.gyro.level', '水平')}
          </span>
        </div>
      </div>
    </div>
  );
}
