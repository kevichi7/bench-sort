#define main CppTest25orig6_main_disabled
#include "../CppTest25orig6.cpp"
#undef main

// Include the older variant (orig5) in a distinct namespace to avoid symbol collisions,
// and provide a wrapper entrypoint hybrid_sort_auto_v2.
namespace customv2_shim {
#define main CppTest25orig5_main_disabled
#include "../CppTest25orig5.cpp"
#undef main
} // namespace customv2_shim

// Wrapper with a unique name used by the harness
void hybrid_sort_auto_v2(int* arr, int n) {
    customv2_shim::hybrid_sort_auto(arr, n);
}
