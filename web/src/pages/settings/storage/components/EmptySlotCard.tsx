import { useTranslation } from 'react-i18next';
import { Card, CardContent } from '@/components/ui/card';
import { HardDrive, Usb } from 'lucide-react';
import type { StorageDeviceType } from '@/services/types';

export default function EmptySlotCard({ type }: { type: StorageDeviceType }) {
  const { t } = useTranslation();
  const isSdCard = type === 'sd_card';
  const Icon = isSdCard ? HardDrive : Usb;

  return (
    <Card className="border-dashed">
      <CardContent className="py-6">
        <div className="flex items-center gap-3">
          <div className="rounded-lg bg-muted/60 p-2.5">
            <Icon className="h-5 w-5 text-muted-foreground/50" />
          </div>
          <div>
            <span className="text-sm font-medium text-muted-foreground">
              {isSdCard
                ? t('sys.storage.sd_card_label', 'SD 卡')
                : t('sys.storage.usb_label', 'USB 存储')}
            </span>
            <p className="text-xs text-muted-foreground/60 mt-0.5">
              {isSdCard
                ? t(
                    'sys.storage.insert_sd_card',
                    '插入 microSD 卡以扩展存储容量'
                  )
                : t('sys.storage.insert_usb', '插入 USB 存储设备以扩展容量')}
            </p>
          </div>
        </div>
      </CardContent>
    </Card>
  );
}
