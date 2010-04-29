/*
 *  Copyright (C) 2002 - 2003 Ardis Technolgies <roman@ardistech.com>
 *  Copyright (C) 2007 - 2010 Vladislav Bolkhovitin
 *  Copyright (C) 2007 - 2010 ID7 Ltd.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation, version 2
 *  of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/hash.h>
#include <linux/kthread.h>
#include <linux/scatterlist.h>
#include <net/tcp.h>
#include <scsi/scsi.h>

#include "iscsi.h"
#include "digest.h"

#ifndef GENERATING_UPSTREAM_PATCH
#if !defined(CONFIG_TCP_ZERO_COPY_TRANSFER_COMPLETION_NOTIFICATION)
#warning "Patch put_page_callback-<kernel-version>.patch not applied on your\
 kernel or CONFIG_TCP_ZERO_COPY_TRANSFER_COMPLETION_NOTIFICATION\
 config option not set. ISCSI-SCST will be working with not the best\
 performance. Refer README file for details."
#endif
#endif

#define ISCSI_INIT_WRITE_WAKE		0x1

static int ctr_major;
static char ctr_name[] = "iscsi-scst-ctl";

#if defined(CONFIG_SCST_DEBUG) || defined(CONFIG_SCST_TRACING)
unsigned long iscsi_trace_flag = ISCSI_DEFAULT_LOG_FLAGS;
#endif

static struct kmem_cache *iscsi_cmnd_cache;

DEFINE_SPINLOCK(iscsi_rd_lock);
LIST_HEAD(iscsi_rd_list);
DECLARE_WAIT_QUEUE_HEAD(iscsi_rd_waitQ);

DEFINE_SPINLOCK(iscsi_wr_lock);
LIST_HEAD(iscsi_wr_list);
DECLARE_WAIT_QUEUE_HEAD(iscsi_wr_waitQ);

static struct page *dummy_page;
static struct scatterlist dummy_sg;

struct iscsi_thread_t {
	struct task_struct *thr;
	struct list_head threads_list_entry;
};

static LIST_HEAD(iscsi_threads_list);

static void cmnd_remove_data_wait_hash(struct iscsi_cmnd *cmnd);
static void iscsi_send_task_mgmt_resp(struct iscsi_cmnd *req, int status);
static void iscsi_check_send_delayed_tm_resp(struct iscsi_session *sess);
static void req_cmnd_release(struct iscsi_cmnd *req);
static int iscsi_preliminary_complete(struct iscsi_cmnd *req,
	struct iscsi_cmnd *orig_req, bool get_data);
static int cmnd_insert_data_wait_hash(struct iscsi_cmnd *cmnd);
static void __cmnd_abort(struct iscsi_cmnd *cmnd);
static void iscsi_set_resid(struct iscsi_cmnd *rsp, bool bufflen_set);
static void iscsi_cmnd_init_write(struct iscsi_cmnd *rsp, int flags);

static void req_del_from_write_timeout_list(struct iscsi_cmnd *req)
{
	struct iscsi_conn *conn;

	TRACE_ENTRY();

	if (!req->on_write_timeout_list)
		goto out;

	conn = req->conn;

	TRACE_DBG("Deleting cmd %p from conn %p write_timeout_list",
		req, conn);

	spin_lock_bh(&conn->write_list_lock);

	/* Recheck, since it can be changed behind us */
	if (unlikely(!req->on_write_timeout_list))
		goto out_unlock;

	list_del(&req->write_timeout_list_entry);
	req->on_write_timeout_list = 0;

out_unlock:
	spin_unlock_bh(&conn->write_list_lock);

out:
	TRACE_EXIT();
	return;
}

static inline u32 cmnd_write_size(struct iscsi_cmnd *cmnd)
{
	struct iscsi_scsi_cmd_hdr *hdr = cmnd_hdr(cmnd);

	if (hdr->flags & ISCSI_CMD_WRITE)
		return be32_to_cpu(hdr->data_length);
	return 0;
}

static inline int cmnd_read_size(struct iscsi_cmnd *cmnd)
{
	struct iscsi_scsi_cmd_hdr *hdr = cmnd_hdr(cmnd);

	if (hdr->flags & ISCSI_CMD_READ) {
		struct iscsi_ahs_hdr *ahdr;

		if (!(hdr->flags & ISCSI_CMD_WRITE))
			return be32_to_cpu(hdr->data_length);

		ahdr = (struct iscsi_ahs_hdr *)cmnd->pdu.ahs;
		if (ahdr != NULL) {
			uint8_t *p = (uint8_t *)ahdr;
			unsigned int size = 0;
			do {
				int s;

				ahdr = (struct iscsi_ahs_hdr *)p;

				if (ahdr->ahstype == ISCSI_AHSTYPE_RLENGTH) {
					struct iscsi_rlength_ahdr *rh =
					      (struct iscsi_rlength_ahdr *)ahdr;
					return be32_to_cpu(rh->read_length);
				}

				s = 3 + be16_to_cpu(ahdr->ahslength);
				s = (s + 3) & -4;
				size += s;
				p += s;
			} while (size < cmnd->pdu.ahssize);
		}
		return -1;
	}
	return 0;
}

void iscsi_restart_cmnd(struct iscsi_cmnd *cmnd)
{
	int status;

	TRACE_ENTRY();

	EXTRACHECKS_BUG_ON(cmnd->r2t_len_to_receive != 0);
	EXTRACHECKS_BUG_ON(cmnd->r2t_len_to_send != 0);

	req_del_from_write_timeout_list(cmnd);

	/*
	 * Let's remove cmnd from the hash earlier to keep it smaller.
	 * See also corresponding comment in req_cmnd_release().
	 */
	if (cmnd->hashed)
		cmnd_remove_data_wait_hash(cmnd);

	if (unlikely(test_bit(ISCSI_CONN_REINSTATING,
			&cmnd->conn->conn_aflags))) {
		struct iscsi_target *target = cmnd->conn->session->target;
		bool get_out;

		mutex_lock(&target->target_mutex);

		get_out = test_bit(ISCSI_CONN_REINSTATING,
				&cmnd->conn->conn_aflags);
		/* Let's don't look dead */
		if (scst_cmd_get_cdb(cmnd->scst_cmd)[0] == TEST_UNIT_READY)
			get_out = false;

		if (!get_out)
			goto unlock_cont;

		TRACE_MGMT_DBG("Pending cmnd %p, because conn %p is "
			"reinstated", cmnd, cmnd->conn);

		cmnd->scst_state = ISCSI_CMD_STATE_REINST_PENDING;
		list_add_tail(&cmnd->reinst_pending_cmd_list_entry,
			&cmnd->conn->reinst_pending_cmd_list);

unlock_cont:
		mutex_unlock(&target->target_mutex);

		if (get_out)
			goto out;
	}

	if (unlikely(cmnd->prelim_compl_flags != 0)) {
		if (test_bit(ISCSI_CMD_ABORTED, &cmnd->prelim_compl_flags)) {
			TRACE_MGMT_DBG("cmnd %p (scst_cmd %p) aborted", cmnd,
				cmnd->scst_cmd);
			req_cmnd_release_force(cmnd);
			goto out;
		}

		if (cmnd->scst_cmd == NULL) {
			TRACE_MGMT_DBG("Finishing preliminary completed cmd %p "
				"with NULL scst_cmd", cmnd);
			req_cmnd_release(cmnd);
			goto out;
		}

		status = SCST_PREPROCESS_STATUS_ERROR_SENSE_SET;
	} else
		status = SCST_PREPROCESS_STATUS_SUCCESS;

	cmnd->scst_state = ISCSI_CMD_STATE_RESTARTED;

	scst_restart_cmd(cmnd->scst_cmd, status, SCST_CONTEXT_THREAD);

out:
	TRACE_EXIT();
	return;
}

void iscsi_fail_data_waiting_cmnd(struct iscsi_cmnd *cmnd)
{
	TRACE_ENTRY();

	TRACE_MGMT_DBG("Failing data waiting cmnd %p", cmnd);

	/*
	 * There is no race with conn_abort(), since all functions
	 * called from single read thread
	 */
	iscsi_extracheck_is_rd_thread(cmnd->conn);
	cmnd->r2t_len_to_receive = 0;
	cmnd->r2t_len_to_send = 0;

	req_cmnd_release_force(cmnd);

	TRACE_EXIT();
	return;
}

struct iscsi_cmnd *cmnd_alloc(struct iscsi_conn *conn,
			      struct iscsi_cmnd *parent)
{
	struct iscsi_cmnd *cmnd;

	/* ToDo: __GFP_NOFAIL?? */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 17)
	cmnd = kmem_cache_alloc(iscsi_cmnd_cache, GFP_KERNEL|__GFP_NOFAIL);
	memset(cmnd, 0, sizeof(*cmnd));
#else
	cmnd = kmem_cache_zalloc(iscsi_cmnd_cache, GFP_KERNEL|__GFP_NOFAIL);
#endif

	atomic_set(&cmnd->ref_cnt, 1);
	cmnd->scst_state = ISCSI_CMD_STATE_NEW;
	cmnd->conn = conn;
	cmnd->parent_req = parent;

	if (parent == NULL) {
		conn_get(conn);

#if defined(CONFIG_TCP_ZERO_COPY_TRANSFER_COMPLETION_NOTIFICATION)
		atomic_set(&cmnd->net_ref_cnt, 0);
#endif
		INIT_LIST_HEAD(&cmnd->rsp_cmd_list);
		INIT_LIST_HEAD(&cmnd->rx_ddigest_cmd_list);
		cmnd->target_task_tag = cpu_to_be32(ISCSI_RESERVED_TAG);

		spin_lock_bh(&conn->cmd_list_lock);
		list_add_tail(&cmnd->cmd_list_entry, &conn->cmd_list);
		spin_unlock_bh(&conn->cmd_list_lock);
	}

	TRACE_DBG("conn %p, parent %p, cmnd %p", conn, parent, cmnd);
	return cmnd;
}

/* Frees a command. Also frees the additional header. */
static void cmnd_free(struct iscsi_cmnd *cmnd)
{
	TRACE_ENTRY();

	TRACE_DBG("cmnd %p", cmnd);

	if (unlikely(test_bit(ISCSI_CMD_ABORTED, &cmnd->prelim_compl_flags))) {
		TRACE_MGMT_DBG("Free aborted cmd %p (scst cmd %p, state %d, "
			"parent_req %p)", cmnd, cmnd->scst_cmd,
			cmnd->scst_state, cmnd->parent_req);
	}

	/* Catch users from cmd_list or rsp_cmd_list */
	EXTRACHECKS_BUG_ON(atomic_read(&cmnd->ref_cnt) != 0);

	kfree(cmnd->pdu.ahs);

#ifdef CONFIG_SCST_EXTRACHECKS
	if (unlikely(cmnd->on_write_list || cmnd->on_write_timeout_list)) {
		struct iscsi_scsi_cmd_hdr *req = cmnd_hdr(cmnd);

		PRINT_CRIT_ERROR("cmnd %p still on some list?, %x, %x, %x, "
			"%x, %x, %x, %x", cmnd, req->opcode, req->scb[0],
			req->flags, req->itt, be32_to_cpu(req->data_length),
			req->cmd_sn, be32_to_cpu(cmnd->pdu.datasize));

		if (unlikely(cmnd->parent_req)) {
			struct iscsi_scsi_cmd_hdr *preq =
					cmnd_hdr(cmnd->parent_req);
			PRINT_CRIT_ERROR("%p %x %u", preq, preq->opcode,
				preq->scb[0]);
		}
		sBUG();
	}
#endif

	kmem_cache_free(iscsi_cmnd_cache, cmnd);

	TRACE_EXIT();
	return;
}

/* Might be called under some lock and on SIRQ */
void cmnd_done(struct iscsi_cmnd *cmnd)
{
	TRACE_ENTRY();

	TRACE_DBG("cmnd %p", cmnd);

	if (unlikely(test_bit(ISCSI_CMD_ABORTED, &cmnd->prelim_compl_flags))) {
		TRACE_MGMT_DBG("Done aborted cmd %p (scst cmd %p, state %d, "
			"parent_req %p)", cmnd, cmnd->scst_cmd,
			cmnd->scst_state, cmnd->parent_req);
	}

	EXTRACHECKS_BUG_ON(cmnd->on_rx_digest_list);
	EXTRACHECKS_BUG_ON(cmnd->hashed);

	req_del_from_write_timeout_list(cmnd);

	if (cmnd->parent_req == NULL) {
		struct iscsi_conn *conn = cmnd->conn;
		struct iscsi_cmnd *rsp, *t;

		TRACE_DBG("Deleting req %p from conn %p", cmnd, conn);

		spin_lock_bh(&conn->cmd_list_lock);
		list_del(&cmnd->cmd_list_entry);
		spin_unlock_bh(&conn->cmd_list_lock);

		conn_put(conn);

		EXTRACHECKS_BUG_ON(!list_empty(&cmnd->rx_ddigest_cmd_list));

		/* Order between above and below code is important! */

		if ((cmnd->scst_cmd != NULL) || (cmnd->scst_aen != NULL)) {
			switch (cmnd->scst_state) {
			case ISCSI_CMD_STATE_PROCESSED:
				TRACE_DBG("cmd %p PROCESSED", cmnd);
				scst_tgt_cmd_done(cmnd->scst_cmd,
					SCST_CONTEXT_DIRECT_ATOMIC);
				break;

			case ISCSI_CMD_STATE_AFTER_PREPROC:
			{
				/* It can be for some aborted commands */
				struct scst_cmd *scst_cmd = cmnd->scst_cmd;
				TRACE_DBG("cmd %p AFTER_PREPROC", cmnd);
				cmnd->scst_state = ISCSI_CMD_STATE_RESTARTED;
				cmnd->scst_cmd = NULL;
				scst_restart_cmd(scst_cmd,
					SCST_PREPROCESS_STATUS_ERROR_FATAL,
					SCST_CONTEXT_THREAD);
				break;
			}

			case ISCSI_CMD_STATE_AEN:
				TRACE_DBG("cmd %p AEN PROCESSED", cmnd);
				scst_aen_done(cmnd->scst_aen);
				break;

			case ISCSI_CMD_STATE_OUT_OF_SCST_PRELIM_COMPL:
				break;

			default:
				PRINT_CRIT_ERROR("Unexpected cmnd scst state "
					"%d", cmnd->scst_state);
				sBUG();
				break;
			}
		}

		if (cmnd->own_sg) {
			TRACE_DBG("own_sg for req %p", cmnd);
			if (cmnd->sg != &dummy_sg)
				scst_free(cmnd->sg, cmnd->sg_cnt);
#ifdef CONFIG_SCST_DEBUG
			cmnd->own_sg = 0;
			cmnd->sg = NULL;
			cmnd->sg_cnt = -1;
#endif
		}

		if (cmnd->dec_active_cmnds) {
			struct iscsi_session *sess = cmnd->conn->session;
			TRACE_DBG("Decrementing active_cmds (cmd %p, sess %p, "
				"new value %d)", cmnd, sess,
				atomic_read(&sess->active_cmds)-1);
			atomic_dec(&sess->active_cmds);
#ifdef CONFIG_SCST_EXTRACHECKS
			if (unlikely(atomic_read(&sess->active_cmds) < 0)) {
				PRINT_CRIT_ERROR("active_cmds < 0 (%d)!!",
					atomic_read(&sess->active_cmds));
				sBUG();
			}
#endif
		}

		list_for_each_entry_safe(rsp, t, &cmnd->rsp_cmd_list,
					rsp_cmd_list_entry) {
			cmnd_free(rsp);
		}

		cmnd_free(cmnd);
	} else {
		if (cmnd->own_sg) {
			TRACE_DBG("own_sg for rsp %p", cmnd);
			if ((cmnd->sg != &dummy_sg) && (cmnd->sg != cmnd->rsp_sg))
				scst_free(cmnd->sg, cmnd->sg_cnt);
#ifdef CONFIG_SCST_DEBUG
			cmnd->own_sg = 0;
			cmnd->sg = NULL;
			cmnd->sg_cnt = -1;
#endif
		}

		EXTRACHECKS_BUG_ON(cmnd->dec_active_cmnds);

		if (cmnd == cmnd->parent_req->main_rsp) {
			TRACE_DBG("Finishing main rsp %p (req %p)", cmnd,
				cmnd->parent_req);
			cmnd->parent_req->main_rsp = NULL;
		}

		cmnd_put(cmnd->parent_req);
		/*
		 * rsp will be freed on the last parent's put and can already
		 * be freed!!
		 */
	}

	TRACE_EXIT();
	return;
}

/*
 * Corresponding conn may also get destroyed after this function, except only
 * if it's called from the read thread!
 *
 * It can't be called in parallel with iscsi_cmnds_init_write()!
 */
void req_cmnd_release_force(struct iscsi_cmnd *req)
{
	struct iscsi_cmnd *rsp, *t;
	struct iscsi_conn *conn = req->conn;
	LIST_HEAD(cmds_list);

	TRACE_ENTRY();

	TRACE_MGMT_DBG("req %p", req);

	sBUG_ON(req == conn->read_cmnd);

	spin_lock_bh(&conn->write_list_lock);
	list_for_each_entry_safe(rsp, t, &conn->write_list, write_list_entry) {
		if (rsp->parent_req != req)
			continue;

		cmd_del_from_write_list(rsp);

		list_add_tail(&rsp->write_list_entry, &cmds_list);
	}
	spin_unlock_bh(&conn->write_list_lock);

	list_for_each_entry_safe(rsp, t, &cmds_list, write_list_entry) {
		TRACE_MGMT_DBG("Putting write rsp %p", rsp);
		list_del(&rsp->write_list_entry);
		cmnd_put(rsp);
	}

	/* Supposed nobody can add responses in the list anymore */
	list_for_each_entry_reverse(rsp, &req->rsp_cmd_list,
			rsp_cmd_list_entry) {
		bool r;

		if (rsp->force_cleanup_done)
			continue;

		rsp->force_cleanup_done = 1;

		if (cmnd_get_check(rsp))
			continue;

		spin_lock_bh(&conn->write_list_lock);
		r = rsp->on_write_list || rsp->write_processing_started;
		spin_unlock_bh(&conn->write_list_lock);

		cmnd_put(rsp);

		if (r)
			continue;

		/*
		 * If both on_write_list and write_processing_started not set,
		 * we can safely put() rsp.
		 */
		TRACE_MGMT_DBG("Putting rsp %p", rsp);
		cmnd_put(rsp);
	}

	if (req->main_rsp != NULL) {
		TRACE_MGMT_DBG("Putting main rsp %p", req->main_rsp);
		cmnd_put(req->main_rsp);
		req->main_rsp = NULL;
	}

	req_cmnd_release(req);

	TRACE_EXIT();
	return;
}

/*
 * Corresponding conn may also get destroyed after this function, except only
 * if it's called from the read thread!
 */
static void req_cmnd_release(struct iscsi_cmnd *req)
{
	struct iscsi_cmnd *c, *t;

	TRACE_ENTRY();

	TRACE_DBG("req %p", req);

#ifdef CONFIG_SCST_EXTRACHECKS
	sBUG_ON(req->release_called);
	req->release_called = 1;
#endif

	if (unlikely(test_bit(ISCSI_CMD_ABORTED, &req->prelim_compl_flags))) {
		TRACE_MGMT_DBG("Release aborted req cmd %p (scst cmd %p, "
			"state %d)", req, req->scst_cmd, req->scst_state);
	}

	sBUG_ON(req->parent_req != NULL);

	/*
	 * We have to remove hashed req from the hash list before sending
	 * response. Otherwise we can have a race, when for some reason cmd's
	 * release (and, hence, removal from the hash) is delayed after the
	 * transmission and initiator sends cmd with the same ITT, hence
	 * the new command will be erroneously rejected as a duplicate.
	 */
	if (unlikely(req->hashed)) {
		/* It sometimes can happen during errors recovery */
		cmnd_remove_data_wait_hash(req);
	}

	if (unlikely(req->main_rsp != NULL)) {
		TRACE_DBG("Sending main rsp %p", req->main_rsp);
		iscsi_cmnd_init_write(req->main_rsp, ISCSI_INIT_WRITE_WAKE);
		req->main_rsp = NULL;
	}

	list_for_each_entry_safe(c, t, &req->rx_ddigest_cmd_list,
				rx_ddigest_cmd_list_entry) {
		cmd_del_from_rx_ddigest_list(c);
		cmnd_put(c);
	}

	EXTRACHECKS_BUG_ON(req->pending);

	if (req->dec_active_cmnds) {
		struct iscsi_session *sess = req->conn->session;
		TRACE_DBG("Decrementing active_cmds (cmd %p, sess %p, "
			"new value %d)", req, sess,
			atomic_read(&sess->active_cmds)-1);
		atomic_dec(&sess->active_cmds);
		req->dec_active_cmnds = 0;
#ifdef CONFIG_SCST_EXTRACHECKS
		if (unlikely(atomic_read(&sess->active_cmds) < 0)) {
			PRINT_CRIT_ERROR("active_cmds < 0 (%d)!!",
				atomic_read(&sess->active_cmds));
			sBUG();
		}
#endif
	}

	cmnd_put(req);

	TRACE_EXIT();
	return;
}

/*
 * Corresponding conn may also get destroyed after this function, except only
 * if it's called from the read thread!
 */
void rsp_cmnd_release(struct iscsi_cmnd *cmnd)
{
	TRACE_DBG("%p", cmnd);

#ifdef CONFIG_SCST_EXTRACHECKS
	sBUG_ON(cmnd->release_called);
	cmnd->release_called = 1;
#endif

	EXTRACHECKS_BUG_ON(cmnd->parent_req == NULL);

	cmnd_put(cmnd);
	return;
}

static struct iscsi_cmnd *iscsi_alloc_rsp(struct iscsi_cmnd *parent)
{
	struct iscsi_cmnd *rsp;

	TRACE_ENTRY();

	rsp = cmnd_alloc(parent->conn, parent);

	TRACE_DBG("Adding rsp %p to parent %p", rsp, parent);
	list_add_tail(&rsp->rsp_cmd_list_entry, &parent->rsp_cmd_list);

	cmnd_get(parent);

	TRACE_EXIT_HRES((unsigned long)rsp);
	return rsp;
}

static inline struct iscsi_cmnd *iscsi_alloc_main_rsp(struct iscsi_cmnd *parent)
{
	struct iscsi_cmnd *rsp;

	TRACE_ENTRY();

	EXTRACHECKS_BUG_ON(parent->main_rsp != NULL);

	rsp = iscsi_alloc_rsp(parent);
	parent->main_rsp = rsp;

	TRACE_EXIT_HRES((unsigned long)rsp);
	return rsp;
}

static void iscsi_cmnds_init_write(struct list_head *send, int flags)
{
	struct iscsi_cmnd *rsp = list_entry(send->next, struct iscsi_cmnd,
						write_list_entry);
	struct iscsi_conn *conn = rsp->conn;
	struct list_head *pos, *next;

	sBUG_ON(list_empty(send));

	if (!(conn->ddigest_type & DIGEST_NONE)) {
		list_for_each(pos, send) {
			rsp = list_entry(pos, struct iscsi_cmnd,
						write_list_entry);

			if (rsp->pdu.datasize != 0) {
				TRACE_DBG("Doing data digest (%p:%x)", rsp,
					cmnd_opcode(rsp));
				digest_tx_data(rsp);
			}
		}
	}

	spin_lock_bh(&conn->write_list_lock);
	list_for_each_safe(pos, next, send) {
		rsp = list_entry(pos, struct iscsi_cmnd, write_list_entry);

		TRACE_DBG("%p:%x", rsp, cmnd_opcode(rsp));

		sBUG_ON(conn != rsp->conn);

		list_del(&rsp->write_list_entry);
		cmd_add_on_write_list(conn, rsp);
	}
	spin_unlock_bh(&conn->write_list_lock);

	if (flags & ISCSI_INIT_WRITE_WAKE)
		iscsi_make_conn_wr_active(conn);

	return;
}

static void iscsi_cmnd_init_write(struct iscsi_cmnd *rsp, int flags)
{
	LIST_HEAD(head);

#ifdef CONFIG_SCST_EXTRACHECKS
	if (unlikely(rsp->on_write_list)) {
		PRINT_CRIT_ERROR("cmd already on write list (%x %x %x "
			"%u %u %d %d", cmnd_itt(rsp),
			cmnd_opcode(rsp), cmnd_scsicode(rsp),
			rsp->hdigest, rsp->ddigest,
			list_empty(&rsp->rsp_cmd_list), rsp->hashed);
		sBUG();
	}
#endif
	list_add_tail(&rsp->write_list_entry, &head);
	iscsi_cmnds_init_write(&head, flags);
	return;
}

static void send_data_rsp(struct iscsi_cmnd *req, u8 status, int send_status)
{
	struct iscsi_cmnd *rsp;
	struct iscsi_scsi_cmd_hdr *req_hdr = cmnd_hdr(req);
	struct iscsi_data_in_hdr *rsp_hdr;
	u32 pdusize, expsize, size, offset, sn;
	LIST_HEAD(send);

	TRACE_DBG("req %p", req);

	pdusize = req->conn->session->sess_params.max_xmit_data_length;
	expsize = req->read_size;
	size = min(expsize, (u32)req->bufflen);
	offset = 0;
	sn = 0;

	while (1) {
		rsp = iscsi_alloc_rsp(req);
		TRACE_DBG("rsp %p", rsp);
		rsp->sg = req->sg;
		rsp->sg_cnt = req->sg_cnt;
		rsp->bufflen = req->bufflen;
		rsp_hdr = (struct iscsi_data_in_hdr *)&rsp->pdu.bhs;

		rsp_hdr->opcode = ISCSI_OP_SCSI_DATA_IN;
		rsp_hdr->itt = req_hdr->itt;
		rsp_hdr->ttt = cpu_to_be32(ISCSI_RESERVED_TAG);
		rsp_hdr->buffer_offset = cpu_to_be32(offset);
		rsp_hdr->data_sn = cpu_to_be32(sn);

		if (size <= pdusize) {
			TRACE_DBG("offset %d, size %d", offset, size);
			rsp->pdu.datasize = size;
			if (send_status) {
				unsigned int scsisize;

				TRACE_DBG("status %x", status);

				EXTRACHECKS_BUG_ON((cmnd_hdr(req)->flags & ISCSI_CMD_WRITE) != 0);

				rsp_hdr->flags = ISCSI_FLG_FINAL | ISCSI_FLG_STATUS;
				rsp_hdr->cmd_status = status;

				scsisize = req->bufflen;
				if (scsisize < expsize) {
					rsp_hdr->flags |= ISCSI_FLG_RESIDUAL_UNDERFLOW;
					size = expsize - scsisize;
				} else if (scsisize > expsize) {
					rsp_hdr->flags |= ISCSI_FLG_RESIDUAL_OVERFLOW;
					size = scsisize - expsize;
				} else
					size = 0;
				rsp_hdr->residual_count = cpu_to_be32(size);
			}
			list_add_tail(&rsp->write_list_entry, &send);
			break;
		}

		TRACE_DBG("pdusize %d, offset %d, size %d", pdusize, offset,
			size);

		rsp->pdu.datasize = pdusize;

		size -= pdusize;
		offset += pdusize;
		sn++;

		list_add_tail(&rsp->write_list_entry, &send);
	}
	iscsi_cmnds_init_write(&send, 0);
	return;
}

static void iscsi_init_status_rsp(struct iscsi_cmnd *rsp,
	int status, const u8 *sense_buf, int sense_len, bool bufflen_set)
{
	struct iscsi_cmnd *req = rsp->parent_req;
	struct iscsi_scsi_rsp_hdr *rsp_hdr;
	struct scatterlist *sg;

	TRACE_ENTRY();

	rsp_hdr = (struct iscsi_scsi_rsp_hdr *)&rsp->pdu.bhs;
	rsp_hdr->opcode = ISCSI_OP_SCSI_RSP;
	rsp_hdr->flags = ISCSI_FLG_FINAL;
	rsp_hdr->response = ISCSI_RESPONSE_COMMAND_COMPLETED;
	rsp_hdr->cmd_status = status;
	rsp_hdr->itt = cmnd_hdr(req)->itt;

	if (SCST_SENSE_VALID(sense_buf)) {
		TRACE_DBG("%s", "SENSE VALID");

		sg = rsp->sg = rsp->rsp_sg;
		rsp->sg_cnt = 2;
		rsp->own_sg = 1;

		sg_init_table(sg, 2);
		sg_set_buf(&sg[0], &rsp->sense_hdr, sizeof(rsp->sense_hdr));
		sg_set_buf(&sg[1], sense_buf, sense_len);

		rsp->sense_hdr.length = cpu_to_be16(sense_len);

		rsp->pdu.datasize = sizeof(rsp->sense_hdr) + sense_len;
		rsp->bufflen = rsp->pdu.datasize;
	} else {
		rsp->pdu.datasize = 0;
		rsp->bufflen = 0;
	}

	iscsi_set_resid(rsp, bufflen_set);

	TRACE_EXIT();
	return;
}

static inline struct iscsi_cmnd *create_status_rsp(struct iscsi_cmnd *req,
	int status, const u8 *sense_buf, int sense_len, bool bufflen_set)
{
	struct iscsi_cmnd *rsp;

	TRACE_ENTRY();

	rsp = iscsi_alloc_rsp(req);
	TRACE_DBG("rsp %p", rsp);

	iscsi_init_status_rsp(rsp, status, sense_buf, sense_len, bufflen_set);

	TRACE_EXIT_HRES((unsigned long)rsp);
	return rsp;
}

static struct iscsi_cmnd *create_prelim_status_rsp(struct iscsi_cmnd *req,
	int status, const u8 *sense_buf, int sense_len)
{
	struct iscsi_cmnd *rsp;

	TRACE_ENTRY();

	rsp = iscsi_alloc_main_rsp(req);
	TRACE_DBG("main rsp %p", rsp);

	iscsi_init_status_rsp(rsp, status, sense_buf, sense_len, false);

	TRACE_EXIT_HRES((unsigned long)rsp);
	return rsp;
}

static int iscsi_set_prelim_r2t_len_to_receive(struct iscsi_cmnd *req)
{
	struct iscsi_hdr *req_hdr = &req->pdu.bhs;
	int res = 0;

	TRACE_ENTRY();

	if (req_hdr->flags & ISCSI_CMD_FINAL)
		goto out;

	res = cmnd_insert_data_wait_hash(req);
	if (res != 0) {
		/*
		 * We have to close connection, because otherwise a data
		 * corruption is possible if we allow to receive data
		 * for this request in another request with dublicated ITT.
		 */
		mark_conn_closed(req->conn);
		goto out;
	}

	/*
	 * We need to wait for one or more PDUs. Let's simplify
	 * other code and pretend we need to receive 1 byte.
	 * In data_out_start() we will correct it.
	 */
	if (req->outstanding_r2t == 0) {
		req->outstanding_r2t = 1;
		req_add_to_write_timeout_list(req);
	}
	req->r2t_len_to_receive = 1;
	req->r2t_len_to_send = 0;

	TRACE_DBG("req %p, op %x, outstanding_r2t %d, r2t_len_to_receive %d, "
		"r2t_len_to_send %d", req, cmnd_opcode(req),
		req->outstanding_r2t, req->r2t_len_to_receive,
		req->r2t_len_to_send);

out:
	TRACE_EXIT_RES(res);
	return res;
}

static int create_preliminary_status_rsp(struct iscsi_cmnd *req,
	int status, const u8 *sense_buf, int sense_len)
{
	int res = 0;
	struct iscsi_scsi_cmd_hdr *req_hdr = cmnd_hdr(req);

	TRACE_ENTRY();

	if (req->prelim_compl_flags != 0) {
		TRACE_MGMT_DBG("req %p already prelim completed", req);
		goto out;
	}

	req->scst_state = ISCSI_CMD_STATE_OUT_OF_SCST_PRELIM_COMPL;

	if ((req_hdr->flags & ISCSI_CMD_READ) &&
	    (req_hdr->flags & ISCSI_CMD_WRITE)) {
		int sz = cmnd_read_size(req);
		if (sz > 0)
			req->read_size = sz;
	} else if (req_hdr->flags & ISCSI_CMD_READ)
		req->read_size = be32_to_cpu(req_hdr->data_length);

	create_prelim_status_rsp(req, status, sense_buf, sense_len);
	res = iscsi_preliminary_complete(req, req, true);

out:
	TRACE_EXIT_RES(res);
	return res;
}

static int set_scst_preliminary_status_rsp(struct iscsi_cmnd *req,
	bool get_data, int key, int asc, int ascq)
{
	int res = 0;

	TRACE_ENTRY();

	if (req->scst_cmd == NULL) {
		/* There must be already error set */
		goto complete;
	}

	scst_set_cmd_error(req->scst_cmd, key, asc, ascq);

complete:
	res = iscsi_preliminary_complete(req, req, get_data);

	TRACE_EXIT_RES(res);
	return res;
}

static int create_reject_rsp(struct iscsi_cmnd *req, int reason, bool get_data)
{
	int res = 0;
	struct iscsi_cmnd *rsp;
	struct iscsi_reject_hdr *rsp_hdr;
	struct scatterlist *sg;

	TRACE_ENTRY();

	TRACE_MGMT_DBG("Reject: req %p, reason %x", req, reason);

	if (cmnd_opcode(req) == ISCSI_OP_SCSI_CMD) {
		if (req->scst_cmd == NULL) {
			/* BUSY status must be already set */
			struct iscsi_scsi_rsp_hdr *rsp_hdr;
			rsp_hdr = (struct iscsi_scsi_rsp_hdr *)&req->main_rsp->pdu.bhs;
			sBUG_ON(rsp_hdr->cmd_status == 0);
			/*
			 * Let's not send REJECT here. The initiator will retry
			 * and, hopefully, next time we will not fail allocating
			 * scst_cmd, so we will then send the REJECT.
			 */
			goto out;
		} else
			set_scst_preliminary_status_rsp(req, get_data,
				SCST_LOAD_SENSE(scst_sense_invalid_message));
	}

	rsp = iscsi_alloc_main_rsp(req);
	rsp_hdr = (struct iscsi_reject_hdr *)&rsp->pdu.bhs;

	rsp_hdr->opcode = ISCSI_OP_REJECT;
	rsp_hdr->ffffffff = ISCSI_RESERVED_TAG;
	rsp_hdr->reason = reason;

	sg = rsp->sg = rsp->rsp_sg;
	rsp->sg_cnt = 1;
	rsp->own_sg = 1;
	sg_init_one(sg, &req->pdu.bhs, sizeof(struct iscsi_hdr));
	rsp->bufflen = rsp->pdu.datasize = sizeof(struct iscsi_hdr);

	res = iscsi_preliminary_complete(req, req, true);

out:
	TRACE_EXIT_RES(res);
	return res;
}

static inline int iscsi_get_allowed_cmds(struct iscsi_session *sess)
{
	int res = max(-1, (int)sess->tgt_params.queued_cmnds -
				atomic_read(&sess->active_cmds)-1);
	TRACE_DBG("allowed cmds %d (sess %p, active_cmds %d)", res,
		sess, atomic_read(&sess->active_cmds));
	return res;
}

static u32 cmnd_set_sn(struct iscsi_cmnd *cmnd, int set_stat_sn)
{
	struct iscsi_conn *conn = cmnd->conn;
	struct iscsi_session *sess = conn->session;
	u32 res;

	spin_lock(&sess->sn_lock);

	if (set_stat_sn)
		cmnd->pdu.bhs.sn = cpu_to_be32(conn->stat_sn++);
	cmnd->pdu.bhs.exp_sn = cpu_to_be32(sess->exp_cmd_sn);
	cmnd->pdu.bhs.max_sn = cpu_to_be32(sess->exp_cmd_sn +
				 iscsi_get_allowed_cmds(sess));

	res = cpu_to_be32(conn->stat_sn);

	spin_unlock(&sess->sn_lock);
	return res;
}

/* Called under sn_lock */
static void __update_stat_sn(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;
	u32 exp_stat_sn;

	cmnd->pdu.bhs.exp_sn = exp_stat_sn = be32_to_cpu(cmnd->pdu.bhs.exp_sn);
	TRACE_DBG("%x,%x", cmnd_opcode(cmnd), exp_stat_sn);
	if ((int)(exp_stat_sn - conn->exp_stat_sn) > 0 &&
	    (int)(exp_stat_sn - conn->stat_sn) <= 0) {
		/* free pdu resources */
		cmnd->conn->exp_stat_sn = exp_stat_sn;
	}
	return;
}

static inline void update_stat_sn(struct iscsi_cmnd *cmnd)
{
	spin_lock(&cmnd->conn->session->sn_lock);
	__update_stat_sn(cmnd);
	spin_unlock(&cmnd->conn->session->sn_lock);
	return;
}

/* Called under sn_lock */
static int check_cmd_sn(struct iscsi_cmnd *cmnd)
{
	struct iscsi_session *session = cmnd->conn->session;
	u32 cmd_sn;

	cmnd->pdu.bhs.sn = cmd_sn = be32_to_cpu(cmnd->pdu.bhs.sn);
	TRACE_DBG("%d(%d)", cmd_sn, session->exp_cmd_sn);
	if (likely((s32)(cmd_sn - session->exp_cmd_sn) >= 0))
		return 0;
	PRINT_ERROR("sequence error (%x,%x)", cmd_sn, session->exp_cmd_sn);
	return -ISCSI_REASON_PROTOCOL_ERROR;
}

static struct iscsi_cmnd *cmnd_find_itt_get(struct iscsi_conn *conn, u32 itt)
{
	struct iscsi_cmnd *cmnd, *found_cmnd = NULL;

	spin_lock_bh(&conn->cmd_list_lock);
	list_for_each_entry(cmnd, &conn->cmd_list, cmd_list_entry) {
		if ((cmnd->pdu.bhs.itt == itt) && !cmnd_get_check(cmnd)) {
			found_cmnd = cmnd;
			break;
		}
	}
	spin_unlock_bh(&conn->cmd_list_lock);

	return found_cmnd;
}

/**
 ** We use the ITT hash only to find original request PDU for subsequent
 ** Data-Out PDUs.
 **/

/* Must be called under cmnd_data_wait_hash_lock */
static struct iscsi_cmnd *__cmnd_find_data_wait_hash(struct iscsi_conn *conn,
	u32 itt)
{
	struct list_head *head;
	struct iscsi_cmnd *cmnd;

	head = &conn->session->cmnd_data_wait_hash[cmnd_hashfn(itt)];

	list_for_each_entry(cmnd, head, hash_list_entry) {
		if (cmnd->pdu.bhs.itt == itt)
			return cmnd;
	}
	return NULL;
}

static struct iscsi_cmnd *cmnd_find_data_wait_hash(struct iscsi_conn *conn,
	u32 itt)
{
	struct iscsi_cmnd *res;
	struct iscsi_session *session = conn->session;

	spin_lock(&session->cmnd_data_wait_hash_lock);
	res = __cmnd_find_data_wait_hash(conn, itt);
	spin_unlock(&session->cmnd_data_wait_hash_lock);

	return res;
}

static inline u32 get_next_ttt(struct iscsi_conn *conn)
{
	u32 ttt;
	struct iscsi_session *session = conn->session;

	/* Not compatible with MC/S! */

	iscsi_extracheck_is_rd_thread(conn);

	if (unlikely(session->next_ttt == ISCSI_RESERVED_TAG))
		session->next_ttt++;
	ttt = session->next_ttt++;

	return ttt;
}

static int cmnd_insert_data_wait_hash(struct iscsi_cmnd *cmnd)
{
	struct iscsi_session *session = cmnd->conn->session;
	struct iscsi_cmnd *tmp;
	struct list_head *head;
	int err = 0;
	u32 itt = cmnd->pdu.bhs.itt;

	if (unlikely(cmnd->hashed)) {
		/* It can be for preliminary completed commands */
		goto out;
	}

	/*
	 * We don't need TTT, because ITT/buffer_offset pair is sufficient
	 * to find out the original request and buffer for Data-Out PDUs, but
	 * crazy iSCSI spec requires us to send this superfluous field in
	 * R2T PDUs and some initiators may rely on it.
	 */
	cmnd->target_task_tag = get_next_ttt(cmnd->conn);

	TRACE_DBG("%p:%x", cmnd, itt);
	if (unlikely(itt == ISCSI_RESERVED_TAG)) {
		PRINT_ERROR("%s", "ITT is RESERVED_TAG");
		PRINT_BUFFER("Incorrect BHS", &cmnd->pdu.bhs,
			sizeof(cmnd->pdu.bhs));
		err = -ISCSI_REASON_PROTOCOL_ERROR;
		goto out;
	}

	spin_lock(&session->cmnd_data_wait_hash_lock);

	head = &session->cmnd_data_wait_hash[cmnd_hashfn(itt)];

	tmp = __cmnd_find_data_wait_hash(cmnd->conn, itt);
	if (likely(!tmp)) {
		TRACE_DBG("Adding cmnd %p to the hash (ITT %x)", cmnd,
			cmnd_itt(cmnd));
		list_add_tail(&cmnd->hash_list_entry, head);
		cmnd->hashed = 1;
	} else {
		PRINT_ERROR("Task %x in progress, cmnd %p", itt, cmnd);
		err = -ISCSI_REASON_TASK_IN_PROGRESS;
	}

	spin_unlock(&session->cmnd_data_wait_hash_lock);

out:
	return err;
}

static void cmnd_remove_data_wait_hash(struct iscsi_cmnd *cmnd)
{
	struct iscsi_session *session = cmnd->conn->session;
	struct iscsi_cmnd *tmp;

	spin_lock(&session->cmnd_data_wait_hash_lock);

	tmp = __cmnd_find_data_wait_hash(cmnd->conn, cmnd->pdu.bhs.itt);

	if (likely(tmp && tmp == cmnd)) {
		TRACE_DBG("Deleting cmnd %p from the hash (ITT %x)", cmnd,
			cmnd_itt(cmnd));
		list_del(&cmnd->hash_list_entry);
		cmnd->hashed = 0;
	} else
		PRINT_ERROR("%p:%x not found", cmnd, cmnd_itt(cmnd));

	spin_unlock(&session->cmnd_data_wait_hash_lock);

	return;
}

static void cmnd_prepare_get_rejected_immed_data(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;
	struct scatterlist *sg = cmnd->sg;
	char __user *addr;
	u32 size;
	unsigned int i;

	TRACE_ENTRY();

	TRACE_DBG_FLAG(iscsi_get_flow_ctrl_or_mgmt_dbg_log_flag(cmnd),
		"Skipping (cmnd %p, ITT %x, op %x, cmd op %x, "
		"datasize %u, scst_cmd %p, scst state %d)", cmnd,
		cmnd_itt(cmnd), cmnd_opcode(cmnd), cmnd_hdr(cmnd)->scb[0],
		cmnd->pdu.datasize, cmnd->scst_cmd, cmnd->scst_state);

	iscsi_extracheck_is_rd_thread(conn);

	size = cmnd->pdu.datasize;
	if (!size)
		goto out;

	/* We already checked pdu.datasize in check_segment_length() */

	if (sg == NULL) {
		/*
		 * There are no problems with the safety from concurrent
		 * accesses to dummy_page in dummy_sg, since data only
		 * will be read and then discarded.
		 */
		sg = cmnd->sg = &dummy_sg;
		cmnd->bufflen = PAGE_SIZE;
		cmnd->own_sg = 1;
	}

	addr = (char __force __user *)(page_address(sg_page(&sg[0])));
	sBUG_ON(addr == NULL);
	conn->read_size = size;
	for (i = 0; size > PAGE_SIZE; i++, size -= cmnd->bufflen) {
		/* We already checked pdu.datasize in check_segment_length() */
		sBUG_ON(i >= ISCSI_CONN_IOV_MAX);
		conn->read_iov[i].iov_base = addr;
		conn->read_iov[i].iov_len = cmnd->bufflen;
	}
	conn->read_iov[i].iov_base = addr;
	conn->read_iov[i].iov_len = size;
	conn->read_msg.msg_iov = conn->read_iov;
	conn->read_msg.msg_iovlen = ++i;

out:
	TRACE_EXIT();
	return;
}

static void iscsi_set_resid(struct iscsi_cmnd *rsp, bool bufflen_set)
{
	struct iscsi_cmnd *req = rsp->parent_req;
	struct iscsi_scsi_cmd_hdr *req_hdr = cmnd_hdr(req);
	struct iscsi_scsi_rsp_hdr *rsp_hdr;
	int resid, resp_len, in_resp_len;

	if ((req_hdr->flags & ISCSI_CMD_READ) &&
	    (req_hdr->flags & ISCSI_CMD_WRITE)) {
		rsp_hdr = (struct iscsi_scsi_rsp_hdr *)&rsp->pdu.bhs;

		if (bufflen_set) {
			resp_len = req->bufflen;
			if (req->scst_cmd != NULL)
				in_resp_len = scst_cmd_get_in_bufflen(req->scst_cmd);
			else
				in_resp_len = 0;
		} else {
			resp_len = 0;
			in_resp_len = 0;
		}

		resid = be32_to_cpu(req_hdr->data_length) - in_resp_len;
		if (resid > 0) {
			rsp_hdr->flags |= ISCSI_FLG_RESIDUAL_UNDERFLOW;
			rsp_hdr->residual_count = cpu_to_be32(resid);
		} else if (resid < 0) {
			resid = -resid;
			rsp_hdr->flags |= ISCSI_FLG_RESIDUAL_OVERFLOW;
			rsp_hdr->residual_count = cpu_to_be32(resid);
		}

		resid = req->read_size - resp_len;
		if (resid > 0) {
			rsp_hdr->flags |= ISCSI_FLG_BIRESIDUAL_UNDERFLOW;
			rsp_hdr->bi_residual_count = cpu_to_be32(resid);
		} else if (resid < 0) {
			resid = -resid;
			rsp_hdr->flags |= ISCSI_FLG_BIRESIDUAL_OVERFLOW;
			rsp_hdr->bi_residual_count = cpu_to_be32(resid);
		}
	} else {
		if (bufflen_set)
			resp_len = req->bufflen;
		else
			resp_len = 0;

		resid = req->read_size - resp_len;
		if (resid > 0) {
			rsp_hdr = (struct iscsi_scsi_rsp_hdr *)&rsp->pdu.bhs;
			rsp_hdr->flags |= ISCSI_FLG_RESIDUAL_UNDERFLOW;
			rsp_hdr->residual_count = cpu_to_be32(resid);
		} else if (resid < 0) {
			rsp_hdr = (struct iscsi_scsi_rsp_hdr *)&rsp->pdu.bhs;
			resid = -resid;
			rsp_hdr->flags |= ISCSI_FLG_RESIDUAL_OVERFLOW;
			rsp_hdr->residual_count = cpu_to_be32(resid);
		}
	}
	return;
}

static int iscsi_preliminary_complete(struct iscsi_cmnd *req,
	struct iscsi_cmnd *orig_req, bool get_data)
{
	int res = 0;
	bool set_r2t_len;

	TRACE_ENTRY();

#ifdef CONFIG_SCST_DEBUG
	{
		struct iscsi_hdr *req_hdr = &req->pdu.bhs;
		TRACE_DBG_FLAG(iscsi_get_flow_ctrl_or_mgmt_dbg_log_flag(orig_req),
			"Prelim completed req %p, orig_req %p (FINAL %x, "
			"outstanding_r2t %d)", req, orig_req,
			(req_hdr->flags & ISCSI_CMD_FINAL),
			orig_req->outstanding_r2t);
	}
#endif

	iscsi_extracheck_is_rd_thread(req->conn);
	sBUG_ON(req->parent_req != NULL);

	if (test_bit(ISCSI_CMD_PRELIM_COMPLETED, &req->prelim_compl_flags)) {
		TRACE_MGMT_DBG("req %p already prelim completed", req);
		/* To not try to get data twice */
		get_data = false;
	}

	set_r2t_len = !req->hashed &&
		      (cmnd_opcode(req) == ISCSI_OP_SCSI_CMD) &&
		      !test_bit(ISCSI_CMD_PRELIM_COMPLETED,
				&orig_req->prelim_compl_flags);
	set_bit(ISCSI_CMD_PRELIM_COMPLETED, &orig_req->prelim_compl_flags);

	TRACE_DBG("get_data %d, set_r2t_len %d", get_data, set_r2t_len);

	if (get_data)
		cmnd_prepare_get_rejected_immed_data(req);

	if (set_r2t_len)
		res = iscsi_set_prelim_r2t_len_to_receive(orig_req);

	TRACE_EXIT_RES(res);
	return res;
}

static int cmnd_prepare_recv_pdu(struct iscsi_conn *conn,
	struct iscsi_cmnd *cmd,	u32 offset, u32 size)
{
	struct scatterlist *sg = cmd->sg;
	unsigned int bufflen = cmd->bufflen;
	unsigned int idx, i, buff_offs;
	int res = 0;

	TRACE_ENTRY();

	TRACE_DBG("cmd %p, sg %p, offset %u, size %u", cmd, cmd->sg,
		offset, size);

	iscsi_extracheck_is_rd_thread(conn);

	buff_offs = offset;
	idx = (offset + sg[0].offset) >> PAGE_SHIFT;
	offset &= ~PAGE_MASK;

	conn->read_msg.msg_iov = conn->read_iov;
	conn->read_size = size;

	i = 0;
	while (1) {
		unsigned int sg_len;
		char __user *addr;
		
		if (unlikely(buff_offs >= bufflen)) {
			TRACE_DBG("Residual overflow (cmd %p, buff_offs %d, "
				"bufflen %d)", cmd, buff_offs, bufflen);
			idx = 0;
			sg = &dummy_sg;
			offset = 0;
		}

		addr = (char __force __user *)(sg_virt(&sg[idx]));
		EXTRACHECKS_BUG_ON(addr == NULL);
		sg_len = sg[idx].length - offset;

		conn->read_iov[i].iov_base = addr + offset;

		if (size <= sg_len) {
			TRACE_DBG("idx=%d, offset=%u, size=%d, addr=%p",
				idx, offset, size, addr);
			conn->read_iov[i].iov_len = size;
			conn->read_msg.msg_iovlen = i;
			break;
		}
		conn->read_iov[i].iov_len = sg_len;

		TRACE_DBG("idx=%d, offset=%u, size=%d, sg_len=%zd, addr=%p",
			idx, offset, size, sg_len, addr);

		size -= sg_len;
		buff_offs += sg_len;

		i++;
		if (unlikely(i >= ISCSI_CONN_IOV_MAX)) {
			PRINT_ERROR("Initiator %s violated negotiated "
				"parameters by sending too much data (size "
				"left %d)", conn->session->initiator_name,
				size);
			mark_conn_closed(conn);
			res = -EINVAL;
			break;
		}

		idx++;
		offset = 0;
	}

	TRACE_DBG("msg_iov=%p, msg_iovlen=%zd",
		conn->read_msg.msg_iov, conn->read_msg.msg_iovlen);

	TRACE_EXIT_RES(res);
	return res;
}

static void send_r2t(struct iscsi_cmnd *req)
{
	struct iscsi_session *sess = req->conn->session;
	struct iscsi_cmnd *rsp;
	struct iscsi_r2t_hdr *rsp_hdr;
	u32 offset, burst;
	LIST_HEAD(send);

	TRACE_ENTRY();

	EXTRACHECKS_BUG_ON(req->r2t_len_to_send == 0);

	/*
	 * There is no race with data_out_start() and conn_abort(), since
	 * all functions called from single read thread
	 */
	iscsi_extracheck_is_rd_thread(req->conn);

	/*
	 * We don't need to check for PRELIM_COMPLETED here, because for such
	 * commands we set r2t_len_to_send = 0, hence made sure we won't
	 * called here.
	 */

	EXTRACHECKS_BUG_ON(req->outstanding_r2t >
			   sess->sess_params.max_outstanding_r2t);

	if (req->outstanding_r2t == sess->sess_params.max_outstanding_r2t)
		goto out;

	burst = sess->sess_params.max_burst_length;
	offset = be32_to_cpu(cmnd_hdr(req)->data_length) -
			req->r2t_len_to_send;

	do {
		rsp = iscsi_alloc_rsp(req);
		rsp->pdu.bhs.ttt = req->target_task_tag;
		rsp_hdr = (struct iscsi_r2t_hdr *)&rsp->pdu.bhs;
		rsp_hdr->opcode = ISCSI_OP_R2T;
		rsp_hdr->flags = ISCSI_FLG_FINAL;
		rsp_hdr->lun = cmnd_hdr(req)->lun;
		rsp_hdr->itt = cmnd_hdr(req)->itt;
		rsp_hdr->r2t_sn = cpu_to_be32(req->r2t_sn++);
		rsp_hdr->buffer_offset = cpu_to_be32(offset);
		if (req->r2t_len_to_send > burst) {
			rsp_hdr->data_length = cpu_to_be32(burst);
			req->r2t_len_to_send -= burst;
			offset += burst;
		} else {
			rsp_hdr->data_length = cpu_to_be32(req->r2t_len_to_send);
			req->r2t_len_to_send = 0;
		}

		TRACE_WRITE("req %p, data_length %u, buffer_offset %u, "
			"r2t_sn %u, outstanding_r2t %u", req,
			be32_to_cpu(rsp_hdr->data_length),
			be32_to_cpu(rsp_hdr->buffer_offset),
			be32_to_cpu(rsp_hdr->r2t_sn), req->outstanding_r2t);

		list_add_tail(&rsp->write_list_entry, &send);
		req->outstanding_r2t++;

	} while ((req->outstanding_r2t < sess->sess_params.max_outstanding_r2t) &&
		 (req->r2t_len_to_send != 0));

	iscsi_cmnds_init_write(&send, ISCSI_INIT_WRITE_WAKE);

out:
	TRACE_EXIT();
	return;
}

static int iscsi_pre_exec(struct scst_cmd *scst_cmd)
{
	int res = SCST_PREPROCESS_STATUS_SUCCESS;
	struct iscsi_cmnd *req = (struct iscsi_cmnd *)
		scst_cmd_get_tgt_priv(scst_cmd);
	struct iscsi_cmnd *c, *t;

	TRACE_ENTRY();

	EXTRACHECKS_BUG_ON(scst_cmd_atomic(scst_cmd));

	/* If data digest isn't used this list will be empty */
	list_for_each_entry_safe(c, t, &req->rx_ddigest_cmd_list,
				rx_ddigest_cmd_list_entry) {
		TRACE_DBG("Checking digest of RX ddigest cmd %p", c);
		if (digest_rx_data(c) != 0) {
			scst_set_cmd_error(scst_cmd,
				SCST_LOAD_SENSE(iscsi_sense_crc_error));
			res = SCST_PREPROCESS_STATUS_ERROR_SENSE_SET;
			/*
			 * The rest of rx_ddigest_cmd_list will be freed
			 * in req_cmnd_release()
			 */
			goto out;
		}
		cmd_del_from_rx_ddigest_list(c);
		cmnd_put(c);
	}

out:
	TRACE_EXIT_RES(res);
	return res;
}

static int nop_out_start(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;
	struct iscsi_hdr *req_hdr = &cmnd->pdu.bhs;
	u32 size, tmp;
	int i, err = 0;

	TRACE_DBG("%p", cmnd);

	iscsi_extracheck_is_rd_thread(conn);

	if (!(req_hdr->flags & ISCSI_FLG_FINAL)) {
		PRINT_ERROR("%s", "Initiator sent Nop-Out with not a single "
			"PDU");
		err = -ISCSI_REASON_PROTOCOL_ERROR;
		goto out;
	}

	if (cmnd_itt(cmnd) == cpu_to_be32(ISCSI_RESERVED_TAG)) {
		if (unlikely(!(cmnd->pdu.bhs.opcode & ISCSI_OP_IMMEDIATE)))
			PRINT_ERROR("%s", "Initiator sent RESERVED tag for "
				"non-immediate Nop-Out command");
	}

	spin_lock(&conn->session->sn_lock);
	__update_stat_sn(cmnd);
	err = check_cmd_sn(cmnd);
	spin_unlock(&conn->session->sn_lock);
	if (unlikely(err))
		goto out;

	size = cmnd->pdu.datasize;

	if (size) {
		conn->read_msg.msg_iov = conn->read_iov;
		if (cmnd->pdu.bhs.itt != cpu_to_be32(ISCSI_RESERVED_TAG)) {
			struct scatterlist *sg;

			cmnd->sg = sg = scst_alloc(size, GFP_KERNEL,
						&cmnd->sg_cnt);
			if (sg == NULL) {
				TRACE(TRACE_OUT_OF_MEM, "Allocating buffer for"
				      " %d Nop-Out payload failed", size);
				err = -ISCSI_REASON_OUT_OF_RESOURCES;
				goto out;
			}

			/* We already checked it in check_segment_length() */
			sBUG_ON(cmnd->sg_cnt > (signed)ISCSI_CONN_IOV_MAX);

			cmnd->own_sg = 1;
			cmnd->bufflen = size;

			for (i = 0; i < cmnd->sg_cnt; i++) {
				conn->read_iov[i].iov_base =
					(void __force __user *)(page_address(sg_page(&sg[i])));
				tmp = min_t(u32, size, PAGE_SIZE);
				conn->read_iov[i].iov_len = tmp;
				conn->read_size += tmp;
				size -= tmp;
			}
			sBUG_ON(size != 0);
		} else {
			/*
			 * There are no problems with the safety from concurrent
			 * accesses to dummy_page, since for ISCSI_RESERVED_TAG
			 * the data only read and then discarded.
			 */
			for (i = 0; i < (signed)ISCSI_CONN_IOV_MAX; i++) {
				conn->read_iov[i].iov_base =
					(void __force __user *)(page_address(dummy_page));
				tmp = min_t(u32, size, PAGE_SIZE);
				conn->read_iov[i].iov_len = tmp;
				conn->read_size += tmp;
				size -= tmp;
			}

			/* We already checked size in check_segment_length() */
			sBUG_ON(size != 0);
		}

		conn->read_msg.msg_iovlen = i;
		TRACE_DBG("msg_iov=%p, msg_iovlen=%zd", conn->read_msg.msg_iov,
			conn->read_msg.msg_iovlen);
	}

out:
	return err;
}

int cmnd_rx_continue(struct iscsi_cmnd *req)
{
	struct iscsi_conn *conn = req->conn;
	struct iscsi_session *session = conn->session;
	struct iscsi_scsi_cmd_hdr *req_hdr = cmnd_hdr(req);
	struct scst_cmd *scst_cmd = req->scst_cmd;
	scst_data_direction dir;
	bool unsolicited_data_expected = false;
	int res = 0;

	TRACE_ENTRY();

	TRACE_DBG("scsi command: %x", req_hdr->scb[0]);

	EXTRACHECKS_BUG_ON(req->scst_state != ISCSI_CMD_STATE_AFTER_PREPROC);

	dir = scst_cmd_get_data_direction(scst_cmd);

	/*
	 * Check for preliminary completion here to save R2Ts. For TASK QUEUE
	 * FULL statuses that might be a big performance win.
	 */
	if (unlikely(scst_cmd_prelim_completed(scst_cmd) ||
	    unlikely(req->prelim_compl_flags != 0))) {
		/*
		 * If necessary, ISCSI_CMD_ABORTED will be set by
		 * iscsi_xmit_response().
		 */
		res = iscsi_preliminary_complete(req, req, true);
		goto trace;
	}

	/* For prelim completed commands sg & K can be already set! */

	if (dir != SCST_DATA_BIDI) {
		req->sg = scst_cmd_get_sg(scst_cmd);
		req->sg_cnt = scst_cmd_get_sg_cnt(scst_cmd);
		req->bufflen = scst_cmd_get_bufflen(scst_cmd);
	} else {
		req->sg = scst_cmd_get_in_sg(scst_cmd);
		req->sg_cnt = scst_cmd_get_in_sg_cnt(scst_cmd);
		req->bufflen = scst_cmd_get_in_bufflen(scst_cmd);
	}

	if (dir & SCST_DATA_WRITE) {
		unsolicited_data_expected = !(req_hdr->flags & ISCSI_CMD_FINAL);

		if (unlikely(session->sess_params.initial_r2t &&
		    unsolicited_data_expected)) {
			PRINT_ERROR("Initiator %s violated negotiated "
				"parameters: initial R2T is required (ITT %x, "
				"op  %x)", session->initiator_name,
				cmnd_itt(req), req_hdr->scb[0]);
			goto out_close;
		}

		if (unlikely(!session->sess_params.immediate_data &&
		    req->pdu.datasize)) {
			PRINT_ERROR("Initiator %s violated negotiated "
				"parameters: forbidden immediate data sent "
				"(ITT %x, op  %x)", session->initiator_name,
				cmnd_itt(req), req_hdr->scb[0]);
			goto out_close;
		}

		if (unlikely(session->sess_params.first_burst_length < req->pdu.datasize)) {
			PRINT_ERROR("Initiator %s violated negotiated "
				"parameters: immediate data len (%d) > "
				"first_burst_length (%d) (ITT %x, op  %x)",
				session->initiator_name,
				req->pdu.datasize,
				session->sess_params.first_burst_length,
				cmnd_itt(req), req_hdr->scb[0]);
			goto out_close;
		}

		req->r2t_len_to_receive = be32_to_cpu(req_hdr->data_length) -
					  req->pdu.datasize;

		/*
		 * In case of residual overflow req->r2t_len_to_receive and
		 * req->pdu.datasize might be > req->bufflen
		 */

		res = cmnd_insert_data_wait_hash(req);
		if (unlikely(res != 0)) {
			/*
			 * We have to close connection, because otherwise a data
			 * corruption is possible if we allow to receive data
			 * for this request in another request with dublicated
			 * ITT.
			 */
			goto out_close;
		}

		if (unsolicited_data_expected) {
			req->outstanding_r2t = 1;
			req->r2t_len_to_send = req->r2t_len_to_receive -
				min_t(unsigned int,
				      session->sess_params.first_burst_length -
						req->pdu.datasize,
				      req->r2t_len_to_receive);
		} else
			req->r2t_len_to_send = req->r2t_len_to_receive;

		req_add_to_write_timeout_list(req);

		if (req->pdu.datasize) {
			res = cmnd_prepare_recv_pdu(conn, req, 0, req->pdu.datasize);
			/* For performance better to send R2Ts ASAP */
			if (likely(res == 0) && (req->r2t_len_to_send != 0))
				send_r2t(req);
		}
	} else {
		if (unlikely(!(req_hdr->flags & ISCSI_CMD_FINAL) ||
			     req->pdu.datasize)) {
			PRINT_ERROR("Unexpected unsolicited data (ITT %x "
				"CDB %x", cmnd_itt(req), req_hdr->scb[0]);
			set_scst_preliminary_status_rsp(req, true,
				SCST_LOAD_SENSE(iscsi_sense_unexpected_unsolicited_data));
		}
	}

trace:
	TRACE_DBG("req=%p, dir=%d, unsolicited_data_expected=%d, "
		"r2t_len_to_receive=%d, r2t_len_to_send=%d, bufflen=%d, "
		"own_sg %d", req, dir, unsolicited_data_expected,
		req->r2t_len_to_receive, req->r2t_len_to_send, req->bufflen,
		req->own_sg);

out:
	TRACE_EXIT_RES(res);
	return res;

out_close:
	mark_conn_closed(conn);
	res = -EINVAL;
	goto out;
}

static int scsi_cmnd_start(struct iscsi_cmnd *req)
{
	struct iscsi_conn *conn = req->conn;
	struct iscsi_session *session = conn->session;
	struct iscsi_scsi_cmd_hdr *req_hdr = cmnd_hdr(req);
	struct scst_cmd *scst_cmd;
	scst_data_direction dir;
	struct iscsi_ahs_hdr *ahdr;
	int res = 0;

	TRACE_ENTRY();

	TRACE_DBG("scsi command: %x", req_hdr->scb[0]);

	TRACE_DBG("Incrementing active_cmds (cmd %p, sess %p, "
		"new value %d)", req, session,
		atomic_read(&session->active_cmds)+1);
	atomic_inc(&session->active_cmds);
	req->dec_active_cmnds = 1;

	scst_cmd = scst_rx_cmd(session->scst_sess,
		(uint8_t *)&req_hdr->lun, sizeof(req_hdr->lun),
		req_hdr->scb, sizeof(req_hdr->scb), SCST_NON_ATOMIC);
	if (scst_cmd == NULL) {
		res = create_preliminary_status_rsp(req, SAM_STAT_BUSY,
			NULL, 0);
		goto out;
	}

	req->scst_cmd = scst_cmd;
	scst_cmd_set_tag(scst_cmd, req_hdr->itt);
	scst_cmd_set_tgt_priv(scst_cmd, req);

	if ((req_hdr->flags & ISCSI_CMD_READ) &&
	    (req_hdr->flags & ISCSI_CMD_WRITE)) {
		int sz = cmnd_read_size(req);
		if (unlikely(sz < 0)) {
			PRINT_ERROR("%s", "BIDI data transfer, but initiator "
				"not supplied Bidirectional Read Expected Data "
				"Transfer Length AHS");
			set_scst_preliminary_status_rsp(req, true,
			   SCST_LOAD_SENSE(scst_sense_parameter_value_invalid));
		} else {
			req->read_size = sz;
			dir = SCST_DATA_BIDI;
			scst_cmd_set_expected(scst_cmd, dir, sz);
			scst_cmd_set_expected_in_transfer_len(scst_cmd,
				be32_to_cpu(req_hdr->data_length));
#if !defined(CONFIG_TCP_ZERO_COPY_TRANSFER_COMPLETION_NOTIFICATION)
			scst_cmd_set_tgt_need_alloc_data_buf(scst_cmd);
#endif
		}
	} else if (req_hdr->flags & ISCSI_CMD_READ) {
		req->read_size = be32_to_cpu(req_hdr->data_length);
		dir = SCST_DATA_READ;
		scst_cmd_set_expected(scst_cmd, dir, req->read_size);
#if !defined(CONFIG_TCP_ZERO_COPY_TRANSFER_COMPLETION_NOTIFICATION)
		scst_cmd_set_tgt_need_alloc_data_buf(scst_cmd);
#endif
	} else if (req_hdr->flags & ISCSI_CMD_WRITE) {
		dir = SCST_DATA_WRITE;
		scst_cmd_set_expected(scst_cmd, dir,
			be32_to_cpu(req_hdr->data_length));
	} else {
		dir = SCST_DATA_NONE;
		scst_cmd_set_expected(scst_cmd, dir, 0);
	}

	switch (req_hdr->flags & ISCSI_CMD_ATTR_MASK) {
	case ISCSI_CMD_SIMPLE:
		scst_cmd_set_queue_type(scst_cmd, SCST_CMD_QUEUE_SIMPLE);
		break;
	case ISCSI_CMD_HEAD_OF_QUEUE:
		scst_cmd_set_queue_type(scst_cmd, SCST_CMD_QUEUE_HEAD_OF_QUEUE);
		break;
	case ISCSI_CMD_ORDERED:
		scst_cmd_set_queue_type(scst_cmd, SCST_CMD_QUEUE_ORDERED);
		break;
	case ISCSI_CMD_ACA:
		scst_cmd_set_queue_type(scst_cmd, SCST_CMD_QUEUE_ACA);
		break;
	case ISCSI_CMD_UNTAGGED:
		scst_cmd_set_queue_type(scst_cmd, SCST_CMD_QUEUE_UNTAGGED);
		break;
	default:
		PRINT_ERROR("Unknown task code %x, use ORDERED instead",
			req_hdr->flags & ISCSI_CMD_ATTR_MASK);
		scst_cmd_set_queue_type(scst_cmd, SCST_CMD_QUEUE_ORDERED);
		break;
	}

	/* cmd_sn is already in CPU format converted in check_cmd_sn() */
	scst_cmd_set_tgt_sn(scst_cmd, req_hdr->cmd_sn);

	ahdr = (struct iscsi_ahs_hdr *)req->pdu.ahs;
	if (ahdr != NULL) {
		uint8_t *p = (uint8_t *)ahdr;
		unsigned int size = 0;
		do {
			int s;

			ahdr = (struct iscsi_ahs_hdr *)p;

			if (ahdr->ahstype == ISCSI_AHSTYPE_CDB) {
				struct iscsi_cdb_ahdr *eca =
					(struct iscsi_cdb_ahdr *)ahdr;
				scst_cmd_set_ext_cdb(scst_cmd, eca->cdb,
					be16_to_cpu(ahdr->ahslength) - 1);
				break;
			}
			s = 3 + be16_to_cpu(ahdr->ahslength);
			s = (s + 3) & -4;
			size += s;
			p += s;
		} while (size < req->pdu.ahssize);
	}

	TRACE_DBG("START Command (itt %x, queue_type %d)",
		req_hdr->itt, scst_cmd_get_queue_type(scst_cmd));
	req->scst_state = ISCSI_CMD_STATE_RX_CMD;
	conn->rx_task = current;
	scst_cmd_init_stage1_done(scst_cmd, SCST_CONTEXT_DIRECT, 0);

	if (req->scst_state != ISCSI_CMD_STATE_RX_CMD)
		res = cmnd_rx_continue(req);
	else {
		TRACE_DBG("Delaying req %p post processing (scst_state %d)",
			req, req->scst_state);
		res = 1;
	}

out:
	TRACE_EXIT_RES(res);
	return res;
}

static int data_out_start(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;
	struct iscsi_data_out_hdr *req_hdr =
		(struct iscsi_data_out_hdr *)&cmnd->pdu.bhs;
	struct iscsi_cmnd *orig_req;
#if 0
	struct iscsi_hdr *orig_req_hdr;
#endif
	u32 offset = be32_to_cpu(req_hdr->buffer_offset);
	int res = 0;

	TRACE_ENTRY();

	/*
	 * There is no race with send_r2t() and conn_abort(), since
	 * all functions called from single read thread
	 */
	iscsi_extracheck_is_rd_thread(cmnd->conn);

	update_stat_sn(cmnd);

	orig_req = cmnd_find_data_wait_hash(conn, req_hdr->itt);
	cmnd->cmd_req = orig_req;
	if (unlikely(orig_req == NULL)) {
		/*
		 * It shouldn't happen, since we don't abort any request until
		 * we received all related PDUs from the initiator or timeout
		 * them. Let's quietly drop such PDUs.
		 */
		TRACE_MGMT_DBG("Unable to find scsi task ITT %x",
			cmnd_itt(cmnd));
		res = iscsi_preliminary_complete(cmnd, cmnd, true);
		goto out;
	}

	if (unlikely(orig_req->r2t_len_to_receive < cmnd->pdu.datasize)) {
		if (orig_req->prelim_compl_flags != 0) {
			/* We can have fake r2t_len_to_receive */
			goto go;
		}
		PRINT_ERROR("Data size (%d) > R2T length to receive (%d)",
			cmnd->pdu.datasize, orig_req->r2t_len_to_receive);
		set_scst_preliminary_status_rsp(orig_req, false,
			SCST_LOAD_SENSE(iscsi_sense_incorrect_amount_of_data));
		goto go;
	}

	/* Crazy iSCSI spec requires us to make this unneeded check */
#if 0 /* ...but some initiators (Windows) don't care to correctly set it */
	orig_req_hdr = &orig_req->pdu.bhs;
	if (unlikely(orig_req_hdr->lun != req_hdr->lun)) {
		PRINT_ERROR("Wrong LUN (%lld) in Data-Out PDU (expected %lld), "
			"orig_req %p, cmnd %p", (unsigned long long)req_hdr->lun,
			(unsigned long long)orig_req_hdr->lun, orig_req, cmnd);
		create_reject_rsp(orig_req, ISCSI_REASON_PROTOCOL_ERROR, false);
		goto go;
	}
#endif

go:
	if (req_hdr->flags & ISCSI_FLG_FINAL)
		orig_req->outstanding_r2t--;

	if (unlikely(orig_req->prelim_compl_flags != 0)) {
		res = iscsi_preliminary_complete(cmnd, orig_req, true);
		goto out;
	}

	TRACE_WRITE("cmnd %p, orig_req %p, offset %u, datasize %u", cmnd,
		orig_req, offset, cmnd->pdu.datasize);

	res = cmnd_prepare_recv_pdu(conn, orig_req, offset, cmnd->pdu.datasize);

out:
	TRACE_EXIT_RES(res);
	return res;
}

static void data_out_end(struct iscsi_cmnd *cmnd)
{
	struct iscsi_data_out_hdr *req_hdr =
		(struct iscsi_data_out_hdr *)&cmnd->pdu.bhs;
	struct iscsi_cmnd *req;

	TRACE_ENTRY();

	EXTRACHECKS_BUG_ON(cmnd == NULL);
	req = cmnd->cmd_req;
	if (unlikely(req == NULL))
		goto out;

	TRACE_DBG("cmnd %p, req %p", cmnd, req);

	iscsi_extracheck_is_rd_thread(cmnd->conn);

	if (!(cmnd->conn->ddigest_type & DIGEST_NONE) &&
	    !cmnd->ddigest_checked) {
		cmd_add_on_rx_ddigest_list(req, cmnd);
		cmnd_get(cmnd);
	}

	/*
	 * Now we received the data and can adjust r2t_len_to_receive of the
	 * orig req. We couldn't do it earlier, because it will break data
	 * receiving errors recovery (calls of iscsi_fail_data_waiting_cmnd()).
	 */
	req->r2t_len_to_receive -= cmnd->pdu.datasize;

	if (unlikely(req->prelim_compl_flags != 0)) {
		/*
		 * We might need to wait for one or more PDUs. Let's simplify
		 * other code.
		 */
		req->r2t_len_to_receive = req->outstanding_r2t;
		req->r2t_len_to_send = 0;
	}

	TRACE_DBG("req %p, FINAL %x, outstanding_r2t %d, r2t_len_to_receive %d,"
		" r2t_len_to_send %d", req, req_hdr->flags & ISCSI_FLG_FINAL,
		req->outstanding_r2t, req->r2t_len_to_receive,
		req->r2t_len_to_send);

	if (!(req_hdr->flags & ISCSI_FLG_FINAL))
		goto out;

	if (req->r2t_len_to_receive == 0) {
		if (!req->pending)
			iscsi_restart_cmnd(req);
	} else if (req->r2t_len_to_send != 0)
		send_r2t(req);

out:
	TRACE_EXIT();
	return;
}

/* Might be called under target_mutex and cmd_list_lock */
static void __cmnd_abort(struct iscsi_cmnd *cmnd)
{
	unsigned long timeout_time = jiffies + ISCSI_TM_DATA_WAIT_TIMEOUT +
					ISCSI_ADD_SCHED_TIME;
	struct iscsi_conn *conn = cmnd->conn;

	TRACE_MGMT_DBG("Aborting cmd %p, scst_cmd %p (scst state %x, "
		"ref_cnt %d, on_write_timeout_list %d, write_start %ld, ITT %x, "
		"sn %u, op %x, r2t_len_to_receive %d, r2t_len_to_send %d, "
		"CDB op %x, size to write %u, outstanding_r2t %d, "
		"sess->exp_cmd_sn %u, conn %p, rd_task %p)",
		cmnd, cmnd->scst_cmd, cmnd->scst_state,
		atomic_read(&cmnd->ref_cnt), cmnd->on_write_timeout_list,
		cmnd->write_start, cmnd_itt(cmnd), cmnd->pdu.bhs.sn,
		cmnd_opcode(cmnd), cmnd->r2t_len_to_receive,
		cmnd->r2t_len_to_send, cmnd_scsicode(cmnd),
		cmnd_write_size(cmnd), cmnd->outstanding_r2t,
		cmnd->conn->session->exp_cmd_sn, cmnd->conn,
		cmnd->conn->rd_task);

#if defined(CONFIG_TCP_ZERO_COPY_TRANSFER_COMPLETION_NOTIFICATION)
	TRACE_MGMT_DBG("net_ref_cnt %d", atomic_read(&cmnd->net_ref_cnt));
#endif

	/*
	 * Lock to sync with iscsi_check_tm_data_wait_timeouts(), including
	 * CMD_ABORTED bit set.
	 */
	spin_lock_bh(&iscsi_rd_lock);

	/*
	 * We suppose that preliminary commands completion is tested by
	 * comparing prelim_compl_flags with 0. Otherwise a race is possible,
	 * like sending command in SCST core as PRELIM_COMPLETED, while it
	 * wasn't aborted in it yet and have as the result a wrong success
	 * status sent to the initiator.
	 */
	set_bit(ISCSI_CMD_ABORTED, &cmnd->prelim_compl_flags);

	TRACE_MGMT_DBG("Setting conn_tm_active for conn %p", conn);
	conn->conn_tm_active = 1;

	spin_unlock_bh(&iscsi_rd_lock);

	/*
	 * We need the lock to sync with req_add_to_write_timeout_list() and
	 * close races for rsp_timer.expires.
	 */
	spin_lock_bh(&conn->write_list_lock);
	if (!timer_pending(&conn->rsp_timer) ||
	    time_after(conn->rsp_timer.expires, timeout_time)) {
		TRACE_MGMT_DBG("Mod timer on %ld (conn %p)", timeout_time,
			conn);
		mod_timer(&conn->rsp_timer, timeout_time);
	} else
		TRACE_MGMT_DBG("Timer for conn %p is going to fire on %ld "
			"(timeout time %ld)", conn, conn->rsp_timer.expires,
			timeout_time);
	spin_unlock_bh(&conn->write_list_lock);

	return;
}

/* Must be called from the read or conn close thread */
static int cmnd_abort(struct iscsi_cmnd *req, int *status)
{
	struct iscsi_task_mgt_hdr *req_hdr =
		(struct iscsi_task_mgt_hdr *)&req->pdu.bhs;
	struct iscsi_cmnd *cmnd;
	int res = -1;

	req_hdr->ref_cmd_sn = be32_to_cpu(req_hdr->ref_cmd_sn);

	if (!before(req_hdr->ref_cmd_sn, req_hdr->cmd_sn)) {
		TRACE(TRACE_MGMT, "ABORT TASK: RefCmdSN(%u) > CmdSN(%u)",
			req_hdr->ref_cmd_sn, req_hdr->cmd_sn);
		*status = ISCSI_RESPONSE_UNKNOWN_TASK;
		goto out;
	}

	cmnd = cmnd_find_itt_get(req->conn, req_hdr->rtt);
	if (cmnd) {
		struct iscsi_conn *conn = cmnd->conn;
		struct iscsi_scsi_cmd_hdr *hdr = cmnd_hdr(cmnd);

		if (req_hdr->lun != hdr->lun) {
			PRINT_ERROR("ABORT TASK: LUN mismatch: req LUN "
				    "%llx, cmd LUN %llx, rtt %u",
				    (long long unsigned int)req_hdr->lun,
				    (long long unsigned int)hdr->lun,
				    req_hdr->rtt);
			*status = ISCSI_RESPONSE_FUNCTION_REJECTED;
			goto out_put;
		}

		if (cmnd->pdu.bhs.opcode & ISCSI_OP_IMMEDIATE) {
			if (req_hdr->ref_cmd_sn != req_hdr->cmd_sn) {
				PRINT_ERROR("ABORT TASK: RefCmdSN(%u) != TM "
					"cmd CmdSN(%u) for immediate command "
					"%p", req_hdr->ref_cmd_sn,
					req_hdr->cmd_sn, cmnd);
				*status = ISCSI_RESPONSE_FUNCTION_REJECTED;
				goto out_put;
			}
		} else {
			if (req_hdr->ref_cmd_sn != hdr->cmd_sn) {
				PRINT_ERROR("ABORT TASK: RefCmdSN(%u) != "
					"CmdSN(%u) for command %p",
					req_hdr->ref_cmd_sn, req_hdr->cmd_sn,
					cmnd);
				*status = ISCSI_RESPONSE_FUNCTION_REJECTED;
				goto out_put;
			}
		}

		if (before(req_hdr->cmd_sn, hdr->cmd_sn) ||
		    (req_hdr->cmd_sn == hdr->cmd_sn)) {
			PRINT_ERROR("ABORT TASK: SN mismatch: req SN %x, "
				"cmd SN %x, rtt %u", req_hdr->cmd_sn,
				hdr->cmd_sn, req_hdr->rtt);
			*status = ISCSI_RESPONSE_FUNCTION_REJECTED;
			goto out_put;
		}

		spin_lock_bh(&conn->cmd_list_lock);
		__cmnd_abort(cmnd);
		spin_unlock_bh(&conn->cmd_list_lock);

		cmnd_put(cmnd);
		res = 0;
	} else {
		TRACE_MGMT_DBG("cmd RTT %x not found", req_hdr->rtt);
		/*
		 * iSCSI RFC:
		 *
		 * b)  If the Referenced Task Tag does not identify an existing task,
		 * but if the CmdSN indicated by the RefCmdSN field in the Task
		 * Management function request is within the valid CmdSN window
		 * and less than the CmdSN of the Task Management function
		 * request itself, then targets must consider the CmdSN received
		 * and return the "Function complete" response.
		 *
		 * c)  If the Referenced Task Tag does not identify an existing task
		 * and if the CmdSN indicated by the RefCmdSN field in the Task
		 * Management function request is outside the valid CmdSN window,
		 * then targets must return the "Task does not exist" response.
		 *
		 * 128 seems to be a good "window".
		 */
		if (between(req_hdr->ref_cmd_sn, req_hdr->cmd_sn - 128,
			    req_hdr->cmd_sn)) {
			*status = ISCSI_RESPONSE_FUNCTION_COMPLETE;
			res = 0;
		} else
			*status = ISCSI_RESPONSE_UNKNOWN_TASK;
	}

out:
	return res;

out_put:
	cmnd_put(cmnd);
	goto out;
}

/* Must be called from the read or conn close thread */
static int target_abort(struct iscsi_cmnd *req, int all)
{
	struct iscsi_target *target = req->conn->session->target;
	struct iscsi_task_mgt_hdr *req_hdr =
		(struct iscsi_task_mgt_hdr *)&req->pdu.bhs;
	struct iscsi_session *session;
	struct iscsi_conn *conn;
	struct iscsi_cmnd *cmnd;

	mutex_lock(&target->target_mutex);

	list_for_each_entry(session, &target->session_list,
			    session_list_entry) {
		list_for_each_entry(conn, &session->conn_list,
				    conn_list_entry) {
			spin_lock_bh(&conn->cmd_list_lock);
			list_for_each_entry(cmnd, &conn->cmd_list,
					    cmd_list_entry) {
				if (cmnd == req)
					continue;
				if (all)
					__cmnd_abort(cmnd);
				else if (req_hdr->lun == cmnd_hdr(cmnd)->lun)
					__cmnd_abort(cmnd);
			}
			spin_unlock_bh(&conn->cmd_list_lock);
		}
	}

	mutex_unlock(&target->target_mutex);
	return 0;
}

/* Must be called from the read or conn close thread */
static void task_set_abort(struct iscsi_cmnd *req)
{
	struct iscsi_session *session = req->conn->session;
	struct iscsi_task_mgt_hdr *req_hdr =
		(struct iscsi_task_mgt_hdr *)&req->pdu.bhs;
	struct iscsi_target *target = session->target;
	struct iscsi_conn *conn;
	struct iscsi_cmnd *cmnd;

	mutex_lock(&target->target_mutex);

	list_for_each_entry(conn, &session->conn_list, conn_list_entry) {
		spin_lock_bh(&conn->cmd_list_lock);
		list_for_each_entry(cmnd, &conn->cmd_list, cmd_list_entry) {
			struct iscsi_scsi_cmd_hdr *hdr = cmnd_hdr(cmnd);
			if (cmnd == req)
				continue;
			if (req_hdr->lun != hdr->lun)
				continue;
			if (before(req_hdr->cmd_sn, hdr->cmd_sn) ||
			    req_hdr->cmd_sn == hdr->cmd_sn)
				continue;
			__cmnd_abort(cmnd);
		}
		spin_unlock_bh(&conn->cmd_list_lock);
	}

	mutex_unlock(&target->target_mutex);
	return;
}

/* Must be called from the read or conn close thread */
void conn_abort(struct iscsi_conn *conn)
{
	struct iscsi_cmnd *cmnd, *r, *t;

	TRACE_MGMT_DBG("Aborting conn %p", conn);

	iscsi_extracheck_is_rd_thread(conn);

	cancel_delayed_work_sync(&conn->nop_in_delayed_work);

	/* No locks, we are the only user */
	list_for_each_entry_safe(r, t, &conn->nop_req_list,
			nop_req_list_entry) {
		list_del(&r->nop_req_list_entry);
		cmnd_put(r);
	}

	spin_lock_bh(&conn->cmd_list_lock);
again:
	list_for_each_entry(cmnd, &conn->cmd_list, cmd_list_entry) {
		__cmnd_abort(cmnd);
		if (cmnd->r2t_len_to_receive != 0) {
			if (!cmnd_get_check(cmnd)) {
				spin_unlock_bh(&conn->cmd_list_lock);

				/* ToDo: this is racy for MC/S */
				iscsi_fail_data_waiting_cmnd(cmnd);

				cmnd_put(cmnd);

				/*
				 * We are in the read thread, so we may not
				 * worry that after cmnd release conn gets
				 * released as well.
				 */
				spin_lock_bh(&conn->cmd_list_lock);
				goto again;
			}
		}
	}
	spin_unlock_bh(&conn->cmd_list_lock);

	return;
}

static void execute_task_management(struct iscsi_cmnd *req)
{
	struct iscsi_conn *conn = req->conn;
	struct iscsi_session *sess = conn->session;
	struct iscsi_task_mgt_hdr *req_hdr =
		(struct iscsi_task_mgt_hdr *)&req->pdu.bhs;
	int rc, status = ISCSI_RESPONSE_FUNCTION_REJECTED;
	int function = req_hdr->function & ISCSI_FUNCTION_MASK;
	struct scst_rx_mgmt_params params;

	TRACE(TRACE_MGMT, "iSCSI TM fn %d", function);

	TRACE_MGMT_DBG("TM req %p, ITT %x, RTT %x, sn %u, con %p", req,
		cmnd_itt(req), req_hdr->rtt, req_hdr->cmd_sn, conn);

	iscsi_extracheck_is_rd_thread(conn);

	spin_lock(&sess->sn_lock);
	sess->tm_active++;
	sess->tm_sn = req_hdr->cmd_sn;
	if (sess->tm_rsp != NULL) {
		struct iscsi_cmnd *tm_rsp = sess->tm_rsp;

		TRACE_MGMT_DBG("Dropping delayed TM rsp %p", tm_rsp);

		sess->tm_rsp = NULL;
		sess->tm_active--;

		spin_unlock(&sess->sn_lock);

		sBUG_ON(sess->tm_active < 0);

		rsp_cmnd_release(tm_rsp);
	} else
		spin_unlock(&sess->sn_lock);

	memset(&params, 0, sizeof(params));
	params.atomic = SCST_NON_ATOMIC;
	params.tgt_priv = req;

	if ((function != ISCSI_FUNCTION_ABORT_TASK) &&
	    (req_hdr->rtt != ISCSI_RESERVED_TAG)) {
		PRINT_ERROR("Invalid RTT %x (TM fn %d)", req_hdr->rtt,
			function);
		rc = -1;
		status = ISCSI_RESPONSE_FUNCTION_REJECTED;
		goto reject;
	}

	/* cmd_sn is already in CPU format converted in check_cmd_sn() */

	switch (function) {
	case ISCSI_FUNCTION_ABORT_TASK:
		rc = cmnd_abort(req, &status);
		if (rc == 0) {
			params.fn = SCST_ABORT_TASK;
			params.tag = req_hdr->rtt;
			params.tag_set = 1;
			params.lun = (uint8_t *)&req_hdr->lun;
			params.lun_len = sizeof(req_hdr->lun);
			params.lun_set = 1;
			params.cmd_sn = req_hdr->cmd_sn;
			params.cmd_sn_set = 1;
			rc = scst_rx_mgmt_fn(conn->session->scst_sess,
				&params);
			status = ISCSI_RESPONSE_FUNCTION_REJECTED;
		}
		break;
	case ISCSI_FUNCTION_ABORT_TASK_SET:
		task_set_abort(req);
		params.fn = SCST_ABORT_TASK_SET;
		params.lun = (uint8_t *)&req_hdr->lun;
		params.lun_len = sizeof(req_hdr->lun);
		params.lun_set = 1;
		params.cmd_sn = req_hdr->cmd_sn;
		params.cmd_sn_set = 1;
		rc = scst_rx_mgmt_fn(conn->session->scst_sess,
			&params);
		status = ISCSI_RESPONSE_FUNCTION_REJECTED;
		break;
	case ISCSI_FUNCTION_CLEAR_TASK_SET:
		task_set_abort(req);
		params.fn = SCST_CLEAR_TASK_SET;
		params.lun = (uint8_t *)&req_hdr->lun;
		params.lun_len = sizeof(req_hdr->lun);
		params.lun_set = 1;
		params.cmd_sn = req_hdr->cmd_sn;
		params.cmd_sn_set = 1;
		rc = scst_rx_mgmt_fn(conn->session->scst_sess,
			&params);
		status = ISCSI_RESPONSE_FUNCTION_REJECTED;
		break;
	case ISCSI_FUNCTION_CLEAR_ACA:
		params.fn = SCST_CLEAR_ACA;
		params.lun = (uint8_t *)&req_hdr->lun;
		params.lun_len = sizeof(req_hdr->lun);
		params.lun_set = 1;
		params.cmd_sn = req_hdr->cmd_sn;
		params.cmd_sn_set = 1;
		rc = scst_rx_mgmt_fn(conn->session->scst_sess,
			&params);
		status = ISCSI_RESPONSE_FUNCTION_REJECTED;
		break;
	case ISCSI_FUNCTION_TARGET_COLD_RESET:
	case ISCSI_FUNCTION_TARGET_WARM_RESET:
		target_abort(req, 1);
		params.fn = SCST_TARGET_RESET;
		params.cmd_sn = req_hdr->cmd_sn;
		params.cmd_sn_set = 1;
		rc = scst_rx_mgmt_fn(conn->session->scst_sess,
			&params);
		status = ISCSI_RESPONSE_FUNCTION_REJECTED;
		break;
	case ISCSI_FUNCTION_LOGICAL_UNIT_RESET:
		target_abort(req, 0);
		params.fn = SCST_LUN_RESET;
		params.lun = (uint8_t *)&req_hdr->lun;
		params.lun_len = sizeof(req_hdr->lun);
		params.lun_set = 1;
		params.cmd_sn = req_hdr->cmd_sn;
		params.cmd_sn_set = 1;
		rc = scst_rx_mgmt_fn(conn->session->scst_sess,
			&params);
		status = ISCSI_RESPONSE_FUNCTION_REJECTED;
		break;
	case ISCSI_FUNCTION_TASK_REASSIGN:
		rc = -1;
		status = ISCSI_RESPONSE_ALLEGIANCE_REASSIGNMENT_UNSUPPORTED;
		break;
	default:
		PRINT_ERROR("Unknown TM function %d", function);
		rc = -1;
		status = ISCSI_RESPONSE_FUNCTION_REJECTED;
		break;
	}

reject:
	if (rc != 0)
		iscsi_send_task_mgmt_resp(req, status);

	return;
}

static void nop_out_exec(struct iscsi_cmnd *req)
{
	struct iscsi_cmnd *rsp;
	struct iscsi_nop_in_hdr *rsp_hdr;

	TRACE_ENTRY();

	TRACE_DBG("%p", req);

	if (cmnd_itt(req) != cpu_to_be32(ISCSI_RESERVED_TAG)) {
		rsp = iscsi_alloc_main_rsp(req);

		rsp_hdr = (struct iscsi_nop_in_hdr *)&rsp->pdu.bhs;
		rsp_hdr->opcode = ISCSI_OP_NOP_IN;
		rsp_hdr->flags = ISCSI_FLG_FINAL;
		rsp_hdr->itt = req->pdu.bhs.itt;
		rsp_hdr->ttt = cpu_to_be32(ISCSI_RESERVED_TAG);

		if (req->pdu.datasize)
			sBUG_ON(req->sg == NULL);
		else
			sBUG_ON(req->sg != NULL);

		if (req->sg) {
			rsp->sg = req->sg;
			rsp->sg_cnt = req->sg_cnt;
			rsp->bufflen = req->bufflen;
		}

		/* We already checked it in check_segment_length() */
		sBUG_ON(get_pgcnt(req->pdu.datasize, 0) > ISCSI_CONN_IOV_MAX);

		rsp->pdu.datasize = req->pdu.datasize;
	} else {
		bool found = false;
		struct iscsi_cmnd *r;
		struct iscsi_conn *conn = req->conn;

		TRACE_DBG("Receive Nop-In response (ttt 0x%08x)",
			be32_to_cpu(cmnd_ttt(req)));

		spin_lock_bh(&conn->nop_req_list_lock);
		list_for_each_entry(r, &conn->nop_req_list,
				nop_req_list_entry) {
			if (cmnd_ttt(req) == cmnd_ttt(r)) {
				list_del(&r->nop_req_list_entry);
				found = true;
				break;
			}
		}
		spin_unlock_bh(&conn->nop_req_list_lock);

		if (found)
			cmnd_put(r);
		else
			TRACE_MGMT_DBG("%s", "Got Nop-out response without "
				"corresponding Nop-In request");
	}

	req_cmnd_release(req);

	TRACE_EXIT();
	return;
}

static void logout_exec(struct iscsi_cmnd *req)
{
	struct iscsi_logout_req_hdr *req_hdr;
	struct iscsi_cmnd *rsp;
	struct iscsi_logout_rsp_hdr *rsp_hdr;

	PRINT_INFO("Logout received from initiator %s",
		req->conn->session->initiator_name);
	TRACE_DBG("%p", req);

	req_hdr = (struct iscsi_logout_req_hdr *)&req->pdu.bhs;
	rsp = iscsi_alloc_main_rsp(req);
	rsp_hdr = (struct iscsi_logout_rsp_hdr *)&rsp->pdu.bhs;
	rsp_hdr->opcode = ISCSI_OP_LOGOUT_RSP;
	rsp_hdr->flags = ISCSI_FLG_FINAL;
	rsp_hdr->itt = req_hdr->itt;
	rsp->should_close_conn = 1;

	req_cmnd_release(req);

	return;
}

static void iscsi_cmnd_exec(struct iscsi_cmnd *cmnd)
{
	TRACE_ENTRY();

	TRACE_DBG("cmnd %p, op %x, SN %u", cmnd, cmnd_opcode(cmnd),
		cmnd->pdu.bhs.sn);

	iscsi_extracheck_is_rd_thread(cmnd->conn);

	if (cmnd_opcode(cmnd) == ISCSI_OP_SCSI_CMD) {
		if (cmnd->r2t_len_to_receive == 0)
			iscsi_restart_cmnd(cmnd);
		else if (cmnd->r2t_len_to_send != 0)
			send_r2t(cmnd);
		goto out;
	}

	if (cmnd->prelim_compl_flags != 0) {
		TRACE_MGMT_DBG("Terminating prelim completed non-SCSI cmnd %p "
			"(op %x)", cmnd, cmnd_opcode(cmnd));
		req_cmnd_release(cmnd);
		goto out;
	}

	switch (cmnd_opcode(cmnd)) {
	case ISCSI_OP_NOP_OUT:
		nop_out_exec(cmnd);
		break;
	case ISCSI_OP_SCSI_TASK_MGT_MSG:
		execute_task_management(cmnd);
		break;
	case ISCSI_OP_LOGOUT_CMD:
		logout_exec(cmnd);
		break;
	default:
		PRINT_CRIT_ERROR("Unexpected cmnd op %x", cmnd_opcode(cmnd));
		sBUG();
		break;
	}

out:
	TRACE_EXIT();
	return;
}

static void set_cork(struct socket *sock, int on)
{
	int opt = on;
	mm_segment_t oldfs;

	oldfs = get_fs();
	set_fs(get_ds());
	sock->ops->setsockopt(sock, SOL_TCP, TCP_CORK,
			      (void __force __user *)&opt, sizeof(opt));
	set_fs(oldfs);
	return;
}

void cmnd_tx_start(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;

	TRACE_DBG("conn %p, cmnd %p, opcode %x", conn, cmnd, cmnd_opcode(cmnd));
	iscsi_cmnd_set_length(&cmnd->pdu);

	iscsi_extracheck_is_wr_thread(conn);

	set_cork(conn->sock, 1);

	conn->write_iop = conn->write_iov;
	conn->write_iop->iov_base = (void __force __user *)(&cmnd->pdu.bhs);
	conn->write_iop->iov_len = sizeof(cmnd->pdu.bhs);
	conn->write_iop_used = 1;
	conn->write_size = sizeof(cmnd->pdu.bhs) + cmnd->pdu.datasize;
	conn->write_offset = 0;

	switch (cmnd_opcode(cmnd)) {
	case ISCSI_OP_NOP_IN:
		if (cmnd_itt(cmnd) == cpu_to_be32(ISCSI_RESERVED_TAG))
			cmnd->pdu.bhs.sn = cmnd_set_sn(cmnd, 0);
		else
			cmnd_set_sn(cmnd, 1);
		break;
	case ISCSI_OP_SCSI_RSP:
		cmnd_set_sn(cmnd, 1);
		break;
	case ISCSI_OP_SCSI_TASK_MGT_RSP:
		cmnd_set_sn(cmnd, 1);
		break;
	case ISCSI_OP_TEXT_RSP:
		cmnd_set_sn(cmnd, 1);
		break;
	case ISCSI_OP_SCSI_DATA_IN:
	{
		struct iscsi_data_in_hdr *rsp =
			(struct iscsi_data_in_hdr *)&cmnd->pdu.bhs;
		u32 offset = cpu_to_be32(rsp->buffer_offset);

		TRACE_DBG("cmnd %p, offset %u, datasize %u, bufflen %u", cmnd,
			offset, cmnd->pdu.datasize, cmnd->bufflen);

		sBUG_ON(offset > cmnd->bufflen);
		sBUG_ON(offset + cmnd->pdu.datasize > cmnd->bufflen);

		conn->write_offset = offset;

		cmnd_set_sn(cmnd, (rsp->flags & ISCSI_FLG_FINAL) ? 1 : 0);
		break;
	}
	case ISCSI_OP_LOGOUT_RSP:
		cmnd_set_sn(cmnd, 1);
		break;
	case ISCSI_OP_R2T:
		cmnd->pdu.bhs.sn = cmnd_set_sn(cmnd, 0);
		break;
	case ISCSI_OP_ASYNC_MSG:
		cmnd_set_sn(cmnd, 1);
		break;
	case ISCSI_OP_REJECT:
		cmnd_set_sn(cmnd, 1);
		break;
	default:
		PRINT_ERROR("Unexpected cmnd op %x", cmnd_opcode(cmnd));
		break;
	}

	iscsi_dump_pdu(&cmnd->pdu);
	return;
}

void cmnd_tx_end(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;

	TRACE_DBG("%p:%x (should_close_conn %d, should_close_all_conn %d)",
		cmnd, cmnd_opcode(cmnd), cmnd->should_close_conn,
		cmnd->should_close_all_conn);

#ifdef CONFIG_SCST_EXTRACHECKS
	switch (cmnd_opcode(cmnd)) {
	case ISCSI_OP_NOP_IN:
	case ISCSI_OP_SCSI_RSP:
	case ISCSI_OP_SCSI_TASK_MGT_RSP:
	case ISCSI_OP_TEXT_RSP:
	case ISCSI_OP_R2T:
	case ISCSI_OP_ASYNC_MSG:
	case ISCSI_OP_REJECT:
	case ISCSI_OP_SCSI_DATA_IN:
	case ISCSI_OP_LOGOUT_RSP:
		break;
	default:
		PRINT_CRIT_ERROR("unexpected cmnd op %x", cmnd_opcode(cmnd));
		sBUG();
		break;
	}
#endif

	if (unlikely(cmnd->should_close_conn)) {
		if (cmnd->should_close_all_conn) {
			PRINT_INFO("Closing all connections for target %x at "
				"initiator's %s request",
				cmnd->conn->session->target->tid,
				conn->session->initiator_name);
			target_del_all_sess(cmnd->conn->session->target, 0);
		} else {
			PRINT_INFO("Closing connection at initiator's %s "
				"request", conn->session->initiator_name);
			mark_conn_closed(conn);
		}
	}

	set_cork(cmnd->conn->sock, 0);
	return;
}

/*
 * Push the command for execution. This functions reorders the commands.
 * Called from the read thread.
 *
 * Basically, since we don't support MC/S and TCP guarantees data delivery
 * order, all that SN's stuff isn't needed at all (commands delivery order is
 * a natural commands execution order), but insane iSCSI spec requires
 * us to check it and we have to, because some crazy initiators can rely
 * on the SN's based order and reorder requests during sending. For all other
 * normal initiators all that code is a NOP.
 */
static void iscsi_push_cmnd(struct iscsi_cmnd *cmnd)
{
	struct iscsi_session *session = cmnd->conn->session;
	struct list_head *entry;
	u32 cmd_sn;

	TRACE_DBG("cmnd %p, iSCSI opcode %x, sn %u, exp sn %u", cmnd,
		cmnd_opcode(cmnd), cmnd->pdu.bhs.sn, session->exp_cmd_sn);

	iscsi_extracheck_is_rd_thread(cmnd->conn);

	sBUG_ON(cmnd->parent_req != NULL);

	if (cmnd->pdu.bhs.opcode & ISCSI_OP_IMMEDIATE) {
		TRACE_DBG("Immediate cmd %p (cmd_sn %u)", cmnd,
			cmnd->pdu.bhs.sn);
		iscsi_cmnd_exec(cmnd);
		goto out;
	}

	spin_lock(&session->sn_lock);

	cmd_sn = cmnd->pdu.bhs.sn;
	if (cmd_sn == session->exp_cmd_sn) {
		while (1) {
			session->exp_cmd_sn = ++cmd_sn;

			if (unlikely(session->tm_active > 0)) {
				if (before(cmd_sn, session->tm_sn)) {
					struct iscsi_conn *conn = cmnd->conn;

					spin_unlock(&session->sn_lock);

					spin_lock_bh(&conn->cmd_list_lock);
					__cmnd_abort(cmnd);
					spin_unlock_bh(&conn->cmd_list_lock);

					spin_lock(&session->sn_lock);
				}
				iscsi_check_send_delayed_tm_resp(session);
			}

			spin_unlock(&session->sn_lock);

			iscsi_cmnd_exec(cmnd);

			spin_lock(&session->sn_lock);

			if (list_empty(&session->pending_list))
				break;
			cmnd = list_entry(session->pending_list.next,
					  struct iscsi_cmnd,
					  pending_list_entry);
			if (cmnd->pdu.bhs.sn != cmd_sn)
				break;

			list_del(&cmnd->pending_list_entry);
			cmnd->pending = 0;

			TRACE_MGMT_DBG("Processing pending cmd %p (cmd_sn %u)",
				cmnd, cmd_sn);
		}
	} else {
		int drop = 0;

		TRACE_DBG("Pending cmd %p (cmd_sn %u, exp_cmd_sn %u)",
			cmnd, cmd_sn, session->exp_cmd_sn);

		/*
		 * iSCSI RFC 3720: "The target MUST silently ignore any
		 * non-immediate command outside of [from ExpCmdSN to MaxCmdSN
		 * inclusive] range". But we won't honor the MaxCmdSN
		 * requirement, because, since we adjust MaxCmdSN from the
		 * separate write thread, rarely it is possible that initiator
		 * can legally send command with CmdSN>MaxSN. But it won't
		 * hurt anything, in the worst case it will lead to
		 * additional QUEUE FULL status.
		 */

		if (unlikely(before(cmd_sn, session->exp_cmd_sn))) {
			PRINT_ERROR("Unexpected cmd_sn (%u,%u)", cmd_sn,
				session->exp_cmd_sn);
			drop = 1;
		}

#if 0
		if (unlikely(after(cmd_sn, session->exp_cmd_sn +
					iscsi_get_allowed_cmds(session)))) {
			TRACE_MGMT_DBG("Too large cmd_sn %u (exp_cmd_sn %u, "
				"max_sn %u)", cmd_sn, session->exp_cmd_sn,
				iscsi_get_allowed_cmds(session));
		}
#endif

		spin_unlock(&session->sn_lock);

		if (unlikely(drop)) {
			req_cmnd_release_force(cmnd);
			goto out;
		}

		if (unlikely(test_bit(ISCSI_CMD_ABORTED,
					&cmnd->prelim_compl_flags))) {
			struct iscsi_cmnd *tm_clone;

			TRACE_MGMT_DBG("Pending aborted cmnd %p, creating TM "
				"clone (scst cmd %p, state %d)", cmnd,
				cmnd->scst_cmd, cmnd->scst_state);

			tm_clone = cmnd_alloc(cmnd->conn, NULL);
			if (tm_clone != NULL) {
				set_bit(ISCSI_CMD_ABORTED,
					&tm_clone->prelim_compl_flags);
				tm_clone->pdu = cmnd->pdu;

				TRACE_MGMT_DBG("TM clone %p created",
					       tm_clone);

				iscsi_cmnd_exec(cmnd);
				cmnd = tm_clone;
			} else
				PRINT_ERROR("%s", "Unable to create TM clone");
		}

		TRACE_MGMT_DBG("Pending cmnd %p (op %x, sn %u, exp sn %u)",
			cmnd, cmnd_opcode(cmnd), cmd_sn, session->exp_cmd_sn);

		spin_lock(&session->sn_lock);
		list_for_each(entry, &session->pending_list) {
			struct iscsi_cmnd *tmp =
				list_entry(entry, struct iscsi_cmnd,
					   pending_list_entry);
			if (before(cmd_sn, tmp->pdu.bhs.sn))
				break;
		}
		list_add_tail(&cmnd->pending_list_entry, entry);
		cmnd->pending = 1;
	}

	spin_unlock(&session->sn_lock);
out:
	return;
}

static int check_segment_length(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;
	struct iscsi_session *session = conn->session;

	if (unlikely(cmnd->pdu.datasize > session->sess_params.max_recv_data_length)) {
		PRINT_ERROR("Initiator %s violated negotiated parameters: "
			"data too long (ITT %x, datasize %u, "
			"max_recv_data_length %u", session->initiator_name,
			cmnd_itt(cmnd), cmnd->pdu.datasize,
			session->sess_params.max_recv_data_length);
		mark_conn_closed(conn);
		return -EINVAL;
	}
	return 0;
}

int cmnd_rx_start(struct iscsi_cmnd *cmnd)
{
	int res, rc;

	iscsi_dump_pdu(&cmnd->pdu);

	res = check_segment_length(cmnd);
	if (res != 0)
		goto out;

	switch (cmnd_opcode(cmnd)) {
	case ISCSI_OP_SCSI_CMD:
		res = scsi_cmnd_start(cmnd);
		if (unlikely(res < 0))
			goto out;
		spin_lock(&cmnd->conn->session->sn_lock);
		__update_stat_sn(cmnd);
		rc = check_cmd_sn(cmnd);
		spin_unlock(&cmnd->conn->session->sn_lock);
		break;
	case ISCSI_OP_SCSI_DATA_OUT:
		res = data_out_start(cmnd);
		goto out;
	case ISCSI_OP_NOP_OUT:
		rc = nop_out_start(cmnd);
		break;
	case ISCSI_OP_SCSI_TASK_MGT_MSG:
	case ISCSI_OP_LOGOUT_CMD:
		spin_lock(&cmnd->conn->session->sn_lock);
		__update_stat_sn(cmnd);
		rc = check_cmd_sn(cmnd);
		spin_unlock(&cmnd->conn->session->sn_lock);
		break;
	case ISCSI_OP_TEXT_CMD:
	case ISCSI_OP_SNACK_CMD:
	default:
		rc = -ISCSI_REASON_UNSUPPORTED_COMMAND;
		break;
	}

	if (unlikely(rc < 0)) {
		PRINT_ERROR("Error %d (iSCSI opcode %x, ITT %x)", rc,
			cmnd_opcode(cmnd), cmnd_itt(cmnd));
		res = create_reject_rsp(cmnd, -rc, true);
	}

out:
	TRACE_EXIT_RES(res);
	return res;
}

void cmnd_rx_end(struct iscsi_cmnd *cmnd)
{
	TRACE_ENTRY();

	TRACE_DBG("cmnd %p, opcode %x", cmnd, cmnd_opcode(cmnd));

	cmnd->conn->last_rcv_time = jiffies;
	TRACE_DBG("Updated last_rcv_time %ld", cmnd->conn->last_rcv_time);

	switch (cmnd_opcode(cmnd)) {
	case ISCSI_OP_SCSI_CMD:
	case ISCSI_OP_NOP_OUT:
	case ISCSI_OP_SCSI_TASK_MGT_MSG:
	case ISCSI_OP_LOGOUT_CMD:
		iscsi_push_cmnd(cmnd);
		goto out;
	case ISCSI_OP_SCSI_DATA_OUT:
		data_out_end(cmnd);
		break;
	default:
		PRINT_ERROR("Unexpected cmnd op %x", cmnd_opcode(cmnd));
		break;
	}

	req_cmnd_release(cmnd);

out:
	TRACE_EXIT();
	return;
}

#if !defined(CONFIG_TCP_ZERO_COPY_TRANSFER_COMPLETION_NOTIFICATION)
static int iscsi_alloc_data_buf(struct scst_cmd *cmd)
{
	/*
	 * sock->ops->sendpage() is async zero copy operation,
	 * so we must be sure not to free and reuse
	 * the command's buffer before the sending was completed
	 * by the network layers. It is possible only if we
	 * don't use SGV cache.
	 */
	EXTRACHECKS_BUG_ON(!(scst_cmd_get_data_direction(cmd) & SCST_DATA_READ));
	scst_cmd_set_no_sgv(cmd);
	return 1;
}
#endif

static void iscsi_preprocessing_done(struct scst_cmd *scst_cmd)
{
	struct iscsi_cmnd *req = (struct iscsi_cmnd *)
				scst_cmd_get_tgt_priv(scst_cmd);

	TRACE_DBG("req %p", req);

	if (req->conn->rx_task == current)
		req->scst_state = ISCSI_CMD_STATE_AFTER_PREPROC;
	else {
		/*
		 * We wait for the state change without any protection, so
		 * without cmnd_get() it is possible that req will die
		 * "immediately" after the state assignment and
		 * iscsi_make_conn_rd_active() will operate on dead data.
		 * We use the ordered version of cmnd_get(), because "get"
		 * must be done before the state assignment.
		 *
		 * We protected from the race on calling cmnd_rx_continue(),
		 * because there can be only one read thread processing
		 * connection.
		 */
		cmnd_get_ordered(req);
		req->scst_state = ISCSI_CMD_STATE_AFTER_PREPROC;
		iscsi_make_conn_rd_active(req->conn);
		if (unlikely(req->conn->closing)) {
			TRACE_DBG("Waking up closing conn %p", req->conn);
			wake_up(&req->conn->read_state_waitQ);
		}
		cmnd_put(req);
	}

	return;
}

/*
 * No locks.
 *
 * IMPORTANT! Connection conn must be protected by additional conn_get()
 * upon entrance in this function, because otherwise it could be destroyed
 * inside as a result of iscsi_send(), which releases sent commands.
 */
static void iscsi_try_local_processing(struct iscsi_conn *conn)
{
	int local;

	TRACE_ENTRY();

	spin_lock_bh(&iscsi_wr_lock);
	switch (conn->wr_state) {
	case ISCSI_CONN_WR_STATE_IN_LIST:
		list_del(&conn->wr_list_entry);
		/* go through */
	case ISCSI_CONN_WR_STATE_IDLE:
#ifdef CONFIG_SCST_EXTRACHECKS
		conn->wr_task = current;
#endif
		conn->wr_state = ISCSI_CONN_WR_STATE_PROCESSING;
		conn->wr_space_ready = 0;
		local = 1;
		break;
	default:
		local = 0;
		break;
	}
	spin_unlock_bh(&iscsi_wr_lock);

	if (local) {
		int rc = 1;

		if (test_write_ready(conn))
			rc = iscsi_send(conn);

		spin_lock_bh(&iscsi_wr_lock);
#ifdef CONFIG_SCST_EXTRACHECKS
		conn->wr_task = NULL;
#endif
		if ((rc <= 0) || test_write_ready(conn)) {
			list_add_tail(&conn->wr_list_entry, &iscsi_wr_list);
			conn->wr_state = ISCSI_CONN_WR_STATE_IN_LIST;
			wake_up(&iscsi_wr_waitQ);
		} else
			conn->wr_state = ISCSI_CONN_WR_STATE_IDLE;
		spin_unlock_bh(&iscsi_wr_lock);
	}

	TRACE_EXIT();
	return;
}

static int iscsi_xmit_response(struct scst_cmd *scst_cmd)
{
	int is_send_status = scst_cmd_get_is_send_status(scst_cmd);
	struct iscsi_cmnd *req = (struct iscsi_cmnd *)
					scst_cmd_get_tgt_priv(scst_cmd);
	struct iscsi_conn *conn = req->conn;
	int status = scst_cmd_get_status(scst_cmd);
	u8 *sense = scst_cmd_get_sense_buffer(scst_cmd);
	int sense_len = scst_cmd_get_sense_buffer_len(scst_cmd);

	if (unlikely(scst_cmd_atomic(scst_cmd)))
		return SCST_TGT_RES_NEED_THREAD_CTX;

	scst_cmd_set_tgt_priv(scst_cmd, NULL);

	EXTRACHECKS_BUG_ON(req->scst_state != ISCSI_CMD_STATE_RESTARTED);

	if (unlikely(scst_cmd_aborted(scst_cmd)))
		set_bit(ISCSI_CMD_ABORTED, &req->prelim_compl_flags);

	if (unlikely(req->prelim_compl_flags != 0)) {
		if (test_bit(ISCSI_CMD_ABORTED, &req->prelim_compl_flags)) {
			TRACE_MGMT_DBG("req %p (scst_cmd %p) aborted", req,
				req->scst_cmd);
			scst_set_delivery_status(req->scst_cmd,
				SCST_CMD_DELIVERY_ABORTED);
			req->scst_state = ISCSI_CMD_STATE_PROCESSED;
			req_cmnd_release_force(req);
			goto out;
		}

		TRACE_DBG("Prelim completed req %p", req);

		/*
		 * We could preliminary have finished req before we
		 * knew its device, so check if we return correct sense
		 * format.
		 */
		scst_check_convert_sense(scst_cmd);

		if (!req->own_sg) {
			req->sg = scst_cmd_get_sg(scst_cmd);
			req->sg_cnt = scst_cmd_get_sg_cnt(scst_cmd);
		}
	} else {
		EXTRACHECKS_BUG_ON(req->own_sg);
		req->sg = scst_cmd_get_sg(scst_cmd);
		req->sg_cnt = scst_cmd_get_sg_cnt(scst_cmd);
	}

	req->bufflen = scst_cmd_get_resp_data_len(scst_cmd);

	req->scst_state = ISCSI_CMD_STATE_PROCESSED;

	TRACE_DBG("req %p, is_send_status=%x, req->bufflen=%d, req->sg=%p, "
		"req->sg_cnt %d", req, is_send_status, req->bufflen, req->sg,
		req->sg_cnt);

	EXTRACHECKS_BUG_ON(req->hashed);
	if (req->main_rsp != NULL)
		EXTRACHECKS_BUG_ON(cmnd_opcode(req->main_rsp) != ISCSI_OP_REJECT);

	if (unlikely((req->bufflen != 0) && !is_send_status)) {
		PRINT_CRIT_ERROR("%s", "Sending DATA without STATUS is "
			"unsupported");
		scst_set_cmd_error(scst_cmd,
			SCST_LOAD_SENSE(scst_sense_hardw_error));
		sBUG(); /* ToDo */
	}

	if (req->bufflen != 0) {
		/*
		 * Check above makes sure that is_send_status is set,
		 * so status is valid here, but in future that could change.
		 * ToDo
		 */
		if ((status != SAM_STAT_CHECK_CONDITION) &&
		    ((cmnd_hdr(req)->flags & (ISCSI_CMD_WRITE|ISCSI_CMD_READ)) !=
				(ISCSI_CMD_WRITE|ISCSI_CMD_READ))) {
			send_data_rsp(req, status, is_send_status);
		} else {
			struct iscsi_cmnd *rsp;
			send_data_rsp(req, 0, 0);
			if (is_send_status) {
				rsp = create_status_rsp(req, status, sense,
					sense_len, true);
				iscsi_cmnd_init_write(rsp, 0);
			}
		}
	} else if (is_send_status) {
		struct iscsi_cmnd *rsp;
		rsp = create_status_rsp(req, status, sense, sense_len, false);
		iscsi_cmnd_init_write(rsp, 0);
	}
#ifdef CONFIG_SCST_EXTRACHECKS
	else
		sBUG();
#endif

	/*
	 * "_ordered" here to protect from reorder, which can lead to
	 * preliminary connection destroy in req_cmnd_release(). Just in
	 * case, actually, because reordering shouldn't go so far, but who
	 * knows..
	 */
	conn_get_ordered(conn);
	req_cmnd_release(req);
	iscsi_try_local_processing(conn);
	conn_put(conn);

out:
	return SCST_TGT_RES_SUCCESS;
}

/* Called under sn_lock */
static bool iscsi_is_delay_tm_resp(struct iscsi_cmnd *rsp)
{
	bool res = 0;
	struct iscsi_task_mgt_hdr *req_hdr =
		(struct iscsi_task_mgt_hdr *)&rsp->parent_req->pdu.bhs;
	int function = req_hdr->function & ISCSI_FUNCTION_MASK;
	struct iscsi_session *sess = rsp->conn->session;

	TRACE_ENTRY();

	/* This should be checked for immediate TM commands as well */

	switch (function) {
	default:
		if (before(sess->exp_cmd_sn, req_hdr->cmd_sn))
			res = 1;
		break;
	}

	TRACE_EXIT_RES(res);
	return res;
}

/* Called under sn_lock, but might drop it inside, then reaquire */
static void iscsi_check_send_delayed_tm_resp(struct iscsi_session *sess)
	__acquires(&sn_lock)
	__releases(&sn_lock)
{
	struct iscsi_cmnd *tm_rsp = sess->tm_rsp;

	TRACE_ENTRY();

	if (tm_rsp == NULL)
		goto out;

	if (iscsi_is_delay_tm_resp(tm_rsp))
		goto out;

	TRACE_MGMT_DBG("Sending delayed rsp %p", tm_rsp);

	sess->tm_rsp = NULL;
	sess->tm_active--;

	spin_unlock(&sess->sn_lock);

	sBUG_ON(sess->tm_active < 0);

	iscsi_cmnd_init_write(tm_rsp, ISCSI_INIT_WRITE_WAKE);

	spin_lock(&sess->sn_lock);

out:
	TRACE_EXIT();
	return;
}

static void iscsi_send_task_mgmt_resp(struct iscsi_cmnd *req, int status)
{
	struct iscsi_cmnd *rsp;
	struct iscsi_task_mgt_hdr *req_hdr =
				(struct iscsi_task_mgt_hdr *)&req->pdu.bhs;
	struct iscsi_task_rsp_hdr *rsp_hdr;
	struct iscsi_session *sess = req->conn->session;
	int fn = req_hdr->function & ISCSI_FUNCTION_MASK;

	TRACE_ENTRY();

	TRACE_MGMT_DBG("TM req %p finished", req);
	TRACE(TRACE_MGMT, "iSCSI TM fn %d finished, status %d", fn, status);

	rsp = iscsi_alloc_rsp(req);
	rsp_hdr = (struct iscsi_task_rsp_hdr *)&rsp->pdu.bhs;

	rsp_hdr->opcode = ISCSI_OP_SCSI_TASK_MGT_RSP;
	rsp_hdr->flags = ISCSI_FLG_FINAL;
	rsp_hdr->itt = req_hdr->itt;
	rsp_hdr->response = status;

	if (fn == ISCSI_FUNCTION_TARGET_COLD_RESET) {
		rsp->should_close_conn = 1;
		rsp->should_close_all_conn = 1;
	}

	sBUG_ON(sess->tm_rsp != NULL);

	spin_lock(&sess->sn_lock);
	if (iscsi_is_delay_tm_resp(rsp)) {
		TRACE_MGMT_DBG("Delaying TM fn %d response %p "
			"(req %p), because not all affected commands "
			"received (TM cmd sn %u, exp sn %u)",
			req_hdr->function & ISCSI_FUNCTION_MASK, rsp, req,
			req_hdr->cmd_sn, sess->exp_cmd_sn);
		sess->tm_rsp = rsp;
		spin_unlock(&sess->sn_lock);
		goto out_release;
	}
	sess->tm_active--;
	spin_unlock(&sess->sn_lock);

	sBUG_ON(sess->tm_active < 0);

	iscsi_cmnd_init_write(rsp, ISCSI_INIT_WRITE_WAKE);

out_release:
	req_cmnd_release(req);

	TRACE_EXIT();
	return;
}

static inline int iscsi_get_mgmt_response(int status)
{
	switch (status) {
	case SCST_MGMT_STATUS_SUCCESS:
		return ISCSI_RESPONSE_FUNCTION_COMPLETE;

	case SCST_MGMT_STATUS_TASK_NOT_EXIST:
		return ISCSI_RESPONSE_UNKNOWN_TASK;

	case SCST_MGMT_STATUS_LUN_NOT_EXIST:
		return ISCSI_RESPONSE_UNKNOWN_LUN;

	case SCST_MGMT_STATUS_FN_NOT_SUPPORTED:
		return ISCSI_RESPONSE_FUNCTION_UNSUPPORTED;

	case SCST_MGMT_STATUS_REJECTED:
	case SCST_MGMT_STATUS_FAILED:
	default:
		return ISCSI_RESPONSE_FUNCTION_REJECTED;
	}
}

static void iscsi_task_mgmt_fn_done(struct scst_mgmt_cmd *scst_mcmd)
{
	int fn = scst_mgmt_cmd_get_fn(scst_mcmd);
	struct iscsi_cmnd *req = (struct iscsi_cmnd *)
				scst_mgmt_cmd_get_tgt_priv(scst_mcmd);
	int status =
		iscsi_get_mgmt_response(scst_mgmt_cmd_get_status(scst_mcmd));

	if ((status == ISCSI_RESPONSE_UNKNOWN_TASK) &&
	    (fn == SCST_ABORT_TASK)) {
		/* If we are here, we found the task, so must succeed */
		status = ISCSI_RESPONSE_FUNCTION_COMPLETE;
	}

	TRACE_MGMT_DBG("req %p, scst_mcmd %p, fn %d, scst status %d, status %d",
		req, scst_mcmd, fn, scst_mgmt_cmd_get_status(scst_mcmd),
		status);

	switch (fn) {
	case SCST_NEXUS_LOSS_SESS:
	case SCST_ABORT_ALL_TASKS_SESS:
		/* They are internal */
		break;
	default:
		iscsi_send_task_mgmt_resp(req, status);
		scst_mgmt_cmd_set_tgt_priv(scst_mcmd, NULL);
		break;
	}
	return;
}

static int iscsi_scsi_aen(struct scst_aen *aen)
{
	int res = SCST_AEN_RES_SUCCESS;
	uint64_t lun = scst_aen_get_lun(aen);
	const uint8_t *sense = scst_aen_get_sense(aen);
	int sense_len = scst_aen_get_sense_len(aen);
	struct iscsi_session *sess = scst_sess_get_tgt_priv(
					scst_aen_get_sess(aen));
	struct iscsi_conn *conn;
	bool found;
	struct iscsi_cmnd *fake_req, *rsp;
	struct iscsi_async_msg_hdr *rsp_hdr;
	struct scatterlist *sg;

	TRACE_ENTRY();

	TRACE_MGMT_DBG("SCSI AEN to sess %p (initiator %s)", sess,
		sess->initiator_name);

	mutex_lock(&sess->target->target_mutex);

	found = false;
	list_for_each_entry_reverse(conn, &sess->conn_list, conn_list_entry) {
		if (!test_bit(ISCSI_CONN_SHUTTINGDOWN, &conn->conn_aflags) &&
		    (conn->conn_reinst_successor == NULL)) {
			found = true;
			break;
		}
	}
	if (!found) {
		TRACE_MGMT_DBG("Unable to find alive conn for sess %p", sess);
		goto out_err;
	}

	/* Create a fake request */
	fake_req = cmnd_alloc(conn, NULL);
	if (fake_req == NULL) {
		PRINT_ERROR("%s", "Unable to alloc fake AEN request");
		goto out_err;
	}

	mutex_unlock(&sess->target->target_mutex);

	rsp = iscsi_alloc_main_rsp(fake_req);
	if (rsp == NULL) {
		PRINT_ERROR("%s", "Unable to alloc AEN rsp");
		goto out_err_free_req;
	}

	fake_req->scst_state = ISCSI_CMD_STATE_AEN;
	fake_req->scst_aen = aen;

	rsp_hdr = (struct iscsi_async_msg_hdr *)&rsp->pdu.bhs;

	rsp_hdr->opcode = ISCSI_OP_ASYNC_MSG;
	rsp_hdr->flags = ISCSI_FLG_FINAL;
	rsp_hdr->lun = lun; /* it's already in SCSI form */
	rsp_hdr->ffffffff = 0xffffffff;
	rsp_hdr->async_event = ISCSI_ASYNC_SCSI;

	sg = rsp->sg = rsp->rsp_sg;
	rsp->sg_cnt = 2;
	rsp->own_sg = 1;

	sg_init_table(sg, 2);
	sg_set_buf(&sg[0], &rsp->sense_hdr, sizeof(rsp->sense_hdr));
	sg_set_buf(&sg[1], sense, sense_len);

	rsp->sense_hdr.length = cpu_to_be16(sense_len);
	rsp->pdu.datasize = sizeof(rsp->sense_hdr) + sense_len;
	rsp->bufflen = rsp->pdu.datasize;

	req_cmnd_release(fake_req);

out:
	TRACE_EXIT_RES(res);
	return res;

out_err_free_req:
	req_cmnd_release(fake_req);

out_err:
	mutex_unlock(&sess->target->target_mutex);
	res = SCST_AEN_RES_FAILED;
	goto out;
}

static int iscsi_report_aen(struct scst_aen *aen)
{
	int res;
	int event_fn = scst_aen_get_event_fn(aen);

	TRACE_ENTRY();

	switch (event_fn) {
	case SCST_AEN_SCSI:
		res = iscsi_scsi_aen(aen);
		break;
	default:
		TRACE_MGMT_DBG("Unsupported AEN %d", event_fn);
		res = SCST_AEN_RES_NOT_SUPPORTED;
		break;
	}

	TRACE_EXIT_RES(res);
	return res;
}

void iscsi_send_nop_in(struct iscsi_conn *conn)
{
	struct iscsi_cmnd *req, *rsp;
	struct iscsi_nop_in_hdr *rsp_hdr;

	TRACE_ENTRY();

	req = cmnd_alloc(conn, NULL);
	if (req == NULL) {
		PRINT_ERROR("%s", "Unable to alloc fake Nop-In request");
		goto out_err;
	}

	rsp = iscsi_alloc_main_rsp(req);
	if (rsp == NULL) {
		PRINT_ERROR("%s", "Unable to alloc Nop-In rsp");
		goto out_err_free_req;
	}

	cmnd_get(rsp);

	rsp_hdr = (struct iscsi_nop_in_hdr *)&rsp->pdu.bhs;
	rsp_hdr->opcode = ISCSI_OP_NOP_IN;
	rsp_hdr->flags = ISCSI_FLG_FINAL;
	rsp_hdr->itt = cpu_to_be32(ISCSI_RESERVED_TAG);
	rsp_hdr->ttt = conn->nop_in_ttt++;

	if (conn->nop_in_ttt == cpu_to_be32(ISCSI_RESERVED_TAG))
		conn->nop_in_ttt = 0;

	/* Supposed that all other fields are zeroed */

	TRACE_DBG("Sending Nop-In request (ttt 0x%08x)", rsp_hdr->ttt);
	spin_lock_bh(&conn->nop_req_list_lock);
	list_add_tail(&rsp->nop_req_list_entry, &conn->nop_req_list);
	spin_unlock_bh(&conn->nop_req_list_lock);

out_err_free_req:
	req_cmnd_release(req);

out_err:
	TRACE_EXIT();
	return;
}

static int iscsi_target_detect(struct scst_tgt_template *templ)
{
	/* Nothing to do */
	return 0;
}

static int iscsi_target_release(struct scst_tgt *scst_tgt)
{
	/* Nothing to do */
	return 0;
}

#if !defined(CONFIG_SCST_PROC) && \
	(defined(CONFIG_SCST_DEBUG) || defined(CONFIG_SCST_TRACING))
static struct scst_trace_log iscsi_local_trace_tbl[] = {
    { TRACE_D_WRITE,		"d_write" },
    { TRACE_CONN_OC,		"conn" },
    { TRACE_CONN_OC_DBG,	"conn_dbg" },
    { TRACE_D_IOV,		"iov" },
    { TRACE_D_DUMP_PDU,		"pdu" },
    { TRACE_NET_PG,		"net_page" },
    { 0,			NULL }
};

#define ISCSI_TRACE_TLB_HELP	", d_read, d_write, conn, conn_dbg, iov, pdu, net_page"
#endif

#define ISCSI_MGMT_CMD_HELP	\
	"       echo \"add_attribute IncomingUser name password\" >mgmt\n" \
	"       echo \"del_attribute IncomingUser name\" >mgmt\n" \
	"       echo \"add_attribute OutgoingUser name password\" >mgmt\n" \
	"       echo \"del_attribute OutgoingUser name\" >mgmt\n" \
	"       echo \"add_target_attribute target_name IncomingUser name password\" >mgmt\n" \
	"       echo \"del_target_attribute target_name IncomingUser name\" >mgmt\n" \
	"       echo \"add_target_attribute target_name OutgoingUser name password\" >mgmt\n" \
	"       echo \"del_target_attribute target_name OutgoingUser name\" >mgmt\n"

struct scst_tgt_template iscsi_template = {
	.name = "iscsi",
	.sg_tablesize = 0xFFFF /* no limit */,
	.threads_num = 0,
	.no_clustering = 1,
	.xmit_response_atomic = 0,
#ifndef CONFIG_SCST_PROC
	.tgtt_attrs = iscsi_attrs,
	.tgt_attrs = iscsi_tgt_attrs,
	.sess_attrs = iscsi_sess_attrs,
	.enable_target = iscsi_enable_target,
	.is_target_enabled = iscsi_is_target_enabled,
	.add_target = iscsi_sysfs_add_target,
	.del_target = iscsi_sysfs_del_target,
	.mgmt_cmd = iscsi_sysfs_mgmt_cmd,
	.mgmt_cmd_help = ISCSI_MGMT_CMD_HELP,
#endif
#if defined(CONFIG_SCST_DEBUG) || defined(CONFIG_SCST_TRACING)
	.default_trace_flags = ISCSI_DEFAULT_LOG_FLAGS,
	.trace_flags = &trace_flag,
#if !defined(CONFIG_SCST_PROC) && \
	(defined(CONFIG_SCST_DEBUG) || defined(CONFIG_SCST_TRACING))
	.trace_tbl = iscsi_local_trace_tbl,
	.trace_tbl_help = ISCSI_TRACE_TLB_HELP,
#endif
#endif
	.detect = iscsi_target_detect,
	.release = iscsi_target_release,
	.xmit_response = iscsi_xmit_response,
#if !defined(CONFIG_TCP_ZERO_COPY_TRANSFER_COMPLETION_NOTIFICATION)
	.alloc_data_buf = iscsi_alloc_data_buf,
#endif
	.preprocessing_done = iscsi_preprocessing_done,
	.pre_exec = iscsi_pre_exec,
	.task_mgmt_affected_cmds_done = iscsi_task_mgmt_affected_cmds_done,
	.task_mgmt_fn_done = iscsi_task_mgmt_fn_done,
	.report_aen = iscsi_report_aen,
};

static __init int iscsi_run_threads(int count, char *name, int (*fn)(void *))
{
	int res = 0;
	int i;
	struct iscsi_thread_t *thr;

	for (i = 0; i < count; i++) {
		thr = kmalloc(sizeof(*thr), GFP_KERNEL);
		if (!thr) {
			res = -ENOMEM;
			PRINT_ERROR("Failed to allocate thr %d", res);
			goto out;
		}
		thr->thr = kthread_run(fn, NULL, "%s%d", name, i);
		if (IS_ERR(thr->thr)) {
			res = PTR_ERR(thr->thr);
			PRINT_ERROR("kthread_create() failed: %d", res);
			kfree(thr);
			goto out;
		}
		list_add_tail(&thr->threads_list_entry, &iscsi_threads_list);
	}

out:
	return res;
}

static void iscsi_stop_threads(void)
{
	struct iscsi_thread_t *t, *tmp;

	list_for_each_entry_safe(t, tmp, &iscsi_threads_list,
				threads_list_entry) {
		int rc = kthread_stop(t->thr);
		if (rc < 0)
			TRACE_MGMT_DBG("kthread_stop() failed: %d", rc);
		list_del(&t->threads_list_entry);
		kfree(t);
	}
	return;
}

static int __init iscsi_init(void)
{
	int err = 0;
	int num;

	PRINT_INFO("iSCSI SCST Target - version %s", ISCSI_VERSION_STRING);

	dummy_page = alloc_pages(GFP_KERNEL, 0);
	if (dummy_page == NULL) {
		PRINT_ERROR("%s", "Dummy page allocation failed");
		goto out;
	}

	sg_init_table(&dummy_sg, 1);
	sg_set_page(&dummy_sg, dummy_page, PAGE_SIZE, 0);

#if defined(CONFIG_TCP_ZERO_COPY_TRANSFER_COMPLETION_NOTIFICATION)
	err = net_set_get_put_page_callbacks(iscsi_get_page_callback,
			iscsi_put_page_callback);
	if (err != 0) {
		PRINT_INFO("Unable to set page callbackes: %d", err);
		goto out_free_dummy;
	}
#else
#ifndef GENERATING_UPSTREAM_PATCH
	PRINT_WARNING("%s",
		"CONFIG_TCP_ZERO_COPY_TRANSFER_COMPLETION_NOTIFICATION "
		"not enabled in your kernel. ISCSI-SCST will be working with "
		"not the best performance. Refer README file for details.");
#endif
#endif

	ctr_major = register_chrdev(0, ctr_name, &ctr_fops);
	if (ctr_major < 0) {
		PRINT_ERROR("failed to register the control device %d",
			    ctr_major);
		err = ctr_major;
		goto out_callb;
	}

	err = event_init();
	if (err < 0)
		goto out_reg;

	iscsi_cmnd_cache = KMEM_CACHE(iscsi_cmnd, SCST_SLAB_FLAGS);
	if (!iscsi_cmnd_cache) {
		err = -ENOMEM;
		goto out_event;
	}

	err = scst_register_target_template(&iscsi_template);
	if (err < 0)
		goto out_kmem;

#ifdef CONFIG_SCST_PROC
	err = iscsi_procfs_init();
	if (err < 0)
		goto out_reg_tmpl;
#endif

	num = max((int)num_online_cpus(), 2);

	err = iscsi_run_threads(num, "iscsird", istrd);
	if (err != 0)
		goto out_thr;

	err = iscsi_run_threads(num, "iscsiwr", istwr);
	if (err != 0)
		goto out_thr;

out:
	return err;

out_thr:
#ifdef CONFIG_SCST_PROC
	iscsi_procfs_exit();
#endif
	iscsi_stop_threads();

#ifdef CONFIG_SCST_PROC
out_reg_tmpl:
#endif
	scst_unregister_target_template(&iscsi_template);

out_kmem:
	kmem_cache_destroy(iscsi_cmnd_cache);

out_event:
	event_exit();

out_reg:
	unregister_chrdev(ctr_major, ctr_name);

out_callb:
#if defined(CONFIG_TCP_ZERO_COPY_TRANSFER_COMPLETION_NOTIFICATION)
	net_set_get_put_page_callbacks(NULL, NULL);

out_free_dummy:
#endif
	__free_pages(dummy_page, 0);
	goto out;
}

static void __exit iscsi_exit(void)
{
	iscsi_stop_threads();

	unregister_chrdev(ctr_major, ctr_name);

#ifdef CONFIG_SCST_PROC
	iscsi_procfs_exit();
#endif
	event_exit();

	kmem_cache_destroy(iscsi_cmnd_cache);

	scst_unregister_target_template(&iscsi_template);

#if defined(CONFIG_TCP_ZERO_COPY_TRANSFER_COMPLETION_NOTIFICATION)
	net_set_get_put_page_callbacks(NULL, NULL);
#endif

	__free_pages(dummy_page, 0);
	return;
}

module_init(iscsi_init);
module_exit(iscsi_exit);

MODULE_VERSION(ISCSI_VERSION_STRING);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SCST iSCSI Target");
