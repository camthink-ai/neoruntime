import { useTranslation } from 'react-i18next';
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from '@/components/ui/table';
import { parseVStreamTable } from '../../lib/modelImportFlow';

/**
 * Raw-mode output tensor preview on the configure screen. The parse
 * endpoint's vstream_info is a HEF-info JSON string rather than proto
 * TensorSpecs, so this card shows the coarser but honest view the wizard
 * actually has: the output stream names, as parsed tolerantly. Hides itself
 * entirely when there is nothing parseable to show.
 */
export default function RawTensorPreview({
  vstreamInfo,
}: {
  vstreamInfo?: string | null;
}) {
  const { t } = useTranslation();
  const table = parseVStreamTable(vstreamInfo);
  if (!table) return null;

  return (
    <section className="space-y-2 min-w-0">
      <h4 className="text-xs font-semibold uppercase tracking-wide text-muted-foreground border-b border-border pb-2">
        {t('sys.ai_models.wizard.raw_tensor_preview', '输出张量预览')}
      </h4>
      <div className="overflow-x-auto rounded-lg border">
        <Table>
          <TableHeader>
            <TableRow>
              <TableHead className="h-8 w-10">#</TableHead>
              <TableHead className="h-8">
                {t(
                  'sys.ai_models.wizard.raw_tensor_preview_stream',
                  '输出流名称'
                )}
              </TableHead>
            </TableRow>
          </TableHeader>
          <TableBody>
            {table.vstreams.map((name, i) => (
              <TableRow key={name + i}>
                <TableCell className="text-xs text-muted-foreground">
                  {i + 1}
                </TableCell>
                <TableCell className="font-mono text-xs break-all">
                  {name}
                </TableCell>
              </TableRow>
            ))}
          </TableBody>
        </Table>
      </div>
      <p className="text-xs text-muted-foreground">
        {t(
          'sys.ai_models.wizard.raw_tensor_preview_note',
          '输出按字节回传，NMS 流语义 float32；由消费方自行解码。'
        )}
      </p>
    </section>
  );
}
