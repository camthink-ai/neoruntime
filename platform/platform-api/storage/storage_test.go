package storage

import "testing"

func TestParseInputDimensions(t *testing.T) {
	tests := []struct {
		name       string
		line       string
		wantWidth  int
		wantHeight int
	}{
		{
			name:       "HWC bracket shape",
			line:       "Input image (HailoStream) [640, 480, 3]",
			wantWidth:  480,
			wantHeight: 640,
		},
		{
			name:       "NHWC batch shape",
			line:       "images UINT8, NHWC(1x640x480x3)",
			wantWidth:  480,
			wantHeight: 640,
		},
		{
			name:       "NCHW batch shape",
			line:       "images FLOAT32, NCHW(1x3x640x480)",
			wantWidth:  480,
			wantHeight: 640,
		},
		{
			name:       "NV12 vstream shape",
			line:       "Input  hailo_yolov8n_384_640/input_layer1 UINT8, NV12(192x640x3)",
			wantWidth:  640,
			wantHeight: 384,
		},
		{
			name:       "NV12 OCR vstream shape",
			line:       "Input  paddle_ocr_v5_mobile_recognition/input_layer1 UINT8, NV12(24x320x3)",
			wantWidth:  320,
			wantHeight: 48,
		},
		{
			name:       "NV12 segmentation vstream shape",
			line:       "Input  linknet_mbv1_ss_dpm_256/input_layer1 UINT8, NV12(128x256x3)",
			wantWidth:  256,
			wantHeight: 256,
		},
		{
			name:       "inferred NCHW shape assignment",
			line:       "images shape: 1x3x640x480",
			wantWidth:  480,
			wantHeight: 640,
		},
		{
			name:       "inferred NHWC shape assignment",
			line:       "images shape: (1, 640, 480, 3)",
			wantWidth:  480,
			wantHeight: 640,
		},
		{
			name:       "two dimension shape",
			line:       "Input image [720, 1280]",
			wantWidth:  1280,
			wantHeight: 720,
		},
		{
			name:       "dimension pair",
			line:       "Input image 1280x720",
			wantWidth:  1280,
			wantHeight: 720,
		},
		{
			name:       "output tensor is not image dimensions",
			line:       "output FLOAT32 [1, 8400, 84]",
			wantWidth:  0,
			wantHeight: 0,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			gotWidth, gotHeight := parseInputDimensions(tt.line)
			if gotWidth != tt.wantWidth || gotHeight != tt.wantHeight {
				t.Fatalf("parseInputDimensions() = %dx%d, want %dx%d", gotWidth, gotHeight, tt.wantWidth, tt.wantHeight)
			}
		})
	}
}

func TestParseHEFInfoReadsOnlyInputVStreamDimensions(t *testing.T) {
	output := `Network group name: yolov8n, Single Context
Output VStream infos:
    output_boxes (HailoStream) FLOAT32 [1, 8400, 84]
Input VStream infos:
    images (HailoStream) UINT8, NHWC(1x720x1280x3)
`

	info := parseHEFInfo(output)
	if info.NetworkName != "yolov8n" {
		t.Fatalf("NetworkName = %q, want %q", info.NetworkName, "yolov8n")
	}
	if info.InputWidth != 1280 || info.InputHeight != 720 {
		t.Fatalf("input dimensions = %dx%d, want 1280x720", info.InputWidth, info.InputHeight)
	}
}

func TestParseHEFInfoReadsNV12InputVStreamDimensions(t *testing.T) {
	output := `Network group name: hailo_yolov8n_384_640, Multi Context - Number of contexts: 2
    Network name: hailo_yolov8n_384_640/hailo_yolov8n_384_640
        VStream infos:
            Input  hailo_yolov8n_384_640/input_layer1 UINT8, NV12(192x640x3)
            Output hailo_yolov8n_384_640/yolov8_nms_postprocess FLOAT32, HAILO NMS BY CLASS(number of classes: 4, maximum bounding boxes per class: 100, maximum frame size: 8016)
`

	info := parseHEFInfo(output)
	if info.InputWidth != 640 || info.InputHeight != 384 {
		t.Fatalf("input dimensions = %dx%d, want 640x384", info.InputWidth, info.InputHeight)
	}
}

func TestParseHEFInfoDoesNotFallbackToOutputDimensions(t *testing.T) {
	output := `Network group name: detector
Output VStream infos:
    output_boxes (HailoStream) FLOAT32 [1, 8400, 84]
`

	info := parseHEFInfo(output)
	if info.InputWidth != 0 || info.InputHeight != 0 {
		t.Fatalf("input dimensions = %dx%d, want 0x0", info.InputWidth, info.InputHeight)
	}
}

func TestParseHEFInfoIgnoresInputSectionMetadata(t *testing.T) {
	output := `Network group name: detector
Input VStream infos:
    quantization range [1, 255]
    images shape: 1x3x640x480
`

	info := parseHEFInfo(output)
	if info.InputWidth != 480 || info.InputHeight != 640 {
		t.Fatalf("input dimensions = %dx%d, want 480x640", info.InputWidth, info.InputHeight)
	}
}
