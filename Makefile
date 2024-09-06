# for out-of-tree build support
SRC_DIR := $(dir $(firstword $(MAKEFILE_LIST)))
VPATH := $(SRC_DIR)
CMAKE := cmake
YOSYS_CONFIG := $(YOSYS_PREFIX)yosys-config
SRCS = $(wildcard $(SRC_DIR)/src/*.cc)
OBJS = $(patsubst $(SRC_DIR)/src/%.cc,build/%.o,$(SRCS))

build: build/slang.so

configure-slang:
	@mkdir -p $(@D)
	@if [ ! -f "$(SRC_DIR)/third_party/slang/CMakeLists.txt" ]; then \
		echo "The content of the slang submodule seems to be missing."; \
		echo "Initialize the submodule with"; \
		echo ""; \
		echo "  git submodule init"; \
		echo "  git submodule update third_party/slang"; \
		echo ""; \
		exit 1; \
	fi
	$(CMAKE) -S $(SRC_DIR)/third_party/slang -B build/slang \
		-DCMAKE_INSTALL_PREFIX=build/slang_install \
		-DSLANG_INCLUDE_TESTS=OFF \
		-DSLANG_INCLUDE_TOOLS=OFF \
		-DCMAKE_BUILD_TYPE=Release \
		-DSLANG_USE_MIMALLOC=OFF \
		-DCMAKE_CXX_FLAGS="-fPIC" \
		-DCMAKE_DISABLE_FIND_PACKAGE_Boost=ON \
		-DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON

build/slang/.configured:
	$(MAKE) configure-slang
	touch $@

build-slang: build/slang/.configured
	$(MAKE) -C $(dir $^)
	$(MAKE) -C $(dir $^) install
	touch build/slang_install/.built

build/slang_install/.built:
	$(MAKE) build-slang

clean-slang:
	rm -rf build/slang build/slang_install

clean-objects:
	rm -f $(OBJS)

clean: clean-objects
	rm -f build/slang.so

clean-all: clean clean-slang

-include $(OBJS:.o=.d)
build/%.o: src/%.cc build/slang_install/.built
	@mkdir -p $(@D)
	@echo "    CXX $@"
	@$(YOSYS_CONFIG) --exec --cxx --cxxflags -O3 -g -I . -MD \
		 -c -o $@ $< -std=c++20 \
		 -DSLANG_BOOST_SINGLE_HEADER \
		 -Ibuild/slang_install/include

build/slang.so: $(OBJS)
	@mkdir -p $(@D)
	@echo "   LINK $@"
	@$(YOSYS_CONFIG) --exec --cxx --cxxflags --ldflags -g -o $@ \
		-shared $^ --ldlibs \
		-Lbuild/slang_install/lib \
		-Lbuild/slang_install/lib64 \
		-lsvlang -lfmt

.PHONY: build configure-slang build-slang clean-slang clean-objects clean clean-all
