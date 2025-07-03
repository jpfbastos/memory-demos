#include <iostream>
#include <memory>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <sys/types.h>
#include <grpcpp/grpcpp.h>
#include <cuda_runtime_api.h>
#include "allocator/allocator.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using allocator::v1::Allocator;
using allocator::v1::MemoryRequest;
using allocator::v1::MemoryReply;
using allocator::v1::FreeRequest;
using allocator::v1::FreeReply;

#define CHECK(call)                                                   \
    {                                                                 \
        const cudaError_t error = call;                               \
        if (error != cudaSuccess) {                                   \
            std::cerr << "Error: " << __FILE__ << ":" << __LINE__     \
                      << ", code:" << error                           \
                      << ", reason: " << cudaGetErrorString(error)    \
                      << std::endl;                                   \
            goto cleanup;                                             \
        }                                                             \
    }

// Configuration constants
namespace Config {
    constexpr const char* DEFAULT_SERVER_ADDRESS = "localhost:50051";
    constexpr size_t DEFAULT_ALLOCATION_SIZE = 4 * 1024 * 1024;  // 4MB
    constexpr size_t SAMPLE_STEP_SIZE = 4 * 1024 * 1024;         // 4MB step for sampling
}

// Represents a locally mapped chunk of shared memory
struct LocalChunkInfo {
    std::string shm_name;
    int shm_fd;
    void* ptr;
    size_t segment_size; 

    LocalChunkInfo(const std::string& name, int fd, void* p, size_t ps)
        : shm_name(name), shm_fd(fd), ptr(p), segment_size(ps) {}

    ~LocalChunkInfo() {
        if (ptr != nullptr && ptr != MAP_FAILED) {
            munmap(ptr, segment_size);
        }
        if (shm_fd >= 0) {
            close(shm_fd);
        }
    }
};


class AllocatorClient {
public:
    explicit AllocatorClient(std::shared_ptr<Channel> channel)
        : stub_(Allocator::NewStub(channel)),
          segment_size_(Config::DEFAULT_ALLOCATION_SIZE),
          mapped_memory_(nullptr),
          total_size_(0),
          is_allocated_(false) {}

    ~AllocatorClient() {
        cleanup();
    }

    // Delete copy constructor and assignment operator
    AllocatorClient(const AllocatorClient&) = delete;
    AllocatorClient& operator=(const AllocatorClient&) = delete;

    bool allocateMemory(size_t size) {
        if (is_allocated_) {
            std::cerr << "Memory already allocated. Free existing allocation first." << std::endl;
            return false;
        }

        if (!requestMemoryFromServer(size)) {
            return false;
        }

        if (!mapSharedMemory()) {
            cleanup();
            return false;
        }

        is_allocated_ = true;
        std::cout << "Successfully allocated and mapped " << total_size_ 
                  << " bytes in " << chunks_.size() << " chunks" << std::endl;
        return true;
    }

    bool freeMemory() {
        if (!is_allocated_) {
            std::cerr << "No memory allocated to free" << std::endl;
            return false;
        }

        bool success = true;
        
        // Notify server to free memory
        if (!freeMemoryOnServer()) {
            std::cerr << "Failed to free memory on server" << std::endl;
            success = false;
        }

        // Clean up local mappings regardless of server response
        cleanup();
        
        std::cout << "Memory freed successfully" << std::endl;
        return success;
    }

    void* getDataPointer() const {
        return mapped_memory_;
    }

    size_t getTotalSize() const {
        return total_size_;
    }

    bool isAllocated() const {
        return is_allocated_;
    }

    void printMemorySample() const {
        if (!is_allocated_ || !mapped_memory_) {
            std::cout << "No memory allocated to sample" << std::endl;
            return;
        }

        const uint32_t* data = static_cast<const uint32_t*>(mapped_memory_);
        size_t total_elements = total_size_ / sizeof(uint32_t);
        size_t step = Config::SAMPLE_STEP_SIZE / sizeof(uint32_t);

        std::cout << "Memory sample (every " << Config::SAMPLE_STEP_SIZE / (1024 * 1024) 
                  << "MB):" << std::endl;
        
        for (size_t i = 0; i < total_elements; i += step) {
            std::cout << "  Offset " << i << ": " << data[i] << std::endl;
        }
    }

private:
    std::unique_ptr<Allocator::Stub> stub_;
    size_t segment_size_;
    void* mapped_memory_;
    size_t total_size_;
    std::vector<LocalChunkInfo> chunks_;
    bool is_allocated_;

    bool requestMemoryFromServer(size_t size) {
        MemoryRequest request;
        MemoryReply reply;
        ClientContext context;

        request.set_size(size);

        Status status = stub_->AllocateMemory(&context, request, &reply);
        if (!status.ok()) {
            std::cerr << "gRPC allocation request failed: " << status.error_message() << std::endl;
            return false;
        }

        if (reply.shm_names().empty() || reply.total_size() == 0) {
            std::cerr << "Server returned invalid memory allocation" << std::endl;
            return false;
        }

        segment_size_ = reply.segment_size();
        total_size_ = reply.total_size();
        
        // Store chunk information
        chunks_.clear();
        chunks_.reserve(reply.shm_names().size());
        for (const auto& shm_name : reply.shm_names()) {
            chunks_.emplace_back(
                shm_name.c_str(),
                -1,  // shm_fd will be set later
                nullptr,  // ptr will be set after mapping
                segment_size_
            );
        }
        
        return true;
    }

    bool mapSharedMemory() {

        // Reserve contiguous virtual address space
        mapped_memory_ = mmap(nullptr, total_size_, PROT_NONE, 
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped_memory_ == MAP_FAILED) {
            perror("Failed to reserve virtual address space");
            return false;
        }

        // Map each chunk into the reserved space
        if (!mapChunks()) {
            unmapMemory();
            return false;
        }

        return true;
    }

    bool mapChunks() {

        size_t virtual_offset = 0;

        for (auto& chunk_info : chunks_) {
            // Open shared memory object
            int shm_fd = shm_open(chunk_info.shm_name.c_str(), O_RDWR, 0640);
            if (shm_fd < 0) {
                perror("Failed to open shared memory object");
                return false;
            }

            chunk_info.shm_fd = shm_fd;

            void* chunk_ptr = mmap(
                static_cast<char*>(mapped_memory_) + virtual_offset,
                segment_size_,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FIXED,
                shm_fd,
                0
            );

            if (chunk_ptr == MAP_FAILED) {
                perror("Failed to map chunk");
                return false;
            }

            chunk_info.ptr = chunk_ptr;
            virtual_offset += segment_size_;
        }

        return true;
    }

    bool freeMemoryOnServer() {
        FreeRequest request;
        FreeReply reply;
        ClientContext context;

        for (const auto& chunk : chunks_) {
            request.add_shm_names(chunk.shm_name);
        }

        Status status = stub_->FreeSharedMemory(&context, request, &reply);
        return status.ok();
    }

    void unmapMemory() {
            // Clear chunks first to avoid double-free from destructors
            for (auto& chunk : chunks_) {
                if (chunk.shm_fd >= 0) {
                    close(chunk.shm_fd);
                    chunk.shm_fd = -1;
                }
                chunk.ptr = nullptr;
            }

        if (mapped_memory_ && mapped_memory_ != MAP_FAILED) {
            if (munmap(mapped_memory_, total_size_) == -1) {
                perror("munmap failed");
            }
            mapped_memory_ = nullptr;
        }
    }

    void cleanup() {
        unmapMemory();
        
        total_size_ = 0;
        chunks_.clear();
        is_allocated_ = false;
    }
};

// Global client pointer for signal handling
std::unique_ptr<AllocatorClient> g_client;

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", cleaning up..." << std::endl;
    if (g_client && g_client->isAllocated()) {
        g_client->freeMemory();
    }
    std::cout << "Exiting..." << std::endl;
    exit(0);
}

void printUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [size_in_mb]" << std::endl;
    std::cout << "Default size: " << Config::DEFAULT_ALLOCATION_SIZE / (1024 * 1024) << "MB" << std::endl;
}

size_t parseSize(const char* arg) {
    try {
        int size_mb = std::stoi(arg);
        if (size_mb <= 0) {
            std::cerr << "Size must be a positive integer." << std::endl;
            return 0;
        }
        return static_cast<size_t>(size_mb) * 1024 * 1024;
    } catch (const std::exception& e) {
        std::cerr << "Invalid size argument: " << arg << std::endl;
        return 0;
    }
}


int main(int argc, char* argv[]) {
    // Parse command line arguments
    size_t allocation_size = Config::DEFAULT_ALLOCATION_SIZE;
    if (argc > 1) {
        size_t parsed_size = parseSize(argv[1]);
        if (parsed_size == 0) {
            printUsage(argv[0]);
            return 1;
        }
        allocation_size = parsed_size;
    }

    // Set up signal handling
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    char* h_data = nullptr;
    char* d_data = nullptr;
    auto start = std::chrono::high_resolution_clock::now();
    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ms = 0;
    int return_code = 0;

    try {
        // Create client
        auto channel = grpc::CreateChannel(Config::DEFAULT_SERVER_ADDRESS, 
                                         grpc::InsecureChannelCredentials());
        g_client = std::make_unique<AllocatorClient>(channel);

        // Allocate memory
        std::cout << "Requesting " << allocation_size / (1024 * 1024) 
                  << "MB of shared memory..." << std::endl;
        
        if (!g_client->allocateMemory(allocation_size)) {
            std::cerr << "Failed to allocate shared memory" << std::endl;
            return 1;
        }

        const size_t SIZE = g_client->getTotalSize();
        h_data = static_cast<char*>(g_client->getDataPointer());

        // Access and display memory content
        const int* memory_data = static_cast<const int*>(g_client->getDataPointer());
        std::cout << "Memory content preview: ";
        for (size_t i = 0; i < std::min<size_t>(50, SIZE / sizeof(int)); ++i) {
            if (std::isprint(static_cast<char>(memory_data[i]))) {  // Add cast to char
                std::cout << static_cast<char>(memory_data[i]);
            } else {
                break;
            }
        }
        std::cout << std::endl;

        // Print memory sample
        g_client->printMemorySample();

        CHECK(cudaMalloc((void**)&d_data, SIZE));
        CHECK(cudaMemcpy(d_data, h_data, SIZE, cudaMemcpyHostToDevice));

        start = std::chrono::high_resolution_clock::now();
        CHECK(cudaMemcpy(d_data, h_data, SIZE, cudaMemcpyHostToDevice));
        CHECK(cudaDeviceSynchronize());
        end = std::chrono::high_resolution_clock::now();

        elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
        std::cout << "H2D latency: " << elapsed_ms << " ms, "
                << (SIZE / (1 << 30)) / (elapsed_ms / 1e3) << " GB/s" << std::endl;

        // Wait for user input
        std::cout << "\nPress Enter to free shared memory and exit..." << std::endl;
        std::cin.get();

        // Free memory
cleanup:
        if (d_data) cudaFree(d_data);
        std::cout << "Freeing shared memory..." << std::endl;
        g_client->freeMemory();

        std::cout << "Program completed successfully" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}