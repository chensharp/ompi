/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "ompi_config.h"

#include "mca/pml/pml.h"
#include "mca/ptl/ptl.h"
#include "mca/ptl/base/ptl_base_comm.h"
#include "pml_uniq_recvreq.h"
#include "pml_uniq_sendreq.h"
#include "pml_uniq_recvfrag.h"
                                                                                                               
static mca_ptl_base_recv_frag_t* mca_pml_uniq_recv_request_match_specific_proc(
    mca_ptl_base_recv_request_t* request, int proc);


static int mca_pml_uniq_recv_request_fini(struct ompi_request_t** request)
{
    MCA_PML_UNIQ_FINI(request);
    return OMPI_SUCCESS;
}

static int mca_pml_uniq_recv_request_free(struct ompi_request_t** request)
{
    MCA_PML_UNIQ_FREE(request);
    return OMPI_SUCCESS;
}


static int mca_pml_uniq_recv_request_cancel(struct ompi_request_t* request, int complete)
{
    mca_pml_base_request_t* uniq_request = (mca_pml_base_request_t*)request;
    ompi_communicator_t* ompi_comm = uniq_request->req_comm;
    mca_pml_ptl_comm_t* pml_comm = (mca_pml_ptl_comm_t*)ompi_comm->c_pml_comm;

    if( true == request->req_complete ) {  /* way to late to cancel this one */
       return OMPI_SUCCESS;
    }
    
    /* The rest should be protected behind the match logic lock */
    OMPI_THREAD_LOCK(&pml_comm->c_matching_lock);
    
    if( OMPI_ANY_TAG == request->req_status.MPI_TAG ) { /* the match have not been already done */
       
       if( uniq_request->req_peer == OMPI_ANY_SOURCE ) {
          ompi_list_remove_item( &(pml_comm->c_wild_receives),
                                 (ompi_list_item_t*)request );
       } else {
          ompi_list_remove_item( pml_comm->c_specific_receives + uniq_request->req_peer,
                                 (ompi_list_item_t*)request );
       }
    }
    
    OMPI_THREAD_UNLOCK(&pml_comm->c_matching_lock);
    
    request->req_status._cancelled = true;
    request->req_complete = true;  /* mark it as completed so all the test/wait  functions
                                    * on this particular request will finish */
    /* Now we have a problem if we are in a multi-threaded environment. We should
     * broadcast the condition on the request in order to allow the other threads
     * to complete their test/wait functions.
     */
    if(ompi_request_waiting) {
       ompi_condition_broadcast(&ompi_request_cond);
    }
    return OMPI_SUCCESS;
}

static void mca_pml_uniq_recv_request_construct(mca_pml_base_recv_request_t* request)
{
    request->req_base.req_type = MCA_PML_REQUEST_RECV;
    request->req_base.req_ompi.req_fini = mca_pml_uniq_recv_request_fini;
    request->req_base.req_ompi.req_free = mca_pml_uniq_recv_request_free;
    request->req_base.req_ompi.req_cancel = mca_pml_uniq_recv_request_cancel;
}

static void mca_pml_uniq_recv_request_destruct(mca_pml_base_recv_request_t* request)
{
}

OBJ_CLASS_INSTANCE(
    mca_pml_uniq_recv_request_t,
    mca_pml_base_recv_request_t,
    mca_pml_uniq_recv_request_construct,
    mca_pml_uniq_recv_request_destruct);
                                                                                                                                                     

/*
 * Update the recv request status to reflect the number of bytes
 * received and actually delivered to the application. 
 */

void mca_pml_uniq_recv_request_progress(
    struct mca_ptl_base_module_t* ptl,
    mca_ptl_base_recv_request_t* req,
    size_t bytes_received,
    size_t bytes_delivered)
{
    OMPI_THREAD_LOCK(&ompi_request_lock);
    req->req_bytes_received += bytes_received;
    req->req_bytes_delivered += bytes_delivered;
    if (req->req_bytes_received >= req->req_recv.req_bytes_packed) {
        /* initialize request status */
        req->req_recv.req_base.req_ompi.req_status._count = req->req_bytes_delivered;
        req->req_recv.req_base.req_pml_complete = true; 
        req->req_recv.req_base.req_ompi.req_complete = true;
        if(ompi_request_waiting) {
            ompi_condition_broadcast(&ompi_request_cond);
        }
    }
    OMPI_THREAD_UNLOCK(&ompi_request_lock);
}


                                                                                                                    
/*
 * This routine is used to match a posted receive when the source process 
 * is specified.
*/

void mca_pml_uniq_recv_request_match_specific(mca_ptl_base_recv_request_t* request)
{
    ompi_communicator_t *comm = request->req_recv.req_base.req_comm;
    mca_pml_ptl_comm_t* pml_comm = comm->c_pml_comm;
    int req_peer = request->req_recv.req_base.req_peer;
    mca_ptl_base_recv_frag_t* frag;
   
    /* check for a specific match */
    OMPI_THREAD_LOCK(&pml_comm->c_matching_lock);

    /* assign sequence number */
    request->req_recv.req_base.req_sequence = pml_comm->c_recv_seq++;

    if (ompi_list_get_size(&pml_comm->c_unexpected_frags[req_peer]) > 0 &&
        (frag = mca_pml_uniq_recv_request_match_specific_proc(request, req_peer)) != NULL) {
        mca_ptl_base_module_t* ptl = frag->frag_base.frag_owner;
        /* setup pointer to ptls peer */
        if(NULL == frag->frag_base.frag_peer) 
            frag->frag_base.frag_peer = mca_pml_uniq_proc_lookup_remote_peer(comm,req_peer,ptl);
        OMPI_THREAD_UNLOCK(&pml_comm->c_matching_lock);
        if( !((MCA_PML_REQUEST_IPROBE == request->req_recv.req_base.req_type) ||
              (MCA_PML_REQUEST_PROBE == request->req_recv.req_base.req_type)) ) {
            MCA_PML_UNIQ_RECV_MATCHED( ptl, frag );
        }
        return; /* match found */
    }

    /* We didn't find any matches.  Record this irecv so we can match 
     * it when the message comes in.
    */
    if(request->req_recv.req_base.req_type != MCA_PML_REQUEST_IPROBE) { 
        ompi_list_append(pml_comm->c_specific_receives+req_peer, (ompi_list_item_t*)request);
    }
    OMPI_THREAD_UNLOCK(&pml_comm->c_matching_lock);
}


/*
 * this routine is used to try and match a wild posted receive - where
 * wild is determined by the value assigned to the source process
*/

void mca_pml_uniq_recv_request_match_wild(mca_ptl_base_recv_request_t* request)
{
    ompi_communicator_t *comm = request->req_recv.req_base.req_comm;
    mca_pml_ptl_comm_t* pml_comm = comm->c_pml_comm;
    int proc_count = comm->c_remote_group->grp_proc_count;
    int proc;

    /*
     * Loop over all the outstanding messages to find one that matches.
     * There is an outer loop over lists of messages from each
     * process, then an inner loop over the messages from the
     * process.
    */
    OMPI_THREAD_LOCK(&pml_comm->c_matching_lock);

    /* assign sequence number */
    request->req_recv.req_base.req_sequence = pml_comm->c_recv_seq++;

    for (proc = 0; proc < proc_count; proc++) {
        mca_ptl_base_recv_frag_t* frag;

        /* continue if no frags to match */
        if (ompi_list_get_size(&pml_comm->c_unexpected_frags[proc]) == 0)
            continue;

        /* loop over messages from the current proc */
        if ((frag = mca_pml_uniq_recv_request_match_specific_proc(request, proc)) != NULL) {
            mca_ptl_base_module_t* ptl = frag->frag_base.frag_owner;
            /* if required - setup pointer to ptls peer */
            if(NULL == frag->frag_base.frag_peer) 
                frag->frag_base.frag_peer = mca_pml_uniq_proc_lookup_remote_peer(comm,proc,ptl);
            OMPI_THREAD_UNLOCK(&pml_comm->c_matching_lock);
            if( !((MCA_PML_REQUEST_IPROBE == request->req_recv.req_base.req_type) ||
                  (MCA_PML_REQUEST_PROBE == request->req_recv.req_base.req_type)) ) {
                MCA_PML_UNIQ_RECV_MATCHED( ptl, frag );
            }
            return; /* match found */
        }
    } 

    /* We didn't find any matches.  Record this irecv so we can match to
     * it when the message comes in.
    */
 
    if(request->req_recv.req_base.req_type != MCA_PML_REQUEST_IPROBE)
        ompi_list_append(&pml_comm->c_wild_receives, (ompi_list_item_t*)request);
    OMPI_THREAD_UNLOCK(&pml_comm->c_matching_lock);
}


/*
 *  this routine tries to match a posted receive.  If a match is found,
 *  it places the request in the appropriate matched receive list. 
*/

static mca_ptl_base_recv_frag_t* mca_pml_uniq_recv_request_match_specific_proc(
    mca_ptl_base_recv_request_t* request, int proc)
{
    mca_pml_ptl_comm_t *pml_comm = request->req_recv.req_base.req_comm->c_pml_comm;
    ompi_list_t* unexpected_frags = pml_comm->c_unexpected_frags+proc;
    mca_ptl_base_recv_frag_t* frag;
    mca_ptl_base_match_header_t* header;
    int tag = request->req_recv.req_base.req_tag;

    if( OMPI_ANY_TAG == tag ) {
        for (frag =  (mca_ptl_base_recv_frag_t*)ompi_list_get_first(unexpected_frags);
             frag != (mca_ptl_base_recv_frag_t*)ompi_list_get_end(unexpected_frags);
             frag =  (mca_ptl_base_recv_frag_t*)ompi_list_get_next(frag)) {
            header = &(frag->frag_base.frag_header.hdr_match);
            
            /* check first frag - we assume that process matching has been done already */
            if( header->hdr_tag >= 0 ) {
                goto find_fragment;
            } 
        }
    } else {
        for (frag =  (mca_ptl_base_recv_frag_t*)ompi_list_get_first(unexpected_frags);
             frag != (mca_ptl_base_recv_frag_t*)ompi_list_get_end(unexpected_frags);
             frag =  (mca_ptl_base_recv_frag_t*)ompi_list_get_next(frag)) {
            header = &(frag->frag_base.frag_header.hdr_match);
            
            /* check first frag - we assume that process matching has been done already */
            if ( tag == header->hdr_tag ) {
                /* we assume that the tag is correct from MPI point of view (ie. >= 0 ) */
                goto find_fragment;
            } 
        }
    }
    return NULL;
 find_fragment:
    request->req_recv.req_bytes_packed = header->hdr_msg_length;
    request->req_recv.req_base.req_ompi.req_status.MPI_TAG = header->hdr_tag;
    request->req_recv.req_base.req_ompi.req_status.MPI_SOURCE = header->hdr_src;

    if( !((MCA_PML_REQUEST_IPROBE == request->req_recv.req_base.req_type) ||
          (MCA_PML_REQUEST_PROBE == request->req_recv.req_base.req_type)) ) {
        ompi_list_remove_item(unexpected_frags, (ompi_list_item_t*)frag);
        frag->frag_request = request;
    } else {
        /* it's a probe, therefore report it's completion */
        mca_pml_uniq_recv_request_progress( NULL, request, header->hdr_msg_length, header->hdr_msg_length );
    }
    return frag;
}

