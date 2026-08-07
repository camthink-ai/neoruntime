import { useState } from 'react';
import { Button } from '@/components/ui/button';
import { useTranslation } from 'react-i18next';
import { Download, CheckCircle, AlertCircle, Loader2 } from 'lucide-react';
import Loading from '@/components/loading';
import { Progress } from '@/components/ui/progress';

type UpgradeStatus =
  | 'idle'
  | 'checking'
  | 'available'
  | 'upgrading'
  | 'success'
  | 'failed';

export default function FirmwareUpgrade() {
  const { t } = useTranslation();
  const [status, setStatus] = useState<UpgradeStatus>('idle');
  const [progress, setProgress] = useState(0);

  const currentVersion = 'v2.1.0';
  const latestVersion = 'v2.2.0';

  const handleCheckUpdate = () => {
    setStatus('checking');
    setTimeout(() => {
      setStatus('available');
    }, 1500);
  };

  const handleStartUpgrade = () => {
    setStatus('upgrading');
    setProgress(0);

    const interval = setInterval(() => {
      setProgress(prev => {
        if (prev >= 100) {
          clearInterval(interval);
          setTimeout(() => {
            setStatus('success');
          }, 500);
          return 100;
        }
        return prev + 10;
      });
    }, 300);
  };

  const renderContent = () => {
    switch (status) {
      case 'checking':
        return <Loading className="py-8" fullHeight={false} />;

      case 'available':
        return (
          <div className="space-y-4">
            <div className="flex items-center justify-between py-3 border-b border-border">
              <span className="text-muted-foreground">
                {t('sys.firmware_settings.current', 'Current Firmware')}
              </span>
              <span className="font-medium">{currentVersion}</span>
            </div>
            <div className="flex items-center justify-between py-3 border-b border-border">
              <span className="text-muted-foreground">
                {t('sys.firmware_settings.latest', 'Latest Firmware')}
              </span>
              <span className="font-medium text-primary">{latestVersion}</span>
            </div>
            <Button onClick={handleStartUpgrade} className="w-full">
              <Download className="h-4 w-4 mr-2" />
              {t('sys.firmware_settings.start', 'Start Upgrade')}
            </Button>
          </div>
        );

      case 'upgrading':
        return (
          <div className="space-y-4">
            <div className="flex items-center space-x-3">
              <Loader2 className="h-5 w-5 animate-spin text-primary" />
              <span className="font-medium">
                {t('sys.firmware_settings.upgrading', 'Upgrading')}
              </span>
            </div>
            <Progress value={progress} className="w-full" />
            <div className="text-center text-sm text-muted-foreground">
              {progress}%
            </div>
          </div>
        );

      case 'success':
        return (
          <div className="flex flex-col items-center justify-center py-8 space-y-3">
            <CheckCircle className="h-12 w-12 text-green-600" />
            <div className="text-lg font-medium">
              {t('sys.firmware_settings.success', 'Upgrade Successful')}
            </div>
            <div className="text-sm text-muted-foreground">
              {currentVersion} → {latestVersion}
            </div>
          </div>
        );

      case 'failed':
        return (
          <div className="flex flex-col items-center justify-center py-8 space-y-3">
            <AlertCircle className="h-12 w-12 text-destructive" />
            <div className="text-lg font-medium">
              {t('sys.firmware_settings.failed', 'Upgrade Failed')}
            </div>
            <Button variant="outline" onClick={handleStartUpgrade}>
              {t('common.retry', 'Retry')}
            </Button>
          </div>
        );

      default:
        return (
          <div className="space-y-4">
            <div className="flex items-center justify-between py-3 border-b border-border">
              <span className="text-muted-foreground">
                {t('sys.firmware_settings.current', 'Current Firmware')}
              </span>
              <span className="font-medium">{currentVersion}</span>
            </div>
            <Button onClick={handleCheckUpdate} className="w-full">
              {t('sys.firmware_settings.check', 'Check Update')}
            </Button>
          </div>
        );
    }
  };

  return (
    <div className="space-y-4 max-w-xl">
      <h2 className="text-xl font-semibold mb-4">
        {t('sys.firmware_settings.title', 'Firmware Upgrade')}
      </h2>
      {renderContent()}
    </div>
  );
}
