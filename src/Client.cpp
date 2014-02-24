/*
 * This file is extended from iperf for RDMA integration.
 * by Yufei Ren <yufei.ren@stonybrook.edu>
 */

/*--------------------------------------------------------------- 
 * Copyright (c) 1999,2000,2001,2002,2003                              
 * The Board of Trustees of the University of Illinois            
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
 * Client.cpp
 * by Mark Gates <mgates@nlanr.net>
 * -------------------------------------------------------------------
 * A client thread initiates a connect to the server and handles
 * sending and receiving data, then closes the socket.
 * ------------------------------------------------------------------- */

#include <list>
#include "headers.h"
#include "Client.hpp"
#include "Thread.h"
#include "SocketAddr.h"
#include "PerfSocket.hpp"
#include "Extractor.h"
#include "delay.hpp"
#include "util.h"
#include "Locale.h"
#include "rdma.h"

using std::list;

/* -------------------------------------------------------------------
 * Store server hostname, optionally local hostname, and socket info.
 * ------------------------------------------------------------------- */

Client::Client( thread_Settings *inSettings ) {
    mSettings = inSettings;
    mBuf = NULL;

    // initialize buffer
    mBuf = new char[ mSettings->mBufLen ];
    pattern( mBuf, mSettings->mBufLen );
    if ( isFileInput( mSettings ) ) {
        if ( !isSTDIN( mSettings ) )
            Extractor_Initialize( mSettings->mFileName, mSettings->mBufLen, mSettings );
        else
            Extractor_InitializeFile( stdin, mSettings->mBufLen, mSettings );

        if ( !Extractor_canRead( mSettings ) ) {
            unsetFileInput( mSettings );
        }
    }

    // connect TCP/UDP
    if ( mSettings->mThreadMode == kMode_Client ) {
    	Connect( );
    // connect RDMA
    } else if ( mSettings->mThreadMode == kMode_RDMA_Client ) {
        mSettings->mCtrlBlk = new rdma_Ctrl_Blk;
        Settings_Initialize_RDMA( mSettings );

	ConnectRDMA( );
    }
    else
	    FAIL( 1, "Unknown thread mode", mSettings );

    if ( isReport( inSettings ) ) {
        ReportSettings( inSettings );
        if ( mSettings->multihdr && isMultipleReport( inSettings ) ) {
            mSettings->multihdr->report->connection.peer = mSettings->peer;
            mSettings->multihdr->report->connection.size_peer = mSettings->size_peer;
            mSettings->multihdr->report->connection.local = mSettings->local;
            SockAddr_setPortAny( &mSettings->multihdr->report->connection.local );
            mSettings->multihdr->report->connection.size_local = mSettings->size_local;
        }
    }

} // end Client

/* -------------------------------------------------------------------
 * Delete memory (hostname strings).
 * ------------------------------------------------------------------- */

Client::~Client() {
    if ( mSettings->mSock != INVALID_SOCKET ) {
        int rc = close( mSettings->mSock );
        WARN_errno( rc == SOCKET_ERROR, "close" );
        mSettings->mSock = INVALID_SOCKET;
    }
    DELETE_ARRAY( mBuf );
} // end ~Client

const double kSecs_to_usecs = 1e6; 
const int    kBytes_to_Bits = 8; 

void Client::RunTCP( void ) {
    unsigned long currLen = 0; 
    struct itimerval it;
    max_size_t totLen = 0;

    int err;

    char* readAt = mBuf;

    // Indicates if the stream is readable 
    bool canRead = true, mMode_Time = isModeTime( mSettings ); 

    ReportStruct *reportstruct = NULL;

    // InitReport handles Barrier for multiple Streams
    mSettings->reporthdr = InitReport( mSettings );
    reportstruct = new ReportStruct;
    reportstruct->packetID = 0;

    lastPacketTime.setnow();
    if ( mMode_Time ) {
	memset (&it, 0, sizeof (it));
	it.it_value.tv_sec = (int) (mSettings->mAmount / 100.0);
	it.it_value.tv_usec = (int) 10000 * (mSettings->mAmount -
	    it.it_value.tv_sec * 100.0);
	err = setitimer( ITIMER_REAL, &it, NULL );
	if ( err != 0 ) {
	    perror("setitimer");
	    exit(1);
	}
    }
    do {
        // Read the next data block from 
        // the file if it's file input 
        if ( isFileInput( mSettings ) ) {
            Extractor_getNextDataBlock( readAt, mSettings ); 
            canRead = Extractor_canRead( mSettings ) != 0; 
        } else
            canRead = true; 

        // perform write 
        currLen = write( mSettings->mSock, mBuf, mSettings->mBufLen ); 
        if ( currLen < 0 ) {
            WARN_errno( currLen < 0, "write2" ); 
            break; 
        }
	totLen += currLen;

	if(mSettings->mInterval > 0) {
    	    gettimeofday( &(reportstruct->packetTime), NULL );
            reportstruct->packetLen = currLen;
            ReportPacket( mSettings->reporthdr, reportstruct );
        }

        if ( !mMode_Time ) {
            /* mAmount may be unsigned, so don't let it underflow! */
            if( mSettings->mAmount >= currLen ) {
                mSettings->mAmount -= currLen;
            } else {
                mSettings->mAmount = 0;
            }
        }

    } while ( ! (sInterupted  || 
                   (!mMode_Time  &&  0 >= mSettings->mAmount)) && canRead ); 

    // stop timing
    gettimeofday( &(reportstruct->packetTime), NULL );

    // if we're not doing interval reporting, report the entire transfer as one big packet
    if(0.0 == mSettings->mInterval) {
        reportstruct->packetLen = totLen;
        ReportPacket( mSettings->reporthdr, reportstruct );
    }
    CloseReport( mSettings->reporthdr, reportstruct );

    DELETE_PTR( reportstruct );
    EndReport( mSettings->reporthdr );
}


void Client::RunRDMA( void ) {

    unsigned long currLen = 0; 
    struct itimerval it;
    max_size_t totLen = 0;

    int err;
    int rc, i;

    struct ibv_wc *wc;
    struct ibv_send_wr* bad_wr;
    
    // Indicates if the stream is readable 
    bool canRead = true, mMode_Time = isModeTime( mSettings ); 

    ReportStruct *reportstruct = NULL;

    list<struct iperf_rdma_io_u *> io_flight_list;
    list<struct iperf_rdma_io_u *> io_completed_list;

    rdma_Ctrl_Blk *cb = mSettings->mCtrlBlk;

    struct iperf_rdma_io_u *io_u;
    int nr;

    wc = (struct ibv_wc *) malloc ( cb->rdma_iodepth * \
				    sizeof (struct ibv_wc) );
    FAIL_errno( wc == NULL, "malloc", mSettings );
    memset(wc, '\0', cb->rdma_iodepth * sizeof (struct ibv_wc));

    // InitReport handles Barrier for multiple Streams
    mSettings->reporthdr = InitReport( mSettings );
    reportstruct = new ReportStruct;
    reportstruct->packetID = 0;

    lastPacketTime.setnow();
    if ( mMode_Time ) {
	memset (&it, 0, sizeof (it));
	it.it_value.tv_sec = (int) (mSettings->mAmount / 100.0);
	it.it_value.tv_usec = (int) 10000 * (mSettings->mAmount -
	    it.it_value.tv_sec * 100.0);
	err = setitimer( ITIMER_REAL, &it, NULL );
	if ( err != 0 ) {
	    perror("setitimer");
	    exit(1);
	}
    }

    for (i = 0; i < cb->rdma_iodepth; i++) {
        io_completed_list.push_back(&cb->io_us[i]);
    }

    // asynchronous I/O performing

    do {
        // if not fillup the io depth
        // submit more task
        while (io_flight_list.size() < (unsigned) cb->rdma_iodepth) {
            if (io_completed_list.empty())
	        FAIL( 1, "the completed list should no be empty", mSettings );
            io_u = io_completed_list.front();
            io_completed_list.pop_front();

	    // load data from input file
            if ( isFileInput( mSettings ) 
                && ( (mSettings->rdma_opcode == kRDMAOpc_RDMA_Write) ||
                (mSettings->rdma_opcode == kRDMAOpc_Send_Recv) ) ) {
                Extractor_getNextDataBlock( io_u->addr, mSettings );
                canRead = Extractor_canRead( mSettings ) != 0;
	    } else {
                canRead = true;
	    }

	    rc = ibv_post_send(cb->qp, &io_u->sq_wr, &bad_wr);
	    WARN_errno( rc != 0, "ibv_post_send");

	    io_flight_list.push_back(io_u);
        }

        // reap at least one task
again:
        nr = iperf_rdma_poll_comp(cb, io_flight_list.size(), wc);
        if (nr == 0)
            goto again;

        currLen = 0;
	for ( i = 0; i < nr; i++ ) {
            // find each one in inflight list
	    // comments: may be improved with binary search
            io_u = NULL;
            for (list<struct iperf_rdma_io_u *>::iterator it = io_flight_list.begin(); it != io_flight_list.end(); ++it) {
                if (wc[i].wr_id == ((struct iperf_rdma_io_u *)(*it))->wr_id) {
                    io_u = *it;
                    break;
		}
	    }
	    FAIL( io_u == NULL, "can not find work request", mSettings );

	    io_flight_list.remove(io_u);

	    currLen += io_u->size;
	    io_completed_list.push_back(io_u);
	}

	totLen += currLen;
	
	if( mSettings->mInterval > 0 ) {
    	    gettimeofday( &(reportstruct->packetTime), NULL );
            reportstruct->packetLen = currLen;
            ReportPacket( mSettings->reporthdr, reportstruct );
        }

        if ( !mMode_Time ) {
            /* mAmount may be unsigned, so don't let it underflow! */
            if( mSettings->mAmount >= currLen ) {
                mSettings->mAmount -= currLen;
            } else {
                mSettings->mAmount = 0;
            }
        }

    } while ( ! (sInterupted  || 
                   (!mMode_Time  &&  0 >= mSettings->mAmount)) && canRead ); 

    // reap the left events
    while (!io_flight_list.empty()) {
pollagain:
        nr = iperf_rdma_poll_comp(cb, io_flight_list.size(), wc);
        if (nr == 0)
            goto pollagain;

	for ( i = 0; i < nr; i++ ) {
            // find each one in inflight list
            io_u = NULL;
            for (list<struct iperf_rdma_io_u *>::iterator it = io_flight_list.begin(); it != io_flight_list.end(); ++it) {
                if (wc[i].wr_id == ((struct iperf_rdma_io_u *)(*it))->wr_id) {
                    io_u = *it;
                    break;
		}
	    }
	    FAIL( io_u == NULL, "can not find work request", mSettings );

	    io_flight_list.remove(io_u);

	    currLen += io_u->size;
	    io_completed_list.push_back(io_u);
	}
    }

    // sending completion event to server side

    // stop timing
    gettimeofday( &(reportstruct->packetTime), NULL );

    // if we're not doing interval reporting, report the entire transfer as one big packet
    if(0.0 == mSettings->mInterval) {
        reportstruct->packetLen = totLen;
        ReportPacket( mSettings->reporthdr, reportstruct );
    }
    CloseReport( mSettings->reporthdr, reportstruct );

    DELETE_PTR( reportstruct );
    EndReport( mSettings->reporthdr );
}


/* ------------------------------------------------------------------- 
 * Send data using the connected UDP/TCP socket, 
 * until a termination flag is reached. 
 * Does not close the socket. 
 * ------------------------------------------------------------------- */ 

void Client::Run( void ) {
    struct UDP_datagram* mBuf_UDP = (struct UDP_datagram*) mBuf; 
    unsigned long currLen = 0; 

    int delay_target = 0; 
    int delay = 0; 
    int adjust = 0; 

    char* readAt = mBuf;

#if HAVE_THREAD
    if ( !isUDP( mSettings ) ) {
	RunTCP();
	return;
    }
#endif
    
    // Indicates if the stream is readable 
    bool canRead = true, mMode_Time = isModeTime( mSettings ); 

    // setup termination variables
    if ( mMode_Time ) {
        mEndTime.setnow();
        mEndTime.add( mSettings->mAmount / 100.0 );
    }

    if ( isUDP( mSettings ) ) {
        // Due to the UDP timestamps etc, included 
        // reduce the read size by an amount 
        // equal to the header size
    
        // compute delay for bandwidth restriction, constrained to [0,1] seconds 
        delay_target = (int) ( mSettings->mBufLen * ((kSecs_to_usecs * kBytes_to_Bits) 
                                                     / mSettings->mUDPRate) ); 
        if ( delay_target < 0  || 
             delay_target > (int) 1 * kSecs_to_usecs ) {
            fprintf( stderr, warn_delay_large, delay_target / kSecs_to_usecs ); 
            delay_target = (int) kSecs_to_usecs * 1; 
        }
        if ( isFileInput( mSettings ) ) {
            if ( isCompat( mSettings ) ) {
                Extractor_reduceReadSize( sizeof(struct UDP_datagram), mSettings );
                readAt += sizeof(struct UDP_datagram);
            } else {
                Extractor_reduceReadSize( sizeof(struct UDP_datagram) +
                                          sizeof(struct client_hdr), mSettings );
                readAt += sizeof(struct UDP_datagram) +
                          sizeof(struct client_hdr);
            }
        }
    }

    ReportStruct *reportstruct = NULL;

    // InitReport handles Barrier for multiple Streams
    mSettings->reporthdr = InitReport( mSettings );
    reportstruct = new ReportStruct;
    reportstruct->packetID = 0;

    lastPacketTime.setnow();
    
    do {

        // Test case: drop 17 packets and send 2 out-of-order: 
        // sequence 51, 52, 70, 53, 54, 71, 72 
        //switch( datagramID ) { 
        //  case 53: datagramID = 70; break; 
        //  case 71: datagramID = 53; break; 
        //  case 55: datagramID = 71; break; 
        //  default: break; 
        //} 
        gettimeofday( &(reportstruct->packetTime), NULL );

        if ( isUDP( mSettings ) ) {
            // store datagram ID into buffer 
            mBuf_UDP->id      = htonl( (reportstruct->packetID)++ ); 
            mBuf_UDP->tv_sec  = htonl( reportstruct->packetTime.tv_sec ); 
            mBuf_UDP->tv_usec = htonl( reportstruct->packetTime.tv_usec );

            // delay between writes 
            // make an adjustment for how long the last loop iteration took 
            // TODO this doesn't work well in certain cases, like 2 parallel streams 
            adjust = delay_target + lastPacketTime.subUsec( reportstruct->packetTime ); 
            lastPacketTime.set( reportstruct->packetTime.tv_sec, 
                                reportstruct->packetTime.tv_usec ); 

            if ( adjust > 0  ||  delay > 0 ) {
                delay += adjust; 
            }
        }

        // Read the next data block from 
        // the file if it's file input 
        if ( isFileInput( mSettings ) ) {
            Extractor_getNextDataBlock( readAt, mSettings ); 
            canRead = Extractor_canRead( mSettings ) != 0; 
        } else
            canRead = true; 

        // perform write 
        currLen = write( mSettings->mSock, mBuf, mSettings->mBufLen ); 
        if ( currLen < 0 && errno != ENOBUFS ) {
            WARN_errno( currLen < 0, "write2" ); 
            break; 
        }

        // report packets 
        reportstruct->packetLen = currLen;
        ReportPacket( mSettings->reporthdr, reportstruct );
        
        if ( delay > 0 ) {
            delay_loop( delay ); 
        }
        if ( !mMode_Time ) {
            /* mAmount may be unsigned, so don't let it underflow! */
            if( mSettings->mAmount >= currLen ) {
                mSettings->mAmount -= currLen;
            } else {
                mSettings->mAmount = 0;
            }
        }

    } while ( ! (sInterupted  || 
                 (mMode_Time   &&  mEndTime.before( reportstruct->packetTime ))  || 
                 (!mMode_Time  &&  0 >= mSettings->mAmount)) && canRead ); 

    // stop timing
    gettimeofday( &(reportstruct->packetTime), NULL );
    CloseReport( mSettings->reporthdr, reportstruct );

    if ( isUDP( mSettings ) ) {
        // send a final terminating datagram 
        // Don't count in the mTotalLen. The server counts this one, 
        // but didn't count our first datagram, so we're even now. 
        // The negative datagram ID signifies termination to the server. 
    
        // store datagram ID into buffer 
        mBuf_UDP->id      = htonl( -(reportstruct->packetID)  ); 
        mBuf_UDP->tv_sec  = htonl( reportstruct->packetTime.tv_sec ); 
        mBuf_UDP->tv_usec = htonl( reportstruct->packetTime.tv_usec ); 

        if ( isMulticast( mSettings ) ) {
            write( mSettings->mSock, mBuf, mSettings->mBufLen ); 
        } else {
            write_UDP_FIN( ); 
        }
    }
    DELETE_PTR( reportstruct );
    EndReport( mSettings->reporthdr );
} 
// end Run

void Client::InitiateServer() {
    if ( !isCompat( mSettings ) ) {
        int currLen;
        client_hdr* temp_hdr;
        if ( isUDP( mSettings ) ) {
            UDP_datagram *UDPhdr = (UDP_datagram *)mBuf;
            temp_hdr = (client_hdr*)(UDPhdr + 1);
        } else {
            temp_hdr = (client_hdr*)mBuf;
        }
        Settings_GenerateClientHdr( mSettings, temp_hdr );
        if ( !isUDP( mSettings ) ) {
            currLen = send( mSettings->mSock, mBuf, sizeof(client_hdr), 0 );
            if ( currLen < 0 ) {
                WARN_errno( currLen < 0, "write1" );
            }
        }
    }
}

/* -------------------------------------------------------------------
 * Setup a socket connected to a server.
 * If inLocalhost is not null, bind to that address, specifying
 * which outgoing interface to use.
 * ------------------------------------------------------------------- */

void Client::Connect( ) {
    int rc;
    SockAddr_remoteAddr( mSettings );

    assert( mSettings->inHostname != NULL );

    // create an internet socket
    int type = ( isUDP( mSettings )  ?  SOCK_DGRAM : SOCK_STREAM);

    int domain = (SockAddr_isIPv6( &mSettings->peer ) ? 
#ifdef HAVE_IPV6
                  AF_INET6
#else
                  AF_INET
#endif
                  : AF_INET);

    mSettings->mSock = socket( domain, type, 0 );
    WARN_errno( mSettings->mSock == INVALID_SOCKET, "socket" );

    SetSocketOptions( mSettings );


    SockAddr_localAddr( mSettings );
    if ( mSettings->mLocalhost != NULL ) {
        // bind socket to local address
        rc = bind( mSettings->mSock, (sockaddr*) &mSettings->local, 
                   SockAddr_get_sizeof_sockaddr( &mSettings->local ) );
        WARN_errno( rc == SOCKET_ERROR, "bind" );
    }

    // connect socket
    rc = connect( mSettings->mSock, (sockaddr*) &mSettings->peer, 
                  SockAddr_get_sizeof_sockaddr( &mSettings->peer ));
    FAIL_errno( rc == SOCKET_ERROR, "connect", mSettings );

    getsockname( mSettings->mSock, (sockaddr*) &mSettings->local, 
                 &mSettings->size_local );
    getpeername( mSettings->mSock, (sockaddr*) &mSettings->peer,
                 &mSettings->size_peer );
} // end Connect


/* -------------------------------------------------------------------
 * Setup a socket connected to a server use librdmacm.
 * If inLocalhost is not null, bind to that address, specifying
 * which outgoing interface to use.
 * ------------------------------------------------------------------- */

void Client::ConnectRDMA( ) {

    int rc;
    rdma_Ctrl_Blk *cb = mSettings->mCtrlBlk;
    struct rdma_conn_param conn_param;
    SockAddr_remoteAddr( mSettings );

    assert( mSettings->inHostname != NULL );

    // create an internet socket
    //    int type = ( isUDP( mSettings )  ?  SOCK_DGRAM : SOCK_STREAM);

    int domain = (SockAddr_isIPv6( &mSettings->peer ) ? 
#ifdef HAVE_IPV6
                  AF_INET6
#else
                  AF_INET
#endif
                  : AF_INET);

    if (domain == AF_INET)
        ((struct sockaddr_in *) &cb->sin)->sin_port = htons(cb->port);
    else
        ((struct sockaddr_in6 *) &cb->sin)->sin6_port = htons(cb->port);

    cb->cm_channel = rdma_create_event_channel();
    FAIL_errno( cb->cm_channel == NULL, "rdma_create_event_channel", mSettings );

    rc = rdma_create_id(cb->cm_channel, &cb->cm_id, cb, RDMA_PS_TCP);
    FAIL_errno( rc == RDMACM_ERROR, "rdma_create_id", mSettings );

    // resolve address
    rc = rdma_resolve_addr(cb->cm_id, NULL, \
			   (struct sockaddr *) &mSettings->peer, 2000);
    FAIL_errno( rc == RDMACM_ERROR, "rdma_resolve_addr", mSettings );

    rc = get_next_channel_event(cb->cm_channel, RDMA_CM_EVENT_ADDR_RESOLVED);
    WARN( rc == RDMACM_ERROR, "get next event failed" );

    // resolve route
    rc = rdma_resolve_route(cb->cm_id, 2000);
    WARN( rc == RDMACM_ERROR, "rdma_resolve_route" );

    rc = get_next_channel_event(cb->cm_channel, RDMA_CM_EVENT_ROUTE_RESOLVED);
    WARN( rc == RDMACM_ERROR, "get next event" );

    rc = iperf_setup_qp(cb);
    FAIL( rc == RDMAIBV_ERROR, "iperf_setup_qp", mSettings );

    rc = iperf_setup_control_msg(cb);
    FAIL( rc == RDMAIBV_ERROR, "iperf_setup_control_msg", mSettings );

    memset(&conn_param, 0, sizeof conn_param);
    conn_param.responder_resources = 1;
    conn_param.initiator_depth = 1;
    conn_param.retry_count = 10;

    rc = rdma_connect(cb->cm_id, &conn_param);
    FAIL( rc == RDMACM_ERROR, "rdma_connect", mSettings );

    rc = get_next_channel_event(cb->cm_channel, RDMA_CM_EVENT_ESTABLISHED);
    WARN( rc == RDMACM_ERROR, "get next event (RDMA_CM_EVENT_ESTABLISHED) failed" );

    memcpy(&mSettings->local, rdma_get_local_addr(cb->cm_id), \
	   sizeof(iperf_sockaddr));
    memcpy(&mSettings->peer, rdma_get_peer_addr(cb->cm_id), \
	   sizeof(iperf_sockaddr));

    Mutex_Lock( &PseudoSockCond );
    mSettings->mSock = PseudoSock ++;
    Mutex_Unlock( &PseudoSockCond );
	
    return;
} // end ConnectRDMA


/* ------------------------------------------------------------------- 
 * Exchange connection information with server side and setup buffers.
 * Client: mode(4 bytes) + I/O size(4 bytes) + iodepth(4 bytes)
 * if mode is RDMA_WRITE or RDMA_READ
 *     Server: # of credits(4 bytes) + credits
 *         A credit = buffer addr(8 bytes) + rkey(4 bytes) + size(4 bytes)
 * else if mode is Send and Recv
 *     Server: iodepth in server side(4 bytes)
 * ------------------------------------------------------------------- */ 

void Client::InitiateServerRDMA( ) {
    int rc;
    struct ibv_send_wr *bad_send_wr;
    struct ibv_recv_wr *bad_recv_wr;
    rdma_Ctrl_Blk *cb = mSettings->mCtrlBlk;
    struct ibv_wc wc;

    iperf_setup_local_buf( cb );

    // sending test mode, and io depth
    // then receive credits for RDMA_Write and RDMA_Read
    rc = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_recv_wr);
    WARN_errno( rc != 0, "ibv_post_recv" );

    // setup send_buf
    cb->send_buf.mode = htonl(cb->rdma_opcode);
    cb->send_buf.iodepth = htonl(cb->rdma_iodepth);
    cb->send_buf.size = htonl(cb->buflen);

    rc = ibv_post_send(cb->qp, &cb->sq_wr, &bad_send_wr);
    WARN_errno( rc != 0, "ibv_post_send" );

    // get send completion event
    iperf_rdma_poll_wait_control_msg(cb, IBV_WC_SEND, &wc);

    // wait for credit
    iperf_rdma_poll_wait_control_msg(cb, IBV_WC_RECV, &wc);

    switch (cb->rdma_opcode) {
    case kRDMAOpc_RDMA_Write:
    case kRDMAOpc_RDMA_Read:
    case kRDMAOpc_Send_Recv:
        // update request information
        iperf_rdma_setup_credit(cb);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------- 
 * Send a datagram on the socket. The datagram's contents should signify 
 * a FIN to the application. Keep re-transmitting until an 
 * acknowledgement datagram is received. 
 * ------------------------------------------------------------------- */ 

void Client::write_UDP_FIN( ) {
    int rc; 
    fd_set readSet; 
    struct timeval timeout; 

    int count = 0; 
    while ( count < 10 ) {
        count++; 

        // write data 
        write( mSettings->mSock, mBuf, mSettings->mBufLen ); 

        // wait until the socket is readable, or our timeout expires 
        FD_ZERO( &readSet ); 
        FD_SET( mSettings->mSock, &readSet ); 
        timeout.tv_sec  = 0; 
        timeout.tv_usec = 250000; // quarter second, 250 ms 

        rc = select( mSettings->mSock+1, &readSet, NULL, NULL, &timeout ); 
        FAIL_errno( rc == SOCKET_ERROR, "select", mSettings ); 

        if ( rc == 0 ) {
            // select timed out 
            continue; 
        } else {
            // socket ready to read 
            rc = read( mSettings->mSock, mBuf, mSettings->mBufLen ); 
            WARN_errno( rc < 0, "read" );
    	    if ( rc < 0 ) {
                break;
            } else if ( rc >= (int) (sizeof(UDP_datagram) + sizeof(server_hdr)) ) {
                ReportServerUDP( mSettings, (server_hdr*) ((UDP_datagram*)mBuf + 1) );
            }

            return; 
        } 
    } 

    fprintf( stderr, warn_no_ack, mSettings->mSock, count ); 
} 
// end write_UDP_FIN 
