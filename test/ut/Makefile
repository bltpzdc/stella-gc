CXX      ?= g++
CXXFLAGS ?= -std=c++23 -O1 -g -I../../include -I../../3dparty -DFMT_HEADER_ONLY

TESTS = root context heap config
BINS  = $(TESTS:%=%.test)

.PHONY: all check clean

all: $(BINS)

%.test: %.cpp test_util.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

check: all
	@fail=0; \
	for t in $(BINS); do \
		echo "== $$t =="; \
		./$$t || fail=1; \
	done; \
	exit $$fail

clean:
	rm -f $(BINS)