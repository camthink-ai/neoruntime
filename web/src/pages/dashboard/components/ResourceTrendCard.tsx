import { useState, useEffect, useMemo, useRef } from 'react';
import ReactECharts from 'echarts-for-react';
import { useTranslation } from 'react-i18next';
import { Activity } from 'lucide-react';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { useMonitorSnapshot } from '@/services/dashboard';

const MAX_DATA_POINTS = 30;

type ChartType = 'usage' | 'temp' | 'network';

interface DataPoint {
  time: string;
  cpu: number;
  memory: number;
  npu: number;
  tempSoc: number;
  tempBoard: number;
  networkUp: number;
  networkDown: number;
  networkTotal: number;
}

function getCSSVar(name: string): string {
  if (typeof window === 'undefined') return '';
  return getComputedStyle(document.documentElement)
    .getPropertyValue(name)
    .trim();
}

function formatTime(d: Date): string {
  return `${d.getHours().toString().padStart(2, '0')}:${d.getMinutes().toString().padStart(2, '0')}:${d.getSeconds().toString().padStart(2, '0')}`;
}

export default function ResourceTrendCard({
  className,
}: {
  className?: string;
}) {
  const { t } = useTranslation();
  const [dataHistory, setDataHistory] = useState<DataPoint[]>([]);
  const [chartType, setChartType] = useState<ChartType>('usage');

  const prevNetworkRef = useRef<{
    bytesSent: number;
    bytesRecv: number;
    timestamp: number;
  } | null>(null);

  const [themeColors, setThemeColors] = useState(() => ({
    foreground: getCSSVar('--foreground') || '#171717',
    mutedForeground: getCSSVar('--muted-foreground') || '#737373',
    border: getCSSVar('--border') || '#e5e5e5',
    card: getCSSVar('--card') || '#ffffff',
  }));

  // Theme observer
  useEffect(() => {
    const update = () => {
      setThemeColors({
        foreground: getCSSVar('--foreground') || '#171717',
        mutedForeground: getCSSVar('--muted-foreground') || '#737373',
        border: getCSSVar('--border') || '#e5e5e5',
        card: getCSSVar('--card') || '#ffffff',
      });
    };

    const mq = window.matchMedia('(prefers-color-scheme: dark)');
    mq.addEventListener('change', update);

    const observer = new MutationObserver(update);
    observer.observe(document.documentElement, {
      attributes: true,
      attributeFilter: ['class', 'data-theme'],
    });

    return () => {
      mq.removeEventListener('change', update);
      observer.disconnect();
    };
  }, []);

  const { data: snapshot } = useMonitorSnapshot();

  // Append each new snapshot to the history
  useEffect(() => {
    if (!snapshot) return;
    const d = snapshot;

    const now = new Date();
    let networkUp = 0;
    let networkDown = 0;

    if (d.network && prevNetworkRef.current) {
      const dt = (d.timestamp - prevNetworkRef.current.timestamp) / 1000;
      if (dt > 0) {
        // 字节数 ×8 → 比特，单位 Mb/s（小 b = bit）
        networkUp =            ((d.network.bytes_sent - prevNetworkRef.current.bytesSent)
            / dt
            / (1024 * 1024))
          * 8;
        networkDown =            ((d.network.bytes_recv - prevNetworkRef.current.bytesRecv)
            / dt
            / (1024 * 1024))
          * 8;
      }
    }

    if (d.network) {
      prevNetworkRef.current = {
        bytesSent: d.network.bytes_sent,
        bytesRecv: d.network.bytes_recv,
        timestamp: d.timestamp,
      };
    }

    const point: DataPoint = {
      time: formatTime(now),
      cpu: d.cpu || 0,
      memory: d.memory || 0,
      npu: d.npu || 0,
      tempSoc: d.temperatures?.cpu || 0,
      tempBoard: d.temperatures?.board || 0,
      networkUp: Math.max(0, networkUp),
      networkDown: Math.max(0, networkDown),
      networkTotal: Math.max(0, networkUp + networkDown),
    };

    setDataHistory(prev => [...prev, point].slice(-MAX_DATA_POINTS));
  }, [snapshot]);

  const timeLabels = useMemo(() => dataHistory.map(d => d.time), [dataHistory]);

  const chartConfig = useMemo(() => {
    switch (chartType) {
      case 'usage':
        return {
          yAxisMax: 100,
          unit: '%',
          series: [
            {
              name: t('sys.dashboard.cpu', 'CPU'),
              data: dataHistory.map(d => d.cpu),
              color: '#f59e0b',
              areaColor: [
                'rgba(245, 158, 11, 0.3)',
                'rgba(245, 158, 11, 0.05)',
              ],
            },
            {
              name: t('sys.dashboard.memory', 'Memory'),
              data: dataHistory.map(d => d.memory),
              color: '#6366f1',
              areaColor: [
                'rgba(99, 102, 241, 0.3)',
                'rgba(99, 102, 241, 0.05)',
              ],
            },
            {
              name: t('sys.dashboard.npu', 'NPU'),
              data: dataHistory.map(d => d.npu),
              color: '#22c55e',
              areaColor: ['rgba(34, 197, 94, 0.3)', 'rgba(34, 197, 94, 0.05)'],
            },
          ],
        };
      case 'temp':
        return {
          yAxisMax: 80,
          unit: '\u00B0C',
          series: [
            {
              name: t('sys.dashboard.soc_temp', 'SoC'),
              data: dataHistory.map(d => d.tempSoc),
              color: '#ef4444',
              areaColor: ['rgba(239, 68, 68, 0.3)', 'rgba(239, 68, 68, 0.05)'],
            },
            {
              name: t('sys.dashboard.board_temp', 'Board'),
              data: dataHistory.map(d => d.tempBoard),
              color: '#eab308',
              areaColor: ['rgba(234, 179, 8, 0.3)', 'rgba(234, 179, 8, 0.05)'],
            },
          ],
        };
      case 'network':
        return {
          yAxisMax: undefined,
          unit: 'Mb/s',
          series: [
            {
              name: t('sys.dashboard.upload', 'Upload'),
              data: dataHistory.map(d => d.networkUp),
              color: '#06b6d4',
              areaColor: ['rgba(6, 182, 212, 0.3)', 'rgba(6, 182, 212, 0.05)'],
            },
            {
              name: t('sys.dashboard.download', 'Download'),
              data: dataHistory.map(d => d.networkDown),
              color: '#8b5cf6',
              areaColor: [
                'rgba(139, 92, 246, 0.3)',
                'rgba(139, 92, 246, 0.05)',
              ],
            },
            {
              name: t('sys.dashboard.total', 'Total'),
              data: dataHistory.map(d => d.networkTotal),
              color: '#3b82f6',
              areaColor: [
                'rgba(59, 130, 246, 0.3)',
                'rgba(59, 130, 246, 0.05)',
              ],
            },
          ],
        };
      default:
        return { yAxisMax: 100, unit: '%', series: [] };
    }
  }, [chartType, dataHistory, t]);

  const chartOption = useMemo(
    () => ({
      tooltip: {
        trigger: 'axis',
        backgroundColor: themeColors.card,
        borderColor: themeColors.border,
        textStyle: { color: themeColors.foreground },
        formatter: (params: any) => {
          let result = `${params[0].axisValue}<br/>`;
          params.forEach((param: any) => {
            result += `${param.marker}${param.seriesName}: ${param.value.toFixed(1)} ${chartConfig.unit}<br/>`;
          });
          return result;
        },
      },
      legend: {
        data: chartConfig.series.map(s => s.name),
        icon: 'circle',
        itemWidth: 8,
        itemHeight: 8,
        textStyle: { color: themeColors.mutedForeground, fontSize: 11 },
        top: 0,
        itemGap: 16,
      },
      grid: {
        left: '3%',
        right: '4%',
        bottom: 16,
        top: '15%',
        containLabel: true,
      },
      xAxis: {
        type: 'category',
        boundaryGap: false,
        data: timeLabels,
        axisLabel: {
          color: themeColors.mutedForeground,
          fontSize: 10,
          interval: 4,
        },
        axisLine: {
          lineStyle: { color: themeColors.border },
        },
      },
      yAxis: {
        type: 'value',
        name: chartConfig.unit,
        min: 0,
        max: chartConfig.yAxisMax,
        nameTextStyle: { color: themeColors.mutedForeground, fontSize: 10 },
        axisLabel: {
          color: themeColors.mutedForeground,
          fontSize: 10,
        },
        splitLine: {
          lineStyle: { color: themeColors.border, type: 'dashed' },
        },
      },
      dataZoom: [
        {
          type: 'inside',
          xAxisIndex: 0,
          filterMode: 'none',
        },
      ],
      series: chartConfig.series.map(s => ({
        name: s.name,
        type: 'line',
        smooth: true,
        symbol: 'none',
        itemStyle: { color: s.color },
        lineStyle: { width: 2, color: s.color },
        areaStyle: {
          color: {
            type: 'linear',
            x: 0,
            y: 0,
            x2: 0,
            y2: 1,
            colorStops: [
              { offset: 0, color: s.areaColor[0] },
              { offset: 1, color: s.areaColor[1] },
            ],
          },
        },
        data: s.data,
      })),
    }),
    [themeColors, timeLabels, chartConfig]
  );

  const chartTypeOptions = [
    { value: 'usage', label: t('sys.dashboard.usage', 'Usage Rate') },
    { value: 'temp', label: t('sys.dashboard.temperature', 'Temperature') },
    {
      value: 'network',
      label: t('sys.dashboard.network_speed', 'Network Speed'),
    },
  ];

  return (
    <div
      className={`bg-card rounded-2xl p-6 shadow-sm border border-border ${className || ''}`}
    >
      <div className="flex items-center justify-between mb-4">
        <h3 className="text-lg font-bold text-foreground flex items-center gap-2">
          <Activity className="w-4 h-4 text-primary" />
          {t('sys.dashboard.monitor', '监控')}
        </h3>

        <Select
          value={chartType}
          onValueChange={v => setChartType(v as ChartType)}
        >
          <SelectTrigger className="w-36 h-8 text-xs">
            <SelectValue />
          </SelectTrigger>
          <SelectContent>
            {chartTypeOptions.map(opt => (
              <SelectItem key={opt.value} value={opt.value} className="text-xs">
                {opt.label}
              </SelectItem>
            ))}
          </SelectContent>
        </Select>
      </div>

      <div className="h-48 lg:h-56">
        <ReactECharts
          option={chartOption}
          notMerge
          style={{ height: '100%', width: '100%' }}
        />
      </div>
    </div>
  );
}
