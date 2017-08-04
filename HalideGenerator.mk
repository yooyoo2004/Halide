# ------------------------------------------------------------------------------

CXX ?= error-must-define-CXX

GENERATOR_TARGET 	  ?= error-must-define-GENERATOR_TARGET

GENERATOR_BIN_DIR 	  ?= error-must-define-GENERATOR_BIN_DIR
GENERATOR_BUILD_DIR   = $(GENERATOR_BIN_DIR)/build
GENERATOR_FILTERS_DIR = $(GENERATOR_BIN_DIR)/$(GENERATOR_TARGET)/build

INCLUDE_DIR ?=
LIBHALIDE_DEPS ?= 
TEST_CXX_FLAGS ?=
TEST_LD_FLAGS ?=
TOOLS_DIR ?= $(ROOT_DIR)/tools

# General rules for building Generators. These are targeted at the
# Generators in test/generator, but can (and should) be generalized elsewhere.

# These variables are chosen to allow for Target-Specific Variable Values to 
# override part(s) of the Generator ruleset, without having to make error-prone
# near-duplicates of the build rules for things that need minor customization.
#
# Note that we need SECONDEXPANSION enabled several of these to work.
.SECONDEXPANSION:

# Default features to add to the Generator's Halide target.
GENERATOR_EXTRA_FEATURES ?=

# Default target value for Generator AOT rules.
# Only (re)define this if you need something totally custom;
# if you just need to add features, use GENERATOR_EXTRA_FEATURES.
GENERATOR_TARGET_WITH_FEATURES ?= $(GENERATOR_TARGET)-no_runtime$(if $(GENERATOR_EXTRA_FEATURES),-,)$(GENERATOR_EXTRA_FEATURES)

# Default function name for Generator AOT rules (empty = based on Generator name).
GENERATOR_FUNCNAME ?= $(notdir $*)

# Default Generator args for Generator AOT rules (if any)
GENERATOR_ARGS ?=

# Generator name to use (empty = assume only one Generator present)
GENERATOR_GENERATOR_NAME ?=

# Extra deps that are required when building the Generator itself.
GENERATOR_GENERATOR_DEPS ?=

# Extra deps that are required when linking the filter (if any).
GENERATOR_FILTER_DEPS ?=

# The Generator to use to produce a target; usually %.generator,
# but can vary when we produces multiple different filters
# from the same Generator (by changing target, generator_args, etc)
GENERATOR_GENERATOR_EXECUTABLE=$(GENERATOR_BIN_DIR)/$*.generator

GENERATOR_LD_FLAGS ?=

GENERATOR_RUNTIME_LIB = $(GENERATOR_FILTERS_DIR)/runtime.a

# ------------------------------------------------------------------------------

$(GENERATOR_BUILD_DIR)/GenGen.o: $(TOOLS_DIR)/GenGen.cpp $(INCLUDE_DIR)/Halide.h
	@mkdir -p $(@D)
	$(CXX) -c $< $(TEST_CXX_FLAGS) -I$(INCLUDE_DIR) -o $@

# ------------------------------------------------------------------------------

# By default, %.generator is produced by building %_generator.cpp & linking with GenGen.cpp
$(GENERATOR_BUILD_DIR)/%_generator.o: %_generator.cpp $(INCLUDE_DIR)/Halide.h
	@mkdir -p $(@D)
	$(CXX) $(TEST_CXX_FLAGS) -I$(INCLUDE_DIR) -I$(GENERATOR_FILTERS_DIR) -c $< -o $@

$(GENERATOR_BIN_DIR)/%.generator: $(GENERATOR_BUILD_DIR)/GenGen.o $(LIBHALIDE_DEPS) $(GENERATOR_BUILD_DIR)/%_generator.o $$(GENERATOR_GENERATOR_DEPS)
	@mkdir -p $(@D)
	$(CXX) $(filter %.cpp %.o %.a,$^) $(TEST_LD_FLAGS) -o $@

# Don't automatically delete Generators, since we may invoke the same one multiple
# times with different arguments.
# (Really, .SECONDARY is what we want, but it won't accept wildcards)
.PRECIOUS: $(GENERATOR_BIN_DIR)/%.generator

# By default, %.a/.h/.cpp are produced by executing %.generator. Runtimes are not included in these.
#
# TODO: broken because secondexpansion doesn't allow recursion, so 
# if GENERATOR_FILTER_DEPS contains a sub-filter (eg tiledblur->blur2x2)
# it won't find it. you can 'fix' this by ensuring blur2x2 is evaluated first but
# ewwww this is bad.
$(GENERATOR_FILTERS_DIR)/%.a: $$(GENERATOR_GENERATOR_EXECUTABLE) $$(GENERATOR_FILTER_DEPS)
	@mkdir -p $(@D)
	$< -e static_library,h,cpp -g "$(GENERATOR_GENERATOR_NAME)" -f "$(GENERATOR_FUNCNAME)" -n $* -o $(GENERATOR_FILTERS_DIR) target=$(GENERATOR_TARGET_WITH_FEATURES) $(GENERATOR_ARGS)
	if [ -n "$(GENERATOR_FILTER_DEPS)" ]; then $(TOOLS_DIR)/halide_libtool.sh $@ $@ $(GENERATOR_FILTER_DEPS); fi

$(GENERATOR_FILTERS_DIR)/%.h: $(GENERATOR_FILTERS_DIR)/%.a
	@ # @echo $@ produced implicitly by $^

$(GENERATOR_FILTERS_DIR)/%.cpp: $(GENERATOR_FILTERS_DIR)/%.a
	@ # @echo $@ produced implicitly by $^

$(GENERATOR_FILTERS_DIR)/%.stub.h: $(GENERATOR_BIN_DIR)/%.generator
	@mkdir -p $(@D)
	$< -n $* -o $(GENERATOR_FILTERS_DIR) -e cpp_stub

# ------------------------------------------------------------------------------
# Make an empty generator for generating runtimes.
$(GENERATOR_BIN_DIR)/runtime.rgenerator: $(GENERATOR_BUILD_DIR)/GenGen.o $(LIBHALIDE_DEPS)
	@mkdir -p $(@D)
	$(CXX) $(filter-out %.h,$^) $(TEST_LD_FLAGS) -o $@

# Generate a standalone runtime for a given target string
# Note that this goes into GENERATOR_FILTERS_DIR, but we define the rule
# this way so that things downstream can just depend on the right path to
# force generation of custom runtimes.
$(GENERATOR_BIN_DIR)/%/build/runtime.a: $(GENERATOR_BIN_DIR)/runtime.rgenerator
	@mkdir -p $(@D)
	$< -r runtime -o $(@D) target=$*

# ------------------------------------------------------------------------------

# Rules for the "RunGen" utility, which lets most Generators run via
# standard command-line tools.

$(GENERATOR_BUILD_DIR)/RunGen.o: $(TOOLS_DIR)/RunGen.cpp $(RUNTIME_EXPORTED_INCLUDES)
	@mkdir -p $(@D)
	$(CXX) -c $< $(TEST_CXX_FLAGS) $(IMAGE_IO_CXX_FLAGS) -I$(INCLUDE_DIR) -I $(SRC_DIR)/runtime -I$(TOOLS_DIR) -o $@

$(GENERATOR_BIN_DIR)/%.rungen: $(GENERATOR_BUILD_DIR)/RunGen.o $(GENERATOR_FILTERS_DIR)/runtime.a $(TOOLS_DIR)/RunGenStubs.cpp $(GENERATOR_FILTERS_DIR)/%.a
	$(CXX) -std=c++11 -DHL_RUNGEN_FILTER_HEADER=\"$*.h\" -I$(GENERATOR_FILTERS_DIR) $^ $(GENERATOR_LD_FLAGS) $(IMAGE_IO_LIBS) -o $@

# Don't automatically delete RunGen, since we may invoke the same one multiple times with different arguments.
# (Really, .SECONDARY is what we want, but it won't accept wildcards)
.PRECIOUS: $(GENERATOR_BIN_DIR)/%.rungen

# Allow either lowercase or UPPERCASE
runargs ?=
RUNARGS ?=

$(GENERATOR_BIN_DIR)/%.run: $(GENERATOR_BIN_DIR)/%.rungen
	$< $(RUNARGS) $(runargs)

clean_generators:
	rm -rf $(GENERATOR_BIN_DIR)/*.generator
	rm -rf $(GENERATOR_BIN_DIR)/*/runtime.a
	rm -rf $(GENERATOR_BIN_DIR)/*/build/runtime.a
	rm -rf $(GENERATOR_FILTERS_DIR)
	rm -rf $(GENERATOR_BUILD_DIR)/*_generator.o
	rm -f $(GENERATOR_BUILD_DIR)/GenGen.o
	rm -f $(GENERATOR_BUILD_DIR)/RunGen.o
	rm -rf $(GENERATOR_BIN_DIR)/$(GENERATOR_TARGET)/generator_*
