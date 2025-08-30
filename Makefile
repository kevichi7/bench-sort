TARGET := sortbench
CXX := g++
CXXFLAGS := -O3 -march=native -pipe -std=c++20 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -fopenmp
LDFLAGS := -fopenmp -ltbb -ldl

SRC := sortbench.cpp
OBJ := $(SRC:.cpp=.o)

# Core library (phase 1) — header-only public API + single core TU
CORE_INC := include
CORE_SRC := src/sortbench_core.cpp src/sortbench_format.cpp src/sortbench_capi.cpp
CORE_OBJ := $(CORE_SRC:.cpp=.o)
CORE_LIB := libsortbench_core.a

.PHONY: all clean run

all: $(TARGET) $(CORE_LIB)

$(TARGET): $(OBJ) $(CORE_LIB)
	$(CXX) $(CXXFLAGS) -DSORTBENCH_CXX='"$(CXX)"' -DSORTBENCH_CXXFLAGS='"$(CXXFLAGS)"' -DSORTBENCH_LDFLAGS='"$(LDFLAGS)"' -o $@ $(OBJ) $(CORE_LIB) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I$(CORE_INC) -DSORTBENCH_CXX='"$(CXX)"' -DSORTBENCH_CXXFLAGS='"$(CXXFLAGS)"' -DSORTBENCH_LDFLAGS='"$(LDFLAGS)"' -c -o $@ $<



$(CORE_LIB): $(CORE_OBJ)
	ar rcs $@ $^

run: $(TARGET)
	./$(TARGET)

# Tests
.PHONY: test
TEST_BIN := core_tests
TEST_SRC := tests/core_tests.cpp
$(TEST_BIN): $(CORE_LIB) $(TEST_SRC)
	$(CXX) $(CXXFLAGS) -I$(CORE_INC) -o $@ $(TEST_SRC) $(CORE_LIB) $(LDFLAGS)

test: $(TEST_BIN)
	./$(TEST_BIN)

# Go API helpers
.PHONY: api-go api-go-cgo
api-go:
	cd api/go && go mod tidy && go build .

api-go-cgo:
	cd api/go && SORTBENCH_CGO=1 go mod tidy && go build .

PLUGIN_DIR := plugins
PLUGIN_EXAMPLE := $(PLUGIN_DIR)/example_plugin.so
PLUGIN_CUSTOM := $(PLUGIN_DIR)/custom_cpp25.so

.PHONY: plugins example-plugin custom-plugin
plugins: $(PLUGIN_EXAMPLE) $(PLUGIN_CUSTOM)

$(PLUGIN_DIR)/example_plugin.so: $(PLUGIN_DIR)/example_plugin.cpp sortbench_plugin.h
	$(CXX) $(CXXFLAGS) -fPIC -shared -o $@ $(PLUGIN_DIR)/example_plugin.cpp $(LDFLAGS)

$(PLUGIN_DIR)/custom_cpp25.so: $(PLUGIN_DIR)/custom_cpp25.cpp sortbench_plugin.h ../CppTest25orig5.cpp ../BenchStats.h ../radix11_omp.hpp
	$(CXX) $(CXXFLAGS) -fPIC -shared -o $@ $(PLUGIN_DIR)/custom_cpp25.cpp $(LDFLAGS)

clean:
	rm -f $(OBJ) $(TARGET) $(CORE_OBJ) $(CORE_LIB)

# Optional: include custom shim when available and explicitly enabled
# Usage: make ENABLE_CUSTOM_SHIM=1
ifneq ($(ENABLE_CUSTOM_SHIM),)
CORE_SRC += custom_algo_shim.cpp
CORE_OBJ += custom_algo_shim.o
CXXFLAGS += -DSORTBENCH_HAS_CUSTOM_SHIM=1
endif
