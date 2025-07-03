#include <iostream>
#include <memory>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <signal.h>
#include <grpcpp/grpcpp.h>
#include "allocator/allocator.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using allocator::v1::Allocator;
using allocator::v1::MemoryRequest;
using allocator::v1::MemoryReply;
using allocator::v1::FreeRequest;
using allocator::v1::FreeReply;
using allocator::v1::ChunkInfo;

// Configuration constants
namespace Config {
    constexpr const char* DEFAULT_SERVER_ADDRESS = "localhost:50051";
    constexpr size_t DEFAULT_ALLOCATION_SIZE = 4 * 1024 * 1024;  // 4MB
    constexpr size_t SAMPLE_STEP_SIZE = 4 * 1024 * 1024;         // 4MB step for sampling
}

// Represents a locally mapped chunk of shared memory
struct LocalChunkInfo {
    void* ptr;
    size_t size;
    off_t offset;
    
    LocalChunkInfo(void* p, size_t s, off_t o) : ptr(p), size(s), offset(o) {}
};

class AllocatorClient {
public:
    explicit AllocatorClient(std::shared_ptr<Channel> channel)
        : stub_(Allocator::NewStub(channel)),
          shm_fd_(-1),
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
    int shm_fd_;
    void* mapped_memory_;
    size_t total_size_;
    std::string shm_name_;
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

        if (reply.chunks().empty() || reply.total_size() == 0) {
            std::cerr << "Server returned invalid memory allocation" << std::endl;
            return false;
        }

        shm_name_ = reply.shm_name();
        total_size_ = reply.total_size();
        
        // Store chunk information
        chunks_.clear();
        chunks_.reserve(reply.chunks().size());
        for (const auto& chunk : reply.chunks()) {
            chunks_.emplace_back(nullptr, chunk.size(), chunk.offset());
        }
        
        return true;
    }

    bool mapSharedMemory() {
        // Open shared memory segment
        shm_fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0640);
        if (shm_fd_ == -1) {
            perror("shm_open failed");
            return false;
        }

        // Reserve contiguous virtual address space
        mapped_memory_ = mmap(nullptr, total_size_, PROT_NONE, 
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped_memory_ == MAP_FAILED) {
            perror("Failed to reserve virtual address space");
            close(shm_fd_);
            shm_fd_ = -1;
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
        
        for (const auto& chunk_info : chunks_) {
            void* chunk_ptr = mmap(
                static_cast<char*>(mapped_memory_) + virtual_offset,
                chunk_info.size,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FIXED,
                shm_fd_,
                chunk_info.offset
            );

            if (chunk_ptr == MAP_FAILED) {
                perror("Failed to map chunk");
                return false;
            }

            virtual_offset += chunk_info.size;
        }


        return true;
    }

    bool freeMemoryOnServer() {
        FreeRequest request;
        FreeReply reply;
        ClientContext context;

        for (const auto& chunk : chunks_) {
            request.add_offset(chunk.offset);
        }

        Status status = stub_->FreeSharedMemory(&context, request, &reply);
        return status.ok();
    }

    void unmapMemory() {
        if (mapped_memory_ && mapped_memory_ != MAP_FAILED) {
            if (munmap(mapped_memory_, total_size_) == -1) {
                perror("munmap failed");
            }
            mapped_memory_ = nullptr;
        }

        if (shm_fd_ >= 0) {
            close(shm_fd_);
            shm_fd_ = -1;
        }
    }

    void cleanup() {
        unmapMemory();
        
        total_size_ = 0;
        shm_name_.clear();
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

int parseSize(const char* arg) {
    try {
        int size_mb = std::stoi(arg);
        if (size_mb <= 0) {
            std::cerr << "Size must be positive" << std::endl;
            return -1;
        }
        return size_mb * 1024 * 1024;
    } catch (const std::exception& e) {
        std::cerr << "Invalid size argument: " << arg << std::endl;
        return -1;
    }
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    size_t allocation_size = Config::DEFAULT_ALLOCATION_SIZE;
    if (argc > 1) {
        int parsed_size = parseSize(argv[1]);
        if (parsed_size < 0) {
            printUsage(argv[0]);
            return 1;
        }
        allocation_size = parsed_size;
    }

    // Set up signal handling
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

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

        // Access and display memory content
        const char* memory_data = static_cast<const char*>(g_client->getDataPointer());
        std::cout << "Memory content preview: ";
        for (int i = 0; i < std::min(50, static_cast<int>(g_client->getTotalSize())); ++i) {
            if (std::isprint(memory_data[i])) {
                std::cout << memory_data[i];
            } else {
                break;
            }
        }
        std::cout << std::endl;

        // Print memory sample
        g_client->printMemorySample();

        // Wait for user input
        std::cout << "\nPress Enter to free shared memory and exit..." << std::endl;
        std::cin.get();

        // Free memory
        std::cout << "Freeing shared memory..." << std::endl;
        g_client->freeMemory();

        std::cout << "Program completed successfully" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}