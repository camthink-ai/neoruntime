package handlers

import (
	"sort"
	"strconv"
	"strings"
	"syscall"

	"github.com/gin-gonic/gin"
	"github.com/shirou/gopsutil/v4/process"
)

// ProcessHandler handles process management endpoints.
type ProcessHandler struct{}

// NewProcessHandler creates a new ProcessHandler.
func NewProcessHandler() *ProcessHandler {
	return &ProcessHandler{}
}

// List returns a list of running processes.
func (h *ProcessHandler) List(c *gin.Context) {
	sortBy := c.DefaultQuery("sort", "cpu")
	limitStr := c.DefaultQuery("limit", "50")
	search := c.DefaultQuery("search", "")
	limit, _ := strconv.Atoi(limitStr)
	if limit <= 0 || limit > 500 {
		limit = 50
	}

	procs, err := process.Processes()
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	type procInfo struct {
		PID        int32   `json:"pid"`
		Name       string  `json:"name"`
		Status     string  `json:"status"`
		CPUPercent float64 `json:"cpu_percent"`
		MemPercent float32 `json:"mem_percent"`
		MemRSS     uint64  `json:"mem_rss"`
		Username   string  `json:"username"`
		Cmdline    string  `json:"cmdline"`
	}

	result := make([]procInfo, 0)
	searchLower := strings.ToLower(search)
	for _, p := range procs {
		name, _ := p.Name()
		status, _ := p.Status()
		cpuPct, _ := p.CPUPercent()
		memPct, _ := p.MemoryPercent()
		memInfo, _ := p.MemoryInfo()
		user, _ := p.Username()
		cmdline, _ := p.Cmdline()

		statusStr := ""
		if len(status) > 0 {
			statusStr = status[0]
		}

		var rss uint64
		if memInfo != nil {
			rss = memInfo.RSS
		}

		// Apply search filter
		if searchLower != "" {
			pidStr := strconv.Itoa(int(p.Pid))
			if !strings.Contains(strings.ToLower(name), searchLower) &&
				!strings.Contains(strings.ToLower(user), searchLower) &&
				!strings.Contains(strings.ToLower(cmdline), searchLower) &&
				!strings.Contains(pidStr, searchLower) {
				continue
			}
		}

		result = append(result, procInfo{
			PID:        p.Pid,
			Name:       name,
			Status:     statusStr,
			CPUPercent: cpuPct,
			MemPercent: memPct,
			MemRSS:     rss,
			Username:   user,
			Cmdline:    cmdline,
		})
	}

	// Sort
	switch sortBy {
	case "mem":
		sort.Slice(result, func(i, j int) bool { return result[i].MemPercent > result[j].MemPercent })
	case "pid":
		sort.Slice(result, func(i, j int) bool { return result[i].PID < result[j].PID })
	default: // cpu
		sort.Slice(result, func(i, j int) bool { return result[i].CPUPercent > result[j].CPUPercent })
	}

	totalMatched := len(result)
	if limit < len(result) {
		result = result[:limit]
	}

	Resp(c).OK(gin.H{
		"total":     totalMatched,
		"processes": result,
	})
}

// GetInfo returns info about a specific process.
func (h *ProcessHandler) GetInfo(c *gin.Context) {
	pidStr := c.Param("pid")
	pid, err := strconv.ParseInt(pidStr, 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid PID")
		return
	}

	p, err := process.NewProcess(int32(pid))
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "Process not found")
		return
	}

	name, _ := p.Name()
	status, _ := p.Status()
	cpuPct, _ := p.CPUPercent()
	memPct, _ := p.MemoryPercent()
	memInfo, _ := p.MemoryInfo()
	user, _ := p.Username()
	cmdline, _ := p.Cmdline()
	createTime, _ := p.CreateTime()
	ppid, _ := p.Ppid()
	cwd, _ := p.Cwd()
	exe, _ := p.Exe()
	numThreads, _ := p.NumThreads()

	info := gin.H{
		"pid":         p.Pid,
		"ppid":        ppid,
		"name":        name,
		"status":      status,
		"cpu_percent": cpuPct,
		"mem_percent": memPct,
		"username":    user,
		"cmdline":     cmdline,
		"cwd":         cwd,
		"exe":         exe,
		"create_time": createTime,
		"num_threads": numThreads,
	}
	if memInfo != nil {
		info["mem_rss"] = memInfo.RSS
		info["mem_vms"] = memInfo.VMS
	}

	Resp(c).OK(info)
}

// Kill sends a signal to a process.
func (h *ProcessHandler) Kill(c *gin.Context) {
	pidStr := c.Param("pid")
	pid, err := strconv.ParseInt(pidStr, 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid PID")
		return
	}

	signalStr := c.DefaultQuery("signal", "SIGTERM")
	var sig syscall.Signal
	switch signalStr {
	case "SIGKILL", "9":
		sig = syscall.SIGKILL
	case "SIGINT", "2":
		sig = syscall.SIGINT
	case "SIGHUP", "1":
		sig = syscall.SIGHUP
	default:
		sig = syscall.SIGTERM
	}

	p, err := process.NewProcess(int32(pid))
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "Process not found")
		return
	}

	if err := p.SendSignal(sig); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{"pid": pid, "signal": signalStr, "status": "sent"})
}
