import { useEffect, useState } from 'react';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { useTranslation } from 'react-i18next';
import { Card } from '@/components/ui/card';
import { Input } from '@/components/ui/input';
import { Button } from '@/components/ui/button';
import { Label } from '@/components/ui/label';
import { Switch } from '@/components/ui/switch';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import {
  applyCarrierProfile,
  fetchCellularConfig,
  fetchCellularStatus,
  updateCellularConfig,
  type CellularAuth,
  type CellularCarrierProfile,
  type CellularConfig,
  type CellularIpType,
  type CellularNetworkMode,
  type CellularStatus,
  type CellularUsbnet,
} from '@/services/settings';
import {
  Antenna,
  Building2,
  CardSim,
  CircuitBoard,
  Cpu,
  Globe,
  Hash,
  Loader2,
  Network,
  Phone,
  Power,
  Radio,
  Save,
} from 'lucide-react';
import { toast } from 'sonner';
import { cn } from '@/lib/utils';
import {
  DetailRow,
  Metric,
  SectionTitle,
  SignalBars,
  StatusChip,
  dash,
  processStatusText,
} from './cellularPanelUi';

export default function CellularPanel() {
  const { t } = useTranslation();
  const queryClient = useQueryClient();

  const [form, setForm] = useState<CellularConfig | null>(null);

  const statusQuery = useQuery({
    queryKey: ['cellularStatus'],
    queryFn: fetchCellularStatus,
    placeholderData: prev => prev,
    refetchInterval: () => (form?.enabled ? 2_000 : 10_000),
  });

  const configQuery = useQuery({
    queryKey: ['cellularConfig'],
    queryFn: fetchCellularConfig,
  });

  useEffect(() => {
    if (configQuery.data) setForm(configQuery.data);
  }, [configQuery.data]);

  const saveMutation = useMutation({
    mutationFn: updateCellularConfig,
    onSuccess: async saved => {
      setForm(saved);
      queryClient.setQueryData(['cellularConfig'], saved);
      await queryClient.refetchQueries({ queryKey: ['cellularStatus'] });
      toast.success(t('common.success'));
    },
    onError: async (err: Error) => {
      try {
        const cfg = await queryClient.fetchQuery({
          queryKey: ['cellularConfig'],
          queryFn: fetchCellularConfig,
        });
        setForm(cfg);
      } catch {
        if (configQuery.data) setForm({ ...configQuery.data, enabled: false });
        else setForm(prev => (prev ? { ...prev, enabled: false } : prev));
      }
      const msg = err.message || t('common.error');
      const noModule =        /no cellular module detected/i.test(msg)
        || /module not detected/i.test(msg);
      toast.error(
        noModule
          ? t(
              'sys.cellular.enable_no_module',
              'No cellular module detected. Cellular has been turned off.'
            )
          : msg
      );
      queryClient
        .refetchQueries({ queryKey: ['cellularStatus'] })
        .catch(() => undefined);
    },
  });

  useEffect(() => {
    if (!form || !configQuery.data) return;
    if (form.enabled !== configQuery.data.enabled && !saveMutation.isPending) {
      setForm(configQuery.data);
    }
  }, [configQuery.data, form, saveMutation.isPending]);

  useEffect(() => {
    if (!form?.enabled || saveMutation.isPending) return;
    const msg = statusQuery.data?.message || '';
    if (/cellular is disabled/i.test(msg)) {
      queryClient
        .invalidateQueries({ queryKey: ['cellularConfig'] })
        .catch(() => undefined);
    }
  }, [
    form?.enabled,
    saveMutation.isPending,
    statusQuery.data?.message,
    queryClient,
  ]);

  const applyEnableOptimisticStatus = (enabled: boolean) => {
    queryClient.setQueryData<CellularStatus>(['cellularStatus'], old => {
      if (!old) return old;
      const cleared = {
        ...old,
        connected: false,
        ipv4: '',
        ipv6: '',
        iface: '',
        sim_ready: false,
        operator: '',
        network: '',
        reg_state: 'unknown' as const,
        signal_rsrp: null,
        signal_rsrq: null,
        msisdn: '',
        iccid: '',
        imsi: '',
        imei: '',
        firmware: '',
        module: '',
        module_present: false,
      };
      if (!enabled) {
        return {
          ...cleared,
          message: 'Cellular is disabled.',
        };
      }
      return {
        ...cleared,
        message: 'Powering module…',
      };
    });
  };

  if (configQuery.isLoading || !form) {
    return (
      <Card className="p-6 flex items-center justify-center h-48 text-muted-foreground gap-2">
        <Loader2 className="w-4 h-4 animate-spin" />
        {t('common.loading')}
      </Card>
    );
  }

  const status = statusQuery.data;

  const enablingPending = saveMutation.isPending && form.enabled;
  const modulePresent = Boolean(status?.module_present);
  const moduleOk = (modulePresent || enablingPending) && form.enabled;
  const displayModel = modulePresent ? status?.module || '' : '';
  const simOk = Boolean(status?.sim_ready);
  const showAuthFields = form.auth !== 'none';
  const process = saveMutation.isPending
    ? {
        text: form.enabled
          ? t('sys.cellular.process_powering', 'Powering module…')
          : t('sys.cellular.process_powering_down', 'Powering down…'),
        busy: true,
      }
    : processStatusText(status, form.enabled, t);
  const onCarrierChange = (profile: CellularCarrierProfile) => {
    setForm(applyCarrierProfile(profile, form));
  };

  return (
    <Card className="p-6">
      <div className="mb-4 flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
        <div className="flex items-center gap-3 min-w-0">
          <div className="flex size-9 shrink-0 items-center justify-center rounded-lg bg-muted/60">
            <Antenna className="size-5 text-foreground" />
          </div>
          <h2 className="text-lg font-bold text-foreground leading-tight">
            {t('sys.cellular.title', 'Cellular')}
          </h2>
        </div>
        {form.enabled || saveMutation.isPending ? (
          <div className="flex min-w-0 flex-col gap-1.5 sm:items-end sm:max-w-[min(100%,28rem)]">
            <div className="flex min-w-0 flex-wrap items-center gap-1.5 sm:justify-end">
              <StatusChip
                ok={moduleOk}
                icon={CircuitBoard}
                label={
                  enablingPending && !modulePresent
                    ? t('sys.cellular.module_detecting', 'Detecting module…')
                    : modulePresent
                      ? t('sys.cellular.module_present', 'Module detected')
                      : t('sys.cellular.module_absent', 'No module')
                }
              />
              <StatusChip
                ok={simOk && form.enabled}
                icon={CardSim}
                label={
                  form.enabled && !simOk && (enablingPending || modulePresent)
                    ? t('sys.cellular.sim_waiting', 'Waiting for SIM…')
                    : simOk && form.enabled
                      ? t('sys.cellular.sim_ready', 'SIM ready')
                      : t('sys.cellular.sim_absent', 'SIM n/a')
                }
              />
              <StatusChip
                ok={Boolean(status?.connected) && form.enabled}
                icon={Globe}
                label={
                  status?.connected && form.enabled
                    ? t('sys.cellular.state_online', 'Online')
                    : t('sys.cellular.state_offline', 'Offline')
                }
              />
            </div>
            {process ? (
              <div
                className="flex items-start gap-1.5 text-xs leading-snug text-muted-foreground sm:text-right"
                title={process.text}
              >
                {process.busy ? (
                  <Loader2 className="mt-0.5 size-3.5 shrink-0 animate-spin" />
                ) : null}
                <span className="min-w-0">{process.text}</span>
              </div>
            ) : null}
          </div>
        ) : null}
      </div>

      <div className="flex items-center justify-between gap-4 rounded-lg border border-border px-4 py-3.5">
        <div className="flex min-w-0 items-center gap-3 pr-2">
          <div
            className={cn(
              'flex size-8 shrink-0 items-center justify-center rounded-md',
              form.enabled ? 'bg-emerald-500/10' : 'bg-muted/60'
            )}
          >
            <Power
              className={cn(
                'size-4',
                form.enabled
                  ? 'text-emerald-600 dark:text-emerald-400'
                  : 'text-muted-foreground'
              )}
            />
          </div>
          <div className="min-w-0">
            <div className="font-medium leading-tight">
              {t('sys.cellular.enable', 'Enable cellular')}
            </div>
            <p className="text-xs text-muted-foreground mt-0.5 leading-relaxed">
              {t(
                'sys.cellular.enable_hint',
                'Powers the module and auto-dials when it appears on USB.'
              )}
            </p>
          </div>
        </div>
        <Switch
          checked={form.enabled}
          disabled={saveMutation.isPending}
          onCheckedChange={enabled => {
            if (saveMutation.isPending) return;
            const next = { ...form, enabled };
            setForm(next);
            applyEnableOptimisticStatus(enabled);
            saveMutation.mutate(next);
          }}
        />
      </div>

      {form.enabled ? (
        <>
          <div className="mt-5 mb-6 grid grid-cols-1 gap-4 lg:grid-cols-2">
            <div className="rounded-xl border border-border p-4">
              <SectionTitle>
                {t('sys.cellular.section_session', 'Session')}
              </SectionTitle>
              <div className="grid grid-cols-2 gap-2.5">
                <Metric
                  label={t('sys.cellular.operator', 'Operator')}
                  icon={Building2}
                >
                  {dash(status?.operator)}
                </Metric>
                <Metric
                  label={t('sys.cellular.network', 'Network')}
                  icon={Radio}
                >
                  <span className="flex items-center gap-2">
                    <span>{dash(status?.network)}</span>
                    {status?.reg_state && status.reg_state !== 'unknown' ? (
                      <span className="text-[10px] font-normal uppercase tracking-wide text-muted-foreground">
                        {status.reg_state.replace(/_/g, ' ')}
                      </span>
                    ) : null}
                  </span>
                </Metric>
                <Metric label={t('sys.cellular.signal', 'Signal')}>
                  <div className="flex flex-col gap-1">
                    <div className="flex items-center gap-2">
                      <SignalBars rsrp={status?.signal_rsrp} />
                      <span className="tabular-nums whitespace-nowrap">
                        {status?.signal_rsrp != null
                          ? `${status.signal_rsrp} dBm`
                          : '—'}
                      </span>
                    </div>
                    {status?.signal_rsrq != null ? (
                      <span className="text-xs font-medium text-muted-foreground tabular-nums whitespace-nowrap">
                        RSRQ {status.signal_rsrq} dB
                      </span>
                    ) : null}
                  </div>
                </Metric>
                <Metric
                  label={t('sys.cellular.iface', 'Interface')}
                  icon={Network}
                >
                  {dash(status?.iface)}
                </Metric>
                <Metric label={t('sys.cellular.ipv4', 'IPv4')} icon={Globe}>
                  {dash(status?.ipv4)}
                </Metric>
                <Metric label={t('sys.cellular.ipv6', 'IPv6')} icon={Globe}>
                  {dash(status?.ipv6)}
                </Metric>
              </div>
            </div>

            <div className="rounded-xl border border-border p-4">
              <SectionTitle>
                {t('sys.cellular.section_sim', 'SIM & module')}
              </SectionTitle>
              <div className="space-y-0">
                <DetailRow
                  label={t('sys.cellular.model', 'Model')}
                  value={displayModel}
                  icon={CircuitBoard}
                />
                <DetailRow
                  label={t('sys.cellular.msisdn', 'Phone')}
                  value={status?.msisdn}
                  icon={Phone}
                />
                <DetailRow
                  label={t('sys.cellular.iccid', 'ICCID')}
                  value={status?.iccid}
                  icon={CardSim}
                />
                <DetailRow
                  label={t('sys.cellular.imsi', 'IMSI')}
                  value={status?.imsi}
                  icon={Hash}
                />
                <DetailRow
                  label={t('sys.cellular.imei', 'IMEI')}
                  value={status?.imei}
                  icon={CircuitBoard}
                />
                <DetailRow
                  label={t('sys.cellular.firmware', 'Firmware')}
                  value={status?.firmware}
                  icon={Cpu}
                />
              </div>
            </div>
          </div>

          <div className="rounded-xl border border-border p-4">
            <SectionTitle>
              {t('sys.cellular.section_dial', 'Dial settings')}
            </SectionTitle>
            <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
              <div className="space-y-2">
                <Label>{t('sys.cellular.usbnet', 'Protocol')}</Label>
                <Select
                  value={form.usbnet}
                  onValueChange={v => setForm({ ...form, usbnet: v as CellularUsbnet })}
                >
                  <SelectTrigger className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="mbim">
                      {t('sys.cellular.usbnet_mbim', 'MBIM')}
                    </SelectItem>
                    <SelectItem value="qmi">
                      {t('sys.cellular.usbnet_qmi', 'QMI')}
                    </SelectItem>
                    <SelectItem value="ecm">
                      {t('sys.cellular.usbnet_ecm', 'ECM')}
                    </SelectItem>
                  </SelectContent>
                </Select>
              </div>

              <div className="space-y-2">
                <Label>{t('sys.cellular.carrier', 'Carrier profile')}</Label>
                <Select
                  value={form.carrier_profile}
                  onValueChange={v => onCarrierChange(v as CellularCarrierProfile)}
                >
                  <SelectTrigger className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="generic">
                      {t('sys.cellular.carrier_generic', 'Generic')}
                    </SelectItem>
                    <SelectItem value="verizon">
                      {t('sys.cellular.carrier_verizon', 'Verizon (PDP #3)')}
                    </SelectItem>
                  </SelectContent>
                </Select>
              </div>

              <div className="space-y-2">
                <Label>{t('sys.cellular.apn', 'APN')}</Label>
                <Input
                  value={form.apn}
                  onChange={e => setForm({ ...form, apn: e.target.value })}
                  placeholder={
                    form.carrier_profile === 'verizon'
                      ? 'vzwinternet'
                      : 'internet'
                  }
                />
              </div>

              <div className="space-y-2">
                <Label>{t('sys.cellular.pdp_cid', 'PDP context (CID)')}</Label>
                <Input
                  type="number"
                  min={1}
                  max={16}
                  value={form.pdp_cid}
                  onChange={e => setForm({ ...form, pdp_cid: Number(e.target.value) || 1 })}
                />
              </div>

              <div className="space-y-2">
                <Label>{t('sys.cellular.ip_type', 'IP type')}</Label>
                <Select
                  value={form.ip_type}
                  onValueChange={v => setForm({ ...form, ip_type: v as CellularIpType })}
                >
                  <SelectTrigger className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="ipv4">IPv4</SelectItem>
                    <SelectItem value="ipv6">IPv6</SelectItem>
                    <SelectItem value="ipv4v6">IPv4v6</SelectItem>
                  </SelectContent>
                </Select>
              </div>

              <div className="space-y-2">
                <Label>{t('sys.cellular.auth', 'Authentication')}</Label>
                <Select
                  value={form.auth}
                  onValueChange={v => setForm({ ...form, auth: v as CellularAuth })}
                >
                  <SelectTrigger className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="none">
                      {t('sys.cellular.auth_none', 'None')}
                    </SelectItem>
                    <SelectItem value="pap">PAP</SelectItem>
                    <SelectItem value="chap">CHAP</SelectItem>
                  </SelectContent>
                </Select>
              </div>

              {showAuthFields && (
                <>
                  <div className="space-y-2">
                    <Label>{t('sys.cellular.user', 'Username')}</Label>
                    <Input
                      value={form.user}
                      onChange={e => setForm({ ...form, user: e.target.value })}
                    />
                  </div>
                  <div className="space-y-2">
                    <Label>{t('sys.cellular.password', 'Password')}</Label>
                    <Input
                      type="password"
                      value={form.password}
                      onChange={e => setForm({ ...form, password: e.target.value })}
                      autoComplete="new-password"
                    />
                  </div>
                </>
              )}

              <div className="space-y-2">
                <Label>
                  {t('sys.cellular.network_mode', 'Network mode')}
                </Label>
                <Select
                  value={form.network_mode || 'auto'}
                  onValueChange={v => setForm({
                      ...form,
                      network_mode: v as CellularNetworkMode,
                    })}
                >
                  <SelectTrigger className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="auto">
                      {t('sys.cellular.network_mode_auto', 'Auto')}
                    </SelectItem>
                    <SelectItem value="lte">
                      {t('sys.cellular.network_mode_lte', 'LTE only')}
                    </SelectItem>
                    <SelectItem value="wcdma">
                      {t(
                        'sys.cellular.network_mode_wcdma',
                        '3G (WCDMA) only'
                      )}
                    </SelectItem>
                  </SelectContent>
                </Select>
              </div>

              <div className="space-y-2">
                <Label>{t('sys.cellular.roaming', 'Data roaming')}</Label>
                <div className="flex h-9 items-center justify-between rounded-md border border-border px-3">
                  <span className="text-sm text-muted-foreground">
                    {form.roaming
                      ? t('sys.cellular.roaming_on', 'Allowed')
                      : t('sys.cellular.roaming_off', 'Blocked')}
                  </span>
                  <Switch
                    checked={Boolean(form.roaming)}
                    onCheckedChange={v => setForm({ ...form, roaming: v })}
                  />
                </div>
              </div>

              <div className="space-y-2">
                <Label>{t('sys.cellular.pin', 'SIM PIN')}</Label>
                <Input
                  type="password"
                  inputMode="numeric"
                  autoComplete="off"
                  placeholder={t(
                    'sys.cellular.pin_placeholder',
                    'Leave empty if unlocked'
                  )}
                  value={form.pin || ''}
                  onChange={e => setForm({
                      ...form,
                      pin: e.target.value.replace(/\D/g, '').slice(0, 8),
                    })}
                />
              </div>
            </div>

            <div className="mt-6 flex justify-end">
              <Button
                variant="carbon"
                className="gap-2"
                disabled={saveMutation.isPending}
                onClick={() => saveMutation.mutate(form)}
              >
                {saveMutation.isPending ? (
                  <Loader2 className="size-4 animate-spin" />
                ) : (
                  <Save className="size-4" />
                )}
                {saveMutation.isPending
                  ? t('common.saving', 'Saving...')
                  : t('sys.cellular.save', 'Save')}
              </Button>
            </div>
          </div>
        </>
      ) : null}
    </Card>
  );
}
