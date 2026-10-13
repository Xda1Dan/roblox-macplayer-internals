CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -Wall -Wextra -Iinclude
BUILD ?= build

DEMOS = reflection_demo jobqueue_demo sso_string_demo

all: $(addprefix $(BUILD)/,$(DEMOS))

$(BUILD)/%: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

# Needs Luau OSS: make LUAU_INCLUDE=/path/to/luau/VM/include luau_demo
luau_demo: src/luau_embed_demo.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(LUAU_INCLUDE) $< -o $(BUILD)/luau_demo

clean:
	rm -rf $(BUILD)

.PHONY: all clean luau_demo
