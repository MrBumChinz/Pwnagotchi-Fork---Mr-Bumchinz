/* Quick test: manually run pcapng_convert_directory */
#include <stdio.h>
#include "pcapng_gps.h"

int main(void) {
    printf("Testing pcapng_gps converter...\n");
    int n = pcapng_convert_directory("/home/pi/handshakes");
    printf("Converted %d files\n", n);
    return 0;
}
