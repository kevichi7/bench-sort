#pragma once
#include <vector>
#include <cstdint>
#include <cstring>

// Forward declarations provided by the shim-included sources
void hybrid_sort_auto(int *arr, int n);      // from CppTest25orig6.cpp
void hybrid_sort_auto_v2(int *arr, int n);   // from CppTest25orig5.cpp (wrapped)

namespace custom_algo {
inline void sort_int(std::vector<int> &v) {
    if (!v.empty()) hybrid_sort_auto(v.data(), static_cast<int>(v.size()));
}

inline void sort_int_v2(std::vector<int> &v) {
    if (!v.empty()) hybrid_sort_auto_v2(v.data(), static_cast<int>(v.size()));
}

// Order-preserving float<->int key transform using signed-int sortable keys.
// We map float bits (IEEE754) to unsigned order-preserving keys, then XOR
// 0x8000'0000 to obtain signed-sortable keys; sort with int sorter; invert.
inline void sort_float(std::vector<float> &v) {
    const int n = static_cast<int>(v.size());
    if (n <= 1) return;
    std::vector<int> keys;
    keys.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        uint32_t u;
        static_assert(sizeof(float) == sizeof(uint32_t), "float must be 32-bit");
        std::memcpy(&u, &v[static_cast<std::size_t>(i)], sizeof(u));
        uint32_t ukey = (u & 0x80000000u) ? (~u) : (u ^ 0x80000000u);
        uint32_t skey = ukey ^ 0x80000000u; // signed-sortable key
        keys[static_cast<std::size_t>(i)] = static_cast<int>(skey);
    }
    if (!keys.empty()) hybrid_sort_auto(keys.data(), n);
    for (int i = 0; i < n; ++i) {
        uint32_t skey = static_cast<uint32_t>(keys[static_cast<std::size_t>(i)]);
        uint32_t ukey = skey ^ 0x80000000u;
        uint32_t u = (ukey & 0x80000000u) ? (ukey ^ 0x80000000u) : (~ukey);
        float f;
        std::memcpy(&f, &u, sizeof(u));
        v[static_cast<std::size_t>(i)] = f;
    }
}

inline void sort_float_v2(std::vector<float> &v) {
    const int n = static_cast<int>(v.size());
    if (n <= 1) return;
    std::vector<int> keys;
    keys.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        uint32_t u;
        static_assert(sizeof(float) == sizeof(uint32_t), "float must be 32-bit");
        std::memcpy(&u, &v[static_cast<std::size_t>(i)], sizeof(u));
        uint32_t ukey = (u & 0x80000000u) ? (~u) : (u ^ 0x80000000u);
        uint32_t skey = ukey ^ 0x80000000u;
        keys[static_cast<std::size_t>(i)] = static_cast<int>(skey);
    }
    if (!keys.empty()) hybrid_sort_auto_v2(keys.data(), n);
    for (int i = 0; i < n; ++i) {
        uint32_t skey = static_cast<uint32_t>(keys[static_cast<std::size_t>(i)]);
        uint32_t ukey = skey ^ 0x80000000u;
        uint32_t u = (ukey & 0x80000000u) ? (ukey ^ 0x80000000u) : (~ukey);
        float f;
        std::memcpy(&f, &u, sizeof(u));
        v[static_cast<std::size_t>(i)] = f;
    }
}
}
