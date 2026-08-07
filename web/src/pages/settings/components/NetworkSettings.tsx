import { useState, useEffect } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { Label } from '@/components/ui/label';
import { Input } from '@/components/ui/input';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { Button } from '@/components/ui/button';
import { Skeleton } from '@/components/ui/skeleton';
import {
  fetchNetworkConfig,
  updateNetworkConfig,
  type NetworkConfig,
} from '@/services/settings';

export default function NetworkSettings() {
  const { t } = useTranslation();

  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [mode, setMode] = useState<'dhcp' | 'static'>('dhcp');
  const [ipAddress, setIpAddress] = useState('');
  const [subnetMask, setSubnetMask] = useState('');
  const [gateway, setGateway] = useState('');
  const [dns1, setDns1] = useState('');
  const [dns2, setDns2] = useState('');
  const [ifaceName, setIfaceName] = useState('');
  const [macAddress, setMacAddress] = useState('');

  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const config = await fetchNetworkConfig();
        if (cancelled) return;
        setMode(config.mode);
        setIpAddress(config.ip_address || '');
        setSubnetMask(config.subnet_mask || '');
        setGateway(config.gateway || '');
        setDns1(config.dns1 || '');
        setDns2(config.dns2 || '');
        setIfaceName(config.interface || '');
        setMacAddress(config.mac_address || '');
      } catch {
        if (!cancelled) {
          toast.error(
            t('sys.network_settings.load_failed', '加载网络配置失败')
          );
        }
      } finally {
        if (!cancelled) setLoading(false);
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [t]);

  const handleSave = async () => {
    setSaving(true);
    try {
      const payload: Partial<NetworkConfig> = {
        interface: ifaceName || 'eth0',
        mode,
        ip_address: mode === 'static' ? ipAddress : '',
        subnet_mask: mode === 'static' ? subnetMask : '',
        gateway: mode === 'static' ? gateway : '',
        dns1: mode === 'static' ? dns1 : '',
        dns2: mode === 'static' ? dns2 : '',
      };
      await updateNetworkConfig(payload);
      toast.success(
        t('sys.network_settings.save_success', '网络配置已保存，正在应用...')
      );
    } catch {
      toast.error(t('sys.network_settings.save_failed', '保存网络配置失败'));
    } finally {
      setSaving(false);
    }
  };

  const handleReset = () => window.location.reload();

  const fieldsDisabled = mode === 'dhcp';

  if (loading) {
    return (
      <div className="space-y-6 max-w-2xl">
        <Skeleton className="h-8 w-48" />
        <div className="space-y-4">
          {Array.from({ length: 6 }).map((_, i) => (
            <div key={i} className="space-y-2">
              <Skeleton className="h-4 w-24" />
              <Skeleton className="h-10 w-full" />
            </div>
          ))}
        </div>
      </div>
    );
  }

  return (
    <div className="space-y-6 max-w-2xl">
      <div>
        <h2 className="text-2xl font-semibold">
          {t('sys.network_settings.title')}
        </h2>
        {ifaceName && (
          <p className="text-sm text-muted-foreground mt-1">
            {ifaceName}
            {macAddress ? ` · ${macAddress}` : ''}
          </p>
        )}
      </div>

      <div className="space-y-4">
        <div>
          <h3 className="text-lg font-medium">
            {t('sys.network_settings.ipv4')}
          </h3>
        </div>

        {/* Mode */}
        <div className="space-y-2">
          <Label htmlFor="ipv4-mode">{t('sys.network_settings.mode')}</Label>
          <Select
            value={mode}
            onValueChange={v => setMode(v as 'dhcp' | 'static')}
          >
            <SelectTrigger className="w-full">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="dhcp">
                {t('sys.network_settings.dhcp')}
              </SelectItem>
              <SelectItem value="static">
                {t('sys.network_settings.static')}
              </SelectItem>
            </SelectContent>
          </Select>
        </div>

        {/* IP Address */}
        <div className="space-y-2">
          <Label htmlFor="ipv4-ip">
            {t('sys.network_settings.ip_address')}
          </Label>
          <Input
            id="ipv4-ip"
            type="text"
            value={ipAddress}
            onChange={e => setIpAddress(e.target.value)}
            disabled={fieldsDisabled}
            placeholder="192.168.1.100"
          />
        </div>

        {/* Subnet Mask */}
        <div className="space-y-2">
          <Label htmlFor="ipv4-subnet">
            {t('sys.network_settings.subnet_mask')}
          </Label>
          <Input
            id="ipv4-subnet"
            type="text"
            value={subnetMask}
            onChange={e => setSubnetMask(e.target.value)}
            disabled={fieldsDisabled}
            placeholder="255.255.255.0"
          />
        </div>

        {/* Gateway */}
        <div className="space-y-2">
          <Label htmlFor="ipv4-gateway">
            {t('sys.network_settings.gateway')}
          </Label>
          <Input
            id="ipv4-gateway"
            type="text"
            value={gateway}
            onChange={e => setGateway(e.target.value)}
            disabled={fieldsDisabled}
            placeholder="192.168.1.1"
          />
        </div>

        {/* DNS */}
        <div className="grid gap-4 md:grid-cols-2">
          <div className="space-y-2">
            <Label htmlFor="ipv4-dns1">
              {t('sys.network_settings.primary_dns')}
            </Label>
            <Input
              id="ipv4-dns1"
              type="text"
              value={dns1}
              onChange={e => setDns1(e.target.value)}
              disabled={fieldsDisabled}
              placeholder="8.8.8.8"
            />
          </div>
          <div className="space-y-2">
            <Label htmlFor="ipv4-dns2">
              {t('sys.network_settings.secondary_dns')}
            </Label>
            <Input
              id="ipv4-dns2"
              type="text"
              value={dns2}
              onChange={e => setDns2(e.target.value)}
              disabled={fieldsDisabled}
              placeholder="8.8.4.4"
            />
          </div>
        </div>
      </div>

      <div className="flex justify-end space-x-2 pt-4">
        <Button variant="outline" onClick={handleReset}>
          {t('common.reset')}
        </Button>
        <Button onClick={handleSave} disabled={saving}>
          {saving ? t('common.saving', '保存中...') : t('common.save')}
        </Button>
      </div>
    </div>
  );
}
