#include <infiniband/verbs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <memory>
#include <random>
#include "rdma_context.h"

#define MAX_FILENAME_SIZE 20

void parse_arguments(int argc, char **argv, uint16_t *tcp_port, char* filename)
{
    if (argc < 3) {
        printf("usage: %s <tcp_port> <file_name>\n", argv[0]);
        exit(1);
    }
    *tcp_port = atoi(argv[1]);
    strcpy(filename, argv[2]);
}


int main(int argc, char *argv[]) {
    uint16_t tcp_port;
    char* filename = (char*) malloc(MAX_FILENAME_SIZE*sizeof(char));

    parse_arguments(argc, argv, &tcp_port, filename);
    if (!tcp_port) {
        printf("usage: %s <tcp port>\n", argv[0]);
        exit(1);
    }

    auto client = std::make_unique<rdma_client_context>(tcp_port);
    bool file_sent = client->send_file(1, filename);

    return 0;
}
