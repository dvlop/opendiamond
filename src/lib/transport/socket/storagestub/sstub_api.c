/*
 *  The OpenDiamond Platform for Interactive Search
 *  Version 2
 *
 *  Copyright (c) 2002-2007 Intel Corporation
 *  Copyright (c) 2006 Larry Huston <larry@thehustons.net>
 *  Copyright (c) 2006-2007 Carnegie Mellon University
 *  All rights reserved.
 *
 *  This software is distributed under the terms of the Eclipse Public
 *  License, Version 1.0 which can be found in the file named LICENSE.
 *  ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES
 *  RECIPIENT'S ACCEPTANCE OF THIS AGREEMENT
 */

/*
 * These file handles a lot of the device specific code.  For the current
 * version we have state for each of the devices.
 */
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <netdb.h>
#include <string.h>
#include <assert.h>
#include "diamond_consts.h"
#include "diamond_types.h"
#include "lib_tools.h"
#include "obj_attr.h"
#include "lib_odisk.h"
#include "socket_trans.h"
#include "lib_dctl.h"
#include "lib_auth.h"
#include "lib_sstub.h"
#include "sstub_impl.h"
#include "ports.h"


static char const cvsid[] =
    "$Header$";


/*
 * XXX do we manage the complete ring also?? 
 */
float
sstub_get_drate(void *cookie)
{

	cstate_t       *cstate;
	cstate = (cstate_t *) cookie;

	return (ring_2drate(cstate->partial_obj_ring));
}


void sstub_get_conn_info(void *cookie, session_info_t *cinfo) {

	cstate_t       *cstate;

	cstate = (cstate_t *) cookie;
	memcpy((void *)cinfo, (void *)&cstate->cinfo, sizeof(session_info_t));
	
	return;
}


/*
 * Send an object. 
 *
 * return current queue depth??
 */
int
sstub_send_obj(void *cookie, obj_data_t * obj, int ver_no, int complete)
{

	cstate_t       *cstate;
	int             err;

	cstate = (cstate_t *) cookie;

	/*
	 * Set a flag to indicate there is object
	 * data associated with our connection.
	 */
	/*
	 * XXX log 
	 */
	pthread_mutex_lock(&cstate->cmutex);
	cstate->flags |= CSTATE_OBJ_DATA;
	if (complete) {
		err =
		    ring_2enq(cstate->complete_obj_ring, (void *) obj,
			      (void *) ver_no);
	} else {
		err =
		    ring_2enq(cstate->partial_obj_ring, (void *) obj,
			      (void *) ver_no);
	}
	pthread_mutex_unlock(&cstate->cmutex);

	if (err) {
		/*
		 * XXX log 
		 */
		/*
		 * XXX how do we handle this 
		 */
		return (err);
	}

	return (0);
}

int
sstub_get_partial(void *cookie, obj_data_t ** obj)
{

	cstate_t       *cstate;
	int             err;
	void           *vnum;

	cstate = (cstate_t *) cookie;

	/*
	 * Set a flag to indicate there is object
	 * data associated with our connection.
	 */
	/*
	 * XXX log 
	 */
	pthread_mutex_lock(&cstate->cmutex);
	err = ring_2deq(cstate->partial_obj_ring, (void **) obj,
			(void **) &vnum);
	pthread_mutex_unlock(&cstate->cmutex);

	return (err);
}

int
sstub_flush_objs(void *cookie, int ver_no)
{

	cstate_t       *cstate;
	int             err;
	obj_data_t     *obj;
	void           *junk;
	void           *vnum;
	listener_state_t *lstate;

	cstate = (cstate_t *) cookie;
	lstate = cstate->lstate;

	/*
	 * Set a flag to indicate there is object
	 * data associated with our connection.
	 */
	/*
	 * XXX log 
	 */
	while (1) {
		pthread_mutex_lock(&cstate->cmutex);
		err = ring_2deq(cstate->complete_obj_ring,
				(void **) &junk, (void **) &vnum);
		pthread_mutex_unlock(&cstate->cmutex);

		/*
		 * we got through them all 
		 */
		if (err) {
			break;
		}
		obj = (obj_data_t *) junk;
		(*lstate->release_obj_cb) (cstate->app_cookie, obj);
	}

	while (1) {
		pthread_mutex_lock(&cstate->cmutex);
		err = ring_2deq(cstate->partial_obj_ring,
				(void **) &junk, (void **) &vnum);
		pthread_mutex_unlock(&cstate->cmutex);

		/*
		 * we got through them all 
		 */
		if (err) {
			return (0);
		}
		obj = (obj_data_t *) junk;
		(*lstate->release_obj_cb) (cstate->app_cookie, obj);

	}

	return (0);
}


int
sstub_get_obj(void *cookie, sig_val_t *sig)
{

	cstate_t       *cstate;
	control_header_t *cheader;
	get_obj_header_t *shead;
	int             err;

	cstate = (cstate_t *) cookie;


	cheader = malloc(sizeof(*cheader));
	assert(cheader != NULL);

	shead = malloc(sizeof(*shead));
	assert(shead != NULL);


	cheader->generation_number = 0;	/* XXX ??? */
	cheader->command = htonl(CNTL_CMD_GET_OBJ);
	cheader->data_len = htonl(sizeof(*shead));
	cheader->spare = (uint32_t) shead;

	memcpy(&shead->obj_sig, sig, sizeof(*sig));


	/*
	 * mark the control state that there is a pending
	 * control message to send.
	 */

	pthread_mutex_lock(&cstate->cmutex);
	cstate->flags |= CSTATE_CONTROL_DATA;

	err = ring_enq(cstate->control_tx_ring, (void *) cheader);
	pthread_mutex_unlock(&cstate->cmutex);
	if (err) {
		/*
		 * XXX 
		 */
		printf("can't enq message \n");
		exit(1);
	}


	return (0);
}



/*
 * This is the initialization function that is
 * called by the searchlet library when we startup.
 */

/*
 * XXX callback for new packets 
 */
void           *
sstub_init(sstub_cb_args_t * cb_args)
{
	return sstub_init_ext(cb_args, 0, 0);
}


/*
 * This is a new version of sstub_init which allows
 * for binding only to localhost.
 */
void *
sstub_init_2(sstub_cb_args_t * cb_args,
	     int bind_only_locally)
{
	return sstub_init_ext(cb_args, bind_only_locally, 0);
}

/*
 * This is a new version of sstub_init which allows
 * for binding only to localhost.
 */
void *
sstub_init_ext(sstub_cb_args_t * cb_args,
	     int bind_only_locally,
	     int auth_required)
{
	listener_state_t *list_state;
	int             err;

	list_state = (listener_state_t *) malloc(sizeof(*list_state));
	if (list_state == NULL) {
		return (NULL);
	}

	/*
	 * clear out list state, this will also clear all the appropriate
	 * flags.
	 */
	memset((char *) list_state, 0, sizeof(*list_state));

	/*
	 * Save all the callback functions.
	 */
	list_state->new_conn_cb = cb_args->new_conn_cb;
	list_state->close_conn_cb = cb_args->close_conn_cb;
	list_state->start_cb = cb_args->start_cb;
	list_state->stop_cb = cb_args->stop_cb;
	list_state->set_fspec_cb = cb_args->set_fspec_cb;
	list_state->set_fobj_cb = cb_args->set_fobj_cb;
	list_state->set_list_cb = cb_args->set_list_cb;
	list_state->terminate_cb = cb_args->terminate_cb;
	list_state->release_obj_cb = cb_args->release_obj_cb;
	list_state->get_char_cb = cb_args->get_char_cb;
	list_state->get_stats_cb = cb_args->get_stats_cb;
	list_state->rleaf_cb = cb_args->rleaf_cb;
	list_state->wleaf_cb = cb_args->wleaf_cb;
	list_state->lleaf_cb = cb_args->lleaf_cb;
	list_state->lnode_cb = cb_args->lnode_cb;
	list_state->sgid_cb = cb_args->sgid_cb;
	list_state->clear_gids_cb = cb_args->clear_gids_cb;
	list_state->set_blob_cb = cb_args->set_blob_cb;
	list_state->set_offload_cb = cb_args->set_offload_cb;
	list_state->set_exec_mode_cb = cb_args->set_exec_mode_cb;
	list_state->set_user_state_cb = cb_args->set_user_state_cb;
	
	/*
	 * save authentication state
	 */
	if (auth_required) {
		list_state->flags |= LSTATE_AUTH_REQUIRED;
	}

	/*
	 * Open the listner sockets for the different types of connections.
	 */
	err = sstub_new_sock(&list_state->control_fd, 
 			     diamond_get_control_port(),
			     bind_only_locally);
	if (err) {
		/*
		 * XXX log, 
		 */
		printf("failed to create control \n");
		free(list_state);
		return (NULL);
	}

	err = sstub_new_sock(&list_state->data_fd, 
			     diamond_get_data_port(),
			     bind_only_locally);
	if (err) {
		/*
		 * XXX log, 
		 */
		printf("failed to create data \n");
		free(list_state);
		return (NULL);
	}

	tirpc_cstate = NULL;
	tirpc_lstate = NULL;

	return ((void *) list_state);
}


int
sstub_lleaf_response(void *cookie, int err, int num_ents, dctl_entry_t * data,
		     int32_t opid)
{
	cstate_t       *cstate;
	control_header_t *cheader;
	dctl_subheader_t *shead;
	int             eno;
	int             tot_len;
	int             dlen;

	cstate = (cstate_t *) cookie;

	dlen = num_ents * sizeof(dctl_entry_t);

	if (err == 0) {
		tot_len = sizeof(*shead) + dlen;
	} else {
		if (err != ENOSPC) {
			dlen = 0;
		}
		tot_len = sizeof(*shead);
	}

	cheader = (control_header_t *) malloc(sizeof(*cheader));
	if (cheader == NULL) {
		/*
		 * XXX 
		 */
		printf("no memory for header \n");
		exit(1);
	}

	shead = (dctl_subheader_t *) malloc(tot_len);
	if (shead == NULL) {
		/*
		 * XXX 
		 */
		free(cheader);
		printf("no memory for subheader \n");
		exit(1);
	}


	cheader->generation_number = 0;	/* XXX ??? */
	cheader->command = htonl(CNTL_CMD_LLEAFS_DONE);
	cheader->data_len = htonl(tot_len);
	cheader->spare = (uint32_t) shead;

	shead->dctl_err = htonl(err);
	shead->dctl_opid = htonl(opid);
	shead->dctl_plen = htonl(0);
	shead->dctl_dlen = htonl(dlen);

	if (err == 0) {
		memcpy(shead->dctl_data, (char *) data, dlen);
	}

	/*
	 * mark the control state that there is a pending
	 * control message to send.
	 */

	pthread_mutex_lock(&cstate->cmutex);
	cstate->flags |= CSTATE_CONTROL_DATA;

	eno = ring_enq(cstate->control_tx_ring, (void *) cheader);
	pthread_mutex_unlock(&cstate->cmutex);
	if (eno) {
		/*
		 * XXX 
		 */
		free(shead);
		free(cheader);
		printf("can't enq message \n");
		exit(1);
	}
	return (0);

}

int
sstub_lnode_response(void *cookie, int err, int num_ents, dctl_entry_t * data,
		     int32_t opid)
{
	cstate_t       *cstate;
	control_header_t *cheader;
	dctl_subheader_t *shead;
	int             eno;
	int             tot_len;
	int             dlen;

	cstate = (cstate_t *) cookie;

	dlen = num_ents * sizeof(dctl_entry_t);

	if (err == 0) {
		tot_len = sizeof(*shead) + dlen;
	} else {
		if (err != ENOSPC) {
			dlen = 0;
		}
		tot_len = sizeof(*shead);
	}

	cheader = (control_header_t *) malloc(sizeof(*cheader));
	if (cheader == NULL) {
		/*
		 * XXX 
		 */
		printf("no memory for header \n");
		exit(1);
	}

	shead = (dctl_subheader_t *) malloc(tot_len);
	if (shead == NULL) {
		/*
		 * XXX 
		 */
		free(cheader);
		printf("no memory for subheader \n");
		exit(1);
	}


	cheader->generation_number = 0;	/* XXX ??? */
	cheader->command = htonl(CNTL_CMD_LNODES_DONE);
	cheader->data_len = htonl(tot_len);
	cheader->spare = (uint32_t) shead;

	shead->dctl_err = htonl(err);
	shead->dctl_opid = htonl(opid);
	shead->dctl_plen = htonl(0);
	shead->dctl_dlen = htonl(dlen);

	if (err == 0) {
		memcpy(shead->dctl_data, (char *) data, dlen);
	}

	/*
	 * mark the control state that there is a pending
	 * control message to send.
	 */

	pthread_mutex_lock(&cstate->cmutex);
	cstate->flags |= CSTATE_CONTROL_DATA;

	eno = ring_enq(cstate->control_tx_ring, (void *) cheader);
	pthread_mutex_unlock(&cstate->cmutex);
	if (eno) {
		/*
		 * XXX 
		 */
		free(shead);
		free(cheader);
		printf("can't enq message \n");
		exit(1);
	}
	return (0);
}
