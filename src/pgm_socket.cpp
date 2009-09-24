/*
    Copyright (c) 2007-2009 FastMQ Inc.

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the Lesser GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Lesser GNU General Public License for more details.

    You should have received a copy of the Lesser GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "platform.hpp"

#if defined ZMQ_HAVE_OPENPGM1

#ifdef ZMQ_HAVE_LINUX
#include <pgm/pgm.h>
#include <openssl/md5.h>
#endif

#include <string>
#include <iostream>

#include "options.hpp"
#include "pgm_socket.hpp"
#include "config.hpp"
#include "err.hpp"
#include "uuid.hpp"

//#define PGM_SOCKET_DEBUG
//#define PGM_SOCKET_DEBUG_LEVEL 1

// level 1 = key behaviour
// level 2 = processing flow
// level 4 = infos

#ifndef PGM_SOCKET_DEBUG
#   define zmq_log(n, ...)  while (0)
#else
#   define zmq_log(n, ...)    do { if ((n) <= PGM_SOCKET_DEBUG_LEVEL) \
        { printf (__VA_ARGS__);}} while (0)
#endif

#ifdef ZMQ_HAVE_LINUX

zmq::pgm_socket_t::pgm_socket_t (bool receiver_, const options_t &options_) :
    g_transport (NULL),
    options (options_),
    receiver (receiver_),
    port_number (0),
    udp_encapsulation (false),
    pgm_msgv (NULL),
    nbytes_rec (0),
    nbytes_processed (0),
    pgm_msgv_processed (0),
    pgm_msgv_len (0)
{
    
}

int zmq::pgm_socket_t::pgm_create_custom_gsi (const char *data_, pgm_gsi_t *gsi_)
{

    unsigned char result_md5 [16];

    MD5_CTX ctx;
    MD5_Init (&ctx);
    MD5_Update (&ctx, data_, strlen (data_));
    MD5_Final (result_md5, &ctx);

    memcpy (gsi_, result_md5 + 10, 6);

    return 0;
}

int zmq::pgm_socket_t::init (bool udp_encapsulation_, const char *network_)
{
    udp_encapsulation = udp_encapsulation_;
 
    //  Parse port number.
    const char *port_delim = strchr (network_, ':');
    if (!port_delim) {
        errno = EINVAL;
        return -1;
    }

    port_number = atoi (port_delim + 1);
  
    if (port_delim - network_ >= (int) sizeof (network) - 1) {
        errno = EINVAL;
        return -1;
    }

    memset (network, '\0', sizeof (network));
    memcpy (network, network_, port_delim - network_);

    zmq_log (1, "parsed: network  %s, port %i, udp encaps. %s, %s(%i)\n", 
        network, port_number, udp_encapsulation ? "yes" : "no",
        __FILE__, __LINE__);

    //  Open PGM transport.
    int rc = open_transport ();
    if (rc != 0)
        return -1;

    //  For receiver transport preallocate pgm_msgv array.
    //  in_batch_size configured in confing.hpp
    if (receiver) {
        pgm_msgv_len = get_max_apdu_at_once (in_batch_size);
        pgm_msgv = new pgm_msgv_t [pgm_msgv_len];
    }

    return 0;
}

int zmq::pgm_socket_t::open_transport (void)
{

    zmq_log (1, "Opening PGM: network  %s, port %i, udp encaps. %s, %s(%i)\n",
        network, port_number, udp_encapsulation ? "yes" : "no", 
        __FILE__, __LINE__);

    //  Can not open transport before destroying old one. 
    zmq_assert (g_transport == NULL);

    //  Zero counter used in msgrecv.
    nbytes_rec = 0;
    nbytes_processed = 0;
    pgm_msgv_processed = 0;

    //  Init PGM transport.
    //  Ensure threading enabled, ensure timer enabled and find PGM protocol id.
    //
    //  Note that if you want to use gettimeofday and sleep for openPGM timing,
    //  set environment variables PGM_TIMER to "GTOD" 
    //  and PGM_SLEEP to "USLEEP".
    int rc = pgm_init ();
    if (rc != 0) {
        errno = EINVAL;
        return -1;
    }

    //  PGM transport GSI.
    pgm_gsi_t gsi;
 
    //  PGM transport GSRs.
    struct group_source_req recv_gsr, send_gsr;
    size_t recv_gsr_len = 1;

    if (options.identity.size () > 0) {

        //  Create gsi from identity string.
        rc = pgm_create_custom_gsi (options.identity.c_str (), &gsi);

    } else {

        //  Generate random gsi.
        rc = pgm_create_custom_gsi (uuid_t ().to_string (), &gsi);
    }

    if (rc != 0) {
        errno = EINVAL;
        return -1;
    }

    zmq_log (1, "Transport GSI: %s, %s(%i)\n", pgm_print_gsi (&gsi),
        __FILE__, __LINE__);

    //  On success, 0 is returned. On invalid arguments, -EINVAL is returned. 
    //  If more multicast groups are found than the recv_len parameter, 
    //  -ENOMEM is returned.
    rc = pgm_if_parse_transport (network, AF_INET, &recv_gsr, 
        &recv_gsr_len, &send_gsr);
    if (rc != 0) {
        errno = EINVAL;
        return -1;
    }

    if (recv_gsr_len != 1) {
        errno = ENOMEM;
        return -1;
    }

    //  If we are using UDP encapsulation update send_gsr & recv_gsr 
    //  structures. Note that send_gsr & recv_gsr has to be updated after 
    //  pgm_if_parse_transport call.
    if (udp_encapsulation) {

        //  Use the same port for UDP encapsulation.
        ((struct sockaddr_in*)&send_gsr.gsr_group)->sin_port = 
            g_htons (port_number);
	((struct sockaddr_in*)&recv_gsr.gsr_group)->sin_port = 
            g_htons (port_number);
    }

    rc = pgm_transport_create (&g_transport, &gsi, 0, port_number, &recv_gsr, 
        1, &send_gsr);
    if (rc != 0) {
        return -1;
    }

    //  Common parameters for receiver and sender.

    //  Set maximum transport protocol data unit size (TPDU).
    rc = pgm_transport_set_max_tpdu (g_transport, pgm_max_tpdu);
    if (rc != 0) {
        errno = EINVAL;
        return -1;
    }

    //  Set maximum number of network hops to cross.
    rc = pgm_transport_set_hops (g_transport, 16);
    if (rc != 0) {
        errno = EINVAL;
        return -1;
    }

    //  Receiver transport.
    if (receiver) {

        //  Set transport->can_send_data = FALSE.
        //  Note that NAKs are still generated by the transport.
        rc = pgm_transport_set_recv_only (g_transport, false);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

        //  Set NAK transmit back-off interval [us].
        rc = pgm_transport_set_nak_bo_ivl (g_transport, 50*1000);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

        //  Set timeout before repeating NAK [us].
        rc = pgm_transport_set_nak_rpt_ivl (g_transport, 200*1000);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

        //  Set timeout for receiving RDATA.
        rc = pgm_transport_set_nak_rdata_ivl (g_transport, 200*1000);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

        //  Set retries for NAK without NCF/DATA (NAK_DATA_RETRIES).
        rc = pgm_transport_set_nak_data_retries (g_transport, 5);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

        //  Set retries for NCF after NAK (NAK_NCF_RETRIES).
        rc = pgm_transport_set_nak_ncf_retries (g_transport, 2);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

        //  Set timeout for removing a dead peer [us].
        rc = pgm_transport_set_peer_expiry (g_transport, 5*8192*1000);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

        //  Set expiration time of SPM Requests [us].
        rc = pgm_transport_set_spmr_expiry (g_transport, 25*1000);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

        //  Set the size of the receive window.
        //
        //  data rate [B/s]  (options.rate is kb/s).
        if (options.rate <= 0) {
            errno = EINVAL;
            return -1;
        }

        rc = pgm_transport_set_rxw_max_rte (g_transport, 
            options.rate * 1000 / 8);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

        //  Recovery interval [s]. 
        if (options.recovery_ivl <= 0) {
            errno = EINVAL;
            return -1;
        }

        rc = pgm_transport_set_rxw_secs (g_transport, options.recovery_ivl);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

    //  Sender transport.
    } else {

        //  Set transport->can_recv = FALSE, waiting_pipe wont not be read.
        rc = pgm_transport_set_send_only (g_transport, TRUE);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

        //  Set the size of the send window.
        //
        //  data rate [B/s]  (options.rate is kb/s).
        if (options.rate <= 0) {
            errno = EINVAL;
            return -1;
        }

        rc = pgm_transport_set_txw_max_rte (g_transport, 
            options.rate * 1000 / 8);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

        //  Recovery interval [s]. 
        if (options.recovery_ivl <= 0) {
            errno = EINVAL;
            return -1;
        }

        rc = pgm_transport_set_txw_secs (g_transport, options.recovery_ivl);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

        //  Preallocate full transmit window. For simplification always 
        //  worst case is used (40 bytes ipv6 header and 20 bytes UDP 
        //  encapsulation).
        int to_preallocate = options.recovery_ivl * (options.rate * 1000 / 8) 
            / (pgm_max_tpdu - 40 - 20);

        rc = pgm_transport_set_txw_preallocate (g_transport, to_preallocate);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

        zmq_log (2, "Preallocated %i slices in TX window. %s(%i)\n", 
            to_preallocate, __FILE__, __LINE__);

        //  Set interval of background SPM packets [us].
        rc = pgm_transport_set_ambient_spm (g_transport, 8192 * 1000);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }

        //  Set intervals of data flushing SPM packets [us].
        guint spm_heartbeat[] = {4 * 1000, 4 * 1000, 8 * 1000, 16 * 1000, 
            32 * 1000, 64 * 1000, 128 * 1000, 256 * 1000, 512 * 1000, 
            1024 * 1000, 2048 * 1000, 4096 * 1000, 8192 * 1000};
        
	rc = pgm_transport_set_heartbeat_spm (g_transport, spm_heartbeat, 
            G_N_ELEMENTS(spm_heartbeat));
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }
    }
    
    //  Enable multicast loopback.
    if (options.use_multicast_loop) {
        rc = pgm_transport_set_multicast_loop (g_transport, true);
        if (rc != 0) {
            errno = EINVAL;
            return -1;
        }
    }

    //  Bind a transport to the specified network devices.
    rc = pgm_transport_bind (g_transport);
    if (rc != 0) {
        return -1;
    }

    return 0;
}

zmq::pgm_socket_t::~pgm_socket_t ()
{
    //  Celanup.
    if (pgm_msgv) {
        delete [] pgm_msgv;
    }

    if (g_transport)
        close_transport ();
}

void zmq::pgm_socket_t::close_transport (void)
{   
    //  g_transport has to be valid.
    zmq_assert (g_transport);

    pgm_transport_destroy (g_transport, TRUE);

    g_transport = NULL;
}

//   Get receiver fds. recv_fd is from transport->recv_sock
//   waiting_pipe_fd is from transport->waiting_pipe [0]
int zmq::pgm_socket_t::get_receiver_fds (int *recv_fd_, 
    int *waiting_pipe_fd_)
{

    //  For POLLIN there are 2 pollfds in pgm_transport.
    int fds_array_size = pgm_receiver_fd_count;
    pollfd *fds = new pollfd [fds_array_size];
    memset (fds, '\0', fds_array_size * sizeof (fds));

    //  Retrieve pollfds from pgm_transport.
    int rc = pgm_transport_poll_info (g_transport, fds, &fds_array_size, 
        POLLIN);

    //  pgm_transport_poll_info has to return 2 pollfds for POLLIN. 
    //  Note that fds_array_size parameter can be 
    //  changed inside pgm_transport_poll_info call.
    zmq_assert (rc == pgm_receiver_fd_count);
 
    //  Store pfds into user allocated space.
    *recv_fd_ = fds [0].fd;
    *waiting_pipe_fd_ = fds [1].fd;

    delete [] fds;

    return pgm_receiver_fd_count;
}

//   Get fds and store them into user allocated memory. 
//   sender_fd is from pgm_transport->send_sock.
//   receive_fd_ is from  transport->recv_sock.
int zmq::pgm_socket_t::get_sender_fds (int *send_fd_, int *receive_fd_)
{

    //  Preallocate pollfds array.
    int fds_array_size = pgm_sender_fd_count;
    pollfd *fds = new pollfd [fds_array_size];
    memset (fds, '\0', fds_array_size * sizeof (fds));

    //  Retrieve pollfds from pgm_transport
    int rc = pgm_transport_poll_info (g_transport, fds, &fds_array_size, 
        POLLOUT | POLLIN);

    //  pgm_transport_poll_info has to return one pollfds for POLLOUT and
    //  second for POLLIN.
    //  Note that fds_array_size parameter can be 
    //  changed inside pgm_transport_poll_info call.
    zmq_assert (rc == pgm_sender_fd_count);
 
    //  Store pfds into user allocated space.
    *receive_fd_ = fds [0].fd;
    *send_fd_ = fds [1].fd;

    delete [] fds;

    return pgm_sender_fd_count;
}

//  Send one APDU, transmit window owned memory.
size_t zmq::pgm_socket_t::send (unsigned char *data_, size_t data_len_)
{

    iovec iov = {data_,data_len_};

    ssize_t nbytes = pgm_transport_send_packetv (g_transport, &iov, 1, 
        MSG_DONTWAIT | MSG_WAITALL, true);

    zmq_assert (nbytes != -EINVAL);

    if (nbytes == -1 && errno != EAGAIN) {
        errno_assert (false);
    }

    //  If nbytes is -1 and errno is EAGAIN means that we can not send data 
    //  now. We have to call write_one_pkt again.
    nbytes = nbytes == -1 ? 0 : nbytes;

    zmq_log (4, "wrote %iB, %s(%i)\n", (int)nbytes, __FILE__, __LINE__);
    
    // We have to write all data as one packet.
    if (nbytes > 0) {
        zmq_assert (nbytes == (ssize_t)data_len_);
    }

    return nbytes;
}

//  Return max TSDU size without fragmentation from current PGM transport.
size_t zmq::pgm_socket_t::get_max_tsdu_size (void)
{
    return (size_t)pgm_transport_max_tsdu (g_transport, false);
}

//  Returns how many APDUs are needed to fill reading buffer.
size_t zmq::pgm_socket_t::get_max_apdu_at_once (size_t readbuf_size_)
{
    zmq_assert (readbuf_size_ > 0);

    //  Read max TSDU size without fragmentation.
    size_t max_tsdu_size = get_max_tsdu_size ();

    //  Calculate number of APDUs needed to fill the reading buffer.
    size_t apdu_count = (int)readbuf_size_ / max_tsdu_size;

    if ((int) readbuf_size_ % max_tsdu_size)
        apdu_count ++;

    //  Have to have at least one APDU.
    zmq_assert (apdu_count);

    return apdu_count;
}

//  Allocate buffer for one packet from the transmit window, The memory buffer 
//  is owned by the transmit window and so must be returned to the window with 
//  content via pgm_transport_send() calls or unused with pgm_packetv_free1(). 
void *zmq::pgm_socket_t::get_buffer (size_t *size_)
{
    //  Store size.
    *size_ = get_max_tsdu_size ();

    //  Allocate one packet.
    return pgm_packetv_alloc (g_transport, false);
}

//  Return an unused packet allocated from the transmit window 
//  via pgm_packetv_alloc(). 
void zmq::pgm_socket_t::free_buffer (void *data_)
{
    pgm_packetv_free1 (g_transport, data_, false);
}

//  pgm_transport_recvmsgv is called to fill the pgm_msgv array up to 
//  pgm_msgv_len. In subsequent calls data from pgm_msgv structure are 
//  returned.
ssize_t zmq::pgm_socket_t::receive (void **raw_data_, const pgm_tsi_t **tsi_)
{
    //  We just sent all data from pgm_transport_recvmsgv up 
    //  and have to return 0 that another engine in this thread is scheduled.
    if (nbytes_rec == nbytes_processed && nbytes_rec > 0) {

        //  Reset all the counters.
        nbytes_rec = 0;
        nbytes_processed = 0;
        pgm_msgv_processed = 0;

        return 0;
    }

    //  If we have are going first time or if we have processed all pgm_msgv_t
    //  structure previously read from the pgm socket.
    if (nbytes_rec == nbytes_processed) {

        //  Check program flow.
        zmq_assert (pgm_msgv_processed == 0);
        zmq_assert (nbytes_processed == 0);
        zmq_assert (nbytes_rec == 0);

        //  Receive a vector of Application Protocol Domain Unit's (APDUs) 
        //  from the transport.
        nbytes_rec = pgm_transport_recvmsgv (g_transport, pgm_msgv, 
            pgm_msgv_len, MSG_DONTWAIT);
  
        //  In a case when no ODATA/RDATA fired POLLIN event (SPM...)
        //  pgm_transport_recvmsg returns -1 with errno == EAGAIN.
        if (nbytes_rec == -1 && errno == EAGAIN) {
        
            //  In case if no RDATA/ODATA caused POLLIN 0 is 
            //  returned.
            nbytes_rec = 0;
            return 0;
        }

        //  For data loss nbytes_rec == -1 errno == ECONNRESET.
        if (nbytes_rec == -1 && errno == ECONNRESET) {
           
            //  Save lost data TSI.
            *tsi_ = &(g_transport->lost_data_tsi);

            //  In case of dala loss -1 is returned.
            zmq_log (1, "Data loss detected %s, %s(%i)\n", 
                pgm_print_tsi (&(g_transport->lost_data_tsi)), __FILE__, __LINE__);
            nbytes_rec = 0;
            return -1;
        }

        //  Catch the rest of the errors.
        if (nbytes_rec <= 0) {
            zmq_log (2, "received %i B, errno %i, %s(%i)", (int)nbytes_rec, 
                errno, __FILE__, __LINE__);
            errno_assert (nbytes_rec > 0);
        }
   
        zmq_log (4, "received %i bytes\n", (int)nbytes_rec);

    }

    zmq_assert (nbytes_rec > 0);

    // Only one APDU per pgm_msgv_t structure is allowed. 
    zmq_assert (pgm_msgv [pgm_msgv_processed].msgv_iovlen == 1);

    //  Take pointers from pgm_msgv_t structure.
    *raw_data_ = pgm_msgv[pgm_msgv_processed].msgv_iov->iov_base;
    size_t raw_data_len = pgm_msgv[pgm_msgv_processed].msgv_iov->iov_len;

    //  Save current TSI.
    *tsi_ = pgm_msgv [pgm_msgv_processed].msgv_tsi;

    //  Move the the next pgm_msgv_t structure.
    pgm_msgv_processed++;
    nbytes_processed +=raw_data_len;

    zmq_log (4, "sendig up %i bytes\n", (int)raw_data_len);

    return raw_data_len;
}

void zmq::pgm_socket_t::process_upstream (void)
{
    zmq_log (1, "On upstream packet, %s(%i)\n", __FILE__, __LINE__);
    //  We acctually do not want to read any data here we are going to 
    //  process NAK.
    pgm_msgv_t dummy_msg;

    ssize_t dummy_bytes = pgm_transport_recvmsgv (g_transport, &dummy_msg,
        1, MSG_DONTWAIT);
    
    //  No data should be returned.
    zmq_assert (dummy_bytes == -1 && errno == EAGAIN);
}

#endif

#endif
