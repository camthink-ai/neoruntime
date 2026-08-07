package handlers

import (
	"encoding/json"
	"net/http"
	"os"
	"os/exec"
	"strings"
	"time"

	"github.com/creack/pty"
	"github.com/gin-gonic/gin"
	"github.com/gorilla/websocket"
	"github.com/sergeymakinen/go-crypt"
	_ "github.com/sergeymakinen/go-crypt/bcrypt"
	_ "github.com/sergeymakinen/go-crypt/md5"
	_ "github.com/sergeymakinen/go-crypt/sha256"
	_ "github.com/sergeymakinen/go-crypt/sha512"

	"aipc/platform/common/logger"
)

// TerminalHandler handles web terminal WebSocket connections.
type TerminalHandler struct {
	shell    string
	upgrader websocket.Upgrader
}

// NewTerminalHandler creates a new TerminalHandler.
func NewTerminalHandler() *TerminalHandler {
	shell := "/bin/bash"
	if _, err := os.Stat(shell); err != nil {
		shell = "/bin/sh"
	}
	return &TerminalHandler{
		shell:    shell,
		upgrader: websocket.Upgrader{CheckOrigin: func(r *http.Request) bool { return true }},
	}
}

// inputMsg represents an input message from the client.
type inputMsg struct {
	Type string `json:"type"`
	Data string `json:"data"`
	Cols uint16 `json:"cols"`
	Rows uint16 `json:"rows"`
}

// HandleTerminalWS upgrades to WebSocket and connects to a PTY shell.
func (h *TerminalHandler) HandleTerminalWS(c *gin.Context) {
	conn, err := h.upgrader.Upgrade(c.Writer, c.Request, nil)
	if err != nil {
		logger.Error("WebSocket upgrade failed: %v", err)
		return
	}
	defer conn.Close()

	// System user authentication via /etc/shadow
	if !h.authenticateTerminal(conn) {
		return
	}

	// Start shell with PTY
	cmd := exec.Command(h.shell)
	cmd.Env = append(os.Environ(),
		"TERM=xterm-256color",
		"PS1=\\[\\e[1;36m\\]NE503\\[\\e[0m\\]:\\[\\e[1;34m\\]\\w\\[\\e[0m\\]\\$ ")

	ptmx, err := pty.Start(cmd)
	if err != nil {
		logger.Error("Failed to start PTY: %v", err)
		conn.WriteMessage(websocket.TextMessage, []byte("Failed to start terminal: "+err.Error()))
		return
	}
	defer func() {
		ptmx.Close()
		cmd.Process.Kill()
		cmd.Wait()
	}()

	// PTY -> WebSocket (stdout)
	go func() {
		buf := make([]byte, 4096)
		for {
			n, err := ptmx.Read(buf)
			if err != nil {
				conn.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""))
				return
			}
			if err := conn.WriteMessage(websocket.TextMessage, buf[:n]); err != nil {
				return
			}
		}
	}()

	// WebSocket -> PTY (stdin + resize)
	for {
		_, msg, err := conn.ReadMessage()
		if err != nil {
			break
		}

		var input inputMsg
		if err := json.Unmarshal(msg, &input); err == nil {
			switch input.Type {
			case "input":
				ptmx.Write([]byte(input.Data))
			case "resize":
				if input.Cols > 0 && input.Rows > 0 {
					pty.Setsize(ptmx, &pty.Winsize{Cols: input.Cols, Rows: input.Rows})
				}
			}
		} else {
			ptmx.Write(msg)
		}
	}
}

// authenticateTerminal performs in-terminal login against system users.
func (h *TerminalHandler) authenticateTerminal(conn *websocket.Conn) bool {
	const maxAttempts = 3

	conn.SetReadDeadline(time.Now().Add(60 * time.Second))
	defer conn.SetReadDeadline(time.Time{})

	conn.WriteMessage(websocket.TextMessage, []byte("\033[1;36mNE503 Terminal\033[0m\r\n\r\n"))

	for attempt := 0; attempt < maxAttempts; attempt++ {
		conn.WriteMessage(websocket.TextMessage, []byte("login: "))
		username := h.readLine(conn, true)
		if username == "" {
			if attempt == maxAttempts-1 {
				conn.WriteMessage(websocket.TextMessage, []byte("\r\nConnection closed.\r\n"))
				return false
			}
			continue
		}

		conn.WriteMessage(websocket.TextMessage, []byte("Password: "))
		password := h.readLine(conn, false)
		if password == "" {
			if attempt == maxAttempts-1 {
				conn.WriteMessage(websocket.TextMessage, []byte("\r\nConnection closed.\r\n"))
				return false
			}
			continue
		}

		if validateSystemUser(username, password) {
			conn.WriteMessage(websocket.TextMessage, []byte("\r\n"))
			logger.Info("Terminal login success for user: %s", username)
			return true
		}

		conn.WriteMessage(websocket.TextMessage, []byte("\r\nLogin incorrect\r\n\r\n"))
	}

	conn.WriteMessage(websocket.TextMessage, []byte("Maximum login attempts exceeded.\r\n\r\nConnection closed.\r\n"))
	return false
}

// validateSystemUser validates credentials against /etc/shadow.
func validateSystemUser(username, password string) bool {
	data, err := os.ReadFile("/etc/shadow")
	if err != nil {
		logger.Error("Failed to read /etc/shadow: %v", err)
		return false
	}

	for _, line := range strings.Split(string(data), "\n") {
		parts := strings.SplitN(line, ":", 9)
		if len(parts) < 2 || parts[0] != username {
			continue
		}

		hashed := parts[1]
		// Locked or no-login accounts
		if hashed == "" || hashed == "!" || hashed == "*" || hashed == "!!" || strings.HasPrefix(hashed, "!") {
			return false
		}

		return verifyPassword(hashed, password)
	}

	return false
}

// verifyPassword checks a plaintext password against a shadow hash.
// Supports: $6$ (SHA-512), $5$ (SHA-256), $1$ (MD5), $2b$/$2a$/$2y$ (bcrypt).
func verifyPassword(hashed, password string) bool {
	return crypt.Check(hashed, password) == nil
}

// readLine reads a line of input from the WebSocket.
func (h *TerminalHandler) readLine(conn *websocket.Conn, echoPlain bool) string {
	var line []byte

	for {
		_, msg, err := conn.ReadMessage()
		if err != nil {
			return ""
		}

		var input inputMsg
		if jsonErr := json.Unmarshal(msg, &input); jsonErr == nil && input.Type == "input" {
			msg = []byte(input.Data)
		} else if jsonErr == nil {
			continue
		}

		for _, ch := range msg {
			switch ch {
			case '\r', '\n':
				conn.WriteMessage(websocket.TextMessage, []byte("\r\n"))
				if len(line) > 0 {
					return string(line)
				}
				return ""
			case 127, 8:
				if len(line) > 0 {
					line = line[:len(line)-1]
					if echoPlain {
						conn.WriteMessage(websocket.TextMessage, []byte("\b \b"))
					}
				}
			case 3:
				return ""
			default:
				if ch >= 32 && ch < 127 {
					line = append(line, ch)
					if echoPlain {
						conn.WriteMessage(websocket.TextMessage, []byte{ch})
					} else {
						conn.WriteMessage(websocket.TextMessage, []byte("*"))
					}
				}
			}
		}
	}
}
