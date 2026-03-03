# Userspace Application Makefile

CXX := g++
CXXFLAGS := -std=c++11 -Wall -O2 -I../common
LDFLAGS := -lpthread

TARGET := zerocopy_gateway

SRCS := main.cpp netlink_client.cpp dma_mapper.cpp epoll_server.cpp
OBJS := $(SRCS:.cpp=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $(TARGET) $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
	@echo "Clean complete"

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	@echo "Installed to /usr/local/bin/$(TARGET)"

help:
	@echo "Userspace Application Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build the application"
	@echo "  clean   - Clean build files"
	@echo "  install - Install to /usr/local/bin"
	@echo "  help    - Show this help"
