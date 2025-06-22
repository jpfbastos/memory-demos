#include <sys/ipc.h>
#include <sys/shm.h>
#include <cstring>
#include <unistd.h>
#include <iostream>

int main() {
    // owner, group, others
    int id = shmget(1234, 4096, IPC_CREAT | 0640);
    if (id < 0) {
        perror("shmget failed");
        return 1;
    }

    char* ptr = (char*)shmat(id, nullptr, 0);
    if (ptr == (char*)-1) {
        perror("shmat failed");
        return 1;
    }

    strcpy(ptr, "Writer says hello :)");
    std::cout << "Writer wrote: " << ptr << "\n";
    
    shmdt(ptr);
    return 0;
}
