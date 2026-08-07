import { describe, it, expect, beforeEach, vi, afterEach } from 'vitest';
import { getItem, setItem, removeItem, clear, hasItem } from '@/utils/storage';

describe('Storage Utils', () => {
  let mockGetItem: ReturnType<typeof vi.fn>;
  let mockSetItem: ReturnType<typeof vi.fn>;
  let mockRemoveItem: ReturnType<typeof vi.fn>;
  let mockClear: ReturnType<typeof vi.fn>;

  beforeEach(() => {
    // Create new mock functions
    mockGetItem = vi.fn();
    mockSetItem = vi.fn();
    mockRemoveItem = vi.fn();
    mockClear = vi.fn();

    // Use vi.stubGlobal instead of Object.defineProperty, safer
    vi.stubGlobal('localStorage', {
      getItem: mockGetItem,
      setItem: mockSetItem,
      removeItem: mockRemoveItem,
      clear: mockClear,
      length: 0,
      key: vi.fn(),
    });

    // Clear all mock calls
    vi.clearAllMocks();
  });

  afterEach(() => {
    vi.restoreAllMocks();
    // Use vi.unstubAllGlobals to safely clean up all global stubs
    vi.unstubAllGlobals();
  });

  describe('getItem', () => {
    it('should successfully get string value', () => {
      // Implementation uses JSON.parse, so return valid JSON string here
      mockGetItem.mockReturnValue('"test-value"');

      const result = getItem('test-key');
      expect(result).toBe('test-value');
      expect(mockGetItem).toHaveBeenCalledWith('test-key');
    });

    it('should successfully get object value', () => {
      const mockValue = '{"name":"test","age":25}';
      mockGetItem.mockReturnValue(mockValue);

      const result = getItem<{ name: string; age: number }>('test-key');
      expect(result).toEqual({ name: 'test', age: 25 });
    });

    it('should return null when key does not exist', () => {
      mockGetItem.mockReturnValue(null);

      const result = getItem('non-existent-key');
      expect(result).toBeNull();
    });

    it('should return null when JSON parsing fails', () => {
      const invalidJson = 'invalid-json';
      mockGetItem.mockReturnValue(invalidJson);

      const result = getItem('test-key');
      expect(result).toBeNull();
    });

    it('should return null when localStorage is unavailable', () => {
      // Mock localStorage unavailable
      Object.defineProperty(window, 'localStorage', {
        value: undefined,
        writable: true,
      });

      const result = getItem('test-key');
      expect(result).toBeNull();
    });
  });

  describe('setItem', () => {
    it('should successfully set string value', () => {
      const key = 'test-key';
      const value = 'test-value';

      setItem(key, value);

      expect(mockSetItem).toHaveBeenCalledWith(key, JSON.stringify(value));
    });

    it('should successfully set object value', () => {
      const key = 'test-key';
      const value = { name: 'test', age: 25 };

      setItem(key, value);

      expect(mockSetItem).toHaveBeenCalledWith(key, JSON.stringify(value));
    });

    it('should successfully set number value', () => {
      const key = 'test-key';
      const value = 42;

      setItem(key, value);

      expect(mockSetItem).toHaveBeenCalledWith(key, JSON.stringify(value));
    });

    it('should successfully set array value', () => {
      const key = 'test-key';
      const value = [1, 2, 3, 'test'];

      setItem(key, value);

      expect(mockSetItem).toHaveBeenCalledWith(key, JSON.stringify(value));
    });

    it('should handle localStorage unavailable case', () => {
      // Mock localStorage unavailable
      Object.defineProperty(window, 'localStorage', {
        value: undefined,
        writable: true,
      });

      // Should not throw error
      expect(() => setItem('test-key', 'test-value')).not.toThrow();
    });

    it('should handle localStorage storage failure case', () => {
      // Mock setItem throwing error
      mockSetItem.mockImplementation(() => {
        throw new Error('QuotaExceededError');
      });

      // Should not throw error
      expect(() => setItem('test-key', 'test-value')).not.toThrow();
    });
  });

  describe('removeItem', () => {
    it('should successfully remove specified item', () => {
      const key = 'test-key';

      removeItem(key);

      expect(mockRemoveItem).toHaveBeenCalledWith(key);
    });

    it('should handle localStorage unavailable case', () => {
      // Mock localStorage unavailable
      Object.defineProperty(window, 'localStorage', {
        value: undefined,
        writable: true,
      });

      // Should not throw error
      expect(() => removeItem('test-key')).not.toThrow();
    });

    it('should handle removeItem failure case', () => {
      // Mock removeItem throwing error
      mockRemoveItem.mockImplementation(() => {
        throw new Error('Access denied');
      });

      // Should not throw error
      expect(() => removeItem('test-key')).not.toThrow();
    });
  });

  describe('clear', () => {
    it('should successfully clear all items', () => {
      clear();

      expect(mockClear).toHaveBeenCalled();
    });

    it('should handle localStorage unavailable case', () => {
      // Mock localStorage unavailable
      Object.defineProperty(window, 'localStorage', {
        value: undefined,
        writable: true,
      });

      // Should not throw error
      expect(() => clear()).not.toThrow();
    });

    it('should handle clear failure case', () => {
      // Mock clear throwing error
      mockClear.mockImplementation(() => {
        throw new Error('Access denied');
      });

      // Should not throw error
      expect(() => clear()).not.toThrow();
    });
  });

  describe('hasItem', () => {
    it('should return true when item exists', () => {
      // Mock getItem returning valid JSON string
      mockGetItem.mockReturnValue('"test-value"');

      const result = hasItem('test-key');
      expect(result).toBe(true);
    });

    it('should return false when item does not exist', () => {
      // Mock getItem returning null
      mockGetItem.mockReturnValue(null);

      const result = hasItem('test-key');
      expect(result).toBe(false);
    });

    it('should return false when JSON parsing fails', () => {
      // Mock getItem returning invalid JSON
      mockGetItem.mockReturnValue('invalid-json');

      const result = hasItem('test-key');
      expect(result).toBe(false);
    });

    it('should return false when localStorage is unavailable', () => {
      // Mock localStorage unavailable
      Object.defineProperty(window, 'localStorage', {
        value: undefined,
        writable: true,
      });

      const result = hasItem('test-key');
      expect(result).toBe(false);
    });
  });

  describe('Edge cases', () => {
    it('should handle empty string key', () => {
      setItem('', 'test-value');
      expect(mockSetItem).toHaveBeenCalledWith(
        '',
        JSON.stringify('test-value')
      );

      // Mock getItem returning set JSON value
      mockGetItem.mockReturnValue('"test-value"');
      const result = getItem('');
      expect(result).toBe('test-value');
    });

    it('should handle null and undefined values', () => {
      setItem('null-key', null);
      expect(mockSetItem).toHaveBeenCalledWith('null-key', 'null');

      setItem('undefined-key', undefined as any);
      // JSON.stringify(undefined) -> undefined
      expect(mockSetItem).toHaveBeenCalledWith('undefined-key', undefined);
    });

    it('should handle special character keys', () => {
      const specialKey = 'test-key-with-special-chars!@#$%^&*()';
      const value = 'test-value';

      setItem(specialKey, value);
      expect(mockSetItem).toHaveBeenCalledWith(
        specialKey,
        JSON.stringify(value)
      );
    });

    it('should handle large objects', () => {
      const largeObject = {
        id: 1,
        name: 'Test Object',
        data: Array.from({ length: 1000 }, (_, i) => `item-${i}`),
        metadata: {
          created: new Date().toISOString(),
          tags: ['test', 'large', 'object'],
        },
      };

      setItem('large-object', largeObject);
      expect(mockSetItem).toHaveBeenCalledWith(
        'large-object',
        JSON.stringify(largeObject)
      );
    });
  });

  describe('Error handling', () => {
    it('should handle completely unavailable localStorage case', () => {
      // Safely remove localStorage
      try {
        delete (window as any).localStorage;
      } catch {
        // If cannot delete, set to undefined
        (window as any).localStorage = undefined;
      }

      expect(() => getItem('test-key')).not.toThrow();
      expect(() => setItem('test-key', 'value')).not.toThrow();
      expect(() => removeItem('test-key')).not.toThrow();
      expect(() => clear()).not.toThrow();
      expect(() => hasItem('test-key')).not.toThrow();
    });

    it('should handle localStorage methods not existing case', () => {
      // Mock localStorage exists but methods don't exist
      const brokenLocalStorage = {} as Storage;
      Object.defineProperty(window, 'localStorage', {
        value: brokenLocalStorage,
        writable: true,
      });

      expect(() => getItem('test-key')).not.toThrow();
      expect(() => setItem('test-key', 'value')).not.toThrow();
      expect(() => removeItem('test-key')).not.toThrow();
      expect(() => clear()).not.toThrow();
      expect(() => hasItem('test-key')).not.toThrow();
    });
  });
});
