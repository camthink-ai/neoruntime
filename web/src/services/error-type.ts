interface ErrorType {
  errorCode: string;
  errorMessage: string;
}

const statusCode: Record<string, ErrorType> = {
  '400': { errorCode: 'BAD_REQUEST', errorMessage: 'Bad request format' },
  '401': { errorCode: 'UNAUTHORIZED', errorMessage: 'Unauthorized access' },
  '403': { errorCode: 'FORBIDDEN', errorMessage: 'Forbidden access' },
  '404': { errorCode: 'NOT_FOUND', errorMessage: 'Resource not found' },
  '405': {
    errorCode: 'METHOD_NOT_ALLOWED',
    errorMessage: 'Method not allowed',
  },
  '409': { errorCode: 'CONFLICT', errorMessage: 'Resource conflict' },
  '422': {
    errorCode: 'VALIDATION_ERROR',
    errorMessage: 'Data validation error',
  },
  '429': { errorCode: 'TOO_MANY_REQUESTS', errorMessage: 'Too many requests' },
  '500': {
    errorCode: 'INTERNAL_SERVER_ERROR',
    errorMessage: 'Internal server error',
  },
  '501': {
    errorCode: 'NOT_IMPLEMENTED',
    errorMessage: 'Feature not implemented',
  },
  '503': {
    errorCode: 'SERVICE_UNAVAILABLE',
    errorMessage: 'Service unavailable',
  },
};

const businessErrorCode = {
  INVALID_PARAM: 'Invalid parameter',
  DEVICE_OFFLINE: 'Device offline',
  DEVICE_BUSY: 'Device busy',
  OPERATION_TIMEOUT: 'Operation timeout',
  STORAGE_FULL: 'Storage space insufficient',
  MODEL_INVALID: 'Invalid model',
  NETWORK_ERROR: 'Network error',
  CAMERA_ERROR: 'Camera error',
  FIRMWARE_ERROR: 'Firmware error',
  INVALID_PASSWORD: 'Invalid password',
};

export { businessErrorCode, statusCode };
