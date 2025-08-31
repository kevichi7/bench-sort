package main

import (
    "bytes"
    "encoding/json"
    "net/http"
    "net/http/httptest"
    "os"
    "testing"
    "time"
)

func TestJobsCap(t *testing.T) {
    // Reset global jobs and set a low cap
    jobs = &JobManager{m: make(map[string]*Job)}
    oldMax := maxJobs
    maxJobs = 1
    defer func(){ maxJobs = oldMax }()
    t.Setenv("SB_TEST_JOB_DELAY_MS", "200")

    mux := http.NewServeMux()
    mux.Handle("/jobs", http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
        if r.Method == http.MethodPost {
            if maxJobs > 0 {
                if jobs.activeCount() >= maxJobs {
                    writeJSON(w, 429, errorResp{Error: "too many jobs"})
                    return
                }
            }
            submitJobHandler(w, r); return
        }
        w.WriteHeader(405)
    }))

    srv := httptest.NewServer(mux)
    defer srv.Close()

    body := RunRequest{N: 50000, Dist: "runs", Type: "i32", Repeats: 1, Algos: []string{"std_sort"}}
    var buf bytes.Buffer
    if err := json.NewEncoder(&buf).Encode(body); err != nil { t.Fatal(err) }
    // First job should be accepted (202)
    resp1, err := http.Post(srv.URL+"/jobs", "application/json", &buf)
    if err != nil { t.Fatalf("post1: %v", err) }
    if resp1.StatusCode != 202 { t.Fatalf("want 202, got %d", resp1.StatusCode) }
    // Second job immediately; should get 429 due to cap
    buf2 := bytes.Buffer{}
    _ = json.NewEncoder(&buf2).Encode(body)
    resp2, err := http.Post(srv.URL+"/jobs", "application/json", &buf2)
    if err != nil { t.Fatalf("post2: %v", err) }
    if resp2.StatusCode != 429 { t.Fatalf("want 429, got %d", resp2.StatusCode) }
    _ = os.Unsetenv("SB_TEST_JOB_DELAY_MS")
}

func TestJobCancel(t *testing.T) {
    jobs = &JobManager{m: make(map[string]*Job)}
    t.Setenv("SB_TEST_JOB_DELAY_MS", "300")
    mux := http.NewServeMux()
    mux.Handle("/jobs", http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
        if r.Method == http.MethodPost { submitJobHandler(w, r); return }
        w.WriteHeader(405)
    }))
    mux.HandleFunc("/jobs/", func(w http.ResponseWriter, r *http.Request) {
        if r.Method == http.MethodPost && r.URL.Path != "" && 
           len(r.URL.Path) > len("/jobs/") && 
           r.URL.Path[len(r.URL.Path)-len("/cancel"):] == "/cancel" {
            cancelJobHandler(w, r); return
        }
        if r.Method == http.MethodGet { getJobHandler(w, r); return }
        w.WriteHeader(405)
    })
    srv := httptest.NewServer(mux)
    defer srv.Close()

    body := RunRequest{N: 80000, Dist: "runs", Type: "i32", Repeats: 1, Algos: []string{"std_sort"}}
    var buf bytes.Buffer
    if err := json.NewEncoder(&buf).Encode(body); err != nil { t.Fatal(err) }

    resp, err := http.Post(srv.URL+"/jobs", "application/json", &buf)
    if err != nil { t.Fatalf("submit: %v", err) }
    if resp.StatusCode != 202 { t.Fatalf("want 202, got %d", resp.StatusCode) }
    var got map[string]string
    if err := json.NewDecoder(resp.Body).Decode(&got); err != nil { t.Fatal(err) }
    id := got["job_id"]
    if id == "" { t.Fatal("empty job_id") }

    // cancel quickly
    req, _ := http.NewRequest(http.MethodPost, srv.URL+"/jobs/"+id+"/cancel", nil)
    resp2, err := http.DefaultClient.Do(req)
    if err != nil { t.Fatalf("cancel: %v", err) }
    if resp2.StatusCode != 200 { t.Fatalf("cancel want 200, got %d", resp2.StatusCode) }

    // poll for status
    deadline := time.Now().Add(3 * time.Second)
    for time.Now().Before(deadline) {
        r3, err := http.Get(srv.URL+"/jobs/"+id)
        if err != nil { t.Fatalf("get: %v", err) }
        if r3.StatusCode != 200 { t.Fatalf("get want 200, got %d", r3.StatusCode) }
        var jr Job
        if err := json.NewDecoder(r3.Body).Decode(&jr); err != nil { t.Fatal(err) }
        if jr.Status == JobCanceled || jr.Status == JobFailed || jr.Status == JobDone {
            if jr.Status != JobCanceled { t.Fatalf("expected canceled, got %s", jr.Status) }
            return
        }
        time.Sleep(50 * time.Millisecond)
    }
    t.Fatalf("job did not reach canceled state in time")
}

