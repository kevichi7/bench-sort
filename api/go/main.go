package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"time"
)

// Basic config and defaults
var (
	defaultSortbench = "./sortbench"
	maxN             = int64(10_000_000) // cap requests
	maxRepeats       = 50
	defaultTimeout   = 2 * time.Minute
)

type MetaResponse struct {
	Types []string            `json:"types"`
	Dists []string            `json:"dists"`
	Algos map[string][]string `json:"algos"` // by type
}

// Run request payload
type RunRequest struct {
	N         int64    `json:"N"`
	Dist      string   `json:"dist"`
	Type      string   `json:"type"`
	Repeats   int      `json:"repeats,omitempty"`
	Warmup    int      `json:"warmup,omitempty"`
	Seed      *uint64  `json:"seed,omitempty"`
	Algos     []string `json:"algos,omitempty"`
	Threads   int      `json:"threads,omitempty"`
	Assert    bool     `json:"assert_sorted,omitempty"`
	Baseline  *string  `json:"baseline,omitempty"`
	Plugins   []string `json:"plugins,omitempty"`
	TimeoutMs int      `json:"timeout_ms,omitempty"`
}

type errorResp struct {
	Error string `json:"error"`
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func sbPath() string {
	if p := os.Getenv("SORTBENCH_BIN"); p != "" {
		return p
	}
	// Try PATH first
	if p, err := exec.LookPath("sortbench"); err == nil {
		return p
	}
	// Try common relative locations based on likely working directories
	candidates := []string{
		"./sortbench",
		"../sortbench",
		"../../sortbench",
		"../../../sortbench",
	}
	for _, c := range candidates {
		if fi, err := os.Stat(c); err == nil && fi.Mode().IsRegular() {
			return c
		}
	}
	return defaultSortbench
}

func types() []string { return []string{"i32", "u32", "i64", "u64", "f32", "f64", "str"} }
func dists() []string {
	return []string{"random", "partial", "dups", "reverse", "sorted", "saw", "runs", "gauss", "exp", "zipf"}
}

func metaHandler(w http.ResponseWriter, r *http.Request) {
	// Optional plugin=path query can be repeated
	plugins := r.URL.Query()["plugin"]
	algos := make(map[string][]string)
	for _, t := range types() {
		var names []string
		var err error
		if os.Getenv("SORTBENCH_CGO") == "1" {
			names, err = listAlgosCGO(t, plugins)
		} else {
			names, err = listAlgos(t, plugins)
		}
		if err != nil {
			writeJSON(w, 500, errorResp{Error: err.Error()})
			return
		}
		algos[t] = names
	}
	writeJSON(w, 200, MetaResponse{Types: types(), Dists: dists(), Algos: algos})
}

func listAlgos(typ string, plugins []string) ([]string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	args := []string{"--type", typ, "--list"}
	for _, p := range plugins {
		if p != "" {
			args = append(args, "--plugin", p)
		}
	}
	cmd := exec.CommandContext(ctx, sbPath(), args...)
	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("list algos failed: %w", err)
	}
	lines := strings.Split(string(out), "\n")
	var names []string
	for _, l := range lines {
		l = strings.TrimSpace(l)
		if l != "" {
			names = append(names, l)
		}
	}
	return names, nil
}

func runHandler(w http.ResponseWriter, r *http.Request) {
	var req RunRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, 400, errorResp{Error: "invalid JSON: " + err.Error()})
		return
	}
	if err := validate(&req); err != nil {
		writeJSON(w, 400, errorResp{Error: err.Error()})
		return
	}
	tout := defaultTimeout
	if req.TimeoutMs > 0 {
		tout = time.Duration(req.TimeoutMs) * time.Millisecond
	}
	ctx, cancel := context.WithTimeout(r.Context(), tout)
	defer cancel()
	if os.Getenv("SORTBENCH_CGO") == "1" {
		out, err := runCGO(req)
		if err != nil {
			writeJSON(w, 500, errorResp{Error: err.Error()})
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(200)
		_, _ = w.Write(out)
		return
	}
	{
		args := buildArgs(&req)
		cmd := exec.CommandContext(ctx, sbPath(), args...)
		out, err := cmd.Output()
		if err != nil {
			var ee *exec.ExitError
			if errors.As(err, &ee) {
				writeJSON(w, 500, errorResp{Error: fmt.Sprintf("sortbench failed: %s", string(ee.Stderr))})
			} else {
				writeJSON(w, 500, errorResp{Error: err.Error()})
			}
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(200)
		_, _ = w.Write(out)
	}
}

// =============== Async Jobs ===============

type JobStatus string

const (
	JobPending  JobStatus = "pending"
	JobRunning  JobStatus = "running"
	JobDone     JobStatus = "done"
	JobFailed   JobStatus = "failed"
	JobCanceled JobStatus = "canceled"
)

type Job struct {
	ID         string          `json:"id"`
	Status     JobStatus       `json:"status"`
	Error      string          `json:"error,omitempty"`
	ResultJSON json.RawMessage `json:"result,omitempty"`
	CreatedAt  time.Time       `json:"created_at"`
	StartedAt  time.Time       `json:"started_at,omitempty"`
	FinishedAt time.Time       `json:"finished_at,omitempty"`
	DurationMs int64           `json:"duration_ms,omitempty"`
	cancel     context.CancelFunc
}

type JobManager struct {
	m map[string]*Job
}

var jobs = &JobManager{m: make(map[string]*Job)}

func (jm *JobManager) create(j *Job) {
	jm.m[j.ID] = j
}
func (jm *JobManager) get(id string) (*Job, bool) { j, ok := jm.m[id]; return j, ok }

func genID() string {
	// simple time-based id
	return fmt.Sprintf("%d", time.Now().UnixNano())
}

// POST /jobs — submit async run
func submitJobHandler(w http.ResponseWriter, r *http.Request) {
	var req RunRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, 400, errorResp{Error: "invalid JSON: " + err.Error()})
		return
	}
	if err := validate(&req); err != nil {
		writeJSON(w, 400, errorResp{Error: err.Error()})
		return
	}
	id := genID()
	ctx, cancel := context.WithTimeout(context.Background(), defaultTimeout)
	job := &Job{ID: id, Status: JobPending, CreatedAt: time.Now(), cancel: cancel}
	jobs.create(job)
	go func() {
		job.Status = JobRunning
		job.StartedAt = time.Now()
		var out []byte
		var err error
		if os.Getenv("SORTBENCH_CGO") == "1" {
			out, err = runCGO(req)
		} else {
			args := buildArgs(&req)
			cmd := exec.CommandContext(ctx, sbPath(), args...)
			out, err = cmd.Output()
			if err != nil {
				var ee *exec.ExitError
				if errors.As(err, &ee) {
					err = fmt.Errorf("sortbench failed: %s", string(ee.Stderr))
				}
			}
		}
		job.FinishedAt = time.Now()
		job.DurationMs = job.FinishedAt.Sub(job.StartedAt).Milliseconds()
		if err != nil {
			if ctx.Err() == context.Canceled || ctx.Err() == context.DeadlineExceeded {
				job.Status = JobCanceled
				job.Error = ctx.Err().Error()
			} else {
				job.Status = JobFailed
				job.Error = err.Error()
			}
			return
		}
		job.ResultJSON = json.RawMessage(out)
		job.Status = JobDone
	}()
	writeJSON(w, 202, map[string]string{"job_id": id})
}

// GET /jobs/{id}
func getJobHandler(w http.ResponseWriter, r *http.Request) {
	id := strings.TrimPrefix(r.URL.Path, "/jobs/")
	if id == "" {
		writeJSON(w, 400, errorResp{Error: "missing job id"})
		return
	}
	if j, ok := jobs.get(id); ok {
		writeJSON(w, 200, j)
	} else {
		writeJSON(w, 404, errorResp{Error: "not found"})
	}
}

// POST /jobs/{id}/cancel
func cancelJobHandler(w http.ResponseWriter, r *http.Request) {
	id := strings.TrimPrefix(r.URL.Path, "/jobs/")
	id = strings.TrimSuffix(id, "/cancel")
	if id == "" {
		writeJSON(w, 400, errorResp{Error: "missing job id"})
		return
	}
	if j, ok := jobs.get(id); ok {
		if j.cancel != nil {
			j.cancel()
		}
		writeJSON(w, 200, map[string]string{"status": "cancelled"})
	} else {
		writeJSON(w, 404, errorResp{Error: "not found"})
	}
}

func validate(req *RunRequest) error {
	if req.N <= 0 || req.N > maxN {
		return fmt.Errorf("N must be in [1,%d]", maxN)
	}
	if req.Repeats < 0 || req.Repeats > maxRepeats {
		return fmt.Errorf("repeats must be in [0,%d]", maxRepeats)
	}
	// Dist/type membership
	okDist := false
	for _, d := range dists() {
		if d == req.Dist {
			okDist = true
			break
		}
	}
	if !okDist {
		return fmt.Errorf("invalid dist")
	}
	okType := false
	for _, t := range types() {
		if t == req.Type {
			okType = true
			break
		}
	}
	if !okType {
		return fmt.Errorf("invalid type")
	}
	return nil
}

func buildArgs(req *RunRequest) []string {
	args := []string{"--N", strconv.FormatInt(req.N, 10), "--dist", req.Dist, "--type", req.Type, "--format", "json"}
	if req.Repeats > 0 {
		args = append(args, "--repeat", strconv.Itoa(req.Repeats))
	}
	if req.Warmup > 0 {
		args = append(args, "--warmup", strconv.Itoa(req.Warmup))
	}
	if req.Seed != nil {
		args = append(args, "--seed", strconv.FormatUint(*req.Seed, 10))
	}
	if len(req.Algos) > 0 {
		args = append(args, "--algo", strings.Join(req.Algos, ","))
	}
	if req.Threads > 0 {
		args = append(args, "--threads", strconv.Itoa(req.Threads))
	}
	if req.Assert {
		args = append(args, "--assert-sorted")
	}
	if req.Baseline != nil && *req.Baseline != "" {
		args = append(args, "--baseline", *req.Baseline)
	}
	for _, p := range req.Plugins {
		if p != "" {
			args = append(args, "--plugin", p)
		}
	}
	// Always suppress file writes in API mode
	args = append(args, "--no-file")
	return args
}

func healthHandler(w http.ResponseWriter, r *http.Request) {
	w.WriteHeader(200)
	_, _ = w.Write([]byte("ok"))
}

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", healthHandler)
	mux.HandleFunc("/meta", metaHandler)
	mux.HandleFunc("/run", runHandler)
	mux.HandleFunc("/jobs", func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost {
			submitJobHandler(w, r)
			return
		}
		w.WriteHeader(405)
	})
	mux.HandleFunc("/jobs/", func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/cancel") && r.Method == http.MethodPost {
			cancelJobHandler(w, r)
			return
		}
		if r.Method == http.MethodGet {
			getJobHandler(w, r)
			return
		}
		w.WriteHeader(405)
	})
	addr := ":8080"
	if v := os.Getenv("PORT"); v != "" {
		addr = ":" + v
	}
	log.Printf("sortbench API listening on %s (bin=%s)", addr, sbPath())
	srv := &http.Server{Addr: addr, Handler: mux}
	log.Fatal(srv.ListenAndServe())
}
