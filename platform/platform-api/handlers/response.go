package handlers

import (
	"github.com/gin-gonic/gin"
)

// APIResponse is the unified API response envelope.
// All API endpoints MUST return responses in this format.
//
//	Success: { "code": 0, "message": "Success", "data": {...} }
//	Error:   { "code": 1001, "message": "Invalid request", "error": {"type": "validation", "detail": "..."} }
type APIResponse struct {
	Code    int          `json:"code"`
	Message string       `json:"message"`
	Data    interface{}  `json:"data,omitempty"`
	Error   *ErrorDetail `json:"error,omitempty"`
}

// ErrorDetail contains structured error information.
type ErrorDetail struct {
	Type   string `json:"type,omitempty"`   // Error category: validation, auth, service, resource, system
	Detail string `json:"detail,omitempty"` // Detailed error description
}

// ========== Business Error Codes ==========

const (
	// 0 — Success
	CodeSuccess = 0

	// 1xxx — General / request errors
	CodeUnknownError     = 1000
	CodeInvalidRequest   = 1001
	CodeInvalidJSON      = 1002
	CodeMissingParameter = 1003
	CodeInvalidParameter = 1004
	CodeInvalidTimestamp = 1005 // request timestamp outside the freshness window

	// 2xxx — Authentication & authorization
	CodeUnauthorized = 2000
	CodeForbidden    = 2001
	CodeTokenExpired = 2002
	CodeInvalidToken = 2003

	// 3xxx — Service / infrastructure errors
	CodeServiceUnavailable = 3000
	CodeServiceTimeout     = 3001
	CodeServiceError       = 3002
	CodeGRPCError          = 3003
	CodeDatabaseError      = 3004

	// 4xxx — Resource errors
	CodeNotFound          = 4000
	CodeAlreadyExists     = 4001
	CodeResourceExhausted = 4002
	CodeOperationFailed   = 4003

	// 5xxx — AI / Model errors
	CodeModelNotFound       = 5000
	CodeModelLoadFailed     = 5001
	CodeModelInferenceError = 5002
	CodeInvalidModelFormat  = 5003

	// 6xxx — App Manager errors
	CodeAppNotFound      = 6000
	CodeAppInstallFailed = 6001
	CodeAppStartFailed   = 6002
	CodeAppStopFailed    = 6003
	CodeAppRunning       = 6004
	CodeAppNotRunning    = 6005

	// 7xxx — Device errors
	CodeDeviceError = 7000
	CodePTZError    = 7001
	CodeCameraError = 7002
	CodeGPIOError   = 7003

	// 8xxx — File / Storage errors
	CodeFileNotFound     = 8000
	CodeFileUploadFailed = 8001
	CodeFileDeleteFailed = 8002
	CodeStorageFull      = 8003
	CodeAccessDenied     = 8004

	// 9xxx — SSH errors
	CodeSSHConfigError  = 9000
	CodeSSHServiceError = 9001

	// 10xxx — Process errors
	CodeProcessNotFound   = 10000
	CodeProcessKillFailed = 10001
)

// codeMessages maps business codes to default human-readable messages.
var codeMessages = map[int]string{
	CodeSuccess:             "Success",
	CodeUnknownError:        "Unknown error",
	CodeInvalidRequest:      "Invalid request",
	CodeInvalidJSON:         "Invalid JSON format",
	CodeMissingParameter:    "Missing required parameter",
	CodeInvalidParameter:    "Invalid parameter value",
	CodeInvalidTimestamp:    "Request timestamp out of allowed window",
	CodeUnauthorized:        "Unauthorized",
	CodeForbidden:           "Forbidden",
	CodeTokenExpired:        "Token expired",
	CodeInvalidToken:        "Invalid token",
	CodeServiceUnavailable:  "Service unavailable",
	CodeServiceTimeout:      "Service timeout",
	CodeServiceError:        "Internal service error",
	CodeGRPCError:           "gRPC communication error",
	CodeDatabaseError:       "Database error",
	CodeNotFound:            "Resource not found",
	CodeAlreadyExists:       "Resource already exists",
	CodeResourceExhausted:   "Resource exhausted",
	CodeOperationFailed:     "Operation failed",
	CodeModelNotFound:       "Model not found",
	CodeModelLoadFailed:     "Failed to load model",
	CodeModelInferenceError: "Model inference error",
	CodeInvalidModelFormat:  "Invalid model format",
	CodeAppNotFound:         "Application not found",
	CodeAppInstallFailed:    "Failed to install application",
	CodeAppStartFailed:      "Failed to start application",
	CodeAppStopFailed:       "Failed to stop application",
	CodeAppRunning:          "Application is already running",
	CodeAppNotRunning:       "Application is not running",
	CodeDeviceError:         "Device error",
	CodePTZError:            "PTZ control error",
	CodeCameraError:         "Camera error",
	CodeGPIOError:           "GPIO error",
	CodeFileNotFound:        "File not found",
	CodeFileUploadFailed:    "Failed to upload file",
	CodeFileDeleteFailed:    "Failed to delete file",
	CodeStorageFull:         "Storage is full",
	CodeAccessDenied:        "Access denied",
	CodeSSHConfigError:      "SSH configuration error",
	CodeSSHServiceError:     "SSH service error",
	CodeProcessNotFound:     "Process not found",
	CodeProcessKillFailed:   "Failed to send signal to process",
}

// codeToHTTPStatus maps business codes to HTTP status codes.
var codeToHTTPStatus = map[int]int{
	CodeSuccess:             200,
	CodeUnknownError:        500,
	CodeInvalidRequest:      400,
	CodeInvalidJSON:         400,
	CodeMissingParameter:    400,
	CodeInvalidParameter:    400,
	CodeInvalidTimestamp:    400,
	CodeUnauthorized:        401,
	CodeForbidden:           403,
	CodeTokenExpired:        401,
	CodeInvalidToken:        401,
	CodeServiceUnavailable:  503,
	CodeServiceTimeout:      504,
	CodeServiceError:        500,
	CodeGRPCError:           502,
	CodeDatabaseError:       500,
	CodeNotFound:            404,
	CodeAlreadyExists:       409,
	CodeResourceExhausted:   429,
	CodeOperationFailed:     500,
	CodeModelNotFound:       404,
	CodeModelLoadFailed:     500,
	CodeModelInferenceError: 500,
	CodeInvalidModelFormat:  400,
	CodeAppNotFound:         404,
	CodeAppInstallFailed:    500,
	CodeAppStartFailed:      500,
	CodeAppStopFailed:       500,
	CodeAppRunning:          409,
	CodeAppNotRunning:       400,
	CodeDeviceError:         500,
	CodePTZError:            500,
	CodeCameraError:         500,
	CodeGPIOError:           500,
	CodeFileNotFound:        404,
	CodeFileUploadFailed:    500,
	CodeFileDeleteFailed:    500,
	CodeStorageFull:         507,
	CodeAccessDenied:        403,
	CodeSSHConfigError:      500,
	CodeSSHServiceError:     500,
	CodeProcessNotFound:     404,
	CodeProcessKillFailed:   500,
}

// GetMessage returns the default message for a business code.
func GetMessage(code int) string {
	if msg, ok := codeMessages[code]; ok {
		return msg
	}
	return "Unknown error"
}

// GetHTTPStatus returns the HTTP status for a business code.
func GetHTTPStatus(code int) int {
	if status, ok := codeToHTTPStatus[code]; ok {
		return status
	}
	return 500
}

// ========== Gin Response Helpers ==========

// R is a response helper bound to a gin.Context.
type R struct {
	c *gin.Context
}

// Resp creates a response helper for the given gin.Context.
func Resp(c *gin.Context) *R {
	return &R{c: c}
}

// OK sends { "code":0, "message":"Success", "data": data }
func (r *R) OK(data interface{}) {
	r.c.JSON(200, &APIResponse{
		Code:    CodeSuccess,
		Message: GetMessage(CodeSuccess),
		Data:    data,
	})
}

// OKMsg sends a success response with a custom message.
func (r *R) OKMsg(msg string, data interface{}) {
	r.c.JSON(200, &APIResponse{
		Code:    CodeSuccess,
		Message: msg,
		Data:    data,
	})
}

// Fail sends an error response using a business code.
// HTTP status is derived automatically from the business code.
func (r *R) Fail(code int) {
	r.c.JSON(GetHTTPStatus(code), &APIResponse{
		Code:    code,
		Message: GetMessage(code),
		Error:   &ErrorDetail{},
	})
}

// FailMsg sends an error with a custom detail message.
//
//	Resp(c).FailMsg(CodeInvalidRequest, "model_path is required")
func (r *R) FailMsg(code int, detail string) {
	r.c.JSON(GetHTTPStatus(code), &APIResponse{
		Code:    code,
		Message: GetMessage(code),
		Error:   &ErrorDetail{Detail: detail},
	})
}

// FailTyped sends an error with type and detail.
//
//	Resp(c).FailTyped(CodeGRPCError, "service", err.Error())
func (r *R) FailTyped(code int, errType, detail string) {
	r.c.JSON(GetHTTPStatus(code), &APIResponse{
		Code:    code,
		Message: GetMessage(code),
		Error:   &ErrorDetail{Type: errType, Detail: detail},
	})
}
