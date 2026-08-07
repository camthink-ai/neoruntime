import { useTranslation } from 'react-i18next';
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs';
import MainStream from './MainStream';
import SubStream from './SubStream';
import ThirdStream from './ThirdStream';

interface VideoStreamTabsProps {
  initialTab?: 'main' | 'sub' | 'third';
}

export default function VideoStreamTabs({
  initialTab = 'main',
}: VideoStreamTabsProps) {
  const { t } = useTranslation();

  return (
    <Tabs defaultValue={initialTab} className="w-full">
      <TabsList>
        <TabsTrigger value="main">{t('common.main_stream')}</TabsTrigger>
        <TabsTrigger value="sub">{t('common.sub_stream')}</TabsTrigger>
        <TabsTrigger value="third">{t('common.third_stream')}</TabsTrigger>
      </TabsList>

      <TabsContent value="main" className="mt-4">
        <MainStream />
      </TabsContent>
      <TabsContent value="sub" className="mt-4">
        <SubStream />
      </TabsContent>
      <TabsContent value="third" className="mt-4">
        <ThirdStream />
      </TabsContent>
    </Tabs>
  );
}
