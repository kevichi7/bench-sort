package main

import (
    "net/http"
    "net/http/httptest"
    "testing"
)

func TestLimits(t *testing.T) {
    mux := http.NewServeMux()
    mux.HandleFunc("/limits", limitsHandler)
    srv := httptest.NewServer(mux)
    defer srv.Close()
    resp, err := http.Get(srv.URL + "/limits")
    if err != nil { t.Fatalf("limits error: %v", err) }
    if resp.StatusCode != 200 { t.Fatalf("unexpected status: %d", resp.StatusCode) }
}

