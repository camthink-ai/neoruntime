import { useDeviceStatus, useLensStatus } from '@/hooks/useDeviceControl';
import LensControl from './LensControl';
import LightingControl from './LightingControl';
import {
  LensControlSkeleton,
  LightingControlSkeleton,
} from './DeviceControlSkeletons';

interface ImagingControlsProps {
  hasMcu?: boolean;
  hasLed?: boolean;
}

// Lens + lighting share the "Control" tab. Their underlying queries (lens
// status / device status) resolve at different times, so each card would
// otherwise flip skeleton→content independently and the tab would feel
// janky. Gate both behind a single loading flag: both skeletons together,
// then both real cards reveal together.
export default function ImagingControls({
  hasMcu,
  hasLed,
}: ImagingControlsProps) {
  const lens = useLensStatus();
  const device = useDeviceStatus();
  const isLoading = lens.isLoading || device.isLoading;

  if (isLoading) {
    return (
      <div className="space-y-6">
        {hasMcu && <LensControlSkeleton />}
        {hasLed && <LightingControlSkeleton />}
      </div>
    );
  }

  return (
    <div className="space-y-6">
      {hasMcu && <LensControl />}
      {hasLed && <LightingControl />}
    </div>
  );
}
