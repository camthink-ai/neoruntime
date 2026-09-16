import { useTranslation } from 'react-i18next';
import { Checkbox } from '@/components/ui/checkbox';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { ScrollArea } from '@/components/ui/scroll-area';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import type { WizardConfig } from '@/services/types';

export interface PermissionsSectionProps {
  config: WizardConfig;
  onChange: (next: WizardConfig) => void;
  availableStreams: Array<{
    stream_id: string;
    width?: number;
    height?: number;
    fps?: number;
    status?: string;
  }>;
}

/**
 * 权限设置 page of the paginated import form: IO permissions only — video
 * streams, event topics, network mode and device control. Inference
 * permissions (models, QPS, concurrency) live on 模型配置.
 */
export default function PermissionsSection({
  config,
  onChange,
  availableStreams,
}: PermissionsSectionProps) {
  const { t } = useTranslation();

  return (
    <div className="space-y-6 pr-2 sm:pr-4">
      {/* Video Streams */}
      <div>
        <Label className="text-base font-semibold mb-3 block">
          {t('sys.apps.import.video_streams', 'Video Stream Permissions')}
        </Label>
        <ScrollArea className="max-h-[200px] border rounded-lg p-3">
          <div className="flex flex-wrap gap-2 pr-4">
            {availableStreams.length > 0 ? (
              availableStreams.map(stream => {
                const streamValue = stream.stream_id;
                return (
                  <label
                    key={stream.stream_id}
                    className={`inline-flex items-center space-x-2 px-3 py-1.5 rounded-full border cursor-pointer transition-all ${
                      config.permissions?.video?.includes(streamValue)
                        ? 'border-[#f24a00] bg-[#fff5f0] text-[#f24a00]'
                        : 'border-gray-200 hover:border-gray-300'
                    }`}
                  >
                    <Checkbox
                      checked={config.permissions?.video?.includes(streamValue)}
                      onCheckedChange={checked => {
                        const current = config.permissions?.video || [];
                        const updated = checked
                          ? [...current, streamValue]
                          : current.filter(v => v !== streamValue);
                        onChange({
                          ...config,
                          permissions: {
                            ...config.permissions!,
                            video: updated,
                          },
                        });
                      }}
                      className="sr-only"
                    />
                    <span className="text-sm">
                      {stream.stream_id}
                      {stream.width && stream.height && (
                        <span className="text-xs text-gray-400 ml-1">
                          ({stream.width}x{stream.height}
                          {stream.fps ? `@${stream.fps}` : ''})
                        </span>
                      )}
                    </span>
                  </label>
                );
              })
            ) : (
              <p className="text-sm text-gray-500">
                {t('sys.apps.import.no_streams', 'No available video streams')}
              </p>
            )}
          </div>
        </ScrollArea>
      </div>

      {/* Events */}
      <div>
        <Label className="text-base font-semibold mb-3 block">
          {t('sys.apps.import.events', 'Event Permissions')}
        </Label>
        <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
          <div>
            <Label className="text-sm text-muted-foreground">
              {t('sys.apps.import.events_publish', 'Publish Topics')}
            </Label>
            <Input
              placeholder="app/output"
              value={config.permissions?.events?.publish?.join(', ') || ''}
              onChange={e => onChange({
                  ...config,
                  permissions: {
                    ...config.permissions!,
                    events: {
                      ...config.permissions!.events!,
                      publish: e.target.value
                        .split(',')
                        .map(s => s.trim())
                        .filter(Boolean),
                    },
                  },
                })}
              className="mt-1"
            />
            <p className="text-xs text-muted-foreground mt-1">
              {t('sys.apps.import.events_hint', 'Separate topics with commas')}
            </p>
          </div>
          <div>
            <Label className="text-sm text-muted-foreground">
              {t('sys.apps.import.events_subscribe', 'Subscribe Topics')}
            </Label>
            <Input
              placeholder="camera/*, sensor/#"
              value={config.permissions?.events?.subscribe?.join(', ') || ''}
              onChange={e => onChange({
                  ...config,
                  permissions: {
                    ...config.permissions!,
                    events: {
                      ...config.permissions!.events!,
                      subscribe: e.target.value
                        .split(',')
                        .map(s => s.trim())
                        .filter(Boolean),
                    },
                  },
                })}
              className="mt-1"
            />
            <p className="text-xs text-muted-foreground mt-1">
              {t('sys.apps.import.events_hint', 'Separate topics with commas')}
            </p>
          </div>
        </div>
      </div>

      {/* Network */}
      <div>
        <Label className="text-base font-semibold mb-3 block">
          {t('sys.apps.import.network', 'Network Mode')}
        </Label>
        <Select
          value={config.permissions?.network?.mode || 'isolated'}
          onValueChange={value => onChange({
              ...config,
              permissions: {
                ...config.permissions!,
                network: {
                  ...config.permissions!.network,
                  mode: value,
                },
              },
            })}
        >
          <SelectTrigger className="mt-2 w-full sm:w-48">
            <SelectValue />
          </SelectTrigger>
          <SelectContent>
            <SelectItem value="isolated">
              {t('sys.apps.import.network_isolated', 'Isolated')}
            </SelectItem>
            <SelectItem value="host">
              {t('sys.apps.import.network_host', 'Host')}
            </SelectItem>
          </SelectContent>
        </Select>
        <p className="text-xs text-gray-400 mt-1">
          {t(
            'sys.apps.import.network_hint',
            '隔离模式：无网络访问；主机模式：共享主机网络'
          )}
        </p>
        {config.permissions?.network?.mode === 'host' && (
          <div className="mt-3">
            <Label className="text-sm text-muted-foreground">
              {t('sys.apps.import.inbound_ports', '入站端口')}
            </Label>
            <Input
              className="mt-1 w-full sm:w-48"
              placeholder="e.g. 8889"
              value={(config.permissions?.network?.inbound || []).join(', ')}
              onChange={e => {
                const ports = e.target.value
                  .split(/[,\s]+/)
                  .map(s => parseInt(s.trim(), 10))
                  .filter(n => !Number.isNaN(n) && n > 0 && n < 65536);
                onChange({
                  ...config,
                  permissions: {
                    ...config.permissions!,
                    network: {
                      ...config.permissions!.network!,
                      inbound: ports,
                    },
                  },
                });
              }}
            />
            <p className="text-xs text-gray-400 mt-1">
              {t(
                'sys.apps.import.inbound_hint',
                '应用对外暴露的端口，多个用逗号分隔'
              )}
            </p>
          </div>
        )}
      </div>

      {/* Device Control */}
      <div>
        <Label className="text-base font-semibold mb-3 block">
          {t('sys.apps.import.device_control')}
        </Label>
        <div className="space-y-2 pr-4 border rounded-lg p-3">
          <div className="flex items-center space-x-2">
            <Checkbox
              checked={config.permissions?.device?.light}
              onCheckedChange={checked => onChange({
                  ...config,
                  permissions: {
                    ...config.permissions!,
                    device: {
                      ...config.permissions!.device!,
                      light: !!checked,
                    },
                  },
                })}
            />
            <Label className="font-normal">
              {t('sys.apps.import.light_control')}
            </Label>
          </div>
          <div className="flex items-center space-x-2">
            <Checkbox
              checked={config.permissions?.device?.ir_cut}
              onCheckedChange={checked => onChange({
                  ...config,
                  permissions: {
                    ...config.permissions!,
                    device: {
                      ...config.permissions!.device!,
                      ir_cut: !!checked,
                    },
                  },
                })}
            />
            <Label className="font-normal">{t('sys.apps.import.ir_cut')}</Label>
          </div>
          <div className="flex items-center space-x-2">
            <Checkbox
              checked={config.permissions?.device?.ptz}
              onCheckedChange={checked => onChange({
                  ...config,
                  permissions: {
                    ...config.permissions!,
                    device: {
                      ...config.permissions!.device!,
                      ptz: !!checked,
                    },
                  },
                })}
            />
            <Label className="font-normal">
              {t('sys.apps.import.ptz_control')}
            </Label>
          </div>
          <div className="flex items-center space-x-2">
            <Checkbox
              checked={config.permissions?.device?.lens}
              onCheckedChange={checked => onChange({
                  ...config,
                  permissions: {
                    ...config.permissions!,
                    device: {
                      ...config.permissions!.device!,
                      lens: !!checked,
                    },
                  },
                })}
            />
            <Label className="font-normal">
              {t('sys.apps.import.lens_control', 'Lens Control')}
            </Label>
          </div>
        </div>
      </div>
    </div>
  );
}
