#include <sys/ipc.h>
#include <sys/shm.h>
#include <cstring>
#include <unistd.h>
#include <iostream>

int main() {

    int id = shmget(1234, 4096, 0640);
    if (id < 0) {
        perror("shmget");
        return 1;
    }

    char* ptr = (char*)shmat(id, nullptr, 0);
    if (ptr == (char*)-1) {
        perror("shmat");
        return 1;
    }

    std::cout << "Reader read: " << ptr << "\n";
    shmdt(ptr);

    shmctl(id, IPC_RMID, nullptr);
    return 0;

}