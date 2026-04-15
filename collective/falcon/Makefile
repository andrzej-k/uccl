ABS_ROOT_PATH := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
ABS_REPO_ROOT := $(shell realpath $(ABS_ROOT_PATH)/../..)

NCCL_HOME:=$(ABS_REPO_ROOT)/thirdparty/nccl
CUDA_HOME?=/usr/local/cuda

INC = -I./ -I$(CUDA_HOME)/include -I$(ABS_REPO_ROOT)/include
LIBS = -libverbs -L $(CUDA_HOME)/lib64 -lcudart -lpthread
LIBS_SHARED = -libverbs
# Added -MMD -MP for automatic dependency generation
override CXXFLAGS += -O3 -g -std=c++17 -Wno-pointer-arith -Wno-interference-size -fPIC -DUSE_CUDA -MMD -MP

PLUGIN_SO = libnccl-net-falcon.so
NCCL_INC:= -I$(NCCL_HOME)/build/include -I$(NCCL_HOME)/src/include -I$(CUDA_HOME)/include

lib_src = $(wildcard *.cc)
lib_src := $(filter-out %_main.cc,$(lib_src))
lib_src := $(filter-out %_test.cc,$(lib_src))
lib_src := $(filter-out %_plugin.cc,$(lib_src))
lib_obj = $(lib_src:.cc=.o)

# Include automatically generated dependency files
-include $(lib_obj:.o=.d)

.PHONY: build
build: $(lib_obj) $(PLUGIN_SO)

%.o: %.cc
	g++ -c $< -o $@ $(INC) $(CXXFLAGS)

$(PLUGIN_SO): nccl_plugin.cc $(lib_obj)
	g++ $(NCCL_INC) -fPIC -shared -o $@ -Wl,-soname,$(PLUGIN_SO) nccl_plugin.cc $(lib_obj) $(INC) $(LIBS_SHARED) $(CXXFLAGS)

.PHONY: clean
clean:
	rm -f *.o *.d $(PLUGIN_SO)
