#include <unistd.h>
int main(void) {
    write(1, "root\n", 5);
    return 0;
}
