/*
 * Copyright (c) 2014-2015 Freescale Semiconductor, Inc. All rights reserved.
 *
 */

/*!
 * @file	nadk_eth_ldpaa.c
 *
 * @brief	Ethernet driver implementation. It contains initialization of
 *		network interface for NADK device framework based application.
 *
 * @addtogroup	NADK_ETH
 * @ingroup	NADK_DEV
 * @{
 */

/*Standard header files*/
#include <pthread.h>

/*NADK header files*/
#include <odp/std_types.h>
#include <nadk_common.h>
#include <nadk_dev.h>
#include <nadk_dev_priv.h>
#include <nadk_io_portal_priv.h>
#include "nadk_eth_priv.h"
#include "nadk_vq.h"
#include <nadk_eth_ldpaa_annot.h>
#include <nadk_eth_ldpaa_qbman.h>
#include <nadk_mbuf.h>
#include <nadk_mbuf_priv.h>
#include <nadk_malloc.h>
#include <odp/byteorder.h>
#include <nadk_conc_priv.h>
#include <nadk_dev_notif.h>
#include <nadk_dev_notif_priv.h>
#include <nadk_memconfig.h>
#include <odp/hints.h>
#include <odp/config.h>
#include <odp_debug_internal.h>
#include <odp_align_internal.h>
#include <odp_packet_internal.h>

/*MC header files*/
#include <fsl_dpni.h>
#include <fsl_dpni_cmd.h>
#include <fsl_mc_sys.h>

#define NADK_ASAL_VAL (NADK_MBUF_HW_ANNOTATION / 64)

#define LDPAA_ETH_DEV_VENDOR_ID		6487
#define LDPAA_ETH_DEV_MAJ_NUM		DPNI_VER_MAJOR
#define LDPAA_ETH_DEV_MIN_NUM		DPNI_VER_MINOR
#define LDPAA_ETH_DEV_NAME		"ldpaa-ethernet"

/* Number of frames to be received in SC mode */
#define MAX_NUM_RECV_FRAMES	16
/* Short Circuit the Ethernet Driver */
bool eth_short_circuit;
/* Signal caching variable for SC mode */
bool eth_sc_sigint;

int32_t nadk_mbuf_sw_annotation;

struct nadk_driver eth_driver = {
	.name			=	LDPAA_ETH_DEV_NAME,
	.vendor_id		=	LDPAA_ETH_DEV_VENDOR_ID,
	.major			=	LDPAA_ETH_DEV_MAJ_NUM,
	.minor			=	LDPAA_ETH_DEV_MIN_NUM,
	.dev_type		=	NADK_NIC,
	.dev_probe		=	nadk_eth_probe,
	.dev_shutdown		=	nadk_eth_remove
};

/*Ethernet spcific statistics objects*/
#ifdef NADK_DEBUG_XSTATS
struct nadk_eth_xstats xstats;
#endif

static inline void nadk_eth_mbuf_to_fd(
		nadk_mbuf_pt mbuf,
		struct qbman_fd *fd);

void *nadk_eth_cb_dqrr_fd_to_mbuf(
		struct qbman_swp *qm,
		const struct qbman_fd *fd,
		const struct qbman_result *dqrr);

void *nadk_eth_cb_dqrr_tx_conf_err(
		struct qbman_swp *qm,
		const struct qbman_fd *fd,
		const struct qbman_result *dqrr);

int32_t nadk_eth_driver_init(void)
{
	/*Register Ethernet driver to NADK device framework*/
	nadk_register_driver(&eth_driver);
	return NADK_SUCCESS;
}

int32_t nadk_eth_driver_exit(void)
{
	/*Unregister Ethernet driver to NADK device framework*/
	nadk_unregister_driver(&eth_driver);
	return NADK_SUCCESS;
}

int32_t nadk_eth_probe(struct nadk_dev *dev,
			const void *data)
{
	/*Probe function is responsible to initialize the DPNI devices.
	 * It does the following:
	 * 1. Register device specific callbacks to NADK device framework
	 * 2. Allocate memory for RX/TX VQ's and assign into NADL device
	 *	structure.
	 * 3. Assigns available resource information into NADK device
	 *	structure.
	 */
	struct nadk_dev_priv *dev_priv =
				(struct nadk_dev_priv *)dev->priv;
	struct nadk_eth_priv *eth_priv;
	struct fsl_mc_io *dpni_dev;
	struct dpni_attr attr;
	int32_t retcode;
	int16_t i;
	struct nadk_vq *vq_mem;
	uint8_t bcast_addr[ETH_ADDR_LEN];
	uint8_t mac_addr[ETH_ADDR_LEN];
	struct dpni_buffer_layout layout;
	struct queues_config *q_config;
	const struct nadk_init_cfg *cfg = (const struct nadk_init_cfg *)data;
	struct vfio_device_info *obj_info =
		(struct vfio_device_info *)dev_priv->drv_priv;
	struct dpni_extended_cfg *ext_cfg = NULL;

	/*Allocate space for device specific data*/
	eth_priv = (struct nadk_eth_priv *)nadk_calloc(NULL, 1,
		sizeof(struct nadk_eth_priv) + sizeof(struct nadk_vq) *
		(MAX_RX_VQS + MAX_TX_VQS + MAX_ERR_VQS + MAX_DEF_ERR_VQS), 0);
	if (!eth_priv) {
		NADK_ERR(ETH, "Failure to allocate the memory for ethernet"
							"private data\n");
		return NADK_FAILURE;
	}

	/*Assigning RX/TX VQs to NADK device structure*/
	vq_mem = (struct nadk_vq *)(eth_priv + 1);
	for (i = 0; i < MAX_RX_VQS; i++) {
		vq_mem->dev = dev;
		dev->rx_vq[i] = vq_mem++;
	}
	for (i = 0; i < MAX_TX_VQS; i++) {
		vq_mem->dev = dev;
		dev->tx_vq[i] = vq_mem++;
	};
	for (i = 0; i < MAX_ERR_VQS + MAX_DEF_ERR_VQS; i++) {
		vq_mem->dev = dev;
		dev->err_vq[i] = vq_mem++;
	}

	/*Configure device specific callbacks to the NADK framework*/
	dev_priv->fn_dev_start	 = nadk_eth_start;
	dev_priv->fn_dev_stop	 = nadk_eth_stop;
	dev_priv->fn_dev_rcv	 = nadk_eth_recv;
	dev_priv->fn_dev_send	 = nadk_eth_xmit;
	dev_priv->fn_setup_rx_vq = nadk_eth_setup_rx_vq;
	dev_priv->fn_setup_tx_vq = nadk_eth_setup_tx_vq;
	dev_priv->fn_set_rx_vq_notif = nadk_eth_set_rx_vq_notification;
	dev_priv->fn_get_eventfd_from_vq = nadk_eth_get_eventfd_from_vq;
	dev_priv->fn_get_vqid	 = nadk_eth_get_fqid;
	dev_priv->drv_priv	 = eth_priv;

	if (cfg->flags & NADK_PREFETCH_MODE)
		dev_priv->fn_dev_rcv	 = nadk_eth_prefetch_recv;
	if (eth_short_circuit)
		dev_priv->fn_dev_rcv	 = nadk_eth_loopback;

	/* Get the interrupts for Ethernet device */
	retcode = nadk_get_interrupt_info(dev_priv->vfio_fd,
			obj_info, &(dev_priv->intr_handle));
	if (retcode != NADK_SUCCESS) {
		NADK_ERR(FW, "Unable to get interrupt information\n");
		goto mem_alloc_failure;
	};

	/*Open the nadk device via MC and save the handle for further use*/
	dpni_dev = (struct fsl_mc_io *)nadk_calloc(NULL, 1,
						sizeof(struct fsl_mc_io), 0);
	if (!dpni_dev) {
		NADK_ERR(ETH, "Error in allocating the memory\n");
		goto mem_alloc_failure;
	}
	dpni_dev->regs = dev_priv->mc_portal;
	retcode = dpni_open(dpni_dev, CMD_PRI_LOW, dev_priv->hw_id, &(dev_priv->token));
	if (retcode != 0) {
		NADK_ERR(ETH, "Cannot open the device %s: Error Code = %0x\n",
						dev->dev_string, retcode);
		goto dev_open_failure;
	}
#ifndef QODP_464
	/* Reset the DPNI before use. It's a workaround to
	   enable Stashing via MC configuration */
	retcode = dpni_reset(dpni_dev, CMD_PRI_LOW, dev_priv->token);
	if (retcode)
		NADK_ERR(ETH, "Error in Resetting the DPNI"
				" : ErrorCode = %d\n", retcode);
#endif
	ext_cfg = (struct dpni_extended_cfg *)nadk_data_malloc(NULL, 256,
							 ODP_CACHE_LINE_SIZE);
	if (!ext_cfg) {
		NADK_ERR(ETH, "No data memory\n");
		return NADK_FAILURE;
	}
	attr.ext_cfg_iova = (uint64_t)(NADK_VADDR_TO_IOVA(ext_cfg));

	/*Get the resource information i.e. numner of RX/TX Queues, TC etc*/
	retcode = dpni_get_attributes(dpni_dev, CMD_PRI_LOW, dev_priv->token, &attr);
	if (retcode) {
		NADK_ERR(ETH, "DPNI get attribute failed: Error Code = %0x\n",
								retcode);
		goto get_attr_failure;
	}
	q_config = &(eth_priv->q_config);
	q_config->num_tcs = attr.max_tcs;
	dev->num_tx_vqueues = attr.max_senders;
	dev->num_rx_vqueues = 0;

	for (i = 0; i < attr.max_tcs; i++) {
		q_config->tc_config[i].num_dist =
					ext_cfg->tc_cfg[i].max_dist;
		dev->num_rx_vqueues += q_config->tc_config[i].num_dist;
	}

	NADK_INFO(ETH, "TX VQ = %d\t RX VQ = %d\n",
		dev->num_tx_vqueues, dev->num_rx_vqueues);
	dev_priv->hw = dpni_dev;
	retcode = dpni_get_primary_mac_addr(dpni_dev, CMD_PRI_LOW, dev_priv->token, mac_addr);
	if (retcode) {
		NADK_ERR(ETH, "DPNI get mac address failed:"
					" Error Code = %d\n", retcode);
		goto get_attr_failure;
	}
	sprintf(dev->dev_string, "dpni-%u", dev_priv->hw_id);
	sprintf((char *)eth_priv->cfg.name, "dpni-%u", dev_priv->hw_id);
	memcpy(eth_priv->cfg.mac_addr, mac_addr, ETH_ADDR_LEN);
	/* driver may only return MTU in case of IPF/IPR offload support
	 * otherwise it will return 0 in all other cases*/
	eth_priv->cfg.mtu = nadk_eth_mtu_get(dev);
	if (0 == eth_priv->cfg.mtu) {
		/* Using Default value */
		eth_priv->cfg.mtu = ETH_MTU;
		retcode = nadk_eth_mtu_set(dev, eth_priv->cfg.mtu);
		if (retcode < 0) {
			NADK_ERR(ETH, "Fail to set MTU %d\n", retcode);
			goto get_attr_failure;
		}
	}

	/* WRIOP don't accept packets with broadcast address by default,
	   So adding rule entry for same. */
	NADK_INFO(ETH, "Adding Broadcast Address...\n");
	memset(bcast_addr, 0xff, ETH_ADDR_LEN);
	retcode = dpni_add_mac_addr(dpni_dev, CMD_PRI_LOW, dev_priv->token, bcast_addr);
	if (retcode) {
		NADK_ERR(ETH, "DPNI set broadcast mac address failed:"
					" Error Code = %0x\n", retcode);
		goto get_attr_failure;
	}

	/*Configure WRIOP to provide parse results, frame annoatation status and
	timestamp*/

	/*if  headroom is already initialized in the previous device probe*/
	if (!nadk_mbuf_head_room) {
		uint32_t tot_size;
		/* ... rx buffer layout ... */
		nadk_mbuf_sw_annotation = NADK_FD_PTA_SIZE;
		nadk_mbuf_head_room	= ODP_CONFIG_PACKET_HEADROOM;

		/*Check alignment for buffer layouts first*/
		tot_size = nadk_mbuf_sw_annotation + NADK_MBUF_HW_ANNOTATION +
							nadk_mbuf_head_room;
		tot_size = ODP_ALIGN_ROUNDUP(tot_size, ODP_PACKET_LAYOUT_ALIGN);
		nadk_mbuf_head_room = tot_size - (nadk_mbuf_sw_annotation +
						NADK_MBUF_HW_ANNOTATION);
	}
	memset(&layout, 0, sizeof(struct dpni_buffer_layout));
	layout.options = DPNI_BUF_LAYOUT_OPT_FRAME_STATUS |
				DPNI_BUF_LAYOUT_OPT_TIMESTAMP |
				DPNI_BUF_LAYOUT_OPT_PARSER_RESULT |
				DPNI_BUF_LAYOUT_OPT_DATA_HEAD_ROOM |
				DPNI_BUF_LAYOUT_OPT_PRIVATE_DATA_SIZE;
	layout.pass_frame_status = TRUE;
	layout.data_head_room = nadk_mbuf_head_room;
	layout.private_data_size = nadk_mbuf_sw_annotation;
	layout.pass_timestamp = TRUE;
	layout.pass_parser_result = TRUE;
	retcode = dpni_set_rx_buffer_layout(dpni_dev, CMD_PRI_LOW,
			dev_priv->token, &layout);
	if (retcode) {
		NADK_ERR(ETH, "Error (%d) in setting rx buffer layout\n",
								retcode);
		goto get_attr_failure;
	}

	/* ... tx buffer layout ... */
	memset(&layout, 0, sizeof(struct dpni_buffer_layout));
	layout.options = DPNI_BUF_LAYOUT_OPT_FRAME_STATUS |
				DPNI_BUF_LAYOUT_OPT_TIMESTAMP;
	layout.pass_frame_status = TRUE;
	layout.pass_timestamp = TRUE;
	retcode = dpni_set_tx_buffer_layout(dpni_dev, CMD_PRI_LOW, dev_priv->token,
								&layout);
	if (retcode) {
		NADK_ERR(ETH, "Error (%d) in setting tx buffer layout\n",
								retcode);
		goto get_attr_failure;
	}
	/* ... tx-conf and error buffer layout ... */
	memset(&layout, 0, sizeof(struct dpni_buffer_layout));
	layout.options = DPNI_BUF_LAYOUT_OPT_FRAME_STATUS |
				DPNI_BUF_LAYOUT_OPT_TIMESTAMP;
	layout.pass_frame_status = TRUE;
	layout.pass_timestamp = TRUE;
	retcode = dpni_set_tx_conf_buffer_layout(dpni_dev, CMD_PRI_LOW, dev_priv->token,
								&layout);
	if (retcode) {
		NADK_ERR(ETH, "Error (%d) in setting tx-conf buffer layout\n",
								retcode);
		goto get_attr_failure;
	}

	nadk_data_free(ext_cfg);
	return NADK_SUCCESS;

get_attr_failure:
		dpni_close(dpni_dev, CMD_PRI_LOW, dev_priv->token);
dev_open_failure:
		nadk_free(dpni_dev);
mem_alloc_failure:
		nadk_data_free(ext_cfg);
		nadk_free(eth_priv);
		return NADK_FAILURE;
}

int32_t nadk_eth_remove(struct nadk_dev *dev)
{
	/*Function is reverse of nadk_eth_probe.
	 * It does the following:
	 * 1. Detach a DPNI from attached resources i.e. buffer pools, dpbp_id.
	 * 2. Close the DPNI device
	 * 3. Free the allocated reqources.
	 */
	struct nadk_dev_priv *dev_priv = dev->priv;
	struct nadk_eth_priv *eth_priv = dev_priv->drv_priv;
	struct fsl_mc_io *dpni = dev_priv->hw;
	int32_t retcode;

	/* Reset the DPNI device object for next use */
	retcode = dpni_reset(dpni, CMD_PRI_LOW, dev_priv->token);
	if (retcode != 0)
		NADK_ERR(ETH, "Error in Resetting the Ethernet"
				" device: ErrorCode = %d\n", retcode);
	/*Close the device at underlying layer*/
	retcode = dpni_close(dpni, CMD_PRI_LOW, dev_priv->token);
	if (retcode != 0)
		NADK_ERR(ETH, "Error in closing the Ethernet"
				" device: ErrorCode = %d\n", retcode);
	/*Free the allocated memory for ethernet private data and dpni*/
	nadk_free(eth_priv);
	nadk_free(dpni);

	return NADK_SUCCESS;
}

int32_t nadk_eth_start(struct nadk_dev *dev)
{
	/* Function is responsible to create underlying resources and to
	 * to make device ready to use for RX/TX.
	 */
	struct nadk_dev_priv *dev_priv = dev->priv;
	struct fsl_mc_io *dpni = (struct fsl_mc_io *)dev_priv->hw;
	struct nadk_eth_priv *eth_priv = dev_priv->drv_priv;
	struct dpni_queue_attr cfg;
	int32_t retcode;
	uint8_t tc_idx;
	uint16_t qdid, dist_idx;
	uint32_t vq_id;
	struct nadk_vq *eth_rx_vq;
	struct queues_config *q_config;
	uint16_t num_flows;

	/* After enabling a DPNI, Resources i.e. RX/TX VQs etc will be created
	 * and device will be ready for RX/TX.*/
	retcode = dpni_enable(dpni, CMD_PRI_LOW, dev_priv->token);
	if (retcode != 0) {
		NADK_ERR(ETH, "Error in enabling the DPNI to underlying layer"
						"Error code = %0x\n", retcode);
		return NADK_FAILURE;
	}

	q_config = &(eth_priv->q_config);
	/*Save the RX/TX flow information in NADK device structure*/
	for (tc_idx = 0; tc_idx < q_config->num_tcs; tc_idx++) {
		if (q_config->tc_config[tc_idx].dist_type == NADK_ETH_FLOW_DIST)
			num_flows = q_config->tc_config[tc_idx].num_dist_used;
		else
			num_flows = 1;

		for (dist_idx = 0; dist_idx <	num_flows; dist_idx++) {
			retcode = dpni_get_rx_flow(dpni, CMD_PRI_LOW, dev_priv->token,
						tc_idx, dist_idx, &cfg);
			if (retcode) {
				NADK_ERR(ETH, "Error to get flow information"
						"Error code = %0x\n", retcode);
				goto failure;
			}
			vq_id = (tc_idx * MAX_DIST_PER_TC) + dist_idx;
			eth_rx_vq = (struct nadk_vq *)(dev->rx_vq[vq_id]);
			eth_rx_vq->fqid = cfg.fqid;
			NADK_INFO(ETH, "FQID = %d\n", cfg.fqid);
		}
	}
	/*Save the respective qdid of DPNI device into NADK device structure*/
	retcode = dpni_get_qdid(dpni, CMD_PRI_LOW, dev_priv->token, &qdid);
	if (retcode != 0) {
		NADK_ERR(ETH, "Error to get qdid:ErrorCode = %d\n", retcode);
		goto failure;
	}
	dev_priv->qdid = qdid;
	NADK_INFO(ETH, "QDID = %d\n", qdid);

	/*All Well. Set the device as Active*/
	dev->state = DEV_ACTIVE;

	return NADK_SUCCESS;

failure:
	/*Disable the device which is enabled before*/
	dpni_disable(dpni, CMD_PRI_LOW, dev_priv->token);
	return NADK_FAILURE;
}

int32_t nadk_eth_stop(struct nadk_dev *dev)
{
	int32_t retcode;
	struct nadk_dev_priv *dev_priv = dev->priv;
	struct fsl_mc_io *dpni = (struct fsl_mc_io *)dev_priv->hw;

	/* Disable the network interface and set nadk device as inactive*/
	retcode = dpni_disable(dpni, CMD_PRI_LOW, dev_priv->token);
	if (retcode != 0) {
		NADK_ERR(ETH, "Device cannot be disabled:Error Code = %0x\n",
								retcode);
		return NADK_FAILURE;
	}
	/*Set device as inactive*/
	dev->state = DEV_INACTIVE;
	return NADK_SUCCESS;
}

int32_t nadk_eth_prefetch_recv(ODP_UNUSED struct nadk_dev *dev,
			void *vq,
			uint32_t num,
			nadk_mbuf_pt mbuf[])
{
	/* Function is responsible to receive frames for a given device and VQ*/
	struct nadk_vq *eth_rx_vq = (struct nadk_vq *)(vq);
	uint32_t fqid = eth_rx_vq->fqid;
	struct qbman_swp *swp = thread_io_info.dpio_dev->sw_portal;
	struct qbman_result *dq_storage;
	uint8_t is_last = 0, status;
	const struct qbman_fd *fd;
	struct qbman_pull_desc pulldesc;
	uint32_t rcvd_pkts = 0;

	if (eth_rx_vq->toggle == -1) {
		eth_rx_vq->toggle = 0;
		eth_rx_vq->dqrr_idx = 0;
		dq_storage = eth_rx_vq->dq_storage[eth_rx_vq->toggle];
		qbman_pull_desc_clear(&pulldesc);
		qbman_pull_desc_set_numframes(&pulldesc, MAX_NUM_RECV_FRAMES);
		qbman_pull_desc_set_fq(&pulldesc, fqid);
		qbman_pull_desc_set_storage(&pulldesc, dq_storage,
			(dma_addr_t)dq_storage, TRUE);

		while (1) {
			if (qbman_swp_pull(swp, &pulldesc)) {
				NADK_WARN(ETH, "VDQ command is not issued....QBMAN is busy\n");
				/* Portal was busy, try again */
				continue;
			}
			break;
		};
	}

	dq_storage = eth_rx_vq->dq_storage[eth_rx_vq->toggle] +
			eth_rx_vq->dqrr_idx;
	/* Recieve the packets till Last Dequeue entry is found with
	   respect to the above issues PULL command. */
	while (!is_last && rcvd_pkts < num) {
		/* Loop until the dq_storage is updated with
		 * new token by QBMAN */
		while (!qbman_result_has_new_result(swp, dq_storage))
			;

		/* Check whether Last Pull command is Expired and
		setting Condition for Loop termination */
		if (qbman_result_DQ_is_pull_complete(dq_storage)) {
			is_last = 1;
			/* Check for valid frame. */
			status = (uint8_t)qbman_result_DQ_flags(dq_storage);
			if (odp_unlikely((status & QBMAN_DQ_STAT_VALIDFRAME) == 0)) {
				NADK_INFO(ETH, "No frame is delivered\n");
				continue;
			}
		}

		/* Can avoid "qbman_result_is_DQ" check as
		   we are not expecting Notification on this SW-Portal */
		fd = qbman_result_DQ_fd(dq_storage);

		mbuf[rcvd_pkts] = eth_rx_vq->qmfq.cb(swp, fd, dq_storage);
		if (mbuf[rcvd_pkts])
			rcvd_pkts++;

		dq_storage++;
		eth_rx_vq->dqrr_idx++;
	} /* End of Packet Rx loop */

	NADK_INFO(ETH, "Ethernet Received %d Packets", rcvd_pkts);

	if (is_last) {
		eth_rx_vq->toggle ^= 1;
		eth_rx_vq->dqrr_idx = 0;
		dq_storage = eth_rx_vq->dq_storage[eth_rx_vq->toggle];
		qbman_pull_desc_clear(&pulldesc);
		qbman_pull_desc_set_numframes(&pulldesc, MAX_NUM_RECV_FRAMES);
		qbman_pull_desc_set_fq(&pulldesc, fqid);
		qbman_pull_desc_set_storage(&pulldesc, dq_storage,
			(dma_addr_t)dq_storage, TRUE);

		while (1) {
			if (qbman_swp_pull(swp, &pulldesc)) {
				NADK_WARN(ETH, "VDQ command is not issued....QBMAN is busy\n");
				/* Portal was busy, try again */
				continue;
			}
			break;
		};
	}

	/*Return the total number of packets received to NADK app*/
	return rcvd_pkts;
}

int32_t nadk_eth_recv(ODP_UNUSED struct nadk_dev *dev,
			void *vq,
			uint32_t num,
			nadk_mbuf_pt mbuf[])
{
	/* Function is responsible to receive frames for a given device and VQ*/
	struct nadk_vq *eth_vq = (struct nadk_vq *)vq;
	struct qbman_swp *swp = thread_io_info.dpio_dev->sw_portal;
	struct qbman_result *dq_storage = thread_io_info.dq_storage;
	uint8_t is_last = 0, status;
	const struct qbman_fd *fd;
	struct qbman_pull_desc pulldesc;
	int32_t rcvd_pkts = 0;

	nadk_qbman_pull_desc_set(&pulldesc, num, eth_vq->fqid, dq_storage);

	while (1) {
		if (qbman_swp_pull(swp, &pulldesc)) {
			NADK_WARN(ETH, "VDQ command is not issued....QBMAN is busy\n");
			/* Portal was busy, try again */
			continue;
		}
		break;
	};

	/* Recieve the packets till Last Dequeue entry is found with
	   respect to the above issues PULL command.
	 */
	while (!is_last) {
		/* Loop until the dq_storage is updated with
		 * new token by QBMAN */
		while (!qbman_result_has_new_result(swp, dq_storage))
			;

		/* Check whether Last Pull command is Expired and
		setting Condition for Loop termination */
		if (qbman_result_DQ_is_pull_complete(dq_storage)) {
			is_last = 1;
			/* Check for valid frame. */
			status = (uint8_t)qbman_result_DQ_flags(dq_storage);
			if (odp_unlikely((status & QBMAN_DQ_STAT_VALIDFRAME) == 0)) {
				NADK_INFO(ETH, "No frame is delivered\n");
				continue;
			}
		}

		/* Can avoid "qbman_result_is_DQ" check as
		   we are not expecting Notification on this SW-Portal */

		fd = qbman_result_DQ_fd(dq_storage);

		mbuf[rcvd_pkts] = eth_vq->qmfq.cb(swp, fd, dq_storage);
		if (mbuf[rcvd_pkts])
			rcvd_pkts++;

		dq_storage++;
	} /* End of Packet Rx loop */

	NADK_INFO(ETH, "Ethernet Received %d Packets", rcvd_pkts);
	/*Return the total number of packets received to NADK app*/
	return rcvd_pkts;
}

int32_t nadk_eth_xmit(struct nadk_dev *dev,
			void *vq,
			uint32_t num,
			nadk_mbuf_pt mbuf[])
{
	/* Function to transmit the frames to given device and VQ*/
	uint32_t loop;
	int32_t ret;
	struct qbman_fd fd;
	struct qbman_eq_desc eqdesc;
	uint64_t eq_storage_phys = NULL;
	struct nadk_dev_priv *dev_priv =
				(struct nadk_dev_priv *)dev->priv;
	struct qbman_swp *swp;
	struct nadk_vq *eth_tx_vq = (struct nadk_vq *)vq;

	/*Prepare enqueue descriptor*/
	qbman_eq_desc_clear(&eqdesc);
	qbman_eq_desc_set_no_orp(&eqdesc, NADK_EQ_RESP_ERR_FQ);
	qbman_eq_desc_set_response(&eqdesc, eq_storage_phys, 0);
	qbman_eq_desc_set_qd(&eqdesc, dev_priv->qdid,
			eth_tx_vq->flow_id, eth_tx_vq->tc_index);
	swp = thread_io_info.dpio_dev->sw_portal;

	/*Clear the unused FD fields before sending*/
	fd.simple.frc = 0;
	NADK_RESET_FD_CTRL((&fd));
	NADK_SET_FD_FLC((&fd), NULL);

	/*Prepare each packet which is to be sent*/
	for (loop = 0; loop < num; loop++) {
		/* Set DCA for freeing DQRR if required. We are saving
		   DQRR entry index in buffer when using DQRR mode.
		   The same need to be freed by H/W.
		*/
		if (ANY_ATOMIC_CNTXT_TO_FREE(mbuf[loop])) {
			qbman_eq_desc_set_dca(&eqdesc, 1, GET_HOLD_DQRR_IDX, 0);
			MARK_HOLD_DQRR_PTR_INVALID;
		}
		/*Convert nadk buffer into frame descriptor*/
		nadk_eth_mbuf_to_fd(mbuf[loop], &fd);

		/*Enqueue a packet to the QBMAN*/
		do {
			ret = qbman_swp_enqueue(swp, &eqdesc, &fd);
			if (ret != 0) {
				NADK_DBG(ETH, "Error in transmiting the frame\n");
			}
		} while (ret == -EBUSY);

		if (mbuf[loop]->flags & NADKBUF_ALLOCATED_SHELL)
			nadk_mbuf_free_shell(mbuf[loop]);
	}
	return loop;
}

int32_t nadk_eth_xmit_fqid(void *vq,
			uint32_t num,
			nadk_mbuf_pt mbuf[])
{
	/* Function to transmit the frames to given device and VQ*/
	uint32_t loop;
	int32_t ret;
	struct qbman_fd fd;
	struct qbman_eq_desc eqdesc;
	uint64_t eq_storage_phys = NULL;
	struct qbman_swp *swp;
	struct nadk_vq *eth_tx_vq = (struct nadk_vq *)vq;

	/*Prepare enqueue descriptor*/
	qbman_eq_desc_clear(&eqdesc);
	qbman_eq_desc_set_no_orp(&eqdesc, NADK_EQ_RESP_ERR_FQ);
	qbman_eq_desc_set_response(&eqdesc, eq_storage_phys, 0);
	qbman_eq_desc_set_fq(&eqdesc, eth_tx_vq->fqid);
	swp = thread_io_info.dpio_dev->sw_portal;

	/*Clear the unused FD fields before sending*/
	fd.simple.frc = 0;
	NADK_RESET_FD_CTRL((&fd));
	NADK_SET_FD_FLC((&fd), NULL);

	/*Prepare each packet which is to be sent*/
	for (loop = 0; loop < num; loop++) {

		/*Convert nadk buffer into frame descriptor*/
		nadk_eth_mbuf_to_fd(mbuf[loop], &fd);

		/*Enqueue a packet to the QBMAN*/
		do {
			ret = qbman_swp_enqueue(swp, &eqdesc, &fd);
			if (ret != 0) {
				NADK_DBG(ETH, "Error in transmiting the frame\n");
			}
		} while (ret == -EBUSY);

		if (mbuf[loop]->flags & NADKBUF_ALLOCATED_SHELL)
			nadk_mbuf_free_shell(mbuf[loop]);
	}
	return loop;
}

int32_t nadk_eth_loopback(struct nadk_dev *dev,
			void *vq,
			uint32_t num ODP_UNUSED,
			nadk_mbuf_pt mbuf[] ODP_UNUSED)
{
	struct nadk_dev_priv *dev_priv = (struct nadk_dev_priv *)dev->priv;
	uint32_t rx_fqid = ((struct nadk_vq *)vq)->fqid;
	struct nadk_vq *eth_tx_vq = (struct nadk_vq *)(dev->tx_vq[0]);
	struct qbman_swp *swp = thread_io_info.dpio_dev->sw_portal;
	struct qbman_pull_desc pulldesc1, pulldesc2;
	struct qbman_eq_desc eqdesc;
	const struct qbman_fd *fd;
	uint8_t is_last = 0, status;
	struct qbman_result *dq_storage;
	int stash = 1, dq_idx, ret;

	dq_storage = nadk_data_malloc(NULL, 2 * MAX_NUM_RECV_FRAMES *
		sizeof(struct qbman_result), ODP_CACHE_LINE_SIZE);
	if (!dq_storage) {
		NADK_ERR(ETH, "No memory");
		return NADK_FAILURE;
	}

	/* Prepare dequeue descriptors*/
	qbman_pull_desc_clear(&pulldesc1);
	qbman_pull_desc_set_numframes(&pulldesc1,
		MAX_NUM_RECV_FRAMES);
	qbman_pull_desc_set_fq(&pulldesc1, rx_fqid);
	qbman_pull_desc_set_storage(&pulldesc1,
		&(dq_storage[0]), (dma_addr_t)&(dq_storage[0]), stash);

	qbman_pull_desc_clear(&pulldesc2);
	qbman_pull_desc_set_numframes(&pulldesc2,
		MAX_NUM_RECV_FRAMES);
	qbman_pull_desc_set_fq(&pulldesc2, rx_fqid);
	qbman_pull_desc_set_storage(&pulldesc2,
		&(dq_storage[MAX_NUM_RECV_FRAMES]),
		(dma_addr_t)&(dq_storage[MAX_NUM_RECV_FRAMES]), stash);

	/* Prepare enqueue descriptor*/
	qbman_eq_desc_clear(&eqdesc);
	qbman_eq_desc_set_no_orp(&eqdesc, NADK_EQ_RESP_ERR_FQ);
	qbman_eq_desc_set_response(&eqdesc, 0, 0);
	qbman_eq_desc_set_qd(&eqdesc, dev_priv->qdid, eth_tx_vq->flow_id,
		eth_tx_vq->tc_index);

	/* Pull to des1 */
	do {
		ret = qbman_swp_pull(swp, &pulldesc1);
	} while (ret == -EBUSY);

	while (!eth_sc_sigint) {
		dq_idx = is_last = 0;
		/* Loop until the first dq_storage is updated with
		 * new token by QBMAN */
		while (!qbman_result_has_new_result(swp,
			&(dq_storage[0])))
			;

		/* Pull to des2 */
		do {
			ret = qbman_swp_pull(swp, &pulldesc2);
		} while (ret == -EBUSY);

		/* Recieve the packets till Last Dequeue entry is found with respect
		 * to the above issues PULL command. */
		while (is_last != 2) {
			if ((is_last == 1) && (dq_idx <= MAX_NUM_RECV_FRAMES)) {
				dq_idx = MAX_NUM_RECV_FRAMES;
				/* Loop until the first dq_storage of second pull is
				 * updated with new token by QBMAN */
				while (!qbman_result_has_new_result(swp,
					&(dq_storage[MAX_NUM_RECV_FRAMES])))
					;

				do {
					ret = qbman_swp_pull(swp, &pulldesc1);
				} while (ret == -EBUSY);
			}

			/* Check whether Last Pull command is Expired and setting
			 * Condition for Loop termination */
			if (odp_unlikely(qbman_result_DQ_is_pull_complete(
					&(dq_storage[dq_idx])))) {
				is_last++;
				/* Check for valid frame. If not then continue */
				status = (uint8_t)qbman_result_DQ_flags(
					&(dq_storage[dq_idx]));
				if (odp_unlikely((status & QBMAN_DQ_STAT_VALIDFRAME) == 0))
					continue;
			}

			/* Dequeue FD from QBMAN*/
			fd = qbman_result_DQ_fd(&(dq_storage[dq_idx]));
			/* Enqueue FD to QBMAN*/
			do {
				ret = qbman_swp_enqueue(swp, &eqdesc, fd);
			} while (ret == -EBUSY);
			dq_idx++;
		}
	}

	return NADK_SUCCESS;
}

int32_t nadk_eth_setup_rx_vq(struct nadk_dev *dev,
				uint8_t vq_id,
				struct nadk_vq_param *vq_cfg)
{
	/* Function to setup RX flow information. It contains traffic class ID,
	 * flow ID, destination configuration etc.
	 */
	int32_t retcode;
	struct nadk_dev_priv *dev_priv = dev->priv;
	struct fsl_mc_io *dpni = dev_priv->hw;
	struct dpni_queue_cfg cfg;
	uint8_t tc_id, flow_id;
	struct nadk_vq *eth_rx_vq;

	/*Get the tc id and flow id from given VQ id*/
	tc_id = vq_id / MAX_DIST_PER_TC;
	flow_id = vq_id % MAX_DIST_PER_TC;
	memset(&cfg, 0, sizeof(struct dpni_queue_cfg));

	eth_rx_vq = (struct nadk_vq *)(dev->rx_vq[vq_id]);
	eth_rx_vq->sync = ODP_SCHED_SYNC_NONE;
	if (vq_cfg) {
		if (vq_cfg->conc_dev) {
			struct conc_attr attr;
			memset(&attr, 0, sizeof(struct conc_attr));
			/*Get DPCONC object attributes*/
			nadk_conc_get_attributes(vq_cfg->conc_dev, &attr);

			/*Do settings to get the frame on a DPCON object*/
			cfg.options		= DPNI_QUEUE_OPT_DEST;
			cfg.dest_cfg.dest_type	= DPNI_DEST_DPCON;
			cfg.dest_cfg.dest_id	= attr.obj_id;
			cfg.dest_cfg.priority	= vq_cfg->prio;
			dev->conc_dev		= vq_cfg->conc_dev;
			NADK_INFO(ETH, "DPCON ID = %d\t Prio = %d\n",
				cfg.dest_cfg.dest_id, cfg.dest_cfg.priority);
			NADK_INFO(ETH, "Attaching Ethernet device %s"
				"with Channel %s\n", dev->dev_string,
				vq_cfg->conc_dev->dev_string);
		}
		if (vq_cfg->sync & ODP_SCHED_SYNC_ATOMIC) {
			cfg.options = cfg.options |
				DPNI_QUEUE_OPT_ORDER_PRESERVATION;
			cfg.order_preservation_en = TRUE;
			eth_rx_vq->sync = vq_cfg->sync;
		}
	}

	cfg.options = cfg.options | DPNI_QUEUE_OPT_USER_CTX;

#ifndef QODP_464
	cfg.options = cfg.options | DPNI_QUEUE_OPT_FLC;
#endif

	cfg.user_ctx = (uint64_t)(eth_rx_vq);
#ifndef QODP_464
	cfg.flc_cfg.flc_type = DPNI_FLC_STASH;
	cfg.flc_cfg.frame_data_size = DPNI_STASH_SIZE_64B;
	/* Enabling Annotation stashing */
	cfg.options |= DPNI_FLC_STASH_FRAME_ANNOTATION;
	cfg.flc_cfg.options = DPNI_FLC_STASH_FRAME_ANNOTATION;
#endif
	retcode = dpni_set_rx_flow(dpni, CMD_PRI_LOW, dev_priv->token,
						tc_id, flow_id, &cfg);
	if (retcode) {
		NADK_ERR(ETH, "Error in setting the rx flow: ErrorCode = %d\n",
								retcode);
		return NADK_FAILURE;
	}
	eth_rx_vq->flow_id = flow_id;
	eth_rx_vq->tc_index = tc_id;
	eth_rx_vq->fq_type = NADK_FQ_TYPE_RX;
	eth_rx_vq->qmfq.cb = nadk_eth_cb_dqrr_fd_to_mbuf;

	/* if prefetch mode is enabled and not the conc device*/
	if ((dev_priv->flags & NADK_PREFETCH_MODE)
		&& (!vq_cfg || !vq_cfg->conc_dev)) {
		eth_rx_vq->dq_storage[0] = nadk_data_malloc(NULL,
			NUM_MAX_RECV_FRAMES * 2 * sizeof(struct qbman_result),
			ODP_CACHE_LINE_SIZE);
		if (!eth_rx_vq->dq_storage[0]) {
			NADK_ERR(FW, "Memory allocation failure");
			return NADK_FAILURE;
		}
		eth_rx_vq->dq_storage[1] = eth_rx_vq->dq_storage[0] + NUM_MAX_RECV_FRAMES;
		eth_rx_vq->toggle = -1;
	}

	return NADK_SUCCESS;
}

int32_t nadk_eth_setup_tx_vq(struct nadk_dev *dev, uint32_t num,
					uint32_t action)
{
	/* Function to setup TX flow information. It contains traffic class ID,
	 * flow ID.
	 */
	uint32_t tc_idx;
	int32_t retcode;

	uint16_t flow_id = DPNI_NEW_FLOW_ID;
	struct nadk_dev_priv *dev_priv = dev->priv;
	struct fsl_mc_io *dpni = dev_priv->hw;
	struct dpni_tx_flow_cfg cfg;
	struct dpni_tx_flow_attr tx_flow_attr;
	struct dpni_tx_conf_attr tx_conf_attr;
	struct dpni_tx_conf_cfg tx_conf_cfg;
	struct nadk_vq *eth_tx_vq;
	struct nadk_vq *conf_err_vq;
	struct nadk_vq *def_err_vq;

	memset(&cfg, 0, sizeof(struct dpni_tx_flow_cfg));
	memset(&tx_conf_cfg, 0, sizeof(struct dpni_tx_conf_cfg));
	tx_conf_cfg.errors_only = TRUE;

	if (action & NADKBUF_TX_CONF_REQUIRED) {
		cfg.options = DPNI_TX_FLOW_OPT_TX_CONF_ERROR;
		cfg.use_common_tx_conf_queue =
				((action & NADKBUF_TX_CONF_ERR_ON_COMMON_Q) ?
								TRUE : FALSE);
		tx_conf_cfg.errors_only = FALSE;
	}
	for (tc_idx = 0; tc_idx < num; tc_idx++) {
		retcode = dpni_set_tx_flow(dpni, CMD_PRI_LOW, dev_priv->token,
							&flow_id, &cfg);
		if (retcode) {
			NADK_ERR(ETH, "Error in setting the tx flow\n"
						"ErrorCode = %x", retcode);
				return NADK_FAILURE;
		}
		/*Set tx-conf and error configuration*/
		retcode = dpni_set_tx_conf(dpni, CMD_PRI_LOW, dev_priv->token,
					   flow_id, &tx_conf_cfg);
		if (retcode) {
			NADK_ERR(ETH, "Error in setting tx conf settings\n"
						"ErrorCode = %x", retcode);
			return NADK_FAILURE;
		}
		eth_tx_vq = (struct nadk_vq *)(dev->tx_vq[tc_idx]);
		conf_err_vq = (struct nadk_vq *)(dev->err_vq[tc_idx]);
		if (flow_id == DPNI_NEW_FLOW_ID) {
			eth_tx_vq->flow_id = 0;
			conf_err_vq->flow_id = 0;
		} else {
			eth_tx_vq->flow_id = flow_id;
			conf_err_vq->flow_id = flow_id;
		}
		eth_tx_vq->tc_index = tc_idx;

		/*Set frame queue type*/
		eth_tx_vq->fq_type = NADK_FQ_TYPE_TX;
		conf_err_vq->fq_type = NADK_FQ_TYPE_TX_CONF_ERR;

		conf_err_vq->qmfq.cb = nadk_eth_cb_dqrr_tx_conf_err;
		retcode = dpni_get_tx_flow(dpni, CMD_PRI_LOW, dev_priv->token, flow_id,
								&tx_flow_attr);
		if (retcode) {
			NADK_ERR(ETH, "Error in getting the tx flow\n"
						"ErrorCode = %x", retcode);
			return NADK_FAILURE;
		}
		/*Get tx-conf and error frame queue id correspond to each
		sender*/
		retcode = dpni_get_tx_conf(dpni, CMD_PRI_LOW, dev_priv->token,
					   flow_id, &tx_conf_attr);
		if (retcode) {
			NADK_ERR(ETH, "Error in getting the tx conf settings\n"
						"ErrorCode = %x", retcode);
			return NADK_FAILURE;
		}
		conf_err_vq->fqid = tx_conf_attr.queue_attr.fqid;
		NADK_INFO(ETH, "tx-conf-err FQID = %d\nFlowID = %d",
				conf_err_vq->fqid, conf_err_vq->flow_id);
	}
	/*Set tx-conf and error configuration*/
	retcode = dpni_set_tx_conf(dpni, CMD_PRI_LOW, dev_priv->token,
				   DPNI_COMMON_TX_CONF, &tx_conf_cfg);
	if (retcode) {
		NADK_ERR(ETH, "Error in setting tx conf settings\n"
					"ErrorCode = %x", retcode);
		return NADK_FAILURE;
	}
	/*Get Common tx-conf and error frame queue id correspond to each dpni*/
	retcode = dpni_get_tx_conf(dpni, CMD_PRI_LOW, dev_priv->token,
				   DPNI_COMMON_TX_CONF, &tx_conf_attr);
	if (retcode) {
		NADK_ERR(ETH, "Error in getting common error fq attributes\n"
					"ErrorCode = %d", retcode);
		return NADK_FAILURE;
	}
	def_err_vq = (struct nadk_vq *)(dev->err_vq[DEF_ERR_VQ_INDEX]);
	def_err_vq->fq_type = NADK_FQ_TYPE_TX_CONF_ERR;
	def_err_vq->fqid = tx_conf_attr.queue_attr.fqid;
	def_err_vq->qmfq.cb = nadk_eth_cb_dqrr_tx_conf_err;
	NADK_INFO(ETH, "Default Error frame queue ID = %d\n", def_err_vq->fqid);
	return NADK_SUCCESS;
}

int nadk_eth_set_rx_vq_notification(
		struct nadk_dev *dev,
		uint8_t vq_id,
		uint64_t user_context,
		nadk_notification_callback_t cb)
{
	/* Function to setup RX flow information. It contains traffic class ID,
	 * flow ID, destination configuration etc.
	 */
	int32_t retcode;
	struct nadk_dev_priv *dev_priv = dev->priv;
	struct fsl_mc_io *dpni = dev_priv->hw;
	struct dpni_queue_cfg cfg;
	struct nadk_vq *eth_rx_vq = (struct nadk_vq *)(dev->rx_vq[vq_id]);
	uint64_t notifier_context;

	if (!notif_dpio) {
		NADK_ERR(ETH, "No notification portal available");
		return NADK_FAILURE;
	}

	retcode = nadk_reg_with_notifier(user_context, cb,
		&(eth_rx_vq->eventfd), &notifier_context);
	if (retcode != NADK_SUCCESS) {
		NADK_ERR(ETH, "nadk_reg_with_notifier failed");
		return NADK_FAILURE;
	}

	memset(&cfg, 0, sizeof(struct dpni_queue_cfg));
	cfg.options = DPNI_QUEUE_OPT_USER_CTX | DPNI_QUEUE_OPT_DEST;
	cfg.user_ctx = notifier_context;
	cfg.dest_cfg.dest_type = DPNI_DEST_DPIO;
	cfg.dest_cfg.dest_id = notif_dpio->hw_id;
	cfg.dest_cfg.priority = NADK_NOTIF_DEF_PRIO;

	retcode = dpni_set_rx_flow(dpni, CMD_PRI_LOW, dev_priv->token,
		eth_rx_vq->tc_index, eth_rx_vq->flow_id, &cfg);
	if (retcode) {
		NADK_ERR(ETH, "Error in setting the rx flow: ErrorCode = %x\n",
								retcode);
		return NADK_FAILURE;
	}

	return NADK_SUCCESS;
}

int32_t nadk_eth_attach_bp_list(struct nadk_dev *dev,
			void *blist)
{
	/* Function to attach a DPNI with a buffer pool list. Buffer pool list
	 * handle is passed in blist.
	 */
	int32_t loop, retcode;
	struct nadk_dev_priv *dev_priv = dev->priv;
	struct fsl_mc_io *dpni = dev_priv->hw;
	struct dpni_pools_cfg bpool_cfg;
	struct nadk_bp_list *bp_list = (struct nadk_bp_list *)blist;

	/*Attach buffer pool to the network interface as described by the user*/
	bpool_cfg.num_dpbp = bp_list->num_buf_pools;
	for (loop = 0; loop < bpool_cfg.num_dpbp; loop++) {
		bpool_cfg.pools[loop].dpbp_id =
				bp_list->buf_pool[loop].dpbp_node->dpbp_id;
		bpool_cfg.pools[loop].backup_pool = 0;
		bpool_cfg.pools[loop].buffer_size =
			bp_list->buf_pool[loop].size;
	}

	retcode = dpni_set_pools(dpni, CMD_PRI_LOW, dev_priv->token, &bpool_cfg);
	if (retcode != 0) {
		NADK_ERR(ETH, "Error in attaching the buffer pool list"
						"Error code = %d\n", retcode);
		return NADK_FAILURE;
	}

	dev_priv->bp_list = bp_list;
	return NADK_SUCCESS;
}

int32_t nadk_eth_reset(struct nadk_dev *dev)
{
	int32_t retcode;
	struct nadk_dev_priv *dev_priv = dev->priv;
	struct fsl_mc_io *dpni = dev_priv->hw;

	retcode = dpni_reset(dpni, CMD_PRI_LOW, dev_priv->token);
	if (retcode != 0) {
		NADK_ERR(ETH, "Error in Resetting the Ethernet"
				" device: ErrorCode = %d\n", retcode);
	}

	return retcode;
}

int nadk_eth_get_eventfd_from_vq(void *vq)
{
	struct nadk_vq *rx_vq = vq;
	return rx_vq->eventfd;
}

int nadk_eth_get_fqid(void *vq)
{
	struct nadk_vq *rx_vq = vq;
	return rx_vq->fqid;
}

void *nadk_eth_cb_dqrr_fd_to_mbuf(
		struct qbman_swp *qm ODP_UNUSED,
		const struct qbman_fd *fd,
		const struct qbman_result *dqrr)
{
	nadk_mbuf_pt mbuf;
	uint32_t frc;
	uint8_t *p_annotation;
	uint64_t fd_addr = (uint64_t)(NADK_IOVA_TO_VADDR(
		NADK_GET_FD_ADDR(fd)));

	if (odp_unlikely(NADK_GET_FD_IVP(fd))) {
		mbuf = nadk_mbuf_alloc_shell();
		if (!mbuf) {
			NADK_ERR(ETH, "Unable to allocate shell");
			return NULL;
		}
		mbuf->bpid = NADK_GET_FD_BPID(fd);
		mbuf->priv_meta_off = NADK_GET_FD_OFFSET(fd);
		mbuf->head = (uint8_t *)fd_addr + mbuf->priv_meta_off;
		mbuf->end_off = NADK_GET_FD_LEN(fd);

	} else {
		mbuf = NADK_INLINE_MBUF_FROM_BUF(fd_addr,
			bpid_info[NADK_GET_FD_BPID(fd)].meta_data_size);
		nadk_inline_mbuf_reset(mbuf);
		_odp_buffer_type_set(mbuf, ODP_EVENT_PACKET);
	}

	p_annotation	= (uint8_t *)fd_addr;
	mbuf->data	= (uint8_t *)fd_addr + NADK_GET_FD_OFFSET(fd);
	mbuf->frame_len	= NADK_GET_FD_LEN(fd);
	mbuf->tot_frame_len = mbuf->frame_len;

	/* Detect jumbo frames */
	if (mbuf->frame_len > ODPH_ETH_LEN_MAX)
		BIT_SET_AT_POS(mbuf->eth_flags, NADKBUF_IS_JUMBO);

	if (fd->simple.ctrl & NADK_FD_CTRL_PTA)
		p_annotation += NADK_FD_PTA_SIZE;

	frc = NADK_GET_FD_FRC(fd);
	if (frc & NADK_FD_FRC_FASV)
		mbuf->timestamp = odp_be_to_cpu_64
			(*((uint64_t *)(p_annotation +
			NADK_ETH_TIMESTAMP_OFFSET)));

	/* Fetch the User context */
	mbuf->vq = (void *)qbman_result_DQ_fqd_ctx(dqrr);

	/*TODO - based on vq type, store the DQRR in mbuf*/
	return (void *)mbuf;
}


static inline void nadk_eth_parse_tx_conf_error(const struct qbman_fd *fd,
				nadk_mbuf_pt mbuf)
{
	uint32_t status, errors = (fd->simple).ctrl;
	struct nadk_fas *fas;

	NADK_INFO(ETH, "Errors returned = %0x\n", errors);
	/*First - Check error in FD Error bits*/
	if (errors & NADK_FD_CTRL_FSE) {
		NADK_DBG(ETH, "Frame size too long\n");
		mbuf->eth_flags =
			mbuf->eth_flags |
			NADKBUF_ERROR_FRAME_TOO_LONG | NADKBUF_ERROR_TX;
#ifdef NADK_DEBUG_XSTATS
		xstats.tx_frm_len_err++;
#endif
	}
	if (errors & NADK_FD_CTRL_SBE) {
		NADK_DBG(ETH, "System bus error while transmitting\n");
		mbuf->eth_flags =
			mbuf->eth_flags |
			NADKBUF_ERROR_SYSTEM_BUS_ERROR |
			NADKBUF_ERROR_TX;
#ifdef NADK_DEBUG_XSTATS
		xstats.tx_sys_bus_err++;
#endif
	}
	if (errors & NADK_FD_CTRL_UFD) {
		NADK_DBG(ETH, "Unsupported frame format\n");
		mbuf->eth_flags =
			mbuf->eth_flags | NADKBUF_ERROR_TX;
	}
	/*Second - Check for the error bits in annotation area*/
	if (errors & NADK_FD_CTRL_FAERR) {
		fas = (struct nadk_fas *)
			(NADK_GET_FD_ADDR(fd) + NADK_ETH_PRIV_DATA_SIZE);
		status = odp_be_to_cpu_32(fas->status);
		mbuf->eth_flags =
				mbuf->eth_flags |
				NADKBUF_ERROR_TX;
		NADK_NOTE(ETH, "TxConf frame error(s): 0x%08x\n",
				status & NADK_ETH_TXCONF_ERR_MASK);
	}
	NADK_INFO(ETH, "Frame Descriptor parsing is completed\n");
	return;
}

/*todo - this function needs to be optimized*/
void *nadk_eth_cb_dqrr_tx_conf_err(
		struct qbman_swp *qm,
		const struct qbman_fd *fd,
		const struct qbman_result *dqrr)
{
	nadk_mbuf_pt mbuf;
	mbuf = nadk_eth_cb_dqrr_fd_to_mbuf(qm, fd, dqrr);
	if (mbuf)
		nadk_eth_parse_tx_conf_error(fd, mbuf);
	return (void *)mbuf;
}

static inline void nadk_eth_mbuf_to_fd(
		nadk_mbuf_pt mbuf,
		struct qbman_fd *fd)
{
	/*Resetting the buffer pool id and offset field*/
	fd->simple.bpid_offset = 0;
	NADK_SET_FD_ADDR(fd, NADK_VADDR_TO_IOVA(
		mbuf->head - mbuf->priv_meta_off));
	NADK_SET_FD_LEN(fd, mbuf->frame_len);
	NADK_SET_FD_BPID(fd, mbuf->bpid);
	NADK_SET_FD_OFFSET(fd, (nadk_mbuf_headroom(mbuf) +
		mbuf->priv_meta_off));
	NADK_SET_FD_ASAL(fd, NADK_ASAL_VAL);

	/*TODO: Check whether tx-conf is required for the frame of not*/
	if (mbuf->flags & NADKBUF_TX_CONF_REQUIRED) {
		NADK_INFO(ETH, "Confirmation is reuired for this buffer\n");
		/*Set the specified bits and fqid in Action descriptor so
		that confirmation*/
	}

	return;
}

/*! @} */
