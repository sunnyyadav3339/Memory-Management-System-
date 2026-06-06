CXX      = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -O2 -Iinclude
TARGET   = vm_simulator
SRC      = src/main.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
