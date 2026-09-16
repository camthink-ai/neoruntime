import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { aiApi, downloadModelPackage } from '@/services/api';

const unwrapApiData = (res: any) => res?.data ?? res;

// Output delivery mode: 'platform' = plugin-decoded structured results,
// 'raw' = bare tensors returned as-is (consumer decodes).
export type OutputMode = 'platform' | 'raw';

// Parse-result extras (POST /api/v1/ai/models/parse): output_format classifies
// the HEF's compiled output (nms blob vs feature map); `package` is present
// only for AMPK .bin imports and pre-fills the wizard from package metadata.
export interface ModelPackagePrefill {
  model_id: string;
  name: string;
  model_type: string;
  output_mode: string;
  config: Record<string, unknown>;
}

export const useModels = () => useQuery({
    queryKey: ['ai', 'models'],
    queryFn: async () => {
      const response = await aiApi.list();
      const data = unwrapApiData(response);
      return data?.models ?? [];
    },
    // The list endpoint syncs against the runtime over gRPC (per-model N+1),
    // so serve the cache for a beat instead of refetching on every focus.
    staleTime: 30_000,
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
  options?: { value: string; label: string; custom?: boolean }[];
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
    // onProgress/signal ride along so the wizard can show upload percentage
    // and abort mid-flight (ai.ts maps them onto axios config).
    mutationFn: async ({
      formData,
      onProgress,
      signal,
    }: {
      formData: FormData;
      onProgress?: (percent: number) => void;
      signal?: AbortSignal;
    }) => {
      const response = await aiApi.parseModel(formData, {
        onProgress,
        signal,
      });
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
      output_mode: string;
      model_variant: string;
      config: Record<string, unknown>;
      file_size: number;
      network_name: string;
      vstream_info: string;
      // Omitted when the parser could not extract a dimension — the backend
      // treats a sent 0 as "clear", so null must never become 0.
      input_width?: number;
      input_height?: number;
    }) => {
      const response = await aiApi.registerModel(data);
      return unwrapApiData(response);
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['ai', 'models'] });
    },
  });
};

export const useUpdateModel = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async ({
      modelId,
      ...data
    }: {
      modelId: string;
    } & Partial<{
      file_hash: string;
      model_type: string;
      output_mode: string;
      model_variant: string;
      config: Record<string, unknown>;
      file_size: number;
      network_name: string;
      vstream_info: string;
      input_width: number;
      input_height: number;
    }>) => {
      const response = await aiApi.update(modelId, data);
      return unwrapApiData(response);
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['ai', 'models'] });
    },
  });
};

// Export a device-level model as an AMPK .bin package (HEF + registration
// metadata) and trigger a browser download of <model_id>.bin.
export const useExportModel = () => useMutation({
    mutationFn: async (modelId: string) => {
      await downloadModelPackage(modelId);
      return modelId;
    },
  });
