/*
 * Copyright (c) 2015-2017, Stanford University
 *  
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  * Redistributions of source code must retain the above copyright notice, 
 *    this list of conditions and the following disclaimer.
 * 
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 *  * Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright 2013-16 Board of Trustees of Stanford University
 * Copyright 2013-16 Ecole Polytechnique Federale Lausanne (EPFL)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * Transmission Control Protocol, outgoing traffic
 *
 * The output functions of TCP.
 *
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 */

#include "lwip/opt.h"

#if LWIP_TCP /* don't build if not configured for use in lwipopts.h */

#include "lwip/tcp_impl.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/inet_chksum.h"
#include "lwip/stats.h"
#include "lwip/snmp.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/inet_chksum.h"
#if LWIP_TCP_TIMESTAMPS
#include "lwip/sys.h"
#endif

#include <string.h>
#include <assert.h>

// direct into IX (tcp_api)
extern int tcp_output_packet(struct eth_fg *,struct tcp_pcb *pcb, struct pbuf *p);

/* Define some copy-macros for checksum-on-copy so that the code looks
   nicer by preventing too many ifdef's. */
#if TCP_CHECKSUM_ON_COPY
#define TCP_DATA_COPY(dst, src, len, seg) do { \
  tcp_seg_add_chksum(LWIP_CHKSUM_COPY(dst, src, len), \
                     len, &seg->chksum, &seg->chksum_swapped); \
  seg->flags |= TF_SEG_DATA_CHECKSUMMED; } while(0)
#define TCP_DATA_COPY2(dst, src, len, chksum, chksum_swapped)  \
  tcp_seg_add_chksum(LWIP_CHKSUM_COPY(dst, src, len), len, chksum, chksum_swapped);
#else /* TCP_CHECKSUM_ON_COPY*/
#define TCP_DATA_COPY(dst, src, len, seg)                     MEMCPY(dst, src, len)
#define TCP_DATA_COPY2(dst, src, len, chksum, chksum_swapped) MEMCPY(dst, src, len)
#endif /* TCP_CHECKSUM_ON_COPY*/

/** Define this to 1 for an extra check that the output checksum is valid
 * (usefule when the checksum is generated by the application, not the stack) */
#ifndef TCP_CHECKSUM_ON_COPY_SANITY_CHECK
#define TCP_CHECKSUM_ON_COPY_SANITY_CHECK   0
#endif

/* Forward declarations.*/
static void tcp_output_segment(struct eth_fg *cur_fg,struct tcp_seg *seg, struct tcp_pcb *pcb);

/** Allocate a pbuf and create a tcphdr at p->payload, used for output
 * functions other than the default tcp_output -> tcp_output_segment
 * (e.g. tcp_send_empty_ack, etc.)
 *
 * @param pcb tcp pcb for which to send a packet (used to initialize tcp_hdr)
 * @param optlen length of header-options
 * @param datalen length of tcp data to reserve in pbuf
 * @param seqno_be seqno in network byte order (big-endian)
 * @return pbuf with p->payload being the tcp_hdr
 */
static struct pbuf *
tcp_output_alloc_header(struct tcp_pcb *pcb, u16_t optlen, u16_t datalen,
                      u32_t seqno_be /* already in network byte order */)
{
  struct tcp_hdr *tcphdr;
  struct pbuf *p = pbuf_alloc(PBUF_IP, TCP_HLEN + optlen + datalen, PBUF_RAM);
  if (p != NULL) {
    LWIP_ASSERT("check that first pbuf can hold struct tcp_hdr",
                 (p->len >= TCP_HLEN + optlen));
    tcphdr = (struct tcp_hdr *)p->payload;
    tcphdr->src = htons(pcb->local_port);
    tcphdr->dest = htons(pcb->remote_port);
    tcphdr->seqno = seqno_be;
    tcphdr->ackno = htonl(pcb->rcv_nxt);
    TCPH_HDRLEN_FLAGS_SET(tcphdr, (5 + optlen / 4), TCP_ACK);
    tcphdr->wnd = htons(RCV_WND_SCALE(pcb, pcb->rcv_ann_wnd));
    tcphdr->chksum = 0;
    tcphdr->urgp = 0;

    /* If we're sending a packet, update the announced right window edge */
    pcb->rcv_ann_right_edge = pcb->rcv_nxt + pcb->rcv_ann_wnd;
  }
  return p;
}

/**
 * Called by tcp_close() to send a segment including FIN flag but not data.
 *
 * @param pcb the tcp_pcb over which to send a segment
 * @return ERR_OK if sent, another err_t otherwise
 */
err_t
tcp_send_fin(struct tcp_pcb *pcb)
{
  /* first, try to add the fin to the last unsent segment */
  if (pcb->unsent != NULL) {
    struct tcp_seg *last_unsent;
    for (last_unsent = pcb->unsent; last_unsent->next != NULL;
         last_unsent = last_unsent->next);

    if ((TCPH_FLAGS(last_unsent->tcphdr) & (TCP_SYN | TCP_FIN | TCP_RST)) == 0) {
      /* no SYN/FIN/RST flag in the header, we can add the FIN flag */
      TCPH_SET_FLAG(last_unsent->tcphdr, TCP_FIN);
      pcb->flags |= TF_FIN;
      return ERR_OK;
    }
  }
  /* no data, no length, flags, copy=1, no optdata */
  return tcp_enqueue_flags(pcb, TCP_FIN);
}

/**
 * Create a TCP segment with prefilled header.
 *
 * Called by tcp_write and tcp_enqueue_flags.
 *
 * @param pcb Protocol control block for the TCP connection.
 * @param p pbuf that is used to hold the TCP header.
 * @param flags TCP flags for header.
 * @param seqno TCP sequence number of this packet
 * @param optflags options to include in TCP header
 * @return a new tcp_seg pointing to p, or NULL.
 * The TCP header is filled in except ackno and wnd.
 * p is freed on failure.
 */
static struct tcp_seg *
tcp_create_segment(struct tcp_pcb *pcb, struct pbuf *p, u8_t flags, u32_t seqno, u8_t optflags)
{
  struct tcp_seg *seg;
  u8_t optlen = LWIP_TCP_OPT_LENGTH(optflags);

  if ((seg = (struct tcp_seg *)memp_malloc(MEMP_TCP_SEG)) == NULL) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | 2, ("tcp_create_segment: no memory.\n"));
    pbuf_free(p);
    return NULL;
  }
  MEMPOOL_SANITY_LINK(pcb,p);
  MEMPOOL_SANITY_LINK(pcb,seg);

  seg->flags = optflags;
  seg->next = NULL;
  seg->p = p;
  seg->len = p->tot_len - optlen;
#if TCP_OVERSIZE_DBGCHECK
  seg->oversize_left = 0;
#endif /* TCP_OVERSIZE_DBGCHECK */
#if TCP_CHECKSUM_ON_COPY
  seg->chksum = 0;
  seg->chksum_swapped = 0;
  /* check optflags */
  LWIP_ASSERT("invalid optflags passed: TF_SEG_DATA_CHECKSUMMED",
              (optflags & TF_SEG_DATA_CHECKSUMMED) == 0);
#endif /* TCP_CHECKSUM_ON_COPY */

  /* build TCP header */
  if (pbuf_header(p, TCP_HLEN)) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | 2, ("tcp_create_segment: no room for TCP header in pbuf.\n"));
    TCP_STATS_INC(tcp.err);
    tcp_seg_free(seg);
    return NULL;
  }
  seg->tcphdr = (struct tcp_hdr *)seg->p->payload;
  seg->tcphdr->src = htons(pcb->local_port);
  seg->tcphdr->dest = htons(pcb->remote_port);
  seg->tcphdr->seqno = htonl(seqno);
  /* ackno is set in tcp_output */
  TCPH_HDRLEN_FLAGS_SET(seg->tcphdr, (5 + optlen / 4), flags);
  /* wnd and chksum are set in tcp_output */
  seg->tcphdr->urgp = 0;
  return seg;
}

/**
 * Allocate a PBUF_RAM pbuf, perhaps with extra space at the end.
 *
 * This function is like pbuf_alloc(layer, length, PBUF_RAM) except
 * there may be extra bytes available at the end.
 *
 * @param layer flag to define header size.
 * @param length size of the pbuf's payload.
 * @param max_length maximum usable size of payload+oversize.
 * @param oversize pointer to a u16_t that will receive the number of usable tail bytes.
 * @param pcb The TCP connection that willo enqueue the pbuf.
 * @param apiflags API flags given to tcp_write.
 * @param first_seg true when this pbuf will be used in the first enqueued segment.
 * @param
 */
#if TCP_OVERSIZE
static struct pbuf *
tcp_pbuf_prealloc(pbuf_layer layer, u16_t length, u16_t max_length,
                  u16_t *oversize, struct tcp_pcb *pcb, u8_t apiflags,
                  u8_t first_seg)
{
  struct pbuf *p;
  u16_t alloc = length;

#if LWIP_NETIF_TX_SINGLE_PBUF
  LWIP_UNUSED_ARG(max_length);
  LWIP_UNUSED_ARG(pcb);
  LWIP_UNUSED_ARG(apiflags);
  LWIP_UNUSED_ARG(first_seg);
  /* always create MSS-sized pbufs */
  alloc = max_length;
#else /* LWIP_NETIF_TX_SINGLE_PBUF */
  if (length < max_length) {
    /* Should we allocate an oversized pbuf, or just the minimum
     * length required? If tcp_write is going to be called again
     * before this segment is transmitted, we want the oversized
     * buffer. If the segment will be transmitted immediately, we can
     * save memory by allocating only length. We use a simple
     * heuristic based on the following information:
     *
     * Did the user set TCP_WRITE_FLAG_MORE?
     *
     * Will the Nagle algorithm defer transmission of this segment?
     */
    if ((apiflags & TCP_WRITE_FLAG_MORE) ||
        (!(pcb->flags & TF_NODELAY) &&
         (!first_seg ||
          pcb->unsent != NULL ||
          pcb->unacked != NULL))) {
      alloc = LWIP_MIN(max_length, LWIP_MEM_ALIGN_SIZE(length + TCP_OVERSIZE));
    }
  }
#endif /* LWIP_NETIF_TX_SINGLE_PBUF */
  p = pbuf_alloc(layer, alloc, PBUF_RAM);
  if (p == NULL) {
    return NULL;
  }
  LWIP_ASSERT("need unchained pbuf", p->next == NULL);
  *oversize = p->len - length;
  /* trim p->len to the currently used size */
  p->len = p->tot_len = length;
  return p;
}
#else /* TCP_OVERSIZE */
#define tcp_pbuf_prealloc(layer, length, mx, os, pcb, api, fst) pbuf_alloc((layer), (length), PBUF_RAM)
#endif /* TCP_OVERSIZE */

#if TCP_CHECKSUM_ON_COPY
/** Add a checksum of newly added data to the segment */
static void
tcp_seg_add_chksum(u16_t chksum, u16_t len, u16_t *seg_chksum,
                   u8_t *seg_chksum_swapped)
{
  u32_t helper;
  /* add chksum to old chksum and fold to u16_t */
  helper = chksum + *seg_chksum;
  chksum = FOLD_U32T(helper);
  if ((len & 1) != 0) {
    *seg_chksum_swapped = 1 - *seg_chksum_swapped;
    chksum = SWAP_BYTES_IN_WORD(chksum);
  }
  *seg_chksum = chksum;
}
#endif /* TCP_CHECKSUM_ON_COPY */

/** Checks if tcp_write is allowed or not (checks state, snd_buf and snd_queuelen).
 *
 * @param pcb the tcp pcb to check for
 * @param len length of data to send (checked agains snd_buf)
 * @return ERR_OK if tcp_write is allowed to proceed, another err_t otherwise
 */
static err_t
tcp_write_checks(struct tcp_pcb *pcb, u16_t len)
{
  /* connection is in invalid state for data transmission? */
  if ((pcb->state != ESTABLISHED) &&
      (pcb->state != CLOSE_WAIT) &&
      (pcb->state != SYN_SENT) &&
      (pcb->state != SYN_RCVD)) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_STATE | LWIP_DBG_LEVEL_SEVERE, ("tcp_write() called in invalid state\n"));
    return ERR_CONN;
  } else if (len == 0) {
    return ERR_OK;
  }

  /* fail on too much data */
  if (len > pcb->snd_buf) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | 3, ("tcp_write: too much data (len=%"U16_F" > snd_buf=%"U16_F")\n",
      len, pcb->snd_buf));
    pcb->flags |= TF_NAGLEMEMERR;
    return ERR_MEM;
  }

  LWIP_DEBUGF(TCP_QLEN_DEBUG, ("tcp_write: queuelen: %"TCPWNDSIZE_F"\n", (tcpwnd_size_t)pcb->snd_queuelen));

  /* If total number of pbufs on the unsent/unacked queues exceeds the
   * configured maximum, return an error */
  /* check for configured max queuelen and possible overflow */
  if ((pcb->snd_queuelen >= TCP_SND_QUEUELEN) || (pcb->snd_queuelen > TCP_SNDQUEUELEN_OVERFLOW)) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | 3, ("tcp_write: too long queue %"TCPWNDSIZE_F" (max %"TCPWNDSIZE_F")\n",
      pcb->snd_queuelen, TCP_SND_QUEUELEN));
    TCP_STATS_INC(tcp.memerr);
    pcb->flags |= TF_NAGLEMEMERR;
    return ERR_MEM;
  }
  if (pcb->snd_queuelen != 0) {
    LWIP_ASSERT("tcp_write: pbufs on queue => at least one queue non-empty",
      pcb->unacked != NULL || pcb->unsent != NULL);
  } else {
    LWIP_ASSERT("tcp_write: no pbufs on queue => both queues empty",
      pcb->unacked == NULL && pcb->unsent == NULL);
  }
  return ERR_OK;
}

/**
 * Write data for sending (but does not send it immediately).
 *
 * It waits in the expectation of more data being sent soon (as
 * it can send them more efficiently by combining them together).
 * To prompt the system to send data now, call tcp_output() after
 * calling tcp_write().
 *
 * @param pcb Protocol control block for the TCP connection to enqueue data for.
 * @param arg Pointer to the data to be enqueued for sending.
 * @param len Data length in bytes
 * @param apiflags combination of following flags :
 * - TCP_WRITE_FLAG_COPY (0x01) data will be copied into memory belonging to the stack
 * - TCP_WRITE_FLAG_MORE (0x02) for TCP connection, PSH flag will be set on last segment sent,
 * @return ERR_OK if enqueued, another err_t on error
 */
err_t
tcp_write(struct tcp_pcb *pcb, const void *arg, u16_t len, u8_t apiflags)
{
  struct pbuf *concat_p = NULL;
  struct tcp_seg *last_unsent = NULL, *seg = NULL, *prev_seg = NULL, *queue = NULL;
  u16_t pos = 0; /* position in 'arg' data */
  u16_t queuelen;
  u8_t optlen = 0;
  u8_t optflags = 0;
#if TCP_OVERSIZE
  u16_t oversize = 0;
  u16_t oversize_used = 0;
#endif /* TCP_OVERSIZE */
#if TCP_CHECKSUM_ON_COPY
  u16_t concat_chksum = 0;
  u8_t concat_chksum_swapped = 0;
  u16_t concat_chksummed = 0;
#endif /* TCP_CHECKSUM_ON_COPY */
  err_t err;
  /* don't allocate segments bigger than half the maximum window we ever received */
  u16_t mss_local = LWIP_MIN(pcb->mss, pcb->snd_wnd_max/2);

#if LWIP_NETIF_TX_SINGLE_PBUF
  /* Always copy to try to create single pbufs for TX */
  apiflags |= TCP_WRITE_FLAG_COPY;
#endif /* LWIP_NETIF_TX_SINGLE_PBUF */

  LWIP_DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_write(pcb=%p, data=%p, len=%"U16_F", apiflags=%"U16_F")\n",
    (void *)pcb, arg, len, (u16_t)apiflags));
  LWIP_ERROR("tcp_write: arg == NULL (programmer violates API)",
             arg != NULL, return ERR_ARG;);

  err = tcp_write_checks(pcb, len);
  if (err != ERR_OK) {
    return err;
  }
  queuelen = pcb->snd_queuelen;

#if LWIP_TCP_TIMESTAMPS
  if ((pcb->flags & TF_TIMESTAMP)) {
    /* Make sure the timestamp option is only included in data segments if we
       agreed about it with the remote host. */
    optflags = TF_SEG_OPTS_TS;
    optlen = LWIP_TCP_OPT_LENGTH(TF_SEG_OPTS_TS);
  }
#endif /* LWIP_TCP_TIMESTAMPS */


  /*
   * TCP segmentation is done in three phases with increasing complexity:
   *
   * 1. Copy data directly into an oversized pbuf.
   * 2. Chain a new pbuf to the end of pcb->unsent.
   * 3. Create new segments.
   *
   * We may run out of memory at any point. In that case we must
   * return ERR_MEM and not change anything in pcb. Therefore, all
   * changes are recorded in local variables and committed at the end
   * of the function. Some pcb fields are maintained in local copies:
   *
   * queuelen = pcb->snd_queuelen
   * oversize = pcb->unsent_oversize
   *
   * These variables are set consistently by the phases:
   *
   * seg points to the last segment tampered with.
   *
   * pos records progress as data is segmented.
   */

  /* Find the tail of the unsent queue. */
  if (pcb->unsent != NULL) {
    u16_t space;
    u16_t unsent_optlen;

    /* @todo: this could be sped up by keeping last_unsent in the pcb */
    for (last_unsent = pcb->unsent; last_unsent->next != NULL;
         last_unsent = last_unsent->next);

    /* Usable space at the end of the last unsent segment */
    unsent_optlen = LWIP_TCP_OPT_LENGTH(last_unsent->flags);
    space = mss_local - (last_unsent->len + unsent_optlen);

    /*
     * Phase 1: Copy data directly into an oversized pbuf.
     *
     * The number of bytes copied is recorded in the oversize_used
     * variable. The actual copying is done at the bottom of the
     * function.
     */
#if TCP_OVERSIZE
#if TCP_OVERSIZE_DBGCHECK
    /* check that pcb->unsent_oversize matches last_unsent->unsent_oversize */
    LWIP_ASSERT("unsent_oversize mismatch (pcb vs. last_unsent)",
                pcb->unsent_oversize == last_unsent->oversize_left);
#endif /* TCP_OVERSIZE_DBGCHECK */
    oversize = pcb->unsent_oversize;
    if (oversize > 0) {
      LWIP_ASSERT("inconsistent oversize vs. space", oversize_used <= space);
      seg = last_unsent;
      oversize_used = oversize < len ? oversize : len;
      pos += oversize_used;
      oversize -= oversize_used;
      space -= oversize_used;
    }
    /* now we are either finished or oversize is zero */
    LWIP_ASSERT("inconsistend oversize vs. len", (oversize == 0) || (pos == len));
#endif /* TCP_OVERSIZE */

    /*
     * Phase 2: Chain a new pbuf to the end of pcb->unsent.
     *
     * We don't extend segments containing SYN/FIN flags or options
     * (len==0). The new pbuf is kept in concat_p and pbuf_cat'ed at
     * the end.
     */
    if ((pos < len) && (space > 0) && (last_unsent->len > 0)) {
      u16_t seglen = space < len - pos ? space : len - pos;
      seg = last_unsent;

      /* Create a pbuf with a copy or reference to seglen bytes. We
       * can use PBUF_RAW here since the data appears in the middle of
       * a segment. A header will never be prepended. */
      if (apiflags & TCP_WRITE_FLAG_COPY) {
        /* Data is copied */
        if ((concat_p = tcp_pbuf_prealloc(PBUF_RAW, seglen, space, &oversize, pcb, apiflags, 1)) == NULL) {
          LWIP_DEBUGF(TCP_OUTPUT_DEBUG | 2,
                      ("tcp_write : could not allocate memory for pbuf copy size %"U16_F"\n",
                       seglen));
          goto memerr;
        }
#if TCP_OVERSIZE_DBGCHECK
        last_unsent->oversize_left += oversize;
#endif /* TCP_OVERSIZE_DBGCHECK */
        TCP_DATA_COPY2(concat_p->payload, (u8_t*)arg + pos, seglen, &concat_chksum, &concat_chksum_swapped);
#if TCP_CHECKSUM_ON_COPY
        concat_chksummed += seglen;
#endif /* TCP_CHECKSUM_ON_COPY */
      } else {
        /* Data is not copied */
        if ((concat_p = pbuf_alloc(PBUF_RAW, seglen, PBUF_ROM)) == NULL) {
          LWIP_DEBUGF(TCP_OUTPUT_DEBUG | 2,
                      ("tcp_write: could not allocate memory for zero-copy pbuf\n"));
          goto memerr;
        }
#if TCP_CHECKSUM_ON_COPY
        /* calculate the checksum of nocopy-data */
        tcp_seg_add_chksum(~inet_chksum((u8_t*)arg + pos, seglen), seglen,
          &concat_chksum, &concat_chksum_swapped);
        concat_chksummed += seglen;
#endif /* TCP_CHECKSUM_ON_COPY */
        /* reference the non-volatile payload data */
        concat_p->payload = (u8_t*)arg + pos;
      }

      pos += seglen;
      queuelen += pbuf_clen(concat_p);
    }
  } else {
#if TCP_OVERSIZE
    LWIP_ASSERT("unsent_oversize mismatch (pcb->unsent is NULL)",
                pcb->unsent_oversize == 0);
#endif /* TCP_OVERSIZE */
  }

  /*
   * Phase 3: Create new segments.
   *
   * The new segments are chained together in the local 'queue'
   * variable, ready to be appended to pcb->unsent.
   */
  while (pos < len) {
    struct pbuf *p;
    u16_t left = len - pos;
    u16_t max_len = mss_local - optlen;
    u16_t seglen = left > max_len ? max_len : left;
#if TCP_CHECKSUM_ON_COPY
    u16_t chksum = 0;
    u8_t chksum_swapped = 0;
#endif /* TCP_CHECKSUM_ON_COPY */

    if (apiflags & TCP_WRITE_FLAG_COPY) {
      /* If copy is set, memory should be allocated and data copied
       * into pbuf */
      if ((p = tcp_pbuf_prealloc(PBUF_TRANSPORT, seglen + optlen, mss_local, &oversize, pcb, apiflags, queue == NULL)) == NULL) {
        LWIP_DEBUGF(TCP_OUTPUT_DEBUG | 2, ("tcp_write : could not allocate memory for pbuf copy size %"U16_F"\n", seglen));
        goto memerr;
      }
      LWIP_ASSERT("tcp_write: check that first pbuf can hold the complete seglen",
                  (p->len >= seglen));
      TCP_DATA_COPY2((char *)p->payload + optlen, (u8_t*)arg + pos, seglen, &chksum, &chksum_swapped);
    } else {
      /* Copy is not set: First allocate a pbuf for holding the data.
       * Since the referenced data is available at least until it is
       * sent out on the link (as it has to be ACKed by the remote
       * party) we can safely use PBUF_ROM instead of PBUF_REF here.
       */
      struct pbuf *p2;
#if TCP_OVERSIZE
      LWIP_ASSERT("oversize == 0", oversize == 0);
#endif /* TCP_OVERSIZE */
      if ((p2 = pbuf_alloc(PBUF_TRANSPORT, seglen, PBUF_ROM)) == NULL) {
        LWIP_DEBUGF(TCP_OUTPUT_DEBUG | 2, ("tcp_write: could not allocate memory for zero-copy pbuf\n"));
        goto memerr;
      }
#if TCP_CHECKSUM_ON_COPY
      /* calculate the checksum of nocopy-data */
      chksum = ~inet_chksum((u8_t*)arg + pos, seglen);
#endif /* TCP_CHECKSUM_ON_COPY */
      /* reference the non-volatile payload data */
      p2->payload = (u8_t*)arg + pos;

      /* Second, allocate a pbuf for the headers. */
      if ((p = pbuf_alloc(PBUF_TRANSPORT, optlen, PBUF_RAM)) == NULL) {
        /* If allocation fails, we have to deallocate the data pbuf as
         * well. */
        pbuf_free(p2);
        LWIP_DEBUGF(TCP_OUTPUT_DEBUG | 2, ("tcp_write: could not allocate memory for header pbuf\n"));
        goto memerr;
      }
      /* Concatenate the headers and data pbufs together. */
      pbuf_cat(p/*header*/, p2/*data*/);
    }

    queuelen += pbuf_clen(p);

    /* Now that there are more segments queued, we check again if the
     * length of the queue exceeds the configured maximum or
     * overflows. */
    if ((queuelen > TCP_SND_QUEUELEN) || (queuelen > TCP_SNDQUEUELEN_OVERFLOW)) {
      LWIP_DEBUGF(TCP_OUTPUT_DEBUG | 2, ("tcp_write: queue too long %"TCPWNDSIZE_F" (%"TCPWNDSIZE_F")\n", queuelen, TCP_SND_QUEUELEN));
      pbuf_free(p);
      goto memerr;
    }

    if ((seg = tcp_create_segment(pcb, p, 0, pcb->snd_lbb + pos, optflags)) == NULL) {
      goto memerr;
    }
#if TCP_OVERSIZE_DBGCHECK
    seg->oversize_left = oversize;
#endif /* TCP_OVERSIZE_DBGCHECK */
#if TCP_CHECKSUM_ON_COPY
    seg->chksum = chksum;
    seg->chksum_swapped = chksum_swapped;
    seg->flags |= TF_SEG_DATA_CHECKSUMMED;
#endif /* TCP_CHECKSUM_ON_COPY */

    /* first segment of to-be-queued data? */
    if (queue == NULL) {
      queue = seg;
    } else {
      /* Attach the segment to the end of the queued segments */
      LWIP_ASSERT("prev_seg != NULL", prev_seg != NULL);
      prev_seg->next = seg;
    }
    /* remember last segment of to-be-queued data for next iteration */
    prev_seg = seg;

    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_TRACE, ("tcp_write: queueing %"U32_F":%"U32_F"\n",
      ntohl(seg->tcphdr->seqno),
      ntohl(seg->tcphdr->seqno) + TCP_TCPLEN(seg)));

    pos += seglen;
  }

  /*
   * All three segmentation phases were successful. We can commit the
   * transaction.
   */

  /*
   * Phase 1: If data has been added to the preallocated tail of
   * last_unsent, we update the length fields of the pbuf chain.
   */
#if TCP_OVERSIZE
  if (oversize_used > 0) {
    struct pbuf *p;
    /* Bump tot_len of whole chain, len of tail */
    for (p = last_unsent->p; p; p = p->next) {
      p->tot_len += oversize_used;
      if (p->next == NULL) {
        TCP_DATA_COPY((char *)p->payload + p->len, arg, oversize_used, last_unsent);
        p->len += oversize_used;
      }
    }
    last_unsent->len += oversize_used;
#if TCP_OVERSIZE_DBGCHECK
    LWIP_ASSERT("last_unsent->oversize_left >= oversize_used",
                last_unsent->oversize_left >= oversize_used);
    last_unsent->oversize_left -= oversize_used;
#endif /* TCP_OVERSIZE_DBGCHECK */
  }
  pcb->unsent_oversize = oversize;
#endif /* TCP_OVERSIZE */

  /*
   * Phase 2: concat_p can be concatenated onto last_unsent->p
   */
  if (concat_p != NULL) {
    LWIP_ASSERT("tcp_write: cannot concatenate when pcb->unsent is empty",
      (last_unsent != NULL));
    pbuf_cat(last_unsent->p, concat_p);
    last_unsent->len += concat_p->tot_len;
#if TCP_CHECKSUM_ON_COPY
    if (concat_chksummed) {
      tcp_seg_add_chksum(concat_chksum, concat_chksummed, &last_unsent->chksum,
        &last_unsent->chksum_swapped);
      last_unsent->flags |= TF_SEG_DATA_CHECKSUMMED;
    }
#endif /* TCP_CHECKSUM_ON_COPY */
  }

  /*
   * Phase 3: Append queue to pcb->unsent. Queue may be NULL, but that
   * is harmless
   */
  if (last_unsent == NULL) {
    pcb->unsent = queue;
  } else {
    last_unsent->next = queue;
  }

  /*
   * Finally update the pcb state.
   */
  pcb->snd_lbb += len;
  pcb->snd_buf -= len;
  pcb->snd_queuelen = queuelen;

  LWIP_DEBUGF(TCP_QLEN_DEBUG, ("tcp_write: %"S16_F" (after enqueued)\n",
    pcb->snd_queuelen));
  if (pcb->snd_queuelen != 0) {
    LWIP_ASSERT("tcp_write: valid queue length",
                pcb->unacked != NULL || pcb->unsent != NULL);
  }

  /* Set the PSH flag in the last segment that we enqueued. */
  if (seg != NULL && seg->tcphdr != NULL && ((apiflags & TCP_WRITE_FLAG_MORE)==0)) {
    TCPH_SET_FLAG(seg->tcphdr, TCP_PSH);
  }

  return ERR_OK;
memerr:
  pcb->flags |= TF_NAGLEMEMERR;
  TCP_STATS_INC(tcp.memerr);

  if (concat_p != NULL) {
    pbuf_free(concat_p);
  }
  if (queue != NULL) {
    tcp_segs_free(queue);
  }
  if (pcb->snd_queuelen != 0) {
    LWIP_ASSERT("tcp_write: valid queue length", pcb->unacked != NULL ||
      pcb->unsent != NULL);
  }
  LWIP_DEBUGF(TCP_QLEN_DEBUG | LWIP_DBG_STATE, ("tcp_write: %"S16_F" (with mem err)\n", pcb->snd_queuelen));
  return ERR_MEM;
}

/**
 * Enqueue TCP options for transmission.
 *
 * Called by tcp_connect(), tcp_listen_input(), and tcp_send_ctrl().
 *
 * @param pcb Protocol control block for the TCP connection.
 * @param flags TCP header flags to set in the outgoing segment.
 * @param optdata pointer to TCP options, or NULL.
 * @param optlen length of TCP options in bytes.
 */
err_t
tcp_enqueue_flags(struct tcp_pcb *pcb, u8_t flags)
{
  struct pbuf *p;
  struct tcp_seg *seg;
  u8_t optflags = 0;
  u8_t optlen = 0;

  LWIP_DEBUGF(TCP_QLEN_DEBUG, ("tcp_enqueue_flags: queuelen: %"U16_F"\n", (u16_t)pcb->snd_queuelen));

  LWIP_ASSERT("tcp_enqueue_flags: need either TCP_SYN or TCP_FIN in flags (programmer violates API)",
              (flags & (TCP_SYN | TCP_FIN)) != 0);

  /* check for configured max queuelen and possible overflow */
  if ((pcb->snd_queuelen >= TCP_SND_QUEUELEN) || (pcb->snd_queuelen > TCP_SNDQUEUELEN_OVERFLOW)) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | 3, ("tcp_enqueue_flags: too long queue %"U16_F" (max %"U16_F")\n",
                                       pcb->snd_queuelen, TCP_SND_QUEUELEN));
    TCP_STATS_INC(tcp.memerr);
    pcb->flags |= TF_NAGLEMEMERR;
    return ERR_MEM;
  }

  if (flags & TCP_SYN) {
    optflags = TF_SEG_OPTS_MSS;
#if LWIP_WND_SCALE
    if ((pcb->state != SYN_RCVD) || (pcb->flags & TF_WND_SCALE)) {
      /* In a <SYN,ACK> (sent in state SYN_RCVD), the window scale option may only
         be sent if we received a window scale option from the remote host. */
      optflags |= TF_SEG_OPTS_WND_SCALE;
    }
#endif /* LWIP_WND_SCALE */
  }
#if LWIP_TCP_TIMESTAMPS
  if ((pcb->flags & TF_TIMESTAMP)) {
    /* Make sure the timestamp option is only included in data segments if we
       agreed about it with the remote host. */
    optflags |= TF_SEG_OPTS_TS;
  }
#endif /* LWIP_TCP_TIMESTAMPS */
  optlen = LWIP_TCP_OPT_LENGTH(optflags);

  /* tcp_enqueue_flags is always called with either SYN or FIN in flags.
   * We need one available snd_buf byte to do that.
   * This means we can't send FIN while snd_buf==0. A better fix would be to
   * not include SYN and FIN sequence numbers in the snd_buf count. */
  if (pcb->snd_buf == 0) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG | 3, ("tcp_enqueue_flags: no send buffer available\n"));
    TCP_STATS_INC(tcp.memerr);
    return ERR_MEM;
  }

  /* Allocate pbuf with room for TCP header + options */
  if ((p = pbuf_alloc(PBUF_TRANSPORT, optlen, PBUF_RAM)) == NULL) {
    pcb->flags |= TF_NAGLEMEMERR;
    TCP_STATS_INC(tcp.memerr);
    return ERR_MEM;
  }
  LWIP_ASSERT("tcp_enqueue_flags: check that first pbuf can hold optlen",
              (p->len >= optlen));

  /* Allocate memory for tcp_seg, and fill in fields. */
  if ((seg = tcp_create_segment(pcb, p, flags, pcb->snd_lbb, optflags)) == NULL) {
    pcb->flags |= TF_NAGLEMEMERR;
    TCP_STATS_INC(tcp.memerr);
    return ERR_MEM;
  }
  LWIP_ASSERT("tcp_enqueue_flags: invalid segment length", seg->len == 0);

  LWIP_DEBUGF(TCP_OUTPUT_DEBUG | LWIP_DBG_TRACE,
              ("tcp_enqueue_flags: queueing %"U32_F":%"U32_F" (0x%"X16_F")\n",
               ntohl(seg->tcphdr->seqno),
               ntohl(seg->tcphdr->seqno) + TCP_TCPLEN(seg),
               (u16_t)flags));

  /* Now append seg to pcb->unsent queue */
  if (pcb->unsent == NULL) {
    pcb->unsent = seg;
  } else {
    struct tcp_seg *useg;
    for (useg = pcb->unsent; useg->next != NULL; useg = useg->next);
    useg->next = seg;
  }
#if TCP_OVERSIZE
  /* The new unsent tail has no space */
  pcb->unsent_oversize = 0;
#endif /* TCP_OVERSIZE */

  /* SYN and FIN bump the sequence number */
  if ((flags & TCP_SYN) || (flags & TCP_FIN)) {
    pcb->snd_lbb++;
    /* optlen does not influence snd_buf */
    pcb->snd_buf--;
  }
  if (flags & TCP_FIN) {
    pcb->flags |= TF_FIN;
  }

  /* update number of segments on the queues */
  pcb->snd_queuelen += pbuf_clen(seg->p);
  LWIP_DEBUGF(TCP_QLEN_DEBUG, ("tcp_enqueue_flags: %"S16_F" (after enqueued)\n", pcb->snd_queuelen));
  if (pcb->snd_queuelen != 0) {
    LWIP_ASSERT("tcp_enqueue_flags: invalid queue length",
      pcb->unacked != NULL || pcb->unsent != NULL);
  }

  return ERR_OK;
}

#if LWIP_TCP_TIMESTAMPS
/* Build a timestamp option (12 bytes long) at the specified options pointer)
 *
 * @param pcb tcp_pcb
 * @param opts option pointer where to store the timestamp option
 */
static void
tcp_build_timestamp_option(struct tcp_pcb *pcb, u32_t *opts)
{
  /* Pad with two NOP options to make everything nicely aligned */
  opts[0] = PP_HTONL(0x0101080A);
  opts[1] = htonl(sys_now());
  opts[2] = htonl(pcb->ts_recent);
}
#endif

#if LWIP_WND_SCALE
/** Build a window scale option (3 bytes long) at the specified options pointer)
 *
 * @param opts option pointer where to store the window scale option
 */
static void
tcp_build_wnd_scale_option(u32_t *opts)
{
  /* Pad with one NOP option to make everything nicely aligned */
  opts[0] = PP_HTONL(0x01030300 | TCP_RCV_SCALE);
}
#endif

/** Send an ACK without data.
 *
 * @param pcb Protocol control block for the TCP connection to send the ACK
 */
err_t
tcp_send_empty_ack(struct eth_fg *cur_fg,struct tcp_pcb *pcb)
{
  struct pbuf *p;
  u8_t optlen = 0;
#if LWIP_TCP_TIMESTAMPS || CHECKSUM_GEN_TCP
  struct tcp_hdr *tcphdr;
#endif /* LWIP_TCP_TIMESTAMPS || CHECKSUM_GEN_TCP */

#if LWIP_TCP_TIMESTAMPS
  if (pcb->flags & TF_TIMESTAMP) {
    optlen = LWIP_TCP_OPT_LENGTH(TF_SEG_OPTS_TS);
  }
#endif

  p = tcp_output_alloc_header(pcb, optlen, 0, htonl(pcb->snd_nxt));
  if (p == NULL) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_output: (ACK) could not allocate pbuf\n"));
    return ERR_BUF;
  }
#if LWIP_TCP_TIMESTAMPS || CHECKSUM_GEN_TCP
  tcphdr = (struct tcp_hdr *)p->payload;
#endif /* LWIP_TCP_TIMESTAMPS || CHECKSUM_GEN_TCP */
  LWIP_DEBUGF(TCP_OUTPUT_DEBUG,
              ("tcp_output: sending ACK for %"U32_F"\n", pcb->rcv_nxt));
  /* remove ACK flags from the PCB, as we send an empty ACK now */
  pcb->flags &= ~TF_ACK_NOW;
  pcb->timer_delayedack_expires = 0;
  tcp_recompute_timers(cur_fg,pcb);


  /* NB. MSS and window scale options are only sent on SYNs, so ignore them here */
#if LWIP_TCP_TIMESTAMPS
  pcb->ts_lastacksent = pcb->rcv_nxt;

  if (pcb->flags & TF_TIMESTAMP) {
    tcp_build_timestamp_option(pcb, (u32_t *)(tcphdr + 1));
  }
#endif

#if CHECKSUM_GEN_TCP
  tcphdr->chksum = ipX_chksum_pseudo(PCB_ISIPV6(pcb), p, IP_PROTO_TCP, p->tot_len,
    &pcb->local_ip, &pcb->remote_ip);
#endif
#if LWIP_NETIF_HWADDRHINT
  tcp_output_packet(cur_fg,pcb,p);
//  ipX_output_hinted(PCB_ISIPV6(pcb), p, &pcb->local_ip, &pcb->remote_ip, pcb->ttl, pcb->tos,
//      IP_PROTO_TCP, &pcb->dst_eth_addr[0]);
#else /* LWIP_NETIF_HWADDRHINT*/
  ipX_output(PCB_ISIPV6(pcb), p, &pcb->local_ip, &pcb->remote_ip, pcb->ttl, pcb->tos,
      IP_PROTO_TCP);
#endif /* LWIP_NETIF_HWADDRHINT*/
  pbuf_free(p);

  return ERR_OK;
}

/**
 * Find out what we can send and send it
 *
 * @param pcb Protocol control block for the TCP connection to send data
 * @return ERR_OK if data has been sent or nothing to send
 *         another err_t on error
 */
err_t
tcp_output(struct eth_fg *cur_fg,struct tcp_pcb *pcb)
{

  struct tcp_seg *seg, *useg;
  u32_t wnd, snd_nxt;
#if TCP_CWND_DEBUG
  s16_t i = 0;
#endif /* TCP_CWND_DEBUG */

  mem_prefetch(&pcb->rttest);

  MEMPOOL_SANITY_ACCESS(pcb);

  /* pcb->state LISTEN not allowed here */
  LWIP_ASSERT("don't call tcp_output for listen-pcbs",
    pcb->state != LISTEN);

  /* First, check if we are invoked by the TCP input processing
     code. If so, we do not output anything. Instead, we rely on the
     input processing code to call us when input processing is done
     with. */
  if (percpu_get(tcp_input_pcb) == pcb) {
    return ERR_OK;
  }

  wnd = LWIP_MIN(pcb->snd_wnd, pcb->cwnd);

  seg = pcb->unsent;

  /* If the TF_ACK_NOW flag is set and no data will be sent (either
   * because the ->unsent queue is empty or because the window does
   * not allow it), construct an empty ACK segment and send it.
   *
   * If data is to be sent, we will just piggyback the ACK (see below).
   */
  if (pcb->flags & TF_ACK_NOW &&
     (seg == NULL ||
      ntohl(seg->tcphdr->seqno) - pcb->lastack + seg->len > wnd)) {
	  return tcp_send_empty_ack(cur_fg,pcb);
  }

  /* useg should point to last segment on unacked queue */
  useg = pcb->unacked;
  if (useg != NULL) {
    for (; useg->next != NULL; useg = useg->next);
  }

#if TCP_OUTPUT_DEBUG
  if (seg == NULL) {
    LWIP_DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_output: nothing to send (%p)\n",
                                   (void*)pcb->unsent));
  }
#endif /* TCP_OUTPUT_DEBUG */
#if TCP_CWND_DEBUG
  if (seg == NULL) {
    LWIP_DEBUGF(TCP_CWND_DEBUG, ("tcp_output: snd_wnd %"TCPWNDSIZE_F
                                 ", cwnd %"TCPWNDSIZE_F", wnd %"U32_F
                                 ", seg == NULL, ack %"U32_F"\n",
                                 pcb->snd_wnd, pcb->cwnd, wnd, pcb->lastack));
  } else {
    LWIP_DEBUGF(TCP_CWND_DEBUG,
                ("tcp_output: snd_wnd %"TCPWNDSIZE_F", cwnd %"TCPWNDSIZE_F", wnd %"U32_F
                 ", effwnd %"U32_F", seq %"U32_F", ack %"U32_F"\n",
                 pcb->snd_wnd, pcb->cwnd, wnd,
                 ntohl(seg->tcphdr->seqno) - pcb->lastack + seg->len,
                 ntohl(seg->tcphdr->seqno), pcb->lastack));
  }
#endif /* TCP_CWND_DEBUG */
  /* data available and window allows it to be sent? */
  while (seg != NULL &&
         ntohl(seg->tcphdr->seqno) - pcb->lastack + seg->len <= wnd) {
    LWIP_ASSERT("RST not expected here!",
                (TCPH_FLAGS(seg->tcphdr) & TCP_RST) == 0);
    /* Stop sending if the nagle algorithm would prevent it
     * Don't stop:
     * - if tcp_write had a memory error before (prevent delayed ACK timeout) or
     * - if FIN was already enqueued for this PCB (SYN is always alone in a segment -
     *   either seg->next != NULL or pcb->unacked == NULL;
     *   RST is no sent using tcp_write/tcp_output.
     */
    if((tcp_do_output_nagle(pcb) == 0) &&
      ((pcb->flags & (TF_NAGLEMEMERR | TF_FIN)) == 0)){
      break;
    }
#if TCP_CWND_DEBUG
    LWIP_DEBUGF(TCP_CWND_DEBUG, ("tcp_output: snd_wnd %"TCPWNDSIZE_F", cwnd %"TCPWNDSIZE_F", wnd %"U32_F", effwnd %"U32_F", seq %"U32_F", ack %"U32_F", i %"S16_F"\n",
                            pcb->snd_wnd, pcb->cwnd, wnd,
                            ntohl(seg->tcphdr->seqno) + seg->len -
                            pcb->lastack,
                            ntohl(seg->tcphdr->seqno), pcb->lastack, i));
    ++i;
#endif /* TCP_CWND_DEBUG */

    pcb->unsent = seg->next;

    if (pcb->state != SYN_SENT) {
      TCPH_SET_FLAG(seg->tcphdr, TCP_ACK);
      pcb->flags &= ~TF_ACK_NOW;
      pcb->timer_delayedack_expires = 0;
      tcp_recompute_timers(cur_fg,pcb);
    }

#if TCP_OVERSIZE_DBGCHECK
    seg->oversize_left = 0;
#endif /* TCP_OVERSIZE_DBGCHECK */
    tcp_output_segment(cur_fg,seg, pcb);
    snd_nxt = ntohl(seg->tcphdr->seqno) + TCP_TCPLEN(seg);
    if (TCP_SEQ_LT(pcb->snd_nxt, snd_nxt)) {
      pcb->snd_nxt = snd_nxt;
    }
    /* put segment on unacknowledged list if length > 0 */
    if (TCP_TCPLEN(seg) > 0) {
      seg->next = NULL;
      /* unacked list is empty? */
      if (pcb->unacked == NULL) {
        pcb->unacked = seg;
        useg = seg;
      /* unacked list is not empty? */
      } else {
        /* In the case of fast retransmit, the packet should not go to the tail
         * of the unacked queue, but rather somewhere before it. We need to check for
         * this case. -STJ Jul 27, 2004 */
        if (TCP_SEQ_LT(ntohl(seg->tcphdr->seqno), ntohl(useg->tcphdr->seqno))) {
          /* add segment to before tail of unacked list, keeping the list sorted */
          struct tcp_seg **cur_seg = &(pcb->unacked);
          while (*cur_seg &&
            TCP_SEQ_LT(ntohl((*cur_seg)->tcphdr->seqno), ntohl(seg->tcphdr->seqno))) {
              cur_seg = &((*cur_seg)->next );
          }
          seg->next = (*cur_seg);
          (*cur_seg) = seg;
        } else {
          /* add segment to tail of unacked list */
          useg->next = seg;
          useg = useg->next;
        }
      }
    /* do not queue empty segments on the unacked list */
    } else {
      tcp_seg_free(seg);
    }
    seg = pcb->unsent;
  }
#if TCP_OVERSIZE
  if (pcb->unsent == NULL) {
    /* last unsent has been removed, reset unsent_oversize */
    pcb->unsent_oversize = 0;
  }
#endif /* TCP_OVERSIZE */

  pcb->flags &= ~TF_NAGLEMEMERR;
  return ERR_OK;
}

/**
 * Called by tcp_output() to actually send a TCP segment over IP.
 *
 * @param seg the tcp_seg to send
 * @param pcb the tcp_pcb for the TCP connection used to send the segment
 */
static void
tcp_output_segment(struct eth_fg *cur_fg,struct tcp_seg *seg, struct tcp_pcb *pcb)
{
  u16_t len;
  u32_t *opts;

  /** @bug Exclude retransmitted segments from this count. */
  snmp_inc_tcpoutsegs();

  /* The TCP header has already been constructed, but the ackno and
   wnd fields remain. */
  seg->tcphdr->ackno = htonl(pcb->rcv_nxt);

  /* advertise our receive window size in this TCP segment */
#if LWIP_WND_SCALE
  if (seg->flags & TF_SEG_OPTS_WND_SCALE) {
    /* The Window field in a SYN segment itself (the only type where we send
       the window scale option) is never scaled. */
    seg->tcphdr->wnd = htons(pcb->rcv_ann_wnd);
  } else
#endif /* LWIP_WND_SCALE */
  {
    seg->tcphdr->wnd = htons(RCV_WND_SCALE(pcb, pcb->rcv_ann_wnd));
  }

  pcb->rcv_ann_right_edge = pcb->rcv_nxt + pcb->rcv_ann_wnd;

  /* Add any requested options.  NB MSS option is only set on SYN
     packets, so ignore it here */
  opts = (u32_t *)(void *)(seg->tcphdr + 1);
  if (seg->flags & TF_SEG_OPTS_MSS) {
    u16_t mss;
#if TCP_CALCULATE_EFF_SEND_MSS
    mss = tcp_eff_send_mss(TCP_MSS, &pcb->local_ip, &pcb->remote_ip, PCB_ISIPV6(pcb));
#else /* TCP_CALCULATE_EFF_SEND_MSS */
    mss = TCP_MSS;
#endif /* TCP_CALCULATE_EFF_SEND_MSS */
    *opts = TCP_BUILD_MSS_OPTION(mss);
    opts += 1;
  }
#if LWIP_TCP_TIMESTAMPS
  pcb->ts_lastacksent = pcb->rcv_nxt;

  if (seg->flags & TF_SEG_OPTS_TS) {
    tcp_build_timestamp_option(pcb, opts);
    opts += 3;
  }
#endif
#if LWIP_WND_SCALE
  if (seg->flags & TF_SEG_OPTS_WND_SCALE) {
    tcp_build_wnd_scale_option(opts);
    opts += 1;
  }
#endif

  /* Set retransmission timer running if it is not currently enabled
     This must be set before checking the route. */
  pcb->timer_retransmit_expires = timer_now() + pcb->rto * RTO_UNITS;
  tcp_recompute_timers(cur_fg,pcb);

  /* If we don't have a local IP address, we get one by
     calling ip_route(). */
  if (ipX_addr_isany(PCB_ISIPV6(pcb), &pcb->local_ip)) {
    struct netif *netif;
    ipX_addr_t *local_ip;
    ipX_route_get_local_ipX(PCB_ISIPV6(pcb), &pcb->local_ip, &pcb->remote_ip, netif, local_ip);
    if ((netif == NULL) || (local_ip == NULL)) {
      return;
    }
    ipX_addr_copy(PCB_ISIPV6(pcb), pcb->local_ip, *local_ip);
  }

  if (pcb->rttest == 0) {
    pcb->rttest = cur_fg->tcp_ticks;
    pcb->rtseq = ntohl(seg->tcphdr->seqno);

    LWIP_DEBUGF(TCP_RTO_DEBUG, ("tcp_output_segment: rtseq %"U32_F"\n", pcb->rtseq));
  }
  LWIP_DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_output_segment: %"U32_F":%"U32_F"\n",
          htonl(seg->tcphdr->seqno), htonl(seg->tcphdr->seqno) +
          seg->len));

  len = (u16_t)((u8_t *)seg->tcphdr - (u8_t *)seg->p->payload);

  seg->p->len -= len;
  seg->p->tot_len -= len;

  seg->p->payload = seg->tcphdr;

  seg->tcphdr->chksum = 0;
#if TCP_CHECKSUM_ON_COPY
  {
    u32_t acc;
#if TCP_CHECKSUM_ON_COPY_SANITY_CHECK
    u16_t chksum_slow = ipX_chksum_pseudo(PCB_ISIPV6(pcb), seg->p, IP_PROTO_TCP,
      seg->p->tot_len, &pcb->local_ip, &pcb->remote_ip);
#endif /* TCP_CHECKSUM_ON_COPY_SANITY_CHECK */
    if ((seg->flags & TF_SEG_DATA_CHECKSUMMED) == 0) {
      LWIP_ASSERT("data included but not checksummed",
        seg->p->tot_len == (TCPH_HDRLEN(seg->tcphdr) * 4));
    }

    /* rebuild TCP header checksum (TCP header changes for retransmissions!) */
    acc = ipX_chksum_pseudo_partial(PCB_ISIPV6(pcb), seg->p, IP_PROTO_TCP,
      seg->p->tot_len, TCPH_HDRLEN(seg->tcphdr) * 4, &pcb->local_ip, &pcb->remote_ip);
    /* add payload checksum */
    if (seg->chksum_swapped) {
      seg->chksum = SWAP_BYTES_IN_WORD(seg->chksum);
      seg->chksum_swapped = 0;
    }
    acc += (u16_t)~(seg->chksum);
    seg->tcphdr->chksum = FOLD_U32T(acc);
#if TCP_CHECKSUM_ON_COPY_SANITY_CHECK
    if (chksum_slow != seg->tcphdr->chksum) {
      LWIP_DEBUGF(TCP_DEBUG | LWIP_DBG_LEVEL_WARNING,
                  ("tcp_output_segment: calculated checksum is %"X16_F" instead of %"X16_F"\n",
                  seg->tcphdr->chksum, chksum_slow));
      seg->tcphdr->chksum = chksum_slow;
    }
#endif /* TCP_CHECKSUM_ON_COPY_SANITY_CHECK */
  }
#else /* TCP_CHECKSUM_ON_COPY */
#if CHECKSUM_GEN_TCP
  seg->tcphdr->chksum = ipX_chksum_pseudo(PCB_ISIPV6(pcb), seg->p, IP_PROTO_TCP,
    seg->p->tot_len, &pcb->local_ip, &pcb->remote_ip);
#endif /* CHECKSUM_GEN_TCP */
#endif /* TCP_CHECKSUM_ON_COPY */
  TCP_STATS_INC(tcp.xmit);

#if LWIP_NETIF_HWADDRHINT
  tcp_output_packet(cur_fg,pcb,seg->p);
//  ipX_output_hinted(PCB_ISIPV6(pcb), seg->p, &pcb->local_ip, &pcb->remote_ip,
//    pcb->ttl, pcb->tos, IP_PROTO_TCP, &pcb->dst_eth_addr[0]);
#else /* LWIP_NETIF_HWADDRHINT*/
  ipX_output(PCB_ISIPV6(pcb), seg->p, &pcb->local_ip, &pcb->remote_ip, pcb->ttl,
    pcb->tos, IP_PROTO_TCP);
#endif /* LWIP_NETIF_HWADDRHINT*/
}

/**
 * Send a TCP RESET packet (empty segment with RST flag set) either to
 * abort a connection or to show that there is no matching local connection
 * for a received segment.
 *
 * Called by tcp_abort() (to abort a local connection), tcp_input() (if no
 * matching local pcb was found), tcp_listen_input() (if incoming segment
 * has ACK flag set) and tcp_process() (received segment in the wrong state)
 *
 * Since a RST segment is in most cases not sent for an active connection,
 * tcp_rst() has a number of arguments that are taken from a tcp_pcb for
 * most other segment output functions.
 *
 * @param seqno the sequence number to use for the outgoing segment
 * @param ackno the acknowledge number to use for the outgoing segment
 * @param local_ip the local IP address to send the segment from
 * @param remote_ip the remote IP address to send the segment to
 * @param local_port the local TCP port to send the segment from
 * @param remote_port the remote TCP port to send the segment to
 */
void
tcp_rst_impl(struct eth_fg *cur_fg,u32_t seqno, u32_t ackno,
  ipX_addr_t *local_ip, ipX_addr_t *remote_ip,
  u16_t local_port, u16_t remote_port
#if LWIP_IPV6
  , u8_t isipv6
#endif /* LWIP_IPV6 */
  )
{
  struct pbuf *p;
  struct tcp_hdr *tcphdr;
  p = pbuf_alloc(PBUF_IP, TCP_HLEN, PBUF_RAM);
  if (p == NULL) {
      LWIP_DEBUGF(TCP_DEBUG, ("tcp_rst: could not allocate memory for pbuf\n"));
      return;
  }
  LWIP_ASSERT("check that first pbuf can hold struct tcp_hdr",
              (p->len >= sizeof(struct tcp_hdr)));

  tcphdr = (struct tcp_hdr *)p->payload;
  tcphdr->src = htons(local_port);
  tcphdr->dest = htons(remote_port);
  tcphdr->seqno = htonl(seqno);
  tcphdr->ackno = htonl(ackno);
  TCPH_HDRLEN_FLAGS_SET(tcphdr, TCP_HLEN/4, TCP_RST | TCP_ACK);
#if LWIP_WND_SCALE
  tcphdr->wnd = PP_HTONS(((TCP_WND >> TCP_RCV_SCALE) & 0xFFFF));
#else
  tcphdr->wnd = PP_HTONS(TCP_WND);
#endif
  tcphdr->chksum = 0;
  tcphdr->urgp = 0;

  TCP_STATS_INC(tcp.xmit);
  snmp_inc_tcpoutrsts();

#if CHECKSUM_GEN_TCP
  tcphdr->chksum = ipX_chksum_pseudo(isipv6, p, IP_PROTO_TCP, p->tot_len,
                                     local_ip, remote_ip);
#endif
  /* Send output with hardcoded TTL/HL since we have no access to the pcb */
  ipX_output_hinted(isipv6, p, local_ip, remote_ip, TCP_TTL, 0, IP_PROTO_TCP,NULL);
  pbuf_free(p);
  LWIP_DEBUGF(TCP_RST_DEBUG, ("tcp_rst: seqno %"U32_F" ackno %"U32_F".\n", seqno, ackno));
}

/**
 * Requeue all unacked segments for retransmission
 *
 * Called by tcp_slowtmr() for slow retransmission.
 *
 * @param pcb the tcp_pcb for which to re-enqueue all unacked segments
 */
void
tcp_rexmit_rto(struct eth_fg *cur_fg,struct tcp_pcb *pcb)
{
  struct tcp_seg *seg;

  if (pcb->unacked == NULL) {
    return;
  }

  /* Move all unacked segments to the head of the unsent queue */
  for (seg = pcb->unacked; seg->next != NULL; seg = seg->next);
  /* concatenate unsent queue after unacked queue */
  seg->next = pcb->unsent;
#if TCP_OVERSIZE && TCP_OVERSIZE_DBGCHECK
  /* if last unsent changed, we need to update unsent_oversize */
  if (pcb->unsent == NULL) {
    pcb->unsent_oversize = seg->oversize_left;
  }
#endif /* TCP_OVERSIZE && TCP_OVERSIZE_DBGCHECK*/
  /* unsent queue is the concatenated queue (of unacked, unsent) */
  pcb->unsent = pcb->unacked;
  /* unacked queue is now empty */
  pcb->unacked = NULL;

  /* increment number of retransmissions */
  ++pcb->nrtx;

  /* Don't take any RTT measurements after retransmitting. */
  pcb->rttest = 0;

  /* Do the actual retransmission */
  tcp_output(cur_fg,pcb);
}

/**
 * Requeue the first unacked segment for retransmission
 *
 * Called by tcp_receive() for fast retramsmit.
 *
 * @param pcb the tcp_pcb for which to retransmit the first unacked segment
 */
void
tcp_rexmit(struct tcp_pcb *pcb)
{
  struct tcp_seg *seg;
  struct tcp_seg **cur_seg;

  if (pcb->unacked == NULL) {
    return;
  }

  /* Move the first unacked segment to the unsent queue */
  /* Keep the unsent queue sorted. */
  seg = pcb->unacked;
  pcb->unacked = seg->next;

  cur_seg = &(pcb->unsent);
  while (*cur_seg &&
    TCP_SEQ_LT(ntohl((*cur_seg)->tcphdr->seqno), ntohl(seg->tcphdr->seqno))) {
      cur_seg = &((*cur_seg)->next );
  }
  seg->next = *cur_seg;
  *cur_seg = seg;
#if TCP_OVERSIZE
  if (seg->next == NULL) {
    /* the retransmitted segment is last in unsent, so reset unsent_oversize */
    pcb->unsent_oversize = 0;
  }
#endif /* TCP_OVERSIZE */

  ++pcb->nrtx;

  /* Don't take any rtt measurements after retransmitting. */
  pcb->rttest = 0;

  /* Do the actual retransmission. */
  snmp_inc_tcpretranssegs();
  /* No need to call tcp_output: we are always called from tcp_input()
     and thus tcp_output directly returns. */
}


/**
 * Handle retransmission after three dupacks received
 *
 * @param pcb the tcp_pcb for which to retransmit the first unacked segment
 */
void
tcp_rexmit_fast(struct tcp_pcb *pcb)
{
  if (pcb->unacked != NULL && !(pcb->flags & TF_INFR)) {
    /* This is fast retransmit. Retransmit the first unacked segment. */
    LWIP_DEBUGF(TCP_FR_DEBUG,
                ("tcp_receive: dupacks %"U16_F" (%"U32_F
                 "), fast retransmit %"U32_F"\n",
                 (u16_t)pcb->dupacks, pcb->lastack,
                 ntohl(pcb->unacked->tcphdr->seqno)));
    tcp_rexmit(pcb);

    /* Set ssthresh to half of the minimum of the current
     * cwnd and the advertised window */
    if (pcb->cwnd > pcb->snd_wnd) {
      pcb->ssthresh = pcb->snd_wnd / 2;
    } else {
      pcb->ssthresh = pcb->cwnd / 2;
    }

    /* The minimum value for ssthresh should be 2 MSS */
    if (pcb->ssthresh < (2U * pcb->mss)) {
      LWIP_DEBUGF(TCP_FR_DEBUG,
                  ("tcp_receive: The minimum value for ssthresh %"U16_F
                   " should be min 2 mss %"U16_F"...\n",
                   pcb->ssthresh, 2*pcb->mss));
      pcb->ssthresh = 2*pcb->mss;
    }

    pcb->cwnd = pcb->ssthresh + 3 * pcb->mss;
    pcb->flags |= TF_INFR;
  }
}


/**
 * Send keepalive packets to keep a connection active although
 * no data is sent over it.
 *
 * Called by tcp_slowtmr()
 *
 * @param pcb the tcp_pcb for which to send a keepalive packet
 */
void
tcp_keepalive(struct eth_fg *cur_fg,struct tcp_pcb *pcb)
{

  struct pbuf *p;
#if CHECKSUM_GEN_TCP
  struct tcp_hdr *tcphdr;
#endif /* CHECKSUM_GEN_TCP */

  LWIP_DEBUGF(TCP_DEBUG, ("tcp_keepalive: sending KEEPALIVE probe to "));
  ipX_addr_debug_print(PCB_ISIPV6(pcb), TCP_DEBUG, &pcb->remote_ip);
  LWIP_DEBUGF(TCP_DEBUG, ("\n"));

  LWIP_DEBUGF(TCP_DEBUG, ("tcp_keepalive: tcp_ticks %"U32_F"   pcb->tmr %"U32_F" pcb->keep_cnt_sent %"U16_F"\n",
                          cur_fg->tcp_ticks, pcb->tmr, pcb->keep_cnt_sent));

  p = tcp_output_alloc_header(pcb, 0, 0, htonl(pcb->snd_nxt - 1));
  if(p == NULL) {
    LWIP_DEBUGF(TCP_DEBUG,
                ("tcp_keepalive: could not allocate memory for pbuf\n"));
    return;
  }
#if CHECKSUM_GEN_TCP
  tcphdr = (struct tcp_hdr *)p->payload;

  tcphdr->chksum = ipX_chksum_pseudo(PCB_ISIPV6(pcb), p, IP_PROTO_TCP, p->tot_len,
      &pcb->local_ip, &pcb->remote_ip);
#endif /* CHECKSUM_GEN_TCP */
  TCP_STATS_INC(tcp.xmit);

  /* Send output to IP */
#if LWIP_NETIF_HWADDRHINT
  tcp_output_packet(cur_fg,pcb,p);
//ipX_output_hinted(PCB_ISIPV6(pcb), p, &pcb->local_ip, &pcb->remote_ip,
//    pcb->ttl, 0, IP_PROTO_TCP, &pcb->dst_eth_addr[0]);
#else /* LWIP_NETIF_HWADDRHINT*/
  ipX_output(PCB_ISIPV6(pcb), p, &pcb->local_ip, &pcb->remote_ip, pcb->ttl,
    0, IP_PROTO_TCP);
#endif /* LWIP_NETIF_HWADDRHINT*/

  pbuf_free(p);

  LWIP_DEBUGF(TCP_DEBUG, ("tcp_keepalive: seqno %"U32_F" ackno %"U32_F".\n",
                          pcb->snd_nxt - 1, pcb->rcv_nxt));
}


/**
 * Send persist timer zero-window probes to keep a connection active
 * when a window update is lost.
 *
 * Called by tcp_slowtmr()
 *
 * @param pcb the tcp_pcb for which to send a zero-window probe packet
 */
void
tcp_zero_window_probe(struct eth_fg *cur_fg,struct tcp_pcb *pcb)
{

  struct pbuf *p;
  struct tcp_hdr *tcphdr;
  struct tcp_seg *seg;
  u16_t len;
  u8_t is_fin;

  LWIP_DEBUGF(TCP_DEBUG, ("tcp_zero_window_probe: sending ZERO WINDOW probe to "));
  ipX_addr_debug_print(PCB_ISIPV6(pcb), TCP_DEBUG, &pcb->remote_ip);
  LWIP_DEBUGF(TCP_DEBUG, ("\n"));

  LWIP_DEBUGF(TCP_DEBUG,
              ("tcp_zero_window_probe: tcp_ticks %"U32_F
               "   pcb->tmr %"U32_F" pcb->keep_cnt_sent %"U16_F"\n",
               cur_fg->tcp_ticks, pcb->tmr, pcb->keep_cnt_sent));

  seg = pcb->unacked;

  if(seg == NULL) {
    seg = pcb->unsent;
  }
  if(seg == NULL) {
    return;
  }

  is_fin = ((TCPH_FLAGS(seg->tcphdr) & TCP_FIN) != 0) && (seg->len == 0);
  /* we want to send one seqno: either FIN or data (no options) */
  len = is_fin ? 0 : 1;

  p = tcp_output_alloc_header(pcb, 0, len, seg->tcphdr->seqno);
  if(p == NULL) {
    LWIP_DEBUGF(TCP_DEBUG, ("tcp_zero_window_probe: no memory for pbuf\n"));
    return;
  }
  tcphdr = (struct tcp_hdr *)p->payload;

  if (is_fin) {
    /* FIN segment, no data */
    TCPH_FLAGS_SET(tcphdr, TCP_ACK | TCP_FIN);
  } else {
    /* Data segment, copy in one byte from the head of the unacked queue */
    char *d = ((char *)p->payload + TCP_HLEN);
    /* Depending on whether the segment has already been sent (unacked) or not
       (unsent), seg->p->payload points to the IP header or TCP header.
       Ensure we copy the first TCP data byte: */
    pbuf_copy_partial(seg->p, d, 1, seg->p->tot_len - seg->len);
  }

#if CHECKSUM_GEN_TCP
  tcphdr->chksum = ipX_chksum_pseudo(PCB_ISIPV6(pcb), p, IP_PROTO_TCP, p->tot_len,
      &pcb->local_ip, &pcb->remote_ip);
#endif
  TCP_STATS_INC(tcp.xmit);

  /* Send output to IP */
#if LWIP_NETIF_HWADDRHINT
  tcp_output_packet(cur_fg,pcb,p);
//ipX_output_hinted(PCB_ISIPV6(pcb), p, &pcb->local_ip, &pcb->remote_ip, pcb->ttl,
//    0, IP_PROTO_TCP, &pcb->dst_eth_addr[0]);
#else /* LWIP_NETIF_HWADDRHINT*/
  ipX_output(PCB_ISIPV6(pcb), p, &pcb->local_ip, &pcb->remote_ip, pcb->ttl, 0, IP_PROTO_TCP);
#endif /* LWIP_NETIF_HWADDRHINT*/

  pbuf_free(p);

  LWIP_DEBUGF(TCP_DEBUG, ("tcp_zero_window_probe: seqno %"U32_F
                          " ackno %"U32_F".\n",
                          pcb->snd_nxt - 1, pcb->rcv_nxt));
}
#endif /* LWIP_TCP */
