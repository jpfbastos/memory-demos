#include <iostream>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>
#include <signal.h>
#include <vector>
#include <string>
#include <sys/types.h>
#include <unordered_map>
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

// Configuration constants
namespace Config {
    constexpr size_t SEGMENT_SIZE = 4 * 1024 * 1024;     // 4 MB per segment
    constexpr size_t TOTAL_POOL_SIZE = 10ULL * 1024 * 1024 * 1024;   // 10 GB total
    constexpr size_t NUM_SEGMENTS = TOTAL_POOL_SIZE / SEGMENT_SIZE;   // Number of segments
    constexpr const char* SERVER_ADDRESS = "0.0.0.0:50051";
    constexpr const char* SHM_PREFIX = "/allocator_seg_";
    constexpr int SLEEP_INTERVAL_MS = 200;
}

// Memory segment structure
struct MemorySegment {
    std::string shm_name;
    int fd;
    void* base_ptr;
    bool is_free;
    size_t segment_id;
    
    MemorySegment(size_t id) : segment_id(id), fd(-1), base_ptr(nullptr), is_free(true) {
        uuid_t uuid;
        uuid_generate(uuid);
        char shm_name_array[37]; // 36 characters + null terminator
        uuid_unparse(uuid, shm_name_array);
        shm_name = std::string(Config::SHM_PREFIX) + shm_name_array;
    }
    
    ~MemorySegment() {
        cleanup();
    }
    
    bool initialize() {
        fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0640);
        if (fd == -1) {
            perror(("shm_open failed for " + shm_name).c_str());
            return false;
        }
        
        if (ftruncate(fd, Config::SEGMENT_SIZE) == -1) {
            perror("ftruncate failed");
            cleanup();
            return false;
        }
        
        base_ptr = mmap(nullptr, Config::SEGMENT_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (base_ptr == MAP_FAILED) {
            perror("mmap failed");
            cleanup();
            return false;
        }
        
        // Register with CUDA for pinned memory
        cudaError_t cuda_err = cudaHostRegister(base_ptr, Config::SEGMENT_SIZE, cudaHostRegisterDefault);
        if (cuda_err != cudaSuccess) {
            std::cerr << "cudaHostRegister failed for segment " << segment_id 
                      << ": " << cudaGetErrorString(cuda_err) << std::endl;
            cleanup();
            return false;
        }
        
        // Initialize memory with test pattern
        initializeMemoryPattern();
        
        return true;
    }
    
    void cleanup() {
        if (base_ptr && base_ptr != MAP_FAILED) {
            cudaError_t cuda_err = cudaHostUnregister(base_ptr);
            if (cuda_err != cudaSuccess) {
                std::cerr << "cudaHostUnregister failed for segment " << segment_id 
                          << ": " << cudaGetErrorString(cuda_err) << std::endl;
            }
            
            if (munmap(base_ptr, Config::SEGMENT_SIZE) == -1) {
                perror(("munmap failed for segment " + std::to_string(segment_id)).c_str());
            }
            base_ptr = nullptr;
        }
        
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
        
        if (shm_unlink(shm_name.c_str()) == -1) {
            perror(("shm_unlink failed for " + shm_name).c_str());
        }
    }
    
private:
    void initializeMemoryPattern() {
        uint32_t* ptr = reinterpret_cast<uint32_t*>(base_ptr);
        size_t count = Config::SEGMENT_SIZE / sizeof(uint32_t);
        
        for (size_t i = 0; i < count; ++i) {
            ptr[i] = static_cast<uint32_t>(segment_id * count + i);
        }
    }
};

class SharedMemoryPool {
private:
    std::vector<std::unique_ptr<MemorySegment>> segments_;
    bool initialized_;
    
public:
    SharedMemoryPool() : initialized_(false) {}
    
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
        
        std::cout << "Initializing memory pool with " << Config::NUM_SEGMENTS 
                  << " segments of " << Config::SEGMENT_SIZE / (1024 * 1024) << "MB each..." << std::endl;
        
        segments_.reserve(Config::NUM_SEGMENTS);
        
        for (size_t i = 0; i < Config::NUM_SEGMENTS; ++i) {
            auto segment = std::make_unique<MemorySegment>(i);
            if (!segment->initialize()) {
                std::cerr << "Failed to initialize segment " << i << std::endl;
                cleanup();
                return false;
            }
            segments_.push_back(std::move(segment));
        }
        
        initialized_ = true;
        std::cout << "Memory pool initialized successfully with " << segments_.size() 
                  << " segments (" << (segments_.size() * Config::SEGMENT_SIZE) / (1024 * 1024) 
                  << "MB total)" << std::endl;
        
        return true;
    }
    
    void cleanup() {
        if (!initialized_) return;
        
        std::cout << "Cleaning up shared memory pool..." << std::endl;
        
        segments_.clear(); // This will call destructors which handle cleanup
        
        initialized_ = false;
        std::cout << "Shared memory pool cleanup completed" << std::endl;
    }
    
    const std::vector<std::unique_ptr<MemorySegment>>& getSegments() const {
        return segments_;
    }
    
    bool isInitialized() const { return initialized_; }
};

// Memory allocator class
class MemoryAllocator {
private:
    SharedMemoryPool& memory_pool_;
    std::unordered_map<std::string, size_t> allocated_segments_; // shm_name -> segment_id
    size_t allocated_count_;
    mutable std::mutex mutex_;
    
public:
    MemoryAllocator(SharedMemoryPool& pool) : memory_pool_(pool), allocated_count_(0) {}
    
    struct AllocationResult {
        bool success;
        std::vector<std::string> shm_names;
        std::string error_message;
    };
    
    AllocationResult allocate(size_t requested_size) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (requested_size <= 0) {
            return {false, {}, "Size must be positive"};
        }
        
        // Calculate number of segments needed
        size_t segments_needed = (requested_size + Config::SEGMENT_SIZE - 1) / Config::SEGMENT_SIZE;
        
        std::cout << "Allocating " << segments_needed << " segments for " 
                  << requested_size / (1024 * 1024) << "MB request" << std::endl;
        
        const auto& segments = memory_pool_.getSegments();
        
        // Check if we have enough free segments
        size_t available_segments = 0;
        for (const auto& segment : segments) {
            if (segment->is_free) {
                available_segments++;
            }
        }
        
        if (available_segments < segments_needed) {
            return {false, {}, "Not enough free segments available"};
        }
        
        // Allocate segments
        AllocationResult result;
        result.success = true;
        
        size_t allocated = 0;
        for (const auto& segment : segments) {
            if (segment->is_free && allocated < segments_needed) {
                segment->is_free = false;
                result.shm_names.push_back(segment->shm_name);
                allocated_segments_[segment->shm_name] = segment->segment_id;
                allocated++;
                
                std::cout << "Allocated segment " << segment->segment_id 
                          << " (shm_name: " << segment->shm_name << ")" << std::endl;
            }
        }
        
        allocated_count_ += allocated;
        
        std::cout << "Successfully allocated " << allocated << " segments ("
                  << allocated * Config::SEGMENT_SIZE / (1024 * 1024) << "MB total)" << std::endl;
        
        printDebugInfo();
        return result;
    }
    
    bool deallocate(const std::vector<std::string>& shm_names) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        const auto& segments = memory_pool_.getSegments();
        
        for (const std::string& shm_name : shm_names) {
            auto it = allocated_segments_.find(shm_name);
            if (it == allocated_segments_.end()) {
                std::cerr << "Shared memory name not found in allocation map: " << shm_name << std::endl;
                return false;
            }
            
            size_t segment_id = it->second;
            if (segment_id >= segments.size()) {
                std::cerr << "Invalid segment ID: " << segment_id << std::endl;
                return false;
            }
            
            segments[segment_id]->is_free = true;
            allocated_segments_.erase(it);
            allocated_count_--;
            
            std::cout << "Freed segment " << segment_id 
                      << " (shm_name: " << shm_name << ")" << std::endl;
        }
        
        printDebugInfo();
        return true;
    }
    
    void printDebugInfo() const {
        const auto& segments = memory_pool_.getSegments();
        
        std::cout << "Segment allocation status:" << std::endl;
        size_t free_count = 0;
        for (const auto& segment : segments) {
            if (segment->is_free) {
                free_count++;
            }
        }
        
        std::cout << "  Free segments: " << free_count << " / " << segments.size() << std::endl;
        std::cout << "  Allocated segments: " << allocated_count_ << " / " << segments.size() << std::endl;
        std::cout << "  Memory usage: " << (allocated_count_ * Config::SEGMENT_SIZE) / (1024 * 1024) 
                  << "MB / " << (segments.size() * Config::SEGMENT_SIZE) / (1024 * 1024) << "MB" << std::endl;
    }
};

// gRPC service implementation
class AllocatorServiceImpl : public allocator::v1::Allocator::Service {
private:
    MemoryAllocator allocator_;
    
public:
    AllocatorServiceImpl(SharedMemoryPool& pool) : allocator_(pool) {}
    
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
        
        // Populate response with multiple shm_names
        for (const std::string& shm_name : result.shm_names) {
            response->add_shm_names(shm_name);    
        }
        response->set_segment_size(Config::SEGMENT_SIZE);
        response->set_total_size(result.shm_names.size() * Config::SEGMENT_SIZE);
        
        return grpc::Status::OK;
    }
    
    grpc::Status FreeSharedMemory(grpc::ServerContext* context,
                                 const allocator::v1::FreeRequest* request,
                                 allocator::v1::FreeReply* response) override {
        std::vector<std::string> shm_names;
        
        // Extract shm_names from the request
        for (const auto& shm_name : request->shm_names()) {
            shm_names.push_back(shm_name);
        }
        
        std::cout << "Received deallocation request for " << shm_names.size() << " segments" << std::endl;
        
        if (!allocator_.deallocate(shm_names)) {
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
        
        service_ = std::make_unique<AllocatorServiceImpl>(memory_pool_);
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