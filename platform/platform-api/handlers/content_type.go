package handlers

import (
	"mime"
	"net/http"
	"strings"

	"github.com/gin-gonic/gin"
)

func requireJSONContentType(c *gin.Context) bool {
	mediaType, _, err := mime.ParseMediaType(c.GetHeader("Content-Type"))
	mediaType = strings.ToLower(mediaType)
	if err == nil && (mediaType == "application/json" || (strings.HasPrefix(mediaType, "application/") && strings.HasSuffix(mediaType, "+json"))) {
		return true
	}

	c.JSON(http.StatusUnsupportedMediaType, &APIResponse{
		Code:    CodeInvalidRequest,
		Message: GetMessage(CodeInvalidRequest),
		Error: &ErrorDetail{
			Type:   "validation",
			Detail: "Content-Type must be application/json or application/*+json",
		},
	})
	return false
}
