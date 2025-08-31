//go:build !sortbench_cgo

package main

import (
    "errors"
)

// listAlgosCGO is a stub used when the CGO core is not built.
func listAlgosCGO(typ string, plugins []string) ([]string, error) {
    return nil, errors.New("cgo core not enabled; build with -tags sortbench_cgo")
}

// runCGO is a stub used when the CGO core is not built.
func runCGO(req RunRequest) ([]byte, error) {
    return nil, errors.New("cgo core not enabled; build with -tags sortbench_cgo")
}

// Indicates whether CGO core is available in this binary
func cgoAvailable() bool { return false }
