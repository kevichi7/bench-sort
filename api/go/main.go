package main

import (
    "context"
    "encoding/json"
    "errors"
    "fmt"
    "log/slog"
    "log"
    "net/http"
    "os"
    "os/exec"
    "strconv"
    "strings"
    "time"

    "github.com/prometheus/client_golang/prometheus"
    "github.com/prometheus/client_golang/prometheus/promauto"
    "github.com/prometheus/client_golang/prometheus/promhttp"
)

// Basic config and defaults
var (
    defaultSortbench = "./sortbench"
    maxN             = int64(10_000_000) // cap requests
    maxRepeats       = 50
    maxThreads       = 0   // 0 = unlimited
    maxJobs          = 64  // concurrent async jobs cap
    defaultTimeout   = 2 * time.Minute
)

// Prometheus metrics
var (
    reqTotal    = promauto.NewCounterVec(prometheus.CounterOpts{Name: "sortbench_requests_total", Help: "Total HTTP requests"}, []string{"route", "status"})
    reqDuration = promauto.NewHistogramVec(prometheus.HistogramOpts{Name: "sortbench_request_duration_seconds", Help: "HTTP request duration in seconds", Buckets: prometheus.DefBuckets}, []string{"route"})
    jobsRunningGauge = promauto.NewGauge(prometheus.GaugeOpts{Name: "sortbench_jobs_running", Help: "Number of running async jobs"})
    jobsSubmitted    = promauto.NewCounter(prometheus.CounterOpts{Name: "sortbench_jobs_submitted_total", Help: "Total async jobs submitted"})
    jobsCompleted    = promauto.NewCounterVec(prometheus.CounterOpts{Name: "sortbench_jobs_completed_total", Help: "Total async jobs completed by result"}, []string{"result"})
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
    start := time.Now()
    var req RunRequest
    if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
        writeJSON(w, 400, errorResp{Error: "invalid JSON: " + err.Error()})
        slog.Warn("run_invalid_json", "error", err.Error())
        return
    }
    if err := validate(&req); err != nil {
        writeJSON(w, 400, errorResp{Error: err.Error()})
        slog.Warn("run_invalid_args", "error", err.Error(), "N", req.N, "dist", req.Dist, "type", req.Type, "repeats", req.Repeats, "threads", req.Threads)
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
            slog.Error("run_failed", "mode", "cgo", "error", err.Error(), "N", req.N, "dist", req.Dist, "type", req.Type, "repeats", req.Repeats, "threads", req.Threads, "duration_ms", time.Since(start).Milliseconds())
            return
        }
        w.Header().Set("Content-Type", "application/json")
        w.WriteHeader(200)
        _, _ = w.Write(out)
        slog.Info("run_ok", "mode", "cgo", "N", req.N, "dist", req.Dist, "type", req.Type, "repeats", req.Repeats, "threads", req.Threads, "duration_ms", time.Since(start).Milliseconds())
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
                slog.Error("run_failed", "mode", "shell", "error", string(ee.Stderr), "N", req.N, "dist", req.Dist, "type", req.Type, "repeats", req.Repeats, "threads", req.Threads, "duration_ms", time.Since(start).Milliseconds())
            } else {
                writeJSON(w, 500, errorResp{Error: err.Error()})
                slog.Error("run_failed", "mode", "shell", "error", err.Error(), "N", req.N, "dist", req.Dist, "type", req.Type, "repeats", req.Repeats, "threads", req.Threads, "duration_ms", time.Since(start).Milliseconds())
            }
            return
        }
        w.Header().Set("Content-Type", "application/json")
        w.WriteHeader(200)
        _, _ = w.Write(out)
        slog.Info("run_ok", "mode", "shell", "N", req.N, "dist", req.Dist, "type", req.Type, "repeats", req.Repeats, "threads", req.Threads, "duration_ms", time.Since(start).Milliseconds())
    }
}

// metrics wrapper
type statusRecorder struct { http.ResponseWriter; code int }
func (sr *statusRecorder) WriteHeader(c int) { sr.code = c; sr.ResponseWriter.WriteHeader(c) }

func withMetrics(route string, h func(http.ResponseWriter, *http.Request)) http.Handler {
    return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
        start := time.Now()
        rec := &statusRecorder{ResponseWriter: w, code: 200}
        h(rec, r)
        reqTotal.WithLabelValues(route, strconv.Itoa(rec.code)).Inc()
        reqDuration.WithLabelValues(route).Observe(time.Since(start).Seconds())
    })
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
    jobsSubmitted.Inc()
    slog.Info("job_submit", "job_id", id, "N", req.N, "dist", req.Dist, "type", req.Type, "repeats", req.Repeats, "threads", req.Threads)
    go func() {
        job.Status = JobRunning
        job.StartedAt = time.Now()
        jobsRunningGauge.Inc()
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
        defer jobsRunningGauge.Dec()
        if err != nil {
            if ctx.Err() == context.Canceled || ctx.Err() == context.DeadlineExceeded {
                job.Status = JobCanceled
                job.Error = ctx.Err().Error()
                jobsCompleted.WithLabelValues("canceled").Inc()
                slog.Warn("job_canceled", "job_id", id, "duration_ms", job.DurationMs)
            } else {
                job.Status = JobFailed
                job.Error = err.Error()
                jobsCompleted.WithLabelValues("failed").Inc()
                slog.Error("job_failed", "job_id", id, "error", err.Error(), "duration_ms", job.DurationMs)
            }
            return
        }
        job.ResultJSON = json.RawMessage(out)
        job.Status = JobDone
        jobsCompleted.WithLabelValues("done").Inc()
        slog.Info("job_done", "job_id", id, "duration_ms", job.DurationMs)
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
        slog.Warn("job_cancel_request", "job_id", id)
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
    if maxThreads > 0 && req.Threads > maxThreads {
        return fmt.Errorf("threads must be <= %d", maxThreads)
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

// readyz: ensure discovery and tiny smoke run succeed
func readyHandler(w http.ResponseWriter, r *http.Request) {
    // quick algo discovery for i32
    if os.Getenv("SORTBENCH_CGO") == "1" {
        if _, err := listAlgosCGO("i32", nil); err != nil {
            writeJSON(w, 500, errorResp{Error: "algo discovery failed: " + err.Error()})
            return
        }
    } else {
        if _, err := listAlgos("i32", nil); err != nil {
            writeJSON(w, 500, errorResp{Error: "algo discovery failed: " + err.Error()})
            return
        }
    }
    // tiny smoke run
    req := RunRequest{N: 256, Dist: "runs", Type: "i32", Repeats: 1, Algos: []string{"std_sort"}, TimeoutMs: 5000}
    if os.Getenv("SORTBENCH_CGO") == "1" {
        if _, err := runCGO(req); err != nil {
            writeJSON(w, 500, errorResp{Error: "smoke run failed: " + err.Error()})
            return
        }
    } else {
        ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
        defer cancel()
        args := buildArgs(&req)
        cmd := exec.CommandContext(ctx, sbPath(), args...)
        if _, err := cmd.Output(); err != nil {
            writeJSON(w, 500, errorResp{Error: "smoke run failed"})
            return
        }
    }
    w.WriteHeader(200)
    _, _ = w.Write([]byte("ready"))
}

func main() {
    // structured JSON logs with optional level
    lvl := new(slog.LevelVar)
    lvl.Set(slog.LevelInfo)
    switch strings.ToLower(os.Getenv("LOG_LEVEL")) {
    case "debug": lvl.Set(slog.LevelDebug)
    case "warn": lvl.Set(slog.LevelWarn)
    case "error": lvl.Set(slog.LevelError)
    }
    logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: lvl}))
    slog.SetDefault(logger)
    mux := http.NewServeMux()
    mux.Handle("/metrics", promhttp.Handler())
    mux.Handle("/healthz", withMetrics("healthz", healthHandler))
    mux.Handle("/readyz", withMetrics("readyz", readyHandler))
    mux.Handle("/meta", withMetrics("meta", metaHandler))
    mux.Handle("/run", withMetrics("run", runHandler))
    mux.Handle("/jobs", withMetrics("jobs_root", func(w http.ResponseWriter, r *http.Request) {
        if r.Method == http.MethodPost {
            // enforce max jobs cap
            if maxJobs > 0 {
                active := 0
                for _, j := range jobs.m {
                    if j.Status == JobPending || j.Status == JobRunning {
                        active++
                    }
                }
                if active >= maxJobs {
                    writeJSON(w, 429, errorResp{Error: "too many jobs"})
                    return
                }
            }
            submitJobHandler(w, r)
            return
        }
        w.WriteHeader(405)
    }))
    mux.Handle("/jobs/", withMetrics("jobs_item", func(w http.ResponseWriter, r *http.Request) {
        if strings.HasSuffix(r.URL.Path, "/cancel") && r.Method == http.MethodPost {
            cancelJobHandler(w, r)
            return
        }
        if r.Method == http.MethodGet {
            getJobHandler(w, r)
            return
        }
        w.WriteHeader(405)
    }))
    // Optional env config caps
    if v := os.Getenv("MAX_N"); v != "" {
        if n, err := strconv.ParseInt(v, 10, 64); err == nil && n > 0 { maxN = n }
    }
    if v := os.Getenv("MAX_REPEATS"); v != "" {
        if n, err := strconv.Atoi(v); err == nil && n >= 0 { maxRepeats = n }
    }
    if v := os.Getenv("MAX_THREADS"); v != "" {
        if n, err := strconv.Atoi(v); err == nil && n >= 0 { maxThreads = n }
    }
    if v := os.Getenv("MAX_JOBS"); v != "" {
        if n, err := strconv.Atoi(v); err == nil && n >= 0 { maxJobs = n }
    }
    if v := os.Getenv("TIMEOUT_MS"); v != "" {
        if n, err := strconv.Atoi(v); err == nil && n > 0 { defaultTimeout = time.Duration(n) * time.Millisecond }
    }

    addr := ":8080"
    if v := os.Getenv("PORT"); v != "" {
        addr = ":" + v
    }
	log.Printf("sortbench API listening on %s (bin=%s)", addr, sbPath())
	srv := &http.Server{Addr: addr, Handler: mux}
	log.Fatal(srv.ListenAndServe())
}
