// Test model inference API
export const testModelInferenceApi = async (imageData: {
  image_id: string;
  model_name?: string;
}) => {
  try {
    const response = await fetch('/api/modelVerification/inference', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
      },
      body: JSON.stringify(imageData),
    });
    const data = await response.json();
    console.log('POST /api/modelVerification/inference response:', data);
    return data;
  } catch (error) {
    console.error('POST /api/modelVerification/inference error:', error);
    throw error;
  }
};

// Run all tests
export const runAllTests = async () => {
  console.log('🚀 Starting Mock API tests...');

  try {
    // Test model inference API
    await testModelInferenceApi({
      image_id: 'test_image_001',
      model_name: 'test_model_v1.0',
    });

    console.log('✅ All Mock API tests completed!');
  } catch (error) {
    console.error('❌ Mock API tests failed:', error);
  }
};
