CXX ?= c++
CPPFLAGS := -Iinclude -Ithird_party
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
LDLIBS := -pthread

CORE := src/BatchQueue.cpp src/Checksum.cpp src/DeadlockDetector.cpp
HEADERS := $(wildcard include/*.hpp)

.PHONY: all test test-deadlock clean
all: buildqueue

buildqueue: src/main.cpp $(CORE) $(HEADERS) third_party/httplib.h third_party/json.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(filter %.cpp,$^) $(LDFLAGS) $(LDLIBS) -o $@

.build/live_tests: tests/test_live.cpp $(CORE) $(HEADERS)
	mkdir -p .build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(filter %.cpp,$^) $(LDFLAGS) $(LDLIBS) -o $@

.build/deadlock_test: tests/test_deadlock.cpp src/DeadlockDetector.cpp src/DeadlockDemo.cpp $(HEADERS)
	mkdir -p .build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(filter %.cpp,$^) $(LDFLAGS) $(LDLIBS) -o $@

test-deadlock: .build/deadlock_test
	timeout 10s ./.build/deadlock_test demo-files/deadlock

test: buildqueue .build/live_tests test-deadlock
	./.build/live_tests
	python3 tests/test_http.py ./buildqueue web

clean:
	$(RM) buildqueue .build/live_tests .build/deadlock_test
