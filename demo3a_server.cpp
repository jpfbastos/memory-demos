#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cuda_runtime_api.h>

const char* SHM_NAME = "/my_shared_mem";
const size_t SIZE = 5ULL * 1024 * 1024 * 1024; // 5 GB
const char* MESSAGE = "Writer says hello :)";

int main() {
    // Create shared memory object
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0640);
    if (shm_fd == -1) {
        perror("shm_open");
        return 1;
    }

    if (ftruncate(shm_fd, SIZE) == -1) {
        perror("ftruncate");
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return 1;
    }

    size_t page_size = sysconf(_SC_PAGESIZE);
    size_t aligned_bytes = ((SIZE + page_size - 1) / page_size) * page_size;

    // Allocate aligned memory
    void* ptr = aligned_alloc(4096, aligned_bytes);
    if (!ptr) {
        std::cerr << "aligned_alloc failed\n";
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return 1;
    }

    // Pin memory for CUDA
    cudaError_t err = cudaHostRegister(ptr, aligned_bytes, cudaHostRegisterDefault);
    if (err != cudaSuccess) {
        std::cerr << "cudaHostRegister failed: " << cudaGetErrorString(err) << std::endl;
        free(ptr);
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return 1;
    }

    std::cout << "Shared memory created and pinned successfully.\n";

    // Map shared memory to our pinned host memory pointer
    void* h_data = mmap(ptr, aligned_bytes, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_LOCKED, shm_fd, 0);
    if (h_data == MAP_FAILED) {
        perror("mmap");
        cudaHostUnregister(ptr);
        free(ptr);
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return 1;
    }

    // Write message to shared memory
    std::cout << "Writing message: " << MESSAGE << std::endl;
    strcpy(static_cast<char*>(h_data), MESSAGE);

    std::cout << "Press Enter to cleanup and exit...";
    std::cin.get();

    // Cleanup
    cudaHostUnregister(ptr);
    munmap(h_data, aligned_bytes);
    free(ptr);
    close(shm_fd);
    shm_unlink(SHM_NAME);

    return 0;
}
