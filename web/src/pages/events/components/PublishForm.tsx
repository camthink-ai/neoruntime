import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { Send } from 'lucide-react';
import { toast } from 'sonner';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Textarea } from '@/components/ui/textarea';
import { usePublishEvent } from '@/hooks/useEvents';

export default function PublishForm() {
  const { t } = useTranslation();
  const publishEvent = usePublishEvent();
  const [topic, setTopic] = useState('');
  const [payload, setPayload] = useState('');

  const handlePublish = () => {
    if (!topic.trim()) return;

    let parsedPayload: Record<string, unknown> | undefined;
    if (payload.trim()) {
      try {
        parsedPayload = JSON.parse(payload);
      } catch {
        toast.error(t('events.invalid_json'));
        return;
      }
    }

    publishEvent.mutate(
      { topic: topic.trim(), payload: parsedPayload },
      {
        onSuccess: () => {
          toast.success(t('events.publish_success'));
          setTopic('');
          setPayload('');
        },
        onError: () => {
          toast.error(t('events.publish_error'));
        },
      }
    );
  };

  return (
    <div className="rounded-lg border bg-card p-4">
      <h3 className="text-sm font-semibold flex items-center gap-2 mb-3">
        <Send className="h-4 w-4" />
        {t('events.publish')}
      </h3>

      <div className="space-y-3">
        <div>
          <label className="text-xs text-muted-foreground mb-1 block">
            {t('events.publish_topic')}
          </label>
          <Input
            value={topic}
            onChange={e => setTopic(e.target.value)}
            placeholder={t('events.publish_topic_placeholder')}
            className="text-sm"
          />
        </div>

        <div>
          <label className="text-xs text-muted-foreground mb-1 block">
            {t('events.publish_payload')}
          </label>
          <Textarea
            value={payload}
            onChange={e => setPayload(e.target.value)}
            placeholder={t('events.publish_payload_placeholder')}
            className="text-sm font-mono min-h-[80px]"
          />
        </div>

        <Button
          onClick={handlePublish}
          disabled={!topic.trim() || publishEvent.isPending}
          size="sm"
          className="w-full"
        >
          <Send className="h-3.5 w-3.5 mr-1.5" />
          {t('events.publish')}
        </Button>
      </div>
    </div>
  );
}
