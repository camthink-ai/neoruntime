import { describe, expect, it } from 'vitest';
import { classifyModelVariant } from './variantDisplay';

describe('classifyModelVariant', () => {
  it.each([undefined, null, '', '   '])(
    'classifies %j as the platform default',
    value => {
      expect(classifyModelVariant(value)).toEqual({
        kind: 'default',
        value: '',
      });
    }
  );

  it('classifies and trims a named preset', () => {
    expect(classifyModelVariant('  hailo_yolov8n  ')).toEqual({
      kind: 'preset',
      value: 'hailo_yolov8n',
    });
  });

  it('classifies a valid JSON object as a custom variant', () => {
    expect(
      classifyModelVariant('{"backend_function":"hailo_yolov8n"}')
    ).toMatchObject({ kind: 'custom' });
  });

  it.each(['{bad', '{"backend_function":"hailo_yolov8n",}', '{} trailing'])(
    'classifies malformed override %j as invalid',
    value => {
      expect(classifyModelVariant(value)).toMatchObject({ kind: 'invalid' });
    }
  );
});
