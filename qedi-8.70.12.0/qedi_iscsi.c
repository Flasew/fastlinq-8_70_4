/*
 *  QLogic iSCSI Offload Driver
 *  Copyright (c) 2015-2018 Cavium Inc.
 *
 *  See LICENSE.qedi for copyright and licensing details.
 */

#include <linux/blkdev.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <scsi/scsi_tcq.h>

#include "qedi.h"
#include "qedi_iscsi.h"
#include "qedi_gbl.h"

int do_not_recover;

int qedi_recover_all_conns(struct qedi_ctx *qedi)
{
	struct qedi_conn *qedi_conn;
	u16 i;

	for (i = 0; i < qedi->max_active_conns; i++) {
		qedi_conn = qedi_get_conn_from_id(qedi, i);
		if (!qedi_conn)
			continue;

		qedi_start_conn_recovery(qedi, qedi_conn);
	}

	return SUCCESS;
}

static int qedi_eh_host_reset(struct scsi_cmnd *cmd)
{
	struct Scsi_Host *shost = cmd->device->host;
	struct qedi_ctx *qedi;

	qedi = (struct qedi_ctx *)iscsi_host_priv(shost);

	return qedi_recover_all_conns(qedi);
}

struct scsi_host_template qedi_host_template = {
	.module = THIS_MODULE,
	.name = "QLogic 40G/100G open-iscsi Initiator Driver",
	.proc_name = QEDI_MODULE_NAME,
	.queuecommand = iscsi_queuecommand,
	.eh_abort_handler = iscsi_eh_abort,
	.eh_device_reset_handler = iscsi_eh_device_reset,
	.eh_target_reset_handler = iscsi_eh_recover_target,
	.eh_host_reset_handler = qedi_eh_host_reset,
	.target_alloc = iscsi_target_alloc,
#if defined CHANGE_QDEPTH
	.change_queue_depth = scsi_change_queue_depth,
#else
	.change_queue_depth = iscsi_change_queue_depth,
#endif
	.can_queue = 3072,
	.this_id = -1,
	.sg_tablesize = QEDI_ISCSI_MAX_BDS_PER_CMD,
	.max_sectors = 0xffff,
	.dma_boundary = QEDI_HW_DMA_BOUNDARY,
	.cmd_per_lun = 128,
#if defined ENABLE_CLUSTER
	.use_clustering = ENABLE_CLUSTERING,
#endif
	.shost_attrs = qedi_shost_attrs,
};

static void qedi_conn_free_login_resources(struct qedi_ctx *qedi,
					   struct qedi_conn *qedi_conn)
{
	if (qedi_conn->gen_pdu.resp_bd_tbl) {
		dma_free_coherent(&qedi->pdev->dev, QEDI_PAGE_SIZE,
				  qedi_conn->gen_pdu.resp_bd_tbl,
				  qedi_conn->gen_pdu.resp_bd_dma);
		qedi_conn->gen_pdu.resp_bd_tbl = NULL;
	}

	if (qedi_conn->gen_pdu.req_bd_tbl) {
		dma_free_coherent(&qedi->pdev->dev, QEDI_PAGE_SIZE,
				  qedi_conn->gen_pdu.req_bd_tbl,
				  qedi_conn->gen_pdu.req_bd_dma);
		qedi_conn->gen_pdu.req_bd_tbl = NULL;
	}

	if (qedi_conn->gen_pdu.resp_buf) {
		dma_free_coherent(&qedi->pdev->dev,
				  ISCSI_DEF_MAX_RECV_SEG_LEN,
				  qedi_conn->gen_pdu.resp_buf,
				  qedi_conn->gen_pdu.resp_dma_addr);
		qedi_conn->gen_pdu.resp_buf = NULL;
	}

	if (qedi_conn->gen_pdu.req_buf) {
		dma_free_coherent(&qedi->pdev->dev,
				  ISCSI_DEF_MAX_RECV_SEG_LEN,
				  qedi_conn->gen_pdu.req_buf,
				  qedi_conn->gen_pdu.req_dma_addr);
		qedi_conn->gen_pdu.req_buf = NULL;
	}
}

static int qedi_conn_alloc_login_resources(struct qedi_ctx *qedi,
					   struct qedi_conn *qedi_conn)
{
	qedi_conn->gen_pdu.req_buf =
		dma_alloc_coherent(&qedi->pdev->dev,
				   ISCSI_DEF_MAX_RECV_SEG_LEN,
				   &qedi_conn->gen_pdu.req_dma_addr,
				   GFP_KERNEL);
	if (!qedi_conn->gen_pdu.req_buf)
		goto login_req_buf_failure;

	qedi_conn->gen_pdu.req_buf_size = 0;
	qedi_conn->gen_pdu.req_wr_ptr = qedi_conn->gen_pdu.req_buf;

	qedi_conn->gen_pdu.resp_buf =
		dma_alloc_coherent(&qedi->pdev->dev,
				   ISCSI_DEF_MAX_RECV_SEG_LEN,
				   &qedi_conn->gen_pdu.resp_dma_addr,
				   GFP_KERNEL);
	if (!qedi_conn->gen_pdu.resp_buf)
		goto login_resp_buf_failure;

	qedi_conn->gen_pdu.resp_buf_size = ISCSI_DEF_MAX_RECV_SEG_LEN;
	qedi_conn->gen_pdu.resp_wr_ptr = qedi_conn->gen_pdu.resp_buf;

	qedi_conn->gen_pdu.req_bd_tbl =
		dma_alloc_coherent(&qedi->pdev->dev, QEDI_PAGE_SIZE,
				   &qedi_conn->gen_pdu.req_bd_dma, GFP_KERNEL);
	if (!qedi_conn->gen_pdu.req_bd_tbl)
		goto login_req_bd_tbl_failure;

	qedi_conn->gen_pdu.resp_bd_tbl =
		dma_alloc_coherent(&qedi->pdev->dev, QEDI_PAGE_SIZE,
				   &qedi_conn->gen_pdu.resp_bd_dma,
				   GFP_KERNEL);
	if (!qedi_conn->gen_pdu.resp_bd_tbl)
		goto login_resp_bd_tbl_failure;

	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_SESS,
		  "Allocation successful, cid=0x%x\n",
		  qedi_conn->iscsi_conn_id);
	return 0;

login_resp_bd_tbl_failure:
	dma_free_coherent(&qedi->pdev->dev, QEDI_PAGE_SIZE,
			  qedi_conn->gen_pdu.req_bd_tbl,
			  qedi_conn->gen_pdu.req_bd_dma);
	qedi_conn->gen_pdu.req_bd_tbl = NULL;

login_req_bd_tbl_failure:
	dma_free_coherent(&qedi->pdev->dev, ISCSI_DEF_MAX_RECV_SEG_LEN,
			  qedi_conn->gen_pdu.resp_buf,
			  qedi_conn->gen_pdu.resp_dma_addr);
	qedi_conn->gen_pdu.resp_buf = NULL;
login_resp_buf_failure:
	dma_free_coherent(&qedi->pdev->dev, ISCSI_DEF_MAX_RECV_SEG_LEN,
			  qedi_conn->gen_pdu.req_buf,
			  qedi_conn->gen_pdu.req_dma_addr);
	qedi_conn->gen_pdu.req_buf = NULL;
login_req_buf_failure:
	iscsi_conn_printk(KERN_ERR, qedi_conn->cls_conn->dd_data,
			  "login resource alloc failed!!\n");
	return -ENOMEM;
}

static void qedi_destroy_cmd_pool(struct qedi_ctx *qedi,
				  struct iscsi_session *session)
{
	u16 i;

	for (i = 0; i < session->cmds_max; i++) {
		struct iscsi_task *task = session->cmds[i];
		struct qedi_cmd *cmd = task->dd_data;

		if (cmd->io_tbl.sge_tbl)
			dma_free_coherent(&qedi->pdev->dev,
					  QEDI_ISCSI_MAX_BDS_PER_CMD *
					  sizeof(struct scsi_sge),
					  cmd->io_tbl.sge_tbl,
					  cmd->io_tbl.sge_tbl_dma);

		if (cmd->sense_buffer)
			dma_free_coherent(&qedi->pdev->dev,
					  SCSI_SENSE_BUFFERSIZE,
					  cmd->sense_buffer,
					  cmd->sense_buffer_dma);
	}
}

static int qedi_alloc_sget(struct qedi_ctx *qedi, struct iscsi_session *session,
			   struct qedi_cmd *cmd)
{
	struct qedi_io_bdt *io = &cmd->io_tbl;
	struct scsi_sge *sge;

	io->sge_tbl = dma_alloc_coherent(&qedi->pdev->dev,
					QEDI_ISCSI_MAX_BDS_PER_CMD * sizeof(*sge),
					&io->sge_tbl_dma, GFP_KERNEL);
	if (!io->sge_tbl) {
		iscsi_session_printk(KERN_ERR, session,
				     "Could not allocate BD table.\n");
		return -ENOMEM;
	}

	io->sge_valid = 0;
	return 0;
}

static int qedi_setup_cmd_pool(struct qedi_ctx *qedi,
			       struct iscsi_session *session)
{
	u16 i;

	for (i = 0; i < session->cmds_max; i++) {
		struct iscsi_task *task = session->cmds[i];
		struct qedi_cmd *cmd = task->dd_data;

		task->hdr = &cmd->hdr;
		task->hdr_max = sizeof(struct iscsi_hdr);

		if (qedi_alloc_sget(qedi, session, cmd))
			goto free_sgets;

		cmd->sense_buffer = dma_alloc_coherent(&qedi->pdev->dev,
						       SCSI_SENSE_BUFFERSIZE,
						       &cmd->sense_buffer_dma,
						       GFP_KERNEL);
		if (!cmd->sense_buffer)
			goto free_sgets;
	}

	return 0;

free_sgets:
	qedi_destroy_cmd_pool(qedi, session);
	return -ENOMEM;
}

static struct iscsi_cls_session *
qedi_session_create(struct iscsi_endpoint *ep, u16 cmds_max,
		    u16 qdepth, uint32_t initial_cmdsn)
{
	struct Scsi_Host *shost;
	struct iscsi_cls_session *cls_session;
	struct qedi_ctx *qedi;
	struct qedi_endpoint *qedi_ep;

	if (!ep)
		return NULL;

	qedi_ep = ep->dd_data;
	shost = qedi_ep->qedi->shost;
	qedi = iscsi_host_priv(shost);

	if (cmds_max > qedi->max_sqes)
		cmds_max = qedi->max_sqes;
	else if (cmds_max < QEDI_SQ_WQES_MIN)
		cmds_max = QEDI_SQ_WQES_MIN;

	cls_session = iscsi_session_setup(&qedi_iscsi_transport, shost,
					  cmds_max, 0, sizeof(struct qedi_cmd),
					  initial_cmdsn, ISCSI_MAX_TARGET);
	if (!cls_session) {
		QEDI_ERR(&qedi->dbg_ctx,
			 "Failed to setup session for ep=%p\n", qedi_ep);
		return NULL;
	}

	if (qedi_setup_cmd_pool(qedi, cls_session->dd_data)) {
		QEDI_ERR(&qedi->dbg_ctx,
			 "Failed to setup cmd pool for ep=%p\n", qedi_ep);
		goto session_teardown;
	}

	return cls_session;

session_teardown:
	iscsi_session_teardown(cls_session);
	return NULL;
}

void qedi_session_destroy(struct iscsi_cls_session *cls_session)
{
	struct iscsi_session *session = cls_session->dd_data;
	struct Scsi_Host *shost = iscsi_session_to_shost(cls_session);
	struct qedi_ctx *qedi = iscsi_host_priv(shost);

	qedi_destroy_cmd_pool(qedi, session);
	iscsi_session_teardown(cls_session);
}

static struct iscsi_cls_conn *
qedi_conn_create(struct iscsi_cls_session *cls_session, uint32_t cid)
{
	struct Scsi_Host *shost = iscsi_session_to_shost(cls_session);
	struct qedi_ctx *qedi = iscsi_host_priv(shost);
	struct iscsi_cls_conn *cls_conn;
	struct qedi_conn *qedi_conn;
	struct iscsi_conn *conn;

	cls_conn = iscsi_conn_setup(cls_session, sizeof(*qedi_conn),
				    cid);
	if (!cls_conn) {
		QEDI_ERR(&qedi->dbg_ctx,
			 "conn_new: iscsi conn setup failed, cid=0x%x, cls_sess=%p!\n",
			 cid, cls_session);
		return NULL;
	}

	conn = cls_conn->dd_data;
	qedi_conn = conn->dd_data;
	qedi_conn->cls_conn = cls_conn;
	qedi_conn->qedi = qedi;
	qedi_conn->ep = NULL;
	qedi_conn->active_cmd_count = 0;
	INIT_LIST_HEAD(&qedi_conn->active_cmd_list);
	spin_lock_init(&qedi_conn->list_lock);

	if (qedi_conn_alloc_login_resources(qedi, qedi_conn)) {
		iscsi_conn_printk(KERN_ALERT, conn,
				  "conn_new: login resc alloc failed, cid=0x%x, cls_sess=%p!!\n",
				   cid, cls_session);
		goto free_conn;
	}

	return cls_conn;

free_conn:
	iscsi_conn_teardown(cls_conn);
	return NULL;
}

void qedi_mark_device_missing(struct iscsi_cls_session *cls_session)
{
#if defined FRWD_LOCK
	struct iscsi_session *session = cls_session->dd_data;
	struct qedi_conn *qedi_conn = session->leadconn->dd_data;

	spin_lock_bh(&session->frwd_lock);
	set_bit(QEDI_BLOCK_IO, &qedi_conn->qedi->flags);
	spin_unlock_bh(&session->frwd_lock);
#else
	iscsi_block_session(cls_session);
#endif
}

void qedi_mark_device_available(struct iscsi_cls_session *cls_session)
{
#if defined FRWD_LOCK
	struct iscsi_session *session = cls_session->dd_data;
	struct qedi_conn *qedi_conn = session->leadconn->dd_data;

	spin_lock_bh(&session->frwd_lock);
	clear_bit(QEDI_BLOCK_IO, &qedi_conn->qedi->flags);
	spin_unlock_bh(&session->frwd_lock);
#else
	iscsi_unblock_session(cls_session);
#endif
}

/**
 * qedi_bind_conn_to_iscsi_cid - bind conn structure to 'iscsi_cid'
 * @qedi:	pointer to adapter instance
 * @conn:	pointer to iscsi connection
 *
 * update iscsi cid table entry with connection pointer. This enables
 *	driver to quickly get hold of connection structure pointer in
 *	completion/interrupt thread using iscsi context ID
 */
static int qedi_bind_conn_to_iscsi_cid(struct qedi_ctx *qedi,
				       struct qedi_conn *qedi_conn)
{
	u32 iscsi_cid = qedi_conn->iscsi_conn_id;

	if (qedi->cid_que.conn_cid_tbl[iscsi_cid]) {
		iscsi_conn_printk(KERN_ALERT, qedi_conn->cls_conn->dd_data,
				  "conn bind - entry #%d not free\n",
				  iscsi_cid);
		return -EBUSY;
	}

	qedi->cid_que.conn_cid_tbl[iscsi_cid] = qedi_conn;
	return 0;
}

/**
 * qedi_get_conn_from_id - maps an iscsi cid to corresponding conn ptr
 * @qedi:	pointer to adapter instance
 * @iscsi_cid:	iscsi context ID, range 0 - (MAX_CONN - 1)
 */
struct qedi_conn *qedi_get_conn_from_id(struct qedi_ctx *qedi, u32 iscsi_cid)
{
	if (!qedi->cid_que.conn_cid_tbl) {
		QEDI_ERR(&qedi->dbg_ctx, "missing conn<->cid table\n");
		return NULL;

	} else if (iscsi_cid >= qedi->max_active_conns) {
		QEDI_ERR(&qedi->dbg_ctx, "wrong cid #%d\n", iscsi_cid);
		return NULL;
	}
	return qedi->cid_que.conn_cid_tbl[iscsi_cid];
}

static int qedi_conn_bind(struct iscsi_cls_session *cls_session,
			  struct iscsi_cls_conn *cls_conn,
			  u64 transport_fd, int is_leading)
{
	struct iscsi_conn *conn = cls_conn->dd_data;
	struct qedi_conn *qedi_conn = conn->dd_data;
	struct Scsi_Host *shost = iscsi_session_to_shost(cls_session);
	struct qedi_ctx *qedi = iscsi_host_priv(shost);
	struct qedi_endpoint *qedi_ep;
	struct iscsi_endpoint *ep;
	int rc = 0;

	ep = iscsi_lookup_endpoint(transport_fd);
	if (!ep)
		return -EINVAL;

	qedi_ep = ep->dd_data;
	if ((qedi_ep->state == EP_STATE_TCP_FIN_RCVD) ||
	    (qedi_ep->state == EP_STATE_TCP_RST_RCVD)) {
		rc = -EINVAL;
		goto put_ep;
	}

	if (iscsi_conn_bind(cls_session, cls_conn, is_leading)) {
		rc = -EINVAL;
		goto put_ep;
	}

	qedi_ep->conn = qedi_conn;
	qedi_conn->ep = qedi_ep;
	qedi_conn->iscsi_ep = ep;
	qedi_conn->iscsi_conn_id = qedi_ep->iscsi_cid;
	qedi_conn->fw_cid = qedi_ep->fw_cid;
	atomic_set(&qedi_conn->cmd_cleanup_req, 0);
	atomic_set(&qedi_conn->cmd_cleanup_cmpl, 0);

	if (qedi_bind_conn_to_iscsi_cid(qedi, qedi_conn)) {
		rc = -EINVAL;
		goto put_ep;
	}

	spin_lock_init(&qedi_conn->tmf_work_lock);
	INIT_LIST_HEAD(&qedi_conn->tmf_work_list);
	init_waitqueue_head(&qedi_conn->wait_queue);
put_ep:
	qedi_put_endpoint(ep);
	return rc;
}

static int qedi_iscsi_update_conn(struct qedi_ctx *qedi,
				  struct qedi_conn *qedi_conn)
{
	struct qed_iscsi_params_update *conn_info;
	struct iscsi_cls_conn *cls_conn = qedi_conn->cls_conn;
	struct iscsi_conn *conn = cls_conn->dd_data;
	struct qedi_endpoint *qedi_ep;
	int rval;

	qedi_ep = qedi_conn->ep;

	conn_info = kzalloc(sizeof(*conn_info), GFP_KERNEL);
	if (!conn_info) {
		QEDI_ERR(&qedi->dbg_ctx, "memory alloc failed\n");
		return -ENOMEM;
	}

	conn_info->update_flag = 0;

	if (conn->hdrdgst_en)
		SET_FIELD(conn_info->update_flag,
			  ISCSI_CONN_UPDATE_RAMROD_PARAMS_HD_EN, true);
	if (conn->datadgst_en)
		SET_FIELD(conn_info->update_flag,
			  ISCSI_CONN_UPDATE_RAMROD_PARAMS_DD_EN, true);
	if (conn->session->initial_r2t_en)
		SET_FIELD(conn_info->update_flag,
			  ISCSI_CONN_UPDATE_RAMROD_PARAMS_INITIAL_R2T,
			  true);
	if (conn->session->imm_data_en)
		SET_FIELD(conn_info->update_flag,
			  ISCSI_CONN_UPDATE_RAMROD_PARAMS_IMMEDIATE_DATA,
			  true);

	conn_info->max_seq_size = conn->session->max_burst;
	conn_info->max_recv_pdu_length = conn->max_recv_dlength;
	conn_info->max_send_pdu_length = conn->max_xmit_dlength;
	conn_info->first_seq_length = conn->session->first_burst;
	conn_info->exp_stat_sn = conn->exp_statsn;

	rval = qedi_ops->update_conn(qedi->cdev, qedi_ep->handle,
				     conn_info);
	if (rval) {
		rval = -ENXIO;
		QEDI_ERR(&qedi->dbg_ctx, "Could not update connection\n");
	}

	kfree(conn_info);
	return rval;
}

static u16 qedi_calc_mss(u16 pmtu, u8 is_ipv6, u8 tcp_ts_en, u8 vlan_en)
{
	u16 mss = 0;
	u16 hdrs = TCP_HDR_LEN;

	if (is_ipv6)
		hdrs += IPV6_HDR_LEN;
	else
		hdrs += IPV4_HDR_LEN;

	mss = pmtu - hdrs;

	if (!mss)
		mss = DEF_MSS;

	return mss;
}

static int qedi_iscsi_offload_conn(struct qedi_endpoint *qedi_ep)
{
	struct qed_iscsi_params_offload *conn_info;
	struct qedi_ctx *qedi = qedi_ep->qedi;
	int rval;
	u16 i;

	conn_info = kzalloc(sizeof(*conn_info), GFP_KERNEL);
	if (!conn_info) {
		QEDI_ERR(&qedi->dbg_ctx,
			 "Failed to allocate memory ep=%p\n", qedi_ep);
		return -ENOMEM;
	}

	ether_addr_copy(conn_info->src.mac, qedi_ep->src_mac);
	ether_addr_copy(conn_info->dst.mac, qedi_ep->dst_mac);

	conn_info->src.ip[0] = ntohl(qedi_ep->src_addr[0]);
	conn_info->dst.ip[0] = ntohl(qedi_ep->dst_addr[0]);

	if (qedi_ep->ip_type == TCP_IPV4) {
		conn_info->ip_version = 0;
		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_CONN,
			  "After ntohl: src_addr=%pI4, dst_addr=%pI4\n",
			  qedi_ep->src_addr, qedi_ep->dst_addr);
	} else {
		for (i = 1; i < 4; i++) {
			conn_info->src.ip[i] = ntohl(qedi_ep->src_addr[i]);
			conn_info->dst.ip[i] = ntohl(qedi_ep->dst_addr[i]);
		}

		conn_info->ip_version = 1;
		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_CONN,
			  "After ntohl: src_addr=%pI6, dst_addr=%pI6\n",
			  qedi_ep->src_addr, qedi_ep->dst_addr);
	}

	conn_info->src.port = qedi_ep->src_port;
	conn_info->dst.port = qedi_ep->dst_port;

	conn_info->layer_code = ISCSI_SLOW_PATH_LAYER_CODE;
	conn_info->sq_pbl_addr = qedi_ep->sq_pbl_dma;
	conn_info->vlan_id = qedi_ep->vlan_id;

	SET_FIELD(conn_info->tcp_flags, TCP_OFFLOAD_PARAMS_TS_EN, 1);
	SET_FIELD(conn_info->tcp_flags, TCP_OFFLOAD_PARAMS_DA_EN, 1);
	SET_FIELD(conn_info->tcp_flags, TCP_OFFLOAD_PARAMS_DA_CNT_EN, 1);
	SET_FIELD(conn_info->tcp_flags, TCP_OFFLOAD_PARAMS_KA_EN, 1);

	conn_info->default_cq = (qedi_ep->fw_cid % qedi->num_queues);

	conn_info->ka_max_probe_cnt = DEF_KA_MAX_PROBE_COUNT;
	conn_info->dup_ack_theshold = 3;
	conn_info->rcv_wnd = 65535;

	conn_info->ss_thresh = 65535;
	conn_info->srtt = 300;
	conn_info->rtt_var = 150;
	conn_info->flow_label = 0;
	conn_info->ka_timeout = DEF_KA_TIMEOUT;
	conn_info->ka_interval = DEF_KA_INTERVAL;
	conn_info->max_rt_time = DEF_MAX_RT_TIME;
	conn_info->ttl = DEF_TTL;
	conn_info->tos_or_tc = DEF_TOS;
	conn_info->remote_port = qedi_ep->dst_port;
	conn_info->local_port = qedi_ep->src_port;

	conn_info->mss = qedi_calc_mss(qedi_ep->pmtu,
				       (qedi_ep->ip_type == TCP_IPV6),
				       1, (qedi_ep->vlan_id != 0));

	conn_info->cwnd = DEF_MAX_CWND * conn_info->mss;
	conn_info->rcv_wnd_scale = 4;
	conn_info->da_timeout_value = 200;
	conn_info->ack_frequency = 2;

	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_INFO,
		  "Default cq index [%d], mss [%d]\n",
		  conn_info->default_cq, conn_info->mss);

	/* Prepare the doorbell parameters */
	qedi_ep->db_data.agg_flags = 0;
	qedi_ep->db_data.params = 0;
	SET_FIELD(qedi_ep->db_data.params, ISCSI_DB_DATA_DEST, DB_DEST_XCM);
	SET_FIELD(qedi_ep->db_data.params, ISCSI_DB_DATA_AGG_CMD,
			DB_AGG_CMD_MAX);
	SET_FIELD(qedi_ep->db_data.params, ISCSI_DB_DATA_AGG_VAL_SEL,
			DQ_XCM_ISCSI_SQ_PROD_CMD);
	SET_FIELD(qedi_ep->db_data.params, ISCSI_DB_DATA_BYPASS_EN, 1);

	/* register doorbell with doorbell recovery mechanism */
	rval = qedi_ops->common->db_recovery_add(qedi->cdev,
						 qedi_ep->p_doorbell,
						 &qedi_ep->db_data,
						 DB_REC_WIDTH_32B,
						 DB_REC_KERNEL);
	if (rval) {
		kfree(conn_info);
		return rval;
	}

	rval = qedi_ops->offload_conn(qedi->cdev, qedi_ep->handle, conn_info);
	if (rval) {
		/* delete doorbell from doorbell recovery mechanism */
		rval = qedi_ops->common->db_recovery_del(qedi->cdev,
							 qedi_ep->p_doorbell,
							 &qedi_ep->db_data);

		QEDI_ERR(&qedi->dbg_ctx, "offload_conn returned %d, ep=%p\n",
			 rval, qedi_ep);
	}

	kfree(conn_info);
	return rval;
}

static int qedi_conn_start(struct iscsi_cls_conn *cls_conn)
{
	struct iscsi_conn *conn = cls_conn->dd_data;
	struct qedi_conn *qedi_conn = conn->dd_data;
	struct qedi_ctx *qedi;
	int rval;

	qedi = qedi_conn->qedi;

	rval = qedi_iscsi_update_conn(qedi, qedi_conn);
	if (rval) {
		iscsi_conn_printk(KERN_ALERT, conn,
				  "conn_start: FW oflload conn failed.\n");
		rval = -EINVAL;
		goto start_err;
	}

	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_INFO,
		  "Active cmd count=%d\n", qedi_conn->active_cmd_count);

	spin_lock(&qedi_conn->tmf_work_lock);
	qedi_conn->fw_cleanup_works = 0;
	qedi_conn->ep_disconnect_starting = false;
	spin_unlock(&qedi_conn->tmf_work_lock);

	qedi_conn->abrt_conn = 0;

	rval = iscsi_conn_start(cls_conn);
	if (rval) {
		iscsi_conn_printk(KERN_ALERT, conn,
				  "iscsi_conn_start: FW oflload conn failed!!\n");
	}

start_err:
	return rval;
}

void qedi_conn_destroy(struct iscsi_cls_conn *cls_conn)
{
	struct iscsi_conn *conn = cls_conn->dd_data;
	struct qedi_conn *qedi_conn = conn->dd_data;
	struct Scsi_Host *shost;
	struct qedi_ctx *qedi;

	shost = iscsi_session_to_shost(iscsi_conn_to_session(cls_conn));
	qedi = iscsi_host_priv(shost);

	qedi_conn_free_login_resources(qedi, qedi_conn);
	iscsi_conn_teardown(cls_conn);
}

static int qedi_ep_get_param(struct iscsi_endpoint *ep,
			     enum iscsi_param param, char *buf)
{
	struct qedi_endpoint *qedi_ep = ep->dd_data;
	u32 len;

	if (!qedi_ep)
		return -ENOTCONN;

	switch (param) {
	case ISCSI_PARAM_CONN_PORT:
		len = sprintf(buf, "%hu\n", qedi_ep->dst_port);
		break;
	case ISCSI_PARAM_CONN_ADDRESS:
		if (qedi_ep->ip_type == TCP_IPV4)
			len = sprintf(buf, "%pI4\n", qedi_ep->dst_addr);
		else
			len = sprintf(buf, "%pI6\n", qedi_ep->dst_addr);
		break;
	default:
		return -ENOTCONN;
	}

	return len;
}

static int qedi_host_get_param(struct Scsi_Host *shost,
			       enum iscsi_host_param param, char *buf)
{
	struct qedi_ctx *qedi;
	u32 len;

	qedi = iscsi_host_priv(shost);

	switch (param) {
	case ISCSI_HOST_PARAM_HWADDRESS:
		len = sysfs_format_mac(buf, qedi->mac, 6);
		break;
	case ISCSI_HOST_PARAM_NETDEV_NAME:
		len = sprintf(buf, "host%d\n", shost->host_no);
		break;
	case ISCSI_HOST_PARAM_IPADDRESS:
		if (qedi->ip_type == TCP_IPV4)
			len = sprintf(buf, "%pI4\n", qedi->src_ip);
		else
			len = sprintf(buf, "%pI6\n", qedi->src_ip);
		break;
	default:
		return iscsi_host_get_param(shost, param, buf);
	}

	return len;
}

static void qedi_conn_get_stats(struct iscsi_cls_conn *cls_conn,
				struct iscsi_stats *stats)
{
	struct iscsi_conn *conn = cls_conn->dd_data;
	struct qed_iscsi_stats iscsi_stats;
	struct Scsi_Host *shost;
	struct qedi_ctx *qedi;

	shost = iscsi_session_to_shost(iscsi_conn_to_session(cls_conn));
	qedi = iscsi_host_priv(shost);

	mutex_lock(&qedi->stats_lock);
	qedi_ops->get_stats(qedi->cdev, &iscsi_stats);
	mutex_unlock(&qedi->stats_lock);

	conn->txdata_octets = iscsi_stats.iscsi_tx_bytes_cnt;
	conn->rxdata_octets = iscsi_stats.iscsi_rx_bytes_cnt;
	conn->dataout_pdus_cnt = (uint32_t)iscsi_stats.iscsi_tx_data_pdu_cnt;
	conn->datain_pdus_cnt = (uint32_t)iscsi_stats.iscsi_rx_data_pdu_cnt;
	conn->r2t_pdus_cnt = (uint32_t)iscsi_stats.iscsi_rx_r2t_pdu_cnt;

	stats->txdata_octets = conn->txdata_octets;
	stats->rxdata_octets = conn->rxdata_octets;
	stats->scsicmd_pdus = conn->scsicmd_pdus_cnt;
	stats->dataout_pdus = conn->dataout_pdus_cnt;
	stats->scsirsp_pdus = conn->scsirsp_pdus_cnt;
	stats->datain_pdus = conn->datain_pdus_cnt;
	stats->r2t_pdus = conn->r2t_pdus_cnt;
	stats->tmfcmd_pdus = conn->tmfcmd_pdus_cnt;
	stats->tmfrsp_pdus = conn->tmfrsp_pdus_cnt;
	stats->digest_err = 0;
	stats->timeout_err = 0;
	strcpy(stats->custom[0].desc, "eh_abort_cnt");
	stats->custom[0].value = conn->eh_abort_cnt;
	stats->custom_length = 1;
}

static void qedi_iscsi_prep_generic_pdu_bd(struct qedi_conn *qedi_conn)
{
	struct scsi_sge *bd_tbl;

	bd_tbl = (struct scsi_sge *)qedi_conn->gen_pdu.req_bd_tbl;

	bd_tbl->sge_addr.hi =
		(u32)((u64)qedi_conn->gen_pdu.req_dma_addr >> 32);
	bd_tbl->sge_addr.lo = (u32)qedi_conn->gen_pdu.req_dma_addr;
	bd_tbl->sge_len = qedi_conn->gen_pdu.req_wr_ptr -
				qedi_conn->gen_pdu.req_buf;
	//bd_tbl->reserved0 = 0;
	bd_tbl = (struct scsi_sge  *)qedi_conn->gen_pdu.resp_bd_tbl;
	bd_tbl->sge_addr.hi =
			(u32)((u64)qedi_conn->gen_pdu.resp_dma_addr >> 32);
	bd_tbl->sge_addr.lo = (u32)qedi_conn->gen_pdu.resp_dma_addr;
	bd_tbl->sge_len = ISCSI_DEF_MAX_RECV_SEG_LEN;
	//bd_tbl->reserved0 = 0;
}

static int qedi_iscsi_send_generic_request(struct iscsi_task *task)
{
	struct qedi_cmd *cmd = task->dd_data;
	struct qedi_conn *qedi_conn = cmd->conn;
	char *buf;
	u32 data_len;
	int rc = 0;

	qedi_iscsi_prep_generic_pdu_bd(qedi_conn);
	switch (task->hdr->opcode & ISCSI_OPCODE_MASK) {
	case ISCSI_OP_LOGIN:
		qedi_send_iscsi_login(qedi_conn, task);
		break;
	case ISCSI_OP_NOOP_OUT:
#ifdef ERROR_INJECT
	if (qedi_conn->qedi->drop_nopout) {
		qedi_conn->qedi->drop_nopout--;
		goto out;
	}
#endif
		if (qedi_conn->ep_disconnect_starting) {
			QEDI_INFO(&qedi_conn->qedi->dbg_ctx, QEDI_LOG_INFO,
				  "endpoint in recovery\n");
			rc = -EACCES;
			break;
		}
		data_len = qedi_conn->gen_pdu.req_buf_size;
		buf = qedi_conn->gen_pdu.req_buf;
		if (data_len)
			rc = qedi_send_iscsi_nopout(qedi_conn, task,
						    buf, data_len, 1);
		else
			rc = qedi_send_iscsi_nopout(qedi_conn, task,
						    NULL, 0, 1);
		break;
	case ISCSI_OP_LOGOUT:
		rc = qedi_send_iscsi_logout(qedi_conn, task);
		break;
	case ISCSI_OP_SCSI_TMFUNC:
		rc = qedi_send_iscsi_tmf(qedi_conn, task);
		break;
	case ISCSI_OP_TEXT:
		rc = qedi_send_iscsi_text(qedi_conn, task);
		break;
	default:
		iscsi_conn_printk(KERN_ALERT, qedi_conn->cls_conn->dd_data,
				  "unsupported op 0x%x\n", task->hdr->opcode);
	}

#ifdef ERROR_INJECT
out:
#endif
	return rc;
}

static int qedi_mtask_xmit(struct iscsi_conn *conn, struct iscsi_task *task)
{
	struct qedi_conn *qedi_conn = conn->dd_data;
	struct qedi_cmd *cmd = task->dd_data;

	if (test_bit(QEDI_IN_SHUTDOWN, &qedi_conn->qedi->flags)) {
		QEDI_INFO(&qedi_conn->qedi->dbg_ctx, QEDI_LOG_INFO, "qedi in SHUTDOWN mode\n");
		return 0;
	}

	memset(qedi_conn->gen_pdu.req_buf, 0, ISCSI_DEF_MAX_RECV_SEG_LEN);

	qedi_conn->gen_pdu.req_buf_size = task->data_count;

	if (task->data_count) {
		memcpy(qedi_conn->gen_pdu.req_buf, task->data,
		       task->data_count);
		qedi_conn->gen_pdu.req_wr_ptr =
			qedi_conn->gen_pdu.req_buf + task->data_count;
	}

	cmd->conn = conn->dd_data;
	return qedi_iscsi_send_generic_request(task);
}

static int qedi_task_xmit(struct iscsi_task *task)
{
	struct iscsi_conn *conn = task->conn;
	struct qedi_conn *qedi_conn = conn->dd_data;
	struct qedi_cmd *cmd = task->dd_data;
	struct scsi_cmnd *sc = task->sc;

	/* Clear now so in cleanup_task we know it didn't make it */
	cmd->scsi_cmd = NULL;
	cmd->task_id = U16_MAX;


	if (test_bit(QEDI_IN_SHUTDOWN, &qedi_conn->qedi->flags)) {
		QEDI_INFO(&qedi_conn->qedi->dbg_ctx, QEDI_LOG_INFO, "qedi in SHUTDOWN mode\n");
		return 0;
	}

	if (test_bit(QEDI_BLOCK_IO, &qedi_conn->qedi->flags))
		return -EACCES;

	cmd->state = 0;
	cmd->task = NULL;
	cmd->use_slowpath = false;
	cmd->conn = qedi_conn;
	cmd->task = task;
	cmd->io_cmd_in_list = false;
	INIT_LIST_HEAD(&cmd->io_cmd);

	if (!sc)
		return qedi_mtask_xmit(conn, task);

	cmd->scsi_cmd = sc;
	return qedi_iscsi_send_ioreq(task);
}

static void qedi_offload_work(struct work_struct *work)
{
	struct qedi_endpoint *qedi_ep =
		container_of(work, struct qedi_endpoint, offload_work);
	struct qedi_ctx *qedi;
	int wait_delay = 5 * HZ;
	int ret;

	qedi = qedi_ep->qedi;

	ret = qedi_iscsi_offload_conn(qedi_ep);
	if (ret) {
		QEDI_ERR(&qedi->dbg_ctx,
			 "offload error: iscsi_cid=%u, qedi_ep=%p, ret=%d\n",
			 qedi_ep->iscsi_cid, qedi_ep, ret);
		qedi_ep->state = EP_STATE_OFLDCONN_FAILED;
		return;
	}

	ret = wait_event_interruptible_timeout(qedi_ep->tcp_ofld_wait,
					       (qedi_ep->state ==
					       EP_STATE_OFLDCONN_COMPL),
					       wait_delay);
	if (ret <= 0 || qedi_ep->state != EP_STATE_OFLDCONN_COMPL) {
		qedi_ep->state = EP_STATE_OFLDCONN_FAILED;
		QEDI_ERR(&qedi->dbg_ctx,
			 "Offload conn TIMEOUT iscsi_cid=%u, qedi_ep=%p\n",
			 qedi_ep->iscsi_cid, qedi_ep);
	}
}

static struct iscsi_endpoint *
qedi_ep_connect(struct Scsi_Host *shost, struct sockaddr *dst_addr,
		int non_blocking)
{
	struct qedi_ctx *qedi;
	struct iscsi_endpoint *ep;
	struct qedi_endpoint *qedi_ep;
	struct sockaddr_in *addr;
	struct sockaddr_in6 *addr6;
	struct qed_dev *cdev  =  NULL;
	struct qedi_uio_dev *udev = NULL;
	struct iscsi_path path_req;
	u32 msg_type = ISCSI_KEVENT_IF_DOWN;
	u32 iscsi_cid = QEDI_CID_RESERVED;
	u16 len = 0;
	char *buf = NULL;
	int ret, rc;

	if (!shost) {
		ret = -ENXIO;
		QEDI_ERR(NULL, "shost is NULL\n");
		return ERR_PTR(ret);
	}

	if (do_not_recover) {
		ret = -ENOMEM;
		return ERR_PTR(ret);
	}

	qedi = iscsi_host_priv(shost);
	cdev = qedi->cdev;
	udev = qedi->udev;

	if (test_bit(QEDI_IN_OFFLINE, &qedi->flags) ||
	    test_bit(QEDI_IN_RECOVERY, &qedi->flags) ||
	    (atomic_read(&qedi->link_state) != QEDI_LINK_UP)) {
		ret = -ENOMEM;
		return ERR_PTR(ret);
	}

	ep = iscsi_create_endpoint(sizeof(struct qedi_endpoint));
	if (!ep) {
		QEDI_ERR(&qedi->dbg_ctx, "endpoint create fail\n");
		ret = -ENOMEM;
		return ERR_PTR(ret);
	}
	qedi_ep = ep->dd_data;
	memset(qedi_ep, 0, sizeof(struct qedi_endpoint));
	INIT_WORK(&qedi_ep->offload_work, qedi_offload_work);
	qedi_ep->state = EP_STATE_IDLE;
	qedi_ep->iscsi_cid = (u32)-1;
	qedi_ep->qedi = qedi;

	if (dst_addr->sa_family == AF_INET) {
		addr = (struct sockaddr_in *)dst_addr;
		memcpy(qedi_ep->dst_addr, &addr->sin_addr.s_addr,
		       sizeof(struct in_addr));
		qedi_ep->dst_port = ntohs(addr->sin_port);
		qedi_ep->ip_type = TCP_IPV4;
		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_CONN,
			  "dst_addr=%pI4, dst_port=%u\n",
			  qedi_ep->dst_addr, qedi_ep->dst_port);
	} else if (dst_addr->sa_family == AF_INET6) {
		addr6 = (struct sockaddr_in6 *)dst_addr;
		memcpy(qedi_ep->dst_addr, &addr6->sin6_addr,
		       sizeof(struct in6_addr));
		qedi_ep->dst_port = ntohs(addr6->sin6_port);
		qedi_ep->ip_type = TCP_IPV6;
		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_CONN,
			  "dst_addr=%pI6, dst_port=%u\n",
			  qedi_ep->dst_addr, qedi_ep->dst_port);
	} else {
		QEDI_ERR(&qedi->dbg_ctx, "Invalid endpoint\n");
	}

	if (atomic_read(&qedi->link_state) != QEDI_LINK_UP) {
		QEDI_WARN(&qedi->dbg_ctx, "qedi link down\n");
		ret = -ENXIO;
		goto ep_conn_exit;
	}

	ret = qedi_alloc_sq(qedi, qedi_ep);
	if (ret)
		goto ep_conn_exit;

	ret = qedi_ops->acquire_conn(qedi->cdev, &qedi_ep->handle,
				     &qedi_ep->fw_cid, &qedi_ep->p_doorbell);

	if (ret) {
		QEDI_ERR(&qedi->dbg_ctx, "Could not acquire connection\n");
		ret = -ENXIO;
		goto ep_free_sq;
	}

	iscsi_cid = qedi_ep->handle;
	qedi_ep->iscsi_cid = iscsi_cid;

	init_waitqueue_head(&qedi_ep->ofld_wait);
	init_waitqueue_head(&qedi_ep->tcp_ofld_wait);
	qedi_ep->state = EP_STATE_OFLDCONN_START;
	qedi->ep_tbl[iscsi_cid] = qedi_ep;

	buf = (char *)&path_req;
	len = sizeof(path_req);
	memset(&path_req, 0, len);

	msg_type = ISCSI_KEVENT_PATH_REQ;
	path_req.handle = (u64)qedi_ep->iscsi_cid;
	path_req.pmtu = qedi->ll2_mtu;
	qedi_ep->pmtu = qedi->ll2_mtu;
	if (qedi_ep->ip_type == TCP_IPV4) {
		memcpy(&path_req.dst.v4_addr, &qedi_ep->dst_addr,
		       sizeof(struct in_addr));
		path_req.ip_addr_len = 4;
	} else {
		memcpy(&path_req.dst.v6_addr, &qedi_ep->dst_addr,
		       sizeof(struct in6_addr));
		path_req.ip_addr_len = 16;
	}

	ret = iscsi_offload_mesg(shost, &qedi_iscsi_transport, msg_type, buf,
				 len);
	if (ret) {
		QEDI_ERR(&qedi->dbg_ctx,
			 "iscsi_offload_mesg() failed for cid=0x%x ret=%d\n",
			 iscsi_cid, ret);
		goto ep_rel_conn;
	}

	atomic_inc(&qedi->num_offloads);
	return ep;

ep_rel_conn:
	qedi->ep_tbl[iscsi_cid] = NULL;
	rc = qedi_ops->release_conn(qedi->cdev, qedi_ep->handle);
	if (rc)
		QEDI_WARN(&qedi->dbg_ctx, "release_conn returned %d\n",
			  rc);
ep_free_sq:
	qedi_free_sq(qedi, qedi_ep);
ep_conn_exit:
	iscsi_destroy_endpoint(ep);
	return ERR_PTR(ret);
}

static int qedi_ep_poll(struct iscsi_endpoint *ep, int timeout_ms)
{
	struct qedi_endpoint *qedi_ep;
	int ret = 0;

	if (do_not_recover)
		return 1;

	qedi_ep = ep->dd_data;
	if (qedi_ep->state == EP_STATE_IDLE ||
	    qedi_ep->state == EP_STATE_OFLDCONN_FAILED ||
	    qedi_ep->state == EP_STATE_OFLDCONN_NONE)
		return -1;

	if (qedi_ep->state == EP_STATE_OFLDCONN_COMPL)
		ret = 1;

	ret = wait_event_interruptible_timeout(qedi_ep->ofld_wait,
					       ((qedi_ep->state ==
						EP_STATE_OFLDCONN_FAILED) ||
						(qedi_ep->state ==
						EP_STATE_OFLDCONN_COMPL)),
						msecs_to_jiffies(timeout_ms));

	if (qedi_ep->state == EP_STATE_OFLDCONN_FAILED)
		ret = -1;

	if (ret > 0)
		return 1;
	else if (!ret)
		return 0;
	else
		return ret;
}

static void qedi_cleanup_active_cmd_list(struct qedi_conn *qedi_conn)
{
	struct qedi_cmd *cmd, *cmd_tmp;

	spin_lock(&qedi_conn->list_lock);
	list_for_each_entry_safe(cmd, cmd_tmp, &qedi_conn->active_cmd_list,
				 io_cmd) {
		cmd->io_cmd_in_list = false;
		list_del_init(&cmd->io_cmd);
		qedi_conn->active_cmd_count--;
	}
	spin_unlock(&qedi_conn->list_lock);
}

void qedi_ep_disconnect(struct iscsi_endpoint *ep)
{
	struct qedi_endpoint *qedi_ep;
	struct qedi_conn *qedi_conn = NULL;
	struct iscsi_conn *conn = NULL;
	struct qedi_ctx *qedi;
	int ret = 0;
	u32 wait_delay = ((60 * HZ) + DEF_MAX_RT_TIME);
	u32 abrt_conn = 0;

	qedi_ep = ep->dd_data;
	qedi = qedi_ep->qedi;

	flush_work(&qedi_ep->offload_work);

	if (qedi_ep->conn) {
		qedi_conn = qedi_ep->conn;
		conn = qedi_conn->cls_conn->dd_data;
		qedi_suspend_queue(conn);
		abrt_conn = qedi_conn->abrt_conn;

		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_INFO,
			  "cid=0x%x qedi_ep=%p waiting for %d tmfs\n",
			  qedi_ep->iscsi_cid, qedi_ep,
			  qedi_conn->fw_cleanup_works);

		spin_lock(&qedi_conn->tmf_work_lock);
		qedi_conn->ep_disconnect_starting = true;
		while (qedi_conn->fw_cleanup_works > 0) {
			spin_unlock(&qedi_conn->tmf_work_lock);
			msleep(1000);
			spin_lock(&qedi_conn->tmf_work_lock);
		}
		spin_unlock(&qedi_conn->tmf_work_lock);

		if (test_bit(QEDI_IN_RECOVERY, &qedi->flags)) {
			if (do_not_recover) {
				QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_INFO,
					  "Do not recover cid=0x%x\n",
					  qedi_ep->iscsi_cid);
				goto ep_exit_recover;
			}
			QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_INFO,
				  "Reset recovery cid=0x%x, qedi_ep=%p, state=0x%x\n",
				  qedi_ep->iscsi_cid, qedi_ep, qedi_ep->state);
			qedi_cleanup_active_cmd_list(qedi_conn);
			goto ep_release_conn;
		}
	}

	if (do_not_recover)
		goto ep_exit_recover;

	switch (qedi_ep->state) {
	case EP_STATE_OFLDCONN_START:
	case EP_STATE_OFLDCONN_NONE:
		goto ep_release_conn;
	case EP_STATE_OFLDCONN_FAILED:
			break;
	case EP_STATE_OFLDCONN_COMPL:
		if (unlikely(!qedi_conn))
			break;

		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_INFO,
			  "DBG1: Active cmd count=%d, abrt_conn=%d, ep state=0x%x, cid=0x%x, qedi_conn=%p\n",
			  qedi_conn->active_cmd_count, abrt_conn,
			  qedi_ep->state,
			  qedi_ep->iscsi_cid,
			  qedi_ep->conn
			  );

		if (!qedi_conn->active_cmd_count)
			abrt_conn = 0;
		else
			abrt_conn = 1;

		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_INFO,
			  "DBG2: Active cmd count=%d, abrt_conn=%d, ep state=0x%x, cid=0x%x, qedi_conn=%p\n",
			  qedi_conn->active_cmd_count, abrt_conn,
			  qedi_ep->state,
			  qedi_ep->iscsi_cid,
			  qedi_ep->conn
			  );
		if (abrt_conn)
			qedi_clearsq(qedi, qedi_conn, NULL);
		break;
	default:
		break;
	}

	/* graceful termination timeout */
	if (!abrt_conn)
		wait_delay += qedi->pf_params.iscsi_pf_params.two_msl_timer;

	qedi_ep->state = EP_STATE_DISCONN_START;

	if (test_bit(QEDI_IN_SHUTDOWN, &qedi->flags) ||
	    test_bit(QEDI_IN_RECOVERY, &qedi->flags))
		goto ep_release_conn;

	/* delete doorbell from doorbell recovery mechanism */
	ret = qedi_ops->common->db_recovery_del(qedi->cdev, qedi_ep->p_doorbell,
						&qedi_ep->db_data);

	ret = qedi_ops->destroy_conn(qedi->cdev, qedi_ep->handle, abrt_conn);
	if (ret) {
		QEDI_WARN(&qedi->dbg_ctx,
			  "destroy_conn failed returned %d\n", ret);
	} else {
		ret = wait_event_interruptible_timeout(
					qedi_ep->tcp_ofld_wait,
					(qedi_ep->state !=
					 EP_STATE_DISCONN_START),
					wait_delay);
		if ((ret <= 0) || (qedi_ep->state == EP_STATE_DISCONN_START)) {
			QEDI_WARN(&qedi->dbg_ctx,
				  "Destroy conn timedout or interrupted, ret=%d, delay=%d, cid=0x%x\n",
				  ret, wait_delay, qedi_ep->iscsi_cid);
		}
	}

ep_release_conn:
	ret = qedi_ops->release_conn(qedi->cdev, qedi_ep->handle);
	if (ret)
		QEDI_WARN(&qedi->dbg_ctx,
			  "release_conn returned %d, cid=0x%x\n",
			  ret, qedi_ep->iscsi_cid);
ep_exit_recover:
	qedi_ep->state = EP_STATE_IDLE;
	qedi->ep_tbl[qedi_ep->iscsi_cid] = NULL;
	qedi->cid_que.conn_cid_tbl[qedi_ep->iscsi_cid] = NULL;
	qedi_free_id(&qedi->lcl_port_tbl, qedi_ep->src_port);
	qedi_free_sq(qedi, qedi_ep);

	if (qedi_conn)
		qedi_conn->ep = NULL;

	qedi_ep->conn = NULL;
	qedi_ep->qedi = NULL;
	atomic_dec(&qedi->num_offloads);

	iscsi_destroy_endpoint(ep);
}

static int qedi_data_avail(struct qedi_ctx *qedi, u16 vlanid)
{
	struct qed_dev *cdev = qedi->cdev;
	struct qedi_uio_dev *udev;
	struct qedi_uio_ctrl *uctrl;
	struct sk_buff *skb;
	u32 len;
	int rc = 0;

	udev = qedi->udev;
	if (!udev) {
		QEDI_ERR(&qedi->dbg_ctx, "udev is NULL.\n");
		return -EINVAL;
	}

	uctrl = (struct qedi_uio_ctrl *)udev->uctrl;
	if (!uctrl) {
		QEDI_ERR(&qedi->dbg_ctx, "uctlr is NULL.\n");
		return -EINVAL;
	}

	len = uctrl->host_tx_pkt_len;
	if (!len) {
		QEDI_ERR(&qedi->dbg_ctx, "Invalid len %u\n", len);
		return -EINVAL;
	}

	skb = alloc_skb(len, GFP_ATOMIC);
	if (!skb) {
		QEDI_ERR(&qedi->dbg_ctx, "alloc_skb failed\n");
		return -EINVAL;
	}

	skb_put(skb, len);
	memcpy(skb->data, udev->tx_pkt, len);
	skb->ip_summed = CHECKSUM_NONE;

	if (vlanid)
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0))
		__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), vlanid);
#else
		__vlan_hwaccel_put_tag(skb, vlanid);
#endif

	rc = qedi_ops->ll2->start_xmit(cdev, skb, 0);
	if (rc) {
		QEDI_ERR(&qedi->dbg_ctx, "ll2 start_xmit returned %d\n",
			 rc);
		kfree_skb(skb);
	}

	uctrl->host_tx_pkt_len = 0;
	uctrl->hw_tx_cons++;

	return rc;
}

static int qedi_set_path(struct Scsi_Host *shost, struct iscsi_path *path_data)
{
	struct qedi_ctx *qedi;
	struct qedi_endpoint *qedi_ep;
	int ret = 0;
	u32 iscsi_cid;
	u16 port_id = 0;

	if (!shost) {
		ret = -ENXIO;
		QEDI_ERR(NULL, "shost is NULL\n");
		return ret;
	}

	if (strcmp(shost->hostt->proc_name, "qedi")) {
		ret = -ENXIO;
		QEDI_ERR(NULL, "shost %s is invalid\n",
			 shost->hostt->proc_name);
		return ret;
	}

	qedi = iscsi_host_priv(shost);
	if (path_data->handle == QEDI_PATH_HANDLE) {
		ret = qedi_data_avail(qedi, path_data->vlan_id);
		goto set_path_exit;
	}

	iscsi_cid = (u32)path_data->handle;
	qedi_ep = qedi->ep_tbl[iscsi_cid];

	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_CONN,
		  "iscsi_cid=0x%x, qedi_ep=%p\n", iscsi_cid, qedi_ep);
	if (!qedi_ep) {
		ret = -EINVAL;
		goto set_path_exit;
	}

	if (!is_valid_ether_addr(&path_data->mac_addr[0])) {
		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_INFO,
			  "Invalid mac [%pM]\n", &path_data->mac_addr[0]);
		qedi_ep->state = EP_STATE_OFLDCONN_NONE;
		ret = -EIO;
		goto set_path_exit;
	}

	ether_addr_copy(&qedi_ep->src_mac[0], &qedi->mac[0]);
	ether_addr_copy(&qedi_ep->dst_mac[0], &path_data->mac_addr[0]);

	qedi_ep->vlan_id = path_data->vlan_id;

	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_INFO,
		  "src [%pM] dst [%pM] vlan [%d]\n",
		  &qedi_ep->src_mac[0], &qedi_ep->dst_mac[0],
		  qedi_ep->vlan_id);

	if (qedi_validate_mtu(qedi, path_data))
		goto set_path_exit;

	if (qedi_ep->pmtu != qedi->ll2_mtu)
		qedi_ep->pmtu = qedi->ll2_mtu;

	port_id = qedi_ep->src_port;
	if (port_id >= QEDI_LOCAL_PORT_MIN &&
	    port_id < QEDI_LOCAL_PORT_MAX) {
		if (qedi_alloc_id(&qedi->lcl_port_tbl, port_id))
			port_id = 0;
	} else {
		port_id = 0;
	}

	if (!port_id) {
		port_id = qedi_alloc_new_id(&qedi->lcl_port_tbl);
		if (port_id == QEDI_LOCAL_PORT_INVALID) {
			QEDI_ERR(&qedi->dbg_ctx,
				 "Failed to allocate port id for iscsi_cid=0x%x\n",
				 iscsi_cid);
			ret = -ENOMEM;
			goto set_path_exit;
		}
	}

	qedi_ep->src_port = port_id;

	if (qedi_ep->ip_type == TCP_IPV4) {
		memcpy(&qedi_ep->src_addr[0], &path_data->src.v4_addr,
		       sizeof(struct in_addr));
		memcpy(&qedi->src_ip[0], &path_data->src.v4_addr,
		       sizeof(struct in_addr));
		qedi->ip_type = TCP_IPV4;

		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_INFO,
			  "src addr:port=%pI4:%u, dst addr:port=%pI4:%u\n",
			  qedi_ep->src_addr, qedi_ep->src_port,
			  qedi_ep->dst_addr, qedi_ep->dst_port);
	} else {
		memcpy(&qedi_ep->src_addr[0], &path_data->src.v6_addr,
		       sizeof(struct in6_addr));
		memcpy(&qedi->src_ip[0], &path_data->src.v6_addr,
		       sizeof(struct in6_addr));
		qedi->ip_type = TCP_IPV6;

		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_INFO,
			  "src addr:port=%pI6:%u, dst addr:port=%pI6:%u\n",
			  qedi_ep->src_addr, qedi_ep->src_port,
			  qedi_ep->dst_addr, qedi_ep->dst_port);
	}

	queue_work(qedi->offload_thread, &qedi_ep->offload_work);

	ret = 0;

set_path_exit:
	return ret;
}

#if defined UMODE_T_USED
static umode_t qedi_attr_is_visible(int param_type, int param)
#else
static mode_t qedi_attr_is_visible(int param_type, int param)
#endif
{
	switch (param_type) {
	case ISCSI_HOST_PARAM:
		switch (param) {
		case ISCSI_HOST_PARAM_NETDEV_NAME:
		case ISCSI_HOST_PARAM_HWADDRESS:
		case ISCSI_HOST_PARAM_IPADDRESS:
			return S_IRUGO;
		default:
			return 0;
		}
	case ISCSI_PARAM:
		switch (param) {
		case ISCSI_PARAM_MAX_RECV_DLENGTH:
		case ISCSI_PARAM_MAX_XMIT_DLENGTH:
		case ISCSI_PARAM_HDRDGST_EN:
		case ISCSI_PARAM_DATADGST_EN:
		case ISCSI_PARAM_CONN_ADDRESS:
		case ISCSI_PARAM_CONN_PORT:
		case ISCSI_PARAM_EXP_STATSN:
		case ISCSI_PARAM_PERSISTENT_ADDRESS:
		case ISCSI_PARAM_PERSISTENT_PORT:
		case ISCSI_PARAM_PING_TMO:
		case ISCSI_PARAM_RECV_TMO:
		case ISCSI_PARAM_INITIAL_R2T_EN:
		case ISCSI_PARAM_MAX_R2T:
		case ISCSI_PARAM_IMM_DATA_EN:
		case ISCSI_PARAM_FIRST_BURST:
		case ISCSI_PARAM_MAX_BURST:
		case ISCSI_PARAM_PDU_INORDER_EN:
		case ISCSI_PARAM_DATASEQ_INORDER_EN:
		case ISCSI_PARAM_ERL:
		case ISCSI_PARAM_TARGET_NAME:
		case ISCSI_PARAM_TPGT:
		case ISCSI_PARAM_USERNAME:
		case ISCSI_PARAM_PASSWORD:
		case ISCSI_PARAM_USERNAME_IN:
		case ISCSI_PARAM_PASSWORD_IN:
		case ISCSI_PARAM_FAST_ABORT:
		case ISCSI_PARAM_ABORT_TMO:
		case ISCSI_PARAM_LU_RESET_TMO:
		case ISCSI_PARAM_TGT_RESET_TMO:
		case ISCSI_PARAM_IFACE_NAME:
		case ISCSI_PARAM_INITIATOR_NAME:
#ifdef ISCSI_PARAM_BOOT
		case ISCSI_PARAM_BOOT_ROOT:
		case ISCSI_PARAM_BOOT_NIC:
		case ISCSI_PARAM_BOOT_TARGET:
#endif
			return S_IRUGO;
		default:
			return 0;
		}
	}

	return 0;
}

static void qedi_cleanup_task(struct iscsi_task *task)
{
	struct qedi_cmd *cmd;

	if (task->state == ISCSI_TASK_PENDING) {
		QEDI_INFO(NULL, QEDI_LOG_IO, "Returning ref_cnt=%d\n",
			/*
			 * iscsi_task refcount from atomic_t to refcount_t
			 * was done in 4.12 however in 4.11 we refcount_read
			 * exists so we need to do this ifdef based on kernel
			 * version and not whether refcount_read exists.
			 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
			atomic_read(&task->refcount));
#else
			refcount_read(&task->refcount));
#endif
		return;
	}

	if (task->sc)
		qedi_iscsi_unmap_sg_list(task->dd_data);

	cmd = task->dd_data;
	if ((cmd->task_id != U16_MAX)  && (cmd->io_cmd_in_list == false)) {
		qedi_clear_task_idx(iscsi_host_priv(task->conn->session->host),
				    cmd->task_id);

		cmd->task_id = U16_MAX;
		cmd->scsi_cmd = NULL;
	}
}

struct iscsi_transport qedi_iscsi_transport = {
	.owner = THIS_MODULE,
	.name = QEDI_MODULE_NAME,
	.caps = CAP_RECOVERY_L0 | CAP_HDRDGST | CAP_MULTI_R2T | CAP_DATADGST |
		CAP_DATA_PATH_OFFLOAD | CAP_TEXT_NEGO,
	.create_session = qedi_session_create,
	.destroy_session = qedi_session_destroy,
	.create_conn = qedi_conn_create,
	.bind_conn = qedi_conn_bind,
#if defined UNBIND_CONN
	.unbind_conn = iscsi_conn_unbind,
#endif
	.start_conn = qedi_conn_start,
	.stop_conn = iscsi_conn_stop,
	.destroy_conn = qedi_conn_destroy,
	.set_param = iscsi_set_param,
	.get_ep_param = qedi_ep_get_param,
	.get_conn_param = iscsi_conn_get_param,
	.get_session_param = iscsi_session_get_param,
	.get_host_param = qedi_host_get_param,
	.send_pdu = iscsi_conn_send_pdu,
	.get_stats = qedi_conn_get_stats,
	.xmit_task = qedi_task_xmit,
	.cleanup_task = qedi_cleanup_task,
	.session_recovery_timedout = iscsi_session_recovery_timedout,
	.ep_connect = qedi_ep_connect,
	.ep_poll = qedi_ep_poll,
	.ep_disconnect = qedi_ep_disconnect,
	.set_path = qedi_set_path,
	.attr_is_visible = qedi_attr_is_visible,
};

void qedi_start_conn_recovery(struct qedi_ctx *qedi,
			      struct qedi_conn *qedi_conn)
{
	struct iscsi_cls_session *cls_sess;
	struct iscsi_cls_conn *cls_conn;
	struct iscsi_conn *conn;

	cls_conn = qedi_conn->cls_conn;
	conn = cls_conn->dd_data;
	cls_sess = iscsi_conn_to_session(cls_conn);

	if (iscsi_is_session_online(cls_sess)) {
		qedi_conn->abrt_conn = 1;
		QEDI_ERR(&qedi->dbg_ctx,
			 "Failing connection, state=0x%x, cid=0x%x\n",
			 conn->session->state, qedi_conn->iscsi_conn_id);
		iscsi_conn_failure(qedi_conn->cls_conn->dd_data,
				   ISCSI_ERR_CONN_FAILED);
	}
}

void qedi_process_iscsi_error(struct qedi_endpoint *ep, struct iscsi_eqe_data *data)
{
	struct qedi_conn *qedi_conn;
	struct qedi_ctx *qedi;
	char warn_notice[] = "iscsi_warning";
	char error_notice[] = "iscsi_error";
	char *message;
	u32 need_recovery = 0;
	u32 err_mask = 0;
	char msg[64];

	if (!ep)
		return;

	qedi_conn = ep->conn;
	if (!qedi_conn)
		return;

	qedi = ep->qedi;

	QEDI_ERR(&qedi->dbg_ctx, "async event iscsi error:0x%x\n",
		 data->error_code);

	if (err_mask) {
		need_recovery = 0;
		message = warn_notice;
	} else {
		need_recovery = 1;
		message = error_notice;
	}

	switch (data->error_code) {
	case ISCSI_STATUS_NONE:
		strcpy(msg, "tcp_error none");
		break;
	case ISCSI_CONN_ERROR_TASK_CID_MISMATCH:
		strcpy(msg, "task cid mismatch");
		break;
	case ISCSI_CONN_ERROR_TASK_NOT_VALID:
		strcpy(msg, "invalid task");
		break;
	case ISCSI_CONN_ERROR_RQ_RING_IS_FULL:
		strcpy(msg, "rq ring full");
		break;
	case ISCSI_CONN_ERROR_CMDQ_RING_IS_FULL:
		strcpy(msg, "cmdq ring full");
		break;
	case ISCSI_CONN_ERROR_HQE_CACHING_FAILED:
		strcpy(msg, "sge caching failed");
		break;
	case ISCSI_CONN_ERROR_HEADER_DIGEST_ERROR:
		strcpy(msg, "hdr digest error");
		break;
	case ISCSI_CONN_ERROR_LOCAL_COMPLETION_ERROR:
		strcpy(msg, "local cmpl error");
		break;
	case ISCSI_CONN_ERROR_DATA_OVERRUN:
		strcpy(msg, "invalid task");
		break;
	case ISCSI_CONN_ERROR_OUT_OF_SGES_ERROR:
		strcpy(msg, "out of sge error");
		break;
	case ISCSI_CONN_ERROR_IP_OPTIONS_ERROR:
		strcpy(msg, "tcp seg ip options error");
		break;
	case ISCSI_CONN_ERROR_TCP_IP_FRAGMENT_ERROR:
		strcpy(msg, "tcp ip fragment error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_AHS_LEN:
		strcpy(msg, "AHS len protocol error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_ITT_OUT_OF_RANGE:
		strcpy(msg, "itt out of range error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_DATA_SEG_LEN_EXCEEDS_PDU_SIZE:
		strcpy(msg, "data seg more than pdu size");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_INVALID_OPCODE:
		strcpy(msg, "invalid opcode");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_INVALID_OPCODE_BEFORE_UPDATE:
		strcpy(msg, "invalid opcode before update");
		break;
	case ISCSI_CONN_ERROR_UNVALID_NOPIN_DSL:
		strcpy(msg, "unexpected opcode");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_R2T_CARRIES_NO_DATA:
		strcpy(msg, "r2t carries no data");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_DATA_SN:
		strcpy(msg, "data sn error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_DATA_IN_TTT:
		strcpy(msg, "data TTT error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_R2T_TTT:
		strcpy(msg, "r2t TTT error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_R2T_BUFFER_OFFSET:
		strcpy(msg, "buffer offset error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_BUFFER_OFFSET_OOO:
		strcpy(msg, "buffer offset ooo");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_R2T_SN:
		strcpy(msg, "data seg len 0");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_DESIRED_DATA_TRNS_LEN_0:
		strcpy(msg, "data xer len error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_DESIRED_DATA_TRNS_LEN_1:
		strcpy(msg, "data xer len1 error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_DESIRED_DATA_TRNS_LEN_2:
		strcpy(msg, "data xer len2 error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_LUN:
		strcpy(msg, "protocol lun error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_F_BIT_ZERO:
		strcpy(msg, "f bit zero error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_F_BIT_ZERO_S_BIT_ONE:
		strcpy(msg, "f bit zero s bit one error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_EXP_STAT_SN:
		strcpy(msg, "exp stat sn error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_DSL_NOT_ZERO:
		strcpy(msg, "dsl not zero error");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_INVALID_DSL:
		strcpy(msg, "invalid dsl");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_DATA_SEG_LEN_TOO_BIG:
		strcpy(msg, "data seg len too big");
		break;
	case ISCSI_CONN_ERROR_PROTOCOL_ERR_OUTSTANDING_R2T_COUNT:
		strcpy(msg, "outstanding r2t count error");
		break;
	case ISCSI_CONN_ERROR_SENSE_DATA_LENGTH:
		strcpy(msg, "sense datalen error");
		break;
	case ISCSI_ERROR_UNKNOWN:
	default:
		need_recovery = 0;
		strcpy(msg, "unknown error");
		break;
	}
	iscsi_conn_printk(KERN_ALERT,
			  qedi_conn->cls_conn->dd_data,
			  "qedi: %s - %s\n", message, msg);

	if (need_recovery)
		qedi_start_conn_recovery(qedi_conn->qedi, qedi_conn);
}

void qedi_process_tcp_error(struct qedi_endpoint *ep, struct iscsi_eqe_data *data)
{
	struct qedi_conn *qedi_conn;

	if (!ep)
		return;

	qedi_conn = ep->conn;
	if (!qedi_conn)
		return;

	QEDI_ERR(&ep->qedi->dbg_ctx, "async event TCP error:0x%x\n",
		 data->error_code);

	qedi_start_conn_recovery(qedi_conn->qedi, qedi_conn);
}
