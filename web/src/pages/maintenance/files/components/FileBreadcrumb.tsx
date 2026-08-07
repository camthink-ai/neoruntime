import { Home, ChevronRight, MoreHorizontal } from 'lucide-react';
import { Button } from '@/components/ui/button';
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from '@/components/ui/dropdown-menu';

interface FileBreadcrumbProps {
  currentPath: string;
  onNavigate: (path: string) => void;
  rootPath: string;
}

export function FileBreadcrumb({
  currentPath,
  onNavigate,
  rootPath,
}: FileBreadcrumbProps) {
  // Split path into segments
  const segments = currentPath.replace(rootPath, '').split('/').filter(Boolean);

  // Generate full path for each segment
  const getFullPath = (index: number) => `${rootPath}/${segments.slice(0, index + 1).join('/')}`;

  // Responsive logic: show fewer segments when path is long
  const getTotalSegments = () => segments.length;
  const calculateVisibleCount = () => {
    const total = getTotalSegments();
    if (total <= 2) return total;
    if (total <= 4) return 2;
    return 1; // Only show last segment when path is very long
  };

  // Calculate max width for each button based on total segments
  const calculateButtonMaxWidth = () => {
    const total = getTotalSegments();
    if (total <= 2) return '150px';
    if (total <= 4) return '100px';
    return '80px';
  };

  const MAX_VISIBLE = calculateVisibleCount();
  const showDropdown = segments.length > MAX_VISIBLE;
  const buttonMaxWidth = calculateButtonMaxWidth();

  const visibleSegments = showDropdown
    ? segments.slice(-MAX_VISIBLE)
    : segments;

  const hiddenSegments = showDropdown ? segments.slice(0, -MAX_VISIBLE) : [];

  return (
    <div className="flex items-center gap-1 text-sm min-w-0">
      <Button
        variant="ghost"
        size="icon-sm"
        className="h-6 w-6 shrink-0"
        onClick={() => onNavigate(rootPath)}
        title={rootPath}
      >
        <Home className="h-4 w-4" />
      </Button>

      {showDropdown && hiddenSegments.length > 0 && (
        <>
          <ChevronRight className="h-4 w-4 text-muted-foreground shrink-0" />
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button
                variant="ghost"
                size="icon-sm"
                className="h-6 w-6 shrink-0"
              >
                <MoreHorizontal className="h-4 w-4" />
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent
              align="start"
              className="max-h-60 overflow-y-auto"
            >
              {hiddenSegments.map((segment, index) => (
                <DropdownMenuItem
                  key={index}
                  onClick={() => onNavigate(getFullPath(index))}
                  className="max-w-[200px]"
                >
                  <span className="truncate">{segment}</span>
                </DropdownMenuItem>
              ))}
            </DropdownMenuContent>
          </DropdownMenu>
        </>
      )}

      {visibleSegments.map((segment, index) => {
        const actualIndex = showDropdown
          ? hiddenSegments.length + index
          : index;
        const fullPath = getFullPath(actualIndex);
        const isLast = actualIndex === segments.length - 1;

        return (
          <div key={actualIndex} className="flex items-center gap-1 min-w-0">
            <ChevronRight className="h-4 w-4 text-muted-foreground shrink-0" />
            <Button
              variant="ghost"
              // size="icon-sm"
              className={`h-6 px-2 ${isLast ? 'font-medium' : ''}`}
              style={{ maxWidth: buttonMaxWidth }}
              onClick={() => onNavigate(fullPath)}
            >
              <span className="truncate block" title={segment}>
                {segment}
              </span>
            </Button>
          </div>
        );
      })}
    </div>
  );
}
