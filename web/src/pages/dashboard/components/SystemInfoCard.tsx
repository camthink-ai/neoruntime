import { useTranslation } from 'react-i18next';
import type { SystemInfo } from '@/services/types';
import { Cpu, HardDrive, Calendar, Network } from 'lucide-react';

interface SystemInfoCardProps {
  info: SystemInfo;
}

export default function SystemInfoCard({ info }: SystemInfoCardProps) {
  const { t } = useTranslation();

  return (
    <div className="bg-card rounded-2xl p-5 shadow-sm border border-border h-full">
      <h3 className="text-base font-bold text-foreground mb-4 flex items-center gap-2">
        <Cpu className="w-4 h-4 text-primary" />
        {t('sys.dashboard.device_info', '设备信息')}
      </h3>

      <div className="space-y-1">
        {/* Device Name + Model */}
        <div className="flex items-center justify-between py-3 px-3 rounded-lg">
          <span className="text-sm text-muted-foreground flex items-center gap-2">
            <HardDrive className="w-3.5 h-3.5 opacity-60" />
            {t('sys.device_info.name', '设备名称')}
          </span>
          <span className="text-sm font-medium text-foreground font-mono bg-card/50 px-2 py-0.5 rounded">
            {info.device_name}
          </span>
        </div>

        {/* IP / MAC */}
        <div className="flex items-center justify-between py-3 px-3 rounded-lg">
          <span className="text-sm text-muted-foreground flex items-center gap-2">
            <Network className="w-3.5 h-3.5 opacity-60" />
            {t('sys.device_info.ip_address', 'IP 地址')}
          </span>
          <span className="text-sm font-medium text-foreground font-mono bg-card/50 px-2 py-0.5 rounded">
            {info.ip_address}
          </span>
        </div>
        <div className="flex items-center justify-between py-3 px-3 rounded-lg">
          <span className="text-sm text-muted-foreground flex items-center gap-2">
            <Network className="w-3.5 h-3.5 opacity-60" />
            {t('sys.device_info.mac_address', 'Mac地址')}
          </span>
          <span className="text-sm font-medium text-foreground font-mono bg-card/50 px-2 py-0.5 rounded">
            {info.mac_address}
          </span>
        </div>

        {/* Firmware Version + Build Date */}
        <div className="flex items-center justify-between py-3 px-3 rounded-lg">
          <span className="text-sm text-muted-foreground flex items-center gap-2">
            <HardDrive className="w-3.5 h-3.5 opacity-60" />
            {t('sys.device_info.firmware_version', '固件版本')}
          </span>
          <span className="text-sm font-medium text-foreground font-mono bg-card/50 px-2 py-0.5 rounded">
            {info.firmware_version}
          </span>
        </div>
        <div className="flex items-center justify-between py-3 px-3 rounded-lg">
          <span className="text-sm text-muted-foreground flex items-center gap-2">
            <Calendar className="w-3.5 h-3.5 opacity-60" />
            {t('sys.device_info.build_date', '构建日期')}
          </span>
          <span className="text-sm font-medium text-foreground font-mono bg-card/50 px-2 py-0.5 rounded">
            {info.build_date}
          </span>
        </div>
      </div>
    </div>
  );
}
