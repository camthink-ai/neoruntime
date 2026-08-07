import store from 'store';

// Error type
interface StorageError {
  code: 'QUOTA_EXCEEDED' | 'NOT_SUPPORTED' | 'ACCESS_DENIED' | 'UNKNOWN';
  message: string;
  key?: string;
}

// Check if localStorage is available
const isStorageAvailable = (): boolean => {
  try {
    if (typeof window === 'undefined') {
      return false;
    }
    return store.enabled;
  } catch {
    return false;
  }
};

// Basic getItem
export const getItem = <T = any>(key: string): T | null => {
  try {
    if (!isStorageAvailable()) {
      throw new Error('localStorage is not available');
    }

    const value = store.get(key) as T | undefined;
    return value === undefined ? null : value;
  } catch (err) {
    console.error('Failed to get item:', err);
    return null;
  }
};

// Basic setItem
export const setItem = <T = any>(key: string, value: T): void => {
  try {
    if (!isStorageAvailable()) {
      throw new Error('localStorage is not available');
    }

    store.set(key, value);
  } catch (err) {
    console.error('Failed to set item:', err);
  }
};

// Remove specified item
export const removeItem = (key: string): void => {
  try {
    if (!isStorageAvailable()) {
      throw new Error('localStorage is not available');
    }

    store.remove(key);
  } catch (err) {
    console.error('Failed to remove item:', err);
  }
};

// Clear all items
export const clear = (): void => {
  try {
    if (!isStorageAvailable()) {
      throw new Error('localStorage is not available');
    }

    store.clearAll();
  } catch (err) {
    console.error('Failed to clear storage:', err);
  }
};

// Check if specified item exists
export const hasItem = (key: string): boolean => getItem(key) !== null;

// Export type
export type { StorageError };
