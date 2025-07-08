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
    constexpr const char* SHM_PREFIX = "/shm_";
    constexpr int SLEEP_INTERVAL_MS = 200;
}

struct MemorySegment {
    std::string shm_name;    
    int shm_fd;              
    void* addr;              
    size_t length;           
    std::vector<char*> buffers; 

    MemorySegment() = default;
    MemorySegment(const std::string& name,
                  int fd,
                  void* a,
                  size_t len,
                  std::vector<char*>&& s)
      : shm_name(name), shm_fd(fd), addr(a), length(len), buffers(std::move(s)) {}

    ~MemorySegment() {
        if (addr && addr != MAP_FAILED) {
            munmap(addr, length);
            addr = nullptr;
        }
        if (shm_fd >= 0) {
            close(shm_fd);
            shm_unlink(shm_name.c_str());
        }

        shm_unlink(shm_name.c_str());
    }
};

class PinnedMemoryPool {
private:
    std::mutex mutex_;
    std::unordered_set<char*> free_list_;
    std::unordered_set<char*> pool_;
    std::vector<MemorySegment> segments_;

    void initializeMemoryPattern(char* base_ptr, size_t size) {
        uint32_t* ptr = reinterpret_cast<uint32_t*>(base_ptr);
        size_t count = size / sizeof(uint32_t);
        
        for (size_t i = 0; i < count; ++i) {
            ptr[i] = static_cast<uint32_t>(i);
        }
    }
    
public:
    PinnedMemoryPool() {}

    ~PinnedMemoryPool() { cleanup(); }
    
    // Delete copy constructor and assignment operator
    PinnedMemoryPool(const PinnedMemoryPool&) = delete;
    PinnedMemoryPool& operator=(const PinnedMemoryPool&) = delete;
        
    bool initialize() {
        std::cout << "Initializing memory pool with " << Config::NUM_SEGMENTS 
                  << " segments of " << Config::SEGMENT_SIZE / (1024 * 1024) << "MB each..." << std::endl;

        for (size_t i = 0; i < Config::NUM_SEGMENTS; ++i) {
            char* buffer = static_cast<char*>(aligned_alloc(4096, Config::SEGMENT_SIZE));
            if (buffer == nullptr) {
                perror("aligned_alloc failed");
                return false;
            }
            cudaError_t err = cudaHostRegister(buffer, Config::SEGMENT_SIZE, cudaHostRegisterDefault);
            if (err != cudaSuccess) {
                std::cerr << "cudaHostRegister failed for buffer " << i 
                          << ": " << cudaGetErrorString(err) << std::endl;
                cleanup();
                return false;
            }

            pool_.insert(buffer);
            free_list_.insert(buffer);
        }
        
        std::cout << "Memory pool initialized successfully with " << pool_.size() 
                  << " segments (" << (pool_.size() * Config::SEGMENT_SIZE) / (1024 * 1024) 
                  << "MB total)" << std::endl;
        
        return true;

    }
    
    int Allocate(size_t size, std::string& out_shm_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size == 0) return -1;

        int num_buf = (size + Config::SEGMENT_SIZE - 1) / Config::SEGMENT_SIZE;
        if (num_buf > static_cast<int>(free_list_.size())) {
            return num_buf - free_list_.size();
        }

        // grab buffers
        std::vector<char*> buffers;
        auto it = free_list_.begin();
        for (int i = 0; i < num_buf; ++i) {
            buffers.push_back(*it);
            it = free_list_.erase(it);
        }

        // generate unique name
        uuid_t uuid;
        uuid_generate(uuid);
        char buf[37]; uuid_unparse(uuid, buf);
        std::string shm_name = std::string(Config::SHM_PREFIX) + buf;
        out_shm_name = shm_name;

        size_t total_len = buffers.size() * Config::SEGMENT_SIZE;

        // create shared memory object
        int fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0640);
        if (fd < 0) {
            // return buffers back
            for (auto* b : buffers) free_list_.insert(b);
            return -1;
        }
        if (ftruncate(fd, total_len) < 0) {
            close(fd); shm_unlink(shm_name.c_str());
            for (auto* b : buffers) free_list_.insert(b);
            return -1;
        }

        void* addr = mmap(nullptr,
                          total_len,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED,
                          fd,
                          0);
        if (addr == MAP_FAILED) {
            close(fd); shm_unlink(shm_name.c_str());
            for (auto* b : buffers) free_list_.insert(b);
            return -1;
        }

        // map each buffer into the shared region
        size_t offset = 0;
        for (auto* b : buffers) {
            void* ret = mmap(
                static_cast<char*>(addr) + offset,
                Config::SEGMENT_SIZE,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FIXED,
                fd,
                offset);

            if (ret == MAP_FAILED) {
                // cleanup via segment destructor
                munmap(addr, total_len);
                close(fd); shm_unlink(shm_name.c_str());
                for (auto* b : buffers) free_list_.insert(b);
                return -1;
            }
            offset += Config::SEGMENT_SIZE;
        }

        // register with CUDA once for the whole region
        cudaHostRegister(addr, total_len, cudaHostRegisterDefault);

        segments_.emplace_back(shm_name, fd, addr, total_len, std::move(buffers));

        initializeMemoryPattern(static_cast<char*>(addr), total_len);

        printDebugInfo();

        return 0; // Success
    }

    int Deallocate(const std::string& shm_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(
            segments_.begin(), segments_.end(),
            [&](const MemorySegment& ms){ return ms.shm_name == shm_name; });
        if (it == segments_.end()) return -1;

        // unregister CUDA once
        cudaHostUnregister(it->addr);

        for (auto* b : it->buffers) free_list_.insert(b);

        segments_.erase(it);
    
        printDebugInfo();
        return 0;  // Success
    }

    void printDebugInfo() const {
        std::cout << "Segment allocation status:" << std::endl;

        std::cout << "  Free segments: " << free_list_.size() << " / " << pool_.size() << std::endl;
        std::cout << "  Allocated segments: " << pool_.size() - free_list_.size() << " / " << pool_.size() << std::endl;
        std::cout << "  Memory usage: " << (pool_.size() - free_list_.size()) * Config::SEGMENT_SIZE / (1024 * 1024) 
                  << "MB / " << (pool_.size() * Config::SEGMENT_SIZE) / (1024 * 1024) << "MB" << std::endl;
    }

    void cleanup() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        segments_.clear();
        
        for (char* buffer : pool_) {
            if (buffer) {
                cudaError_t err = cudaHostUnregister(buffer);
                if (err != cudaSuccess) {
                    std::cerr << "cudaHostUnregister failed: " << cudaGetErrorString(err) << std::endl;
                }
                free(buffer);
            }
        }
        
        pool_.clear();
        free_list_.clear();

        std::cout << "Memory pool cleaned up successfully" << std::endl;
    }
};

// gRPC service implementation
class AllocatorServiceImpl : public allocator::v1::Allocator::Service {
private:
    PinnedMemoryPool& pool_;
    
public:
    AllocatorServiceImpl(PinnedMemoryPool& pool) : pool_(pool) {}
    
    grpc::Status AllocateMemory(grpc::ServerContext* context,
                               const allocator::v1::MemoryRequest* request,
                               allocator::v1::MemoryReply* response) override {
        size_t requested_size = request->size();
        std::cout << "Received allocation request for " << requested_size << " bytes" << std::endl;

        std::string shm_name;

        int num_buf = (requested_size + Config::SEGMENT_SIZE - 1) / Config::SEGMENT_SIZE;
        size_t total_size = num_buf * Config::SEGMENT_SIZE;

        int result = pool_.Allocate(total_size, shm_name);

        if (result != 0) {
            if (result > 0) {
                // the code is the difference between requested and available segments
                std::cerr << "Allocation failed: not enough free segments, over by "
                          << result << " segments" << std::endl;
                return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "Not enough free space by "
                                + std::to_string(result) + " " + std::to_string(Config::SEGMENT_SIZE / (1024*1024))
                                 +  "MB segments");
            }
            std::cerr << "Allocation failed: " << result << std::endl;
            return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, std::to_string(result));
        }

        response->set_shm_name(shm_name);
        response->set_total_size(total_size);
        
        return grpc::Status::OK;
    }
    
    grpc::Status FreeSharedMemory(grpc::ServerContext* context,
                                 const allocator::v1::FreeRequest* request,
                                 allocator::v1::FreeReply* response) override {
        
        std::string shm_name = request->shm_name();
        std::cout << "Received deallocation request for shared memory: " << shm_name << std::endl;

        if (shm_name.empty()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Shared memory name cannot be empty");
        }

        if (pool_.Deallocate(shm_name) != 0) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Failed to deallocate memory");
        }
        
        return grpc::Status::OK;
    }
};

// Server management class
class AllocatorServer {
private:
    std::unique_ptr<grpc::Server> server_;
    PinnedMemoryPool memory_pool_;
    std::unique_ptr<AllocatorServiceImpl> service_;
    std::atomic<bool> running_;
    
public:
    AllocatorServer() : running_(false) {}

    PinnedMemoryPool& getPool() { return memory_pool_; }

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