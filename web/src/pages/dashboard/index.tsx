import { useTranslation } from 'react-i18next';
import { Cpu, BrainCircuit, MemoryStick, HardDrive } from 'lucide-react';
import {
  useDashboardStats,
  useSystemInfo,
  usePlatformStats,
} from '@/services/dashboard';

import ResourceCard from './components/ResourceCard';
import SystemInfoCard from './components/SystemInfoCard';
import ResourceTrendCard from './components/ResourceTrendCard';
import StreamPreview from './components/StreamPreview';
import DeviceStatusCard from './components/DeviceStatusCard';
import AppsCard from './components/AppsCard';
import GyroCalibrationCard from './components/GyroCalibrationCard';

const defaultStats = {
  cpu: { usage: 0, cores: 0 },
  npu: { usage: 0 },
  memory: { usageGB: 0, totalGB: 0, usagePercent: 0 },
  storage: {
    usageGB: 0,
    totalGB: 0,
    usagePercent: 0,
    type: 'eMMC',
    mountpoint: '/data',
  },
};

const defaultSysInfo = {
  device_name: '-',
  model: '-',
  firmware_version: '-',
  build_date: '-',
  mac_address: '-',
  ip_address: '-',
};

const defaultPlatformStats = {
  apps: { total: 0, running: 0, stopped: 0, list: [] },
  containers: { total: 0, running: 0, stopped: 0, list: [] as any[] },
  models: { total: 0, loaded: 0, list: [] },
  cameras: { total: 0, online: 0, offline: 0 },
};

// 根据百分比返回颜色类
function getProgressColor(percent: number): string {
  if (percent >= 80) return 'text-red-500';
  if (percent >= 50) return 'text-yellow-500';
  return 'text-emerald-500';
}

export default function Dashboard() {
  const { t } = useTranslation();

  const { data: stats } = useDashboardStats();
  const { data: sysInfo } = useSystemInfo();
  const { data: platformStats } = usePlatformStats();

  const safeStats = stats || defaultStats;
  const safeSysInfo = sysInfo || defaultSysInfo;
  const safePlatformStats = platformStats || defaultPlatformStats;

  return (
    <div className="p-4 mx-auto w-full h-full overflow-auto bg-background text-foreground">
      <div className="max-w-[1800px] mx-auto space-y-4">
        {/* Row 1: 设备状态(3) + CPU(2.25) + NPU(2.25) + 内存(2.25) + 硬盘(2.25) — 用 subgrid 右侧9列平分4卡 */}
        <div className="grid grid-cols-2 lg:grid-cols-12 gap-4">
          <div className="col-span-2 lg:col-span-3 w-full min-w-0">
            <DeviceStatusCard />
          </div>
          <div className="col-span-2 lg:col-span-9 grid grid-cols-2 lg:grid-cols-4 gap-4 min-w-0">
            <ResourceCard
              title={t('sys.dashboard.cpu_usage', 'CPU Usage')}
              value={safeStats.cpu.usage}
              progressPercent={safeStats.cpu.usage}
              progressColorClass={getProgressColor(safeStats.cpu.usage)}
              subtext={`${safeStats.cpu.cores} ${t('sys.device_info.cores', 'cores')}`}
              icon={<Cpu className="w-4 h-4 text-primary" />}
            />
            <ResourceCard
              title={t('sys.dashboard.npu_usage', 'NPU Usage')}
              value={`${safeStats.npu.usage}%`}
              progressPercent={safeStats.npu.usage}
              progressColorClass={getProgressColor(safeStats.npu.usage)}
              subtext={
                safeStats.npu.usage < 1
                  ? t('sys.dashboard.npu_idle', 'Idle')
                  : t('sys.dashboard.npu_active', 'Active')
              }
              icon={<BrainCircuit className="w-4 h-4 text-primary" />}
            />
            <ResourceCard
              title={t('sys.dashboard.memory_usage', 'Memory Usage')}
              value={`${safeStats.memory.usageGB} GB`}
              progressPercent={safeStats.memory.usagePercent}
              progressColorClass={getProgressColor(
                safeStats.memory.usagePercent
              )}
              subtext={t('sys.dashboard.memory_total', '{{total}} GB total', {
                total: safeStats.memory.totalGB,
              })}
              icon={<MemoryStick className="w-4 h-4 text-primary" />}
            />
            <ResourceCard
              title={t('sys.dashboard.storage_usage', 'Storage Usage')}
              value={`${safeStats.storage.usageGB} GB`}
              progressPercent={safeStats.storage.usagePercent}
              progressColorClass={getProgressColor(
                safeStats.storage.usagePercent
              )}
              subtext={t('sys.dashboard.storage_total', '{{total}} GB total', {
                total: safeStats.storage.totalGB,
              })}
              icon={<HardDrive className="w-4 h-4 text-primary" />}
            />
          </div>
        </div>

        {/* Row 2: 视频流(4) + Applications(4) + 陀螺仪校准(4) */}
        <div className="grid grid-cols-1 lg:grid-cols-12 gap-4 lg:h-[400px]">
          <div className="lg:col-span-4 min-h-0 overflow-hidden">
            <StreamPreview />
          </div>
          <div className="lg:col-span-4 min-h-0 overflow-hidden">
            <AppsCard stats={safePlatformStats.apps} />
          </div>
          <div className="lg:col-span-4 min-h-0 overflow-hidden">
            <GyroCalibrationCard />
          </div>
        </div>

        {/* Row 4: 监控趋势(9) + 设备信息(3) */}
        <div className="grid grid-cols-1 lg:grid-cols-12 gap-4 items-stretch">
          <div className="lg:col-span-9">
            <ResourceTrendCard className="h-full" />
          </div>
          <div className="lg:col-span-3">
            <SystemInfoCard info={safeSysInfo} />
          </div>
        </div>
      </div>
    </div>
  );
}
