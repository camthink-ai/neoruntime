import { useCallback, useState } from 'react';
import { useTranslation } from 'react-i18next';
import TopicList from './components/TopicList';
import PublishForm from './components/PublishForm';
import SubscriptionPanel from './components/SubscriptionPanel';
import EventStream from './components/EventStream';
import { ScrollArea } from '@/components/ui/scroll-area';

export default function Events() {
  const { t } = useTranslation();
  const [subscribedTopics, setSubscribedTopics] = useState<string[]>([]);

  const handleSubscribe = useCallback((topic: string) => {
    setSubscribedTopics(prev => (prev.includes(topic) ? prev : [...prev, topic]));
  }, []);

  const handleUnsubscribe = useCallback((topic: string) => {
    setSubscribedTopics(prev => prev.filter(topicItem => topicItem !== topic));
  }, []);

  return (
    <div className="h-full flex flex-col gap-4 p-4 overflow-auto">
      {/* Page header */}
      <div>
        <h1 className="text-lg font-semibold">{t('events.title')}</h1>
        <p className="text-sm text-muted-foreground">
          {t('events.description')}
        </p>
      </div>

      {/* Content: left sidebar + right stream */}
      <div className="flex-1 min-h-0 grid grid-cols-1 lg:grid-cols-[320px_1fr] gap-2">
        {/* Left column: topics + subscriptions + publish */}
        <ScrollArea className="flex flex-col gap-4 min-h-0 lg:overflow-auto">
          <div className="flex flex-col gap-4 min-h-0 lg:overflow-auto pr-3">
            <TopicList
              subscribedTopics={subscribedTopics}
              onSubscribe={handleSubscribe}
              onUnsubscribe={handleUnsubscribe}
            />
            <SubscriptionPanel
              subscribedTopics={subscribedTopics}
              onSubscribe={handleSubscribe}
              onUnsubscribe={handleUnsubscribe}
            />
            <PublishForm />
          </div>
        </ScrollArea>

        {/* Right column: live event stream */}
        <EventStream subscribedTopics={subscribedTopics} />
      </div>
    </div>
  );
}
