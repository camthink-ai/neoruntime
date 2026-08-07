import { useTranslation } from 'react-i18next';
import { RefreshCw, Radio, BellPlus, BellOff } from 'lucide-react';
import { toast } from 'sonner';
import { Button } from '@/components/ui/button';
import {
  useEventTopics,
  useSubscribeTopic,
  useUnsubscribeTopic,
} from '@/hooks/useEvents';

interface TopicListProps {
  subscribedTopics: string[];
  onSubscribe: (topic: string) => void;
  onUnsubscribe: (topic: string) => void;
}

export default function TopicList({
  subscribedTopics,
  onSubscribe,
  onUnsubscribe,
}: TopicListProps) {
  const { t } = useTranslation();
  const { data: topics, isLoading, refetch } = useEventTopics();
  const subscribeMutation = useSubscribeTopic();
  const unsubscribeMutation = useUnsubscribeTopic();

  const handleToggle = (topicName: string) => {
    const isSubscribed = subscribedTopics.includes(topicName);
    if (isSubscribed) {
      unsubscribeMutation.mutate(topicName, {
        onSuccess: () => {
          onUnsubscribe(topicName);
          toast.success(t('events.unsubscribe_success'));
        },
        onError: () => toast.error(t('events.unsubscribe_error')),
      });
    } else {
      subscribeMutation.mutate(topicName, {
        onSuccess: () => {
          onSubscribe(topicName);
          toast.success(t('events.subscribe_success'));
        },
        onError: () => toast.error(t('events.subscribe_error')),
      });
    }
  };

  return (
    <div className="rounded-lg border bg-card p-4">
      <div className="flex items-center justify-between mb-3">
        <h3 className="text-sm font-semibold flex items-center gap-2">
          <Radio className="h-4 w-4" />
          {t('events.topics')}
        </h3>
        <Button
          variant="ghost"
          size="icon-sm"
          onClick={() => refetch()}
          disabled={isLoading}
        >
          <RefreshCw
            className={`h-3.5 w-3.5 ${isLoading ? 'animate-spin' : ''}`}
          />
        </Button>
      </div>

      {!topics?.length ? (
        <p className="text-xs text-muted-foreground py-4 text-center">
          {t('events.no_topics')}
        </p>
      ) : (
        <div className="space-y-1">
          {topics.map(topic => {
            const isSubscribed = subscribedTopics.includes(topic.name);
            return (
              <div
                key={topic.name}
                className="flex items-center justify-between rounded-md px-3 py-2 text-sm hover:bg-accent/50 transition-colors"
              >
                <div className="flex-1 min-w-0 mr-2">
                  <code className="text-xs font-mono block truncate">
                    {topic.name}
                  </code>
                  <span className="text-[10px] text-muted-foreground">
                    {topic.subscriber_count} {t('events.subscribers')}
                  </span>
                </div>
                <Button
                  variant={isSubscribed ? 'default' : 'ghost'}
                  size="icon-sm"
                  onClick={() => handleToggle(topic.name)}
                  disabled={
                    subscribeMutation.isPending || unsubscribeMutation.isPending
                  }
                  title={
                    isSubscribed
                      ? t('events.unsubscribe')
                      : t('events.subscribe')
                  }
                >
                  {isSubscribed ? (
                    <BellOff className="h-3.5 w-3.5" />
                  ) : (
                    <BellPlus className="h-3.5 w-3.5" />
                  )}
                </Button>
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}
