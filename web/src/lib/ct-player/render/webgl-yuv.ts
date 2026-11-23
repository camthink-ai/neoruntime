/**
 * WebGL YUV420P 渲染器（WASM 管道用；禁止 Canvas2D 逐像素转换）。
 * 输入：连续内存的 Y/U/V 平面（stride = width），BT.601 limited range。
 */

const VERTEX_SRC = `
attribute vec2 aPos;
varying vec2 vUV;
void main() {
  vUV = vec2(aPos.x * 0.5 + 0.5, 0.5 - aPos.y * 0.5);
  gl_Position = vec4(aPos, 0.0, 1.0);
}`;

const FRAGMENT_SRC = `
precision mediump float;
varying vec2 vUV;
uniform sampler2D uY;
uniform sampler2D uU;
uniform sampler2D uV;
void main() {
  float y = texture2D(uY, vUV).r;
  float u = texture2D(uU, vUV).r - 0.5;
  float v = texture2D(uV, vUV).r - 0.5;
  // BT.601 limited range
  float c = y - 16.0 / 255.0;
  vec3 rgb;
  rgb.r = clamp(1.164383 * c + 1.596027 * v, 0.0, 1.0);
  rgb.g = clamp(1.164383 * c - 0.391762 * u - 0.812968 * v, 0.0, 1.0);
  rgb.b = clamp(1.164383 * c + 2.017232 * u, 0.0, 1.0);
  gl_FragColor = vec4(rgb, 1.0);
}`;

export class WebGLYuvRenderer {
  private gl: WebGLRenderingContext | null = null;
  private program: WebGLProgram | null = null;
  private textures: WebGLTexture[] = [];
  private frameWidth = 0;
  private frameHeight = 0;
  private readonly objectFit: 'contain' | 'cover';

  constructor(canvas: HTMLCanvasElement, objectFit: 'contain' | 'cover' = 'contain') {
    this.objectFit = objectFit;
    // preserveDrawingBuffer：供截图/像素校验外部读取（性能代价可忽略）
    const gl = canvas.getContext('webgl', {
      alpha: false,
      antialias: false,
      preserveDrawingBuffer: true,
    }) as WebGLRenderingContext | null;
    if (!gl) return;
    this.gl = gl;

    const compile = (type: number, src: string): WebGLShader | null => {
      const sh = gl.createShader(type);
      if (!sh) return null;
      gl.shaderSource(sh, src);
      gl.compileShader(sh);
      if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
        console.error('[player:webgl]', gl.getShaderInfoLog(sh));
        gl.deleteShader(sh);
        return null;
      }
      return sh;
    };

    const vs = compile(gl.VERTEX_SHADER, VERTEX_SRC);
    const fs = compile(gl.FRAGMENT_SHADER, FRAGMENT_SRC);
    if (!vs || !fs) {
      this.gl = null;
      return;
    }
    const program = gl.createProgram();
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      console.error('[player:webgl]', gl.getProgramInfoLog(program));
      this.gl = null;
      return;
    }
    this.program = program;
    gl.useProgram(program);

    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]),
      gl.STATIC_DRAW
    );
    const loc = gl.getAttribLocation(program, 'aPos');
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);

    for (const unit of [gl.TEXTURE0, gl.TEXTURE1, gl.TEXTURE2]) {
      const tex = gl.createTexture();
      gl.activeTexture(unit);
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      this.textures.push(tex);
    }
    gl.uniform1i(gl.getUniformLocation(program, 'uY'), 0);
    gl.uniform1i(gl.getUniformLocation(program, 'uU'), 1);
    gl.uniform1i(gl.getUniformLocation(program, 'uV'), 2);
  }

  get available(): boolean {
    return this.gl !== null;
  }

  /** yuv 为连续 [Y | U | V] 缓冲（stride = width）。 */
  draw(yuv: Uint8Array, width: number, height: number): void {
    const gl = this.gl;
    if (!gl) return;
    if (width !== this.frameWidth || height !== this.frameHeight) {
      this.frameWidth = width;
      this.frameHeight = height;
    }
    const yLen = width * height;
    const uvLen = (width >> 1) * (height >> 1);
    if (yuv.length < yLen + uvLen * 2) return;

    const upload = (index: number, offset: number, w: number, h: number) => {
      gl.activeTexture(gl.TEXTURE0 + index);
      gl.texImage2D(
        gl.TEXTURE_2D,
        0,
        gl.LUMINANCE,
        w,
        h,
        0,
        gl.LUMINANCE,
        gl.UNSIGNED_BYTE,
        yuv.subarray(offset, offset + w * h)
      );
    };
    upload(0, 0, width, height);
    upload(1, yLen, width >> 1, height >> 1);
    upload(2, yLen + uvLen, width >> 1, height >> 1);

    // objectFit：cover 铺满裁切 / contain 完整显示
    const canvas = gl.canvas as HTMLCanvasElement;
    const cssW = canvas.clientWidth || width;
    const cssH = canvas.clientHeight || height;
    const dpr = window.devicePixelRatio || 1;
    const targetW = Math.max(1, Math.round(cssW * dpr));
    const targetH = Math.max(1, Math.round(cssH * dpr));
    if (canvas.width !== targetW || canvas.height !== targetH) {
      canvas.width = targetW;
      canvas.height = targetH;
    }
    const scale =
      this.objectFit === 'cover'
        ? Math.max(canvas.width / width, canvas.height / height)
        : Math.min(canvas.width / width, canvas.height / height);
    const drawW = width * scale;
    const drawH = height * scale;
    gl.viewport(
      Math.round((canvas.width - drawW) / 2),
      Math.round((canvas.height - drawH) / 2),
      Math.round(drawW),
      Math.round(drawH)
    );
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  }

  destroy(): void {
    const gl = this.gl;
    if (!gl) return;
    for (const tex of this.textures) gl.deleteTexture(tex);
    if (this.program) gl.deleteProgram(this.program);
    this.textures = [];
    this.program = null;
    this.gl = null;
  }
}
