import { useTranslation } from 'react-i18next';
import PeripheralControl from './components/PeripheralControl';

export default function Peripherals() {
  const { t } = useTranslation();

  return (
    <div className="p-6 md:p-12 max-w-4xl w-full min-h-screen bg-background mx-auto">
      <h1 className="text-2xl font-bold tracking-tight text-foreground">
        {t('common.peripherals', '外设')}
      </h1>
      <div className="mt-6">
        <PeripheralControl />
      </div>
    </div>
  )
}
