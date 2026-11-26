/**
 * H.264 fMP4 封装（avcC/avc1）。移植自底座项目 h264ToMp4.ts。
 */

import {
  box,
  concat,
  createInitSegmentBoxes,
  createMediaSegmentBoxes,
  FragmentMuxerBase,
  ftyp,
  visualSampleEntry,
} from './mp4-boxes';

/**
 * AVCDecoderConfigurationRecord（裸 record，非 box）。
 * WebCodecs description 直接使用；MP4 sample entry 由调用方包 box('avcC', ...)。
 */
export function buildAvcConfigurationRecord(sps: Uint8Array, pps: Uint8Array): Uint8Array {
  const content = new Uint8Array(11 + sps.length + pps.length);
  let i = 0;
  content[i++] = 1; // configurationVersion
  content[i++] = sps[1]; // profile_idc
  content[i++] = sps[2]; // profileCompatibility
  content[i++] = sps[3]; // level_idc
  content[i++] = 0xff; // lengthSizeMinusOne = 3
  content[i++] = 0xe1; // numOfSequenceParameterSets = 1
  content[i++] = (sps.length >> 8) & 0xff;
  content[i++] = sps.length & 0xff;
  content.set(sps, i);
  i += sps.length;
  content[i++] = 1; // numOfPictureParameterSets
  content[i++] = (pps.length >> 8) & 0xff;
  content[i++] = pps.length & 0xff;
  content.set(pps, i);
  return content;
}

export class H264Muxer extends FragmentMuxerBase {
  createInitSegment(sps: Uint8Array, pps: Uint8Array, width: number, height: number): Uint8Array {
    const entry = visualSampleEntry('avc1', box('avcC', buildAvcConfigurationRecord(sps, pps)), width, height);
    return concat(createInitSegmentBoxes(ftyp('isom', 512, ['isom', 'iso2', 'avc1']), entry, width, height));
  }
}

export { createMediaSegmentBoxes };
