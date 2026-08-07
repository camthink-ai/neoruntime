package handlers

import (
	"crypto/rand"
	"crypto/rsa"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/gin-gonic/gin"
)

func TestGetPublicKeyIncludesDeviceTimestamp(t *testing.T) {
	gin.SetMode(gin.TestMode)

	priv, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatalf("generate rsa key: %v", err)
	}

	handler := &APIHandlers{
		rsaPriv:   priv,
		rsaPubPEM: "-----BEGIN PUBLIC KEY-----\ntest\n-----END PUBLIC KEY-----",
	}

	router := gin.New()
	router.GET("/api/v1/auth/public-key", handler.GetPublicKey)

	before := time.Now().Unix()
	req := httptest.NewRequest(http.MethodGet, "/api/v1/auth/public-key", nil)
	resp := httptest.NewRecorder()
	router.ServeHTTP(resp, req)
	after := time.Now().Unix()

	if resp.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200; body=%s", resp.Code, resp.Body.String())
	}

	var payload struct {
		Code int `json:"code"`
		Data struct {
			PublicKey     string `json:"public_key"`
			Algorithm     string `json:"algorithm"`
			UnixTimestamp int64  `json:"unix_timestamp"`
		} `json:"data"`
	}
	if err := json.Unmarshal(resp.Body.Bytes(), &payload); err != nil {
		t.Fatalf("decode response: %v", err)
	}

	if payload.Code != CodeSuccess {
		t.Fatalf("code = %d, want %d", payload.Code, CodeSuccess)
	}
	if payload.Data.PublicKey != handler.rsaPubPEM {
		t.Fatalf("public_key mismatch")
	}
	if payload.Data.Algorithm != "RSA-2048/PKCS1v15" {
		t.Fatalf("algorithm = %q", payload.Data.Algorithm)
	}
	if payload.Data.UnixTimestamp < before || payload.Data.UnixTimestamp > after {
		t.Fatalf(
			"unix_timestamp = %d, want between %d and %d",
			payload.Data.UnixTimestamp,
			before,
			after,
		)
	}
}
