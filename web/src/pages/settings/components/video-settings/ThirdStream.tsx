import StreamConfig from './StreamConfig';

export default function ThirdStream() {
  const mockData = {
    encodingFormat: 'H.264',
    resolution: '320x240',
    frameRate: 10,
    bitrateType: 'CBR',
    bitrate: 256,
    iFrameInterval: 5,
    imageQuality: 'low',
  };

  return <StreamConfig data={mockData} />;
}
