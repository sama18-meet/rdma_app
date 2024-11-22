#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include <memory>
#include <vector>

#include <infiniband/verbs.h>

#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <infiniband/verbs.h>

#include <algorithm>
#include <cassert>



#include "settings.h"



/* Data to exchange between client and server for communication */
struct connection_establishment_data {
    ibv_gid gid;
    int qpn;
};

struct file_request
{
    int request_id; /* Returned to the client via RDMA write immediate value; use -1 to terminate */
    int rkey;
    int length;
    uint64_t addr;
};



class rdma_context
{
protected:
    uint16_t tcp_port;
    int socket_fd; /* Connected socket for TCP connection */

    /* InfiniBand/verbs resources */
    struct ibv_context *context = nullptr;
    struct ibv_pd *pd = nullptr;
    struct ibv_qp *qp = nullptr;
    struct ibv_cq *cq = nullptr;

    std::array<file_request, MAX_NUM_REQUESTS> requests; /* Array of outstanding requests received from the network */
    struct ibv_mr *mr_requests = nullptr; /* Memory region for RPC requests */

    void initialize_verbs(const char *device_name);
    void send_over_socket(void *buffer, size_t len);
    void recv_over_socket(void *buffer, size_t len);
    void send_connection_establishment_data();
    connection_establishment_data recv_connection_establishment_data();
    static void print_connection_establishment_data(const char *type, const connection_establishment_data& data);
    void connect_qp(const connection_establishment_data& remote_info);

    /* Post a receive buffer of the given index (from the requests array) to the receive queue */
    void post_recv(int index = -1);

    /* Helper function to post an asynchronous RDMA Read request */
    void post_rdma_read(void *local_dst, uint32_t len, uint32_t lkey,
                        uint64_t remote_src, uint32_t rkey, uint64_t wr_id);
    void post_rdma_write(uint64_t remote_dst, uint32_t len, uint32_t rkey,
			 void *local_src, uint32_t lkey, uint64_t wr_id,
			 uint32_t *immediate = NULL);
    bool poll_cq();

public:
    explicit rdma_context(uint16_t tcp_port);
    ~rdma_context();
};

/* Abstract server class for RPC and remote queue servers */
class rdma_server_context : public rdma_context
{
private:
    int listen_fd; /* Listening socket for TCP connection */

public:
    explicit rdma_server_context(uint16_t tcp_port);

    ~rdma_server_context();
    void receive_file();
    char *file;

protected:
    void tcp_connection();

    struct ibv_mr *mr_file;
};

/* Abstract client class for RPC and remote queue parts of the exercise */
class rdma_client_context : public rdma_context
{
private:

public:
    explicit rdma_client_context(uint16_t tcp_port);

    ~rdma_client_context();

    bool send_file(int file_id, char *filename);

protected:
    void tcp_connection();
};



