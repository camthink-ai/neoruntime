/**
 * @file exec.go
 * @brief ExecContainer bidirectional streaming RPC implementation with PTY support
 */

package server

import (
	"context"
	"fmt"
	"io"

	"github.com/containerd/containerd/namespaces"

	"aipc/platform/app-manager/proto"
	"aipc/platform/common/logger"
)

// ExecContainer handles bidirectional streaming for interactive shell with PTY support
func (s *AppManagerServer) ExecContainer(stream proto.AppManager_ExecContainerServer) error {
	// Receive first message to get app_id and command
	firstMsg, err := stream.Recv()
	if err != nil {
		return fmt.Errorf("failed to receive first message: %w", err)
	}

	if firstMsg.AppId == "" {
		return fmt.Errorf("app_id is required in first message")
	}

	if s.client == nil {
		return fmt.Errorf("containerd client not available")
	}

	// Get container ID from registry (try app ID first, then container ID)
	appInfo, err := s.registry.Get(firstMsg.AppId)
	if err != nil {
		if app, ok := s.registry.GetByContainerID(firstMsg.AppId); ok {
			appInfo = app
		} else {
			return fmt.Errorf("app not found: %w", err)
		}
	}

	if appInfo.ContainerID == "" {
		return fmt.Errorf("app has no container")
	}

	// Default command
	cmd := []string{"/bin/sh"}
	if firstMsg.Command != "" {
		cmd = []string{firstMsg.Command}
	}

	ctx := namespaces.WithNamespace(stream.Context(), s.config.Containerd.Namespace)

	// Use PTY mode if requested (for interactive terminal)
	if firstMsg.Tty {
		return s.execContainerWithPTY(ctx, stream, appInfo.ContainerID, cmd, int(firstMsg.Cols), int(firstMsg.Rows))
	}

	// Fallback to basic mode (for simple command execution)
	return s.execContainerBasic(ctx, stream, appInfo.ContainerID, cmd)
}

// execContainerWithPTY executes command with PTY allocation for real-time interactive terminal
func (s *AppManagerServer) execContainerWithPTY(ctx context.Context, stream proto.AppManager_ExecContainerServer, containerID string, cmd []string, cols, rows int) error {
	// Default terminal size
	if cols <= 0 {
		cols = 80
	}
	if rows <= 0 {
		rows = 24
	}

	// Create PTY session
	session, err := s.client.ExecInContainerWithPTY(ctx, containerID, cmd, cols, rows)
	if err != nil {
		return fmt.Errorf("failed to create PTY session: %w", err)
	}
	defer session.Close(ctx)

	// Channel to signal completion
	done := make(chan struct{})
	var exitCode uint32
	var execErr error

	// Goroutine: Copy PTY output to gRPC stream
	// In terminal mode, stdout carries all output (stderr is merged by the PTY)
	go func() {
		buf := make([]byte, 4096)
		for {
			n, err := session.Stdout.Read(buf)
			if n > 0 {
				if sendErr := stream.Send(&proto.ExecOutput{
					Stdout: buf[:n],
				}); sendErr != nil {
					logger.Warn("Failed to send stdout: %v", sendErr)
					return
				}
			}
			if err != nil {
				if err != io.EOF {
					logger.Debug("Stdout read ended: %v", err)
				}
				return
			}
		}
	}()

	// Goroutine: Wait for process completion
	go func() {
		exitCode, execErr = session.Wait(ctx)
		close(done)
	}()

	// Main loop: Handle stdin and resize events
	for {
		msg, err := stream.Recv()
		if err == io.EOF {
			break
		}
		if err != nil {
			logger.Warn("Stream recv error: %v", err)
			break
		}

		// Handle resize event
		if msg.Resize {
			if msg.Cols > 0 && msg.Rows > 0 {
				if err := session.Resize(ctx, int(msg.Cols), int(msg.Rows)); err != nil {
					logger.Warn("Failed to resize PTY: %v", err)
				} else {
					logger.Debug("PTY resized to %dx%d", msg.Cols, msg.Rows)
				}
			}
			continue
		}

		// Write stdin to container
		if len(msg.Stdin) > 0 {
			logger.Debug("Writing %d bytes to PTY Stdin", len(msg.Stdin))
			if _, err := session.Stdin.Write(msg.Stdin); err != nil {
				logger.Warn("Failed to write stdin: %v", err)
			}
		}
	}

	// Wait for process to complete
	<-done

	// Send final output with exit code
	if err := stream.Send(&proto.ExecOutput{
		ExitCode: int32(exitCode),
		Exited:   true,
	}); err != nil {
		logger.Warn("Failed to send exit code: %v", err)
	}

	if execErr != nil {
		return fmt.Errorf("exec failed: %w", execErr)
	}

	return nil
}

// execContainerBasic executes command without PTY (for simple command execution)
func (s *AppManagerServer) execContainerBasic(ctx context.Context, stream proto.AppManager_ExecContainerServer, containerID string, cmd []string) error {
	// Create pipes for stdin/stdout/stderr
	stdinReader, stdinWriter := io.Pipe()
	stdoutReader, stdoutWriter := io.Pipe()
	stderrReader, stderrWriter := io.Pipe()

	// Channel to signal completion
	done := make(chan struct{})
	var exitCode uint32
	var execErr error

	// Start exec in goroutine
	go func() {
		defer close(done)
		code, err := s.client.ExecInContainer(ctx, containerID, cmd, stdinReader, stdoutWriter, stderrWriter)
		if err != nil {
			logger.Error("Exec failed: %v", err)
			execErr = err
		}
		exitCode = code
		stdoutWriter.Close()
		stderrWriter.Close()
	}()

	// Goroutine: Copy stdout to stream
	go func() {
		buf := make([]byte, 4096)
		for {
			n, err := stdoutReader.Read(buf)
			if err != nil {
				return
			}
			if n > 0 {
				if err := stream.Send(&proto.ExecOutput{
					Stdout: buf[:n],
				}); err != nil {
					return
				}
			}
		}
	}()

	// Goroutine: Copy stderr to stream
	go func() {
		buf := make([]byte, 4096)
		for {
			n, err := stderrReader.Read(buf)
			if err != nil {
				return
			}
			if n > 0 {
				if err := stream.Send(&proto.ExecOutput{
					Stderr: buf[:n],
				}); err != nil {
					return
				}
			}
		}
	}()

	// Main loop: Handle stdin
	for {
		msg, err := stream.Recv()
		if err == io.EOF {
			stdinWriter.Close()
			break
		}
		if err != nil {
			stdinWriter.Close()
			break
		}

		// Write stdin to container
		if len(msg.Stdin) > 0 {
			if _, err := stdinWriter.Write(msg.Stdin); err != nil {
				logger.Warn("Failed to write stdin: %v", err)
			}
		}
	}

	// Wait for exec to complete
	<-done

	// Send final output with exit code
	if err := stream.Send(&proto.ExecOutput{
		ExitCode: int32(exitCode),
		Exited:   true,
	}); err != nil {
		logger.Warn("Failed to send exit code: %v", err)
	}

	return execErr
}
