import { useTranslation } from 'react-i18next';
import {
  Smartphone,
  Cpu,
  Hash,
  HardDrive,
  Wifi,
  Network,
  Clock,
} from 'lucide-react';

interface InfoItemProps {
  icon: React.ReactNode;
  label: string;
  value: string;
}

function InfoItem({ icon, label, value }: InfoItemProps) {
  return (
    <div className="flex items-center space-x-3 py-3">
      <div className="text-muted-foreground">{icon}</div>
      <div className="flex-1">
        <div className="text-sm text-muted-foreground">{label}</div>
        <div className="font-medium">{value}</div>
      </div>
    </div>
  );
}

export default function DeviceInfo() {
  const { t } = useTranslation();

  const deviceData = {
    name: 'NE503-001',
    model: 'NE503',
    serialNumber: 'SN20240301001',
    firmwareVersion: 'v2.1.0',
    hardwareVersion: 'v1.0',
    ipAddress: '192.168.1.100',
    macAddress: '00:1A:2B:3C:4D:5E',
    uptime: `15 ${t('common.days')}`,
  };

  return (
    <div className="space-y-4 max-w-2xl border-b w-full">
      <div className="space-y-2">
        <InfoItem
          icon={<Smartphone className="h-5 w-5" />}
          label={t('sys.device_info.name', 'Device Name')}
          value={deviceData.name}
        />
        <InfoItem
          icon={<Cpu className="h-5 w-5" />}
          label={t('sys.device_info.model', 'Device Model')}
          value={deviceData.model}
        />
        <InfoItem
          icon={<Hash className="h-5 w-5" />}
          label={t('sys.device_info.sn', 'Serial Number')}
          value={deviceData.serialNumber}
        />
        <InfoItem
          icon={<HardDrive className="h-5 w-5" />}
          label={t('sys.device_info.firmware_version', 'Firmware Version')}
          value={deviceData.firmwareVersion}
        />
        <InfoItem
          icon={<HardDrive className="h-5 w-5" />}
          label={t('sys.device_info.hardware_version', 'Hardware Version')}
          value={deviceData.hardwareVersion}
        />
        <InfoItem
          icon={<Wifi className="h-5 w-5" />}
          label={t('common.ip_address')}
          value={deviceData.ipAddress}
        />
        <InfoItem
          icon={<Network className="h-5 w-5" />}
          label={t('common.mac_address')}
          value={deviceData.macAddress}
        />
        <InfoItem
          icon={<Clock className="h-5 w-5" />}
          label={t('common.uptime')}
          value={deviceData.uptime}
        />
      </div>
    </div>
  );
}
