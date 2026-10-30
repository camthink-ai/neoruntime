package containerd

import (
	"context"
	"errors"
	"strings"
	"syscall"
	"testing"
	"time"

	containerdapi "github.com/containerd/containerd"
)

type stopKillCall struct {
	signal syscall.Signal
	all    bool
	ctxErr error
	ctx    context.Context
}

type fakeStopTask struct {
	waitC            chan containerdapi.ExitStatus
	waitErr          error
	killErr          error
	deleteErr        error
	releaseOnSIGKILL bool
	kills            []stopKillCall
	deleteContexts   []context.Context
	deleteCtxErrs    []error
	deleteOptCounts  []int
}

func newFakeStopTask() *fakeStopTask {
	return &fakeStopTask{waitC: make(chan containerdapi.ExitStatus, 1)}
}

func (f *fakeStopTask) ID() string { return "fake-task" }

func (f *fakeStopTask) Wait(context.Context) (<-chan containerdapi.ExitStatus, error) {
	if f.waitErr != nil {
		return nil, f.waitErr
	}
	return f.waitC, nil
}

func (f *fakeStopTask) Kill(ctx context.Context, signal syscall.Signal, opts ...containerdapi.KillOpts) error {
	info := &containerdapi.KillInfo{}
	for _, opt := range opts {
		if err := opt(ctx, info); err != nil {
			return err
		}
	}
	f.kills = append(f.kills, stopKillCall{
		signal: signal,
		all:    info.All,
		ctxErr: ctx.Err(),
		ctx:    ctx,
	})
	if signal == syscall.SIGKILL && f.releaseOnSIGKILL {
		f.waitC <- *containerdapi.NewExitStatus(137, time.Now(), nil)
	}
	return f.killErr
}

func (f *fakeStopTask) Delete(ctx context.Context, opts ...containerdapi.ProcessDeleteOpts) (*containerdapi.ExitStatus, error) {
	f.deleteContexts = append(f.deleteContexts, ctx)
	f.deleteCtxErrs = append(f.deleteCtxErrs, ctx.Err())
	f.deleteOptCounts = append(f.deleteOptCounts, len(opts))
	if f.deleteErr != nil {
		return nil, f.deleteErr
	}
	return containerdapi.NewExitStatus(0, time.Now(), nil), nil
}

func requireKill(t *testing.T, call stopKillCall, signal syscall.Signal) {
	t.Helper()
	if call.signal != signal {
		t.Fatalf("signal = %v, want %v", call.signal, signal)
	}
	if !call.all {
		t.Fatal("kill did not include containerd.WithKillAll")
	}
}

func TestStopContainerGracefulExitDeletesTask(t *testing.T) {
	task := newFakeStopTask()
	task.waitC <- *containerdapi.NewExitStatus(0, time.Now(), nil)

	c := &Client{namespace: "test"}
	if err := c.stopContainer(context.Background(), task, time.Second); err != nil {
		t.Fatalf("stopContainer: %v", err)
	}

	if len(task.kills) != 1 {
		t.Fatalf("kill calls = %d, want one SIGTERM", len(task.kills))
	}
	requireKill(t, task.kills[0], syscall.SIGTERM)
	if len(task.deleteOptCounts) != 1 || task.deleteOptCounts[0] == 0 {
		t.Fatalf("delete opts = %v, want WithProcessKill", task.deleteOptCounts)
	}
}

func TestStopContainerCancelledCallerStillForceKills(t *testing.T) {
	task := newFakeStopTask()
	task.releaseOnSIGKILL = true
	c := &Client{namespace: "test"}

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := c.stopContainer(ctx, task, time.Second); err != nil {
		t.Fatalf("stopContainer: %v", err)
	}

	if len(task.kills) != 2 {
		t.Fatalf("kill calls = %d, want SIGTERM then SIGKILL", len(task.kills))
	}
	requireKill(t, task.kills[0], syscall.SIGTERM)
	requireKill(t, task.kills[1], syscall.SIGKILL)
	if task.kills[1].ctxErr != nil {
		t.Fatalf("SIGKILL inherited cancelled caller context: %v", task.kills[1].ctxErr)
	}
	if len(task.deleteCtxErrs) != 1 || task.deleteCtxErrs[0] != nil {
		t.Fatalf("delete did not receive a fresh context: %v", task.deleteCtxErrs)
	}
}

func TestStopContainerWaitFailureUsesIndependentDeleteContext(t *testing.T) {
	task := newFakeStopTask()
	task.waitErr = errors.New("wait unavailable")
	c := &Client{namespace: "test"}

	if err := c.stopContainer(context.Background(), task, time.Second); err != nil {
		t.Fatalf("stopContainer: %v", err)
	}

	if len(task.kills) != 1 || len(task.deleteContexts) != 1 {
		t.Fatalf("kill/delete calls = %d/%d, want 1/1", len(task.kills), len(task.deleteContexts))
	}
	requireKill(t, task.kills[0], syscall.SIGKILL)
	if task.kills[0].ctx == task.deleteContexts[0] {
		t.Fatal("SIGKILL and Delete reused the same operation context")
	}
	if task.deleteCtxErrs[0] != nil {
		t.Fatalf("delete context was already done at call time: %v", task.deleteCtxErrs[0])
	}
}

func TestStopContainerDeleteFailureIsReported(t *testing.T) {
	task := newFakeStopTask()
	task.waitC <- *containerdapi.NewExitStatus(0, time.Now(), nil)
	task.deleteErr = errors.New("shim busy")
	c := &Client{namespace: "test"}

	err := c.stopContainer(context.Background(), task, time.Second)
	if err == nil || !strings.Contains(err.Error(), "force delete failed") {
		t.Fatalf("error = %v, want force-delete failure", err)
	}
}
