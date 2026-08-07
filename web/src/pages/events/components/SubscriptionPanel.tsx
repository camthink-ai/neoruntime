import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { BellRing, Plus, X } from 'lucide-react';
import { toast } from 'sonner';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Badge } from '@/components/ui/badge';
import { useSubscribeTopic, useUnsubscribeTopic } from '@/hooks/useEvents';

interface SubscriptionPanelProps {
  subscribedTopics: string[];
  onSubscribe: (topic: string) => void;
  onUnsubscribe: (topic: string) => void;
}

export default function SubscriptionPanel({
  subscribedTopics,
  onSubscribe,
  onUnsubscribe,
}: SubscriptionPanelProps) {
  const { t } = useTranslation();
  const [input, setInput] = useState('');
  const subscribeMutation = useSubscribeTopic();
  const unsubscribeMutation = useUnsubscribeTopic();

  const handleAdd = () => {
    const topic = input.trim();
    if (!topic || subscribedTopics.includes(topic)) return;

    subscribeMutation.mutate(topic, {
      onSuccess: () => {
        onSubscribe(topic);
        setInput('');
        toast.success(t('events.subscribe_success'));
      },
      onError: () => toast.error(t('events.subscribe_error')),
    });
  };

  const handleRemove = (topic: string) => {
    unsubscribeMutation.mutate(topic, {
      onSuccess: () => {
        onUnsubscribe(topic);
        toast.success(t('events.unsubscribe_success'));
      },
      onError: () => toast.error(t('events.unsubscribe_error')),
    });
  };

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter') handleAdd();
  };

  return (
    <div className="rounded-lg border bg-card p-4">
      <h3 className="text-sm font-semibold flex items-center gap-2 mb-3">
        <BellRing className="h-4 w-4" />
        {t('events.subscribed_topics')}
      </h3>

      {/* Add subscription input */}
      <div className="flex gap-1.5 mb-3">
        <Input
          value={input}
          onChange={e => setInput(e.target.value)}
          onKeyDown={handleKeyDown}
          placeholder={t('events.subscribe_topic_placeholder')}
          className="text-xs"
        />
        <Button
          variant="outline"
          size="sm"
          onClick={handleAdd}
          disabled={!input.trim() || subscribeMutation.isPending}
        >
          <Plus className="h-3.5 w-3.5" />
        </Button>
      </div>

      {/* Subscribed topics list */}
      {subscribedTopics.length === 0 ? (
        <p className="text-xs text-muted-foreground text-center py-2">
          {t('events.no_subscriptions')}
        </p>
      ) : (
        <div className="flex flex-wrap gap-1.5">
          {subscribedTopics.map(topic => (
            <Badge
              key={topic}
              variant="secondary"
              className="text-xs font-mono gap-1 pr-1"
            >
              {topic}
              <button
                type="button"
                onClick={() => handleRemove(topic)}
                disabled={unsubscribeMutation.isPending}
                className="ml-0.5 rounded-full hover:bg-foreground/10 p-0.5"
              >
                <X className="h-2.5 w-2.5" />
              </button>
            </Badge>
          ))}
        </div>
      )}
    </div>
  );
}
