import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import { ChevronRight, Brain } from 'lucide-react';
import { ScrollArea } from '@/components/ui/scroll-area';
import { Button } from '@/components/ui/button';
import { getModelTypeLabel } from '@/pages/ai-models/utils';

interface ModelsCardProps {
  stats: {
    total: number;
    loaded: number;
    list: any[];
  };
}

export default function ModelsCard({ stats }: ModelsCardProps) {
  const { t } = useTranslation();
  const navigate = useNavigate();

  const models = stats.list || [];

  return (
    <div className="bg-card rounded-2xl p-5 shadow-sm border border-border h-full max-lg:max-h-80 flex flex-col overflow-hidden">
      {/* Header */}
      <div className="flex items-center justify-between mb-4 shrink-0">
        <h3 className="text-base font-bold text-foreground flex items-center gap-2">
          <Brain className="w-4 h-4 text-primary" />
          {t('sys.ai_models.title', 'AI Models')}
        </h3>
        <Button
          variant="ghost"
          size="sm"
          className="text-xs text-muted-foreground hover:text-foreground h-7 px-2"
          onClick={() => navigate('/models')}
        >
          {t('common.view_all', '查看全部')}
          <ChevronRight className="w-3.5 h-3.5 ml-1" />
        </Button>
      </div>

      {/* Model list */}
      <ScrollArea className="flex-1 min-h-0">
        {models.length === 0 ? (
          <div className="flex flex-col items-center justify-center h-full py-8 text-muted-foreground">
            <Brain className="w-10 h-10 mb-3 opacity-50" />
            <p className="text-sm">
              {t('sys.ai_models.no_models', '暂无模型')}
            </p>
          </div>
        ) : (
          <div className="space-y-2 pr-2">
            {models.map((model: any, index: number) => {
              const isLoaded = !!model.load_timestamp;
              const modelType = getModelTypeLabel(
                model.model_type,
                model.model_id || model.id || '',
                t
              );

              return (
                <div
                  key={model.id || model.name || index}
                  className="flex items-center justify-between p-3 rounded-lg bg-secondary/30 hover:bg-secondary/50 transition-colors"
                >
                  <div className="flex items-center gap-3 flex-1 min-w-0">
                    {/* Status dot */}
                    <div
                      className={`w-2 h-2 rounded-full shrink-0 ${
                        isLoaded ? 'bg-purple-500' : 'bg-gray-300'
                      }`}
                    />

                    {/* Name + info */}
                    <div className="flex-1 min-w-0">
                      <div className="flex items-center gap-2">
                        <span className="text-sm font-medium text-foreground truncate">
                          {model.name || model.id}
                        </span>
                        <p className="text-xs text-muted-foreground mt-0.5">
                          {modelType}
                        </p>
                      </div>
                    </div>
                  </div>
                </div>
              );
            })}
          </div>
        )}
      </ScrollArea>
    </div>
  );
}
