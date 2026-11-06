/*
 * libwebsockets - small server side websockets and web server implementation
 *
 * Copyright (C) 2010-2017 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include "core/private.h"

/*
 * notice this returns number of bytes consumed, or -1
 */
int lws_issue_raw(struct lws *wsi, unsigned char *buf, size_t len)
{
 if (lws_vr_tracked1(wsi->context)) return lws_vr_issue_raw1(wsi, buf, len);
	struct lws_context *context = lws_get_context(wsi);
	struct lws_context_per_thread *pt = &wsi->context->pt[(int)wsi->tsi];
	size_t real_len = len;
	unsigned int n;

	// lwsl_hexdump_err(buf, len);

	/*
	 * Detect if we got called twice without going through the
	 * event loop to handle pending.  This would be caused by either
	 * back-to-back writes in one WRITABLE (illegal) or calling lws_write()
	 * from outside the WRITABLE callback (illegal).
	 */
	if (wsi->could_have_pending) {
		lwsl_hexdump_level(LLL_ERR, buf, len);
		lwsl_err("** %p: vh: %s, prot: %s, role %s: "
			 "Illegal back-to-back write of %lu detected...\n",
			 wsi, wsi->vhost->name, wsi->protocol->name,
			 wsi->role_ops->name,
			 (unsigned long)len);
		// assert(0);

		return -1;
	}

	lws_stats_atomic_bump(wsi->context, pt, LWSSTATS_C_API_WRITE, 1);

	if (!len)
		return 0;
	/* just ignore sends after we cleared the truncation buffer */
	if (lwsi_state(wsi) == LRS_FLUSHING_BEFORE_CLOSE && !wsi->trunc_len)
		return (int)len;

	if (wsi->trunc_len && (buf < wsi->trunc_alloc ||
	    buf > (wsi->trunc_alloc + wsi->trunc_len + wsi->trunc_offset))) {
		lwsl_hexdump_level(LLL_ERR, buf, len);
		lwsl_err("** %p: vh: %s, prot: %s, Sending new %lu, pending truncated ...\n"
			 "   It's illegal to do an lws_write outside of\n"
			 "   the writable callback: fix your code\n",
			 wsi, wsi->vhost->name, wsi->protocol->name,
			 (unsigned long)len);
		assert(0);

		return -1;
	}

	if (!wsi->http2_substream && !lws_socket_is_valid(wsi->desc.sockfd))
		lwsl_warn("** error invalid sock but expected to send\n");

	/* limit sending */
	if (wsi->protocol->tx_packet_size)
		n = (int)wsi->protocol->tx_packet_size;
	else {
		n = (int)wsi->protocol->rx_buffer_size;
		if (!n)
			n = context->pt_serv_buf_size;
	}
	n += LWS_PRE + 4;
	if (n > len)
		n = (int)len;

	/* nope, send it on the socket directly */
	lws_latency_pre(context, wsi);
	n = lws_ssl_capable_write(wsi, buf, n);
	lws_latency(context, wsi, "send lws_issue_raw", n, n == len);

	/* something got written, it can have been truncated now */
	wsi->could_have_pending = 1;

	switch (n) {
	case LWS_SSL_CAPABLE_ERROR:
		/* we're going to close, let close know sends aren't possible */
		wsi->socket_is_permanently_unusable = 1;
		return -1;
	case LWS_SSL_CAPABLE_MORE_SERVICE:
		/*
		 * nothing got sent, not fatal.  Retry the whole thing later,
		 * ie, implying treat it was a truncated send so it gets
		 * retried
		 */
		n = 0;
		break;
	}

	/*
	 * we were already handling a truncated send?
	 */
	if (wsi->trunc_len) {
		lwsl_info("%p partial adv %d (vs %ld)\n", wsi, n, (long)real_len);
		wsi->trunc_offset += n;
		wsi->trunc_len -= n;

		if (!wsi->trunc_len) {
			lwsl_info("** %p partial send completed\n", wsi);
			/* done with it, but don't free it */
			n = (int)real_len;
			if (lwsi_state(wsi) == LRS_FLUSHING_BEFORE_CLOSE) {
				lwsl_info("** %p signalling to close now\n", wsi);
				return -1; /* retry closing now */
			}
		}
		/* always callback on writeable */
		lws_callback_on_writable(wsi);

		return n;
	}

	if ((unsigned int)n == real_len)
		/* what we just sent went out cleanly */
		return n;

	/*
	 * Newly truncated send.  Buffer the remainder (it will get
	 * first priority next time the socket is writable).
	 */
	lwsl_debug("%p new partial sent %d from %lu total\n", wsi, n,
		    (unsigned long)real_len);

	lws_stats_atomic_bump(wsi->context, pt, LWSSTATS_C_WRITE_PARTIALS, 1);
	lws_stats_atomic_bump(wsi->context, pt, LWSSTATS_B_PARTIALS_ACCEPTED_PARTS, n);

	/*
	 *  - if we still have a suitable malloc lying around, use it
	 *  - or, if too small, reallocate it
	 *  - or, if no buffer, create it
	 */
	if (!wsi->trunc_alloc || real_len - n > wsi->trunc_alloc_len) {
		lws_free(wsi->trunc_alloc);

		wsi->trunc_alloc_len = (unsigned int)(real_len - n);
		wsi->trunc_alloc = lws_malloc(real_len - n,
					      "truncated send alloc");
		if (!wsi->trunc_alloc) {
			lwsl_err("truncated send: unable to malloc %lu\n",
				 (unsigned long)(real_len - n));
			return -1;
		}
	}
	wsi->trunc_offset = 0;
	wsi->trunc_len = (unsigned int)(real_len - n);
	memcpy(wsi->trunc_alloc, buf + n, real_len - n);

#if !defined(LWS_WITH_ESP32)
	if (lws_wsi_is_udp(wsi)) {
		/* stash original destination for fulfilling UDP partials */
		wsi->udp->sa_pending = wsi->udp->sa;
		wsi->udp->salen_pending = wsi->udp->salen;
	}
#endif

	/* since something buffered, force it to get another chance to send */
	lws_callback_on_writable(wsi);

	return (int)real_len;
}

LWS_VISIBLE int lws_write(struct lws *wsi, unsigned char *buf, size_t len,
			  enum lws_write_protocol wp)
{
 if (lws_vr_tracked1(wsi->context) && (len > 65536 || len > 65552 - LWS_PRE))
  return -1; /* complete frame/header capacity BEFORE role effects or OS write */
	struct lws_context_per_thread *pt = &wsi->context->pt[(int)wsi->tsi];

	if (wsi->parent_carries_io) {
		struct lws_write_passthru pas;

		pas.buf = buf;
		pas.len = len;
		pas.wp = wp;
		pas.wsi = wsi;

		if (wsi->parent->protocol->callback(wsi->parent,
				LWS_CALLBACK_CHILD_WRITE_VIA_PARENT,
				wsi->parent->user_space,
				(void *)&pas, 0))
			return 1;

		return (int)len;
	}

	lws_stats_atomic_bump(wsi->context, pt, LWSSTATS_C_API_LWS_WRITE, 1);

	if ((int)len < 0) {
		lwsl_err("%s: suspicious len int %d, ulong %lu\n", __func__,
				(int)len, (unsigned long)len);
		return -1;
	}

	lws_stats_atomic_bump(wsi->context, pt, LWSSTATS_B_WRITE, len);

#ifdef LWS_WITH_ACCESS_LOG
	wsi->http.access_log.sent += len;
#endif
	if (wsi->vhost)
		wsi->vhost->conn_stats.tx += len;

	assert(wsi->role_ops);
	if (!wsi->role_ops->write_role_protocol)
		return lws_issue_raw(wsi, buf, len);

	return wsi->role_ops->write_role_protocol(wsi, buf, len, &wp);
}

LWS_VISIBLE int
lws_ssl_capable_read_no_ssl(struct lws *wsi, unsigned char *buf, int len)
{
 if (lws_vr_tracked1(wsi->context)) {
  int actual, error;
  if (len < 1 || len > 32768 || !lws_vr_read_claim1(wsi))
   return LWS_SSL_CAPABLE_MORE_SERVICE;
  if (lws_vr_io1(wsi, VR_LWS_NAMED_READ1) != VR_LWS_KNOWN_SUCCESS1) {
   lws_vr_read_release1(wsi); return LWS_SSL_CAPABLE_MORE_SERVICE;
  }
  actual = recv(wsi->desc.sockfd, buf, (size_t)len, 0);
  error = actual < 0 ? errno : 0;
  lws_vr_io_record1(wsi->context, VR_LWS_NAMED_READ1, actual, error);
  if (actual > 0) { wsi->vr_data.ReadBytes = (uint32_t)actual; return actual; }
  lws_vr_read_release1(wsi);
  if (!actual) return 0;
  return error == EAGAIN || error == EWOULDBLOCK || error == EINTR ?
   LWS_SSL_CAPABLE_MORE_SERVICE : LWS_SSL_CAPABLE_ERROR;
 }
	struct lws_context *context = wsi->context;
	struct lws_context_per_thread *pt = &context->pt[(int)wsi->tsi];
	int n = 0;

	lws_stats_atomic_bump(context, pt, LWSSTATS_C_API_READ, 1);

	if (lws_wsi_is_udp(wsi)) {
#if !defined(LWS_WITH_ESP32)
		wsi->udp->salen = sizeof(wsi->udp->sa);
		n = recvfrom(wsi->desc.sockfd, (char *)buf, len, 0,
			     &wsi->udp->sa, &wsi->udp->salen);
#endif
	} else
		n = recv(wsi->desc.sockfd, (char *)buf, len, 0);

	if (n >= 0) {
		if (wsi->vhost)
			wsi->vhost->conn_stats.rx += n;
		lws_stats_atomic_bump(context, pt, LWSSTATS_B_READ, n);

		return n;
	}

	if (LWS_ERRNO == LWS_EAGAIN ||
	    LWS_ERRNO == LWS_EWOULDBLOCK ||
	    LWS_ERRNO == LWS_EINTR)
		return LWS_SSL_CAPABLE_MORE_SERVICE;

	lwsl_notice("error on reading from skt : %d\n", LWS_ERRNO);
	return LWS_SSL_CAPABLE_ERROR;
}

LWS_VISIBLE int
lws_ssl_capable_write_no_ssl(struct lws *wsi, unsigned char *buf, int len)
{
 if (lws_vr_tracked1(wsi->context)) {
  int actual, error;
  if (len < 1 || len > 65552) return LWS_SSL_CAPABLE_ERROR;
  if (lws_vr_io1(wsi, VR_LWS_NAMED_WRITE1) != VR_LWS_KNOWN_SUCCESS1)
   return LWS_SSL_CAPABLE_MORE_SERVICE;
  actual = send(wsi->desc.sockfd, buf, (size_t)len, MSG_NOSIGNAL);
  error = actual < 0 ? errno : 0;
  lws_vr_io_record1(wsi->context, VR_LWS_NAMED_WRITE1, actual, error);
  if (actual >= 0) return actual;
  return error == EAGAIN || error == EWOULDBLOCK || error == EINTR ?
   LWS_SSL_CAPABLE_MORE_SERVICE : LWS_SSL_CAPABLE_ERROR;
 }
	int n = 0;

	if (lws_wsi_is_udp(wsi)) {
#if !defined(LWS_WITH_ESP32)
		if (wsi->trunc_len)
			n = sendto(wsi->desc.sockfd, buf, len, 0, &wsi->udp->sa_pending, wsi->udp->salen_pending);
		else
			n = sendto(wsi->desc.sockfd, buf, len, 0, &wsi->udp->sa, wsi->udp->salen);
#endif
	} else
		n = send(wsi->desc.sockfd, (char *)buf, len, MSG_NOSIGNAL);
//	lwsl_info("%s: sent len %d result %d", __func__, len, n);
	if (n >= 0)
		return n;

	if (LWS_ERRNO == LWS_EAGAIN ||
	    LWS_ERRNO == LWS_EWOULDBLOCK ||
	    LWS_ERRNO == LWS_EINTR) {
		if (LWS_ERRNO == LWS_EWOULDBLOCK) {
			lws_set_blocking_send(wsi);
		}

		return LWS_SSL_CAPABLE_MORE_SERVICE;
	}

	lwsl_debug("ERROR writing len %d to skt fd %d err %d / errno %d\n",
			len, wsi->desc.sockfd, n, LWS_ERRNO);

	return LWS_SSL_CAPABLE_ERROR;
}

LWS_VISIBLE int
lws_ssl_pending_no_ssl(struct lws *wsi)
{
	(void)wsi;
#if defined(LWS_WITH_ESP32)
	return 100;
#else
	return 0;
#endif
}

int
lws_vr_issue_raw1(struct lws *w, unsigned char *buf, size_t len)
{
 unsigned char *storage = w->vr_data.Tx.Pointer;
 size_t request = len, limit;
 int actual, pending = w->trunc_len != 0;
 if (!lws_vr_data_valid1(w) || w->vr_data.Tx.Stage != LWS_VR_ACQUIRED1 ||
     !storage || len > 65552 || (len && !buf) || w->vr_data.FramePending) return -1;
 if (!len) return 0;
 if (pending) {
  if (w->trunc_offset > 65552 || w->trunc_len > 65552 - w->trunc_offset ||
      buf != storage + w->trunc_offset || len > w->trunc_len) return -1;
 } else if (w->could_have_pending) return -1;
 limit = w->protocol->tx_packet_size ? w->protocol->tx_packet_size :
  w->protocol->rx_buffer_size ? w->protocol->rx_buffer_size : w->context->pt_serv_buf_size;
 if (limit > 65536) return -1;
 limit += LWS_PRE + 4;
 if (len > limit) len = limit;
 actual = lws_ssl_capable_write(w, buf, (int)len);
 if (actual == LWS_SSL_CAPABLE_ERROR) {
  w->socket_is_permanently_unusable = 1;
  lws_vr_request_close1(w, LWS_CLOSE_STATUS_NOSTATUS); return -1;
 }
 if (actual == LWS_SSL_CAPABLE_MORE_SERVICE) actual = 0;
 if (actual < 0 || (size_t)actual > len) return -1;
 w->could_have_pending = 1;
 if (pending) {
  w->trunc_offset += (unsigned int)actual; w->trunc_len -= (unsigned int)actual;
  if (!w->trunc_len) w->trunc_offset = 0;
  if (w->trunc_len) lws_callback_on_writable(w);
  return (int)(w->trunc_len ? actual : request);
 }
 if ((size_t)actual < request) {
  /* Even quota denial enqueues the actual bytes in owned storage. It is not
   * recorded as an attempted OS send or a fabricated EOF/failure receipt. */
  memmove(storage, buf + actual, request - (size_t)actual);
  w->trunc_offset = 0; w->trunc_len = (unsigned int)(request - (size_t)actual);
  lws_callback_on_writable(w);
 }
 return (int)request;
}
