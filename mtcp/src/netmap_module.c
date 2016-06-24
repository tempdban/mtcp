/* for io_module_func def'ns */
#include "io_module.h"
#ifndef DISABLE_NETMAP
/* for mtcp related def'ns */
#include "mtcp.h"
/* for errno */
#include <errno.h>
/* for logging */
#include "debug.h"
/* for num_devices_* */
#include "config.h"
/* for netmap definitions */
#define NETMAP_WITH_LIBS
#include "netmap_user.h"
/* for poll */
#include <sys/poll.h>
/*----------------------------------------------------------------------------*/
#define MAX_RX_PKT_BURST		128
#define MAX_TX_PKT_BURST		8
#define ETHERNET_FRAME_SIZE		1514
#define MAX_IFNAMELEN			(IF_NAMESIZE + 10)
#define EXTRA_BUFS			512
/*----------------------------------------------------------------------------*/

struct netmap_private_context {
	struct nm_desc *local_nmd[MAX_DEVICES];
	unsigned char snd_pktbuf[MAX_DEVICES][MAX_TX_PKT_BURST][ETHERNET_FRAME_SIZE];
	unsigned char *rcv_pktbuf[MAX_RX_PKT_BURST];
	uint16_t rcv_pkt_len[MAX_RX_PKT_BURST];
	uint16_t snd_pkt_size[MAX_DEVICES][MAX_TX_PKT_BURST];
	uint8_t to_send[MAX_DEVICES];
	uint8_t dev_poll_flag;
} __attribute__((aligned(__WORDSIZE)));
/*----------------------------------------------------------------------------*/
void
netmap_init_handle(struct mtcp_thread_context *ctxt)
{
	struct netmap_private_context *npc;
	char ifname[MAX_IFNAMELEN];
	char nifname[MAX_IFNAMELEN];
	int j;
	int eidx = 0;

	/* create and initialize private I/O module context */
	ctxt->io_private_context = calloc(1, sizeof(struct netmap_private_context));
	if (ctxt->io_private_context == NULL) {
		TRACE_ERROR("Failed to initialize ctxt->io_private_context: "
			    "Can't allocate memory\n");
		exit(EXIT_FAILURE);
	}
	
	npc = (struct netmap_private_context *)ctxt->io_private_context;

	npc->dev_poll_flag = 1;

	/* initialize per-thread netmap interfaces  */
	for (j = 0; j < num_devices_attached; j++) {
		eidx = devices_attached[j];
		strcpy(ifname, CONFIG.eths[eidx].dev_name);

		if (strstr(ifname, "vale") != NULL)
			strcpy(nifname, ifname);
		else if (unlikely(CONFIG.num_cores == 1))
			sprintf(nifname, "netmap:%s", ifname);
		else
			sprintf(nifname, "netmap:%s-%d", ifname, ctxt->cpu);
		
		TRACE_INFO("Opening %s with j: %d (cpu: %d)\n", nifname, j, ctxt->cpu);

		struct nmreq base_nmd;
		memset(&base_nmd, 0, sizeof(base_nmd));
		base_nmd.nr_arg3 = EXTRA_BUFS;

		npc->local_nmd[j] = nm_open(nifname, &base_nmd, 0, NULL);
		if (npc->local_nmd[j] == NULL) {
			TRACE_ERROR("Unable to open %s: %s\n",
				    nifname, strerror(errno));
			exit(EXIT_FAILURE);
		}
	}
}
/*----------------------------------------------------------------------------*/
int
netmap_link_devices(struct mtcp_thread_context *ctxt)
{
	/* linking takes place during mtcp_init() */
	
	return 0;
}
/*----------------------------------------------------------------------------*/
void
netmap_release_pkt(struct mtcp_thread_context *ctxt, int ifidx, unsigned char *pkt_data, int len)
{
	/* 
	 * do nothing over here - memory reclamation
	 * will take place in dpdk_recv_pkts 
	 */
}
/*----------------------------------------------------------------------------*/
int
netmap_send_pkts(struct mtcp_thread_context *ctxt, int nif)
{
	int cnt, idx;
	struct netmap_private_context *npc;
	mtcp_manager_t mtcp;

	npc = (struct netmap_private_context *)ctxt->io_private_context;
	idx = nif;
	mtcp = ctxt->mtcp_manager;

	/* assert-type statement */
	if (npc->to_send[idx] == 0) return 0;

	for (cnt = 0; cnt < npc->to_send[idx]; cnt++) {
#ifdef NETSTAT
		mtcp->nstat.tx_packets[nif]++;
		mtcp->nstat.tx_bytes[nif] += npc->snd_pkt_size[idx][cnt] + 24;
#endif

		if (nm_inject(npc->local_nmd[idx], npc->snd_pktbuf[idx][cnt],
			npc->snd_pkt_size[idx][cnt]) == 0) {
			TRACE_DBG("Failed to send pkt of size %d on interface: %d\n",
				  npc->snd_pkt_size[idx][cnt], idx);
#ifdef NETSTAT
			mtcp->nstat.rx_errors[idx]++;
#endif
			ioctl(npc->local_nmd[idx]->fd, NIOCTXSYNC, NULL);
		}

		npc->snd_pkt_size[idx][cnt] = 0;
	}

	npc->to_send[idx] = 0;

	return cnt;
}
/*----------------------------------------------------------------------------*/
uint8_t *
netmap_get_wptr(struct mtcp_thread_context *ctxt, int nif, uint16_t pktsize)
{
	struct netmap_private_context *npc;
	int idx = nif;
	uint8_t cnt;

	npc = (struct netmap_private_context *)ctxt->io_private_context;
	if (npc->to_send[idx] == MAX_TX_PKT_BURST)
		netmap_send_pkts(ctxt, nif);

	cnt = npc->to_send[idx]++;
	npc->snd_pkt_size[idx][cnt] = pktsize;

	return (uint8_t *)npc->snd_pktbuf[idx][cnt];
}
/*----------------------------------------------------------------------------*/
int32_t
netmap_recv_pkts(struct mtcp_thread_context *ctxt, int ifidx)
{
	struct netmap_private_context *npc;
	struct nm_desc *d;
	npc = (struct netmap_private_context *)ctxt->io_private_context;
	d = npc->local_nmd[ifidx];

	int p = 0;
	int c, got = 0, ri = d->cur_rx_ring;
	int n = d->last_rx_ring - d->first_rx_ring + 1;
	int cnt = MAX_RX_PKT_BURST;



	for (c = 0; c < n && cnt != got; c++) {
		/* compute current ring to use */
		struct netmap_ring *ring;
		
		ri = d->cur_rx_ring + c;
		if (ri > d->last_rx_ring)
			ri = d->first_rx_ring;
		ring = NETMAP_RXRING(d->nifp, ri);
		for ( ; !nm_ring_empty(ring) && cnt != got; got++) {
			u_int i = ring->cur;
			u_int idx = ring->slot[i].buf_idx;
			npc->rcv_pktbuf[p] = (u_char *)NETMAP_BUF(ring, idx);
			npc->rcv_pkt_len[p] = ring->slot[i].len;
			p++;
			ring->head = ring->cur = nm_ring_next(ring, i);
		}
	}
	d->cur_rx_ring = ri;

	return p;
}
/*----------------------------------------------------------------------------*/
uint8_t *
netmap_get_rptr(struct mtcp_thread_context *ctxt, int ifidx, int index, uint16_t *len)
{
	struct netmap_private_context *npc;
	npc = (struct netmap_private_context *)ctxt->io_private_context;

	*len = npc->rcv_pkt_len[index];
	return (unsigned char *)npc->rcv_pktbuf[index];
}
/*----------------------------------------------------------------------------*/
int32_t
netmap_select(struct mtcp_thread_context *ctxt)
{
	struct netmap_private_context *npc = 
		(struct netmap_private_context *)ctxt->io_private_context;
	
	if (npc->local_nmd[0] == NULL)	return -1;
	struct pollfd pfd = { .fd = npc->local_nmd[0]->fd, .events = POLLIN };

	do { 
		int i = poll(&pfd, 1, 1000);
		if (i > 0 && !(pfd.revents & POLLERR)) {
			npc->dev_poll_flag = 1;
			break;
		}
	} while (npc->dev_poll_flag == 0);
	
	if (pfd.revents & POLLERR) {
		TRACE_ERROR("Poll failed! (cpu: %d\n, err: %d)\n",
			    ctxt->cpu, errno);
		exit(EXIT_FAILURE);
	}
	
	return 0;
}
/*----------------------------------------------------------------------------*/
void
netmap_destroy_handle(struct mtcp_thread_context *ctxt)
{
}
/*----------------------------------------------------------------------------*/
void
netmap_load_module(void)
{
	/* not needed - all initializations done in netmap_init_handle() */
}
/*----------------------------------------------------------------------------*/
io_module_func netmap_module_func = {
	.load_module		   = netmap_load_module,
	.init_handle		   = netmap_init_handle,
	.link_devices		   = netmap_link_devices,
	.release_pkt		   = netmap_release_pkt,
	.send_pkts		   = netmap_send_pkts,
	.get_wptr   		   = netmap_get_wptr,
	.recv_pkts		   = netmap_recv_pkts,
	.get_rptr	   	   = netmap_get_rptr,
	.select			   = netmap_select,
	.destroy_handle		   = netmap_destroy_handle
};
/*----------------------------------------------------------------------------*/
#else
io_module_func netmap_module_func = {
	.load_module		   = NULL,
	.init_handle		   = NULL,
	.link_devices		   = NULL,
	.release_pkt		   = NULL,
	.send_pkts		   = NULL,
	.get_wptr   		   = NULL,
	.recv_pkts		   = NULL,
	.get_rptr	   	   = NULL,
	.select			   = NULL,
	.destroy_handle		   = NULL
};
/*----------------------------------------------------------------------------*/
#endif /* !DISABLE_NETMAP */
