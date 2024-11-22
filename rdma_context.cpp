#include "rdma_context.h"


static void print_file_request(file_request* req) {
    printf("file request:\n\trequest_id=%d, rkey=%d, length=%d, addr=%p\n", req->request_id, req->rkey, req->length, (void*)req->addr);
}

rdma_context::rdma_context(uint16_t tcp_port) : tcp_port(tcp_port) {}

rdma_context::~rdma_context()
{
    /* cleanup */
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr_requests);
    ibv_dealloc_pd(pd);
    ibv_close_device(context);

    /* we don't need TCP anymore. kill the socket */
    close(socket_fd);
}

void rdma_context::initialize_verbs(const char *device_name)
{
    printf("initializing ibverbs with device: %s\n", device_name);

    /* get device list */
    struct ibv_device **device_list = ibv_get_device_list(NULL);
    if (!device_list) {
        perror("ibv_get_device_list failed");
        exit(1);
    }

    /* select device to work with */
    struct ibv_device *requested_dev = nullptr;
    for (int i = 0; device_list[i]; ++i)
        if (strcmp(device_list[i]->name, device_name)) {
            requested_dev = device_list[i];
            break;
        }
    if (!requested_dev)
        requested_dev = device_list[0];
    if (!requested_dev) {
        printf("Unable to find RDMA device '%s'\n", device_name);
        exit(1);
    }

    context = ibv_open_device(requested_dev);
    printf("    ibv context ptr:	%p\n", context);

    ibv_free_device_list(device_list);

    /* create protection domain (PD) */
    pd = ibv_alloc_pd(context);
    if (!pd) {
        perror("ibv_alloc_pd() failed");
        exit(1);
    }
    printf("    pd ptr:			%p\n", pd);

    /* allocate a memory region for the file requests. */
    mr_requests = ibv_reg_mr(pd, requests.begin(), sizeof(file_request) * MAX_NUM_REQUESTS, IBV_ACCESS_LOCAL_WRITE);
    if (!mr_requests) {
        perror("ibv_reg_mr() failed for requests");
        exit(1);
    }
    printf("    file request mr ptr:	%p\n", mr_requests);

    /* create completion queue (CQ). We'll use same CQ for both send and receive parts of the QP */
    cq = ibv_create_cq(context, 2 * MAX_NUM_REQUESTS, NULL, NULL, 0); /* create a CQ with place for two completions per request */
    if (!cq) {
        perror("ibv_create_cq() failed");
        exit(1);
    }
    printf("    send & recv cq ptr:	%p\n", cq);

    /* create QP */
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(struct ibv_qp_init_attr));
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.qp_type = IBV_QPT_RC; /* we'll use RC transport service, which supports RDMA */
    qp_init_attr.cap.max_send_wr = MAX_NUM_REQUESTS; /* max of 1 WQE in-flight in SQ per request. that's enough for us */
    qp_init_attr.cap.max_recv_wr = MAX_NUM_REQUESTS; /* max of 1 WQE in-flight in RQ per request. that's enough for us */
    qp_init_attr.cap.max_send_sge = 1; /* 1 SGE in each send WQE */
    qp_init_attr.cap.max_recv_sge = 1; /* 1 SGE in each recv WQE */
    qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) {
        perror("ibv_create_qp() failed");
        exit(1);
    }
    printf("    qp ptr:			%p\n", qp);
}

void rdma_context::send_over_socket(void *buffer, size_t len)
{
    int ret = send(socket_fd, buffer, len, 0);
    if (ret < 0) {
        perror("send");
        exit(1);
    }
}

void rdma_context::recv_over_socket(void *buffer, size_t len)
{
    int ret = recv(socket_fd, buffer, len, 0);
    if (ret < 0) {
        perror("recv");
        exit(1);
    }
}

void rdma_context::send_connection_establishment_data()
{
    /* ok, before we continue we need to get info about the client' QP, and send it info about ours.
     * namely: QP number, and LID/GID.
     * we'll use the TCP socket for that */

    struct connection_establishment_data my_info = {};
    int ret;

    /* For RoCE, GID (IP address) must by used */
    // the third param is the gid table idx. We pass 0 because we want to query
    // our own port
    ret = ibv_query_gid(context, IB_PORT, 0, &my_info.gid);
    if (ret) {
        perror("ibv_query_gid() failed");
        exit(1);
    }
    my_info.qpn = qp->qp_num;
    send_over_socket(&my_info, sizeof(connection_establishment_data));
    print_connection_establishment_data("local ", my_info);
}

connection_establishment_data rdma_context::recv_connection_establishment_data()
{
    /* ok, before we continue we need to get info about the client' QP
     * namely: QP number, and LID/GID. */

    struct connection_establishment_data remote_info;

    recv_over_socket(&remote_info, sizeof(connection_establishment_data));

    print_connection_establishment_data("remote", remote_info);

    return remote_info;
}

void rdma_context::print_connection_establishment_data(const char *type, const connection_establishment_data& data)
{
    char address[INET6_ADDRSTRLEN];

    inet_ntop(AF_INET6, &data.gid, address, sizeof(address));

    printf("%s address:  %s, QPN 0x%06x\n", type, address, data.qpn);
}

void rdma_context::connect_qp(const connection_establishment_data &remote_info)
{
    /* this is a multi-phase process, moving the state machine of the QP step by step
     * until we are ready */
    struct ibv_qp_attr qp_attr;

    /*QP state: RESET -> INIT */
    memset(&qp_attr, 0, sizeof(struct ibv_qp_attr));
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = IB_PORT;
    qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ; /* we'll allow client to RDMA write and read on this QP */
    int ret = ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret) {
        perror("ibv_modify_qp() to INIT failed");
        exit(1);
    }

    /*QP: state: INIT -> RTR (Ready to Receive) */
    memset(&qp_attr, 0, sizeof(struct ibv_qp_attr));
    qp_attr.qp_state = IBV_QPS_RTR;
    qp_attr.path_mtu = IBV_MTU_1024;
    qp_attr.dest_qp_num = remote_info.qpn; /* qp number of the remote side */
    qp_attr.rq_psn      = 0 ;
    qp_attr.max_dest_rd_atomic = 1; /* max in-flight RDMA reads */
    qp_attr.min_rnr_timer = 12;
    qp_attr.ah_attr.grh.dgid = remote_info.gid; /* GID (L3 address) of the remote side */
    qp_attr.ah_attr.grh.sgid_index = GID_ID;
    qp_attr.ah_attr.grh.hop_limit = 1;
    qp_attr.ah_attr.is_global = 1;
    qp_attr.ah_attr.sl = 0;
    qp_attr.ah_attr.src_path_bits = 0;
    qp_attr.ah_attr.port_num = IB_PORT;
    ret = ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (ret) {
        perror("ibv_modify_qp() to RTR failed");
        exit(1);
    }

    /*QP: state: RTR -> RTS (Ready to Send) */
    memset(&qp_attr, 0, sizeof(struct ibv_qp_attr));
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = 0;
    qp_attr.timeout = 14;
    qp_attr.retry_cnt = 7; // 7 means infinite
    qp_attr.rnr_retry = 7; // 7 means infinite
    qp_attr.max_rd_atomic = 16;
    ret = ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret) {
        perror("ibv_modify_qp() to RTS failed");
        exit(1);
    }

    /* now let's populate the receive QP with recv WQEs */
    for (int i = 0; i < MAX_NUM_REQUESTS; i++) {
        post_recv(i);
    }
}

void rdma_context::post_recv(int index)
{
    struct ibv_recv_wr recv_wr = {}, *bad_wr; /* this is the receive work request (the verb's representation for receive WQE) */
    ibv_sge sgl = {};

    recv_wr.wr_id = index;
    if (index >= 0) {
        sgl.addr = (uintptr_t)&requests[index];
        sgl.length = sizeof(requests[0]);
        sgl.lkey = mr_requests->lkey;
    }
    recv_wr.sg_list = &sgl;
    recv_wr.num_sge = 1;
    if (int ret = ibv_post_recv(qp, &recv_wr, &bad_wr)) {
	errno = ret;
        perror("ibv_post_recv() failed");
        exit(1);
    }
}

void rdma_context::post_rdma_read(void *local_dst, uint32_t len, uint32_t lkey, uint64_t remote_src, uint32_t rkey, uint64_t wr_id)
{
    ibv_sge sgl = {
        (uint64_t)(uintptr_t)local_dst,
        len,
        lkey
    };

    ibv_send_wr send_wr = {};
    ibv_send_wr *bad_send_wr;

    send_wr.opcode = IBV_WR_RDMA_READ;
    send_wr.wr_id = wr_id;
    send_wr.sg_list = &sgl;
    send_wr.num_sge = 1;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.wr.rdma.remote_addr = remote_src;
    send_wr.wr.rdma.rkey = rkey;

    if (ibv_post_send(qp, &send_wr, &bad_send_wr)) {
	perror("ibv_post_send() failed");
	exit(1);
    }
}

void rdma_context::post_rdma_write(uint64_t remote_dst, uint32_t len, uint32_t rkey,
		     void *local_src, uint32_t lkey, uint64_t wr_id,
		     uint32_t *immediate)
{
    ibv_sge sgl = {
        (uint64_t)(uintptr_t)local_src,
        len,
        lkey
    };

    ibv_send_wr send_wr = {};
    ibv_send_wr *bad_send_wr;

    if (immediate) {
        send_wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
        send_wr.imm_data = *immediate;
    } else {
        send_wr.opcode = IBV_WR_RDMA_WRITE;
    }
    send_wr.wr_id = wr_id;
    send_wr.sg_list = &sgl;
    send_wr.num_sge = 1;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.wr.rdma.remote_addr = remote_dst;
    send_wr.wr.rdma.rkey = rkey;

    if (ibv_post_send(qp, &send_wr, &bad_send_wr)) {
	perror("ibv_post_send() failed");
	exit(1);
    }
}

bool rdma_context::poll_cq() {
    struct ibv_wc wc;
    int num_completions;

    // Poll the CQ until an event is received
    while ((num_completions = ibv_poll_cq(cq, 1, &wc)) == 0);

    if (num_completions < 0) {
        perror("Error polling CQ");
    }

    if (wc.status == IBV_WC_SUCCESS) {
        printf("RDMA Read completed successfully!\n");
        return true;
    } else {
        fprintf(stderr, "RDMA Read failed: %d\n", wc.status);
        return false;
    }
}

////////////////////////////////////////////////////////////////////////
//////////////////////////// SERVER CONTEXT ////////////////////////////
////////////////////////////////////////////////////////////////////////

rdma_server_context::rdma_server_context(uint16_t tcp_port) :
    rdma_context(tcp_port)
{
    /* Create a TCP connection to exchange InfiniBand parameters */
    tcp_connection();

    /* Open up some InfiniBand resources */
    initialize_verbs(IB_DEVICE_NAME);

    /* exchange InfiniBand parameters with the server */
    connection_establishment_data client_info = recv_connection_establishment_data();
    send_connection_establishment_data();

    /* now need to connect the QP to the client's QP. */
    connect_qp(client_info);
}

rdma_server_context::~rdma_server_context()
{
    ibv_dereg_mr(mr_file);
    free(file);
    close(listen_fd);
}

void rdma_server_context::tcp_connection()
{
    /* setup a TCP connection for initial negotiation with client */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        perror("socket");
        exit(1);
    }
    listen_fd = lfd;

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(struct sockaddr_in));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(tcp_port);

    int one = 1;
    if (setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) {
        perror("SO_REUSEADDR");
        exit(1);
    }

    if (bind(lfd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in)) < 0) {
        perror("bind");
        exit(1);
    }

    if (listen(lfd, 1)) {
        perror("listen");
        exit(1);
    }

    printf("Server waiting on port %d. Client can connect\n", tcp_port);

    int sfd = accept(lfd, NULL, NULL);
    if (sfd < 0) {
        perror("accept");
        exit(1);
    }
    printf("client connected successfully\n");
    socket_fd = sfd;
}


void rdma_server_context::receive_file()  {


    file_request *req = (file_request*) malloc(sizeof(file_request));
    recv_over_socket(req, sizeof(file_request));

    file = (char*) malloc(req->length+1);

    print_file_request(req);

    /* register a memory region for the input / output images. */
    mr_file = ibv_reg_mr(pd, file, req->length, IBV_ACCESS_REMOTE_READ| IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr_file) {
        perror("ibv_reg_mr() in server failed for file");
        exit(1);
    }

    post_rdma_read(
        file,               // local_src
        req->length,  // len
        mr_file->lkey,      // lkey
        req->addr,    // remote_dst
        req->rkey,    // rkey
        1);          // wr_id

    bool done = poll_cq();
    if (!done) {
        perror("server: poll_cq returned false\n");
    }
}

////////////////////////////////////////////////////////////////////////
//////////////////////////// CLIENT CONTEXT ////////////////////////////
////////////////////////////////////////////////////////////////////////

rdma_client_context::rdma_client_context(uint16_t tcp_port) :
    rdma_context(tcp_port)
{
    /* Create a TCP connection to exchange InfiniBand parameters */
    tcp_connection();

    /* Open up some InfiniBand resources */
    initialize_verbs(IB_DEVICE_NAME);

    /* exchange InfiniBand parameters with the client */
    send_connection_establishment_data();
    connection_establishment_data server_info = recv_connection_establishment_data();

    /* now need to connect the QP to the client's QP. */
    connect_qp(server_info);

}

rdma_client_context::~rdma_client_context()
{
}

void rdma_client_context::tcp_connection()
{
    /* first we'll connect to server via a TCP socket to exchange InfiniBand parameters */
    int sfd;
    sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        exit(1);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_addr.s_addr = inet_addr(IP);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(tcp_port);

    if (connect(sfd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in)) < 0) {
        perror("connect");
        exit(1);
    }

    printf("TCP connection established with server %s successfully\n", IP);
    socket_fd = sfd;
}


bool rdma_client_context::send_file(int file_id, char *filename)  {


    char * buffer = 0;
    long length;
    FILE * f = fopen (filename, "rb");

    if (f)
    {
      fseek (f, 0, SEEK_END);
      length = ftell (f);
      fseek (f, 0, SEEK_SET);
      int res = posix_memalign((void**)&buffer, 4096, length);
      if (buffer)
      {
        fread (buffer, 1, length, f);
      }
      fclose (f);
    }
    struct ibv_mr *mr_file = ibv_reg_mr(pd, buffer, length, IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
    if (!mr_file) {
        perror("ibv_reg_mr() in client failed for file");
        exit(1);
    }
    printf("buf content that will be sent: %s\n", buffer);

    struct file_request req;
    req.request_id = 1;
    req.rkey = mr_file->rkey;
    req.length = length;
    req.addr = (uint64_t) buffer;

    send_over_socket(&req, sizeof(file_request));

    print_file_request(&req);

    return true;

}
