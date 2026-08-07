import StreamConfig from './StreamConfig';

export default function SubStream() {
  const mockData = {
    encodingFormat: 'H.264',
    resolution: '640x480',
    frameRate: 15,
    bitrateType: 'CBR',
    bitrate: 512,
    iFrameInterval: 4,
    imageQuality: 'medium',
  };

  return <StreamConfig data={mockData} />;
}
