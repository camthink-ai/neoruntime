import { useState, useEffect, useCallback } from 'react';
import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { useTranslation } from 'react-i18next';
import type { TFunction } from 'i18next';
import { Card } from '@/components/ui/card';
import { Input } from '@/components/ui/input';
import { Button } from '@/components/ui/button';
import { Label } from '@/components/ui/label';
import {
  fetchNetworkConfig,
  updateNetworkConfig,
  type NetworkConfig,
} from '@/services/settings';
import {
  Network,
  Save,
  Wifi,
  AlertTriangle,
  Copy,
  Check,
  ExternalLink,
} from 'lucide-react';
import { toast } from 'sonner';
import { NetworkSkeleton } from './components/NetworkSkeleton';
import ErrorState from '@/components/ErrorState';
import {
  AlertDialog,
  AlertDialogContent,
  AlertDialogHeader,
  AlertDialogTitle,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogCancel,
  AlertDialogAction,
} from '@/components/ui/alert-dialog';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';

function buildAccessUrl(ip: string) {
  const { protocol } = window.location;
  const { port } = window.location;
  if (port && port !== '80' && port !== '443') {
    return `${protocol}//${ip}:${port}`;
  }
  return `${protocol}//${ip}`;
}

/** Parse dotted IPv4 to big-endian uint32, or null if invalid. */
function parseIPv4Uint32(s: string): number | null {
  const parts = s.trim().split('.');
  if (parts.length !== 4) return null;
  let n = 0;
  for (const p of parts) {
    if (!/^\d{1,3}$/.test(p)) return null;
    const o = Number(p);
    if (Number.isNaN(o) || o < 0 || o > 255) return null;
    n = (n << 8) | o;
  }
  return n >>> 0;
}

function isValidIPv4String(s: string): boolean {
  return parseIPv4Uint32(s) !== null;
}

/** Contiguous subnet mask in dotted form (e.g. 255.255.255.0). */
function isValidSubnetMaskString(s: string): boolean {
  const mask = parseIPv4Uint32(s);
  if (mask === null) return false;
  if (mask === 0) return false;
  const inv = ~mask >>> 0;
  return (inv & (inv + 1)) === 0;
}

type NetworkFormFields = {
  mode: 'dhcp' | 'static';
  ip_address: string;
  subnet_mask: string;
  gateway: string;
  dns1: string;
  dns2: string;
};

function validateNetworkForm(
  form: NetworkFormFields,
  t: TFunction
): string | null {
  const dns1 = form.dns1.trim();
  const dns2 = form.dns2.trim();
  if (dns1 && !isValidIPv4String(dns1)) {
    return t('sys.network.err_invalid_dns', 'Invalid DNS address');
  }
  if (dns2 && !isValidIPv4String(dns2)) {
    return t('sys.network.err_invalid_dns', 'Invalid DNS address');
  }
  if (form.mode !== 'static') return null;

  const ip = form.ip_address.trim();
  const mask = form.subnet_mask.trim();
  const gw = form.gateway.trim();

  if (!ip) return t('sys.network.err_required_ip', 'Please enter an IP address');
  if (!isValidIPv4String(ip)) return t('sys.network.err_invalid_ip', 'Invalid IP address');

  if (!mask) return t('sys.network.err_required_mask', 'Please enter a subnet mask');
  if (!isValidSubnetMaskString(mask)) {
    return t(
      'sys.network.err_invalid_mask',
      'Invalid subnet mask (use a contiguous mask such as 255.255.255.0)'
    );
  }

  if (!gw) {
 return t(
      'sys.network.err_required_gateway',
      'Please enter a default gateway'
    ); 
}
  if (!isValidIPv4String(gw)) return t('sys.network.err_invalid_gateway', 'Invalid gateway address');

  return null;
}

export default function NetworkPage() {
  const { t } = useTranslation();
  const queryClient = useQueryClient();

  const [showConfirmDialog, setShowConfirmDialog] = useState(false);
  const [showOverlay, setShowOverlay] = useState(false);
  const [newAddress, setNewAddress] = useState('');
  const [copied, setCopied] = useState(false);

  const { data, isLoading, error } = useQuery({
    queryKey: ['networkConfig'],
    queryFn: fetchNetworkConfig,
  });

  const [formData, setFormData] = useState({
    mode: 'dhcp' as 'dhcp' | 'static',
    ip_address: '',
    subnet_mask: '',
    gateway: '',
    dns1: '',
    dns2: '',
  });

  useEffect(() => {
    if (data) {
      setFormData({
        mode: data.mode || 'dhcp',
        ip_address: data.ip_address || '',
        subnet_mask: data.subnet_mask || '',
        gateway: data.gateway || '',
        dns1: data.dns1 || '',
        dns2: data.dns2 || '',
      });
    }
  }, [data]);

  const mutation = useMutation({
    mutationFn: updateNetworkConfig,
    onSuccess: response => {
      const newIp = response?.ip_address || formData.ip_address;
      setNewAddress(buildAccessUrl(newIp));
      setShowOverlay(true);
      queryClient.invalidateQueries({ queryKey: ['networkConfig'] });
    },
    onError: (err: Error) => {
      toast.error(err.message || t('common.error'));
    },
  });

  const handleCopy = useCallback(() => {
    navigator.clipboard.writeText(newAddress).then(() => {
      setCopied(true);
      setTimeout(() => setCopied(false), 2000);
    });
  }, [newAddress]);

  if (isLoading) {
    return <NetworkSkeleton />;
  }

  if (error || !data) {
    return (
      <div className="p-6">
        <h1 className="text-2xl font-bold mb-6 text-foreground">
          {t('common.network')}
        </h1>
        <div className="mx-auto flex h-64 max-w-xl items-center justify-center">
          <ErrorState />
        </div>
      </div>
    );
  }

  const handleSave = () => {
    const err = validateNetworkForm(formData, t);
    if (err) {
      toast.error(err);
      return;
    }
    setShowConfirmDialog(true);
  };

  const handleConfirmSave = () => {
    const err = validateNetworkForm(formData, t);
    if (err) {
      toast.error(err);
      setShowConfirmDialog(false);
      return;
    }
    setShowConfirmDialog(false);
    const payload: Partial<NetworkConfig> = {
      mode: formData.mode,
      ip_address: formData.ip_address.trim(),
      subnet_mask: formData.subnet_mask.trim(),
      gateway: formData.gateway.trim(),
      dns1: formData.dns1.trim(),
      dns2: formData.dns2.trim(),
    };
    mutation.mutate(payload);
  };

  const isStatic = formData.mode === 'static';
  const fieldsDisabled = !isStatic;

  // Preview the new address for the confirmation dialog
  const previewAddress = isStatic ? buildAccessUrl(formData.ip_address) : '';

  return (
    <>
      <div className="p-6 md:p-12 space-y-6 max-w-4xl mx-auto">
        <Card className="p-6">
          {/* Header */}
          <div className="mb-4 w-2xl:mb-8 flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between">
            <div className="flex items-center gap-2">
              <Network className="w-5 h-5" />
              <h2 className="text-lg font-bold text-foreground">
                {t('sys.network.title', 'IPv4 设置')}
              </h2>
            </div>
            {/* Interface info badge */}
            {data.interface && (
              <div className="flex w-full min-w-0 flex-wrap items-center gap-2 self-start rounded-lg bg-muted/50 px-3 py-1.5 sm:w-auto sm:self-auto">
                <Wifi className="w-4 h-4 text-muted-foreground" />
                <span className="text-sm font-medium">{data.interface}</span>
                {data.mac_address && (
                  <span className="text-xs text-muted-foreground font-mono">
                    ({data.mac_address})
                  </span>
                )}
              </div>
            )}
          </div>

          <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
            {/* Mode selector */}
            <div className="col-span-1 md:col-span-2 space-y-2">
              <Label>{t('sys.network.mode', 'Mode')}</Label>
              <Select
                value={formData.mode}
                onValueChange={(value: 'dhcp' | 'static') => setFormData({ ...formData, mode: value })}
              >
                <SelectTrigger className="w-full">
                  <SelectValue placeholder="Select mode" />
                </SelectTrigger>
                <SelectContent>
                  <SelectItem value="dhcp">
                    {t('sys.network.dhcp', 'Auto (DHCP)')}
                  </SelectItem>
                  <SelectItem value="static">
                    {t('sys.network.static', 'Static IP')}
                  </SelectItem>
                </SelectContent>
              </Select>
            </div>

            {/* IP Address */}
            <div className="space-y-2">
              <Label>{t('sys.network.ip_address', 'IP 地址')}</Label>
              <Input
                value={formData.ip_address}
                onChange={e => setFormData({ ...formData, ip_address: e.target.value })}
                disabled={fieldsDisabled}
                placeholder={
                  fieldsDisabled ? data.ip_address || '-' : '192.168.1.100'
                }
                className={fieldsDisabled ? 'bg-muted/50' : ''}
              />
            </div>

            {/* Subnet Mask */}
            <div className="space-y-2">
              <Label>{t('sys.network.subnet_mask', 'Subnet Mask')}</Label>
              <Input
                value={formData.subnet_mask}
                onChange={e => setFormData({ ...formData, subnet_mask: e.target.value })}
                disabled={fieldsDisabled}
                placeholder={
                  fieldsDisabled ? data.subnet_mask || '-' : '255.255.255.0'
                }
                className={fieldsDisabled ? 'bg-muted/50' : ''}
              />
            </div>

            {/* Gateway */}
            <div className="space-y-2">
              <Label>{t('sys.network.gateway', 'Gateway')}</Label>
              <Input
                value={formData.gateway}
                onChange={e => setFormData({ ...formData, gateway: e.target.value })}
                disabled={fieldsDisabled}
                placeholder={
                  fieldsDisabled ? data.gateway || '-' : '192.168.1.1'
                }
                className={fieldsDisabled ? 'bg-muted/50' : ''}
              />
            </div>

            {/* DNS Servers */}
            <div className="space-y-2">
              <Label>{t('sys.network.dns', 'DNS 服务器')}</Label>
              <Input
                value={formData.dns1}
                onChange={e => setFormData({ ...formData, dns1: e.target.value })}
                placeholder="8.8.8.8"
              />
            </div>

            {/* DNS2 */}
            <div className="space-y-2">
              <Label>{t('sys.network.secondary_dns', 'Secondary DNS')}</Label>
              <Input
                value={formData.dns2}
                onChange={e => setFormData({ ...formData, dns2: e.target.value })}
                placeholder="8.8.4.4"
              />
            </div>
          </div>

          {/* Actions */}
          <div className="flex justify-end gap-4 mt-8">
            <Button
              variant="carbon"
              onClick={handleSave}
              disabled={mutation.isPending}
            >
              <Save className="w-4 h-4 mr-2" />
              {mutation.isPending
                ? t('common.saving', 'Saving...')
                : t('sys.network.save', 'Save')}
            </Button>
          </div>
        </Card>
      </div>

      {/* Confirm Dialog */}
      <AlertDialog open={showConfirmDialog} onOpenChange={setShowConfirmDialog}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle className="flex items-center gap-2">
              <AlertTriangle className="w-5 h-5 text-[#f24a00]" />
              {t('sys.network.confirm_title')}
            </AlertDialogTitle>
            <AlertDialogDescription>
              {t('sys.network.confirm_desc')}
            </AlertDialogDescription>
          </AlertDialogHeader>
          {previewAddress && (
            <div className="mt-2 p-3 bg-muted/50 rounded-lg border border-border">
              <div className="text-xs text-muted-foreground mb-1">
                {t('sys.network.confirm_new_ip')}
              </div>
              <div className="text-sm font-mono font-semibold text-foreground break-all">
                {previewAddress}
              </div>
            </div>
          )}
          <AlertDialogFooter>
            <AlertDialogCancel>{t('common.cancel')}</AlertDialogCancel>
            <AlertDialogAction variant="carbon" onClick={handleConfirmSave}>
              {t('common.confirm')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>

      {/* Full-screen Overlay */}
      {showOverlay && (
        <div
          className="fixed inset-0 bg-black/80 flex items-center justify-center"
          style={{ zIndex: 99999 }}
        >
          <div className="bg-background border border-border rounded-xl shadow-2xl p-8 max-w-lg w-full mx-4 text-center">
            <div className="w-16 h-16 rounded-full bg-[#f24a00]/10 flex items-center justify-center mx-auto mb-6">
              <Network className="w-8 h-8 text-[#f24a00]" />
            </div>
            <h2 className="text-xl font-bold text-foreground mb-2">
              {t('sys.network.overlay_title')}
            </h2>
            <p className="text-sm text-muted-foreground mb-6">
              {t('sys.network.overlay_desc')}
            </p>

            <div className="bg-muted/50 border border-border rounded-lg p-4 mb-6">
              <div className="text-xs text-muted-foreground mb-2">
                {t('sys.network.overlay_new_address')}
              </div>
              <div className="text-lg font-mono font-semibold text-foreground break-all">
                {newAddress}
              </div>
            </div>

            <div className="flex gap-3 justify-center">
              <Button variant="outline" onClick={handleCopy} className="gap-2">
                {copied ? (
                  <>
                    <Check className="w-4 h-4" />
                    {t('sys.network.overlay_copied')}
                  </>
                ) : (
                  <>
                    <Copy className="w-4 h-4" />
                    {t('sys.network.overlay_copy')}
                  </>
                )}
              </Button>
              <Button
                variant="carbon"
                onClick={() => {
                  window.location.href = newAddress;
                }}
                className="gap-2"
              >
                <ExternalLink className="w-4 h-4" />
                {t('sys.network.overlay_goto')}
              </Button>
            </div>
          </div>
        </div>
      )}
    </>
  );
}
