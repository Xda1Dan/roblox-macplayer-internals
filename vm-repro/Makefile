# rbxvm — reproduction of RobloxPlayer (macOS, 0.742.0.7421053) Luau VM internals
#
# Targets:
#   make            build tools + tests
#   make test       build and run the verification suite
#   make demo       run a wire->canonical disassembly demo
#   make clean
CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Iinclude
AR       ?= ar

LIB_SRCS := src/tables.cpp src/disasm.cpp src/rsb1.cpp src/zstd_loader.cpp src/scriptctx.cpp src/aead.cpp src/lbc.cpp
LIB_OBJS := $(LIB_SRCS:.cpp=.o)

all: lib/librbxvm.a tools/rbxdis tools/rbxrsb1 tests/rbxvm-tests tests/rbxrsb1-tests tests/rbxctx-tests tests/rbxaead-tests

lib/librbxvm.a: $(LIB_OBJS)
	@mkdir -p lib
	$(AR) rcs $@ $(LIB_OBJS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

tools/rbxdis: tools/rbxdis.cpp lib/librbxvm.a
	$(CXX) $(CXXFLAGS) $< lib/librbxvm.a -o $@ -ldl

tools/rbxrsb1: tools/rbxrsb1.cpp lib/librbxvm.a
	$(CXX) $(CXXFLAGS) $< lib/librbxvm.a -o $@ -ldl

tests/rbxvm-tests: tests/test_core.cpp lib/librbxvm.a
	$(CXX) $(CXXFLAGS) -Itests $< lib/librbxvm.a -o $@ -ldl

tests/rbxrsb1-tests: tests/test_rsb1.cpp lib/librbxvm.a
	$(CXX) $(CXXFLAGS) -Itests $< lib/librbxvm.a -o $@ -ldl

tests/rbxctx-tests: tests/test_scriptctx.cpp lib/librbxvm.a
	$(CXX) $(CXXFLAGS) -Itests $< lib/librbxvm.a -o $@ -ldl

tests/rbxaead-tests: tests/test_aead.cpp lib/librbxvm.a
	$(CXX) $(CXXFLAGS) -Itests $< lib/librbxvm.a -o $@ -ldl

tests/rbxlbc-tests: tests/test_lbc.cpp lib/librbxvm.a
	$(CXX) $(CXXFLAGS) -Itests $< lib/librbxvm.a -o $@ -ldl

test: tests/rbxvm-tests tests/rbxrsb1-tests tests/rbxctx-tests tests/rbxaead-tests tests/rbxlbc-tests
	./tests/rbxvm-tests
	./tests/rbxrsb1-tests
	./tests/rbxctx-tests
	./tests/rbxaead-tests
	./tests/rbxlbc-tests

demo: tools/rbxdis
	@echo '--- canonical program, wire-encoded (what ships on disk) ---'
	@# PREPVARARGS 1 | LOADNIL R0 | LOADN R1,42 | ADD R2,R0,R1 | GETIMPORT R3,K5 [K2.K1.K1] | JUMPX ->8 | JUMPX ->4 | RETURN R2,1
	./tools/rbxdis code --openc=wire --hex="a3010000 c6000000 8c012a00 43020001 a4030500 010420c0 69010000 69fcffff 82020200"

clean:
	rm -f $(LIB_OBJS) lib/librbxvm.a tools/rbxdis tools/rbxrsb1 tests/rbxvm-tests tests/rbxrsb1-tests tests/rbxctx-tests tests/rbxaead-tests tests/rbxlbc-tests

.PHONY: all test demo clean
