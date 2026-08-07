import { motion } from 'motion/react';

interface InfoTableProps {
  data: Array<{
    label: string;
    value: string | number;
  }>;
}

export default function InfoTable({ data }: InfoTableProps) {
  return (
    <div className="border border-border rounded-lg overflow-hidden">
      <table className="w-full">
        <tbody>
          {data.map((item, index) => (
            <motion.tr
              key={item.label}
              initial={{ opacity: 0, y: 10 }}
              animate={{ opacity: 1, y: 0 }}
              transition={{ duration: 0.2, delay: index * 0.05 }}
              className="border-b last:border-b-0 hover:bg-muted/50 transition-colors"
            >
              <td className="px-6 py-4 text-sm text-muted-foreground w-1/3">
                {item.label}
              </td>
              <td className="px-6 py-4 text-sm font-medium text-foreground">
                {item.value}
              </td>
            </motion.tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
