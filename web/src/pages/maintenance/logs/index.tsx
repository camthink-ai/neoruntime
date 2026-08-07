import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { Tabs, TabsList, TabsTrigger } from '@/components/ui/tabs';
import OperationLogs from './OperationLogs';
import SystemLogs from './SystemLogs';
import DebugLogExport from './DebugLogExport';

type LogViewMode = 'operation' | 'system' | 'debug-logs';

export default function Logs() {
  const { t } = useTranslation();
  const [viewMode, setViewMode] = useState<LogViewMode>('operation');

  return (
    <div className="flex flex-col h-full p-4 md:p-6">
      {/* Tab Header */}
      <div className="mb-4">
        <Tabs
          value={viewMode}
          onValueChange={value => setViewMode(value as LogViewMode)}
          className="w-full"
        >
          <TabsList className="grid w-full max-w-md grid-cols-3">
            <TabsTrigger value="operation">
              {t('sys.maintenance.operation_logs', '操作日志')}
            </TabsTrigger>
            <TabsTrigger value="system">
              {t('sys.maintenance.system_logs', '系统日志')}
            </TabsTrigger>
            <TabsTrigger value="debug-logs">
              {t('sys.maintenance.debug_logs', '开发日志')}
            </TabsTrigger>
          </TabsList>
        </Tabs>
      </div>

      {/* Content Card */}
      <div className="flex flex-1 min-h-0 flex-col bg-white dark:bg-card rounded-xl shadow-[0_2px_12px_rgba(0,0,0,0.04)] border border-border/40 overflow-hidden">
        {viewMode === 'operation' && <OperationLogs />}
        {viewMode === 'system' && <SystemLogs />}
        {viewMode === 'debug-logs' && <DebugLogExport />}
      </div>
    </div>
  );
}
