import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { aiApi } from '@/services/api';

const unwrapApiData = (res: any) => res?.data ?? res;

export const useModels = () => useQuery({
    queryKey: ['ai', 'models'],
    queryFn: async () => {
      const response = await aiApi.list();
      const data = unwrapApiData(response);
      return data?.models ?? [];
    },
  });

export const useModelInfo = (modelId: string) => useQuery({
    queryKey: ['ai', 'models', modelId],
    queryFn: async () => {
      const response = await aiApi.get(modelId);
      return unwrapApiData(response);
    },
    enabled: !!modelId,
  });

export const useAIStats = () => useQuery({
    queryKey: ['ai', 'stats'],
    queryFn: async () => {
      const response = await aiApi.getStats();
      return response.data;
    },
    refetchInterval: 5000,
  });

export const useRegisterModel = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async ({
      modelPath,
      modelId,
    }: {
      modelPath: string;
      modelId?: string;
    }) => {
      const response = await aiApi.register(modelPath, modelId);
      return response.data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['ai', 'models'] });
    },
  });
};

export const useUnregisterModel = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (modelId: string) => {
      const response = await aiApi.unregister(modelId);
      return response.data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['ai', 'models'] });
    },
  });
};

export const useLoadModel = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (modelId: string) => {
      const response = await aiApi.loadModel(modelId);
      return unwrapApiData(response);
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['ai', 'models'] });
    },
  });
};

export const useUnloadModel = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (modelId: string) => {
      const response = await aiApi.unloadModel(modelId);
      return unwrapApiData(response);
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['ai', 'models'] });
    },
  });
};

export const useScanModels = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async () => {
      const response = await aiApi.scanModels();
      return unwrapApiData(response);
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['ai', 'models'] });
    },
  });
};

export interface ModelFieldDef {
  key: string;
  type: 'number' | 'text' | 'select' | 'boolean';
  required?: boolean;
  default?: number | string | boolean;
  min?: number;
  max?: number;
  step?: number;
  options?: { value: string; label: string }[];
}

export interface ModelTypeDef {
  id: string;
  label: string;
  fields: ModelFieldDef[];
  aliases?: string[];
}

export interface FileFormat {
  extension: string;
  mime_type: string;
  label: string;
}

export interface AICapabilities {
  formats: FileFormat[];
  model_types: ModelTypeDef[];
}

export const useCapabilities = () => useQuery({
    queryKey: ['ai', 'capabilities'],
    queryFn: async () => {
      const response = await aiApi.getCapabilities();
      return unwrapApiData(response) as AICapabilities;
    },
    staleTime: 5 * 60 * 1000,
  });

export const useParseModel = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (formData: FormData) => {
      const response = await aiApi.parseModel(formData);
      return unwrapApiData(response);
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['ai', 'models'] });
    },
  });
};

export const useRegisterModelV2 = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (data: {
      file_hash: string;
      model_id: string;
      model_type: string;
      model_variant: string;
      config: Record<string, unknown>;
      file_size: number;
      network_name: string;
      vstream_info: string;
      input_width: number;
      input_height: number;
    }) => {
      const response = await aiApi.registerModel(data);
      return unwrapApiData(response);
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['ai', 'models'] });
    },
  });
};
