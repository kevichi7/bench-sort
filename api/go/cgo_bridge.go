package main

/*
#cgo CXXFLAGS: -I../../include -std=c++20 -O3
#cgo LDFLAGS: -L../.. -lsortbench_core -lstdc++ -fopenmp -ldl -ltbb -lm
#include <stdlib.h>
#include "../../include/sortbench/capi.h"
*/
import "C"

import (
	"encoding/json"
	"errors"
	"fmt"
	"unsafe"
)

func elemTypeCode(t string) (int, error) {
	switch t {
	case "i32":
		return int(C.SB_ELEM_I32), nil
	case "u32":
		return int(C.SB_ELEM_U32), nil
	case "i64":
		return int(C.SB_ELEM_I64), nil
	case "u64":
		return int(C.SB_ELEM_U64), nil
	case "f32":
		return int(C.SB_ELEM_F32), nil
	case "f64":
		return int(C.SB_ELEM_F64), nil
	case "str":
		return int(C.SB_ELEM_STR), nil
	}
	return 0, fmt.Errorf("invalid type")
}

func distCode(d string) (int, error) {
    switch d {
    case "random":
        return int(C.SB_DIST_RANDOM), nil
    case "partial":
        return int(C.SB_DIST_PARTIAL), nil
    case "dups":
        return int(C.SB_DIST_DUPS), nil
    case "reverse":
        return int(C.SB_DIST_REVERSE), nil
    case "sorted":
        return int(C.SB_DIST_SORTED), nil
    case "saw":
        return int(C.SB_DIST_SAW), nil
    case "runs":
        return int(C.SB_DIST_RUNS), nil
    case "gauss":
        return int(C.SB_DIST_GAUSS), nil
    case "exp":
        return int(C.SB_DIST_EXP), nil
    case "zipf":
        return int(C.SB_DIST_ZIPF), nil
    case "organpipe":
        return int(C.SB_DIST_ORGANPIPE), nil
    case "staggered":
        return int(C.SB_DIST_STAGGERED), nil
    case "runs_ht":
        return int(C.SB_DIST_RUNS_HT), nil
    }
    return 0, fmt.Errorf("invalid dist")
}

// allocates a C array of char* and fills it with C.CString copies of strs
// returns (**C.char, freeFunc)
func makeCStrArray(strs []string) (**C.char, func()) {
	n := len(strs)
	if n == 0 {
		return nil, func() {}
	}
	elemSize := uintptr(unsafe.Sizeof((*C.char)(nil)))
	total := C.size_t(uintptr(n) * elemSize)
	base := C.malloc(total)
	if base == nil {
		return nil, func() {}
	}
	arr := (*[1 << 30]*C.char)(base)[:n:n]
	// keep track to free each string
	cs := make([]*C.char, n)
	for i, s := range strs {
		cs[i] = C.CString(s)
		arr[i] = cs[i]
	}
	freeFn := func() {
		for _, p := range cs {
			if p != nil {
				C.free(unsafe.Pointer(p))
			}
		}
		C.free(base)
	}
	return (**C.char)(base), freeFn
}

func listAlgosCGO(typ string, plugins []string) ([]string, error) {
	tcode, err := elemTypeCode(typ)
	if err != nil {
		return nil, err
	}
	cArr, freeArr := makeCStrArray(plugins)
	defer freeArr()
	var errOut *C.char
	var js *C.char
	if cArr != nil {
		js = C.sb_list_algos_json(C.int(tcode), cArr, C.int(len(plugins)), &errOut)
	} else {
		js = C.sb_list_algos_json(C.int(tcode), nil, 0, &errOut)
	}
	if js == nil {
		defer func() {
			if errOut != nil {
				C.sb_free(errOut)
			}
		}()
		if errOut != nil {
			return nil, errors.New(C.GoString(errOut))
		}
		return nil, errors.New("unknown error")
	}
	defer C.sb_free(js)
	var names []string
	if err := json.Unmarshal([]byte(C.GoString(js)), &names); err != nil {
		return nil, err
	}
	return names, nil
}

func runCGO(req RunRequest) ([]byte, error) {
	tcode, err := elemTypeCode(req.Type)
	if err != nil {
		return nil, err
	}
	dcode, err := distCode(req.Dist)
	if err != nil {
		return nil, err
	}
	cAlgosArr, freeAlgos := makeCStrArray(req.Algos)
	defer freeAlgos()
	cPluginsArr, freePlugins := makeCStrArray(req.Plugins)
	defer freePlugins()
	var cseed C.uint64_t
	var hasSeed C.int
	if req.Seed != nil {
		cseed = C.uint64_t(*req.Seed)
		hasSeed = 1
	}
	var cbase *C.char
	var hasBase C.int
	if req.Baseline != nil && *req.Baseline != "" {
		cbase = C.CString(*req.Baseline)
		defer C.free(unsafe.Pointer(cbase))
		hasBase = 1
	}
    cfg := C.sb_core_config{
        N:                   C.uint64_t(req.N),
        dist:                C.int(dcode),
        elem_type:           C.int(tcode),
        repeats:             C.int(req.Repeats),
        warmup:              C.int(req.Warmup),
        seed:                cseed,
        has_seed:            hasSeed,
        algos:               (**C.char)(nil),
        algos_len:           C.int(len(req.Algos)),
        threads:             C.int(req.Threads),
        assert_sorted:       C.int(boolToInt(req.Assert)),
        verify:              0,
        baseline:            cbase,
        has_baseline:        hasBase,
        partial_shuffle_pct: C.int(10),
        dup_values:          C.int(100),
        zipf_s:              C.double(0),
        runs_alpha:          C.double(0),
        stagger_block:       C.int(0),
        plugin_paths:        (**C.char)(nil),
        plugin_len:          C.int(len(req.Plugins)),
    }
	if cAlgosArr != nil {
		cfg.algos = cAlgosArr
	}
    if cPluginsArr != nil {
        cfg.plugin_paths = cPluginsArr
    }
    // Override defaults with provided values when >0
    if req.PartialPct > 0 { cfg.partial_shuffle_pct = C.int(req.PartialPct) }
    if req.DupValues > 0 { cfg.dup_values = C.int(req.DupValues) }
    if req.ZipfS > 0 { cfg.zipf_s = C.double(req.ZipfS) }
    if req.RunsAlpha > 0 { cfg.runs_alpha = C.double(req.RunsAlpha) }
    if req.StaggerBlock > 0 { cfg.stagger_block = C.int(req.StaggerBlock) }
	var errOut *C.char
	out := C.sb_run_json(&cfg, 0, 1, &errOut)
	if out == nil {
		defer func() {
			if errOut != nil {
				C.sb_free(errOut)
			}
		}()
		if errOut != nil {
			return nil, errors.New(C.GoString(errOut))
		}
		return nil, errors.New("unknown error")
	}
	defer C.sb_free(out)
	s := C.GoString(out)
	return []byte(s), nil
}

func boolToInt(b bool) int {
	if b {
		return 1
	}
	return 0
}
