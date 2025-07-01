#include <iostream>
#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>
#include <cstring>
#include <signal.h>
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

const int SIZE = 4 * 1024; // 1 KB
const char* MESSAGE = "Allocator Server says hello :)";
int running = 1;

struct Allocation {
    int fd;          // file descriptor from shm_open
    void* alloc_ptr;  // from aligned_alloc
    void* mmap_ptr;   // from mmap
};
std::unordered_map<std::string, Allocation> allocations;
std::mutex alloc_mutex;



std::string generate_unique_shm_name() {
    auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    return "/shm_" + std::to_string(timestamp);
}

class AllocatorImpl : public allocator::v1::Allocator::Service {
Status RequestSharedMemory(ServerContext* context,
                                  const MemoryRequest* request,
                                  MemoryReply* response) override {
    
    std::cout << "Received MemoryRequest from client: " << request->client_id() << std::endl;
    
    std::string shm_name;
    int shm_fd = -1;

    // Retry until we get a unique name
    do {
        shm_name = generate_unique_shm_name();
        shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0640);
    } while (shm_fd == -1 && errno == EEXIST);

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

    allocations[shm_name] = { shm_fd, ptr, h_data };

    response->set_shm_name(shm_name);
    response->set_size(SIZE);
    std::cout << "Shared memory created and pinned successfully.\n";
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
