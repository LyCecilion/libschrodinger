/* SIGBUS (BUS_ADRERR): touch a page past the end of a mapped file -> "read". */
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/tmp/libschrodinger_bus", O_RDWR | O_CREAT | O_TRUNC, 0600);
    ftruncate(fd, 4096);
    char *p = (char *)mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    volatile char x = p[4096];
    (void)x;
    return 0;
}
