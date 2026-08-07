import { Card } from '@/components/ui/card';
import { useTranslation } from 'react-i18next';
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs';
import { FileText } from 'lucide-react';
import LogsDialog from '@/components/logs/LogsDialog';
import { useState } from 'react';
import { Button } from '@/components/ui/button';

export default function Maintenance() {
  const { t } = useTranslation();
  const [logsOpen, setLogsOpen] = useState(false);

  return (
    <div className="p-6">
      <h1 className="text-2xl font-bold mb-6 text-foreground">
        {t('common.maintenance')}
      </h1>

      <Tabs defaultValue="logs" className="w-full">
        <TabsList>
          <TabsTrigger value="logs">
            <FileText className="h-4 w-4 mr-2" />
            {t('common.logs')}
          </TabsTrigger>
        </TabsList>

        <TabsContent value="logs" className="mt-6">
          <Card className="p-6">
            <div className="flex items-center justify-between mb-4">
              <h2 className="text-lg font-semibold">
                {t('sys.maintenance.view_logs', 'View System Logs')}
              </h2>
              <Button onClick={() => setLogsOpen(true)}>
                {t('sys.maintenance.view_logs', 'View System Logs')}
              </Button>
            </div>
            <p className="text-sm text-muted-foreground">
              {t(
                'sys.maintenance.logs_description',
                'View system runtime logs including application and system logs'
              )}
            </p>
          </Card>
        </TabsContent>
      </Tabs>

      <LogsDialog open={logsOpen} onOpenChange={setLogsOpen} />
    </div>
  );
}
