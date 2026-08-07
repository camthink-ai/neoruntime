import { useTranslation } from 'react-i18next';
import { Activity, Trash2, Plug, Unplug } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { Badge } from '@/components/ui/badge';
import { ScrollArea } from '@/components/ui/scroll-area';
import { useEventStream } from '@/hooks/useEvents';

interface EventStreamProps {
  subscribedTopics: string[];
}

export default function EventStream({ subscribedTopics }: EventStreamProps) {
  const { t } = useTranslation();
  const { events, connected, connect, disconnect, clearEvents } =    useEventStream(subscribedTopics);

  return (
    <div className="rounded-lg border bg-card flex flex-col min-h-0">
      {/* Header */}
      <div className="flex items-center justify-between p-4 border-b">
        <h3 className="text-sm font-semibold flex items-center gap-2">
          <Activity className="h-4 w-4" />
          {t('events.stream')}
          <Badge
            variant={connected ? 'default' : 'secondary'}
            className="text-[10px] px-1.5 py-0"
          >
            {connected
              ? t('events.stream_connected')
              : t('events.stream_disconnected')}
          </Badge>
        </h3>
        <div className="flex items-center gap-1">
          {connected ? (
            <Button
              variant="ghost"
              size="icon-sm"
              onClick={disconnect}
              title={t('events.stream_disconnect')}
            >
              <Unplug className="h-3.5 w-3.5" />
            </Button>
          ) : (
            <Button
              variant="ghost"
              size="icon-sm"
              onClick={connect}
              title={t('events.stream_connect')}
            >
              <Plug className="h-3.5 w-3.5" />
            </Button>
          )}
          <Button
            variant="ghost"
            size="icon-sm"
            onClick={clearEvents}
            disabled={events.length === 0}
            title={t('events.stream_clear')}
          >
            <Trash2 className="h-3.5 w-3.5" />
          </Button>
        </div>
      </div>

      {/* Event list */}
      <ScrollArea className="flex-1 min-h-0">
        {events.length === 0 ? (
          <p className="text-xs text-muted-foreground text-center py-8">
            {connected ? t('events.stream_empty') : t('events.stream_paused')}
          </p>
        ) : (
          <div className="divide-y">
            {events.map(event => (
              <div
                key={event.id}
                className="px-4 py-2.5 text-xs hover:bg-accent/30 transition-colors"
              >
                <div className="flex items-center gap-2 mb-1">
                  <span className="text-muted-foreground tabular-nums">
                    {new Date(event.timestamp).toLocaleTimeString()}
                  </span>
                  <Badge
                    variant="outline"
                    className="text-[10px] px-1.5 py-0 font-mono"
                  >
                    {event.topic}
                  </Badge>
                </div>
                <pre className="text-[11px] text-muted-foreground font-mono whitespace-pre-wrap break-all leading-relaxed">
                  {JSON.stringify(event.payload, null, 2)}
                </pre>
              </div>
            ))}
          </div>
        )}
      </ScrollArea>
    </div>
  );
}
