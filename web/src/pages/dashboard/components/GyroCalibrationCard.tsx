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
  MeshStandardMaterial,
  Mesh,
  Group,
  CanvasTexture,
  AmbientLight,
  DirectionalLight,
  GridHelper,
  Sprite,
  SpriteMaterial,
  MeshBasicMaterial,
  Quaternion,
  Vector3,
  MathUtils,
  SRGBColorSpace,
} from 'three';

/**
 * 陀螺仪校准可视化（精简 three.js）
 *
 * 姿态只表达为两轴倾角，不含偏航（Z 轴）：
 *   - pitch  前后倾角（°），正值 = 前倾（+X 侧下沉）
 *   - roll   左右倾角（°），正值 = 右倾（+Y 侧下沉）
 * 薄板只在前后/左右两轴上倾斜，永不绕 Z 轴旋转。
 *
 * 坐标约定（Z-up 世界系，与后端一致）：
 *   +X = 前 / -X = 后 / +Y = 右 / -Y = 左 / +Z = 上
 *
 * 数据来源：GyroSSE（GET /api/v1/monitor/gyro/attitude）。
 * 端点不可用或首帧超时时回退 MockGyro，保证可视化始终有姿态数据。
 */

// 设备水平容差：综合倾角 <= 该值视为水平
const LEVEL_TOLERANCE_DEG = 2;

// 倾角有效范围（后端同样钳制到此区间）
const MAX_TILT_DEG = 90;

// ---- 平面纹理（单面文字）----
// PlaneGeometry 默认在 XY 平面、法线 +Z；纹理 +X(右)=+X、+Y(上)=+Y
function makeFaceTexture(label: string, bg: string) {
  const size = 512;
  const cvs = document.createElement('canvas');
  cvs.width = size;
  cvs.height = size;
  const ctx = cvs.getContext('2d')!;
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, size, size);
  ctx.strokeStyle = 'rgba(255,255,255,0.35)';
  ctx.lineWidth = 10;
  ctx.strokeRect(6, 6, size - 12, size - 12);
  ctx.fillStyle = 'rgba(255,255,255,0.92)';
  ctx.font = 'bold 110px sans-serif';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(label, size / 2, size / 2);
  const tex = new CanvasTexture(cvs);
  tex.anisotropy = 4;
  tex.colorSpace = SRGBColorSpace;
  return tex;
}

// ---- 四向倾角标（始终面向相机的文字精灵）----
function makeDirLabel(text: string, color: string) {
  const size = 256;
  const cvs = document.createElement('canvas');
  cvs.width = size;
  cvs.height = size;
  const ctx = cvs.getContext('2d')!;
  ctx.fillStyle = color;
  ctx.font = 'bold 76px sans-serif';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(text, size / 2, size / 2);
  const tex = new CanvasTexture(cvs);
  tex.colorSpace = SRGBColorSpace;
  const sp = new Sprite(new SpriteMaterial({ map: tex, transparent: true }));
  sp.scale.set(1.0, 0.5, 1);
  return sp;
}

/**
 * 由两轴倾角构建 yaw 锁死的姿态四元数（世界轴旋转）。
 * pitch>0 前倾（+X 下沉），roll>0 右倾（+Y 下沉）。
 */
const AXIS_Y = new Vector3(0, 1, 0);
const AXIS_X = new Vector3(1, 0, 0);
function tiltToQuaternion(pitchDeg: number, rollDeg: number, out: Quaternion) {
  const qPitch = new Quaternion().setFromAxisAngle(AXIS_Y, MathUtils.degToRad(pitchDeg));
  const qRoll = new Quaternion().setFromAxisAngle(AXIS_X, -MathUtils.degToRad(rollDeg));
  // 先 pitch 后 roll（均绕世界轴），等价于 yaw 恒为 0
  out.multiplyQuaternions(qRoll, qPitch);
  return out;
}

/**
 * 模拟陀螺仪数据（仅前后/左右两轴，无偏航）。
 * 后端 SSE 接入后由真实数据驱动 setOrientation。
 */
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
    const pitch = Math.sin(this.t * 0.7) * 15;
    const roll = Math.cos(this.t * 0.5) * 12;
    this.onTilt(pitch, roll);
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

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const container = canvas.parentElement!;

    // ---- 渲染器 ----
    const renderer = new WebGLRenderer({ canvas, antialias: true, alpha: true });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    const w = container.clientWidth || 1;
    const h = container.clientHeight || 1;
    renderer.setSize(w, h, false);
    renderer.outputColorSpace = SRGBColorSpace;

    // ---- 场景 / 相机（固定俯视视角）----
    const scene = new Scene();
    scene.background = null;

    const camera = new PerspectiveCamera(40, w / h, 0.1, 100);
    // Z-up world：+Z 向上，相机在 +X 侧 20° 俯视（高度 = 水平距离 × tan20°）。
    camera.up.set(0, 0, 1);
    camera.position.set(8, 0, 8 * Math.tan(MathUtils.degToRad(20)));
    camera.lookAt(0, 0, 0);

    // ---- 拖拽轨道（Z-up 球坐标）----
    // 极角 theta 自 +Z 起算；方位角 phi 绕 +Z（+X 侧 phi=0）。初始 20° 俯视 → theta=70°。
    const orbit = {
      radius: Math.hypot(8, 8 * Math.tan(MathUtils.degToRad(20))),
      theta: MathUtils.degToRad(70),
      phi: 0,
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
      const sens = 0.006;
      orbit.phi -= dx * sens;
      // 极角限制在 5°..89°，避免越过天顶或贴地翻转
      orbit.theta = MathUtils.clamp(
        orbit.theta - dy * sens,
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

    // ---- 灯光 ----
    scene.add(new AmbientLight(0xffffff, 0.7));
    const key = new DirectionalLight(0xffffff, 1.0);
    key.position.set(6, 2, 4);
    scene.add(key);
    const fill = new DirectionalLight(0x88aaff, 0.3);
    fill.position.set(-5, -4, -3);
    scene.add(fill);

    // ---- 平面薄板（上面 TOP / 下面 BOTTOM，法线 +Z = 设备顶面）----
    const plate = new Group();
    const plateGeo = new PlaneGeometry(3, 3);
    // 上面：法线 +Z，FrontSide 仅从上方可见
    const topMesh = new Mesh(
      plateGeo,
      new MeshStandardMaterial({
        map: makeFaceTexture('TOP', '#d29922'),
        roughness: 0.5,
        metalness: 0.1,
      }),
    );
    topMesh.position.z = 0.01;
    plate.add(topMesh);
    // 下面：绕 Y 旋转 180° 使法线朝 -Z，纹理 +Y 仍对齐世界 +Y，从下方可正常阅读
    const bottomMesh = new Mesh(
      plateGeo,
      new MeshStandardMaterial({
        map: makeFaceTexture('BOTTOM', '#6e7681'),
        roughness: 0.5,
        metalness: 0.1,
      }),
    );
    bottomMesh.rotation.y = Math.PI;
    bottomMesh.position.z = -0.01;
    plate.add(bottomMesh);
    scene.add(plate);

    // ---- 地面网格（XY 平面，水平基准面）----
    const grid = new GridHelper(20, 20, 0x30363d, 0x21262d);
    grid.rotation.x = Math.PI / 2;
    grid.position.z = -2;
    scene.add(grid);

    // ---- 水平参考系（始终水平的参考网格 + 四向倾角标）----
    // +X=前 / -X=后 / +Y=右 / -Y=左；薄板某侧下沉即对应方向倾角
    const refGrid = new Mesh(
      new PlaneGeometry(3.4, 3.4, 8, 8),
      new MeshBasicMaterial({
        color: 0x00d9ff,
        wireframe: true,
        transparent: true,
        opacity: 0.9,
      }),
    );
    scene.add(refGrid);
    // 前后/左右四向倾角标：统一使用 UI 基础前景色（跟随 --foreground 主题变量）
    const cssFg = getComputedStyle(document.documentElement)
      .getPropertyValue('--foreground')
      .trim();
    const baseColor = cssFg
      ? /^\d/.test(cssFg) && cssFg.includes('%')
        ? `hsl(${cssFg})`
        : cssFg
      : 'rgba(255,255,255,0.92)';
    const dirLabels = [
      { text: t('sys.gyro.front', '前倾'), pos: [2.3, 0, 0] as const },
      { text: t('sys.gyro.back', '后倾'), pos: [-2.3, 0, 0] as const },
      { text: t('sys.gyro.right', '右倾'), pos: [0, 2.3, 0] as const },
      { text: t('sys.gyro.left', '左倾'), pos: [0, -2.3, 0] as const },
    ].map((d) => {
      const sp = makeDirLabel(d.text, baseColor);
      sp.position.set(d.pos[0], d.pos[1], d.pos[2]);
      scene.add(sp);
      return sp;
    });

    // ---- 姿态接口（双角度，yaw 锁死）----
    // target 由数据源写入；current 每帧平滑插值，抑制抖动。
    let targetPitch = 0;
    let targetRoll = 0;
    let currentPitch = 0;
    let currentRoll = 0;
    const plateQuat = new Quaternion();
    const deviceUp = new Vector3();

    const setOrientation = (pitchDeg: number, rollDeg: number) => {
      targetPitch = MathUtils.clamp(pitchDeg, -MAX_TILT_DEG, MAX_TILT_DEG);
      targetRoll = MathUtils.clamp(rollDeg, -MAX_TILT_DEG, MAX_TILT_DEG);
    };

    // ---- HUD ----
    const updateHud = (pitchDeg: number, rollDeg: number) => {
      // 综合倾角：由当前姿态法线与世界 +Z 的夹角得到（精确）
      tiltToQuaternion(pitchDeg, rollDeg, plateQuat);
      deviceUp.set(0, 0, 1).applyQuaternion(plateQuat);
      const uz = MathUtils.clamp(deviceUp.z, -1, 1);
      const tiltDeg = MathUtils.radToDeg(Math.acos(uz));
      const level = tiltDeg <= LEVEL_TOLERANCE_DEG;
      const signed = (deg: number) => (deg >= 0 ? '+' : '') + deg.toFixed(2);
      if (pitchRef.current) pitchRef.current.textContent = signed(pitchDeg);
      if (rollRef.current) rollRef.current.textContent = signed(rollDeg);
      if (statusTextRef.current) {
        statusTextRef.current.textContent = level
          ? t('sys.gyro.level', '水平')
          : t('sys.gyro.tilted', '未水平');
      }
      if (statusDotRef.current) {
        statusDotRef.current.style.backgroundColor = level ? '#10b981' : '#f85149';
      }
    };

    // ---- 渲染循环 ----
    const clock = new Clock();
    let rafId = 0;
    const animate = () => {
      rafId = requestAnimationFrame(animate);
      const dt = clock.getDelta();
      // 角度线性插值（yaw 锁死，无需四元数 slerp）
      const k = 1 - 0.001 ** dt;
      currentPitch += (targetPitch - currentPitch) * k;
      currentRoll += (targetRoll - currentRoll) * k;
      tiltToQuaternion(currentPitch, currentRoll, plateQuat);
      plate.quaternion.copy(plateQuat);
      updateHud(currentPitch, currentRoll);
      renderer.render(scene, camera);
    };
    animate();

    // ---- 数据源：优先 SSE 实时姿态，不可用则回退 MockGyro ----
    // Data source: prefer real SSE attitude data.
    // Mock data is opt-in for local demos only; otherwise a static device would appear to tilt.
    const enableMock = import.meta.env.DEV && new URLSearchParams(window.location.search).get('gyroMock') === '1';
    const mock = new MockGyro(setOrientation);
    let sseDelivered = false;
    let usingMock = false;

    const useMock = () => {
      if (!enableMock || usingMock) return;
      usingMock = true;
      mock.start();
    };

    // Give the SSE stream a short grace period before optional demo fallback.
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
        // EventSource retries transient errors; use mock only after the stream closes.
        if (sse.closed) useMock();
      },
    });
    sse.start();

    // ---- 自适应（容器尺寸，而非 window）----
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
      plateGeo.dispose();
      for (const m of [topMesh.material, bottomMesh.material] as MeshStandardMaterial[]) {
        m.map?.dispose();
        m.dispose();
      }
      grid.dispose();
      refGrid.geometry.dispose();
      (refGrid.material as MeshBasicMaterial).dispose();
      for (const sp of dirLabels) {
        sp.material.map?.dispose();
        sp.material.dispose();
      }
      renderer.dispose();
    };
  }, []);

  return (
    <div className="bg-card rounded-2xl p-5 shadow-sm border border-border h-full max-lg:max-h-80 flex flex-col overflow-hidden">
      {/* Header */}
      <div className="flex items-center justify-between mb-4 shrink-0">
        <h3 className="text-base font-bold text-foreground flex items-center gap-2">
          <Compass className="w-4 h-4 text-primary" />
          {t('sys.gyro.title', '陀螺仪校准')}
        </h3>
      </div>

      {/* 3D 场景 + 姿态读数 */}
      <div className="relative flex-1 min-h-0 rounded-xl overflow-hidden bg-secondary/20">
        <canvas ref={canvasRef} className="block w-full h-full" style={{ touchAction: 'none' }} />

        {/* 左侧：前后 / 左右 倾角（自上而下纵向排列） */}
        <div className="absolute top-2 left-2 flex flex-col gap-1.5">
          {/* 前后（pitch，正值=前倾） */}
          <div className="flex items-center gap-1.5 px-2 py-1.5 rounded-lg border border-border bg-card/80 backdrop-blur-sm">
            <span className="text-muted-foreground text-[11px]">
              {t('sys.gyro.frontBack', '前后')}
            </span>
            <span ref={pitchRef} className="text-[11px] tabular-nums font-semibold">
              +0.00
            </span>
            <span className="text-muted-foreground text-[11px]">°</span>
          </div>
          {/* 左右（roll，正值=右倾） */}
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

        {/* 右侧：水平判定 */}
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
