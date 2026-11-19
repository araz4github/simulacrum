# Host-side DSP tests for Simulacrum:  make -C tests
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
SRC = host_test.cpp ../src/engine.cpp ../src/tape.cpp ../src/grain.cpp ../src/stack.cpp

host_test: $(SRC) $(wildcard ../src/*.h)
	$(CXX) $(CXXFLAGS) -Istubs -I../src $(SRC) -o $@

run: host_test
	./host_test

clean:
	rm -f host_test
.PHONY: run clean
