include ../support/Makefile.inc

GENERATOR_BIN_DIR = $(BIN)
GENERATOR_TARGET = $(HL_TARGET)

TEST_CXX_FLAGS = $(CXXFLAGS)
TEST_LD_FLAGS = $(LDFLAGS) $(HALIDE_SYSTEM_LDFLAGS)
INCLUDE_DIR = $(HALIDE_SRC_PATH)/include
TOOLS_DIR = $(HALIDE_SRC_PATH)/tools
include ../../HalideGenerator.mk

all: $(BIN)/filter $(BIN)/out.png $(BIN)/filter_viz

# Same Generator, different features
$(GENERATOR_FILTERS_DIR)/bilateral_grid_viz.a: GENERATOR_GENERATOR_EXECUTABLE=$(GENERATOR_BIN_DIR)/bilateral_grid.generator
$(GENERATOR_FILTERS_DIR)/bilateral_grid_viz.a: GENERATOR_FEATURES=trace_loads-trace_stores-trace_realizations
$(GENERATOR_FILTERS_DIR)/bilateral_grid_viz.a: GENERATOR_FUNCNAME=bilateral_grid

$(BIN)/filter: filter.cpp $(GENERATOR_FILTERS_DIR)/bilateral_grid.a $(GENERATOR_RUNTIME_LIB)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -O3 -ffast-math -Wall -Werror -I$(GENERATOR_FILTERS_DIR) $^ -o $@ $(IMAGE_IO_FLAGS) $(LDFLAGS)

$(BIN)/filter_viz: filter.cpp $(GENERATOR_FILTERS_DIR)/bilateral_grid_viz.a $(GENERATOR_RUNTIME_LIB)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -O3 -ffast-math -Wall -Werror -I$(GENERATOR_FILTERS_DIR) $^ -o $@ $(IMAGE_IO_FLAGS) $(LDFLAGS)

$(BIN)/bilateral_grid.mp4: $(BIN)/filter_viz viz.sh
	@mkdir -p $(@D)
	bash viz.sh $(BIN)

$(BIN)/out.png: $(BIN)/filter
	@mkdir -p $(@D)
	$(BIN)/filter $(IMAGES)/gray.png $(BIN)/out.png 0.1 10

clean:
	rm -rf $(BIN)
