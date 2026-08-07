import { useTranslation } from 'react-i18next';
import { Label } from '@/components/ui/label';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { Input } from '@/components/ui/input';
import { Button } from '@/components/ui/button';

interface StreamConfigData {
  encodingFormat: string;
  resolution: string;
  frameRate: number;
  bitrateType: string;
  bitrate: number;
  iFrameInterval: number;
  imageQuality: string;
}

interface StreamConfigProps {
  data: StreamConfigData;
}

export default function StreamConfig({ data }: StreamConfigProps) {
  const { t } = useTranslation();

  const handleSave = () => {
    // Mock save action
    console.log('Saving stream config:', data);
  };

  return (
    <div className="space-y-6 max-w-2xl">
      <div className="space-y-4">
        {/* Encoding Format */}
        <div className="space-y-2">
          <Label htmlFor="encoding-format">
            {t('sys.media_settings.encoding_format')}
          </Label>
          <Select defaultValue={data.encodingFormat}>
            <SelectTrigger className="w-full">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="H.264">H.264</SelectItem>
              <SelectItem value="H.265">H.265</SelectItem>
              <SelectItem value="MJPEG">MJPEG</SelectItem>
            </SelectContent>
          </Select>
        </div>

        {/* Resolution */}
        <div className="space-y-2">
          <Label htmlFor="resolution">
            {t('sys.media_settings.resolution')}
          </Label>
          <Select defaultValue={data.resolution}>
            <SelectTrigger className="w-full">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="3840x2160">3840x2160 (4K)</SelectItem>
              <SelectItem value="1920x1080">1920x1080 (1080P)</SelectItem>
              <SelectItem value="1280x720">1280x720 (720P)</SelectItem>
              <SelectItem value="640x480">640x480 (VGA)</SelectItem>
              <SelectItem value="320x240">320x240 (QVGA)</SelectItem>
            </SelectContent>
          </Select>
        </div>

        {/* Frame Rate */}
        <div className="space-y-2">
          <Label htmlFor="frame-rate">
            {t('sys.media_settings.frame_rate')}
          </Label>
          <div className="flex items-center space-x-2">
            <Input
              id="frame-rate"
              type="number"
              defaultValue={data.frameRate}
              min={1}
              max={60}
              className="flex-1"
            />
            <span className="text-sm text-muted-foreground">
              {t('common.fps')}
            </span>
          </div>
        </div>

        {/* Bitrate Type */}
        <div className="space-y-2">
          <Label htmlFor="bitrate-type">
            {t('sys.media_settings.bitrate_type')}
          </Label>
          <Select defaultValue={data.bitrateType}>
            <SelectTrigger className="w-full">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="CBR">CBR</SelectItem>
              <SelectItem value="VBR">VBR</SelectItem>
            </SelectContent>
          </Select>
        </div>

        {/* Bitrate */}
        <div className="space-y-2">
          <Label htmlFor="bitrate">{t('sys.media_settings.bitrate')}</Label>
          <div className="flex items-center space-x-2">
            <Input
              id="bitrate"
              type="number"
              defaultValue={data.bitrate}
              min={64}
              max={20480}
              className="flex-1"
            />
            <span className="text-sm text-muted-foreground">
              {t('common.kbps')}
            </span>
          </div>
        </div>

        {/* I-Frame Interval */}
        <div className="space-y-2">
          <Label htmlFor="iframe-interval">
            {t('sys.media_settings.i_frame_interval')}
          </Label>
          <div className="flex items-center space-x-2">
            <Input
              id="iframe-interval"
              type="number"
              defaultValue={data.iFrameInterval}
              min={1}
              max={300}
              className="flex-1"
            />
            <span className="text-sm text-muted-foreground">
              {t('common.seconds')}
            </span>
          </div>
        </div>

        {/* Image Quality */}
        <div className="space-y-2">
          <Label htmlFor="image-quality">
            {t('sys.media_settings.image_quality')}
          </Label>
          <Select defaultValue={data.imageQuality}>
            <SelectTrigger className="w-full">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="high">{t('common.high')}</SelectItem>
              <SelectItem value="medium">{t('common.medium')}</SelectItem>
              <SelectItem value="low">{t('common.low')}</SelectItem>
            </SelectContent>
          </Select>
        </div>
      </div>

      <div className="flex justify-end space-x-2 pt-4">
        <Button variant="outline" onClick={() => window.location.reload()}>
          {t('common.reset')}
        </Button>
        <Button onClick={handleSave}>{t('common.save')}</Button>
      </div>
    </div>
  );
}
