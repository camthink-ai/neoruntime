export type PlayerObjectFit = 'contain' | 'cover' | 'adaptive';

/**
 * Adaptive fit: only streams at least as wide as the 16:9 preview fill it
 * (cover is lossless there); narrower streams such as 4:3 640×480, square or
 * portrait stay fully visible instead of being cropped to fill. Unknown
 * dimensions default to contain so startup never crops.
 */
export function resolvePlayerObjectFit(
  mode: PlayerObjectFit,
  width: number,
  height: number
): 'contain' | 'cover' {
  if (mode !== 'adaptive') return mode;
  if (width <= 0 || height <= 0) return 'contain';
  return width * 9 >= height * 16 ? 'cover' : 'contain';
}
