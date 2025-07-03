#include <iostream>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>
#include <cstring>
#include <signal.h>
#include <vector>
#include <string>
#include <dirent.h>
#include <sys/types.h>
#include <fstream>
#include <unordered_map>
#include <sys/stat.h>
#include <uuid/uuid.h>
#include <grpcpp/grpcpp.h>
#include <cuda_runtime_api.h>
#include "allocator/allocator.grpc.pb.h"

using grpc::Status;
using grpc::ServerContext;
using allocator::v1::Allocator;
using allocator::v1::MemoryRequest;
using allocator::v1::MemoryReply;
using allocator::v1::FreeRequest;
using allocator::v1::FreeReply;
using allocator::v1::ChunkInfo;

// Configuration constants
namespace Config {
    constexpr size_t PAGE_SIZE = 4 * 1024 * 1024;     // 4 MB Pages
    constexpr size_t POOL_SIZE = 100 * 1024 * 1024;   // 100 MB pool
    constexpr const char* POOL_SHM_NAME = "/memory_pool_main";
    constexpr const char* SERVER_ADDRESS = "0.0.0.0:50051";
    constexpr int SLEEP_INTERVAL_MS = 200;
}

// Memory block structure
struct MemoryBlock {
    size_t offset;
    size_t size;
    
    MemoryBlock(size_t off, size_t sz) : offset(off), size(sz) {}
    
    bool operator<(const MemoryBlock& other) const {
        return offset < other.offset;
    }
};

class SharedMemoryPool {
private:
    char* pool_base_;
    int pool_fd_;
    bool initialized_;

    public:
    SharedMemoryPool() : pool_base_(nullptr), pool_fd_(-1), initialized_(false) {}
    
    ~SharedMemoryPool() {
        cleanup();
    }
    
    // Delete copy constructor and assignment operator
    SharedMemoryPool(const SharedMemoryPool&) = delete;
    SharedMemoryPool& operator=(const SharedMemoryPool&) = delete;
    
    bool initialize() {
        if (initialized_) {
            std::cerr << "Memory pool already initialized" << std::endl;
            return false;
        }
        
        std::cout << "Initializing memory pool..." << std::endl;
        
        pool_fd_ = shm_open(Config::POOL_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0640);
        if (pool_fd_ == -1) {
            perror("shm_open failed");
            return false;
        }
        
        if (ftruncate(pool_fd_, Config::POOL_SIZE) == -1) {
            perror("ftruncate failed");
            cleanup();
            return false;
        }
        
        pool_base_ = static_cast<char*>(mmap(nullptr, Config::POOL_SIZE, 
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED | MAP_LOCKED, pool_fd_, 0));
        if (pool_base_ == MAP_FAILED) {
            perror("mmap failed");
            cleanup();
            return false;
        }
        
        cudaError_t cuda_err = cudaHostRegister(pool_base_, Config::POOL_SIZE, 
                                               cudaHostRegisterDefault);
        if (cuda_err != cudaSuccess) {
            std::cerr << "cudaHostRegister failed: " << cudaGetErrorString(cuda_err) << std::endl;
            cleanup();
            return false;
        }
        
        // Initialize memory with test pattern
        initializeMemoryPattern();
        
        initialized_ = true;
        std::cout << "Memory pool initialized successfully at address " 
                  << static_cast<void*>(pool_base_) << std::endl;
        
        return true;
    }
    
    void cleanup() {
        if (!initialized_) return;
        
        std::cout << "Cleaning up shared memory..." << std::endl;
        
        if (pool_base_ && pool_base_ != MAP_FAILED) {
            cudaError_t cuda_err = cudaHostUnregister(pool_base_);
            if (cuda_err != cudaSuccess) {
                std::cerr << "cudaHostUnregister failed: " 
                          << cudaGetErrorString(cuda_err) << std::endl;
            }
            
            if (munmap(pool_base_, Config::POOL_SIZE) == -1) {
                perror("munmap failed");
            }
            pool_base_ = nullptr;
        }
        
        if (pool_fd_ >= 0) {
            close(pool_fd_);
            pool_fd_ = -1;
        }
        
        if (shm_unlink(Config::POOL_SHM_NAME) == -1) {
            perror("shm_unlink failed");
        }
        
        initialized_ = false;
        std::cout << "Shared memory cleanup completed" << std::endl;
    }
    
    char* getBase() const { return pool_base_; }
    bool isInitialized() const { return initialized_; }
    
private:
    void initializeMemoryPattern() {
        uint32_t* ptr = reinterpret_cast<uint32_t*>(pool_base_);
        size_t count = Config::POOL_SIZE / sizeof(uint32_t);
        
        for (size_t i = 0; i < count; ++i) {
            ptr[i] = static_cast<uint32_t>(i);
        }
    }
};


// Memory allocator class
class MemoryAllocator {
private:
    std::vector<MemoryBlock> free_blocks_;
    std::unordered_map<size_t, size_t> allocation_map_;
    size_t occupied_memory_;
    mutable std::mutex mutex_;
    
public:
    MemoryAllocator() : occupied_memory_(0) {
        // Initialize with one free block covering the entire pool
        free_blocks_.emplace_back(0, Config::POOL_SIZE);
    }
    
    struct AllocationResult {
        bool success;
        std::vector<MemoryBlock> chunks;
        std::string error_message;
    };
    
    AllocationResult allocate(size_t requested_size) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (requested_size <= 0) {
            return {false, {}, "Size must be positive"};
        }
        
        size_t aligned_size = alignSize(requested_size);
        
        if (occupied_memory_ + aligned_size > Config::POOL_SIZE) {
            return {false, {}, "Not enough memory in pool after alignment"};
        }
        
        std::cout << "Allocating " << aligned_size / (1024 * 1024) << "MB (aligned from " 
                  << requested_size << " bytes)" << std::endl;
        
        // Try to find a single block that fits
        auto single_block = findSingleBlock(aligned_size);
        if (single_block.success) {
            return single_block;
        }
        
        // Try to allocate from multiple blocks
        auto multi_block = allocateMultipleBlocks(aligned_size);
        if (multi_block.success) {
            return multi_block;
        }
        
        return {false, {}, "Failed to allocate memory"};
    }
    
    bool deallocate(const std::vector<size_t>& offsets) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        for (size_t offset : offsets) {
            if (offset >= Config::POOL_SIZE || offset < 0) {
                std::cerr << "Invalid offset: " << offset << std::endl;
                return false;
            }
            
            auto it = allocation_map_.find(offset);
            if (it == allocation_map_.end()) {
                std::cerr << "Offset not found in allocation map: " << offset << std::endl;
                return false;
            }
            
            size_t block_size = it->second;
            occupied_memory_ -= block_size;
            allocation_map_.erase(it);
            free_blocks_.emplace_back(offset, block_size);
            
            std::cout << "Freed memory block: offset=" << offset 
                      << ", size=" << block_size / (1024 * 1024) << "MB" << std::endl;
        }
        
        mergeAdjacentBlocks();
        printDebugInfo();
        
        return true;
    }
    
    void printDebugInfo() const {
        std::cout << "Free blocks: ";
        for (const auto& block : free_blocks_) {
            std::cout << "{offset=" << block.offset 
                      << ", size=" << block.size / (1024 * 1024) << "MB} ";
        }
        std::cout << std::endl;
        
        std::cout << "Allocated blocks: ";
        for (const auto& [offset, size] : allocation_map_) {
            std::cout << "{offset=" << offset 
                      << ", size=" << size / (1024 * 1024) << "MB} ";
        }
        std::cout << std::endl;
        
        std::cout << "Memory usage: " << occupied_memory_ / (1024 * 1024) 
                  << "MB / " << Config::POOL_SIZE / (1024 * 1024) << "MB" << std::endl;
    }
    
private:
    static size_t alignSize(size_t size) {
        return ((size + Config::PAGE_SIZE - 1) / Config::PAGE_SIZE) * Config::PAGE_SIZE;
    }
    
    AllocationResult findSingleBlock(size_t aligned_size) {
        for (auto it = free_blocks_.begin(); it != free_blocks_.end(); ++it) {
            if (it->size >= aligned_size) {
                AllocationResult result;
                result.success = true;
                result.chunks.emplace_back(it->offset, aligned_size);
                
                // Track allocation
                allocation_map_[it->offset] = aligned_size;
                occupied_memory_ += aligned_size;
                
                // Update or remove the free block
                if (it->size == aligned_size) {
                    free_blocks_.erase(it);
                } else {
                    it->offset += aligned_size;
                    it->size -= aligned_size;
                }
                
                std::cout << "Allocated single block: offset=" << result.chunks[0].offset
                          << ", size=" << aligned_size / (1024 * 1024) << "MB" << std::endl;
                
                return result;
            }
        }
        
        return {false, {}, "No single block large enough"};
    }
    
    AllocationResult allocateMultipleBlocks(size_t aligned_size) {
        AllocationResult result;
        size_t remaining_size = aligned_size;
        
        auto it = free_blocks_.begin();
        while (remaining_size > 0 && it != free_blocks_.end()) {
            size_t chunk_size = std::min(it->size, remaining_size);
            
            result.chunks.emplace_back(it->offset, chunk_size);
            allocation_map_[it->offset] = chunk_size;
            occupied_memory_ += chunk_size;
            
            remaining_size -= chunk_size;
            it->offset += chunk_size;
            it->size -= chunk_size;
            
            if (it->size == 0) {
                it = free_blocks_.erase(it);
            } else {
                ++it;
            }
        }
        
        if (remaining_size == 0) {
            result.success = true;
            std::cout << "Allocated " << result.chunks.size() << " chunks totaling "
                      << aligned_size / (1024 * 1024) << "MB" << std::endl;
        } else {
            result.success = false;
            result.error_message = "Insufficient memory in free blocks";
            
            // Rollback partial allocation
            for (const auto& chunk : result.chunks) {
                allocation_map_.erase(chunk.offset);
                occupied_memory_ -= chunk.size;
                free_blocks_.emplace_back(chunk.offset, chunk.size);
            }
            result.chunks.clear();
        }
        
        return result;
    }
    
    void mergeAdjacentBlocks() {
        if (free_blocks_.size() < 2) return;
        
        std::sort(free_blocks_.begin(), free_blocks_.end());
        
        for (size_t i = 0; i < free_blocks_.size() - 1; ++i) {
            if (free_blocks_[i].offset + free_blocks_[i].size == free_blocks_[i + 1].offset) {
                free_blocks_[i].size += free_blocks_[i + 1].size;
                free_blocks_.erase(free_blocks_.begin() + i + 1);
                --i; // Re-check the merged block
            }
        }
    }
};


// gRPC service implementation
class AllocatorServiceImpl : public allocator::v1::Allocator::Service {
private:
    MemoryAllocator allocator_;
    
public:
    grpc::Status AllocateMemory(grpc::ServerContext* context,
                               const allocator::v1::MemoryRequest* request,
                               allocator::v1::MemoryReply* response) override {
        size_t requested_size = request->size();
        std::cout << "Received allocation request for " << requested_size << " bytes" << std::endl;
        
        auto result = allocator_.allocate(requested_size);
        
        if (!result.success) {
            std::cerr << "Allocation failed: " << result.error_message << std::endl;
            return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, result.error_message);
        }
        
        // Populate response
        response->set_shm_name(Config::POOL_SHM_NAME);
        size_t total_size = 0;
        
        for (const auto& chunk : result.chunks) {
            auto* chunk_info = response->add_chunks();
            chunk_info->set_offset(chunk.offset);
            chunk_info->set_size(chunk.size);
            total_size += chunk.size;
        }
        
        response->set_total_size(total_size);
        
        allocator_.printDebugInfo();
        return grpc::Status::OK;
    }
    
    grpc::Status FreeSharedMemory(grpc::ServerContext* context,
                                 const allocator::v1::FreeRequest* request,
                                 allocator::v1::FreeReply* response) override {
        std::vector<size_t> offsets;
        offsets.reserve(request->offset_size());
        
        for (size_t offset : request->offset()) {
            offsets.push_back(offset);
        }
        
        std::cout << "Received deallocation request for " << offsets.size() << " blocks" << std::endl;
        
        if (!allocator_.deallocate(offsets)) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Failed to deallocate memory");
        }
        
        return grpc::Status::OK;
    }
};


// Server management class
class AllocatorServer {
private:
    std::unique_ptr<grpc::Server> server_;
    SharedMemoryPool memory_pool_;
    std::unique_ptr<AllocatorServiceImpl> service_;
    std::atomic<bool> running_;
    
public:
    AllocatorServer() : running_(false) {}

    SharedMemoryPool& getPool() { return memory_pool_; }

    bool initialize() {
        if (!memory_pool_.initialize()) {
            std::cerr << "Failed to initialize memory pool" << std::endl;
            return false;
        }
        
        service_ = std::make_unique<AllocatorServiceImpl>();
        grpc::ServerBuilder builder;
        
        builder.AddListeningPort(Config::SERVER_ADDRESS, grpc::InsecureServerCredentials());
        builder.RegisterService(service_.get());

        server_ = builder.BuildAndStart();
        if (!server_) {
            std::cerr << "Failed to start gRPC server" << std::endl;
            return false;
        }
        
        std::cout << "Allocator server listening on " << Config::SERVER_ADDRESS << std::endl;
        running_ = true;
        
        return true;
    }
    
    void run() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(Config::SLEEP_INTERVAL_MS));
        }
    }
    
    void shutdown() {
        running_ = false;
        if (server_) {
            server_->Shutdown();
        }
    }
    
    void setSignalHandler() {
        signal(SIGINT, [](int) {
            std::cout << "\nReceived shutdown signal..." << std::endl;
        });
    }
};


// Global server instance for signal handling
std::unique_ptr<AllocatorServer> g_server;

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    if (g_server) {
        g_server->shutdown();
        g_server->getPool().cleanup();
    }
}

int main() {
    // Set up signal handling
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    try {
        g_server = std::make_unique<AllocatorServer>();
        
        if (!g_server->initialize()) {
            std::cerr << "Failed to initialize server" << std::endl;
            return 1;
        }
        
        g_server->run();
        
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "Server shutdown complete" << std::endl;
    return 0;
}
