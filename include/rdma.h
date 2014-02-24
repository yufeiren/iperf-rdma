/*--------------------------------------------------------------- 
 * Copyright (c) 2010                              
 * Stony Brook University
 * All Rights Reserved.                                           
 *--------------------------------------------------------------- 
 * Permission is hereby granted, free of charge, to any person    
 * obtaining a copy of this software (Iperf) and associated       
 * documentation files (the "Software"), to deal in the Software  
 * without restriction, including without limitation the          
 * rights to use, copy, modify, merge, publish, distribute,        
 * sublicense, and/or sell copies of the Software, and to permit     
 * persons to whom the Software is furnished to do
 * so, subject to the following conditions: 
 *
 *     
 * Redistributions of source code must retain the above 
 * copyright notice, this list of conditions and 
 * the following disclaimers. 
 *
 *     
 * Redistributions in binary form must reproduce the above 
 * copyright notice, this list of conditions and the following 
 * disclaimers in the documentation and/or other materials 
 * provided with the distribution. 
 * 
 *     
 * Neither the names of the University of Illinois, NCSA, 
 * nor the names of its contributors may be used to endorse 
 * or promote products derived from this Software without
 * specific prior written permission. 
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES 
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND 
 * NONINFRINGEMENT. IN NO EVENT SHALL THE CONTIBUTORS OR COPYRIGHT 
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, 
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. 
 * ________________________________________________________________
 * National Laboratory for Applied Network Research 
 * National Center for Supercomputing Applications 
 * University of Illinois at Urbana-Champaign 
 * http://www.ncsa.uiuc.edu
 * ________________________________________________________________ 
 *
 * rdma.h
 * by Yufei Ren <renyufei83@gmail.com>
 * -------------------------------------------------------------------
 * An abstract class for waiting on a condition variable. If
 * threads are not available, this does nothing.
 * ------------------------------------------------------------------- */

#ifndef IPERF_RDMA_H
#define IPERF_RDMA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <rdma/rdma_cma.h>
#include <infiniband/arch.h>

extern int PseudoSock;
extern pthread_mutex_t PseudoSockCond;

// rdma_listen backlog
#define IPERF_RDMA_MAX_LISTEN_BACKLOG		32
#define IPERF_RDMA_MAX_IO_DEPTH			512

// RDMA opcode: -k
typedef enum RDMAOpcode {
    kRDMAOpc_RDMA_Write,
    kRDMAOpc_RDMA_Read,
    kRDMAOpc_Send_Recv,
    kRDMAOpc_Unknown
} RDMAOpcode;

typedef enum RDMAConnState {
    IDLE = 1,
    CONNECT_REQUEST,
    ADDR_RESOLVED,
    ROUTE_RESOLVED,
    CONNECTED,
    DISCONNECTED,
    CONN_STATE_UNKNOWN
} RDMAConnState;

struct remote_u {
    uint64_t buf;
    uint32_t rkey;
    uint32_t size;
};

struct iperf_rdma_control_msg {
    uint32_t mode;    /* channel semantic or memory semantic */
    uint32_t iodepth; /* client: io depth
                       * server: number of records for memory semantic
                       */
    uint32_t size;
    struct remote_u rmt_us[IPERF_RDMA_MAX_IO_DEPTH];
};

// rdma io unit
struct iperf_rdma_io_u {
    uint64_t wr_id;
    int size;
    char *addr;
    struct ibv_mr *mr;
    struct ibv_send_wr sq_wr;
    struct ibv_recv_wr rq_wr;
    struct ibv_sge rdma_sgl;
};

/*
 * RDMA Control block
 */
typedef struct rdma_Ctrl_Blk {
    int rdma_iodepth;
    RDMAOpcode rdma_opcode;
    int buflen;

    struct rdma_event_channel *cm_channel;
    // Listening id for Listerner, connection id for Client and Server
    struct rdma_cm_id *cm_id;

    struct ibv_comp_channel *cq_channel;
    struct ibv_cq *cq;
    struct ibv_pd *pd;
    struct ibv_qp *qp;

    struct ibv_recv_wr rq_wr;	/* recv work request record */
    struct ibv_sge recv_sgl;	/* recv single SGE */
    struct iperf_rdma_control_msg recv_buf;
    struct ibv_mr *recv_mr;		/* MR associated with this buffer */

    struct ibv_send_wr sq_wr;	/* send work request record */
    struct ibv_sge send_sgl;
    struct iperf_rdma_control_msg send_buf;/* single send buf */
    struct ibv_mr *send_mr;

    char *rdma_buf;
    struct iperf_rdma_io_u io_us[IPERF_RDMA_MAX_IO_DEPTH];

    struct sockaddr_storage sin;
    uint16_t port;
    int validate;

    FILE* outputfile;

} rdma_Ctrl_Blk;


void rdma_init( rdma_Ctrl_Blk *cb );

int iperf_setup_qp( rdma_Ctrl_Blk *cb );

void iperf_free_qp( rdma_Ctrl_Blk *cb );

int iperf_setup_control_msg(rdma_Ctrl_Blk *cb);

void iperf_setup_local_buf(rdma_Ctrl_Blk *cb);

int get_next_channel_event(struct rdma_event_channel *channel,
			   enum rdma_cm_event_type wait_event);

int iperf_rdma_poll_wait_control_msg(rdma_Ctrl_Blk *cb, \
					    enum ibv_wc_opcode opcode, \
					    struct ibv_wc *wc);

int iperf_rdma_poll_comp(rdma_Ctrl_Blk *cb, int in_flight, struct ibv_wc *wc);

void iperf_rdma_setup_credit(rdma_Ctrl_Blk *cb);

void iperf_rdma_setup_sendbuf(rdma_Ctrl_Blk *cb);

void iperf_rdma_setup_recvbuf(rdma_Ctrl_Blk *cb);

int iperf_rdma_post_all_recvbuf(rdma_Ctrl_Blk *cb);

#ifdef __cplusplus
} /* end extern "C" */
#endif

#endif
