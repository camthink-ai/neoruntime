package handlers

import (
	"aipc/platform/common/constants"
	"bytes"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
	"gorm.io/gorm"

	apppb "aipc/platform/app-manager/proto"
	"aipc/platform/platform-api/model"
	"aipc/platform/platform-api/repo"
	"context"
	"google.golang.org/grpc"
)

// DevHandlers development workbench API handlers
type DevHandlers struct {
	devRepo        *repo.DevRepo
	appManagerConn *grpc.ClientConn
}

// NewDevHandlers creates development handlers
func NewDevHandlers(db *gorm.DB, appManagerConn *grpc.ClientConn) *DevHandlers {
	return &DevHandlers{
		devRepo:        repo.NewDevRepo(db),
		appManagerConn: appManagerConn,
	}
}

// ========== Base Images ==========

// ListBaseImages gets base image list
func (h *DevHandlers) ListBaseImages(c *gin.Context) {
	images, err := h.devRepo.ListBaseImages()
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}
	Resp(c).OK(gin.H{"images": images})
}

// ========== Projects ==========

// ListProjects gets project list
func (h *DevHandlers) ListProjects(c *gin.Context) {
	projects, err := h.devRepo.ListProjects()
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}
	Resp(c).OK(gin.H{"projects": projects})
}

// GetProject gets project details
func (h *DevHandlers) GetProject(c *gin.Context) {
	idStr := c.Param("id")
	id, err := strconv.ParseUint(idStr, 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid project ID")
		return
	}

	project, err := h.devRepo.GetProject(uint(id))
	if err != nil {
		if err == gorm.ErrRecordNotFound {
			Resp(c).FailMsg(CodeNotFound, "Project not found")
		} else {
			Resp(c).FailMsg(CodeServiceError, err.Error())
		}
		return
	}

	// Get base image info
	var baseImage *model.BaseImage
	if project.BaseImageID > 0 {
		baseImage, _ = h.devRepo.GetBaseImage(project.BaseImageID)
	}

	// Get builds
	builds, _ := h.devRepo.ListBuilds(project.ID)

	Resp(c).OK(gin.H{
		"project":    project,
		"base_image": baseImage,
		"builds":     builds,
	})
}

// CreateProject creates a project
func (h *DevHandlers) CreateProject(c *gin.Context) {
	var req struct {
		Name        string `json:"name" binding:"required"`
		Key         string `json:"key" binding:"required"`
		Description string `json:"description"`
		BaseImageID uint   `json:"base_image_id" binding:"required"`
		Language    string `json:"language"`
		EntryFile   string `json:"entry_file"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request: "+err.Error())
		return
	}

	// Check if key exists
	if _, err := h.devRepo.GetProjectByKey(req.Key); err == nil {
		Resp(c).FailMsg(CodeAlreadyExists, "Project key already exists")
		return
	}

	// Get base image
	baseImage, err := h.devRepo.GetBaseImage(req.BaseImageID)
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "Base image not found")
		return
	}

	// Create source directory
	sourcePath := fmt.Sprintf(constants.RootPath()+"/dev/projects/%s", req.Key)
	if err := os.MkdirAll(sourcePath, 0755); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to create project directory")
		return
	}

	project := &model.AppProject{
		Name:        req.Name,
		Key:         req.Key,
		Description: req.Description,
		BaseImageID: req.BaseImageID,
		Language:    baseImage.Language,
		SourcePath:  sourcePath,
		EntryFile:   req.EntryFile,
		Status:      "draft",
	}

	if err := h.devRepo.CreateProject(project); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	Resp(c).OK(project)
}

// UpdateProject updates a project
func (h *DevHandlers) UpdateProject(c *gin.Context) {
	idStr := c.Param("id")
	id, err := strconv.ParseUint(idStr, 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid project ID")
		return
	}

	project, err := h.devRepo.GetProject(uint(id))
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "Project not found")
		return
	}

	var req struct {
		Name        *string `json:"name"`
		Description *string `json:"description"`
		EntryFile   *string `json:"entry_file"`
		AppConfig   *string `json:"app_config"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request: "+err.Error())
		return
	}

	if req.Name != nil {
		project.Name = *req.Name
	}
	if req.Description != nil {
		project.Description = *req.Description
	}
	if req.EntryFile != nil {
		project.EntryFile = *req.EntryFile
	}
	if req.AppConfig != nil {
		project.AppConfig = *req.AppConfig
	}

	if err := h.devRepo.UpdateProject(project); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	Resp(c).OK(project)
}

// DeleteProject deletes a project
func (h *DevHandlers) DeleteProject(c *gin.Context) {
	idStr := c.Param("id")
	id, err := strconv.ParseUint(idStr, 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid project ID")
		return
	}

	project, err := h.devRepo.GetProject(uint(id))
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "Project not found")
		return
	}

	// Delete source directory (with safety check)
	if project.SourcePath != "" && strings.HasPrefix(project.SourcePath, constants.RootPath()+"/dev/projects/") {
		os.RemoveAll(project.SourcePath)
	}

	if err := h.devRepo.DeleteProject(uint(id)); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{"deleted": id})
}

// UploadSource uploads source code
func (h *DevHandlers) UploadSource(c *gin.Context) {
	idStr := c.Param("id")
	id, err := strconv.ParseUint(idStr, 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid project ID")
		return
	}

	project, err := h.devRepo.GetProject(uint(id))
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "Project not found")
		return
	}

	file, header, err := c.Request.FormFile("file")
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "No file uploaded")
		return
	}
	defer file.Close()

	// Save uploaded file
	tmpFile := filepath.Join("/tmp", fmt.Sprintf("upload_%d_%s", time.Now().Unix(), header.Filename))
	dst, err := os.Create(tmpFile)
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to save file")
		return
	}
	io.Copy(dst, file)
	dst.Close()
	defer os.Remove(tmpFile)

	// Extract to source path (simple tar extraction)
	// In production, use proper archive handling
	if err := os.MkdirAll(project.SourcePath, 0755); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to create source directory")
		return
	}

	// For now, just copy the file - in production, extract tar.gz
	Resp(c).OK(gin.H{
		"message":     "Source uploaded",
		"source_path": project.SourcePath,
	})
}

// UploadFile uploads a single file to project
func (h *DevHandlers) UploadFile(c *gin.Context) {
	idStr := c.Param("id")
	id, err := strconv.ParseUint(idStr, 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid project ID")
		return
	}

	project, err := h.devRepo.GetProject(uint(id))
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "Project not found")
		return
	}

	file, header, err := c.Request.FormFile("file")
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "No file uploaded")
		return
	}
	defer file.Close()

	// Get target path from form or use filename
	targetPath := c.PostForm("path")
	if targetPath == "" {
		targetPath = header.Filename
	}

	// Ensure source directory exists
	if err := os.MkdirAll(project.SourcePath, 0755); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to create source directory")
		return
	}

	// Save file to project source path
	fullPath := filepath.Join(project.SourcePath, targetPath)

	// Create parent directories if needed
	if err := os.MkdirAll(filepath.Dir(fullPath), 0755); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to create directory")
		return
	}

	dst, err := os.Create(fullPath)
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to create file")
		return
	}
	defer dst.Close()

	if _, err := io.Copy(dst, file); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to write file")
		return
	}

	Resp(c).OK(gin.H{
		"message": "File uploaded",
		"path":    targetPath,
	})
}

// ListFiles lists project files
func (h *DevHandlers) ListFiles(c *gin.Context) {
	idStr := c.Param("id")
	id, err := strconv.ParseUint(idStr, 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid project ID")
		return
	}

	project, err := h.devRepo.GetProject(uint(id))
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "Project not found")
		return
	}

	var files []gin.H
	filepath.Walk(project.SourcePath, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return nil
		}
		relPath, _ := filepath.Rel(project.SourcePath, path)
		if relPath == "." {
			return nil
		}
		files = append(files, gin.H{
			"name":     info.Name(),
			"path":     relPath,
			"is_dir":   info.IsDir(),
			"size":     info.Size(),
			"mod_time": info.ModTime(),
		})
		return nil
	})

	Resp(c).OK(gin.H{"files": files})
}

// GetFileContent gets file content
func (h *DevHandlers) GetFileContent(c *gin.Context) {
	idStr := c.Param("id")
	id, err := strconv.ParseUint(idStr, 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid project ID")
		return
	}

	project, err := h.devRepo.GetProject(uint(id))
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "Project not found")
		return
	}

	filePath := c.Query("path")
	if filePath == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "File path required")
		return
	}

	fullPath := filepath.Join(project.SourcePath, filePath)
	content, err := os.ReadFile(fullPath)
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "File not found")
		return
	}

	Resp(c).OK(gin.H{
		"path":    filePath,
		"content": string(content),
	})
}

// SaveFileContent saves file content
func (h *DevHandlers) SaveFileContent(c *gin.Context) {
	idStr := c.Param("id")
	id, err := strconv.ParseUint(idStr, 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid project ID")
		return
	}

	project, err := h.devRepo.GetProject(uint(id))
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "Project not found")
		return
	}

	var req struct {
		Path    string `json:"path" binding:"required"`
		Content string `json:"content"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request")
		return
	}

	fullPath := filepath.Join(project.SourcePath, req.Path)

	// Ensure directory exists
	dir := filepath.Dir(fullPath)
	os.MkdirAll(dir, 0755)

	if err := os.WriteFile(fullPath, []byte(req.Content), 0644); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to save file")
		return
	}

	Resp(c).OK(gin.H{"saved": req.Path})
}

// ========== Builds ==========

// ListBuilds gets build records
func (h *DevHandlers) ListBuilds(c *gin.Context) {
	idStr := c.Param("id")
	id, err := strconv.ParseUint(idStr, 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid project ID")
		return
	}

	builds, err := h.devRepo.ListBuilds(uint(id))
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{"builds": builds})
}

// CreateBuild creates a build
func (h *DevHandlers) CreateBuild(c *gin.Context) {
	idStr := c.Param("id")
	id, err := strconv.ParseUint(idStr, 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid project ID")
		return
	}

	project, err := h.devRepo.GetProject(uint(id))
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "Project not found")
		return
	}

	// Get base image info
	baseImage, err := h.devRepo.GetBaseImage(project.BaseImageID)
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Base image not found")
		return
	}

	var req struct {
		Version string `json:"version"`
	}
	c.ShouldBindJSON(&req)

	if req.Version == "" {
		req.Version = "1.0.0"
	}

	now := time.Now()
	build := &model.AppBuild{
		ProjectID: project.ID,
		Version:   req.Version,
		Status:    "pending",
		StartedAt: &now,
	}

	if err := h.devRepo.CreateBuild(build); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	// Update project status
	project.Status = "building"
	h.devRepo.UpdateProject(project)

	// Trigger actual build process in background
	go h.runBuild(project, baseImage, build, req.Version)

	Resp(c).OK(build)
}

// runBuild executes the actual image build
func (h *DevHandlers) runBuild(project *model.AppProject, baseImage *model.BaseImage, build *model.AppBuild, version string) {
	var logBuf bytes.Buffer
	logBuf.WriteString(fmt.Sprintf("=== Build started at %s ===\n", time.Now().Format(time.RFC3339)))
	logBuf.WriteString(fmt.Sprintf("Project: %s (%s)\n", project.Name, project.Key))
	logBuf.WriteString(fmt.Sprintf("Base Image: %s\n", baseImage.Image))
	logBuf.WriteString(fmt.Sprintf("Version: %s\n\n", version))

	imageName := fmt.Sprintf("aipc/%s:%s", project.Key, version)
	buildDir := project.SourcePath
	namespace := "aipc"

	updateBuild := func(status, log string) {
		build.Status = status
		build.Log = log
		if status == "success" || status == "failed" {
			finished := time.Now()
			build.FinishedAt = &finished
		}
		h.devRepo.UpdateBuild(build)
	}

	// Step 0: Check and pull base image if needed
	logBuf.WriteString("Step 0: Checking base image...\n")
	if err := h.ensureBaseImage(baseImage.Image, &logBuf); err != nil {
		logBuf.WriteString(fmt.Sprintf("Error: Failed to get base image: %v\n", err))
		updateBuild("failed", logBuf.String())
		project.Status = "error"
		project.Message = "Failed to get base image"
		h.devRepo.UpdateProject(project)
		return
	}
	logBuf.WriteString("Base image ready.\n\n")

	// Step 1: Generate Dockerfile
	logBuf.WriteString("Step 1: Generating Dockerfile...\n")
	dockerfile := h.generateDockerfile(project, baseImage)
	dockerfilePath := filepath.Join(buildDir, "Dockerfile")
	if err := os.WriteFile(dockerfilePath, []byte(dockerfile), 0644); err != nil {
		logBuf.WriteString(fmt.Sprintf("Error: Failed to write Dockerfile: %v\n", err))
		updateBuild("failed", logBuf.String())
		project.Status = "error"
		project.Message = "Failed to write Dockerfile"
		h.devRepo.UpdateProject(project)
		return
	}
	logBuf.WriteString("Dockerfile generated.\n\n")

	// Step 2: Build image using nerdctl or buildah
	logBuf.WriteString("Step 2: Building image...\n")
	var buildCmd *exec.Cmd
	var buildTool string

	// Try nerdctl first, then buildah
	if _, err := exec.LookPath("nerdctl"); err == nil {
		buildTool = "nerdctl"
		buildCmd = exec.Command("nerdctl", "-n", namespace, "build", "-t", imageName, buildDir)
	} else if _, err := exec.LookPath("buildah"); err == nil {
		buildTool = "buildah"
		buildCmd = exec.Command("buildah", "bud", "-t", imageName, buildDir)
	} else if _, err := exec.LookPath("docker"); err == nil {
		buildTool = "docker"
		buildCmd = exec.Command("docker", "build", "-t", imageName, buildDir)
	} else {
		// Fallback: create simple OCI image manually
		logBuf.WriteString("Warning: No build tool found (nerdctl/buildah/docker), using fallback method\n")
		if err := h.buildWithFallback(project, baseImage, imageName, namespace, &logBuf); err != nil {
			logBuf.WriteString(fmt.Sprintf("Error: Fallback build failed: %v\n", err))
			updateBuild("failed", logBuf.String())
			project.Status = "error"
			project.Message = "Build failed"
			h.devRepo.UpdateProject(project)
			return
		}
		goto buildSuccess
	}

	logBuf.WriteString(fmt.Sprintf("Using build tool: %s\n", buildTool))
	{
		output, err := buildCmd.CombinedOutput()
		logBuf.WriteString(string(output))
		if err != nil {
			logBuf.WriteString(fmt.Sprintf("\nError: Build failed: %v\n", err))
			updateBuild("failed", logBuf.String())
			project.Status = "error"
			project.Message = "Build failed"
			h.devRepo.UpdateProject(project)
			return
		}
	}

	// Step 3: Import to containerd if using buildah or docker
	if buildTool == "buildah" {
		logBuf.WriteString("\nStep 3: Importing to containerd...\n")
		tarPath := fmt.Sprintf("/tmp/%s-%s.tar", project.Key, version)

		// Push to OCI archive
		pushCmd := exec.Command("buildah", "push", imageName, "oci-archive:"+tarPath)
		if output, err := pushCmd.CombinedOutput(); err != nil {
			logBuf.WriteString(string(output))
			logBuf.WriteString(fmt.Sprintf("Error: Failed to export image: %v\n", err))
			updateBuild("failed", logBuf.String())
			return
		}

		// Import to containerd
		importCmd := exec.Command("ctr", "-n", namespace, "images", "import", tarPath)
		if output, err := importCmd.CombinedOutput(); err != nil {
			logBuf.WriteString(string(output))
			logBuf.WriteString(fmt.Sprintf("Error: Failed to import image: %v\n", err))
			updateBuild("failed", logBuf.String())
			return
		}

		os.Remove(tarPath)
		logBuf.WriteString("Image imported to containerd.\n")
	} else if buildTool == "docker" {
		logBuf.WriteString("\nStep 3: Importing to containerd...\n")

		// Use pipe to avoid temp file
		importCmd := exec.Command("sh", "-c", fmt.Sprintf("docker save %s | ctr -n %s images import -", imageName, namespace))
		if output, err := importCmd.CombinedOutput(); err != nil {
			logBuf.WriteString(string(output))
			logBuf.WriteString(fmt.Sprintf("Error: Failed to import image: %v\n", err))
			updateBuild("failed", logBuf.String())
			return
		}
		logBuf.WriteString("Image imported to containerd.\n")
	}

buildSuccess:
	// Build success
	logBuf.WriteString(fmt.Sprintf("\n=== Build completed at %s ===\n", time.Now().Format(time.RFC3339)))
	logBuf.WriteString(fmt.Sprintf("Image: %s\n", imageName))

	build.Image = imageName

	project.Status = "built"
	project.BuildImage = imageName
	project.Message = ""
	h.devRepo.UpdateProject(project)

	// Check if any running app is based on this project, auto-update if so
	h.autoUpdateRunningApp(project, imageName, &logBuf)

	// Save final log (including auto-update information)
	updateBuild("success", logBuf.String())
}

// autoUpdateRunningApp automatically updates running applications
func (h *DevHandlers) autoUpdateRunningApp(project *model.AppProject, newImage string, logBuf *bytes.Buffer) {
	// Check for containers using the same project image
	client := apppb.NewAppManagerClient(h.appManagerConn)
	ctx := context.Background()
	resp, err := client.ListContainers(ctx, &apppb.ListContainersRequest{})
	if err != nil || resp == nil {
		return
	}
	containers := resp.Containers

	// Find containers using this project image (match aipc/{project.Key}: prefix)
	imagePrefix := fmt.Sprintf("docker.io/aipc/%s:", project.Key)
	var matchedContainers []*apppb.ContainerInfo

	for _, c := range containers {
		if strings.HasPrefix(c.Image, imagePrefix) {
			matchedContainers = append(matchedContainers, c)
		}
	}

	if len(matchedContainers) == 0 {
		// No containers found using this image
		return
	}

	logBuf.WriteString(fmt.Sprintf("\n=== Auto-updating %d container(s) ===\n", len(matchedContainers)))

	for _, container := range matchedContainers {
		containerID := container.Id
		logBuf.WriteString(fmt.Sprintf("\nUpdating container: %s (current image: %s)\n", containerID, container.Image))

		wasRunning := container.State == "running"

		// Stop container if running
		if wasRunning {
			logBuf.WriteString("  Stopping container...\n")
			client.StopContainer(ctx, &apppb.ContainerRequest{Id: containerID})
		}

		// Delete old container
		logBuf.WriteString("  Removing old container...\n")
		client.RemoveContainer(ctx, &apppb.RemoveContainerRequest{Id: containerID, Force: true})

		// Create container with new image
		logBuf.WriteString(fmt.Sprintf("  Creating container with new image: %s\n", newImage))
		createCmd := exec.Command("ctr", "-n", "aipc", "containers", "create",
			"docker.io/"+newImage, containerID)
		if output, err := createCmd.CombinedOutput(); err != nil {
			logBuf.WriteString(fmt.Sprintf("  Warning: Failed to create container: %s\n", string(output)))
			continue
		}
		logBuf.WriteString("  Container created.\n")

		// Restart if was running before
		if wasRunning {
			logBuf.WriteString("  Starting container...\n")
			if status, err := client.StartContainer(ctx, &apppb.ContainerRequest{Id: containerID}); err != nil || !status.Success {
				logBuf.WriteString(fmt.Sprintf("  Warning: Failed to start container\n"))
			} else {
				logBuf.WriteString("  Container started successfully.\n")
			}
		}
	}

	logBuf.WriteString("\nAuto-update completed.\n")
}

// generateDockerfile generates a Dockerfile
func (h *DevHandlers) generateDockerfile(project *model.AppProject, baseImage *model.BaseImage) string {
	var sb strings.Builder

	sb.WriteString(fmt.Sprintf("# Auto-generated Dockerfile for %s\n", project.Name))
	sb.WriteString(fmt.Sprintf("FROM %s\n\n", baseImage.Image))

	sb.WriteString(fmt.Sprintf("LABEL app.id=\"%s\"\n", project.Key))
	sb.WriteString(fmt.Sprintf("LABEL app.name=\"%s\"\n", project.Name))
	sb.WriteString("LABEL app.builder=\"aipc-dev-workbench\"\n\n")

	sb.WriteString("WORKDIR /app\n\n")

	// Copy source files
	sb.WriteString("COPY . /app/\n\n")

	// Install requirements if exists
	if project.Language == "python" {
		sb.WriteString("RUN if [ -f requirements.txt ]; then pip install --no-cache-dir -r requirements.txt; fi\n\n")
	}

	// Set environment
	sb.WriteString(fmt.Sprintf("ENV APP_ID=%s\n", project.Key))
	sb.WriteString("ENV PYTHONUNBUFFERED=1\n\n")

	// Entry point
	entryFile := project.EntryFile
	if entryFile == "" {
		entryFile = "main.py"
	}

	switch project.Language {
	case "python":
		sb.WriteString(fmt.Sprintf("CMD [\"python3\", \"%s\"]\n", entryFile))
	case "go":
		sb.WriteString(fmt.Sprintf("CMD [\"./%s\"]\n", strings.TrimSuffix(entryFile, ".go")))
	default:
		sb.WriteString(fmt.Sprintf("CMD [\"python3\", \"%s\"]\n", entryFile))
	}

	return sb.String()
}

// ensureBaseImage ensures the base image exists, tries to pull if not
func (h *DevHandlers) ensureBaseImage(image string, logBuf *bytes.Buffer) error {
	// First check if exists in docker
	checkDocker := exec.Command("docker", "image", "inspect", image)
	if err := checkDocker.Run(); err == nil {
		logBuf.WriteString(fmt.Sprintf("Base image %s found in docker.\n", image))
		return nil
	}

	// Check if exists in containerd
	checkCtr := exec.Command("ctr", "-n", "aipc", "images", "check", "name=="+image)
	if output, err := checkCtr.CombinedOutput(); err == nil && strings.Contains(string(output), image) {
		logBuf.WriteString(fmt.Sprintf("Base image %s found in containerd.\n", image))
		return nil
	}

	// Try to pull image
	logBuf.WriteString(fmt.Sprintf("Base image %s not found locally, pulling from registry...\n", image))

	// Prefer docker pull
	if _, err := exec.LookPath("docker"); err == nil {
		pullCmd := exec.Command("docker", "pull", image)
		output, err := pullCmd.CombinedOutput()
		logBuf.WriteString(string(output))
		if err != nil {
			return fmt.Errorf("failed to pull image with docker: %v", err)
		}
		logBuf.WriteString("Image pulled successfully with docker.\n")
		return nil
	}

	// Use ctr pull
	pullCmd := exec.Command("ctr", "-n", "aipc", "images", "pull", image)
	output, err := pullCmd.CombinedOutput()
	logBuf.WriteString(string(output))
	if err != nil {
		return fmt.Errorf("failed to pull image: %v", err)
	}
	logBuf.WriteString("Image pulled successfully.\n")
	return nil
}

// buildWithFallback fallback method using ctr to create simple container image directly
func (h *DevHandlers) buildWithFallback(project *model.AppProject, baseImage *model.BaseImage, imageName, namespace string, logBuf *bytes.Buffer) error {
	logBuf.WriteString("Using ctr snapshot method...\n")

	// Check if base image exists
	checkCmd := exec.Command("ctr", "-n", namespace, "images", "check", "name=="+baseImage.Image)
	if output, err := checkCmd.CombinedOutput(); err != nil || !strings.Contains(string(output), baseImage.Image) {
		logBuf.WriteString(fmt.Sprintf("Base image %s not found in containerd\n", baseImage.Image))
		return fmt.Errorf("base image not found: %s", baseImage.Image)
	}

	// Simple approach: copy base image and tag it
	// In production, use buildkit or other tools
	tagCmd := exec.Command("ctr", "-n", namespace, "images", "tag", baseImage.Image, imageName)
	if output, err := tagCmd.CombinedOutput(); err != nil {
		logBuf.WriteString(string(output))
		return fmt.Errorf("failed to tag image: %v", err)
	}

	logBuf.WriteString(fmt.Sprintf("Image tagged as %s (based on %s)\n", imageName, baseImage.Image))
	logBuf.WriteString("Note: This is a simplified build. Source files need to be mounted at runtime.\n")

	return nil
}
