export type WidgetType = 'statistic' | 'activity' | 'resource' | 'iframe';

export interface BaseWidget {
  id: string;
  type: WidgetType;
  title: string;
  order: number;
}

export interface StatisticWidget extends BaseWidget {
  type: 'statistic';
  value: number;
  suffix?: string;
  icon: string;
  color: string;
}

export interface ActivityWidget extends BaseWidget {
  type: 'activity';
}

export interface ResourceWidget extends BaseWidget {
  type: 'resource';
}

export interface IframeWidget extends BaseWidget {
  type: 'iframe';
  url: string;
  height?: number;
}

export type Widget =
  | StatisticWidget
  | ActivityWidget
  | ResourceWidget
  | IframeWidget;
