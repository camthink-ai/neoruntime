import { type ReactNode } from 'react';
import { useTranslation } from 'react-i18next';

export interface SectionNavItem {
  id: string;
  label: string;
}

export interface SectionNavProps {
  sections: SectionNavItem[];
  activeId: string | undefined;
  /**
   * Called when a page is selected. The dialog owns the active page so
   * install validation can jump to one.
   */
  onActiveChange: (id: string) => void;
  /**
   * Gate run before a click switches pages. Return false to cancel the
   * switch — used to flush pending YAML edits first and keep the user on
   * the YAML view when the server rejects the text.
   */
  onBeforeNavigate?: () => boolean | Promise<boolean>;
  /** Source badge / manifest-edit status / install button block. */
  header?: ReactNode;
}

/**
 * Left pagination nav of the import form: each entry is one page —
 * clicking switches the right pane to that section outright (the pane
 * never scrolls between sections). Desktop: a w-44 aside with the header
 * block on top and page buttons below. Mobile (max-sm): the aside
 * collapses into a horizontal chips row under the header block.
 */
export default function SectionNav({
  sections,
  activeId,
  onActiveChange,
  onBeforeNavigate,
  header,
}: SectionNavProps) {
  const { t } = useTranslation();

  const goTo = async (id: string) => {
    if (onBeforeNavigate) {
      const ok = await onBeforeNavigate();
      if (!ok) return;
    }
    onActiveChange(id);
  };

  return (
    <>
      {/* Desktop aside */}
      <nav
        aria-label={t('sys.apps.import.nav_sections_aria', '配置分区导航')}
        className="hidden w-44 shrink-0 flex-col border-r py-4 pl-4 pr-3 sm:flex"
      >
        {header && <div className="mb-4 space-y-3">{header}</div>}
        <ul className="space-y-1">
          {sections.map(s => (
            <li key={s.id}>
              <button
                type="button"
                onClick={() => goTo(s.id)}
                aria-current={activeId === s.id ? 'true' : undefined}
                className={`w-full rounded-md px-3 py-2 text-left text-sm transition-colors ${
                  activeId === s.id
                    ? 'bg-muted font-medium text-foreground'
                    : 'text-muted-foreground hover:bg-muted/50 hover:text-foreground'
                }`}
              >
                {s.label}
              </button>
            </li>
          ))}
        </ul>
      </nav>

      {/* Mobile chips fallback */}
      <div className="px-4 pb-1 pt-2 sm:hidden">
        {header && <div className="mb-3 space-y-3">{header}</div>}
        <div className="flex gap-2 overflow-x-auto pb-2">
          {sections.map(s => (
            <button
              key={s.id}
              type="button"
              onClick={() => goTo(s.id)}
              aria-current={activeId === s.id ? 'true' : undefined}
              className={`shrink-0 rounded-full border px-3 py-1.5 text-sm transition-colors ${
                activeId === s.id
                  ? 'border-[#f24a00] bg-[#fff5f0] font-medium text-[#f24a00]'
                  : 'border-border text-muted-foreground hover:text-foreground'
              }`}
            >
              {s.label}
            </button>
          ))}
        </div>
      </div>
    </>
  );
}
