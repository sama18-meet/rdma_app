#include <infiniband/verbs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <assert.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <memory>
#include "rdma_context.h"

#define TCP_PORT_OFFSET 23456
#define TCP_PORT_RANGE 1000

void parse_arguments(int argc, char **argv, uint16_t *tcp_port)
{
    if (argc < 1) {
        printf("usage: %s [tcp port]\n", argv[0]);
        exit(1);
    }

    if (argc < 2) {
        *tcp_port = 0;
    } else {
        *tcp_port = atoi(argv[1]);
    }
}



int main(int argc, char *argv[]) {

    uint16_t tcp_port;

    parse_arguments(argc, argv, &tcp_port);
    if (!tcp_port) {
        srand(time(NULL));
        tcp_port = TCP_PORT_OFFSET + (rand() % TCP_PORT_RANGE); /* to avoid conflicts with other users of the machine */
    }

    auto server = std::make_unique<rdma_server_context>(tcp_port);
    if (!server) {
        printf("Error creating server context.\n");
        exit(1);
    }
    printf("waiting to receive file...\n");

    server->receive_file();

    printf("file received: %s\n", server->file);

    printf("exiting...\n");

    return 0;
}
