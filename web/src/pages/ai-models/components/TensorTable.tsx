import { useTranslation } from 'react-i18next';
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from '@/components/ui/table';

/**
 * Tensor specs (proto TensorSpec JSON) from the model detail API:
 * {name, dtype, layout, shape[], byte_size}. Typed loosely on purpose —
 * the dialog merges DB-row and runtime views with different strictness.
 */
interface TensorSpecView {
  name?: unknown;
  dtype?: unknown;
  layout?: unknown;
  shape?: unknown;
  byte_size?: unknown;
}

const asText = (value: unknown): string => (typeof value === 'string' || typeof value === 'number'
    ? String(value)
    : '');

// Proto JSON emits 64-bit integers as strings; accept both spellings.
const asByteSize = (value: unknown): number | null => {
  const n    = typeof value === 'number'
      ? value
      : typeof value === 'string'
        ? Number(value)
        : NaN;
  return Number.isFinite(n) && n >= 0 ? n : null;
};

const formatByteSize = (bytes: number): string => {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
};

const normalizeSpecs = (io: unknown): TensorSpecView[] => (Array.isArray(io)
    ? io.filter(
        (spec): spec is TensorSpecView => !!spec && typeof spec === 'object'
      )
    : []);

const specShape = (spec: TensorSpecView): string => {
  if (!Array.isArray(spec.shape)) return '';
  return spec.shape.map(asText).filter(Boolean).join(' × ');
};

/**
 * Full per-tensor I/O tables for the model detail dialog. Each non-empty
 * direction renders its own section-style header plus a five-column table;
 * renders nothing when the model has no tensor specs (e.g. a runtime that
 * predates ListModels tensor reporting).
 */
export default function TensorTable({
  inputs,
  outputs,
}: {
  inputs?: unknown;
  outputs?: unknown;
}) {
  const { t } = useTranslation();

  const groups = [
    {
      key: 'tensor_inputs',
      fallback: '输入张量',
      specs: normalizeSpecs(inputs),
    },
    {
      key: 'tensor_outputs',
      fallback: '输出张量',
      specs: normalizeSpecs(outputs),
    },
  ].filter(group => group.specs.length > 0);

  if (groups.length === 0) return null;

  return (
    <>
      {groups.map(group => (
        <section key={group.key} className="space-y-2 min-w-0">
          <h4 className="text-xs font-semibold uppercase tracking-wide text-muted-foreground border-b border-border pb-2">
            {t(`sys.ai_models.detail.${group.key}`, group.fallback)}
          </h4>
          <div className="overflow-x-auto rounded-lg border">
            <Table>
              <TableHeader>
                <TableRow>
                  <TableHead className="h-8">
                    {t('sys.ai_models.detail.tensor_name', '名称')}
                  </TableHead>
                  <TableHead className="h-8">
                    {t('sys.ai_models.detail.tensor_dtype', '数据类型')}
                  </TableHead>
                  <TableHead className="h-8">
                    {t('sys.ai_models.detail.tensor_shape', '形状')}
                  </TableHead>
                  <TableHead className="h-8">
                    {t('sys.ai_models.detail.tensor_layout', '布局')}
                  </TableHead>
                  <TableHead className="h-8">
                    {t('sys.ai_models.detail.tensor_bytes', '缓冲大小')}
                  </TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {group.specs.map((spec, i) => {
                  const bytes = asByteSize(spec.byte_size);
                  return (
                    <TableRow key={asText(spec.name) || i}>
                      <TableCell className="font-mono text-xs break-all">
                        {asText(spec.name) || '-'}
                      </TableCell>
                      <TableCell className="text-xs">
                        {asText(spec.dtype) || '-'}
                      </TableCell>
                      <TableCell className="font-mono text-xs whitespace-nowrap">
                        {specShape(spec) || '-'}
                      </TableCell>
                      <TableCell className="font-mono text-xs">
                        {asText(spec.layout) || '-'}
                      </TableCell>
                      <TableCell className="text-xs whitespace-nowrap">
                        {bytes != null ? formatByteSize(bytes) : '-'}
                      </TableCell>
                    </TableRow>
                  );
                })}
              </TableBody>
            </Table>
          </div>
        </section>
      ))}
    </>
  );
}
