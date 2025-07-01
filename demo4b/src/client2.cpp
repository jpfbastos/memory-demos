#include <iostream>
#include <memory>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <signal.h>
#include <cuda_runtime_api.h>

void* h_data = nullptr; // Pointer to the mapped shared memory
size_t aligned_size = 0; // Size of the aligned memory
void* ptr = nullptr; // Pointer to the allocated memory for pinning
int shm_fd; // File descriptor for the shared memory
int SIZE = 4 * 1024; // 4 KB

int main() {

    shm_fd = shm_open("/shm_b07e9f03-faf2-47d7-b9b0-322e9db85c98", O_RDWR, 0640);
    if (shm_fd == -1) {
        perror("shm_open");
        return 1;
    }

    // Allocate aligned memory for pinning
    size_t page_size = sysconf(_SC_PAGESIZE);
    aligned_size = ((SIZE + page_size - 1) / page_size) * page_size;

    ptr = aligned_alloc(4096, aligned_size);
    if (!ptr) {
        std::cerr << "aligned_alloc failed\n";
        close(shm_fd);
        return 1;
    }

    // Map shared memory to pinned pointer
    h_data = mmap(ptr, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, shm_fd, 0);
    if (h_data == MAP_FAILED) {
        perror("mmap");
        cudaHostUnregister(ptr);
        free(ptr);
        close(shm_fd);
        return 1;
    }

    // Print message from server
    while (true)
    {
        std::cout << "Shared memory message: " << static_cast<char*>(h_data) << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    

    // wait 20 seconds
    std::cout << "Waiting for 5 seconds before freeing shared memory...\n";
    std::this_thread::sleep_for(std::chrono::seconds(20));
    std::cout << "Freeing shared memory...\n";

    std::cout << "Hello World" << std::endl; 

    std::cout << "Shared memory message: " << static_cast<char*>(h_data) << "\n";

    return 0;
}
