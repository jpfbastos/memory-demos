#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cuda_runtime_api.h>

constexpr const char* SHM_NAME = "/my_shared_mem";
constexpr size_t SIZE = 5ULL * 1024 * 1024 * 1024; // 5 GB
constexpr const char* MESSAGE = "Writer says hello :)";

int main() {
    // Create and size shared memory object
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0640);
    if (shm_fd == -1 || ftruncate(shm_fd, SIZE) == -1) {
        perror("shm_open or ftruncate");
        if (shm_fd != -1) close(shm_fd);
        shm_unlink(SHM_NAME);
        return 1;
    }

    // Determine aligned size
    size_t page_size = sysconf(_SC_PAGESIZE);
    size_t aligned_bytes = ((SIZE + page_size - 1) / page_size) * page_size;

    // Allocate aligned memory
    void* ptr = aligned_alloc(page_size, aligned_bytes);
    if (!ptr) {
        std::cerr << "aligned_alloc failed\n";
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return 1;
    }

    cudaPointerAttributes attr;
    cudaPointerGetAttributes(&attr, ptr);
    std::cout << "Before mmap - type: " << attr.type << std::endl;

    // Map shared memory at ptr
    void* h_data = mmap(ptr, aligned_bytes, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_LOCKED | MAP_FIXED, shm_fd, 0);
    if (h_data == MAP_FAILED) {
        perror("mmap");
        free(ptr);
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return 1;
    }

    // Check registration after mmap
    cudaPointerGetAttributes(&attr, h_data);
    std::cout << "After mmap - type: " << attr.type << std::endl;

    // Register with CUDA
    if (cudaHostRegister(h_data, aligned_bytes, cudaHostRegisterDefault) != cudaSuccess) {
        std::cerr << "cudaHostRegister failed\n";
        munmap(h_data, aligned_bytes);
        free(ptr);
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return 1;
    }

    std::cout << "Mapped ptr   = " << ptr << "\n";
    std::cout << "Mapped h_data= " << h_data << "\n";

    // Write to shared memory
    strcpy(static_cast<char*>(h_data), MESSAGE);
    std::cout << "Message written: " << MESSAGE << "\n";

    std::cout << "Press Enter to cleanup and exit...";
    std::cin.get();

    // Cleanup
    cudaHostUnregister(h_data);
    munmap(h_data, aligned_bytes);
    free(ptr);
    close(shm_fd);
    shm_unlink(SHM_NAME);

    return 0;
}