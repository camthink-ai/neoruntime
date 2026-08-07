import { useState } from 'react';
import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import {
  KeyRound,
  Hammer,
  Power,
  Info,
  TerminalSquare,
  Upload,
  Wrench,
  Cpu,
  Clock,
  HardDrive,
  HardDriveUpload,
} from 'lucide-react';

import { Button } from '@/components/ui/button';
import { Card } from '@/components/ui/card';
import { fetchDeviceInfo, updateDeviceName } from '@/services/settings';
import ErrorState from '@/components/ErrorState';
import { useCriticalDeviceTransitionActive } from '@/utils/deviceTransition';

// Import decoupled components
import { InfoGridItem } from './components/InfoGridItem';
import { HardwareItem } from './components/HardwareItem';
import { PasswordDialog } from './components/PasswordDialog';
import { RebootDialog } from './components/RebootDialog';
import { FirmwareUpdateDialog } from './components/FirmwareUpdateDialog';
import { OsUpgradeDialog } from './components/OsUpgradeDialog';
import { DeviceInfoSkeleton } from './components/DeviceInfoSkeleton';

export default function DeviceInfo() {
  const { t } = useTranslation();
  const queryClient = useQueryClient();
  const unknown = t('common.unknown', 'Unknown');
  const deviceTransitionActive = useCriticalDeviceTransitionActive();

  // Dialog and Modal states
  const [showPasswordDialog, setShowPasswordDialog] = useState(false);
  const [showRebootDialog, setShowRebootDialog] = useState(false);
  const [showUpdateModal, setShowUpdateModal] = useState(false);
  const [showOsUpgradeModal, setShowOsUpgradeModal] = useState(false);

  // Edit Name states
  const [editingName, setEditingName] = useState(false);
  const [newDeviceName, setNewDeviceName] = useState('');

  // Query Device Info
  const { data, isLoading, error } = useQuery({
    queryKey: ['deviceInfo'],
    queryFn: fetchDeviceInfo,
    retry: !deviceTransitionActive,
    refetchOnWindowFocus: !deviceTransitionActive,
  });

  // Mutate Device Name
  const deviceNameMutation = useMutation({
    mutationFn: (name: string) => updateDeviceName(name),
    onSuccess: () => {
      toast.success(t('sys.device_info.name_updated', 'Device name updated'));
      setEditingName(false);
      queryClient.invalidateQueries({ queryKey: ['deviceInfo'] });
    },
    onError: (err: any) => {
      toast.error(
        t('sys.device_info.name_update_failed', 'Update failed: ')
          + (err?.message || err)
      );
    },
  });

  // Handlers for edit name
  const handleStartEdit = () => {
    if (data) {
      setNewDeviceName(data.device_name);
      setEditingName(true);
    }
  };

  const handleSaveName = () => {
    if (newDeviceName && newDeviceName !== data?.device_name) {
      deviceNameMutation.mutate(newDeviceName);
    } else {
      setEditingName(false);
    }
  };

  const handleCancelEdit = () => {
    setEditingName(false);
    setNewDeviceName('');
  };

  const dialogs = (
    <>
      <PasswordDialog
        open={showPasswordDialog}
        onOpenChange={setShowPasswordDialog}
      />
      <RebootDialog
        open={showRebootDialog}
        onOpenChange={setShowRebootDialog}
      />
      <FirmwareUpdateDialog
        open={showUpdateModal}
        onOpenChange={setShowUpdateModal}
      />
      <OsUpgradeDialog
        open={showOsUpgradeModal}
        onOpenChange={setShowOsUpgradeModal}
      />
    </>
  );

  if (!data && deviceTransitionActive) {
    return (
      <div className="p-6 md:p-12 w-full h-screen bg-background max-w-4xl mx-auto">
        {dialogs}
      </div>
    );
  }

  if (!data && isLoading) {
    return <DeviceInfoSkeleton />;
  }

  if (!data && error) {
    return (
      <div className="p-8 mx-auto w-full min-h-[calc(100vh-64px)] bg-background">
        <div className="mx-auto flex h-64 max-w-xl items-center justify-center">
          <ErrorState />
        </div>
      </div>
    );
  }

  if (!data) {
    return <DeviceInfoSkeleton />;
  }

  return (
    <div className="p-6 md:p-12  w-full h-screen bg-background max-w-4xl mx-auto">
      <div className=" space-y-10">
        {/* Section: 基本信息 */}
        <section className="space-y-4">
          <div className="flex items-center gap-2 text-foreground font-bold text-lg">
            <div className="w-5 h-5 rounded-full text-foreground flex items-center justify-center shrink-0 shadow-sm mt-0.5">
              <Info className="w-4.5 h-4.5" strokeWidth={3} />
            </div>
            {t('sys.device_info.basic_info', 'Basic Info')}
          </div>

          <Card className="overflow-hidden gap-0 p-0 shadow-sm">
            {/* Mobile: compact label | value table */}
            <div className="p-4 md:hidden">
              <table className="w-full border-separate border-spacing-y-3 text-sm [border-spacing:0_0.75rem]">
                <tbody>
                  <InfoGridItem
                    layout="table-row"
                    label={t('sys.device_info.name', 'Device Name')}
                    value={data.device_name}
                    isName
                    editing={editingName}
                    editValue={newDeviceName}
                    onEditChange={setNewDeviceName}
                    onStartEdit={handleStartEdit}
                    onSave={handleSaveName}
                    onCancel={handleCancelEdit}
                    isSaving={deviceNameMutation.isPending}
                  />
                  <InfoGridItem
                    layout="table-row"
                    label={t('sys.device_info.device_model', 'Model')}
                    value={data.model || unknown}
                  />
                  <InfoGridItem
                    layout="table-row"
                    label={t('sys.device_info.serial_number', 'Serial Number')}
                    value={data.serial_number || unknown}
                  />
                  <InfoGridItem
                    layout="table-row"
                    label={t('sys.device_info.mac_address', 'Mac地址')}
                    value={data.mac_address || unknown}
                  />
                  <InfoGridItem
                    layout="table-row"
                    label={t('sys.device_info.camera_module', 'Camera Module')}
                    value={data.camera_module?.model || unknown}
                  />
                  <InfoGridItem
                    layout="table-row"
                    label={t('sys.device_info.ip_address', 'IP 地址')}
                    value={data.ip_address || unknown}
                  />
                </tbody>
              </table>
            </div>

            {/* md+: grid cards */}
            <div className="hidden md:grid md:grid-cols-3 md:gap-x-6 md:gap-y-4 md:p-4 lg:p-6">
              <InfoGridItem
                label={t('sys.device_info.name', 'Device Name')}
                value={data.device_name}
                isName
                editing={editingName}
                editValue={newDeviceName}
                onEditChange={setNewDeviceName}
                onStartEdit={handleStartEdit}
                onSave={handleSaveName}
                onCancel={handleCancelEdit}
                isSaving={deviceNameMutation.isPending}
              />
              <InfoGridItem
                label={t('sys.device_info.device_model', 'Model')}
                value={data.model || unknown}
              />
              <InfoGridItem
                label={t('sys.device_info.serial_number', 'Serial Number')}
                value={data.serial_number || unknown}
              />
              <InfoGridItem
                label={t('sys.device_info.mac_address', 'Mac地址')}
                value={data.mac_address || unknown}
              />
              <InfoGridItem
                label={t('sys.device_info.camera_module', 'Camera Module')}
                value={data.camera_module?.model || unknown}
              />
              <InfoGridItem
                label={t('sys.device_info.ip_address', 'IP 地址')}
                value={data.ip_address || unknown}
              />
            </div>
          </Card>
        </section>

        {/* Section: 固件与硬件 */}
        <section className="space-y-4">
          <div className="flex items-center gap-2 text-foreground font-bold text-lg">
            <div className="text-foreground flex items-center justify-center shrink-0 mt-0.5">
              <Hammer className="w-4.5 h-4.5" strokeWidth={2.5} />
            </div>
            {t('sys.device_info.firmware_hardware', 'Firmware & Hardware')}
          </div>

          <Card className="py-2 flex flex-col overflow-hidden gap-0 shadow-sm">
            <HardwareItem
              icon={TerminalSquare}
              title={t('sys.device_info.firmware_version', 'Firmware Version')}
              desc={`${data.firmware_version || unknown} (${data.build_date ? `${t('sys.device_info.build', 'Build')} ${data.build_date}` : unknown})`}
              action={(
                <Button
                  variant="carbon"
                  onClick={() => setShowUpdateModal(true)}
                >
                  <HardDriveUpload className="mr-2 h-4 w-4" />
                  {t('sys.device_info.update', 'Update')}
                </Button>
              )}
            />
            <HardwareItem
              icon={Info}
              title={t('sys.device_info.os_version', 'System OS Version')}
              desc={`${data.os_version || unknown} (${data.os_build_time ? `${t('sys.device_info.build', 'Build')} ${data.os_build_time}` : unknown})`}
              action={(
                <Button
                  variant="carbon"
                  onClick={() => setShowOsUpgradeModal(true)}
                >
                  <HardDriveUpload className="mr-2 h-4 w-4" />
                  {t('sys.device_info.update', 'Update')}
                </Button>
              )}
            />
            <HardwareItem
              icon={Wrench}
              title={t('sys.device_info.hardware_version', 'Hardware Version')}
              desc={`${data.hardware_version || unknown}`}
            />
            <HardwareItem
              icon={Cpu}
              title={t('sys.device_info.cpu_model', 'Processor')}
              desc={`${data.cpu.cores} ${t('sys.device_info.cores', ' Cores')}${data.cpu.frequency_mhz ? ` @ ${data.cpu.frequency_mhz.toFixed(0)} MHz` : ''}`}
            />
            <HardwareItem
              icon={HardDrive}
              title={t('sys.device_info.memory', 'Memory')}
              desc={`${data.memory.used_gb.toFixed(1)} / ${data.memory.total_gb.toFixed(1)} GB (${data.memory.used_percent.toFixed(1)}%)`}
            />
            <HardwareItem
              icon={Clock}
              title={t('sys.device_info.runtime', 'Runtime Status')}
              desc={`${t('sys.device_info.uptime', 'Uptime')}: ${data.uptime_formatted || unknown}`}
            />
          </Card>
        </section>

        {/* Section: Bottom Actions — firmware Update only on mobile (md+ stays in HardwareItem) */}
        <div className="flex flex-col gap-4 pb-6 md:flex-row md:flex-wrap md:justify-end md:gap-4">
          <Button
            variant="carbon"
            className="flex h-11 w-full items-center gap-2 px-6 shadow-sm md:hidden"
            onClick={() => setShowUpdateModal(true)}
          >
            <Upload className="h-4 w-4" />
            <span className="font-semibold">
              {t('sys.device_info.update', 'Update')}
            </span>
          </Button>

          <Button
            variant="outline"
            className="flex h-11 w-full items-center gap-2 bg-card px-6 shadow-sm hover:bg-muted md:w-auto"
            onClick={() => setShowPasswordDialog(true)}
          >
            <KeyRound className="h-4.5 w-4.5 text-muted-foreground" />
            <span className="font-semibold text-foreground">
              {t('sys.device_info.change_password', 'Change Password')}
            </span>
          </Button>

          <Button
            variant="destructive"
            className="flex h-11 w-full items-center gap-2 px-6 shadow-sm md:w-auto"
            onClick={() => setShowRebootDialog(true)}
          >
            <Power className="h-4.5 w-4.5 text-white" />
            <span className="font-semibold text-white">
              {t('sys.device_info.system_reboot', 'System Reboot')}
            </span>
          </Button>
        </div>
      </div>

      {/* Decoupled Modals & Dialogs */}
      {dialogs}
    </div>
  );
}
