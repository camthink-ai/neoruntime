import { RouterProvider } from 'react-router-dom';
import router from '@/router';
import { ThemeProvider } from '@/components/theme-provider';
import { TooltipProvider } from '@/components/ui/tooltip';
import { useThemeStore } from '@/store/theme';

// Initialize color theme from localStorage before first render
useThemeStore.getState().initTheme();

function App() {
  return (
    <ThemeProvider>
      <TooltipProvider>
        <RouterProvider router={router} />
      </TooltipProvider>
    </ThemeProvider>
  );
}

export default App;
