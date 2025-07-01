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

const int SIZE = 4 * 1024; // 4 KB
const char* MESSAGE = "Allocator Server says hello :)";
int running = 1;

struct Allocation {
    int fd;                    // file descriptor from shm_open
    void* alloc_ptr;          // from aligned_alloc
    void* mmap_ptr;           // from mmap
    int lockfd;               // file descriptor for permission lock
    std::string lockfile_path; // path to lockfile
    bool access_revoked;      // flag indicating if access has been revoked
    
    Allocation() : fd(-1), alloc_ptr(nullptr), mmap_ptr(MAP_FAILED), 
               lockfd(-1), access_revoked(true) {}
};
std::unordered_map<std::string, Allocation> allocations;
std::mutex alloc_mutex;

// Function to acquire permission lock for a shared memory segment
bool acquirePermissionLock(Allocation& alloc, const std::string& shmName) {
    alloc.lockfile_path = "/tmp/." + shmName + ".lock";
    alloc.lockfd = open(alloc.lockfile_path.c_str(), O_CREAT | O_RDWR, 0640);
    
    if (alloc.lockfd == -1) {
        perror("Failed to open lockfile");
        return false;
    }
    
    if (flock(alloc.lockfd, LOCK_EX) == -1) {
        perror("Failed to acquire exclusive lock");
        close(alloc.lockfd);
        alloc.lockfd = -1;
        return false;
    }
    
    std::cout << "Acquired permission lock for " << shmName << std::endl;
    return true;
}

// Function to release permission lock
void releasePermissionLock(Allocation& alloc, const std::string& shmName) {
    if (alloc.lockfd >= 0) {
        flock(alloc.lockfd, LOCK_UN);
        close(alloc.lockfd);
        alloc.lockfd = -1;
        
        // Remove lockfile
        if (!alloc.lockfile_path.empty()) {
            unlink(alloc.lockfile_path.c_str());
        }
        
        std::cout << "Released permission lock for " << shmName << std::endl;
    }
}

// Function to revoke access to shared memory
bool revokeSharedMemoryAccess(const std::string& shmName) {
    
    Allocation alloc;
    {
        std::lock_guard<std::mutex> lock(alloc_mutex);
        auto it = allocations.find(shmName);
        if (it == allocations.end()) {
            std::cerr << "Shared memory not found for revocation: " << shmName << std::endl;
            return false;
        }
        alloc = it->second;
    }

    std::cout << "Starting access revocation for " << shmName << std::endl;

    if (!acquirePermissionLock(alloc, shmName)) {
        return false;
    }

    if (fchmod(alloc.fd, 0000) == -1) {
        perror("Failed to revoke permissions");
        releasePermissionLock(alloc, shmName);
        return false;
    }

    std::cout << "Revoked file permissions for /dev/shm" << shmName << std::endl;

    {
        std::lock_guard<std::mutex> lock(alloc_mutex);
        auto it = allocations.find(shmName);
        if (it != allocations.end()) {
            it->second.access_revoked = true;
        }
    }
    return true;
}

class AllocatorImpl : public allocator::v1::Allocator::Service {
Status RequestSharedMemory(ServerContext* context,
                                  const MemoryRequest* request,
                                  MemoryReply* response) override {

    std::string shm_name;
    int shm_fd = -1;

    // Retry until we get a unique name
    uuid_t uuid;
    uuid_generate(uuid);
    char shm_name_array[37]; // 36 characters + null terminator
    uuid_unparse(uuid, shm_name_array);
    shm_name = std::string("/shm_") + shm_name_array;

    shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0640);
    if (shm_fd == -1) {
        perror("shm_open");
        return Status::CANCELLED;
    }

    if (ftruncate(shm_fd, SIZE) == -1) {
        perror("ftruncate");
        close(shm_fd);
        shm_unlink(shm_name.c_str());
        return Status::CANCELLED;
    }

    size_t page_size = sysconf(_SC_PAGESIZE);
    size_t aligned_bytes = ((SIZE + page_size - 1) / page_size) * page_size;

    // Allocate aligned memory
    void* ptr = aligned_alloc(4096, aligned_bytes);
    if (!ptr) {
        std::cerr << "aligned_alloc failed\n";
        close(shm_fd);
        shm_unlink(shm_name.c_str());
        return Status::CANCELLED;
    }

    // Pin memory for CUDA
    cudaError_t err = cudaHostRegister(ptr, aligned_bytes, cudaHostRegisterDefault);
    if (err != cudaSuccess) {
        std::cerr << "cudaHostRegister failed: " << cudaGetErrorString(err) << std::endl;
        free(ptr);
        close(shm_fd);
        shm_unlink(shm_name.c_str());
        return Status::CANCELLED;
    }

    void* h_data = mmap(ptr, aligned_bytes, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_LOCKED, shm_fd, 0);
    if (h_data == MAP_FAILED) {
        perror("mmap");
        cudaHostUnregister(ptr);
        free(ptr);
        close(shm_fd);
        shm_unlink(shm_name.c_str());
        return Status::CANCELLED;
    }

    std::cout << "Writing message: " << shm_name << " " << MESSAGE << std::endl;
    strcpy(static_cast<char*>(h_data), MESSAGE);

    // Store allocation with access control structure
    {
        std::lock_guard<std::mutex> lock(alloc_mutex);
        Allocation alloc;
        alloc.fd = shm_fd;
        alloc.alloc_ptr = ptr;
        alloc.mmap_ptr = h_data;
        allocations[shm_name] = std::move(alloc);
    }

    response->set_shm_name(shm_name);
    response->set_size(SIZE);
    std::cout << "Shared memory created and pinned successfully.\n";
    return Status::OK;
}


Status FreeSharedMemory(ServerContext* context,
                        const FreeRequest* request,
                        FreeReply* response) override {
    const std::string& shm_name = request->shm_name();
    std::cout << "Received FreeSharedMemory request for: " << shm_name << std::endl;
    
    // First revoke access to prevent new clients
    if (!revokeSharedMemoryAccess(shm_name)) {
        std::cerr << "Failed to revoke access for " << shm_name << std::endl;
        return Status::CANCELLED;
    }
    
    // Now safely cleanup resources
    std::lock_guard<std::mutex> lock(alloc_mutex);
    auto it = allocations.find(shm_name);
    if (it == allocations.end()) {
        std::cerr << "Shared memory not found during cleanup: " << shm_name << std::endl;
        return Status::CANCELLED;
    }

    Allocation& alloc = it->second;

    size_t page_size = sysconf(_SC_PAGESIZE);
    size_t aligned_bytes = ((SIZE + page_size - 1) / page_size) * page_size;

    // Cleanup resources
    if (alloc.mmap_ptr && alloc.mmap_ptr != MAP_FAILED) {
        if (munmap(alloc.mmap_ptr, aligned_bytes) == -1) {
            perror(("munmap failed for " + shm_name).c_str());
        }
    }

    if (alloc.alloc_ptr) {
        cudaError_t err = cudaHostUnregister(alloc.alloc_ptr);
        if (err != cudaSuccess) {
            std::cerr << "cudaHostUnregister failed for " << shm_name << ": "
                        << cudaGetErrorString(err) << std::endl;
        }
        free(alloc.alloc_ptr);
    }

    // truncate the shared memory file to 0
    if (ftruncate(alloc.fd, 0) == -1) {
        perror(("ftruncate failed for " + shm_name).c_str());
    }

    if (alloc.fd >= 0) {
        close(alloc.fd);
    }

    // Release permission lock
    releasePermissionLock(alloc, shm_name);

    if (shm_unlink(shm_name.c_str()) == -1) {
        perror(("shm_unlink failed for " + shm_name).c_str());
    }

    allocations.erase(it);
    std::cout << "Shared memory freed successfully: " << shm_name << std::endl;
    return Status::OK;
}

};

void handler(int sig) {
    running = 0;
}

void cleanupAll() {
    std::lock_guard<std::mutex> lock(alloc_mutex);

    for (const auto& [name, alloc] : allocations) {
        std::cout << "Cleaning up: " << name << std::endl;

        // munmap with mmap pointer
        size_t page_size = sysconf(_SC_PAGESIZE);
        size_t aligned_bytes = ((SIZE + page_size - 1) / page_size) * page_size;

        if (alloc.mmap_ptr && alloc.mmap_ptr != MAP_FAILED) {
            if (munmap(alloc.mmap_ptr, aligned_bytes) == -1) {
                perror(("munmap failed for " + name).c_str());
            }
        }

        // cudaHostUnregister and free with aligned_alloc pointer
        if (alloc.alloc_ptr) {
            cudaError_t err = cudaHostUnregister(alloc.alloc_ptr);
            if (err != cudaSuccess) {
                std::cerr << "cudaHostUnregister failed for " << name << ": "
                          << cudaGetErrorString(err) << std::endl;
            }
            free(alloc.alloc_ptr);
        }

        if (ftruncate(alloc.fd, 0) == -1) {
            perror(("ftruncate failed for " + name).c_str());
        }
        
        if (alloc.fd >= 0) {
            close(alloc.fd);
        }

        // unlink shared memory
        if (shm_unlink(name.c_str()) == -1) {
            perror(("shm_unlink failed for " + name).c_str());
        }

    }

    allocations.clear();
    std::cout << "\n[server] All shared memory cleaned up.\n";
}

int main() {
    signal(SIGINT, handler);

    AllocatorImpl service;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("0.0.0.0:50051", grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());

    std::cout << "Allocator server listening on port 50051" << std::endl;

    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    server->Shutdown();
    cleanupAll();
    
    return 0;
}
