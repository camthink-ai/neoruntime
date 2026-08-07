export type PlayerObjectFit = 'contain' | 'cover' | 'adaptive';

/**
 * Landscape streams fill the 16:9 preview, while portrait streams stay fully
 * visible. Unknown dimensions default to contain so startup never crops.
 */
export function resolvePlayerObjectFit(
  mode: PlayerObjectFit,
  width: number,
  height: number
): 'contain' | 'cover' {
  if (mode !== 'adaptive') return mode;
  if (width <= 0 || height <= 0) return 'contain';
  return width >= height ? 'cover' : 'contain';
}
