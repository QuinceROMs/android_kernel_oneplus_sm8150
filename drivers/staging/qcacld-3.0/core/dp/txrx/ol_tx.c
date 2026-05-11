/*
 * Copyright (c) 2011-2019 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/* OS abstraction libraries */
#include <qdf_nbuf.h>           /* qdf_nbuf_t, etc. */
#include <qdf_atomic.h>         /* qdf_atomic_read, etc. */
#include <qdf_util.h>           /* qdf_unlikely */
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>

/* APIs for other modules */
#include <htt.h>                /* HTT_TX_EXT_TID_MGMT */
#include <ol_htt_tx_api.h>      /* htt_tx_desc_tid */

/* internal header files relevant for all systems */
#include <ol_txrx_internal.h>   /* TXRX_ASSERT1 */
#include <ol_tx_desc.h>         /* ol_tx_desc */
#include <ol_tx_send.h>         /* ol_tx_send */
#include <ol_txrx.h>

/* internal header files relevant only for HL systems */
#include <ol_tx_classify.h>   /* ol_tx_classify, ol_tx_classify_mgmt */
#include <ol_tx_queue.h>        /* ol_tx_enqueue */
#include <ol_tx_sched.h>      /* ol_tx_sched */

/* internal header files relevant only for specific systems (Pronto) */
#include <ol_txrx_encap.h>      /* OL_TX_ENCAP, etc */
#include <ol_tx.h>
#include <cdp_txrx_ipa.h>

#define OL_TXRX_RAW_MIN_CTL_LEN 10
#define OL_TXRX_RAW_CTL_COMMON_LEN 16
#define OL_TXRX_RAW_CTL_CONTROL_LEN 2
#define OL_TXRX_RAW_BA_START_SEQ_LEN 2
#define OL_TXRX_RAW_BA_PER_TID_INFO_LEN 2
#define OL_TXRX_RAW_BA_COMPRESSED_BITMAP_LEN 8
#define OL_TXRX_RAW_BA_BASIC_BITMAP_LEN 128

/**
 * ol_tx_data() - send data frame
 * @soc_hdl: datapath soc handle
 * @vdev_id: virtual interface id
 * @skb: skb
 *
 * Return: skb/NULL for success
 */
qdf_nbuf_t ol_tx_data(struct cdp_soc_t *soc_hdl, uint8_t vdev_id,
		      qdf_nbuf_t skb)
{
	struct ol_txrx_pdev_t *pdev;
	qdf_nbuf_t ret;
	struct ol_txrx_soc_t *soc = cdp_soc_t_to_ol_txrx_soc_t(soc_hdl);
	ol_txrx_vdev_handle vdev = ol_txrx_get_vdev_from_soc_vdev_id(soc,
								     vdev_id);

	if (qdf_unlikely(!vdev)) {
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_DEBUG,
			"%s:vdev is null", __func__);
		return skb;
	}

	pdev = vdev->pdev;

	if (qdf_unlikely(!pdev)) {
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_DEBUG,
			"%s:pdev is null", __func__);
		return skb;
	}

	if ((ol_cfg_is_ip_tcp_udp_checksum_offload_enabled(pdev->ctrl_pdev))
		&& (qdf_nbuf_get_protocol(skb) == htons(ETH_P_IP))
		&& (qdf_nbuf_get_ip_summed(skb) == CHECKSUM_PARTIAL))
		qdf_nbuf_set_ip_summed(skb, CHECKSUM_COMPLETE);

	/* Terminate the (single-element) list of tx frames */
	qdf_nbuf_set_next(skb, NULL);
	ret = OL_TX_SEND(vdev, skb);
	if (ret) {
		ol_txrx_dbg("Failed to tx");
		return ret;
	}

	return NULL;
}

#ifdef IPA_OFFLOAD
qdf_nbuf_t ol_tx_send_ipa_data_frame(struct cdp_soc_t *soc_hdl, uint8_t vdev_id,
				     qdf_nbuf_t skb)
{
	struct ol_txrx_soc_t *soc = cdp_soc_t_to_ol_txrx_soc_t(soc_hdl);
	struct ol_txrx_pdev_t *pdev = ol_txrx_get_pdev_from_pdev_id(
							soc, OL_TXRX_PDEV_ID);
	ol_txrx_vdev_handle vdev = ol_txrx_get_vdev_from_soc_vdev_id(soc,
								     vdev_id);
	qdf_nbuf_t ret;

	if (qdf_unlikely(!pdev)) {
		ol_txrx_err("%s: invalid pdev", __func__);
		return skb;
	}
	if (qdf_unlikely(!vdev)) {
		ol_txrx_err("%s: invalid vdev, vdev_id:%d", __func__, vdev_id);
		return skb;
	}

	if ((ol_cfg_is_ip_tcp_udp_checksum_offload_enabled(pdev->ctrl_pdev))
		&& (qdf_nbuf_get_protocol(skb) == htons(ETH_P_IP))
		&& (qdf_nbuf_get_ip_summed(skb) == CHECKSUM_PARTIAL))
		qdf_nbuf_set_ip_summed(skb, CHECKSUM_COMPLETE);

	/* Terminate the (single-element) list of tx frames */
	qdf_nbuf_set_next(skb, NULL);

	/*
	 * Add SKB to internal tracking table before further processing
	 * in WLAN driver.
	 */
	qdf_net_buf_debug_acquire_skb(skb, __FILE__, __LINE__);

	ret = OL_TX_SEND((struct ol_txrx_vdev_t *)vdev, skb);
	if (ret) {
		ol_txrx_dbg("Failed to tx");
		return ret;
	}

	return NULL;
}
#endif

void
ol_txrx_data_tx_cb_set(struct cdp_soc_t *soc_hdl, uint8_t vdev_id,
		       ol_txrx_data_tx_cb callback, void *ctxt)
{
	struct ol_txrx_soc_t *soc = cdp_soc_t_to_ol_txrx_soc_t(soc_hdl);
	ol_txrx_vdev_handle vdev = ol_txrx_get_vdev_from_soc_vdev_id(soc,
								     vdev_id);
	struct ol_txrx_pdev_t *pdev;

	if (!vdev || !vdev->pdev)
		return;

	pdev = vdev->pdev;
	pdev->tx_data_callback.func = callback;
	pdev->tx_data_callback.ctxt = ctxt;
}

QDF_STATUS
ol_txrx_mgmt_tx_cb_set(struct cdp_soc_t *soc_hdl, uint8_t pdev_id, uint8_t type,
		       ol_txrx_mgmt_tx_cb download_cb,
		       ol_txrx_mgmt_tx_cb ota_ack_cb, void *ctxt)
{
	struct ol_txrx_soc_t *soc = cdp_soc_t_to_ol_txrx_soc_t(soc_hdl);
	struct ol_txrx_pdev_t *pdev = ol_txrx_get_pdev_from_pdev_id(soc,
								    pdev_id);

	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	TXRX_ASSERT1(type < OL_TXRX_MGMT_NUM_TYPES);
	pdev->tx_mgmt_cb.download_cb = download_cb;
	pdev->tx_mgmt_cb.ota_ack_cb = ota_ack_cb;
	pdev->tx_mgmt_cb.ctxt = ctxt;

	return QDF_STATUS_SUCCESS;
}

int
ol_txrx_mgmt_send_ext(struct cdp_soc_t *soc_hdl, uint8_t vdev_id,
		      qdf_nbuf_t tx_mgmt_frm, uint8_t type,
		      uint8_t use_6mbps, uint16_t chanfreq)
{
	struct ol_txrx_soc_t *soc = cdp_soc_t_to_ol_txrx_soc_t(soc_hdl);
	ol_txrx_vdev_handle vdev = ol_txrx_get_vdev_from_soc_vdev_id(soc,
								     vdev_id);
	struct ol_txrx_pdev_t *pdev;
	struct ol_tx_desc_t *tx_desc;
	struct ol_txrx_msdu_info_t tx_msdu_info;
	int result = 0;

	if (!vdev || !vdev->pdev)
		return QDF_STATUS_E_FAULT;

	pdev = vdev->pdev;
	tx_msdu_info.tso_info.is_tso = 0;

	tx_msdu_info.htt.action.use_6mbps = use_6mbps;
	tx_msdu_info.htt.info.ext_tid = HTT_TX_EXT_TID_MGMT;
	tx_msdu_info.htt.info.vdev_id = vdev->vdev_id;
	tx_msdu_info.htt.action.do_tx_complete =
		pdev->tx_mgmt_cb.ota_ack_cb ? 1 : 0;

	/*
	 * FIX THIS: l2_hdr_type should only specify L2 header type
	 * The Peregrine/Rome HTT layer provides the FW with a "pkt type"
	 * that is a combination of L2 header type and 802.11 frame type.
	 * If the 802.11 frame type is "mgmt", then the HTT pkt type is "mgmt".
	 * But if the 802.11 frame type is "data", then the HTT pkt type is
	 * the L2 header type (more or less): 802.3 vs. Native WiFi
	 * (basic 802.11).
	 * (Or the header type can be "raw", which is any version of the 802.11
	 * header, and also implies that some of the offloaded tx data
	 * processing steps may not apply.)
	 * For efficiency, the Peregrine/Rome HTT uses the msdu_info's
	 * l2_hdr_type field to program the HTT pkt type.  Thus, this txrx SW
	 * needs to overload the l2_hdr_type to indicate whether the frame is
	 * data vs. mgmt, as well as 802.3 L2 header vs. 802.11 L2 header.
	 * To fix this, the msdu_info's l2_hdr_type should be left specifying
	 * just the L2 header type.  For mgmt frames, there should be a
	 * separate function to patch the HTT pkt type to store a "mgmt" value
	 * rather than the L2 header type.  Then the HTT pkt type can be
	 * programmed efficiently for data frames, and the msdu_info's
	 * l2_hdr_type field won't be confusingly overloaded to hold the 802.11
	 * frame type rather than the L2 header type.
	 */
	/*
	 * FIX THIS: remove duplication of htt_frm_type_mgmt and
	 * htt_pkt_type_mgmt
	 * The htt module expects a "enum htt_pkt_type" value.
	 * The htt_dxe module expects a "enum htt_frm_type" value.
	 * This needs to be cleaned up, so both versions of htt use a
	 * consistent method of specifying the frame type.
	 */
#ifdef QCA_SUPPORT_INTEGRATED_SOC
	/* tx mgmt frames always come with a 802.11 header */
	tx_msdu_info.htt.info.l2_hdr_type = htt_pkt_type_native_wifi;
	tx_msdu_info.htt.info.frame_type = htt_frm_type_mgmt;
#else
	tx_msdu_info.htt.info.l2_hdr_type = htt_pkt_type_mgmt;
	tx_msdu_info.htt.info.frame_type = htt_pkt_type_mgmt;
#endif

	tx_msdu_info.peer = NULL;

	tx_desc = ol_txrx_mgmt_tx_desc_alloc(pdev, vdev, tx_mgmt_frm,
							&tx_msdu_info);
	if (!tx_desc)
		return -EINVAL;       /* can't accept the tx mgmt frame */

	TXRX_STATS_MSDU_INCR(pdev, tx.mgmt, tx_mgmt_frm);
	TXRX_ASSERT1(type < OL_TXRX_MGMT_NUM_TYPES);
	tx_desc->pkt_type = type + OL_TXRX_MGMT_TYPE_BASE;

	result = ol_txrx_mgmt_send_frame(vdev, tx_desc, tx_mgmt_frm,
						&tx_msdu_info, chanfreq);

	return 0;               /* accepted the tx mgmt frame */
}

static uint32_t ol_txrx_raw_ba_tid_count(uint16_t control)
{
	return ((control & IEEE80211_BAR_CTRL_TID_INFO_MASK) >>
		IEEE80211_BAR_CTRL_TID_INFO_SHIFT) + 1;
}

static uint16_t ol_txrx_raw_ctl_control(struct ieee80211_hdr *hdr)
{
	__le16 control;

	qdf_mem_copy(&control, (uint8_t *)hdr + OL_TXRX_RAW_CTL_COMMON_LEN,
		     sizeof(control));

	return le16_to_cpu(control);
}

static uint32_t ol_txrx_raw_min_len(struct ieee80211_hdr *hdr)
{
	__le16 fc = hdr->frame_control;
	uint16_t control;
	uint32_t tid_count;
	uint32_t bitmap_len;
	bool multi_tid;

	if (!ieee80211_is_back_req(fc) && !ieee80211_is_back(fc))
		return ieee80211_hdrlen(fc);

	control = ol_txrx_raw_ctl_control(hdr);
	multi_tid = control & IEEE80211_BAR_CTRL_MULTI_TID;
	tid_count = multi_tid ? ol_txrx_raw_ba_tid_count(control) : 1;

	if (ieee80211_is_back_req(fc)) {
		if (!multi_tid)
			return sizeof(struct ieee80211_bar);

		return OL_TXRX_RAW_CTL_COMMON_LEN +
		       OL_TXRX_RAW_CTL_CONTROL_LEN +
		       tid_count * (OL_TXRX_RAW_BA_PER_TID_INFO_LEN +
				    OL_TXRX_RAW_BA_START_SEQ_LEN);
	}

	bitmap_len = control & IEEE80211_BAR_CTRL_CBMTID_COMPRESSED_BA ?
		     OL_TXRX_RAW_BA_COMPRESSED_BITMAP_LEN :
		     OL_TXRX_RAW_BA_BASIC_BITMAP_LEN;

	if (!multi_tid)
		return OL_TXRX_RAW_CTL_COMMON_LEN +
		       OL_TXRX_RAW_CTL_CONTROL_LEN +
		       OL_TXRX_RAW_BA_START_SEQ_LEN + bitmap_len;

	return OL_TXRX_RAW_CTL_COMMON_LEN + OL_TXRX_RAW_CTL_CONTROL_LEN +
	       tid_count * (OL_TXRX_RAW_BA_PER_TID_INFO_LEN +
			    OL_TXRX_RAW_BA_START_SEQ_LEN + bitmap_len);
}

static int ol_txrx_raw_frame_info(struct ieee80211_hdr *hdr, uint32_t len,
				  uint8_t *ext_tid, uint8_t *frame_type)
{
	__le16 fc = hdr->frame_control;

	if (ieee80211_is_back_req(fc) || ieee80211_is_back(fc)) {
		if (len < OL_TXRX_RAW_CTL_COMMON_LEN +
			  OL_TXRX_RAW_CTL_CONTROL_LEN)
			return -EINVAL;
	}

	if (len < ol_txrx_raw_min_len(hdr))
		return -EINVAL;

	if (ieee80211_is_ctl(fc)) {
		*ext_tid = HTT_TX_EXT_TID_NON_QOS_MCAST_BCAST;
		*frame_type = htt_frm_type_ctrl;
		return 0;
	}

	if (!ieee80211_is_data(fc))
		return -EINVAL;

	if (ieee80211_is_data_qos(fc)) {
		*ext_tid = ieee80211_get_qos_ctl(hdr)[0] &
			   IEEE80211_QOS_CTL_TID_MASK;
		*frame_type = htt_frm_type_data;
		return 0;
	}

	*ext_tid = is_multicast_ether_addr(hdr->addr1) ?
		   HTT_TX_EXT_TID_NON_QOS_MCAST_BCAST : HTT_NON_QOS_TID;
	*frame_type = htt_frm_type_data;

	return 0;
}

int
ol_txrx_raw_send_ext(struct cdp_soc_t *soc_hdl, uint8_t vdev_id,
		     qdf_nbuf_t tx_frm, uint8_t use_6mbps,
		     uint16_t chanfreq)
{
#ifdef CONFIG_HL_SUPPORT
	return -EOPNOTSUPP;
#else
	struct ol_txrx_soc_t *soc = cdp_soc_t_to_ol_txrx_soc_t(soc_hdl);
	ol_txrx_vdev_handle vdev = ol_txrx_get_vdev_from_soc_vdev_id(soc,
								     vdev_id);
	struct ieee80211_hdr *hdr;
	struct ol_txrx_pdev_t *pdev;
	struct ol_tx_desc_t *tx_desc;
	struct ol_txrx_msdu_info_t tx_msdu_info;
	uint32_t *word0;
	uint8_t frame_type;
	uint8_t ext_tid;
	int ret;

	if (!vdev || !vdev->pdev || !tx_frm)
		return -EFAULT;

	if (qdf_nbuf_len(tx_frm) < OL_TXRX_RAW_MIN_CTL_LEN)
		return -EINVAL;

	hdr = (struct ieee80211_hdr *)qdf_nbuf_data(tx_frm);
	ret = ol_txrx_raw_frame_info(hdr, qdf_nbuf_len(tx_frm), &ext_tid,
				     &frame_type);
	if (ret)
		return ret;

	pdev = vdev->pdev;
	qdf_mem_zero(&tx_msdu_info, sizeof(tx_msdu_info));
	tx_msdu_info.htt.action.use_6mbps = use_6mbps;
	tx_msdu_info.htt.action.do_encrypt = 0;
	tx_msdu_info.htt.action.tx_comp_req = 0;
	tx_msdu_info.htt.info.ext_tid = ext_tid;
	tx_msdu_info.htt.info.vdev_id = vdev->vdev_id;
	tx_msdu_info.htt.info.peer_id = HTT_INVALID_PEER_ID;
	tx_msdu_info.htt.info.l2_hdr_type = htt_pkt_type_raw;
	tx_msdu_info.htt.info.frame_type = frame_type;
	tx_msdu_info.htt.info.is_unicast =
		!is_multicast_ether_addr(hdr->addr1);

	tx_desc = ol_tx_desc_ll(pdev, vdev, tx_frm, &tx_msdu_info);
	if (!tx_desc)
		return -EINVAL;

	htt_tx_desc_set_chanfreq(tx_desc->htt_tx_desc, chanfreq);
	htt_tx_desc_type(pdev->htt_pdev, tx_desc->htt_tx_desc,
			 htt_pkt_type_raw,
			 ol_txrx_tx_raw_subtype(OL_TX_SPEC_RAW |
						OL_TX_SPEC_NO_AGGR |
						OL_TX_SPEC_NO_ENCRYPT));
	word0 = (uint32_t *)tx_desc->htt_tx_desc;
	HTT_TX_DESC_NO_ENCRYPT_SET(*word0, 1);

	QDF_NBUF_CB_TX_PACKET_TRACK(tx_desc->netbuf) =
		QDF_NBUF_TX_PKT_DATA_TRACK;
	ol_tx_send_nonstd(pdev, tx_desc, tx_frm, htt_pkt_type_raw);

	return 0;
#endif
}
