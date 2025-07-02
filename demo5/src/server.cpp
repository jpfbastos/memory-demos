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

const size_t PAGE_SIZE = 4 * 1024 * 1024; // 4 MB Pages
const size_t POOL_SIZE = 100 * 1024 * 1024; // 100 MB pool
const char* POOL_SHM_NAME = "/memory_pool_main";
const char* MESSAGE = "Allocator Server says hello :)";
int running = 1;

struct Block {
    size_t offset;
    size_t size;
};
std::vector<Block> free_list;
std::mutex alloc_mutex;
size_t occupied_memory_ = 0;
std::unordered_map<size_t, size_t> allocation_map_;
char* pool_base = nullptr;
int pool_fd = -1;
std::mutex pool_mutex;

class AllocatorImpl : public allocator::v1::Allocator::Service {
Status AllocateMemory(ServerContext* context,
                                  const MemoryRequest* request,
                                  MemoryReply* response) override {
    size_t size = request->size();
    std::cout << "Received AllocateMemory request for size: " << size << std::endl;
    
    if (size <= 0) {
        std::cerr << "Invalid size requested: " << size << std::endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Size must be positive");
    }

    std::lock_guard<std::mutex> lock(pool_mutex);
    size_t aligned_size = ((size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    if (occupied_memory_ + aligned_size > POOL_SIZE) {
        std::cerr << "Not enough memory in pool after alignment. Requested: " << aligned_size
                  << ", Available: " << (POOL_SIZE - occupied_memory_) << std::endl;
        return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "Not enough memory in pool");
    }

    bool found = false;

    for (auto it = free_list.begin(); it != free_list.end(); ++it) {
        std::cout << "Checking free block: {" << it->offset << ", " << it->size / (1024*1024) << "MB}" << std::endl;
        if (it->size >= aligned_size) {
            ChunkInfo* chunk = response->add_chunks();
            chunk->set_offset(it->offset);
            chunk->set_size(aligned_size);

            allocation_map_[it->offset] = aligned_size;  // track allocated block size
            occupied_memory_ += aligned_size;

            std::cout << "Allocated memory from free block: {" << it->offset << ", " << aligned_size / (1024*1024) << "MB}" << std::endl;
            // Update free block: shrink or remove
            if (it->size == aligned_size) {
                free_list.erase(it);
            } else {
                it->offset += aligned_size;
                it->size -= aligned_size;
            }

            found = true;
            break;
        }
    }

    if (!found) {
        size_t running_size = 0;
        auto it = free_list.begin();

        while (running_size < aligned_size && it != free_list.end()) {
            std::cout << "Checking free block: {" << it->offset << ", " << it->size << "}" << std::endl;
            size_t chunk_size = std::min(it->size, aligned_size - running_size);
            
            ChunkInfo* chunk = response->add_chunks();
            chunk->set_offset(it->offset);
            chunk->set_size(chunk_size);
            
            running_size += chunk_size;
            it->offset += chunk_size;
            it->size -= chunk_size;
            
            allocation_map_[it->offset] = chunk_size; // track allocated block size
            occupied_memory_ += chunk_size;

            if (it->size == 0) {
               it = free_list.erase(it);
            } else {
                ++it;
            }
        }
        
        if (running_size == aligned_size) {
            found = true;
        } else {
            std::cerr << "Failed to allocate enough memory from free blocks. Requested: " 
                      << aligned_size << ", Allocated: " << running_size << std::endl;
            return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "Failed to allocate enough memory from free blocks");
        }
    }

    if (!found) {
        std::cerr << "Failed to allocate aligned memory of size: " << aligned_size << std::endl;
        return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "Failed to allocate aligned memory");
    }

    response->set_shm_name(POOL_SHM_NAME);
    response->set_total_size(aligned_size); // Or sum of chunk sizes if multiple


    // print free list for debugging
    std::cout << "Free blocks after allocation: ";
    for (const auto& block : free_list) {
        std::cout << "{" << block.offset << ", " << block.size / (1024*1024) << "MB} ";
    }
    std::cout << std::endl;

    // print allocation map for debugging
    std::cout << "Allocation map after allocation: ";
    for (const auto& [offset, size] : allocation_map_) {
        std::cout << "{" << offset << ", " << std::dec << size / (1024*1024) << "MB} ";
    }
    std::cout << std::endl;

    return grpc::Status::OK;
}


Status FreeSharedMemory(ServerContext* context,
                        const FreeRequest* request,
                        FreeReply* response) override {

    for (const size_t offset : request->offset()) {
        if (offset >= POOL_SIZE) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Offset out of bounds");
        }

        std::cout << "Received FreeSharedMemory request for offset: " << offset << std::endl;

        const auto& it = allocation_map_.find(offset);
        if (it == allocation_map_.end()) {
            std::cerr << "Pointer not found in allocation map: " << offset << std::endl;
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Pointer not found");
        }

        const size_t alloc_size = it->second;
        occupied_memory_ -= alloc_size;
        allocation_map_.erase(it);
        free_list.push_back({offset, alloc_size});
    }

    if (free_list.size() < 2) {
        return grpc::Status::OK;
    }

    std::sort(free_list.begin(), free_list.end(), [](const Block& a, const Block& b) {
        return a.offset < b.offset;
    });

    for (size_t i = 0; i < free_list.size() - 1; ++i) {
        if (free_list[i].offset + free_list[i].size == free_list[i + 1].offset) {
            free_list[i].size += free_list[i + 1].size;
            free_list.erase(free_list.begin() + i + 1);
            --i; // Check the new merged block again

        }
    }

    // print free list after freeing memory
    std::cout << "Free blocks after freeing memory: ";
    for (const auto& block : free_list) {
        std::cout << "{" << block.offset << ", " << block.size / (1024*1024) << "MB} ";
    }
    std::cout << std::endl;

    return grpc::Status::OK;

}
};

void handler(int sig) {
    running = 0;
}

bool initialiseMemoryPool() {
    std::cout << "Initialising memory pool..." << std::endl;

    pool_fd = shm_open(POOL_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0640);
    if (pool_fd == -1) {
        perror("shm_open");
        return false;
    }

    if (ftruncate(pool_fd, POOL_SIZE) == -1) {
        perror("ftruncate");
        close(pool_fd);
        shm_unlink(POOL_SHM_NAME);
        return false;
    }

    pool_base = static_cast<char*>(mmap(nullptr, POOL_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_LOCKED, pool_fd, 0));
    if (pool_base == MAP_FAILED) {
        perror("mmap");
        close(pool_fd);
        shm_unlink(POOL_SHM_NAME);
        return false;
    }

    cudaError_t err = cudaHostRegister(pool_base, POOL_SIZE, cudaHostRegisterDefault);
    if (err != cudaSuccess) {
        std::cerr << "cudaHostRegister failed for pool: " << cudaGetErrorString(err) << std::endl;
        munmap(pool_base, POOL_SIZE);
        close(pool_fd);
        shm_unlink(POOL_SHM_NAME);
        return false;
    }

    free_list.push_back({0, POOL_SIZE}); // Initial free block covering the whole pool

    std::cout << "Memory pool initialised successfully at address " << static_cast<void*>(pool_base) << std::endl;

    uint32_t* ptr = reinterpret_cast<uint32_t*>(pool_base);
    size_t count = POOL_SIZE / sizeof(uint32_t);

    for (size_t i = 0; i < count; ++i) {
        ptr[i] = i;  // fill with 0, 1, 2, ...
    }

    return true;
}

void cleanupAll() {
    std::lock_guard<std::mutex> lock(alloc_mutex);
    std::cout << "Cleaning up shared memory..." << std::endl;
    cudaError_t err = cudaHostUnregister(pool_base);
    if (err != cudaSuccess) {
        std::cerr << "cudaHostUnregister failed during cleanup: " << cudaGetErrorString(err) << std::endl;
    }

    if (pool_base && pool_base != MAP_FAILED) {
        munmap(pool_base, POOL_SIZE);
    }
    if (pool_fd >= 0) {
        close(pool_fd);
    }
    shm_unlink(POOL_SHM_NAME);

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

    initialiseMemoryPool();

    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    server->Shutdown();
    cleanupAll();
    
    return 0;
}
