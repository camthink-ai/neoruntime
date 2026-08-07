import type { PlatformStats } from '@/services/types';
import AppsCard from './AppsCard';
import ModelsCard from './ModelsCard';

interface PlatformStatsCardProps {
  stats: PlatformStats;
}

export default function PlatformStatsCard({ stats }: PlatformStatsCardProps) {
  return (
    <div className="grid grid-cols-1 lg:grid-cols-2 gap-6">
      <AppsCard stats={stats.apps} />
      <ModelsCard stats={stats.models} />
    </div>
  );
}
