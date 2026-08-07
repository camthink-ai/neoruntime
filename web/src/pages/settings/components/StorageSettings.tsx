import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { HardDrive, AlertTriangle } from 'lucide-react';
import { Button } from '@/components/ui/button';
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '@/components/ui/alert-dialog';
import { Progress } from '@/components/ui/progress';

export default function StorageSettings() {
  const { t } = useTranslation();
  const [showFormatDialog, setShowFormatDialog] = useState(false);
  const [showEjectDialog, setShowEjectDialog] = useState(false);
  const [isFormatting, setIsFormatting] = useState(false);
  const [isEjecting, setIsEjecting] = useState(false);

  // Mock SD card data
  const sdCardData = {
    status: 'mounted',
    totalCapacity: 128, // GB
    usedSpace: 45.6, // GB
    availableSpace: 82.4, // GB
  };

  const usagePercentage =    (sdCardData.usedSpace / sdCardData.totalCapacity) * 100;

  const handleFormat = async () => {
    setIsFormatting(true);
    // Mock format operation
    await new Promise(resolve => {
      setTimeout(resolve, 2000);
    });
    setIsFormatting(false);
    setShowFormatDialog(false);
    console.log('SD card formatted');
  };

  const handleEject = async () => {
    setIsEjecting(true);
    // Mock eject operation
    await new Promise(resolve => {
      setTimeout(resolve, 1000);
    });
    setIsEjecting(false);
    setShowEjectDialog(false);
    console.log('SD card ejected');
  };

  const formatBytes = (bytes: number) => `${bytes.toFixed(1)} GB`;

  return (
    <div className="space-y-6">
      <div>
        <h2 className="text-2xl font-semibold">
          {t('sys.storage_settings.title')}
        </h2>
      </div>

      <div className="max-w-2xl space-y-6">
        {/* SD Card Info */}
        <div className="border rounded-lg p-6 space-y-4">
          <div className="flex items-center space-x-3">
            <HardDrive className="h-6 w-6 text-primary" />
            <h3 className="text-lg font-semibold">
              {t('sys.storage_settings.sd_card')}
            </h3>
          </div>

          <div className="space-y-4">
            {/* Status */}
            <div className="flex items-center justify-between py-2">
              <span className="text-sm text-muted-foreground">
                {t('sys.storage_settings.status')}
              </span>
              <span className="font-medium">
                {sdCardData.status === 'mounted'
                  ? t('sys.storage_settings.mounted')
                  : t('sys.storage_settings.unmounted')}
              </span>
            </div>

            {/* Total Capacity */}
            <div className="flex items-center justify-between py-2">
              <span className="text-sm text-muted-foreground">
                {t('sys.storage_settings.total_capacity')}
              </span>
              <span className="font-medium">
                {formatBytes(sdCardData.totalCapacity)}
              </span>
            </div>

            {/* Used Space */}
            <div className="flex items-center justify-between py-2">
              <span className="text-sm text-muted-foreground">
                {t('sys.storage_settings.used_space')}
              </span>
              <span className="font-medium">
                {formatBytes(sdCardData.usedSpace)}
              </span>
            </div>

            {/* Available Space */}
            <div className="flex items-center justify-between py-2">
              <span className="text-sm text-muted-foreground">
                {t('sys.storage_settings.available_space')}
              </span>
              <span className="font-medium">
                {formatBytes(sdCardData.availableSpace)}
              </span>
            </div>

            {/* Usage Progress */}
            <div className="space-y-2 pt-2">
              <Progress value={usagePercentage} className="h-2" />
              <p className="text-xs text-muted-foreground text-right">
                {usagePercentage.toFixed(1)}% {t('common.used')}
              </p>
            </div>
          </div>

          {/* Actions */}
          <div className="flex space-x-3 pt-4">
            <Button
              variant="outline"
              onClick={() => setShowEjectDialog(true)}
              disabled={sdCardData.status !== 'mounted' || isEjecting}
            >
              {isEjecting
                ? t('sys.storage_settings.ejecting')
                : t('sys.storage_settings.eject')}
            </Button>
            <Button
              variant="destructive"
              onClick={() => setShowFormatDialog(true)}
              disabled={sdCardData.status !== 'mounted' || isFormatting}
            >
              {isFormatting
                ? t('sys.storage_settings.formatting')
                : t('sys.storage_settings.format')}
            </Button>
          </div>
        </div>
      </div>

      {/* Format Confirmation Dialog */}
      <AlertDialog open={showFormatDialog} onOpenChange={setShowFormatDialog}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>
              <div className="flex items-center space-x-2">
                <AlertTriangle className="h-5 w-5 text-destructive" />
                <span>{t('sys.storage_settings.format')}</span>
              </div>
            </AlertDialogTitle>
            <AlertDialogDescription>
              {t('sys.storage_settings.format_warning')}
              <br />
              <br />
              <strong>{t('sys.storage_settings.format_confirm')}</strong>
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('common.cancel')}</AlertDialogCancel>
            <AlertDialogAction
              onClick={handleFormat}
              className="bg-destructive hover:bg-destructive/90"
            >
              {t('common.confirm')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>

      {/* Eject Confirmation Dialog */}
      <AlertDialog open={showEjectDialog} onOpenChange={setShowEjectDialog}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>
              {t('sys.storage_settings.eject')}
            </AlertDialogTitle>
            <AlertDialogDescription>
              {t('sys.storage_settings.eject_warning')}
              <br />
              <br />
              <strong>{t('sys.storage_settings.eject_confirm')}</strong>
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('common.cancel')}</AlertDialogCancel>
            <AlertDialogAction onClick={handleEject}>
              {t('common.confirm')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </div>
  );
}
