package cmd

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"time"

	"github.com/spf13/cobra"

	"aipc/tools/aipc-cli/pkg/output"
)

var filesCmd = &cobra.Command{
	Use:   "files",
	Short: "File management",
	Long:  `Manage files on the device: list, read, write, upload, download, delete.`,
}

var (
	filesAPIBase string
)

// ============ files list ============

var filesListCmd = &cobra.Command{
	Use:   "list [path]",
	Short: "List directory contents",
	Aliases: []string{"ls"},
	Args:  cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		path := "/"
		if len(args) > 0 {
			path = args[0]
		}

		url := fmt.Sprintf("%s/api/v1/files?path=%s", filesAPIBase, path)
		resp, err := doAPIGet(url)
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(resp.Data)
		}

		var result struct {
			Path  string `json:"path"`
			Files []struct {
				Name   string `json:"name"`
				Path   string `json:"path"`
				IsDir  bool   `json:"is_dir"`
				Size   int64  `json:"size"`
				ModTime string `json:"mod_time"`
				Mode   string `json:"mode"`
			} `json:"files"`
		}

		if err := json.Unmarshal(resp.Data, &result); err != nil {
			return fmt.Errorf("failed to parse file list: %w", err)
		}

		printer.Printf("Directory: %s\n", result.Path)
		if len(result.Files) == 0 {
			printer.Info("Empty directory")
			return nil
		}

		table := output.NewTable("NAME", "TYPE", "SIZE", "MODIFIED", "MODE")
		for _, f := range result.Files {
			fileType := "file"
			if f.IsDir {
				fileType = "dir"
			}
			size := "-"
			if !f.IsDir {
				size = output.FormatBytes(int64(f.Size))
			}
			table.AddRow(f.Name, fileType, size, f.ModTime, f.Mode)
		}
		table.RenderTo(printer)
		return nil
	},
}

// ============ files get ============

var filesGetCmd = &cobra.Command{
	Use:   "get <path>",
	Short: "Read file content",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		path := args[0]
		url := fmt.Sprintf("%s/api/v1/files/content?path=%s", filesAPIBase, path)

		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return fmt.Errorf("request failed: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode == http.StatusNotFound {
			return fmt.Errorf("file not found: %s", path)
		}
		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		_, err = io.Copy(os.Stdout, resp.Body)
		return err
	},
}

// ============ files put ============

var filesPutCmd = &cobra.Command{
	Use:   "put <path> <content>",
	Short: "Write content to a file",
	Args:  cobra.ExactArgs(2),
	RunE: func(cmd *cobra.Command, args []string) error {
		path := args[0]
		content := args[1]

		body := map[string]string{"path": path, "content": content}
		payload, err := json.Marshal(body)
		if err != nil {
			return fmt.Errorf("failed to encode request: %w", err)
		}

		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "POST", filesAPIBase+"/api/v1/files/content", bytes.NewReader(payload))
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}
		req.Header.Set("Content-Type", "application/json")

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return fmt.Errorf("request failed: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		printer.Success("File written: %s", path)
		return nil
	},
}

// ============ files upload ============

var filesUploadCmd = &cobra.Command{
	Use:   "upload <local-path> <remote-path>",
	Short: "Upload a file to the device",
	Args:  cobra.ExactArgs(2),
	RunE: func(cmd *cobra.Command, args []string) error {
		localPath := args[0]
		remotePath := args[1]

		file, err := os.Open(localPath)
		if err != nil {
			return fmt.Errorf("failed to open local file: %w", err)
		}
		defer file.Close()

		ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
		defer cancel()

		// Use simple JSON approach
		content, err := io.ReadAll(file)
		if err != nil {
			return fmt.Errorf("failed to read local file: %w", err)
		}

		uploadBody := map[string]string{
			"path":    remotePath,
			"content": string(content),
		}
		payload, _ := json.Marshal(uploadBody)

		req, err := http.NewRequestWithContext(ctx, "POST", filesAPIBase+"/api/v1/files/content", bytes.NewReader(payload))
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}
		req.Header.Set("Content-Type", "application/json")

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return fmt.Errorf("upload failed: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		printer.Success("Uploaded: %s -> %s", localPath, remotePath)
		return nil
	},
}

// ============ files download ============

var filesDownloadCmd = &cobra.Command{
	Use:   "download <remote-path> [local-path]",
	Short: "Download a file from the device",
	Args:  cobra.RangeArgs(1, 2),
	RunE: func(cmd *cobra.Command, args []string) error {
		remotePath := args[0]
		localPath := filepath.Base(remotePath)
		if len(args) > 1 {
			localPath = args[1]
		}

		url := fmt.Sprintf("%s/api/v1/files/download?path=%s", filesAPIBase, remotePath)

		ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return fmt.Errorf("download failed: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		f, err := os.Create(localPath)
		if err != nil {
			return fmt.Errorf("failed to create local file: %w", err)
		}
		defer f.Close()

		if _, err := io.Copy(f, resp.Body); err != nil {
			return fmt.Errorf("failed to write file: %w", err)
		}

		printer.Success("Downloaded: %s -> %s", remotePath, localPath)
		return nil
	},
}

// ============ files delete ============

var filesDeleteCmd = &cobra.Command{
	Use:   "delete <path>",
	Short: "Delete a file or directory",
	Aliases: []string{"rm"},
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		path := args[0]

		body := map[string]string{"path": path}
		payload, _ := json.Marshal(body)

		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "DELETE", filesAPIBase+"/api/v1/files", bytes.NewReader(payload))
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}
		req.Header.Set("Content-Type", "application/json")

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return fmt.Errorf("request failed: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		printer.Success("Deleted: %s", path)
		return nil
	},
}

// ============ files mkdir ============

var filesMkdirCmd = &cobra.Command{
	Use:   "mkdir <path>",
	Short: "Create a directory",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		path := args[0]

		body := map[string]string{"path": path}
		payload, _ := json.Marshal(body)

		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "POST", filesAPIBase+"/api/v1/files/mkdir", bytes.NewReader(payload))
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}
		req.Header.Set("Content-Type", "application/json")

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return fmt.Errorf("request failed: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		printer.Success("Created directory: %s", path)
		return nil
	},
}

// ============ files rename ============

var filesRenameCmd = &cobra.Command{
	Use:   "rename <old-path> <new-path>",
	Short: "Rename or move a file",
	Aliases: []string{"mv"},
	Args:  cobra.ExactArgs(2),
	RunE: func(cmd *cobra.Command, args []string) error {
		body := map[string]string{"old_path": args[0], "new_path": args[1]}
		payload, _ := json.Marshal(body)

		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "POST", filesAPIBase+"/api/v1/files/rename", bytes.NewReader(payload))
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}
		req.Header.Set("Content-Type", "application/json")

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return fmt.Errorf("request failed: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		printer.Success("Renamed: %s -> %s", args[0], args[1])
		return nil
	},
}

func init() {
	filesCmd.PersistentFlags().StringVar(&filesAPIBase, "api", "http://localhost:8080", "Platform API base URL")

	filesCmd.AddCommand(filesListCmd)
	filesCmd.AddCommand(filesGetCmd)
	filesCmd.AddCommand(filesPutCmd)
	filesCmd.AddCommand(filesUploadCmd)
	filesCmd.AddCommand(filesDownloadCmd)
	filesCmd.AddCommand(filesDeleteCmd)
	filesCmd.AddCommand(filesMkdirCmd)
	filesCmd.AddCommand(filesRenameCmd)
}
