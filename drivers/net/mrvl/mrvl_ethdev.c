#include <rte_ethdev.h>
#include <rte_kvargs.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_vdev.h>

#include <drivers/mv_pp2.h>
#include <drivers/mv_pp2_bpool.h>
#include <drivers/mv_pp2_hif.h>
#include <drivers/mv_pp2_ppio.h>

#include <assert.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

/* bitmask with reserved hifs */
#define MRVL_MUSDK_HIFS_RESERVED 0x0F
/* bitmask with reserved bpools */
#define MRVL_MUSDK_BPOOLS_RESERVED 0x07
/* maximum number of available hifs */
#define MRVL_MUSDK_HIFS_MAX 9

/* maximum number of ports supported by packet processor */
#define MRVL_PP2_PORTS_MAX 3
/* maximum number of available packet processors */
#define MRVL_PP2_MAX 2
/* maximum number of rx queues per port */
#define MRVL_PP2_RXQ_MAX 32
/* maximum number of tx queues per port */
#define MRVL_PP2_TXQ_MAX 8
/* minimum number of descriptors in tx queue */
#define MRVL_PP2_TXD_MIN 16
/* maximum number of descriptors in tx queue */
#define MRVL_PP2_TXD_MAX 1024
/* tx queue descriptors alignment */
#define MRVL_PP2_TXD_ALIGN 16
/* minimum number of descriptors in rx queue */
#define MRVL_PP2_RXD_MIN 16
/* maximum number of descriptors in rx queue */
#define MRVL_PP2_RXD_MAX 1024
/* rx queue descriptors alignment */
#define MRVL_PP2_RXD_ALIGN 16
/* maximum number of descriptors in tx aggregated queue */
#define MRVL_PP2_AGGR_TXQD_MAX 1024
/* maximum number of available bpools */
#define MRVL_PP2_BPOOLS_MAX 16
/* maximum number of BPPEs */
#define MRVL_PP2_BPPE_MAX 8192

#define MRVL_MAC_ADDRS_MAX 32
#define MRVL_MATCH_LEN 16
#define MRVL_PKT_OFFS 64
#define MRVL_PKT_EFFEC_OFFS (MRVL_PKT_OFFS + PP2_MH_SIZE)
#define MRVL_MAX_BURST_SIZE 1024

#define MRVL_IFACE_NAME_ARG "iface"

static const char *valid_args[] = {
	MRVL_IFACE_NAME_ARG,
	NULL
};

static int used_hifs = MRVL_MUSDK_HIFS_RESERVED;
static int used_bpools = MRVL_MUSDK_BPOOLS_RESERVED;

struct mrvl_priv {
	struct pp2_hif *hif;
	struct pp2_bpool *bpool;
	struct pp2_ppio	*ppio;
	uint32_t dma_addr_high;

	struct pp2_ppio_params ppio_params;

	uint8_t pp_id;
	uint8_t ppio_id;
};

struct mrvl_rxq {
	struct mrvl_priv *priv;
	struct rte_mempool *mp;
	int queue_id;
	int port_id;
};

struct mrvl_txq {
	struct mrvl_priv *priv;
	int queue_id;
};

static inline int
mrvl_reserve_bit(int *bitmap, int max)
{
	int n = sizeof(*bitmap) * 8 - __builtin_clz(*bitmap);
	if (n >= max)
		return -1;

	*bitmap |= 1 << n;

	return n;
}

static int
mrvl_dev_configure(struct rte_eth_dev *dev)
{
	struct mrvl_priv *priv = dev->data->dev_private;
	struct pp2_ppio_inq_params *inq_params;
	struct pp2_bpool_params bpool_params;
	struct pp2_hif_params hif_params;
	char match[MRVL_MATCH_LEN];
	int ret;

	ret = pp2_netdev_get_port_info(dev->data->name, &priv->pp_id, &priv->ppio_id);
	if (ret)
		return ret;

	ret = mrvl_reserve_bit(&used_bpools, MRVL_PP2_BPOOLS_MAX);
	if (ret < 0)
		return ret;

	snprintf(match, sizeof(match), "pool-%d:%d", priv->pp_id, ret);
	memset(&bpool_params, 0, sizeof(bpool_params));
	bpool_params.match = match;
	bpool_params.max_num_buffs = MRVL_PP2_BPPE_MAX;
	bpool_params.buff_len = RTE_MBUF_DEFAULT_BUF_SIZE;
	ret = pp2_bpool_init(&bpool_params, &priv->bpool);
	if (ret)
		return ret;

	ret = mrvl_reserve_bit(&used_hifs, MRVL_MUSDK_HIFS_MAX);
	if (ret < 0)
		goto out_deinit_bpool;

	snprintf(match, sizeof(match), "hif-%d", ret);
	memset(&hif_params, 0, sizeof(hif_params));
	hif_params.match = match;
	hif_params.out_size = MRVL_PP2_AGGR_TXQD_MAX;
	ret = pp2_hif_init(&hif_params, &priv->hif);
	if (ret)
		goto out_deinit_bpool;

	inq_params = rte_zmalloc_socket("inq_params",
					dev->data->nb_rx_queues * sizeof(*inq_params),
					0, rte_socket_id());
	if (!inq_params) {
		ret = -ENOMEM;
		goto out_deinit_hif;
	}

	priv->dma_addr_high = -1;
	priv->ppio_params.type = PP2_PPIO_T_NIC;

	priv->ppio_params.inqs_params.num_tcs = 1;
	priv->ppio_params.inqs_params.tcs_params[0].pkt_offset = MRVL_PKT_OFFS;
	priv->ppio_params.inqs_params.tcs_params[0].num_in_qs = dev->data->nb_rx_queues;
	priv->ppio_params.inqs_params.tcs_params[0].inqs_params = inq_params;
	priv->ppio_params.inqs_params.tcs_params[0].pools[0] = priv->bpool;

	priv->ppio_params.outqs_params.num_outqs = dev->data->nb_tx_queues;

	return 0;
out_deinit_hif:
	pp2_hif_deinit(priv->hif);
out_deinit_bpool:
	pp2_bpool_deinit(priv->bpool);
	return ret;
}

static int
mrvl_dev_start(struct rte_eth_dev *dev)
{
	struct mrvl_priv *priv = dev->data->dev_private;
	char match[MRVL_MATCH_LEN];

	snprintf(match, sizeof(match), "ppio-%d:%d", priv->pp_id, priv->ppio_id);
	priv->ppio_params.match = match;

	return pp2_ppio_init(&priv->ppio_params, &priv->ppio);
}

static void
mrvl_dev_stop(struct rte_eth_dev *dev)
{
	struct mrvl_priv *priv = dev->data->dev_private;

	rte_free(priv->ppio_params.inqs_params.tcs_params[0].inqs_params);
	pp2_ppio_deinit(priv->ppio);
}

static int
mrvl_dev_set_link_up(struct rte_eth_dev *dev)
{
	struct mrvl_priv *priv = dev->data->dev_private;
	int ret;

	ret = pp2_ppio_enable(priv->ppio);
	if (ret)
		return ret;

	dev->data->dev_link.link_status = ETH_LINK_UP;

	return 0;
}

static int
mrvl_dev_set_link_down(struct rte_eth_dev *dev)
{
	struct mrvl_priv *priv = dev->data->dev_private;
	int ret;


	ret = pp2_ppio_disable(priv->ppio);
	if (ret)
		return ret;

	dev->data->dev_link.link_status = ETH_LINK_DOWN;

	return 0;
}

static int
mrvl_link_update(struct rte_eth_dev *dev, int wait_to_complete)
{
	/*
	 * TODO: how to get that from musdk? in fact there are apis for this
	 * stuff but not exported to userland (pp2_gop)
	 */
	dev->data->dev_link.link_status = ETH_LINK_UP;
	/* pass this as parameter? */
	dev->data->dev_link.link_speed = ETH_SPEED_NUM_10G;

	return 0;
}

static void
mrvl_promiscuous_enable(struct rte_eth_dev *dev)
{
	struct mrvl_priv *priv = dev->data->dev_private;

	pp2_ppio_set_uc_promisc(priv->ppio, 1);
	pp2_ppio_set_mc_promisc(priv->ppio, 1);
}

static void
mrvl_promiscuous_disable(struct rte_eth_dev *dev)
{
	struct mrvl_priv *priv = dev->data->dev_private;

	pp2_ppio_set_uc_promisc(priv->ppio, 0);
	pp2_ppio_set_mc_promisc(priv->ppio, 0);
}

static void
mrvl_mac_addr_remove(struct rte_eth_dev *dev, uint32_t index)
{
	struct mrvl_priv *priv = dev->data->dev_private;
	char buf[ETHER_ADDR_FMT_SIZE];
	int ret;

	ret = pp2_ppio_remove_mac_addr(priv->ppio, dev->data->mac_addrs[index].addr_bytes);
	if (ret) {
		ether_format_addr(buf, sizeof(buf), &dev->data->mac_addrs[index]);
		RTE_LOG(ERR, PMD, "Failed to remove mac %s\n", buf);
	}
}

static void
mrvl_mac_addr_add(struct rte_eth_dev *dev, struct ether_addr *mac_addr,
		  uint32_t index, uint32_t vmdq)
{
	struct mrvl_priv *priv = dev->data->dev_private;
	char buf[ETHER_ADDR_FMT_SIZE];
	int ret;

	ret = pp2_ppio_add_mac_addr(priv->ppio, mac_addr->addr_bytes);
	if (ret) {
		ether_format_addr(buf, sizeof(buf), mac_addr);
		RTE_LOG(ERR, PMD, "Failed to add mac %s\n", buf);
	}
}

static void
mrvl_mac_addr_set(struct rte_eth_dev *dev, struct ether_addr *mac_addr)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	char buf[ETHER_ADDR_FMT_SIZE];
	struct ifreq req;
	int ret;

	/* TODO: temporary solution until musdk provides something similar */
	memset(&req, 0, sizeof(req));
	strcpy(req.ifr_name, dev->data->name);
	memcpy(req.ifr_hwaddr.sa_data, mac_addr->addr_bytes, ETHER_ADDR_LEN);
	req.ifr_hwaddr.sa_family = ARPHRD_ETHER;

	ret = ioctl(fd, SIOCSIFHWADDR, &req);
	if (ret) {
		ether_format_addr(buf, sizeof(buf), mac_addr);
		RTE_LOG(ERR, PMD, "Failed to set mac %s\n", buf);
	}
}

static int
mrvl_mtu_set(struct rte_eth_dev *dev, uint16_t mtu)
{
	struct mrvl_priv *priv = dev->data->dev_private;

	return pp2_ppio_set_mtu(priv->ppio, mtu);
}

static void
mrvl_dev_infos_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *info)
{
	info->max_rx_queues = MRVL_PP2_RXQ_MAX;
	info->max_tx_queues = MRVL_PP2_TXQ_MAX;
	info->max_mac_addrs = MRVL_MAC_ADDRS_MAX;

	info->rx_desc_lim.nb_max = MRVL_PP2_RXD_MAX;
	info->rx_desc_lim.nb_min = MRVL_PP2_RXD_MIN;
	info->rx_desc_lim.nb_align = MRVL_PP2_RXD_ALIGN;

	info->tx_desc_lim.nb_max = MRVL_PP2_TXD_MAX;
	info->tx_desc_lim.nb_min = MRVL_PP2_TXD_MIN;
	info->tx_desc_lim.nb_align = MRVL_PP2_TXD_ALIGN;
}

static int
mrvl_vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
	struct mrvl_priv *priv = dev->data->dev_private;

	return on ? pp2_ppio_add_vlan(priv->ppio, vlan_id) :
		    pp2_ppio_remove_vlan(priv->ppio, vlan_id);
}

static int
mrvl_fill_bpool(struct mrvl_rxq *rxq)
{
	struct pp2_buff_inf buff_inf;
	struct rte_mbuf *mbuf;
	uint64_t dma_addr;
	int ret;

	mbuf = rte_pktmbuf_alloc(rxq->mp);
	if (unlikely(!mbuf))
		return -ENOMEM;

	dma_addr = rte_mbuf_data_dma_addr_default(mbuf);

	if (unlikely(rxq->priv->dma_addr_high == -1))
		rxq->priv->dma_addr_high = dma_addr >> 32;

	/* all BPPEs must be located in the same 4GB address space */
	if (unlikely(rxq->priv->dma_addr_high != dma_addr >> 32)) {
		ret = -EFAULT;
		goto out_free_mbuf;
	}

	buff_inf.addr = dma_addr;
	buff_inf.cookie = (pp2_cookie_t)mbuf;

	ret = pp2_bpool_put_buff(rxq->priv->hif, rxq->priv->bpool,
				 &buff_inf);
	if (unlikely(ret)) {
		RTE_LOG(ERR, PMD, "Failed to release buffer to bm\n");
		ret = -ENOBUFS;
		goto out_free_mbuf;
	}

	return 0;
out_free_mbuf:
	rte_pktmbuf_free(mbuf);

	return ret;
}

static int
mrvl_rx_queue_setup(struct rte_eth_dev *dev, uint16_t idx, uint16_t desc,
		    unsigned int socket, const struct rte_eth_rxconf *conf,
		    struct rte_mempool *mp)
{
	struct mrvl_priv *priv = dev->data->dev_private;
	struct mrvl_rxq *rxq;
	int i, ret;

	rxq = rte_zmalloc_socket("rxq", sizeof(*rxq), 0, socket);
	if (!rxq)
		return -ENOMEM;

	rxq->priv = priv;
	rxq->mp = mp;
	rxq->queue_id = idx;
	rxq->port_id = dev->data->port_id;

	dev->data->rx_queues[idx] = rxq;

	priv->ppio_params.inqs_params.tcs_params[0].inqs_params[idx].size = desc;

	for (i = 0; i < desc; i++) {
		ret = mrvl_fill_bpool(rxq);
		if (ret)
			goto out_free_mbufs;
	}

	return 0;
out_free_mbufs:
	for (; i >= 0; i--) {
		struct pp2_buff_inf inf;

		pp2_bpool_get_buff(rxq->priv->hif, rxq->priv->bpool, &inf);
		rte_pktmbuf_free((void *)inf.cookie);
	}
out_free_rxq:
	rte_free(rxq);
	return ret;
}

static int
mrvl_tx_queue_setup(struct rte_eth_dev *dev, uint16_t idx, uint16_t desc,
		    unsigned int socket, const struct rte_eth_txconf *conf)
{
	struct mrvl_priv *priv = dev->data->dev_private;
	struct mrvl_txq *txq;
	int i;

	txq = rte_zmalloc_socket("txq", sizeof(*txq), 0, socket);
	if (!txq)
		return -ENOMEM;

	txq->priv = priv;
	txq->queue_id = idx;
	dev->data->tx_queues[idx] = txq;

	priv->ppio_params.outqs_params.outqs_params[idx].size = desc;
	priv->ppio_params.outqs_params.outqs_params[idx].weight = 1;

	return 0;
}

static const struct eth_dev_ops mrvl_ops = {
	.dev_configure = mrvl_dev_configure,
	.dev_start = mrvl_dev_start,
	.dev_stop = mrvl_dev_stop,
	.dev_set_link_up = mrvl_dev_set_link_up,
	.dev_set_link_down = mrvl_dev_set_link_down,
	.link_update = mrvl_link_update,
	.promiscuous_enable = mrvl_promiscuous_enable,
	.promiscuous_disable = mrvl_promiscuous_disable,
	.mac_addr_remove = mrvl_mac_addr_remove,
	.mac_addr_add = mrvl_mac_addr_add,
	.mac_addr_set = mrvl_mac_addr_set,
	.mtu_set = mrvl_mtu_set,
	.stats_get = NULL,
	.stats_reset = NULL,
	.dev_infos_get = mrvl_dev_infos_get,
	.rxq_info_get = NULL,
	.txq_info_get = NULL,
	.vlan_filter_set = mrvl_vlan_filter_set,
	.rx_queue_start = NULL,
	.rx_queue_stop = NULL,
	.tx_queue_start = NULL,
	.tx_queue_stop = NULL,
	.rx_queue_setup = mrvl_rx_queue_setup,
	.rx_queue_release = NULL,
	.tx_queue_setup = mrvl_tx_queue_setup,
	.tx_queue_release = NULL,
	.flow_ctrl_get = NULL,
	.flow_ctrl_set = NULL,
	.rss_hash_update = NULL,
	.rss_hash_conf_get = NULL,
};

static uint16_t
mrvl_rx_pkt_burst(void *rxq, struct rte_mbuf **rx_pkts, uint16_t nb_pkts)
{
	struct mrvl_rxq *q = rxq;
	struct pp2_ppio_desc descs[MRVL_MAX_BURST_SIZE];
	struct rte_mbuf *mbuf;
	int i, ret;

	if (nb_pkts > MRVL_MAX_BURST_SIZE) {
		RTE_LOG(INFO, PMD, "Cannot recive %d packets in single burst\n",
			nb_pkts);
		nb_pkts = MRVL_MAX_BURST_SIZE;
	}

	ret = pp2_ppio_recv(q->priv->ppio, 0, q->queue_id, descs, &nb_pkts);
	if (ret < 0) {
		RTE_LOG(ERR, PMD, "Failed to receive packets\n");
		return 0;
	}

	for (i = 0; i < nb_pkts; i++) {
		mbuf = (struct rte_mbuf *)pp2_ppio_inq_desc_get_cookie(&descs[i]);
		mbuf->data_off += MRVL_PKT_EFFEC_OFFS;
		mbuf->pkt_len = pp2_ppio_inq_desc_get_pkt_len(&descs[i]);
		mbuf->data_len = mbuf->pkt_len;
		mbuf->port = q->port_id;

		rx_pkts[i] = mbuf;

		/* TODO: what if it fails? */
		mrvl_fill_bpool(q);
	}

	return nb_pkts;
}

static uint16_t
mrvl_tx_pkt_burst(void *txq, struct rte_mbuf **tx_pkts, uint16_t nb_pkts)
{
	struct mrvl_txq *q = txq;
	struct pp2_ppio_desc descs[MRVL_MAX_BURST_SIZE];
	int i, ret;

	if (nb_pkts > MRVL_MAX_BURST_SIZE) {
		RTE_LOG(INFO, PMD, "Cannot send %d packets in single burst\n",
			nb_pkts);
		nb_pkts = MRVL_MAX_BURST_SIZE;
	}

	for (i = 0; i < nb_pkts; i++) {
		struct rte_mbuf *mbuf = tx_pkts[i];

		pp2_ppio_outq_desc_reset(&descs[i]);
		pp2_ppio_outq_desc_set_phys_addr(&descs[i], rte_pktmbuf_mtophys(mbuf));
		pp2_ppio_outq_desc_set_pkt_offset(&descs[i], 0);
		pp2_ppio_outq_desc_set_pkt_len(&descs[i], rte_pktmbuf_pkt_len(mbuf));
	}

	ret = pp2_ppio_send(q->priv->ppio, q->priv->hif, 0, descs, &nb_pkts);
	if (ret)
		nb_pkts = 0;

	for (i = 0; i < nb_pkts; i++)
		rte_pktmbuf_free(tx_pkts[i]);

	return nb_pkts;
}

static int
mrvl_init_pp2(void)
{
	int ret, num_inst = pp2_get_num_inst();
	struct pp2_init_params init_params;

	/* TODO: Get this from DTS? */
	memset(&init_params, 0, sizeof(init_params));
	init_params.hif_reserved_map = MRVL_MUSDK_HIFS_RESERVED;
	init_params.bm_pool_reserved_map = MRVL_MUSDK_BPOOLS_RESERVED;

	/* Enable 10G port */
	init_params.ppios[0][0].is_enabled = 1;
	init_params.ppios[0][0].first_inq = 0;

	if (num_inst == 1) {
		init_params.ppios[0][2].is_enabled = 1;
		init_params.ppios[0][2].first_inq = 0;
	}
	if (num_inst == 2) {
		/* Enable 10G port */
		init_params.ppios[1][0].is_enabled = 1;
		init_params.ppios[1][0].first_inq = 0;

		/* Enable 1G ports */
		init_params.ppios[1][1].is_enabled = 1;
		init_params.ppios[1][1].first_inq = 0;
	}

	return pp2_init(&init_params);
}

static void
mrvl_deinit_pp2(void)
{
	pp2_deinit();
}

static int
mrvl_eth_dev_create(const char *drv_name, const char *name)
{
	int ret, fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct rte_eth_dev *eth_dev;
	struct mrvl_priv *priv;
	struct ifreq req;

	eth_dev = rte_eth_dev_allocate(name);
	if (!eth_dev)
		return -ENOMEM;

	priv = rte_zmalloc_socket(name, sizeof(*priv), 0, rte_socket_id());
	if (!priv) {
		ret = -ENOMEM;
		goto out_free_dev;
	}

	eth_dev->data->mac_addrs = rte_zmalloc("mac_addrs",
			ETHER_ADDR_LEN * MRVL_MAC_ADDRS_MAX, 0);
	if (!eth_dev->data->mac_addrs) {
		RTE_LOG(ERR, PMD, "Failed to allocate space for eth addrs\n");
		ret = -ENOMEM;
		goto out_free_priv;
	}

	/* TODO: temporary solution until musdk provides something similar */
	memset(&req, 0, sizeof(req));
	strcpy(req.ifr_name, name);
	ret = ioctl(fd, SIOCGIFHWADDR, &req);
	if (ret)
		goto out_free_mac;

	memcpy(eth_dev->data->mac_addrs[0].addr_bytes,
	       req.ifr_addr.sa_data, ETHER_ADDR_LEN);

	eth_dev->rx_pkt_burst = mrvl_rx_pkt_burst;
	eth_dev->tx_pkt_burst = mrvl_tx_pkt_burst;
	eth_dev->data->drv_name = drv_name;
	eth_dev->data->dev_private = priv;
	eth_dev->dev_ops = &mrvl_ops;

	return 0;
out_free_mac:
	rte_free(eth_dev->data->mac_addrs);
out_free_dev:
	rte_eth_dev_release_port(eth_dev);
out_free_priv:
	rte_free(priv);

	return ret;
}

static void
mrvl_eth_dev_destroy(const char *name)
{
	struct rte_eth_dev *eth_dev;
	struct mrvl_priv *priv;
	int i;

	eth_dev = rte_eth_dev_allocated(name);
	if (!eth_dev)
		return;

	priv = eth_dev->data->dev_private;
	/* TODO: cleanup priv before freeing? */
	rte_free(priv);
	rte_free(eth_dev->data->mac_addrs);
	rte_eth_dev_release_port(eth_dev);
}

static int
mrvl_get_ifnames(const char *key __rte_unused, const char *value, void *extra_args)
{
	static int n;
	const char **ifnames = extra_args;

	ifnames[n++] = value;

	return 0;
}

static int
rte_pmd_mrvl_probe(const char *name, const char *params)
{
	struct rte_kvargs *kvlist;
	const char *ifnames[MRVL_PP2_PORTS_MAX * MRVL_PP2_MAX];
	int i, n, ret;

	if (!name && !params)
		return -EINVAL;

	kvlist = rte_kvargs_parse(params, valid_args);
	if (!kvlist)
		return -EINVAL;

	n = rte_kvargs_count(kvlist, MRVL_IFACE_NAME_ARG);
	if (n > RTE_DIM(ifnames)) {
		ret = -EINVAL;
		goto out_free_kvlist;
	}

	rte_kvargs_process(kvlist, MRVL_IFACE_NAME_ARG,
			   mrvl_get_ifnames, &ifnames);

	ret = mv_sys_dma_mem_init(RTE_MRVL_MUSDK_DMA_MEMSIZE);
	if (ret)
		goto out_free_kvlist;

	ret = mrvl_init_pp2();
	if (ret)
		goto out_deinit_dma;

	for (i = 0; i < n; i++) {
		RTE_LOG(INFO, PMD, "Creating %s\n", ifnames[i]);
		ret = mrvl_eth_dev_create(name, ifnames[i]);
		if (ret)
			goto out_cleanup;
	}

	rte_kvargs_free(kvlist);

	return 0;
out_cleanup:
	for (; i >= 0; i--)
		mrvl_eth_dev_destroy(ifnames[i]);
out_deinit_pp2:
	mrvl_deinit_pp2();
out_deinit_dma:
	mv_sys_dma_mem_destroy();
out_free_kvlist:
	rte_kvargs_free(kvlist);

	return ret;
}

static int
rte_pmd_mrvl_remove(const char *name)
{
	int i;

	if (!name)
		return -EINVAL;

	RTE_LOG(INFO, PMD, "Removing %s\n", name);

	for (i = 0; i < rte_eth_dev_count(); i++) {
		char ifname[RTE_ETH_NAME_MAX_LEN];

		rte_eth_dev_get_name_by_port(i, ifname);
		mrvl_eth_dev_destroy(ifname);
	}

	mrvl_deinit_pp2();
	mv_sys_dma_mem_destroy();

	return 0;
}

static struct rte_vdev_driver pmd_mrvl_drv = {
	.probe = rte_pmd_mrvl_probe,
	.remove = rte_pmd_mrvl_remove,
};

RTE_PMD_REGISTER_VDEV(net_mrvl, pmd_mrvl_drv);
RTE_PMD_REGISTER_ALIAS(net_mrvl, hif);
