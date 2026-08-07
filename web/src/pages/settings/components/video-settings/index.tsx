import { useTranslation } from 'react-i18next';
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs';
import MainStream from './MainStream';
import SubStream from './SubStream';
import ThirdStream from './ThirdStream';

export default function VideoSettings() {
  const { t } = useTranslation();

  return (
    <div className="space-y-6">
      <div>
        <h2 className="text-2xl font-semibold">
          {t('sys.media_settings.video')}
        </h2>
      </div>

      <Tabs defaultValue="main" className="w-full">
        <TabsList>
          <TabsTrigger value="main">
            {t('sys.media_settings.main_stream')}
          </TabsTrigger>
          <TabsTrigger value="sub">
            {t('sys.media_settings.sub_stream')}
          </TabsTrigger>
          <TabsTrigger value="third">
            {t('sys.media_settings.third_stream')}
          </TabsTrigger>
        </TabsList>

        <TabsContent value="main" className="mt-6">
          <MainStream />
        </TabsContent>

        <TabsContent value="sub" className="mt-6">
          <SubStream />
        </TabsContent>

        <TabsContent value="third" className="mt-6">
          <ThirdStream />
        </TabsContent>
      </Tabs>
    </div>
  );
}
