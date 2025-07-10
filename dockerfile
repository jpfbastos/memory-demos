FROM nvidia/cuda:12.4.1-devel-ubuntu22.04

# Install required packages
RUN apt-get update && apt-get install -y \
    build-essential \
    htop \
    procps \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR ~/memory-demos

# Copy source files
COPY demo3a_client.cpp .
COPY demo3a_server.cpp .

# Create a Makefile for easy compilation using nvcc
RUN echo 'all: writer reader\n\
\n\
writer: demo3a_server.cpp\n\
\tnvcc -std=c++17 -O2 -o writer demo3a_server.cpp -lrt\n\
\n\
reader: demo3a_client.cpp\n\
\tnvcc -std=c++17 -O2 -o reader demo3a_client.cpp -lrt\n\
\n\
clean:\n\
\trm -f writer reader\n\
\n\
.PHONY: all clean' > Makefile

# Compile the programs
RUN make

# Create test script
RUN echo '#!/bin/bash\n\
echo "=== System Memory Info ==="\n\
free -h\n\
echo "\n=== Starting memory test ==="\n\
echo "Container memory limit should be ~10GB"\n\
echo "Shared memory size: 8GB (reduced for testing)"\n\
echo "\nStarting writer in background..."\n\
./writer &\n\
WRITER_PID=$!\n\
sleep 10\n\
echo "\n=== Memory usage with writer running ==="\n\
free -h\n\
echo "\n=== Process memory usage ==="\n\
ps aux --sort=-%mem | head -10\n\
echo "\nStarting reader..."\n\
./reader\n\
echo "\n=== Final memory usage ==="\n\
free -h\n\
kill $WRITER_PID 2>/dev/null || true\n\
wait $WRITER_PID 2>/dev/null || true\n\
echo "Test completed"' > test_memory.sh

RUN chmod +x test_memory.sh

# Default command
CMD ["./test_memory.sh"]