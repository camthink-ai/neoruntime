import { useEffect, useRef, useState } from 'react';
import { Navigate } from 'react-router-dom';
import { ShieldAlert, HardDriveUpload, Globe, ChevronDown } from 'lucide-react';
import { useTranslation } from 'react-i18next';

import { useAppCompat } from '@/hooks';
import { changeLanguage } from '@/i18n/utils';
import { Button } from '@/components/ui/button';
import { FirmwareUpdateDialog } from '@/pages/settings/device-info/components/FirmwareUpdateDialog';

// Full-screen takeover for the post-OS-upgrade degraded state. Rendered
// outside the main layout on purpose: business navigation is held down by
// the server-side compat gates, so the only action offered here is firmware
// OTA (the /api/v1/system/ota route family served by the surviving
// platform-api), reusing the exact dialog from the device-info page. Visual
// language matches the in-dialog incompatibility warning (OsUpgradeDialog)
// and the login-page banner. Polls so an in-place app upgrade self-heals the
// session without a manual refresh.
export default function AppIncompatible() {
  const { t, i18n } = useTranslation();
  const { data: verdict } = useAppCompat({ refetchInterval: 15000 });
  const [showFirmware, setShowFirmware] = useState(false);
  const [langMenuOpen, setLangMenuOpen] = useState(false);
  const langMenuRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    if (!langMenuOpen) return;
    const handlePointerDown = (e: PointerEvent) => {
      if (!langMenuRef.current) return;
      if (!langMenuRef.current.contains(e.target as Node)) {
        setLangMenuOpen(false);
      }
    };
    window.addEventListener('pointerdown', handlePointerDown);
    return () => window.removeEventListener('pointerdown', handlePointerDown);
  }, [langMenuOpen]);

  const handleLanguageChange = (value: string) => {
    if (value !== i18n.language) {
      changeLanguage(value);
    }
  };

  // Verdict flipped (e.g. app upgraded in place): restore normal routing.
  if (verdict && verdict.valid !== false) {
    return <Navigate to="/dashboard" replace />;
  }

  const rows: Array<{ label: string; value: string }> = [];
  if (verdict?.app_version)
    rows.push({
      label: t('sys.compatibility.app_version_label', '应用版本'),
      value: verdict.app_version,
    });
  if (verdict?.app_min_os_version)
    rows.push({
      label: t('sys.compatibility.supported_os_label', '支持 OS 范围'),
      value: `${verdict.app_min_os_version} – ${verdict.app_max_os_version ?? '-'}`,
    });
  if (verdict?.os_version)
    rows.push({
      label: t('sys.compatibility.current_os_label', '当前设备 OS'),
      value: verdict.os_version,
    });

  return (
    <div className="relative flex min-h-dvh items-center justify-center bg-background px-4 py-10">
      {/* Language control — same trigger/menu pattern and language channel
          as the login page (changeLanguage persists via i18nextLng, so the
          choice carries into the rescued session). */}
      <div className="absolute top-6 right-6 z-10 flex items-center gap-2">
        <div className="relative" ref={langMenuRef}>
          <button
            type="button"
            onClick={() => setLangMenuOpen(v => !v)}
            className="flex h-9 items-center justify-between gap-2 rounded-md border bg-background/70 px-3 text-sm shadow-xs outline-none backdrop-blur transition-[color,box-shadow] hover:bg-background/90 focus-visible:ring-[3px] focus-visible:ring-primary/20"
            aria-haspopup="menu"
            aria-expanded={langMenuOpen}
          >
            <Globe className="h-4 w-4 text-muted-foreground" />
            <span className="text-sm font-medium text-foreground">
              {i18n.language === 'zh'
                ? t('common.language_zh', '简体中文')
                : 'English'}
            </span>
            <ChevronDown className="h-4 w-4 opacity-50" />
          </button>

          {langMenuOpen && (
            <div
              role="menu"
              className="absolute right-0 z-50 mt-2 w-36 overflow-hidden rounded-md border bg-popover text-popover-foreground shadow-md"
            >
              <button
                type="button"
                role="menuitem"
                className={`flex w-full cursor-default items-center rounded-sm py-2 pr-8 pl-2 text-left text-sm outline-none hover:bg-accent ${
                  i18n.language === 'zh'
                    ? 'bg-accent font-medium'
                    : 'text-foreground'
                }`}
                onClick={() => {
                  handleLanguageChange('zh');
                  setLangMenuOpen(false);
                }}
              >
                {t('common.language_zh', '简体中文')}
              </button>
              <button
                type="button"
                role="menuitem"
                className={`flex w-full cursor-default items-center rounded-sm py-1.5 pr-8 pl-2 text-left text-sm outline-none hover:bg-accent ${
                  i18n.language === 'en'
                    ? 'bg-accent font-medium'
                    : 'text-foreground'
                }`}
                onClick={() => {
                  handleLanguageChange('en');
                  setLangMenuOpen(false);
                }}
              >
                English
              </button>
            </div>
          )}
        </div>
      </div>
      <div className="w-full max-w-md space-y-6">
        <div className="flex items-start space-x-2 rounded-md border border-amber-500/30 bg-amber-500/5 p-3">
          <ShieldAlert className="w-5 h-5 text-amber-500 shrink-0 mt-0.5" />
          <div className="flex-1 space-y-1">
            <span className="block text-sm font-medium leading-tight">
              {t(
                'sys.compatibility.banner_title',
                '当前应用与设备 OS 不兼容，业务功能已停用'
              )}
            </span>
            <span className="block text-xs text-muted-foreground">
              {t('sys.compatibility.banner_desc', {
                defaultValue:
                  '应用 {{app}} 支持的 OS 范围为 {{min}}–{{max}}，当前设备 OS 为 {{os}}',
                app: verdict?.app_version || '-',
                min: verdict?.app_min_os_version || '-',
                max: verdict?.app_max_os_version || '-',
                os: verdict?.os_version || '-',
              })}
            </span>
          </div>
        </div>

        {rows.length > 0 && (
          <dl className="space-y-2 rounded-md border px-4 py-3 text-sm">
            {rows.map(row => (
              <div
                key={row.label}
                className="flex items-center justify-between gap-4"
              >
                <dt className="text-muted-foreground">{row.label}</dt>
                <dd className="font-medium text-foreground">{row.value}</dd>
              </div>
            ))}
          </dl>
        )}

        {verdict?.message && (
          <p className="text-xs break-words text-muted-foreground">
            {verdict.message}
          </p>
        )}

        <Button
          variant="carbon"
          className="h-11 w-full"
          onClick={() => setShowFirmware(true)}
        >
          <HardDriveUpload className="mr-2 h-4 w-4" />
          {t('sys.compatibility.firmware_upgrade_action', '固件升级')}
        </Button>
      </div>

      {/* Rescue dialog, identical to the device-info page's */}
      <FirmwareUpdateDialog
        open={showFirmware}
        onOpenChange={setShowFirmware}
      />
    </div>
  );
}
