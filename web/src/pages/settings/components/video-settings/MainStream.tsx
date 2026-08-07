import StreamConfig from './StreamConfig';

export default function MainStream() {
  const mockData = {
    encodingFormat: 'H.265',
    resolution: '1920x1080',
    frameRate: 30,
    bitrateType: 'VBR',
    bitrate: 4096,
    iFrameInterval: 2,
    imageQuality: 'high',
  };

  return <StreamConfig data={mockData} />;
}
