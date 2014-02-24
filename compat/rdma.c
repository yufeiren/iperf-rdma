/*--------------------------------------------------------------- 
 * Copyright (c) 2013                              
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
 * rdma.c
 * by Yufei Ren <yufei.ren@stonybrook.edu>
 * -------------------------------------------------------------------
 * An abstract class for waiting on a condition variable. If
 * threads are not available, this does nothing.
 * ------------------------------------------------------------------- */

#include "headers.h"
#include "rdma.h"
#include "util.h"

int get_next_channel_event(struct rdma_event_channel *channel,
			   enum rdma_cm_event_type wait_event)
{
    struct rdma_cm_event *event;
    int rc;

    rc = rdma_get_cm_event(channel, &event);
    WARN( rc == RDMACM_ERROR, "rdma_get_cm_event" );

    if (event->event != wait_event) {
        WARN( 1, "event type is not match" );
        return RDMACM_ERROR;
    }

    rc  = rdma_ack_cm_event(event);
    WARN( rc == RDMACM_ERROR, "rdma_ack_cm_event" );

    return rc;
}

// setup queue pair and completion queue
int iperf_setup_qp(rdma_Ctrl_Blk *cb)
{
    int rc;
    struct rdma_cm_id *cm_id = cb->cm_id;
    struct ibv_qp_init_attr init_attr;

    cb->pd = ibv_alloc_pd(cm_id->verbs);
    WARN( cb->pd == NULL, "ibv_alloc_pd" );

    cb->cq_channel = ibv_create_comp_channel(cm_id->verbs);
    WARN( cb->cq_channel == NULL, "ibv_create_comp_channel" );
    if (cb->cq_channel == NULL)
	    goto err1;

    // rdma_iodepth on server side is 0 at this time
    cb->cq = ibv_create_cq(cm_id->verbs, IPERF_RDMA_MAX_IO_DEPTH * 2, cb,
			   cb->cq_channel, 0);
    WARN( cb->cq == NULL, "ibv_create_cq" );
    if (cb->cq == NULL)
	    goto err2;

    // Request notification before any completion can be created
    rc = ibv_req_notify_cq(cb->cq, 0);
    WARN_errno( rc != 0, "ibv_req_notify_cq" );
    if (rc != 0)
	    goto err3;

    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.cap.max_send_wr = IPERF_RDMA_MAX_IO_DEPTH;
    init_attr.cap.max_recv_wr = IPERF_RDMA_MAX_IO_DEPTH * 2;
    init_attr.cap.max_recv_sge = 1;
    init_attr.cap.max_send_sge = 1;
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.send_cq = cb->cq;
    init_attr.recv_cq = cb->cq;

    rc = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
    WARN( rc == RDMAIBV_ERROR, "rdma_create_qp" );
    cb->qp = cb->cm_id->qp; // look wierd and ugly to combine rdmacm and ibverbs together

    return rc;

err3:
    ibv_destroy_cq(cb->cq);
err2:
    ibv_destroy_comp_channel(cb->cq_channel);
err1:
    ibv_dealloc_pd(cb->pd);
    return RDMAIBV_ERROR;
}


void iperf_free_qp(rdma_Ctrl_Blk *cb)
{
    ibv_destroy_qp(cb->qp);
    ibv_destroy_cq(cb->cq);
    ibv_destroy_comp_channel(cb->cq_channel);
    ibv_dealloc_pd(cb->pd);
}


int iperf_setup_control_msg(rdma_Ctrl_Blk *cb)
{
    cb->recv_mr = ibv_reg_mr(cb->pd, &cb->recv_buf, sizeof(cb->recv_buf),
			     IBV_ACCESS_LOCAL_WRITE);
    WARN( cb->recv_mr == NULL, "ibv_reg_mr" );

    cb->send_mr = ibv_reg_mr(cb->pd, &cb->send_buf, sizeof(cb->send_buf),
			     0);
    WARN( (cb->send_mr == NULL), "ibv_reg_mr" );

    /* setup work request */
    /* recv wq */
    cb->recv_sgl.addr = (uint64_t) (unsigned long)&cb->recv_buf;
    cb->recv_sgl.length = sizeof(cb->recv_buf);
    cb->recv_sgl.lkey = cb->recv_mr->lkey;
    cb->rq_wr.sg_list = &cb->recv_sgl;
    cb->rq_wr.num_sge = 1;
    cb->rq_wr.wr_id = IPERF_RDMA_MAX_IO_DEPTH;

    /* send wq */
    cb->send_sgl.addr = (uint64_t) (unsigned long)&cb->send_buf;
    cb->send_sgl.length = sizeof(cb->send_buf);
    cb->send_sgl.lkey = cb->send_mr->lkey;

    cb->sq_wr.opcode = IBV_WR_SEND;
    cb->sq_wr.send_flags = IBV_SEND_SIGNALED;
    cb->sq_wr.sg_list = &cb->send_sgl;
    cb->sq_wr.num_sge = 1;
    cb->sq_wr.wr_id = IPERF_RDMA_MAX_IO_DEPTH;

    return 0;
}


void iperf_setup_local_buf(rdma_Ctrl_Blk *cb)
{
    int i;
    struct iperf_rdma_io_u *io_u;

    // setup local buffers
    cb->rdma_buf = malloc( cb->buflen * cb->rdma_iodepth );
    WARN( cb->rdma_buf == NULL, "malloc" );
    for ( i = 0; i < cb->rdma_iodepth; i ++ ) {
        io_u = &cb->io_us[i];
        io_u->wr_id = i;
        io_u->size = cb->buflen;
        io_u->addr = cb->rdma_buf + i * cb->buflen;
	switch (cb->rdma_opcode) {
	case kRDMAOpc_RDMA_Write:
	case kRDMAOpc_RDMA_Read:
            io_u->mr = ibv_reg_mr(cb->pd, io_u->addr, io_u->size,
				  IBV_ACCESS_LOCAL_WRITE
                                | IBV_ACCESS_REMOTE_READ
                                | IBV_ACCESS_REMOTE_WRITE);
	    break;
	case kRDMAOpc_Send_Recv:
            // should be local write
            io_u->mr = ibv_reg_mr(cb->pd, io_u->addr, io_u->size,
				  IBV_ACCESS_LOCAL_WRITE);
	    break;
	default:
            WARN( 1, "Unknown rdma opcode" );
            break;
	}
        WARN( io_u->mr == NULL, "ibv_reg_mr" );
        io_u->rdma_sgl.addr = (uint64_t) (unsigned long) io_u->addr;
        io_u->rdma_sgl.lkey = io_u->mr->lkey;
        io_u->rdma_sgl.length = io_u->size;
    }
}

/*
 * Return -1 for error and 'nr events' for a positive number
 * of events
 */
int iperf_rdma_poll_wait_control_msg(rdma_Ctrl_Blk *cb, \
				     enum ibv_wc_opcode opcode,	\
				     struct ibv_wc *wc)
{
    struct ibv_cq *ev_cq;
    void *ev_ctx;
    int rc;

    rc = ibv_get_cq_event(cb->cq_channel, &ev_cq, &ev_ctx);
    WARN( rc == RDMAIBV_ERROR, "ibv_get_cq_event" );
    WARN( ev_cq != cb->cq, "Unknown CQ" );
    
    rc = ibv_req_notify_cq(cb->cq, 0);
    WARN_errno( rc != 0, "ibv_req_notify_cq" );

    rc = ibv_poll_cq(cb->cq, 1, wc);
    WARN( rc != 1, "ibv_poll_cq" );
    WARN( (wc->status != 0) && (wc->status != IBV_WC_WR_FLUSH_ERR), \
         "cq completion failed status" );

    WARN( wc->opcode != opcode, "get wrong completion event" );

    ibv_ack_cq_events(cb->cq, 1);

    return rc;
}


int iperf_rdma_poll_comp(rdma_Ctrl_Blk *cb, int in_flight, struct ibv_wc *wc)
{
    struct ibv_cq *ev_cq;
    void *ev_ctx;
    int rc, nr, i;

    rc = ibv_get_cq_event(cb->cq_channel, &ev_cq, &ev_ctx);
    WARN( rc == RDMAIBV_ERROR, "ibv_get_cq_event" );
    WARN( ev_cq != cb->cq, "Unknown CQ" );
    
    rc = ibv_req_notify_cq(cb->cq, 0);
    WARN_errno( rc != 0, "ibv_req_notify_cq" );

again:
    nr = ibv_poll_cq(cb->cq, in_flight, wc);
    WARN( nr < 0, "ibv_poll_cq" );
    if (nr == 0)
        goto again;

    for (i = 0; i < nr; i ++) {
        if( (wc[i].status != 0) && (wc[i].status != IBV_WC_WR_FLUSH_ERR) )
            fprintf(stderr, "cq completion failed: %s\n", \
		    ibv_wc_status_str(wc[i].status));
    }

    ibv_ack_cq_events(cb->cq, nr);

    return nr;
}

void iperf_rdma_setup_credit(rdma_Ctrl_Blk *cb)
{
    int i, nr;
    struct remote_u *rmt_u;
    struct iperf_rdma_control_msg *cm;
    struct iperf_rdma_io_u *io_u;

    cm = &cb->recv_buf;

    nr = ntohl(cm->iodepth);
    WARN( nr < cb->rdma_iodepth, "returned credit is not sufficient" );

    for (i = 0; i < nr; i++) {
        rmt_u = &cm->rmt_us[i];
        io_u = &cb->io_us[i];
	switch (cb->rdma_opcode) {
	case kRDMAOpc_RDMA_Write:
            io_u->sq_wr.wr_id = io_u->wr_id;
            io_u->sq_wr.opcode = IBV_WR_RDMA_WRITE;
            io_u->sq_wr.wr.rdma.remote_addr = ntohll(rmt_u->buf);
            io_u->sq_wr.wr.rdma.rkey = ntohl(rmt_u->rkey);
            io_u->sq_wr.send_flags = IBV_SEND_SIGNALED;
            io_u->sq_wr.sg_list = &io_u->rdma_sgl;
            io_u->sq_wr.num_sge = 1;
            io_u->sq_wr.sg_list->length = ntohl(rmt_u->size);
	    break;
	case kRDMAOpc_RDMA_Read:
            io_u->sq_wr.wr_id = io_u->wr_id;
            io_u->sq_wr.opcode = IBV_WR_RDMA_READ;
            io_u->sq_wr.wr.rdma.remote_addr = ntohll(rmt_u->buf);
            io_u->sq_wr.wr.rdma.rkey = ntohl(rmt_u->rkey);
            io_u->sq_wr.send_flags = IBV_SEND_SIGNALED;
            io_u->sq_wr.sg_list = &io_u->rdma_sgl;
            io_u->sq_wr.num_sge = 1;
            io_u->sq_wr.sg_list->length = ntohl(rmt_u->size);
	    break;
	case kRDMAOpc_Send_Recv:
            io_u->sq_wr.wr_id = io_u->wr_id;
            io_u->sq_wr.opcode = IBV_WR_SEND;
            io_u->sq_wr.send_flags = IBV_SEND_SIGNALED;
            io_u->sq_wr.sg_list = &io_u->rdma_sgl;
            io_u->sq_wr.num_sge = 1;
            io_u->sq_wr.sg_list->length = cb->buflen;
	    break;
        default:
            break;
	}
    }
}

void iperf_rdma_setup_sendbuf(rdma_Ctrl_Blk *cb)
{
    int i;
    struct iperf_rdma_io_u *io_u;

    for (i = 0; i < cb->rdma_iodepth; i++) {
        io_u = &cb->io_us[i];

        io_u->sq_wr.opcode = IBV_WR_SEND;
        io_u->sq_wr.send_flags = IBV_SEND_SIGNALED;
    }
}

void iperf_rdma_setup_recvbuf(rdma_Ctrl_Blk *cb)
{
    int i;
    struct iperf_rdma_io_u *io_u;

    for (i = 0; i < cb->rdma_iodepth; i++) {
        io_u = &cb->io_us[i];
	io_u->wr_id = i;

	io_u->rq_wr.wr_id = io_u->wr_id;
	io_u->rq_wr.next = NULL;
	io_u->rq_wr.sg_list = &io_u->rdma_sgl;
	io_u->rq_wr.num_sge = 1;
	// io_u->rq_wr.sg_list->length = cb->buflen;
    }
}

int iperf_rdma_post_all_recvbuf(rdma_Ctrl_Blk *cb)
{
    int rc, i;
    struct ibv_recv_wr *bad_recv_wr;
    struct iperf_rdma_io_u *io_u;

    for (i = 0; i < cb->rdma_iodepth; i++) {
        io_u = &cb->io_us[i];

        rc = ibv_post_recv(cb->qp, &io_u->rq_wr, &bad_recv_wr);
        WARN_errno( rc != 0, "ibv_post_recv" );
	if (rc != 0)
		return -1;
    }

    return 0;
}

