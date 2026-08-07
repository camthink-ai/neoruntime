package handlers

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/gin-gonic/gin"

	platformauth "aipc/platform/platform-api/auth"
)

func TestLoginLogoutRevokesSessionToken(t *testing.T) {
	gin.SetMode(gin.TestMode)
	validator := platformauth.NewTokenValidator("integration-api-key", true)
	handler := &APIHandlers{
		authUser:       "admin",
		authPass:       "password",
		tokenValidator: validator,
	}

	router := gin.New()
	router.POST("/api/login", handler.Login)
	router.POST("/api/v1/logout", handler.Logout)
	protected := router.Group("/api/v1")
	protected.Use(platformauth.Middleware(validator))
	protected.GET("/protected", func(c *gin.Context) {
		c.Status(http.StatusOK)
	})

	loginBody := bytes.NewBufferString(`{"username":"admin","password":"password"}`)
	loginRequest := httptest.NewRequest(http.MethodPost, "/api/login", loginBody)
	loginRequest.Header.Set("Content-Type", "application/json")
	loginResponse := httptest.NewRecorder()
	router.ServeHTTP(loginResponse, loginRequest)
	if loginResponse.Code != http.StatusOK {
		t.Fatalf("login status = %d, want 200; body=%s", loginResponse.Code, loginResponse.Body.String())
	}

	var payload struct {
		Data struct {
			Token string `json:"token"`
		} `json:"data"`
	}
	if err := json.Unmarshal(loginResponse.Body.Bytes(), &payload); err != nil {
		t.Fatalf("decode login response: %v", err)
	}
	if payload.Data.Token == "" {
		t.Fatal("login response did not contain a token")
	}

	requestWithToken := func(method, path string) *http.Request {
		req := httptest.NewRequest(method, path, nil)
		req.Header.Set("Authorization", payload.Data.Token)
		return req
	}

	beforeLogout := httptest.NewRecorder()
	router.ServeHTTP(beforeLogout, requestWithToken(http.MethodGet, "/api/v1/protected"))
	if beforeLogout.Code != http.StatusOK {
		t.Fatalf("protected status before logout = %d, want 200", beforeLogout.Code)
	}

	logoutResponse := httptest.NewRecorder()
	router.ServeHTTP(logoutResponse, requestWithToken(http.MethodPost, "/api/v1/logout"))
	if logoutResponse.Code != http.StatusOK {
		t.Fatalf("logout status = %d, want 200", logoutResponse.Code)
	}

	afterLogout := httptest.NewRecorder()
	router.ServeHTTP(afterLogout, requestWithToken(http.MethodGet, "/api/v1/protected"))
	if afterLogout.Code != http.StatusUnauthorized {
		t.Fatalf("protected status after logout = %d, want 401", afterLogout.Code)
	}

	repeatedLogout := httptest.NewRecorder()
	router.ServeHTTP(repeatedLogout, requestWithToken(http.MethodPost, "/api/v1/logout"))
	if repeatedLogout.Code != http.StatusOK {
		t.Fatalf("repeated logout status = %d, want 200", repeatedLogout.Code)
	}
}
