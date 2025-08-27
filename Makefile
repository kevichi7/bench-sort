TARGET := sortbench
CXX := g++
CXXFLAGS := -O3 -march=native -pipe -std=c++20 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -fopenmp
LDFLAGS := -fopenmp -ltbb -ldl

SRC := sortbench.cpp custom_algo_shim.cpp
OBJ := $(SRC:.cpp=.o)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -DSORTBENCH_CXX='"$(CXX)"' -DSORTBENCH_CXXFLAGS='"$(CXXFLAGS)"' -DSORTBENCH_LDFLAGS='"$(LDFLAGS)"' -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -DSORTBENCH_CXX='"$(CXX)"' -DSORTBENCH_CXXFLAGS='"$(CXXFLAGS)"' -DSORTBENCH_LDFLAGS='"$(LDFLAGS)"' -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

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
	rm -f $(OBJ) $(TARGET)
