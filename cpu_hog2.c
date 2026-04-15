#include <stdio.h>

int main() {
    volatile long long i = 0;
    while (1) i++;
    return 0;
}
