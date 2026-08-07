import { useTranslation } from 'react-i18next';
import { ChevronLeft, ChevronRight } from 'lucide-react';
import {
  Pagination as UiPagination,
  PaginationContent,
  PaginationEllipsis,
  PaginationItem,
  PaginationLink,
} from '@/components/ui/pagination';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';

interface PaginationProps {
  currentPage: number;
  pageSize: number;
  total: number;
  onPageChange: (page: number) => void;
  onPageSizeChange?: (pageSize: number) => void;
}

const DEFAULT_PAGE_SIZE_OPTIONS = [20, 50, 100, 200];

export default function Pagination({
  currentPage,
  pageSize,
  total,
  onPageChange,
  onPageSizeChange,
  pageSizeOptions = DEFAULT_PAGE_SIZE_OPTIONS,
}: PaginationProps & { pageSizeOptions?: number[] }) {
  const { t } = useTranslation();

  const totalPages = Math.max(1, Math.ceil(total / pageSize));
  const startItem = total === 0 ? 0 : (currentPage - 1) * pageSize + 1;
  const endItem = Math.min(currentPage * pageSize, total);

  const handlePageLinkClick =    (page: number) => (e: React.MouseEvent<HTMLAnchorElement>) => {
      e.preventDefault();
      if (page === currentPage) return;
      onPageChange(page);
    };

  const handlePrevClick = (e: React.MouseEvent<HTMLAnchorElement>) => {
    e.preventDefault();
    if (currentPage <= 1) return;
    onPageChange(currentPage - 1);
  };

  const handleNextClick = (e: React.MouseEvent<HTMLAnchorElement>) => {
    e.preventDefault();
    if (currentPage >= totalPages) return;
    onPageChange(currentPage + 1);
  };

  return (
    <div className="flex flex-col sm:flex-row sm:items-center sm:justify-between gap-3 px-4 py-3 md:px-6 border-t border-border/50 text-sm text-muted-foreground">
      <span className="text-xs sm:text-sm">
        {t(
          'sys.event_logs.pagination.showing',
          'Showing {{start}} to {{end}} of {{total}} items',
          {
            start: startItem,
            end: endItem,
            total,
          }
        )}
      </span>

      <div className="flex items-center justify-center sm:justify-end gap-1">
        <UiPagination className="mx-0 justify-end">
          <PaginationContent>
            <PaginationItem>
              <PaginationLink
                href="#"
                aria-label={t('common.prev', 'Previous')}
                size="icon-sm"
                className={
                  currentPage <= 1
                    ? 'pointer-events-none opacity-50'
                    : undefined
                }
                onClick={handlePrevClick}
              >
                <ChevronLeft className="h-4 w-4" />
              </PaginationLink>
            </PaginationItem>

            {totalPages > 7 ? (
              <>
                {Array.from(
                  { length: Math.min(3, totalPages) },
                  (_, i) => i + 1
                ).map(p => (
                  <PaginationItem key={p}>
                    <PaginationLink
                      href="#"
                      size="icon-sm"
                      isActive={p === currentPage}
                      className="h-7 w-7 text-xs"
                      onClick={handlePageLinkClick(p)}
                    >
                      {p}
                    </PaginationLink>
                  </PaginationItem>
                ))}

                {currentPage > 3 && currentPage < totalPages - 2 && (
                  <PaginationItem>
                    <PaginationEllipsis className="h-7 w-7" />
                  </PaginationItem>
                )}

                {currentPage > 3 && currentPage < totalPages - 2 && (
                  <PaginationItem>
                    <PaginationLink
                      href="#"
                      size="icon-sm"
                      isActive
                      className="h-7 w-7 text-xs"
                      onClick={handlePageLinkClick(currentPage)}
                    >
                      {currentPage}
                    </PaginationLink>
                  </PaginationItem>
                )}

                {totalPages > 3 && (
                  <>
                    {totalPages > 4 && (
                      <PaginationItem>
                        <PaginationEllipsis className="hidden sm:flex h-7 w-7" />
                      </PaginationItem>
                    )}
                    <PaginationItem>
                      <PaginationLink
                        href="#"
                        size="icon-sm"
                        isActive={totalPages === currentPage}
                        className="h-7 w-7 text-xs"
                        onClick={handlePageLinkClick(totalPages)}
                      >
                        {totalPages}
                      </PaginationLink>
                    </PaginationItem>
                  </>
                )}
              </>
            ) : (
              Array.from({ length: totalPages }, (_, i) => i + 1).map(p => (
                <PaginationItem key={p}>
                  <PaginationLink
                    href="#"
                    size="icon-sm"
                    isActive={p === currentPage}
                    className="h-7 w-7 text-xs"
                    onClick={handlePageLinkClick(p)}
                  >
                    {p}
                  </PaginationLink>
                </PaginationItem>
              ))
            )}

            <PaginationItem>
              <PaginationLink
                href="#"
                aria-label={t('common.next', 'Next')}
                size="icon-sm"
                className={
                  currentPage >= totalPages
                    ? 'pointer-events-none opacity-50'
                    : undefined
                }
                onClick={handleNextClick}
              >
                <ChevronRight className="h-4 w-4" />
              </PaginationLink>
            </PaginationItem>
          </PaginationContent>
        </UiPagination>

        {onPageSizeChange && (
          <Select
            value={String(pageSize)}
            onValueChange={value => onPageSizeChange(Number(value))}
          >
            <SelectTrigger className="ml-2 h-7 w-[100px] text-xs">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              {pageSizeOptions.map(size => (
                <SelectItem key={size} value={String(size)}>
                  {size}/{t('sys.event_logs.pagination.page', 'page')}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
        )}
      </div>
    </div>
  );
}
