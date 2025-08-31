package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestHealthz(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", healthHandler)
	srv := httptest.NewServer(mux)
	defer srv.Close()
	resp, err := http.Get(srv.URL + "/healthz")
	if err != nil {
		t.Fatalf("healthz error: %v", err)
	}
	if resp.StatusCode != 200 {
		t.Fatalf("unexpected status: %d", resp.StatusCode)
	}
}

func TestMeta(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/meta", metaHandler)
	srv := httptest.NewServer(mux)
	defer srv.Close()
	resp, err := http.Get(srv.URL + "/meta")
	if err != nil {
		t.Fatalf("meta error: %v", err)
	}
	if resp.StatusCode != 200 {
		t.Fatalf("unexpected status: %d", resp.StatusCode)
	}
}

func TestRunSmall(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/run", runHandler)
	srv := httptest.NewServer(mux)
	defer srv.Close()
	body := RunRequest{N: 256, Dist: "runs", Type: "i32", Repeats: 1, Algos: []string{"std_sort"}, Assert: true}
	var buf bytes.Buffer
	if err := json.NewEncoder(&buf).Encode(body); err != nil {
		t.Fatal(err)
	}
	resp, err := http.Post(srv.URL+"/run", "application/json", &buf)
	if err != nil {
		t.Fatalf("run error: %v", err)
	}
	if resp.StatusCode != 200 {
		t.Fatalf("unexpected status: %d", resp.StatusCode)
	}
}

func TestDistsIncludesNewOnes(t *testing.T) {
    have := map[string]bool{}
    for _, d := range dists() { have[d] = true }
    want := []string{"organpipe", "staggered", "runs_ht"}
    for _, w := range want {
        if !have[w] {
            t.Fatalf("missing dist %q in dists()", w)
        }
    }
}

func TestValidateAcceptsNewDists(t *testing.T) {
    for _, d := range []string{"organpipe", "staggered", "runs_ht"} {
        req := RunRequest{N: 16, Dist: d, Type: "i32", Repeats: 0}
        if err := validate(&req); err != nil {
            t.Fatalf("validate rejected dist %s: %v", d, err)
        }
    }
}
