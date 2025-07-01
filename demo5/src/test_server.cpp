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
#include <sys/stat.h>
#include <uuid/uuid.h>
#include <grpcpp/grpcpp.h>
#include <cuda_runtime_api.h>
#include "allocator/allocator.grpc.pb.h"

using grpc::Status;
using grpc::ServerContext;
using grpc::Status;
using grpc::Status;
using allocator::v1::Allocator;
using allocator::v1::MemoryRequest;
using allocator::v1::MemoryReply;
using allocator::v1::FreeRequest;
using allocator::v1::FreeReply;

const int SIZE = 4 * 1024; // 1 KB
const char* MESSAGE = "Allocator Server says hello :)";
int running = 1;

struct Allocation {
    int fd;                    // file descriptor from shm_open
    void* alloc_ptr;          // from aligned_alloc
    void* mmap_ptr;           // from mmap
    pid_t client_pid;        // PID of the process using this shm
    int lockfd;               // file descriptor for permission lock
    std::string lockfile_path; // path to lockfile
    bool access_revoked;      // flag indicating if access has been revoked
    
    Allocation() : fd(-1), alloc_ptr(nullptr), mmap_ptr(MAP_FAILED), 
                   lockfd(-1), access_revoked(false) {}
};
std::unordered_map<std::string, Allocation> allocations;
std::mutex alloc_mutex;

std::vector<pid_t> findProcessesWithSharedMemory(const std::string& shmName) {
    std::vector<pid_t> pids;
    pid_t serverPid = getpid(); 

    DIR* procDir = opendir("/proc");
    if (!procDir) return pids;
    
    struct dirent* entry;
    while ((entry = readdir(procDir)) != nullptr) {
        if (!isdigit(entry->d_name[0])) continue;
        
        pid_t pid = std::stoi(entry->d_name);
        if (pid == serverPid) continue;  // Skip self
        std::string mapsPath = "/proc/" + std::string(entry->d_name) + "/maps";
        
        std::ifstream mapsFile(mapsPath);
        if (!mapsFile.is_open()) continue;
        
        std::string line;
        while (std::getline(mapsFile, line)) {
            if (line.find(shmName) != std::string::npos) {
                pids.push_back(pid);
                break;
            }
        }
    }
    
    closedir(procDir);
    return pids;
}

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
    std::lock_guard<std::mutex> lock(alloc_mutex);

    pid_t serverPid = getpid();
    
    auto it = allocations.find(shmName);
    if (it == allocations.end()) {
        std::cerr << "Shared memory not found for revocation: " << shmName << std::endl;
        return false;
    }
    
    Allocation& alloc = it->second;
    
    if (alloc.access_revoked) {
        std::cout << "Access already revoked for " << shmName << std::endl;
        return true;
    }
    
    std::cout << "Starting access revocation for " << shmName << std::endl;
    
    // Step 1: Acquire permission lock
    if (!acquirePermissionLock(alloc, shmName)) {
        return false;
    }
    
    // Step 2: Find all processes currently using this shared memory
    std::vector<pid_t> current_pids = findProcessesWithSharedMemory(shmName);
    
    // Step 3: Revoke file permissions
    std::string shmPath = "/dev/shm" + shmName;
    if (chmod(shmPath.c_str(), 0000) == -1) {
        perror("Failed to revoke permissions");
        releasePermissionLock(alloc, shmName);
        return false;
    }
    
    std::cout << "Revoked file permissions for " << shmPath << std::endl;
    
    // Step 4: Send SIGBUS to all tracked processes
    for (pid_t pid : alloc.client_pids) {
        if (kill(pid, SIGBUS) == 0) {
            std::cout << "Sent SIGBUS to PID: " << pid << std::endl;
        } else {
            if (errno == ESRCH) {
                std::cout << "PID " << pid << " no longer exists" << std::endl;
            } else {
                perror(("Failed to send SIGBUS to PID " + std::to_string(pid)).c_str());
            }
        }
    }
    
    alloc.access_revoked = true;
    return true;
}

// Function to wait for clients to close and then cleanup
bool waitForClientsAndCleanup(const std::string& shmName, int timeoutSeconds = 30) {
    std::lock_guard<std::mutex> lock(alloc_mutex);

    pid_t serverPid = getpid();
    
    auto it = allocations.find(shmName);
    if (it == allocations.end()) {
        return false;
    }
    
    Allocation& alloc = it->second;
    auto startTime = std::chrono::steady_clock::now();

    while (alloc.client_pid != -1) {
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime);
        
        if (elapsed.count() >= timeoutSeconds) {
            std::cerr << "Timeout waiting for clients to close " << shmName << ". Forcing cleanup." << std::endl;
            break;
        }
        
        // Check which processes are still alive and using the shm
        auto current_pids = findProcessesWithSharedMemory(shmName);
        if (!current_pids.empty()) {
            alloc.client_pid = current_pids[0];  // Track the first client PID
        }

        if (alloc.client_pid != -1) {
            std::cout << "Waiting for process " << alloc.client_pid << " to close " << shmName << "..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    
    std::cout << "All clients closed for " << shmName << ", proceeding with cleanup" << std::endl;
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
    
    // Wait for clients to close
    if (!waitForClientsAndCleanup(shm_name)) {
        std::cerr << "Failed to wait for clients to close " << shm_name << std::endl;
    }
    
    // Now safely cleanup resources
    std::lock_guard<std::mutex> lock(alloc_mutex);
    auto it = allocations.find(shm_name);
    if (it == allocations.end()) {
        std::cerr << "Shared memory not found during cleanup: " << shm_name << std::endl;
        return Status::CANCELLED;
    }

    Allocation& alloc = it->second;

    // Cleanup resources
    if (alloc.mmap_ptr && alloc.mmap_ptr != MAP_FAILED) {
        if (munmap(alloc.mmap_ptr, SIZE) == -1) {
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
        if (alloc.mmap_ptr && alloc.mmap_ptr != MAP_FAILED) {
            if (munmap(alloc.mmap_ptr, SIZE) == -1) {
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
        
        if (alloc.fd) {
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
