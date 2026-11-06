/*
 * libwebsockets - small server side websockets and web server implementation
 *
 * Copyright (C) 2010-2018 Andy Green <andy@warmcat.com>
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

#ifndef LWS_BUILD_HASH
#define LWS_BUILD_HASH "unknown-build-hash"
#endif

const struct lws_role_ops *available_roles[] = {
#if defined(LWS_ROLE_H2)
	&role_ops_h2,
#endif
#if defined(LWS_ROLE_H1)
	&role_ops_h1,
#endif
#if defined(LWS_ROLE_WS)
	&role_ops_ws,
#endif
	NULL
};

const struct lws_event_loop_ops *available_event_libs[] = {
#if defined(LWS_WITH_POLL)
	&event_loop_ops_poll,
#endif
#if defined(LWS_WITH_LIBUV)
	&event_loop_ops_uv,
#endif
#if defined(LWS_WITH_LIBEVENT)
	&event_loop_ops_event,
#endif
#if defined(LWS_WITH_LIBEV)
	&event_loop_ops_ev,
#endif
	NULL
};

static const char *library_version = LWS_LIBRARY_VERSION " " LWS_BUILD_HASH;

/**
 * lws_get_library_version: get version and git hash library built from
 *
 *	returns a const char * to a string like "1.1 178d78c"
 *	representing the library version followed by the git head hash it
 *	was built from
 */
LWS_VISIBLE const char *
lws_get_library_version(void)
{
	return library_version;
}

int
lws_role_call_alpn_negotiated(struct lws *wsi, const char *alpn)
{
#if defined(LWS_WITH_TLS)
	if (!alpn)
		return 0;

	lwsl_info("%s: '%s'\n", __func__, alpn);

	LWS_FOR_EVERY_AVAILABLE_ROLE_START(ar)
		if (ar->alpn && !strcmp(ar->alpn, alpn) && ar->alpn_negotiated)
			return ar->alpn_negotiated(wsi, alpn);
	LWS_FOR_EVERY_AVAILABLE_ROLE_END;
#endif
	return 0;
}

static const char * const mount_protocols[] = {
	"http://",
	"https://",
	"file://",
	"cgi://",
	">http://",
	">https://",
	"callback://"
};

LWS_VISIBLE void *
lws_protocol_vh_priv_zalloc(struct lws_vhost *vhost,
			    const struct lws_protocols *prot, int size)
{
	int n = 0;

	/* allocate the vh priv array only on demand */
	if (!vhost->protocol_vh_privs) {
		vhost->protocol_vh_privs = (void **)lws_zalloc(
				vhost->count_protocols * sizeof(void *),
				"protocol_vh_privs");
		if (!vhost->protocol_vh_privs)
			return NULL;
	}

	while (n < vhost->count_protocols && &vhost->protocols[n] != prot)
		n++;

	if (n == vhost->count_protocols) {
		n = 0;
		while (n < vhost->count_protocols &&
		       strcmp(vhost->protocols[n].name, prot->name))
			n++;

		if (n == vhost->count_protocols)
			return NULL;
	}

	vhost->protocol_vh_privs[n] = lws_zalloc(size, "vh priv");
	return vhost->protocol_vh_privs[n];
}

LWS_VISIBLE void *
lws_protocol_vh_priv_get(struct lws_vhost *vhost,
			 const struct lws_protocols *prot)
{
	int n = 0;

	if (!vhost || !vhost->protocol_vh_privs)
		return NULL;

	while (n < vhost->count_protocols && &vhost->protocols[n] != prot)
		n++;

	if (n == vhost->count_protocols) {
		n = 0;
		while (n < vhost->count_protocols &&
		       strcmp(vhost->protocols[n].name, prot->name))
			n++;

		if (n == vhost->count_protocols) {
			lwsl_err("%s: unknown protocol %p\n", __func__, prot);
			return NULL;
		}
	}

	return vhost->protocol_vh_privs[n];
}

static const struct lws_protocol_vhost_options *
lws_vhost_protocol_options(struct lws_vhost *vh, const char *name)
{
	const struct lws_protocol_vhost_options *pvo = vh->pvo;

	if (!name)
		return NULL;

	while (pvo) {
		if (!strcmp(pvo->name, name))
			return pvo;
		pvo = pvo->next;
	}

	return NULL;
}

/*
 * inform every vhost that hasn't already done it, that
 * his protocols are initializing
 */
LWS_VISIBLE int
lws_protocol_init(struct lws_context *context)
{
 if (lws_vr_tracked1(context)) {
  unsigned int i;
  if (!context->vr.CoreComplete) return 1;
  for (i = 0; i < 2; ++i)
   if (context->vr.Vhosts[i].Object && !context->vr.Vhosts[i].Object->vr.ProtocolInit) return 1;
  return 0; /* actual tracked constructors performed these callbacks already */
 }
	struct lws_vhost *vh = context->vhost_list;
	const struct lws_protocol_vhost_options *pvo, *pvo1;
	struct lws wsi;
	int n, any = 0;

	if (context->doing_protocol_init)
		return 0;

	context->doing_protocol_init = 1;

	memset(&wsi, 0, sizeof(wsi));
	wsi.context = context;

	lwsl_info("%s\n", __func__);

	while (vh) {
		wsi.vhost = vh;

		/* only do the protocol init once for a given vhost */
		if (vh->created_vhost_protocols ||
		    (vh->options & LWS_SERVER_OPTION_SKIP_PROTOCOL_INIT))
			goto next;

		/* initialize supported protocols on this vhost */

		for (n = 0; n < vh->count_protocols; n++) {
			wsi.protocol = &vh->protocols[n];
			if (!vh->protocols[n].name)
				continue;
			pvo = lws_vhost_protocol_options(vh,
							 vh->protocols[n].name);
			if (pvo) {
				/*
				 * linked list of options specific to
				 * vh + protocol
				 */
				pvo1 = pvo;
				pvo = pvo1->options;

				while (pvo) {
					lwsl_debug(
						"    vhost \"%s\", "
						"protocol \"%s\", "
						"option \"%s\"\n",
							vh->name,
							vh->protocols[n].name,
							pvo->name);

					if (!strcmp(pvo->name, "default")) {
						lwsl_info("Setting default "
						   "protocol for vh %s to %s\n",
						   vh->name,
						   vh->protocols[n].name);
						vh->default_protocol_index = n;
					}
					if (!strcmp(pvo->name, "raw")) {
						lwsl_info("Setting raw "
						   "protocol for vh %s to %s\n",
						   vh->name,
						   vh->protocols[n].name);
						vh->raw_protocol_index = n;
					}
					pvo = pvo->next;
				}

				pvo = pvo1->options;
			}

#if defined(LWS_WITH_TLS)
			any |= !!vh->tls.ssl_ctx;
#endif

			/*
			 * inform all the protocols that they are doing their
			 * one-time initialization if they want to.
			 *
			 * NOTE the wsi is all zeros except for the context, vh
			 * + protocol ptrs so lws_get_context(wsi) etc can work
			 */
			if (vh->protocols[n].callback(&wsi,
					LWS_CALLBACK_PROTOCOL_INIT, NULL,
					(void *)pvo, 0)) {
				lws_free(vh->protocol_vh_privs[n]);
				vh->protocol_vh_privs[n] = NULL;
				lwsl_err("%s: protocol %s failed init\n", __func__,
					 vh->protocols[n].name);
			}
		}

		vh->created_vhost_protocols = 1;
next:
		vh = vh->vhost_next;
	}

	context->doing_protocol_init = 0;

	if (!context->protocol_init_done)
		lws_finalize_startup(context);

	context->protocol_init_done = 1;

	if (any)
		lws_tls_check_all_cert_lifetimes(context);

	return 0;
}

LWS_VISIBLE int
lws_callback_http_dummy(struct lws *wsi, enum lws_callback_reasons reason,
		    void *user, void *in, size_t len)
{
	struct lws_ssl_info *si;
#ifdef LWS_WITH_CGI
	struct lws_cgi_args *args;
#endif
#if defined(LWS_WITH_CGI) || defined(LWS_WITH_HTTP_PROXY)
	char buf[512];
	int n;
#endif

	switch (reason) {
#if defined(LWS_ROLE_H1) || defined(LWS_ROLE_H2)
	case LWS_CALLBACK_HTTP:
#ifndef LWS_NO_SERVER
		if (lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL))
			return -1;

		if (lws_http_transaction_completed(wsi))
#endif
			return -1;
		break;
#if !defined(LWS_NO_SERVER)
	case LWS_CALLBACK_HTTP_FILE_COMPLETION:
		if (lws_http_transaction_completed(wsi))
			return -1;
		break;
#endif

	case LWS_CALLBACK_HTTP_WRITEABLE:
#ifdef LWS_WITH_CGI
		if (wsi->reason_bf & (LWS_CB_REASON_AUX_BF__CGI_HEADERS |
				      LWS_CB_REASON_AUX_BF__CGI)) {
			n = lws_cgi_write_split_stdout_headers(wsi);
			if (n < 0) {
				lwsl_debug("AUX_BF__CGI forcing close\n");
				return -1;
			}
			if (!n)
				lws_rx_flow_control(
					wsi->http.cgi->stdwsi[LWS_STDOUT], 1);

			if (wsi->reason_bf & LWS_CB_REASON_AUX_BF__CGI_HEADERS)
				wsi->reason_bf &=
					~LWS_CB_REASON_AUX_BF__CGI_HEADERS;
			else
				wsi->reason_bf &= ~LWS_CB_REASON_AUX_BF__CGI;
			break;
		}

		if (wsi->reason_bf & LWS_CB_REASON_AUX_BF__CGI_CHUNK_END) {
			if (!wsi->http2_substream) {
				memcpy(buf + LWS_PRE, "0\x0d\x0a\x0d\x0a", 5);
				lwsl_debug("writing chunk term and exiting\n");
				n = lws_write(wsi, (unsigned char *)buf +
						   LWS_PRE, 5, LWS_WRITE_HTTP);
			} else
				n = lws_write(wsi, (unsigned char *)buf +
						   LWS_PRE, 0,
						   LWS_WRITE_HTTP_FINAL);

			/* always close after sending it */
			return -1;
		}
#endif
#if defined(LWS_WITH_HTTP_PROXY)
		if (wsi->reason_bf & LWS_CB_REASON_AUX_BF__PROXY) {
			char *px = buf + LWS_PRE;
			int lenx = sizeof(buf) - LWS_PRE;

			/*
			 * our sink is writeable and our source has something
			 * to read.  So read a lump of source material of
			 * suitable size to send or what's available, whichever
			 * is the smaller.
			 */
			wsi->reason_bf &= ~LWS_CB_REASON_AUX_BF__PROXY;
			if (!lws_get_child(wsi))
				break;
			if (lws_http_client_read(lws_get_child(wsi), &px,
						 &lenx) < 0)
				return -1;
			break;
		}
#endif
		break;

#if defined(LWS_WITH_HTTP_PROXY)
	case LWS_CALLBACK_RECEIVE_CLIENT_HTTP:
		assert(lws_get_parent(wsi));
		if (!lws_get_parent(wsi))
			break;
		lws_get_parent(wsi)->reason_bf |= LWS_CB_REASON_AUX_BF__PROXY;
		lws_callback_on_writable(lws_get_parent(wsi));
		break;

	case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
		assert(lws_get_parent(wsi));
		n = lws_write(lws_get_parent(wsi), (unsigned char *)in,
				len, LWS_WRITE_HTTP);
		if (n < 0)
			return -1;
		break;

	case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP: {
		unsigned char *p, *end;
		char ctype[64], ctlen = 0;
	
		p = (unsigned char *)buf + LWS_PRE;
		end = p + sizeof(buf) - LWS_PRE;

		if (lws_add_http_header_status(lws_get_parent(wsi),
					       HTTP_STATUS_OK, &p, end))
			return 1;
		if (lws_add_http_header_by_token(lws_get_parent(wsi),
				WSI_TOKEN_HTTP_SERVER,
			    	(unsigned char *)"libwebsockets",
				13, &p, end))
			return 1;

		ctlen = lws_hdr_copy(wsi, ctype, sizeof(ctype),
				     WSI_TOKEN_HTTP_CONTENT_TYPE);
		if (ctlen > 0) {
			if (lws_add_http_header_by_token(lws_get_parent(wsi),
				WSI_TOKEN_HTTP_CONTENT_TYPE,
				(unsigned char *)ctype, ctlen, &p, end))
				return 1;
		}

		if (lws_finalize_http_header(lws_get_parent(wsi), &p, end))
			return 1;

		*p = '\0';
		n = lws_write(lws_get_parent(wsi),
			      (unsigned char *)buf + LWS_PRE,
			      p - ((unsigned char *)buf + LWS_PRE),
			      LWS_WRITE_HTTP_HEADERS);
		if (n < 0)
			return -1;

		break; }

#endif

#ifdef LWS_WITH_CGI
	/* CGI IO events (POLLIN/OUT) appear here, our default policy is:
	 *
	 *  - POST data goes on subprocess stdin
	 *  - subprocess stdout goes on http via writeable callback
	 *  - subprocess stderr goes to the logs
	 */
	case LWS_CALLBACK_CGI:
		args = (struct lws_cgi_args *)in;
		switch (args->ch) { /* which of stdin/out/err ? */
		case LWS_STDIN:
			/* TBD stdin rx flow control */
			break;
		case LWS_STDOUT:
			/* quench POLLIN on STDOUT until MASTER got writeable */
			lws_rx_flow_control(args->stdwsi[LWS_STDOUT], 0);
			wsi->reason_bf |= LWS_CB_REASON_AUX_BF__CGI;
			/* when writing to MASTER would not block */
			lws_callback_on_writable(wsi);
			break;
		case LWS_STDERR:
			n = lws_get_socket_fd(args->stdwsi[LWS_STDERR]);
			if (n < 0)
				break;
			n = read(n, buf, sizeof(buf) - 2);
			if (n > 0) {
				if (buf[n - 1] != '\n')
					buf[n++] = '\n';
				buf[n] = '\0';
				lwsl_notice("CGI-stderr: %s\n", buf);
			}
			break;
		}
		break;

	case LWS_CALLBACK_CGI_TERMINATED:
		lwsl_debug("LWS_CALLBACK_CGI_TERMINATED: %d %" PRIu64 "\n",
				wsi->http.cgi->explicitly_chunked,
				(uint64_t)wsi->http.cgi->content_length);
		if (!wsi->http.cgi->explicitly_chunked &&
		    !wsi->http.cgi->content_length) {
			/* send terminating chunk */
			lwsl_debug("LWS_CALLBACK_CGI_TERMINATED: ending\n");
			wsi->reason_bf |= LWS_CB_REASON_AUX_BF__CGI_CHUNK_END;
			lws_callback_on_writable(wsi);
			lws_set_timeout(wsi, PENDING_TIMEOUT_CGI, 3);
			break;
		}
		return -1;

	case LWS_CALLBACK_CGI_STDIN_DATA:  /* POST body for stdin */
		args = (struct lws_cgi_args *)in;
		args->data[args->len] = '\0';
		n = lws_get_socket_fd(args->stdwsi[LWS_STDIN]);
		if (n < 0)
			return -1;
		n = write(n, args->data, args->len);
		if (n < args->len)
			lwsl_notice("LWS_CALLBACK_CGI_STDIN_DATA: "
				    "sent %d only %d went", n, args->len);
		return n;
#endif
#endif
	case LWS_CALLBACK_SSL_INFO:
		si = in;

		(void)si;
		lwsl_notice("LWS_CALLBACK_SSL_INFO: where: 0x%x, ret: 0x%x\n",
			    si->where, si->ret);
		break;

	default:
		break;
	}

	return 0;
}

/* list of supported protocols and callbacks */

static const struct lws_protocols protocols_dummy[] = {
	/* first protocol must always be HTTP handler */

	{
		"http-only",			/* name */
		lws_callback_http_dummy,	/* callback */
		0,				/* per_session_data_size */
		0,				/* rx_buffer_size */
		0,				/* id */
		NULL,				/* user */
		0				/* tx_packet_size */
	},
	/*
	 * the other protocols are provided by lws plugins
	 */
	{ NULL, NULL, 0, 0, 0, NULL, 0} /* terminator */
};

#ifdef LWS_PLAT_OPTEE
#undef LWS_HAVE_GETENV
#endif

static void
lws_vhost_destroy2(struct lws_vhost *vh);

LWS_VISIBLE struct lws_vhost *
lws_create_vhost(struct lws_context *context,
		 const struct lws_context_creation_info *info)
{
	if ((context->vr.View && context->vr.View->ResourceBinding))
		return NULL; /* genuine tracked entry required, no untracked allocation */
	struct lws_vhost *vh = lws_zalloc(sizeof(*vh), "create vhost"),
			 **vh1 = &context->vhost_list;
	const struct lws_http_mount *mounts;
	const struct lws_protocols *pcols = info->protocols;
	const struct lws_protocol_vhost_options *pvo;
#ifdef LWS_WITH_PLUGINS
	struct lws_plugin *plugin = context->plugin_list;
#endif
	struct lws_protocols *lwsp;
	int m, f = !info->pvo;
	char buf[20];
#if !defined(LWS_WITHOUT_CLIENT) && defined(LWS_HAVE_GETENV)
	char *p;
#endif
	int n;

	if (!vh)
		return NULL;

#if LWS_MAX_SMP > 1
	pthread_mutex_init(&vh->lock, NULL);
#endif

	if (!pcols)
		pcols = &protocols_dummy[0];

	vh->context = context;
	if (!info->vhost_name)
		vh->name = "default";
	else
		vh->name = info->vhost_name;

#if defined(LWS_ROLE_H1) || defined(LWS_ROLE_H2)
	vh->http.error_document_404 = info->error_document_404;
#endif

	if (info->options & LWS_SERVER_OPTION_ONLY_RAW)
		lwsl_info("%s set to only support RAW\n", vh->name);

	vh->iface = info->iface;
#if !defined(LWS_WITH_ESP32) && \
    !defined(OPTEE_TA) && !defined(WIN32)
	vh->bind_iface = info->bind_iface;
#endif

	for (vh->count_protocols = 0;
	     pcols[vh->count_protocols].callback;
	     vh->count_protocols++)
		;

	vh->options = info->options;
	vh->pvo = info->pvo;
	vh->headers = info->headers;
	vh->user = info->user;

	LWS_FOR_EVERY_AVAILABLE_ROLE_START(ar)
		if (ar->init_vhost)
			if (ar->init_vhost(vh, info))
				return NULL;
	LWS_FOR_EVERY_AVAILABLE_ROLE_END;


	if (info->keepalive_timeout)
		vh->keepalive_timeout = info->keepalive_timeout;
	else
		vh->keepalive_timeout = 5;

	if (info->timeout_secs_ah_idle)
		vh->timeout_secs_ah_idle = info->timeout_secs_ah_idle;
	else
		vh->timeout_secs_ah_idle = 10;

#if defined(LWS_WITH_TLS)

	vh->tls.alpn = info->alpn;
	vh->tls.ssl_info_event_mask = info->ssl_info_event_mask;

	if (info->ecdh_curve)
		lws_strncpy(vh->tls.ecdh_curve, info->ecdh_curve,
			    sizeof(vh->tls.ecdh_curve));

	/* carefully allocate and take a copy of cert + key paths if present */
	n = 0;
	if (info->ssl_cert_filepath)
		n += (int)strlen(info->ssl_cert_filepath) + 1;
	if (info->ssl_private_key_filepath)
		n += (int)strlen(info->ssl_private_key_filepath) + 1;

	if (n) {
		vh->tls.key_path = vh->tls.alloc_cert_path = lws_malloc(n, "vh paths");
		if (info->ssl_cert_filepath) {
			n = (int)strlen(info->ssl_cert_filepath) + 1;
			memcpy(vh->tls.alloc_cert_path, info->ssl_cert_filepath, n);
			vh->tls.key_path += n;
		}
		if (info->ssl_private_key_filepath)
			memcpy(vh->tls.key_path, info->ssl_private_key_filepath,
			       strlen(info->ssl_private_key_filepath) + 1);
	}
#endif

	/*
	 * give the vhost a unified list of protocols including the
	 * ones that came from plugins
	 */
	lwsp = lws_zalloc(sizeof(struct lws_protocols) * (vh->count_protocols +
				   context->plugin_protocol_count + 1),
				   "vhost-specific plugin table");
	if (!lwsp) {
		lwsl_err("OOM\n");
		return NULL;
	}

	m = vh->count_protocols;
	memcpy(lwsp, pcols, sizeof(struct lws_protocols) * m);

	/* for compatibility, all protocols enabled on vhost if only
	 * the default vhost exists.  Otherwise only vhosts who ask
	 * for a protocol get it enabled.
	 */

	if (context->options & LWS_SERVER_OPTION_EXPLICIT_VHOSTS)
		f = 0;
	(void)f;
#ifdef LWS_WITH_PLUGINS
	if (plugin) {

		while (plugin) {
			for (n = 0; n < plugin->caps.count_protocols; n++) {
				/*
				 * for compatibility's sake, no pvo implies
				 * allow all protocols
				 */
				if (f || lws_vhost_protocol_options(vh,
				    plugin->caps.protocols[n].name)) {
					memcpy(&lwsp[m],
					       &plugin->caps.protocols[n],
					       sizeof(struct lws_protocols));
					m++;
					vh->count_protocols++;
				}
			}
			plugin = plugin->list;
		}
	}
#endif

	if (
#ifdef LWS_WITH_PLUGINS
	    (context->plugin_list) ||
#endif
	    context->options & LWS_SERVER_OPTION_EXPLICIT_VHOSTS)
		vh->protocols = lwsp;
	else {
		vh->protocols = pcols;
		lws_free(lwsp);
	}

	vh->same_vh_protocol_list = (struct lws **)
			lws_zalloc(sizeof(struct lws *) * vh->count_protocols,
				   "same vh list");
#if defined(LWS_ROLE_H1) || defined(LWS_ROLE_H2)
	vh->http.mount_list = info->mounts;
#endif

#ifdef LWS_WITH_UNIX_SOCK
	if (LWS_UNIX_SOCK_ENABLED(context)) {
		lwsl_notice("Creating Vhost '%s' path \"%s\", %d protocols\n",
				vh->name, vh->iface, vh->count_protocols);
	} else
#endif
	{
		switch(info->port) {
		case CONTEXT_PORT_NO_LISTEN:
			strcpy(buf, "(serving disabled)");
			break;
		case CONTEXT_PORT_NO_LISTEN_SERVER:
			strcpy(buf, "(no listener)");
			break;
		default:
			lws_snprintf(buf, sizeof(buf), "port %u", info->port);
			break;
		}
		lwsl_notice("Creating Vhost '%s' %s, %d protocols, IPv6 %s\n",
				vh->name, buf, vh->count_protocols,
				LWS_IPV6_ENABLED(vh) ? "on" : "off");
	}
	mounts = info->mounts;
	while (mounts) {
		(void)mount_protocols[0];
		lwsl_info("   mounting %s%s to %s\n",
			  mount_protocols[mounts->origin_protocol],
			  mounts->origin, mounts->mountpoint);

		/* convert interpreter protocol names to pointers */
		pvo = mounts->interpret;
		while (pvo) {
			for (n = 0; n < vh->count_protocols; n++) {
				if (strcmp(pvo->value, vh->protocols[n].name))
					continue;
				((struct lws_protocol_vhost_options *)pvo)->
					value = (const char *)(lws_intptr_t)n;
				break;
			}
			if (n == vh->count_protocols)
				lwsl_err("ignoring unknown interp pr %s\n",
					 pvo->value);
			pvo = pvo->next;
		}

		mounts = mounts->mount_next;
	}

	vh->listen_port = info->port;
#if defined(LWS_ROLE_H1) || defined(LWS_ROLE_H2)
	vh->http.http_proxy_port = 0;
	vh->http.http_proxy_address[0] = '\0';
#endif
#if defined(LWS_WITH_SOCKS5)
	vh->socks_proxy_port = 0;
	vh->socks_proxy_address[0] = '\0';
#endif

#if !defined(LWS_WITHOUT_CLIENT)
	/* either use proxy from info, or try get it from env var */
#if defined(LWS_ROLE_H1) || defined(LWS_ROLE_H2)
	/* http proxy */
	if (info->http_proxy_address) {
		/* override for backwards compatibility */
		if (info->http_proxy_port)
			vh->http.http_proxy_port = info->http_proxy_port;
		lws_set_proxy(vh, info->http_proxy_address);
	} else
#endif
	{
#ifdef LWS_HAVE_GETENV
		p = getenv("http_proxy");
		if (p)
			lws_set_proxy(vh, p);
#endif
	}
#endif
#if defined(LWS_WITH_SOCKS5)
	/* socks proxy */
	if (info->socks_proxy_address) {
		/* override for backwards compatibility */
		if (info->socks_proxy_port)
			vh->socks_proxy_port = info->socks_proxy_port;
		lws_set_socks(vh, info->socks_proxy_address);
	} else {
#ifdef LWS_HAVE_GETENV
		p = getenv("socks_proxy");
		if (p)
			lws_set_socks(vh, p);
#endif
	}
#endif

	vh->ka_time = info->ka_time;
	vh->ka_interval = info->ka_interval;
	vh->ka_probes = info->ka_probes;

	if (vh->options & LWS_SERVER_OPTION_STS)
		lwsl_notice("   STS enabled\n");

#ifdef LWS_WITH_ACCESS_LOG
	if (info->log_filepath) {
		vh->log_fd = open(info->log_filepath,
				  O_CREAT | O_APPEND | O_RDWR, 0600);
		if (vh->log_fd == (int)LWS_INVALID_FILE) {
			lwsl_err("unable to open log filepath %s\n",
				 info->log_filepath);
			goto bail;
		}
#ifndef WIN32
		if (context->uid != -1)
			if (chown(info->log_filepath, context->uid,
				  context->gid) == -1)
				lwsl_err("unable to chown log file %s\n",
						info->log_filepath);
#endif
	} else
		vh->log_fd = (int)LWS_INVALID_FILE;
#endif
	if (lws_context_init_server_ssl(info, vh)) {
		lwsl_err("%s: lws_context_init_server_ssl failed\n", __func__);
		goto bail1;
	}
	if (lws_context_init_client_ssl(info, vh)) {
		lwsl_err("%s: lws_context_init_client_ssl failed\n", __func__);
		goto bail1;
	}
	lws_context_lock(context);
	n = _lws_vhost_init_server(info, vh);
	lws_context_unlock(context);
	if (n < 0) {
		lwsl_err("init server failed\n");
		goto bail1;
	}


	while (1) {
		if (!(*vh1)) {
			*vh1 = vh;
			break;
		}
		vh1 = &(*vh1)->vhost_next;
	};

	/* for the case we are adding a vhost much later, after server init */

	if (context->protocol_init_done)
		if (lws_protocol_init(context)) {
			lwsl_err("%s: lws_protocol_init failed\n", __func__);
			goto bail1;
		}

	return vh;

bail1:
	lws_vhost_destroy(vh);
	lws_vhost_destroy2(vh);

	return NULL;

#ifdef LWS_WITH_ACCESS_LOG
bail:
	lws_free(vh);
#endif

	return NULL;
}

LWS_VISIBLE int
lws_init_vhost_client_ssl(const struct lws_context_creation_info *info,
			  struct lws_vhost *vhost)
{
	struct lws_context_creation_info i;

	memcpy(&i, info, sizeof(i));
	i.port = CONTEXT_PORT_NO_LISTEN;

	return lws_context_init_client_ssl(&i, vhost);
}

LWS_VISIBLE void
lws_cancel_service_pt(struct lws *wsi)
{
	lws_plat_pipe_signal(wsi);
}

LWS_VISIBLE void
lws_cancel_service(struct lws_context *context)
{
	struct lws_context_per_thread *pt = &context->pt[0];
	short m = context->count_threads;

	if (context->being_destroyed1)
		return;

	lwsl_info("%s\n", __func__);

	while (m--) {
		if (pt->pipe_wsi)
			lws_plat_pipe_signal(pt->pipe_wsi);
		pt++;
	}
}

int
lws_create_event_pipes(struct lws_context *context)
{
	struct lws *wsi;
	int n;

	if ((context->vr.View && context->vr.View->ResourceBinding))
		return lws_vr_event_create1(context);

	/*
	 * Create the pt event pipes... these are unique in that they are
	 * not bound to a vhost or protocol (both are NULL)
	 */

	for (n = 0; n < context->count_threads; n++) {
		if (context->pt[n].pipe_wsi)
			continue;

		wsi = lws_zalloc(sizeof(*wsi), "event pipe wsi");
		if (!wsi) {
			lwsl_err("Out of mem\n");
			return 1;
		}
		wsi->context = context;
		lws_role_transition(wsi, 0, LRS_UNCONNECTED, &role_ops_pipe);
		wsi->protocol = NULL;
		wsi->tsi = n;
		wsi->vhost = NULL;
		wsi->event_pipe = 1;

		if (lws_plat_pipe_create(wsi)) {
			lws_free(wsi);
			continue;
		}
		wsi->desc.sockfd = context->pt[n].dummy_pipe_fds[0];
		lwsl_debug("event pipe fd %d\n", wsi->desc.sockfd);

		context->pt[n].pipe_wsi = wsi;

		if (context->event_loop_ops->accept)
			context->event_loop_ops->accept(wsi);

		if (__insert_wsi_socket_into_fds(context, wsi))
			return 1;

		//lws_change_pollfd(context->pt[n].pipe_wsi, 0, LWS_POLLIN);
		context->count_wsi_allocated++;
	}

	return 0;
}

void
lws_destroy_event_pipe(struct lws *wsi)
{
	if ((wsi->context->vr.View && wsi->context->vr.View->ResourceBinding)) {
		(void)lws_vr_event_destroy1(wsi);
		return;
	}
	lwsl_info("%s\n", __func__);
	__remove_wsi_socket_from_fds(wsi);

	if (wsi->context->event_loop_ops->wsi_logical_close) {
		wsi->context->event_loop_ops->wsi_logical_close(wsi);
		lws_plat_pipe_close(wsi);
		return;
	}

	if (wsi->context->event_loop_ops->destroy_wsi)
		wsi->context->event_loop_ops->destroy_wsi(wsi);
	lws_plat_pipe_close(wsi);
	wsi->context->count_wsi_allocated--;
	lws_free(wsi);
}

LWS_VISIBLE struct lws_context *
lws_create_context(const struct lws_context_creation_info *info)
{
	struct lws_context *context = NULL;
	struct lws_plat_file_ops *prev;
#ifndef LWS_NO_DAEMONIZE
	int pid_daemon = get_daemonize_pid();
#endif
	int n;
#if defined(__ANDROID__)
	struct rlimit rt;
#endif



	lwsl_info("Initial logging level %d\n", log_level);
	lwsl_info("Libwebsockets version: %s\n", library_version);
#if defined(GCC_VER)
	lwsl_info("Compiled with  %s\n", GCC_VER);
#endif

#ifdef LWS_WITH_IPV6
	if (!lws_check_opt(info->options, LWS_SERVER_OPTION_DISABLE_IPV6))
		lwsl_info("IPV6 compiled in and enabled\n");
	else
		lwsl_info("IPV6 compiled in but disabled\n");
#else
	lwsl_info("IPV6 not compiled in\n");
#endif

	lwsl_info(" LWS_DEF_HEADER_LEN    : %u\n", LWS_DEF_HEADER_LEN);
	lwsl_info(" LWS_MAX_PROTOCOLS     : %u\n", LWS_MAX_PROTOCOLS);
	lwsl_info(" LWS_MAX_SMP           : %u\n", LWS_MAX_SMP);
	lwsl_info(" sizeof (*info)        : %ld\n", (long)sizeof(*info));
#if defined(LWS_WITH_STATS)
	lwsl_info(" LWS_WITH_STATS        : on\n");
#endif
	lwsl_info(" SYSTEM_RANDOM_FILEPATH: '%s'\n", SYSTEM_RANDOM_FILEPATH);
#if defined(LWS_WITH_HTTP2)
	lwsl_info(" HTTP2 support         : available\n");
#else
	lwsl_info(" HTTP2 support         : not configured\n");
#endif
	if (lws_plat_context_early_init())
		return NULL;

	context = lws_zalloc(sizeof(struct lws_context), "context");
	if (!context) {
		lwsl_err("No memory for websocket context\n");
		return NULL;
	}

#if defined(LWS_WITH_TLS)
#if defined(LWS_WITH_MBEDTLS)
	context->tls_ops = &tls_ops_mbedtls;
#else
	context->tls_ops = &tls_ops_openssl;
#endif
#endif

	if (info->pt_serv_buf_size)
		context->pt_serv_buf_size = info->pt_serv_buf_size;
	else
		context->pt_serv_buf_size = 4096;

#if defined(LWS_ROLE_H2)
	role_ops_h2.init_context(context, info);
#endif

#if LWS_MAX_SMP > 1
	pthread_mutex_init(&context->lock, NULL);
#endif

#if defined(LWS_WITH_ESP32)
	context->last_free_heap = esp_get_free_heap_size();
#endif

	/* default to just the platform fops implementation */

	context->fops_platform.LWS_FOP_OPEN	= _lws_plat_file_open;
	context->fops_platform.LWS_FOP_CLOSE	= _lws_plat_file_close;
	context->fops_platform.LWS_FOP_SEEK_CUR	= _lws_plat_file_seek_cur;
	context->fops_platform.LWS_FOP_READ	= _lws_plat_file_read;
	context->fops_platform.LWS_FOP_WRITE	= _lws_plat_file_write;
	context->fops_platform.fi[0].sig	= NULL;

	/*
	 *  arrange a linear linked-list of fops starting from context->fops
	 *
	 * platform fops
	 * [ -> fops_zip (copied into context so .next settable) ]
	 * [ -> info->fops ]
	 */

	context->fops = &context->fops_platform;
	prev = (struct lws_plat_file_ops *)context->fops;

#if defined(LWS_WITH_ZIP_FOPS)
	/* make a soft copy so we can set .next */
	context->fops_zip = fops_zip;
	prev->next = &context->fops_zip;
	prev = (struct lws_plat_file_ops *)prev->next;
#endif

	/* if user provided fops, tack them on the end of the list */
	if (info->fops)
		prev->next = info->fops;

	context->reject_service_keywords = info->reject_service_keywords;
	if (info->external_baggage_free_on_destroy)
		context->external_baggage_free_on_destroy =
			info->external_baggage_free_on_destroy;

	context->time_up = time(NULL);
	context->pcontext_finalize = info->pcontext;

	context->simultaneous_ssl_restriction =
			info->simultaneous_ssl_restriction;

#ifndef LWS_NO_DAEMONIZE
	if (pid_daemon) {
		context->started_with_parent = pid_daemon;
		lwsl_info(" Started with daemon pid %d\n", pid_daemon);
	}
#endif
#if defined(__ANDROID__)
		n = getrlimit ( RLIMIT_NOFILE,&rt);
		if (-1 == n) {
			lwsl_err("Get RLIMIT_NOFILE failed!\n");
			return NULL;
		}
		context->max_fds = rt.rlim_cur;
#else
		context->max_fds = getdtablesize();
#endif

	if (info->count_threads)
		context->count_threads = info->count_threads;
	else
		context->count_threads = 1;

	if (context->count_threads > LWS_MAX_SMP)
		context->count_threads = LWS_MAX_SMP;

	context->token_limits = info->token_limits;

	context->options = info->options;

	/*
	 * set the context event loops ops struct
	 *
	 * after this, all event_loop actions use the generic ops
	 */

#if defined(LWS_WITH_POLL)
	context->event_loop_ops = &event_loop_ops_poll;
#endif

	if (lws_check_opt(context->options, LWS_SERVER_OPTION_LIBUV))
#if defined(LWS_WITH_LIBUV)
		context->event_loop_ops = &event_loop_ops_uv;
#else
		goto fail_event_libs;
#endif

	if (lws_check_opt(context->options, LWS_SERVER_OPTION_LIBEV))
#if defined(LWS_WITH_LIBEV)
		context->event_loop_ops = &event_loop_ops_ev;
#else
		goto fail_event_libs;
#endif

	if (lws_check_opt(context->options, LWS_SERVER_OPTION_LIBEVENT))
#if defined(LWS_WITH_LIBEVENT)
		context->event_loop_ops = &event_loop_ops_event;
#else
		goto fail_event_libs;
#endif

	if (!context->event_loop_ops)
		goto fail_event_libs;

	lwsl_info("Using event loop: %s\n", context->event_loop_ops->name);

#if defined(LWS_WITH_TLS)
	time(&context->tls.last_cert_check_s);
	if (info->alpn)
		context->tls.alpn_default = info->alpn;
	else {
		char *p = context->tls.alpn_discovered, first = 1;

		LWS_FOR_EVERY_AVAILABLE_ROLE_START(ar) {
			if (ar->alpn) {
				if (!first)
					*p++ = ',';
				p += lws_snprintf(p,
					context->tls.alpn_discovered +
					sizeof(context->tls.alpn_discovered) -
					2 - p, "%s", ar->alpn);
				first = 0;
			}
		} LWS_FOR_EVERY_AVAILABLE_ROLE_END;

		context->tls.alpn_default = context->tls.alpn_discovered;
	}

	lwsl_info("Default ALPN advertisment: %s\n", context->tls.alpn_default);
#endif

	if (info->timeout_secs)
		context->timeout_secs = info->timeout_secs;
	else
		context->timeout_secs = AWAITING_TIMEOUT;

	context->ws_ping_pong_interval = info->ws_ping_pong_interval;

	lwsl_info(" default timeout (secs): %u\n", context->timeout_secs);

	if (info->max_http_header_data)
		context->max_http_header_data = info->max_http_header_data;
	else
		if (info->max_http_header_data2)
			context->max_http_header_data =
					info->max_http_header_data2;
		else
			context->max_http_header_data = LWS_DEF_HEADER_LEN;

	if (info->max_http_header_pool)
		context->max_http_header_pool = info->max_http_header_pool;
	else
		/* The pool field must represent a positive OS fd limit. */
		context->max_http_header_pool = context->max_fds > SHRT_MAX ?
					      SHRT_MAX : context->max_fds;

	if (info->fd_limit_per_thread)
		context->fd_limit_per_thread = info->fd_limit_per_thread;
	else
		context->fd_limit_per_thread = context->max_fds /
					       context->count_threads;

	/*
	 * Allocate the per-thread storage for scratchpad buffers,
	 * and header data pool
	 */
	for (n = 0; n < context->count_threads; n++) {
		context->pt[n].serv_buf = lws_malloc(context->pt_serv_buf_size,
						     "pt_serv_buf");
		if (!context->pt[n].serv_buf) {
			lwsl_err("OOM\n");
			return NULL;
		}

		context->pt[n].context = context;
		context->pt[n].tid = n;

#if defined(LWS_ROLE_H1) || defined(LWS_ROLE_H2)
		context->pt[n].http.ah_list = NULL;
		context->pt[n].http.ah_pool_length = 0;
#endif
		lws_pt_mutex_init(&context->pt[n]);
	}

	lwsl_info(" Threads: %d each %d fds\n", context->count_threads,
		    context->fd_limit_per_thread);

	if (!info->ka_interval && info->ka_time > 0) {
		lwsl_err("info->ka_interval can't be 0 if ka_time used\n");
		return NULL;
	}


#if defined(LWS_WITH_PEER_LIMITS)
	/* scale the peer hash table according to the max fds for the process,
	 * so that the max list depth averages 16.  Eg, 1024 fd -> 64,
	 * 102400 fd -> 6400
	 */
	context->pl_hash_elements =
		(context->count_threads * context->fd_limit_per_thread) / 16;
	context->pl_hash_table = lws_zalloc(sizeof(struct lws_peer *) *
			context->pl_hash_elements, "peer limits hash table");
	context->ip_limit_ah = info->ip_limit_ah;
	context->ip_limit_wsi = info->ip_limit_wsi;
#endif

	lwsl_info(" mem: context:         %5lu B (%ld ctx + (%ld thr x %d))\n",
		  (long)sizeof(struct lws_context) +
		  (context->count_threads * context->pt_serv_buf_size),
		  (long)sizeof(struct lws_context),
		  (long)context->count_threads,
		  context->pt_serv_buf_size);
#if defined(LWS_ROLE_H1) || defined(LWS_ROLE_H2)
	lwsl_info(" mem: http hdr rsvd:   %5lu B (%u thr x (%u + %lu) x %u))\n",
		    (long)(context->max_http_header_data +
		     sizeof(struct allocated_headers)) *
		    context->max_http_header_pool * context->count_threads,
		    context->count_threads,
		    context->max_http_header_data,
		    (long)sizeof(struct allocated_headers),
		    context->max_http_header_pool);
#endif
	n = sizeof(struct lws_pollfd) * context->count_threads *
	    context->fd_limit_per_thread;
	context->pt[0].fds = lws_zalloc(n, "fds table");
	if (context->pt[0].fds == NULL) {
		lwsl_err("OOM allocating %d fds\n", context->max_fds);
		goto bail;
	}
	lwsl_info(" mem: pollfd map:      %5u\n", n);

	if (info->server_string) {
		context->server_string = info->server_string;
		context->server_string_len = (short)
				strlen(context->server_string);
	}

#if LWS_MAX_SMP > 1
	/* each thread serves his own chunk of fds */
	for (n = 1; n < (int)info->count_threads; n++)
		context->pt[n].fds = context->pt[n - 1].fds +
				     context->fd_limit_per_thread;
#endif

	if (lws_plat_init(context, info))
		goto bail;

	if (context->event_loop_ops->init_context)
		if (context->event_loop_ops->init_context(context, info))
			goto bail;


	if (context->event_loop_ops->init_pt)
		for (n = 0; n < context->count_threads; n++) {
			void *lp = NULL;

			if (info->foreign_loops)
				lp = info->foreign_loops[n];

			if (context->event_loop_ops->init_pt(context, lp, n))
				goto bail;
		}

	if (lws_create_event_pipes(context))
		goto bail;

	lws_context_init_ssl_library(info);

	context->user_space = info->user;

	/*
	 * if he's not saying he'll make his own vhosts later then act
	 * compatibly and make a default vhost using the data in the info
	 */
	if (!lws_check_opt(info->options, LWS_SERVER_OPTION_EXPLICIT_VHOSTS))
		if (!lws_create_vhost(context, info)) {
			lwsl_err("Failed to create default vhost\n");
			for (n = 0; n < context->count_threads; n++)
				lws_free_set_NULL(context->pt[n].serv_buf);
#if defined(LWS_WITH_PEER_LIMITS)
			lws_free_set_NULL(context->pl_hash_table);
#endif
			lws_free_set_NULL(context->pt[0].fds);
			lws_plat_context_late_destroy(context);
			lws_free_set_NULL(context);
			return NULL;
		}

	lws_context_init_extensions(info, context);

	lwsl_info(" mem: per-conn:        %5lu bytes + protocol rx buf\n",
		    (unsigned long)sizeof(struct lws));

	strcpy(context->canonical_hostname, "unknown");
	lws_server_get_canonical_hostname(context, info);

	context->uid = info->uid;
	context->gid = info->gid;

#if defined(LWS_HAVE_SYS_CAPABILITY_H) && defined(LWS_HAVE_LIBCAP)
	memcpy(context->caps, info->caps, sizeof(context->caps));
	context->count_caps = info->count_caps;
#endif

	/*
	 * drop any root privs for this process
	 * to listen on port < 1023 we would have needed root, but now we are
	 * listening, we don't want the power for anything else
	 */
	if (!lws_check_opt(info->options, LWS_SERVER_OPTION_EXPLICIT_VHOSTS))
		lws_plat_drop_app_privileges(info);

	/* expedite post-context init (eg, protocols) */
	lws_cancel_service(context);

#if defined(LWS_WITH_SELFTESTS)
	lws_jws_selftest();
#endif

	return context;

bail:
	lws_context_destroy(context);

	return NULL;

fail_event_libs:
	lwsl_err("Requested event library support not configured, available:\n");
	{
		const struct lws_event_loop_ops **elops = available_event_libs;

		while (*elops) {
			lwsl_err("  - %s\n", (*elops)->name);
			elops++;
		}
	}
	lws_free(context);

	return NULL;
}

LWS_VISIBLE LWS_EXTERN void
lws_context_deprecate(struct lws_context *context, lws_reload_func cb)
{
	struct lws_vhost *vh = context->vhost_list, *vh1;
	struct lws *wsi;

	/*
	 * "deprecation" means disable the context from accepting any new
	 * connections and free up listen sockets to be used by a replacement
	 * context.
	 *
	 * Otherwise the deprecated context remains operational, until its
	 * number of connected sockets falls to zero, when it is deleted.
	 */

	/* for each vhost, close his listen socket */

	while (vh) {
		wsi = vh->lserv_wsi;
		if (wsi) {
			wsi->socket_is_permanently_unusable = 1;
			lws_close_free_wsi(wsi, LWS_CLOSE_STATUS_NOSTATUS, "ctx deprecate");
			wsi->context->deprecation_pending_listen_close_count++;
			/*
			 * other vhosts can share the listen port, they
			 * point to the same wsi.  So zap those too.
			 */
			vh1 = context->vhost_list;
			while (vh1) {
				if (vh1->lserv_wsi == wsi)
					vh1->lserv_wsi = NULL;
				vh1 = vh1->vhost_next;
			}
		}
		vh = vh->vhost_next;
	}

	context->deprecated = 1;
	context->deprecation_cb = cb;
}

LWS_VISIBLE LWS_EXTERN int
lws_context_is_deprecated(struct lws_context *context)
{
	return context->deprecated;
}

void
lws_vhost_destroy1(struct lws_vhost *vh)
{
	const struct lws_protocols *protocol = NULL;
	struct lws_context_per_thread *pt;
	int n, m = vh->context->count_threads;
	struct lws_context *context = vh->context;
	struct lws wsi;

	lwsl_info("%s\n", __func__);

	if (vh->being_destroyed)
		return;

	vh->being_destroyed = 1;

	/*
	 * Are there other vhosts that are piggybacking on our listen socket?
	 * If so we need to hand the listen socket off to one of the others
	 * so it will remain open.  If not, leave it attached to the closing
	 * vhost and it will get closed.
	 */

	if (vh->lserv_wsi)
		lws_start_foreach_ll(struct lws_vhost *, v,
				     context->vhost_list) {
			if (v != vh &&
			    !v->being_destroyed &&
			    v->listen_port == vh->listen_port &&
			    ((!v->iface && !vh->iface) ||
			    (v->iface && vh->iface &&
			    !strcmp(v->iface, vh->iface)))) {
				/*
				 * this can only be a listen wsi, which is
				 * restricted... it has no protocol or other
				 * bindings or states.  So we can simply
				 * swap it to a vhost that has the same
				 * iface + port, but is not closing.
				 */
				assert(v->lserv_wsi == NULL);
				v->lserv_wsi = vh->lserv_wsi;
				vh->lserv_wsi = NULL;
				if (v->lserv_wsi)
					v->lserv_wsi->vhost = v;

				lwsl_notice("%s: listen skt from %s to %s\n",
					    __func__, vh->name, v->name);
				break;
			}
		} lws_end_foreach_ll(v, vhost_next);

	/*
	 * Forcibly close every wsi assoicated with this vhost.  That will
	 * include the listen socket if it is still associated with the closing
	 * vhost.
	 */

	while (m--) {
		pt = &context->pt[m];

		for (n = 0; (unsigned int)n < context->pt[m].fds_count; n++) {
			struct lws *wsi = wsi_from_fd(context, pt->fds[n].fd);
			if (!wsi)
				continue;
			if (wsi->vhost != vh)
				continue;

			lws_close_free_wsi(wsi,
				LWS_CLOSE_STATUS_NOSTATUS_CONTEXT_DESTROY,
				"vh destroy"
				/* no protocol close */);
			n--;
		}
	}

	/*
	 * destroy any pending timed events
	 */

	while (vh->timed_vh_protocol_list)
		lws_timed_callback_remove(vh, vh->timed_vh_protocol_list);

	/*
	 * let the protocols destroy the per-vhost protocol objects
	 */

	memset(&wsi, 0, sizeof(wsi));
	wsi.context = vh->context;
	wsi.vhost = vh;
	protocol = vh->protocols;
	if (protocol && vh->created_vhost_protocols) {
		n = 0;
		while (n < vh->count_protocols) {
			wsi.protocol = protocol;
			protocol->callback(&wsi, LWS_CALLBACK_PROTOCOL_DESTROY,
					   NULL, NULL, 0);
			protocol++;
			n++;
		}
	}

	/*
	 * remove vhost from context list of vhosts
	 */

	lws_start_foreach_llp(struct lws_vhost **, pv, context->vhost_list) {
		if (*pv == vh) {
			*pv = vh->vhost_next;
			break;
		}
	} lws_end_foreach_llp(pv, vhost_next);

	/* add ourselves to the pending destruction list */

	vh->vhost_next = vh->context->vhost_pending_destruction_list;
	vh->context->vhost_pending_destruction_list = vh;
}

static void
lws_vhost_destroy2(struct lws_vhost *vh)
{
	const struct lws_protocols *protocol = NULL;
	struct lws_context *context = vh->context;
	struct lws_deferred_free *df;
	int n;

	lwsl_info("%s: %p\n", __func__, vh);

	/* if we are still on deferred free list, remove ourselves */

	lws_start_foreach_llp(struct lws_deferred_free **, pdf,
			      context->deferred_free_list) {
		if ((*pdf)->payload == vh) {
			df = *pdf;
			*pdf = df->next;
			lws_free(df);
			break;
		}
	} lws_end_foreach_llp(pdf, next);

	/* remove ourselves from the pending destruction list */

	lws_start_foreach_llp(struct lws_vhost **, pv,
			      context->vhost_pending_destruction_list) {
		if ((*pv) == vh) {
			*pv = (*pv)->vhost_next;
			break;
		}
	} lws_end_foreach_llp(pv, vhost_next);

	/*
	 * Free all the allocations associated with the vhost
	 */

	protocol = vh->protocols;
	if (protocol) {
		n = 0;
		while (n < vh->count_protocols) {
			if (vh->protocol_vh_privs &&
			    vh->protocol_vh_privs[n]) {
				lws_free(vh->protocol_vh_privs[n]);
				vh->protocol_vh_privs[n] = NULL;
			}
			protocol++;
			n++;
		}
	}
	if (vh->protocol_vh_privs)
		lws_free(vh->protocol_vh_privs);
	lws_ssl_SSL_CTX_destroy(vh);
	lws_free(vh->same_vh_protocol_list);

	if (context->plugin_list ||
	    (context->options & LWS_SERVER_OPTION_EXPLICIT_VHOSTS))
		lws_free((void *)vh->protocols);

	LWS_FOR_EVERY_AVAILABLE_ROLE_START(ar)
		if (ar->destroy_vhost)
			ar->destroy_vhost(vh);
	LWS_FOR_EVERY_AVAILABLE_ROLE_END;

#ifdef LWS_WITH_ACCESS_LOG
	if (vh->log_fd != (int)LWS_INVALID_FILE)
		close(vh->log_fd);
#endif

#if defined (LWS_WITH_TLS)
	lws_free_set_NULL(vh->tls.alloc_cert_path);
#endif

#if LWS_MAX_SMP > 1
       pthread_mutex_destroy(&vh->lock);
#endif

#if defined(LWS_WITH_UNIX_SOCK)
	if (LWS_UNIX_SOCK_ENABLED(context)) {
		n = unlink(vh->iface);
		if (n)
			lwsl_info("Closing unix socket %s: errno %d\n",
				  vh->iface, errno);
	}
#endif
	/*
	 * although async event callbacks may still come for wsi handles with
	 * pending close in the case of asycn event library like libuv,
	 * they do not refer to the vhost.  So it's safe to free.
	 */

	lwsl_info("  %s: Freeing vhost %p\n", __func__, vh);

	memset(vh, 0, sizeof(*vh));
	lws_free(vh);
}

int
lws_check_deferred_free(struct lws_context *context, int force)
{
	struct lws_deferred_free *df;
	time_t now = lws_now_secs();

	lws_start_foreach_llp(struct lws_deferred_free **, pdf,
			      context->deferred_free_list) {
		if (force ||
		    lws_compare_time_t(context, now, (*pdf)->deadline) > 5) {
			df = *pdf;
			*pdf = df->next;
			/* finalize vh destruction */
			lwsl_notice("deferred vh %p destroy\n", df->payload);
			lws_vhost_destroy2(df->payload);
			lws_free(df);
			continue; /* after deletion we already point to next */
		}
	} lws_end_foreach_llp(pdf, next);

	return 0;
}

LWS_VISIBLE void
lws_vhost_destroy(struct lws_vhost *vh)
{
	struct lws_deferred_free *df = lws_malloc(sizeof(*df), "deferred free");

	if (!df)
		return;

	lws_vhost_destroy1(vh);

	/* part 2 is deferred to allow all the handle closes to complete */

	df->next = vh->context->deferred_free_list;
	df->deadline = lws_now_secs();
	df->payload = vh;
	vh->context->deferred_free_list = df;
}

/*
 * When using an event loop, the context destruction is in three separate
 * parts.  This is to cover both internal and foreign event loops cleanly.
 *
 *  - lws_context_destroy() simply starts a soft close of all wsi and
 *     related allocations.  The event loop continues.
 *
 *     As the closes complete in the event loop, reference counting is used
 *     to determine when everything is closed.  It then calls
 *     lws_context_destroy2().
 *
 *  - lws_context_destroy2() cleans up the rest of the higher-level logical
 *     lws pieces like vhosts.  If the loop was foreign, it then proceeds to
 *     lws_context_destroy3().  If it the loop is internal, it stops the
 *     internal loops and waits for lws_context_destroy() to be called again
 *     outside the event loop (since we cannot destroy the loop from
 *     within the loop).  That will cause lws_context_destroy3() to run
 *     directly.
 *
 *  - lws_context_destroy3() destroys any internal event loops and then
 *     destroys the context itself, setting what was info.pcontext to NULL.
 */

/*
 * destroy the actual context itself
 */

static void
lws_context_destroy3(struct lws_context *context)
{
	if (context && (context->vr.View && context->vr.View->ResourceBinding)) {
		context->vr.Stage = LWS_VR_CONTEXT_RETAINED1;
		context->vr.View->Control->Fault |= VR_LWS_SOURCE_FAULT_PROTOCOL1;
		return;
	}
	struct lws_context **pcontext_finalize = context->pcontext_finalize;
	struct lws_context_per_thread *pt;
	int n;

	for (n = 0; n < context->count_threads; n++) {
		pt = &context->pt[n];

		if (context->event_loop_ops->destroy_pt)
			context->event_loop_ops->destroy_pt(context, n);

		lws_free_set_NULL(context->pt[n].serv_buf);

#if defined(LWS_ROLE_H1) || defined(LWS_ROLE_H2)
		while (pt->http.ah_list)
			_lws_destroy_ah(pt, pt->http.ah_list);
#endif
	}

	lws_free(context);
	lwsl_info("%s: ctx %p freed\n", __func__, context);

	if (pcontext_finalize)
		*pcontext_finalize = NULL;
}

/*
 * really start destroying things
 */

void
lws_context_destroy2(struct lws_context *context)
{
	if (context && (context->vr.View && context->vr.View->ResourceBinding)) {
		context->vr.Stage = LWS_VR_CONTEXT_RETAINED1;
		context->vr.View->Control->Fault |= VR_LWS_SOURCE_FAULT_PROTOCOL1;
		return;
	}
	struct lws_vhost *vh = NULL, *vh1;
#if defined(LWS_WITH_PEER_LIMITS)
	uint32_t nu;
#endif
	int n;

	lwsl_info("%s: ctx %p\n", __func__, context);

	context->being_destroyed2 = 1;

	if (context->pt[0].fds)
		lws_free_set_NULL(context->pt[0].fds);

	/*
	 * free all the per-vhost allocations
	 */

	vh = context->vhost_list;
	while (vh) {
		vh1 = vh->vhost_next;
		lws_vhost_destroy2(vh);
		vh = vh1;
	}

	/* remove ourselves from the pending destruction list */

	while (context->vhost_pending_destruction_list)
		/* removes itself from list */
		lws_vhost_destroy2(context->vhost_pending_destruction_list);


	lws_stats_log_dump(context);

	lws_ssl_context_destroy(context);
	lws_plat_context_late_destroy(context);

#if defined(LWS_WITH_PEER_LIMITS)
	for (nu = 0; nu < context->pl_hash_elements; nu++)	{
		lws_start_foreach_llp(struct lws_peer **, peer,
				      context->pl_hash_table[nu]) {
			struct lws_peer *df = *peer;
			*peer = df->next;
			lws_free(df);
			continue;
		} lws_end_foreach_llp(peer, next);
	}
	lws_free(context->pl_hash_table);
#endif

	if (context->external_baggage_free_on_destroy)
		free(context->external_baggage_free_on_destroy);

	lws_check_deferred_free(context, 1);

#if LWS_MAX_SMP > 1
	pthread_mutex_destroy(&context->lock);
#endif

	if (context->event_loop_ops->destroy_context2)
		if (context->event_loop_ops->destroy_context2(context)) {
			context->finalize_destroy_after_internal_loops_stopped = 1;
			return;
		}

	if (!context->pt[0].event_loop_foreign)
		for (n = 0; n < context->count_threads; n++)
			if (context->pt[n].inside_service)
				return;

	lws_context_destroy3(context);
}

/*
 * Begin the context takedown
 */

LWS_VISIBLE void
lws_context_destroy(struct lws_context *context)
{
	if (context && (context->vr.View && context->vr.View->ResourceBinding)) {
		context->vr.Stage = LWS_VR_CONTEXT_RETAINED1;
		context->vr.View->Control->Fault |= VR_LWS_SOURCE_FAULT_PROTOCOL1;
		return;
	}
	volatile struct lws_foreign_thread_pollfd *ftp, *next;
	volatile struct lws_context_per_thread *vpt;
	struct lws_context_per_thread *pt;
	struct lws_vhost *vh = NULL;
	struct lws wsi;
	int n, m;

	if (!context)
		return;

	if (context->finalize_destroy_after_internal_loops_stopped) {
		if (context->event_loop_ops->destroy_context2)
			context->event_loop_ops->destroy_context2(context);

		lws_context_destroy3(context);

		return;
	}

	if (context->being_destroyed1) {
		if (!context->being_destroyed2) {
			lws_context_destroy2(context);

			return;
		}
		lwsl_info("%s: ctx %p: already being destroyed\n",
			    __func__, context);

		lws_context_destroy3(context);
		return;
	}

	lwsl_info("%s: ctx %p\n", __func__, context);

	m = context->count_threads;
	context->being_destroyed = 1;
	context->being_destroyed1 = 1;
	context->requested_kill = 1;

	memset(&wsi, 0, sizeof(wsi));
	wsi.context = context;

#ifdef LWS_LATENCY
	if (context->worst_latency_info[0])
		lwsl_notice("Worst latency: %s\n", context->worst_latency_info);
#endif

	while (m--) {
		pt = &context->pt[m];
		vpt = (volatile struct lws_context_per_thread *)pt;

		ftp = vpt->foreign_pfd_list;
		while (ftp) {
			next = ftp->next;
			lws_free((void *)ftp);
			ftp = next;
		}
		vpt->foreign_pfd_list = NULL;

		for (n = 0; (unsigned int)n < context->pt[m].fds_count; n++) {
			struct lws *wsi = wsi_from_fd(context, pt->fds[n].fd);
			if (!wsi)
				continue;

			if (wsi->event_pipe)
				lws_destroy_event_pipe(wsi);
			else
				lws_close_free_wsi(wsi,
					LWS_CLOSE_STATUS_NOSTATUS_CONTEXT_DESTROY,
					"ctx destroy"
					/* no protocol close */);
			n--;
		}
		lws_pt_mutex_destroy(pt);
	}

	/*
	 * inform all the protocols that they are done and will have no more
	 * callbacks.
	 *
	 * We can't free things until after the event loop shuts down.
	 */
	if (context->protocol_init_done)
		vh = context->vhost_list;
	while (vh) {
		struct lws_vhost *vhn = vh->vhost_next;
		lws_vhost_destroy1(vh);
		vh = vhn;
	}

	lws_plat_context_early_destroy(context);

	/*
	 * We face two different needs depending if foreign loop or not.
	 *
	 * 1) If foreign loop, we really want to advance the destroy_context()
	 *    past here, and block only for libuv-style async close completion.
	 *
	 * 2a) If poll, and we exited by ourselves and are calling a final
	 *     destroy_context() outside of any service already, we want to
	 *     advance all the way in one step.
	 *
	 * 2b) If poll, and we are reacting to a SIGINT, service thread(s) may
	 *     be in poll wait or servicing.  We can't advance the
	 *     destroy_context() to the point it's freeing things; we have to
	 *     leave that for the final destroy_context() after the service
	 *     thread(s) are finished calling for service.
	 */

	if (context->event_loop_ops->destroy_context1) {
		context->event_loop_ops->destroy_context1(context);

		return;
	}

	lws_context_destroy2(context);
}


/* Dormant L1 source prefix. No legacy constructor / bulk TLS / implicit vhost
 * is called. Later L2/L3 edges remain explicitly incomplete at their boundary;
 * a returned partial object never means a service-ready context or vhost. */
_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0), "source cancel bookkeeping is lock-free32");
_Static_assert(LWS_MAX_SMP == 1, "tracked profile is single-service-thread");
_Static_assert(sizeof(struct lws_vr_resource1) == 80, "physical continuation stays80 bytes");
_Static_assert(sizeof(struct lws_vr_tls_wsi1) == 208, "owned SSL/BIO continuation inside WSI");
_Static_assert(sizeof(struct lws_vr_tls_vhost1) == 1576, "owned CTX/PEM/store continuations inside VH");
_Static_assert(sizeof(struct lws_vr_data1) == 1608, "fixed actual WSI child/frame/nonce metadata");
_Static_assert(sizeof(struct lws_vr_context1) == 1392, "actual context external rows/cursors");
_Static_assert(sizeof(struct lws_vr_vhost1) == 1752, "actual vhost constructor/callback cursor");
_Static_assert(sizeof(struct lws_vr_wsi_slot1) == 88, "one external WSI self record");
_Static_assert(sizeof(struct lws_vr_ah_slot1) == 168, "context-owned AH object and data");
_Static_assert(sizeof(((struct lws_vr_context1 *)0)->View) == sizeof(const VrLwsCustody1 *), "original view is one pointer");
_Static_assert(sizeof(struct lws_context) <= 65536, "tracked context ceiling");
_Static_assert(sizeof(struct lws_vhost) <= 16384, "tracked vhost ceiling");
_Static_assert(sizeof(struct lws) <= 16384, "tracked WSI ceiling");
_Static_assert(sizeof(struct lws_pollfd) == 8, "three poll rows are24 bytes");
_Static_assert(sizeof(struct lws_buflist) <= 32, "prepared cache node ceiling");
_Static_assert(sizeof(struct lws_protocols) * 2 <= 512, "one protocol+terminator");
_Static_assert(LWS_PRE == 16, "selected RX/TX geometry");

static int
vr_merge1(int a, int b)
{
 if (a == VR_LWS_RELEASE_UNCERTAIN1 || b == VR_LWS_RELEASE_UNCERTAIN1)
  return VR_LWS_RELEASE_UNCERTAIN1;
 if (a == VR_LWS_PROTOCOL_FAULT1 || b == VR_LWS_PROTOCOL_FAULT1)
  return VR_LWS_PROTOCOL_FAULT1;
 if (a == VR_LWS_FAILED_WITH_OUTSTANDING1 || b == VR_LWS_FAILED_WITH_OUTSTANDING1)
  return VR_LWS_FAILED_WITH_OUTSTANDING1;
 return a != VR_LWS_KNOWN_SUCCESS1 ? a : b;
}

static uint32_t
vr_pending1(const struct lws_vr_resource1 *r)
{
 return r->Stage != LWS_VR_EMPTY1 && r->Stage != LWS_VR_RETIRED1;
}

uint32_t
lws_vr_pending_context1(const struct lws_context *c)
{
 uint32_t count = vr_pending1(&c->vr.Self) + vr_pending1(&c->vr.Scratch) +
  vr_pending1(&c->vr.Poll) + vr_pending1(&c->vr.Event) + vr_pending1(&c->vr.Random) +
  vr_pending1(&c->vr.Wake[0]) + vr_pending1(&c->vr.Wake[1]);
 unsigned int n;
 for (n = 0; n < 2; ++n) {
  count += vr_pending1(&c->vr.Vhosts[n].Allocation);
  count += vr_pending1(&c->vr.Headers[n].Allocation) + vr_pending1(&c->vr.Headers[n].Data);
  count += vr_pending1(&c->vr.Data[n].Allocation);
  if (c->vr.Data[n].Object) {
   const struct lws *w = c->vr.Data[n].Object;
   unsigned int j;
   count += vr_pending1(&w->vr_fd) + vr_pending1(&w->vr_data.Role) +
    vr_pending1(&w->vr_data.Rx) + vr_pending1(&w->vr_data.Tx) +
    vr_pending1(&w->vr_data.Stash) + vr_pending1(&w->vr_data.Hostname) +
    vr_pending1(&w->vr_data.Tls.Ssl) + vr_pending1(&w->vr_data.Tls.Bio);
   for (j = 0; j < 2; ++j) count += vr_pending1(&w->vr_data.Cache[j]);
   for (j = 0; j < 8; ++j) count += vr_pending1(&w->vr_data.Strings[j]);
  }
  if (c->vr.Vhosts[n].Object) {
   count += vr_pending1(&c->vr.Vhosts[n].Object->vr.Protocols);
   const struct lws_vr_tls_vhost1 *t = &c->vr.Vhosts[n].Object->vr.Tls;
   unsigned int j;
   count += vr_pending1(&c->vr.Vhosts[n].Object->vr.SameProtocolList) +
    vr_pending1(&t->Ctx) + vr_pending1(&t->MemBio) + vr_pending1(&t->Local);
   for (j = 0; j < 16; ++j) count += vr_pending1(&t->Store[j]);
  }
 }
 return count;
}

static int
vr_resource_uncertain1(const struct lws_vr_resource1 *r)
{
 return r->ObservedNative19 || r->Stage == LWS_VR_UNKNOWN1 ||
  r->Stage == LWS_VR_DISPOSE_UNKNOWN1 || r->Stage == LWS_VR_UNKNOWN_ACK1 ||
  r->Stage == LWS_VR_FREE_ENTERED1;
}

static int
vr_context_uncertain1(const struct lws_context *c)
{
 unsigned int n;
 if (vr_resource_uncertain1(&c->vr.Self) || vr_resource_uncertain1(&c->vr.Scratch) ||
     vr_resource_uncertain1(&c->vr.Poll) || vr_resource_uncertain1(&c->vr.Event) ||
     vr_resource_uncertain1(&c->vr.Random) || vr_resource_uncertain1(&c->vr.Wake[0]) ||
     vr_resource_uncertain1(&c->vr.Wake[1])) return 1;
 for (n = 0; n < 2; ++n) {
  if (vr_resource_uncertain1(&c->vr.Vhosts[n].Allocation)) return 1;
  if (vr_resource_uncertain1(&c->vr.Headers[n].Allocation) ||
      vr_resource_uncertain1(&c->vr.Headers[n].Data) ||
      vr_resource_uncertain1(&c->vr.Data[n].Allocation)) return 1;
  if (c->vr.Data[n].Object) {
   const struct lws *w = c->vr.Data[n].Object;
   unsigned int j;
   if (vr_resource_uncertain1(&w->vr_fd) || vr_resource_uncertain1(&w->vr_data.Role) ||
       vr_resource_uncertain1(&w->vr_data.Rx) || vr_resource_uncertain1(&w->vr_data.Tx) ||
       vr_resource_uncertain1(&w->vr_data.Stash) || vr_resource_uncertain1(&w->vr_data.Hostname) ||
       vr_resource_uncertain1(&w->vr_data.Tls.Ssl) || vr_resource_uncertain1(&w->vr_data.Tls.Bio) ||
       w->vr_data.Tls.Transferred == 1) return 1;
   for (j = 0; j < 2; ++j) if (vr_resource_uncertain1(&w->vr_data.Cache[j])) return 1;
   for (j = 0; j < 8; ++j) if (vr_resource_uncertain1(&w->vr_data.Strings[j])) return 1;
  }
  if (c->vr.Vhosts[n].Object) {
   const struct lws_vr_tls_vhost1 *t = &c->vr.Vhosts[n].Object->vr.Tls;
   unsigned int j;
   if (vr_resource_uncertain1(&c->vr.Vhosts[n].Object->vr.Protocols) ||
       vr_resource_uncertain1(&c->vr.Vhosts[n].Object->vr.SameProtocolList) ||
       vr_resource_uncertain1(&t->Ctx) || vr_resource_uncertain1(&t->MemBio) || vr_resource_uncertain1(&t->Local)) return 1;
   for (j = 0; j < 16; ++j) if (vr_resource_uncertain1(&t->Store[j])) return 1;
  }
 }
 return 0;
}

static int
vr_source_frame1(const VrLwsCustody1 *v, VrLwsPhaseBudget1 *phase, VrLwsResult1 *out)
{
 if (!v || !v->ResourceBinding || !v->Control || !phase || !out ||
     (uintptr_t)out % _Alignof(VrLwsResult1) ||
     (uintptr_t)v->Control % _Alignof(VrLwsSourceControl1) ||
     phase->Reserved || phase->Used > UINT32_MAX - phase->Remaining ||
     v->Control->Reserved0 || v->Control->Reserved1)
  return 0;
 /* These are shape/arithmetic checks on the ACTUAL externally owned budget,
  * not a packet establishing caller role or permission to create resources. */
 if (phase->Phase == VR_LWS_PHASE_STARTUP1)
  return phase->Used + phase->Remaining == VR_LWS_STARTUP_NAMED_CAP2;
 if (phase->Phase == VR_LWS_PHASE_FINAL1)
  return phase->Used + phase->Remaining == VR_LWS_FINAL_NAMED_CAP2;
 return 0;
}

int
lws_vr_event_create1(struct lws_context *c)
{
 struct lws *wsi;
 int status;
 if (!c->vr.Budget || c->count_threads != 1 || c->pt[0].pipe_wsi ||
     c->vr.Event.Stage != LWS_VR_EMPTY1)
  return 1;
 status = lws_vr_allocate1(c->vr.View, &c->vr.Self.Row, 5, 2,
  sizeof(struct lws), c->vr.Budget, &c->vr.Event, LWS_VR_AT_EVENT1);
 wsi = c->vr.Event.Pointer;
 if (!wsi)
  return 1;
 /* Initialize all native descriptor/membership fields BEFORE any failure or
  * pipe attempt. Event allocation custody remains externally in its context. */
 wsi->context = c; wsi->tsi = 0; wsi->event_pipe = 1; wsi->vr_allocation = &c->vr.Event;
 wsi->desc.sockfd = -1; wsi->position_in_fds_table = LWS_NO_FDS_POS;
 lws_vr_resource_init1(&wsi->vr_fd);
 wsi->wsistate = LRS_UNCONNECTED; wsi->role_ops = &role_ops_pipe;
 c->pt[0].pipe_wsi = wsi; ++c->count_wsi_allocated;
 if (status != VR_LWS_KNOWN_SUCCESS1)
  return 1;
 if (lws_plat_pipe_create(wsi))
  return 1;
 wsi->desc.sockfd = c->pt[0].dummy_pipe_fds[0];
 return __insert_wsi_socket_into_fds(c, wsi);
}

int
lws_vr_event_destroy1(struct lws *wsi)
{
 struct lws_context *c = wsi->context;
 int status;
 if (!c->vr.Budget || !c->vr.Budget->Creating || c->vr.Budget->Active ||
     wsi != c->pt[0].pipe_wsi || !wsi->event_pipe || c->count_wsi_allocated != 1)
  return 1;
 if (lws_vr_remove_fd1(wsi))
  return 1;
 if (lws_vr_pipe_close1(wsi))
  return 1;
 /* No callback/timer/list/lookup/member can retain this event object now.
  * No eventlib virtual finalizer is silently treated as a known release. */
 c->pt[0].pipe_wsi = NULL; --c->count_wsi_allocated;
 status = lws_vr_free1(c->vr.View, c->vr.Budget, &c->vr.Event, LWS_VR_AT_EVENT1);
 if (c->vr.Event.Stage == LWS_VR_ACQUIRED1) { c->pt[0].pipe_wsi = wsi; ++c->count_wsi_allocated; }
 /* Event metadata remains in the actual context even when ACK is pending;
  * wsi is never inspected after the actual physical free. */
 return status != VR_LWS_KNOWN_SUCCESS1;
}

static int vr_cleanup_context1(struct lws_context *, const VrLwsCustody1 *,
 const VrLwsBudgetRef1 *, VrLwsResult1 *, int);

__attribute__((used, visibility("hidden")))
int32_t
varest_lws_create_context_tracked1(const struct lws_context_creation_info *info,
 const VrLwsCustody1 *view, VrLwsPhaseBudget1 *phase, VrLwsResult1 *out)
{
 VrLwsBudgetRef1 budget = {NULL, phase};
 struct lws_vr_resource1 self;
 struct lws_context *c;
 unsigned int n;
 int status;
 if (!vr_source_frame1(view, phase, out))
  return VR_LWS_PROTOCOL_FAULT1;
 lws_vr_source_result1(view, out, VR_LWS_PROTOCOL_FAULT1, NULL, LWS_VR_AT_CONTEXT1, 0);
 if (!info || phase->Phase != VR_LWS_PHASE_STARTUP1 ||
     view->Control->CtorEntered || view->Control->RawContext ||
     view->Control->ContextSourceSerial || view->Control->ContextPhase ||
     view->Control->FullCreateReturned || view->Control->FullDestroyReturned ||
     view->Control->CallbackDepth || __atomic_load_n(&view->Control->CancelUsers, __ATOMIC_ACQUIRE) ||
     view->Control->Fault)
  return (int32_t)out->Disposition;
 if (info->port != CONTEXT_PORT_NO_LISTEN || !(info->options & LWS_SERVER_OPTION_EXPLICIT_VHOSTS) ||
     (info->options & (LWS_SERVER_OPTION_LIBUV | LWS_SERVER_OPTION_LIBEV | LWS_SERVER_OPTION_LIBEVENT)) ||
     info->count_threads > 1 || (info->fd_limit_per_thread && info->fd_limit_per_thread != 3) ||
     info->pt_serv_buf_size > 32768 ||
     info->max_http_header_data2 > 32768 || (info->max_http_header_pool && info->max_http_header_pool != 2) ||
     info->fops || info->foreign_loops || info->http_proxy_address || info->http_proxy_port ||
     info->ws_ping_pong_interval ||
     info->external_baggage_free_on_destroy || info->mounts || info->pvo)
  return lws_vr_source_result1(view, out, VR_LWS_DEFERRED_BEFORE_ACQUIRE1, NULL, LWS_VR_AT_CONTEXT1, 0);
 /* One actual constructor episode; not reset by failure, NULL or unwind. */
 view->Control->CtorEntered = 1;
 view->Control->ContextPhase = LWS_VR_CONTEXT_CREATING1;
 lws_vr_resource_init1(&self);
 status = lws_vr_allocate1(view, NULL, 7, 1, sizeof(*c), &budget, &self, LWS_VR_AT_CONTEXT1);
 c = self.Pointer;
 if (!c) {
  view->Control->ContextPhase = vr_pending1(&self) || self.ObservedNative19 ?
   LWS_VR_CONTEXT_RETAINED1 : LWS_VR_CONTEXT_NO_OBJECT1;
  return lws_vr_source_result1(view, out, status, NULL, LWS_VR_AT_CONTEXT1, vr_pending1(&self));
 }
 /* A failed ACK still leaves the ACTUAL allocation in durable custody. */
 c->vr.View = view; c->vr.Budget = &budget; c->vr.Self = self;
 c->vr.Stage = LWS_VR_CONTEXT_CREATING1;
 view->Control->RawContext = (uint64_t)(uintptr_t)c;
 view->Control->ContextSourceSerial = c->vr.Self.Row.SourceSerial;
 lws_vr_resource_init1(&self); /* source-local move into actual context */
 lws_vr_resource_init1(&c->vr.Scratch); lws_vr_resource_init1(&c->vr.Poll);
 lws_vr_resource_init1(&c->vr.Event); lws_vr_resource_init1(&c->vr.Random);
 lws_vr_resource_init1(&c->vr.Wake[0]); lws_vr_resource_init1(&c->vr.Wake[1]);
 for (n = 0; n < 2; ++n) {
  lws_vr_resource_init1(&c->vr.Vhosts[n].Allocation);
  lws_vr_resource_init1(&c->vr.Headers[n].Allocation);
  lws_vr_resource_init1(&c->vr.Headers[n].Data);
  lws_vr_resource_init1(&c->vr.Data[n].Allocation);
 }
 for (n = 0; n < 3; ++n) c->vr.Sparse[n].Fd = -1;
 c->fd_random = -1; c->pt[0].dummy_pipe_fds[0] = c->pt[0].dummy_pipe_fds[1] = -1;
 c->count_threads = 1; c->max_fds = 3; c->fd_limit_per_thread = 3;
 c->pt[0].context = c; c->pt[0].tid = 0;
 c->options = info->options; c->user_space = info->user;
 c->timeout_secs = info->timeout_secs ? info->timeout_secs : AWAITING_TIMEOUT;
 c->pt_serv_buf_size = info->pt_serv_buf_size ? info->pt_serv_buf_size : 32768;
 c->max_http_header_data = info->max_http_header_data ? info->max_http_header_data :
  info->max_http_header_data2 ? info->max_http_header_data2 : 32768;
 c->max_http_header_pool = 2;
 c->event_loop_ops = &event_loop_ops_poll;
#if defined(LWS_WITH_TLS)
 c->tls_ops = &tls_ops_openssl;
#endif
 if (status != VR_LWS_KNOWN_SUCCESS1)
  goto retained;
 status = lws_vr_allocate1(view, &c->vr.Self.Row, 4, 2, c->pt_serv_buf_size,
  &budget, &c->vr.Scratch, LWS_VR_AT_SCRATCH1);
 c->pt[0].serv_buf = c->vr.Scratch.Pointer;
 if (status != VR_LWS_KNOWN_SUCCESS1) goto retained;
 status = lws_vr_allocate1(view, &c->vr.Self.Row, 4, 3, 3 * sizeof(struct lws_pollfd),
  &budget, &c->vr.Poll, LWS_VR_AT_POLL1);
 c->pt[0].fds = c->vr.Poll.Pointer;
 if (status != VR_LWS_KNOWN_SUCCESS1) goto retained;
 for (n = 0; n < 3; ++n) c->pt[0].fds[n].fd = -1;
 status = lws_vr_random_open1(c, &budget);
 if (status != VR_LWS_KNOWN_SUCCESS1) goto retained;
 if (lws_create_event_pipes(c)) { status = VR_LWS_FAILED_WITH_OUTSTANDING1; goto retained; }
 c->vr.Stage = LWS_VR_CONTEXT_CORE_PREFIX1;
 view->Control->ContextPhase = LWS_VR_CONTEXT_CORE_PREFIX1;
 status = lws_vr_ah_prepare1(c, &budget);
 if (status != VR_LWS_KNOWN_SUCCESS1) goto retained;
 /* This genuine source branch has not requested TLS initialization. TLS
  * remains an owned partial prefix until L3 supplies every provider hook. */
 if (info->options & LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT) {
  c->vr.Budget = NULL;
  return lws_vr_source_result1(view, out, VR_LWS_FAILED_WITH_OUTSTANDING1, c,
   LWS_VR_NEEDS_L2_L3_1, lws_vr_pending_context1(c));
 }
 time(&c->time_up); c->last_timeout_check_s = c->time_up;
 c->vr.CoreComplete = 1; c->vr.Budget = NULL;
 return lws_vr_source_result1(view, out, VR_LWS_KNOWN_SUCCESS1, c,
  LWS_VR_AT_CONTEXT1, lws_vr_pending_context1(c));
retained:
 /* SAME remaining startup gas, one real child-first failure unwind. Actual
  * initiating status/observed19 survives; no external FullDestroy flag here. */
 return vr_cleanup_context1(c, view, &budget, out, status);
}


static int
vr_name_bounded1(const char *name)
{
 unsigned int n;
 if (!name) return 1;
 for (n = 0; n < 4096; ++n) if (!name[n]) return 1;
 return 0;
}

__attribute__((used, visibility("hidden")))
static int
vr_local_route_info1(const struct lws_context_creation_info *info)
{
 const char *p = info->http_proxy_address;
 unsigned int i;
 if (!p || info->http_proxy_port < 1 || info->http_proxy_port > 65535) return 0;
 for (i = 0; i < 81; ++i) {
  if (i < 6) { if (p[i] != "relay:"[i]) return 0; }
  else if (i < 70) { if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return 0; }
  else if (p[i] != "@127.0.0.1"[i - 70]) return 0;
 }
 /* Do not enter ambient file/provided-CTX/client-auth paths in this profile. */
 return !info->provided_client_ssl_ctx && !info->ssl_ca_filepath && !info->client_ssl_ca_filepath &&
  !info->ssl_cert_filepath && !info->client_ssl_cert_filepath && !info->ssl_private_key_filepath &&
  !info->client_ssl_private_key_filepath && !info->ssl_private_key_password;
}

int32_t
varest_lws_create_vhost_tracked1(struct lws_context *c,
 const struct lws_context_creation_info *info, const VrLwsCustody1 *view,
 VrLwsPhaseBudget1 *phase, VrLwsResult1 *out)
{
 VrLwsBudgetRef1 budget = {NULL, phase};
 struct lws_vr_vhost_slot1 *slot;
 struct lws_vhost *vh = NULL;
 int status;
 if (!vr_source_frame1(view, phase, out)) return VR_LWS_PROTOCOL_FAULT1;
 lws_vr_source_result1(view, out, VR_LWS_PROTOCOL_FAULT1, NULL, LWS_VR_AT_VHOST1, 0);
 /* Actual source lifetime/role precedes these consistency checks; no tuple
  * authenticates a context/caller or proves an initialized usable vhost. */
 if (!c || !info || phase->Phase != VR_LWS_PHASE_STARTUP1 ||
     c->vr.View != view ||
     view->Control->RawContext != (uint64_t)(uintptr_t)c ||
     view->Control->ContextSourceSerial != c->vr.Self.Row.SourceSerial ||
     c->vr.Stage != LWS_VR_CONTEXT_CORE_PREFIX1 || c->vr.Budget ||
     c->vr.VhostAttempts >= 2 || view->Control->CallbackDepth ||
     __atomic_load_n(&view->Control->CancelUsers, __ATOMIC_ACQUIRE)) return (int32_t)out->Disposition;
 if (view->Control->Fault)
  return lws_vr_source_result1(view, out,
   view->Control->Fault & VR_LWS_SOURCE_FAULT_NATIVE19_OBSERVED1 ? VR_LWS_RELEASE_UNCERTAIN1 : VR_LWS_PROTOCOL_FAULT1,
   c, LWS_VR_AT_VHOST1, lws_vr_pending_context1(c));
 /* One application protocol plus the exact terminator; no server listener,
  * plugin, mount, hidden session allocation or callback installation. */
 if (info->port != CONTEXT_PORT_NO_LISTEN || !info->protocols ||
     !info->protocols[0].callback || info->protocols[1].callback ||
     info->protocols[0].per_session_data_size || info->protocols[0].rx_buffer_size > 65536 ||
     info->protocols[0].tx_packet_size > 65536 || info->ka_time < 0 || info->ka_interval < 0 || info->ka_probes < 0 ||
     (info->ka_time && (!info->ka_interval || !info->ka_probes)) ||
     ((int64_t)info->ka_time + (int64_t)info->ka_interval * info->ka_probes) > INT_MAX / 1000 ||
     info->mounts || info->pvo || info->headers || info->extensions || info->iface || info->bind_iface || !vr_name_bounded1(info->vhost_name) ||
     !vr_name_bounded1(info->iface) || !vr_name_bounded1(info->protocols[0].name))
  return lws_vr_source_result1(view, out, VR_LWS_DEFERRED_BEFORE_ACQUIRE1, c, LWS_VR_AT_VHOST1, lws_vr_pending_context1(c));
 if (!vr_local_route_info1(info))
  return lws_vr_source_result1(view, out, VR_LWS_DEFERRED_BEFORE_ACQUIRE1, c, LWS_VR_AT_VHOST1, lws_vr_pending_context1(c));
#if defined(LWS_WITH_SOCKS5)
 if (info->socks_proxy_address || info->socks_proxy_port)
  return lws_vr_source_result1(view, out, VR_LWS_DEFERRED_BEFORE_ACQUIRE1, c, LWS_VR_AT_VHOST1, lws_vr_pending_context1(c));
#endif
 slot = &c->vr.Vhosts[c->vr.VhostAttempts++]; c->vr.Budget = &budget;
 status = lws_vr_allocate1(view, &c->vr.Self.Row, 6, 1, sizeof(*vh), &budget,
  &slot->Allocation, LWS_VR_AT_VHOST1);
 vh = slot->Allocation.Pointer;
 if (!vh) goto retained;
 slot->Object = vh; vh->context = c; vh->vr.PrefixOnly = 1;
 vh->vhost_next = c->vhost_list; c->vhost_list = vh; /* cleanup responsibility immediately */
 lws_vr_resource_init1(&vh->vr.Protocols); lws_vr_resource_init1(&vh->vr.SameProtocolList);
 lws_vr_resource_init1(&vh->vr.Tls.Ctx); lws_vr_resource_init1(&vh->vr.Tls.MemBio);
 lws_vr_resource_init1(&vh->vr.Tls.Local);
 { unsigned int j; for (j = 0; j < 16; ++j) lws_vr_resource_init1(&vh->vr.Tls.Store[j]); }
 /* Borrowed input ends at this topcall. Copy actual C1 credentials into the
  * existing builtin fields, never lws_set_proxy/environment/logging. */
 memcpy(vh->http.http_proxy_address, "127.0.0.1", 10);
 vh->http.http_proxy_port = info->http_proxy_port;
 if (lws_b64_encode_string(info->http_proxy_address, 70, vh->proxy_basic_auth_token,
     sizeof(vh->proxy_basic_auth_token)) != 96) { status = VR_LWS_PROTOCOL_FAULT1; goto retained; }
 vh->vr.PrefixOnly = 1; vh->context = c;
 vh->name = info->vhost_name ? info->vhost_name : "default";
 vh->iface = info->iface; vh->options = info->options;
 vh->listen_port = info->port; vh->count_protocols = 1; vh->user = info->user;
 vh->ka_time = info->ka_time; vh->ka_interval = info->ka_interval; vh->ka_probes = info->ka_probes;
 vh->keepalive_timeout = info->keepalive_timeout ? info->keepalive_timeout : 5;
 vh->timeout_secs_ah_idle = info->timeout_secs_ah_idle ? info->timeout_secs_ah_idle : 10;
 /* Partial object is linked for its own bounded cleanup only. No role-init,
  * protocol-init, TLS or user callback has executed. */
 if (status != VR_LWS_KNOWN_SUCCESS1) goto retained;
 status = lws_vr_allocate1(view, &slot->Allocation.Row, 4, 4,
  2 * sizeof(struct lws_protocols), &budget, &vh->vr.Protocols, LWS_VR_AT_PROTOCOL_COPY1);
 vh->protocols = vh->vr.Protocols.Pointer;
 if (status != VR_LWS_KNOWN_SUCCESS1) goto retained;
 memcpy((void *)vh->protocols, info->protocols, 2 * sizeof(struct lws_protocols));
 status = lws_vr_allocate1(view, &slot->Allocation.Row, 4, 5,
  sizeof(struct lws *), &budget, &vh->vr.SameProtocolList, LWS_VR_AT_VHOST_LIST1);
 vh->same_vh_protocol_list = vh->vr.SameProtocolList.Pointer;
 if (status != VR_LWS_KNOWN_SUCCESS1) goto retained;
 if (!c->vr.CoreComplete) { status = VR_LWS_FAILED_WITH_OUTSTANDING1; goto retained; }
 /* Genuine initialized provider/GT/Service/manager/code premises remain H.
  * DO_SSL_GLOBAL_INIT is NOT initialization evidence and is never executed. */
 status = lws_vr_tls_vhost_create1(vh, info);
 if (status != VR_LWS_KNOWN_SUCCESS1) goto retained;
 if ((role_ops_h1.init_vhost && role_ops_h1.init_vhost(vh, info)) ||
     (role_ops_ws.init_vhost && role_ops_ws.init_vhost(vh, info))) {
  status = VR_LWS_FAILED_WITH_OUTSTANDING1; goto retained;
 }
 vh->vr.PrepareStep = 1;
 status = lws_vr_data_prepare1(c, (unsigned int)(slot - c->vr.Vhosts), vh, &budget);
 if (status != VR_LWS_KNOWN_SUCCESS1) goto retained;
 {
  struct lws *w = c->vr.Data[(unsigned int)(slot - c->vr.Vhosts)].Object;
  int callback = lws_vr_callback1(w, LWS_CALLBACK_PROTOCOL_INIT, NULL, NULL, 0);
  if (callback != LWS_VR_CALLBACK_DEFERRED1) { vh->vr.ProtocolInit = 1; vh->created_vhost_protocols = 1; }
  if (callback) { status = VR_LWS_FAILED_WITH_OUTSTANDING1; goto retained; }
 }
 vh->vr.PrefixOnly = 0; vh->vr.PrepareStep = 2; c->protocol_init_done = 1;
 c->vr.Budget = NULL;
 return lws_vr_source_result1(view, out, VR_LWS_KNOWN_SUCCESS1, vh,
  LWS_VR_AT_VHOST1, lws_vr_pending_context1(c));
retained:
 if (vr_context_uncertain1(c)) status = VR_LWS_RELEASE_UNCERTAIN1;
 c->vr.Stage = LWS_VR_CONTEXT_RETAINED1;
 view->Control->ContextPhase = LWS_VR_CONTEXT_RETAINED1; c->vr.Budget = NULL;
 return lws_vr_source_result1(view, out, status, vh, LWS_VR_NEEDS_L2_L3_1, lws_vr_pending_context1(c));
}

static int
vr_vhost_cleanup1(struct lws_context *c, struct lws_vr_vhost_slot1 *slot,
 const VrLwsBudgetRef1 *budget)
{
 struct lws_vhost *vh = slot->Object, **link;
 unsigned int n;
 int status = VR_LWS_KNOWN_SUCCESS1, next;
 if (!vh) return lws_vr_free1(c->vr.View, budget, &slot->Allocation, LWS_VR_AT_VHOST1);
 /* The genuine source interval also excludes every external consumer; a
  * prefix cursor or null pointer by itself proves no such lifetime fact. */
 if (vh->lserv_wsi || vh->protocol_vh_privs || vh->timed_vh_protocol_list || vh->context != c)
  return VR_LWS_FAILED_WITH_OUTSTANDING1;
 if (c->vr.Data[(unsigned int)(slot - c->vr.Vhosts)].Object &&
     c->vr.Data[(unsigned int)(slot - c->vr.Vhosts)].Object->vr_data.Used) {
  struct lws *active = c->vr.Data[(unsigned int)(slot - c->vr.Vhosts)].Object;
  lws_vr_request_close1(active, LWS_CLOSE_STATUS_NOSTATUS_CONTEXT_DESTROY);
  next = lws_vr_close_progress1(active);
  if (next != VR_LWS_KNOWN_SUCCESS1) return next;
 }
 next = lws_vr_tls_vhost_cleanup1(vh, budget);
 if (next != VR_LWS_KNOWN_SUCCESS1) return next;
 if (vh->vr.ProtocolInit && !vh->vr.ProtocolDestroy) {
  struct lws *notification = c->vr.Data[(unsigned int)(slot - c->vr.Vhosts)].Object;
  int callback;
  if (!notification) return VR_LWS_PROTOCOL_FAULT1;
  callback = lws_vr_callback1(notification, LWS_CALLBACK_PROTOCOL_DESTROY, NULL, NULL, 0);
  if (callback == LWS_VR_CALLBACK_DEFERRED1) return VR_LWS_FAILED_WITH_OUTSTANDING1;
  vh->vr.ProtocolDestroy = 1; vh->created_vhost_protocols = 0;
 }
 next = lws_vr_data_cleanup1(c, (unsigned int)(slot - c->vr.Vhosts), budget);
 if (next != VR_LWS_KNOWN_SUCCESS1) return next;
 vh->same_vh_protocol_list = NULL;
 next = lws_vr_free1(c->vr.View, budget, &vh->vr.SameProtocolList, LWS_VR_AT_VHOST_LIST1);
 status = vr_merge1(status, next);
 vh->protocols = NULL;
 next = lws_vr_free1(c->vr.View, budget, &vh->vr.Protocols, LWS_VR_AT_PROTOCOL_COPY1);
 status = vr_merge1(status, next);
 if (vr_pending1(&vh->vr.SameProtocolList) || vr_pending1(&vh->vr.Protocols))
  return status == VR_LWS_KNOWN_SUCCESS1 ? VR_LWS_FAILED_WITH_OUTSTANDING1 : status;
 link = &c->vhost_list;
 for (n = 0; n < 2 && *link && *link != vh; ++n) link = &(*link)->vhost_next;
 if (*link != vh) return VR_LWS_PROTOCOL_FAULT1;
 *link = vh->vhost_next;
 memset(vh->proxy_basic_auth_token, 0, sizeof(vh->proxy_basic_auth_token));
 slot->Object = NULL;
 /* Slot metadata is EXTERNAL to vh. No vh access after free/ACK progress. */
 return vr_merge1(status, lws_vr_free1(c->vr.View, budget, &slot->Allocation, LWS_VR_AT_VHOST1));
}

/* Finite real child-first cleanup, including constructor-failure continuations.
 * ACK BUSY/refusal retains the SAME row; no actual free/close/ctor is retried. */
static int
vr_cleanup_context1(struct lws_context *c, const VrLwsCustody1 *view,
 const VrLwsBudgetRef1 *budget, VrLwsResult1 *out, int original)
{
 struct lws_vr_resource1 self;
 const VrLwsCustody1 *stable_view;
 uint32_t pending;
 int status = original, next;
 unsigned int n;
 c->vr.Budget = budget; c->vr.Stage = LWS_VR_CONTEXT_DESTROYING1;
 view->Control->ContextPhase = LWS_VR_CONTEXT_DESTROYING1;
 if (view->Control->CallbackDepth || __atomic_load_n(&view->Control->CancelUsers, __ATOMIC_ACQUIRE)) goto retained;
 for (n = 2; n != 0; --n) {
  next = vr_vhost_cleanup1(c, &c->vr.Vhosts[n - 1], budget);
  status = vr_merge1(status, next);
 }
 for (n = 0; n < 2; ++n)
  if (c->vr.Vhosts[n].Object || vr_pending1(&c->vr.Vhosts[n].Allocation) ||
      c->vr.Data[n].Object || vr_pending1(&c->vr.Data[n].Allocation)) goto retained;
 next = lws_vr_ah_cleanup1(c, budget); status = vr_merge1(status, next);
 if (c->pt[0].http.ah_count_in_use || c->pt[0].http.ah_list || c->pt[0].http.ah_pool_length)
  goto retained;
 if (c->pt[0].pipe_wsi) {
  if (lws_vr_event_destroy1(c->pt[0].pipe_wsi)) status = vr_merge1(status, VR_LWS_FAILED_WITH_OUTSTANDING1);
 } else {
  next = lws_vr_free1(view, budget, &c->vr.Event, LWS_VR_AT_EVENT1);
  status = vr_merge1(status, next);
 }
 next = lws_vr_close1(view, budget, &c->vr.Random, LWS_VR_AT_RANDOM1);
 status = vr_merge1(status, next);
 if (!vr_pending1(&c->vr.Random)) c->fd_random = -1;
 if (!c->pt[0].fds_count && !c->pt[0].count_conns && !c->pt[0].pipe_wsi) {
  for (n = 0; n < 3; ++n) if (c->vr.Sparse[n].Wsi || c->vr.Sparse[n].Fd != -1) goto retained;
  c->pt[0].fds = NULL;
  next = lws_vr_free1(view, budget, &c->vr.Poll, LWS_VR_AT_POLL1);
  status = vr_merge1(status, next);
 }
 c->pt[0].serv_buf = NULL;
 next = lws_vr_free1(view, budget, &c->vr.Scratch, LWS_VR_AT_SCRATCH1);
 status = vr_merge1(status, next);
 pending = lws_vr_pending_context1(c);
 if (pending != vr_pending1(&c->vr.Self) || c->vhost_list || c->pt[0].pipe_wsi ||
     c->count_wsi_allocated || c->pt[0].fds_count || c->pt[0].count_conns) goto retained;
 /* Prepare all external addresses/scalars BEFORE physical context free. */
 self = c->vr.Self; stable_view = view; c->vr.Budget = NULL;
 next = lws_vr_free1(stable_view, budget, &self, LWS_VR_AT_CONTEXT1);
 status = vr_merge1(status, next);
 if (self.Stage == LWS_VR_FREE_RETURNED1 || self.Stage == LWS_VR_RETIRED1) {
  /* No c read/helper receiving c follows a physically-freed branch. */
  stable_view->Control->RawContext = 0;
  if (!vr_pending1(&self) && status == VR_LWS_FAILED_WITH_OUTSTANDING1)
   status = VR_LWS_NO_RESOURCE_FAILURE1; /* source failure, now fully unwound */
  stable_view->Control->ContextPhase = vr_pending1(&self) ? LWS_VR_CONTEXT_RETAINED1 : LWS_VR_CONTEXT_NO_OBJECT1;
  return lws_vr_source_result1(stable_view, out, status, NULL, LWS_VR_AT_CONTEXT1, vr_pending1(&self));
 }
 c->vr.Self = self; /* actual no-free continuation only, including observed19 */
retained:
 if (vr_context_uncertain1(c)) status = VR_LWS_RELEASE_UNCERTAIN1;
 c->vr.Stage = LWS_VR_CONTEXT_RETAINED1;
 view->Control->ContextPhase = LWS_VR_CONTEXT_RETAINED1; c->vr.Budget = NULL;
 if (status == VR_LWS_KNOWN_SUCCESS1) status = VR_LWS_FAILED_WITH_OUTSTANDING1;
 return lws_vr_source_result1(view, out, status, c, LWS_VR_AT_CONTEXT1, lws_vr_pending_context1(c));
}

__attribute__((used, visibility("hidden")))
int32_t
varest_lws_destroy_context_tracked1(struct lws_context *c, const VrLwsCustody1 *view,
 VrLwsPhaseBudget1 *phase, VrLwsResult1 *out)
{
 VrLwsBudgetRef1 budget = {NULL, phase};
 if (!vr_source_frame1(view, phase, out)) return VR_LWS_PROTOCOL_FAULT1;
 /* NULL is not a cleanup certificate or a request to inspect an opaque journal. */
 if (!c) return lws_vr_source_result1(view, out, VR_LWS_PROTOCOL_FAULT1, NULL, LWS_VR_AT_CONTEXT1, 0);
 if (c->vr.View != view ||
     view->Control->RawContext != (uint64_t)(uintptr_t)c ||
     view->Control->ContextSourceSerial != c->vr.Self.Row.SourceSerial || c->vr.Budget ||
     c->vr.Stage < LWS_VR_CONTEXT_CREATING1 || c->vr.Stage > LWS_VR_CONTEXT_RETAINED1)
  return lws_vr_source_result1(view, out, VR_LWS_PROTOCOL_FAULT1, c, LWS_VR_AT_CONTEXT1, 0);
 return vr_cleanup_context1(c, view, &budget, out, VR_LWS_KNOWN_SUCCESS1);
}

_Static_assert(sizeof(struct _lws_websocket_related) <= 4096, "prepared WS role ceiling");
_Static_assert(sizeof(struct client_info_stash) <= 256, "prepared stash ceiling");

static struct lws_vr_resource1 *
vr_data_child1(struct lws *w, unsigned int i, uint32_t *sub, uint64_t *bytes)
{
 switch (i) {
 case 0: *sub = 6; *bytes = sizeof(*w->ws); return &w->vr_data.Role;
 case 1: *sub = 7; *bytes = 65556; return &w->vr_data.Rx;
 case 2: case 3: *sub = 12; *bytes = 32768 + sizeof(struct lws_buflist); return &w->vr_data.Cache[i - 2];
 case 4: *sub = 11; *bytes = 65552; return &w->vr_data.Tx;
 case 5: *sub = 8; *bytes = sizeof(struct client_info_stash); return &w->vr_data.Stash;
 case 14: *sub = 10; *bytes = 4096; return &w->vr_data.Hostname;
 default: *sub = 9; *bytes = 4096; return &w->vr_data.Strings[i - 6];
 }
}

int
lws_vr_data_prepare1(struct lws_context *c, unsigned int index,
 struct lws_vhost *vh, const VrLwsBudgetRef1 *budget)
{
 struct lws_vr_wsi_slot1 *slot;
 struct lws *w;
 uint32_t sub;
 uint64_t bytes;
 unsigned int i;
 int status;
 if (index >= 2 || c->vr.Vhosts[index].Object != vh) return VR_LWS_PROTOCOL_FAULT1;
 slot = &c->vr.Data[index];
 status = lws_vr_allocate1(c->vr.View, &c->vr.Vhosts[index].Allocation.Row,
  5, 1, sizeof(*w), budget, &slot->Allocation, LWS_VR_AT_DATA_WSI1);
 w = slot->Allocation.Pointer;
 if (w && !slot->Object) {
  slot->Object = w; w->context = c; w->vhost = vh;
  w->vr_allocation = &slot->Allocation;
  w->protocol = &vh->protocols[0]; w->desc.sockfd = -1;
  w->position_in_fds_table = LWS_NO_FDS_POS;
  lws_vr_resource_init1(&w->vr_fd);
  lws_vr_resource_init1(&w->vr_data.Tls.Ssl); lws_vr_resource_init1(&w->vr_data.Tls.Bio);
  lws_vr_resource_init1(&w->vr_data.Role); lws_vr_resource_init1(&w->vr_data.Rx);
  lws_vr_resource_init1(&w->vr_data.Tx); lws_vr_resource_init1(&w->vr_data.Stash);
  lws_vr_resource_init1(&w->vr_data.Hostname);
  for (i = 0; i < 2; ++i) lws_vr_resource_init1(&w->vr_data.Cache[i]);
  for (i = 0; i < 8; ++i) lws_vr_resource_init1(&w->vr_data.Strings[i]);
  w->vr_data.ReadSlot = 2;
  lws_role_transition(w, LWSIFR_CLIENT, LRS_UNCONNECTED, &role_ops_h1);
  ++c->count_wsi_allocated;
 }
 if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 for (i = 0; i < 15; ++i) {
  struct lws_vr_resource1 *r = vr_data_child1(w, i, &sub, &bytes);
  status = lws_vr_allocate1(c->vr.View, &slot->Allocation.Row, 4, sub,
   bytes, budget, r, LWS_VR_AT_ROLE1 + (i == 1));
  if (i == 0) w->ws = r->Pointer;
  if (i == 1 && w->ws) { w->ws->rx_ubuf = r->Pointer; w->ws->rx_ubuf_alloc = 65552; }
  if (i == 4) { w->trunc_alloc = r->Pointer; w->trunc_alloc_len = 65552; }
  if (i == 5) w->stash = r->Pointer;
  if (i == 14) w->client_hostname_copy = r->Pointer;
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  w->vr_data.PrepareStep = i + 1;
 }
 return VR_LWS_KNOWN_SUCCESS1;
}

int
lws_vr_request_close1(struct lws *w, enum lws_close_status reason)
{
 if (!lws_vr_data_valid1(w) || w->event_pipe) return VR_LWS_PROTOCOL_FAULT1;
 if (!w->vr_data.Used) return VR_LWS_KNOWN_SUCCESS1; /* no logical generation, NOT physical free */
 if (!w->vr_data.CloseStep) {
  w->wsistate_pre_close = w->wsistate;
  w->vr_data.CloseReason = (int32_t)reason;
  w->vr_data.CloseStep = 1;
 }
 if (reason == LWS_CLOSE_STATUS_NOSTATUS_CONTEXT_DESTROY || w->socket_is_permanently_unusable ||
     (w->vr_data.CloseAfterTx && !w->vr_data.FramePending && !w->trunc_len) ||
     (reason == LWS_CLOSE_STATUS_NOSTATUS &&
      !(lwsi_role_ws(w) && w->ws &&
        (w->ws->close_in_ping_buffer_len || w->vr_data.CloseStep == 2))))
  w->vr_data.ForceClose = 1; /* NOSTATUS alone does not erase a real prepared close. */
 return VR_LWS_FAILED_WITH_OUTSTANDING1;
}

int
lws_vr_close_progress1(struct lws *w)
{
 struct lws_context *c = w->context;
 int callback, status;
 unsigned int i;
 if (!lws_vr_data_valid1(w) || w->event_pipe || !c->vr.Budget) return VR_LWS_PROTOCOL_FAULT1;
 if (!w->vr_data.CloseStep) return VR_LWS_KNOWN_SUCCESS1;
 if (c->vr.Dispatching || c->vr.View->Control->CallbackDepth)
  return VR_LWS_FAILED_WITH_OUTSTANDING1;
 if (w->vr_data.CloseStep == 1) {
  /* Finish the already accepted FIFO prefix before queuing one close. */
  if (!w->vr_data.ForceClose && !c->vr.Budget->Creating &&
      (w->vr_data.FramePending || w->trunc_len))
   return VR_LWS_FAILED_WITH_OUTSTANDING1;
  if (!w->vr_data.ForceClose && !c->vr.Budget->Creating && lwsi_role_ws(w) &&
      lwsi_state_est_PRE_CLOSE(w) && w->role_ops->close_via_role_protocol) {
   if (w->role_ops->close_via_role_protocol(w, (enum lws_close_status)w->vr_data.CloseReason)) {
    w->vr_data.CloseStep = 2;
    return VR_LWS_FAILED_WITH_OUTSTANDING1;
   }
  }
  w->vr_data.CloseStep = 3;
 }
 if (w->vr_data.CloseStep == 2) {
  if (!w->vr_data.ForceClose && !c->vr.Budget->Creating && !w->socket_is_permanently_unusable &&
      (!w->ws || !w->ws->peer_has_sent_close || w->vr_data.FramePending || w->trunc_len))
   return VR_LWS_FAILED_WITH_OUTSTANDING1; /* genuine peer/timeout wait */
  w->vr_data.CloseStep = 3;
 }
 if (w->vr_data.CloseStep == 3) {
  if (!lwsi_state_est_PRE_CLOSE(w) && !w->already_did_cce) {
   callback = lws_vr_callback1(w, LWS_CALLBACK_CLIENT_CONNECTION_ERROR, w->user_space, NULL, 0);
   if (callback == LWS_VR_CALLBACK_DEFERRED1) return VR_LWS_FAILED_WITH_OUTSTANDING1;
   w->already_did_cce = 1;
  }
  if (w->vr_data.Created && lwsi_state_est_PRE_CLOSE(w) && !w->told_user_closed) {
   callback = lws_vr_callback1(w, LWS_CALLBACK_CLIENT_CLOSED, w->user_space, NULL, 0);
   if (callback == LWS_VR_CALLBACK_DEFERRED1) return VR_LWS_FAILED_WITH_OUTSTANDING1;
   w->told_user_closed = 1;
  }
  w->vr_data.CloseStep = 4;
 }
 if (w->vr_data.CloseStep == 4) {
  if (w->parent || w->child_list || w->sibling_list || w->udp ||
      w->dll_client_transaction_queue_head.next) return VR_LWS_PROTOCOL_FAULT1;
#if defined(LWS_WITH_PEER_LIMITS)
  if (w->peer) return VR_LWS_PROTOCOL_FAULT1;
#endif
#if defined(LWS_WITH_TLS)
  status = lws_vr_tls_wsi_cleanup1(w, c->vr.Budget);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status; /* BEFORE fd close/destroy callback */
#endif
  if (w->http.ah && lws_header_table_detach(w, 0)) return VR_LWS_PROTOCOL_FAULT1;
  __lws_remove_from_timeout_list(w);
  lws_dll_lws_remove(&w->dll_hrtimer);
  lws_dll_lws_remove(&w->dll_buflist);
  lws_dll_lws_remove(&w->dll_active_client_conns);
  lws_dll_lws_remove(&w->dll_client_transaction_queue);
  lws_same_vh_protocol_remove(w);
  lws_vr_buf_destroy1(w, &w->buflist);
  status = lws_vr_client_fd_dispose1(w, c->vr.Budget);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  if (w->vr_fd.Stage != LWS_VR_EMPTY1 && w->vr_fd.Stage != LWS_VR_RETIRED1)
   return VR_LWS_FAILED_WITH_OUTSTANDING1;
  w->vr_data.CloseStep = 5;
 }
 if (w->vr_data.CloseStep == 5) {
  /* The actual fd/SSL/BIO and their ACKs have finished. Force-close aborts
   * owned unsent bytes; it never reports them sent. No provider/yield follows
   * the complete destroy callback: only prevalidated POD reset to Used0. */
  w->trunc_len = w->trunc_offset = 0; w->could_have_pending = 0;
  w->vr_data.FramePending = w->vr_data.FrameBytes = w->vr_data.FrameWp = 0;
  if (!w->vr_data.Destroyed) {
   callback = lws_vr_callback1(w, LWS_CALLBACK_WSI_DESTROY, w->user_space, NULL, 0);
   if (callback == LWS_VR_CALLBACK_DEFERRED1) return VR_LWS_FAILED_WITH_OUTSTANDING1;
   w->vr_data.Destroyed = 1;
  }
  /* Genuine callback/Conn/poll/AH/cache/timeout consumers of OLD generation
   * have retired. Same physical object, memory and original tickets persist. */
  {
   struct lws_vhost *vhost = w->vhost;
   /* Every ordinary-tail pointer/list/parser belongs to the retired logical
    * generation. Preserve ONLY the physical-prefix records and actual owner
    * context/vhost/protocol/prepared-block associations. No ticket is reset. */
   memset((unsigned char *)w + offsetof(struct lws, http), 0,
    sizeof(*w) - offsetof(struct lws, http));
   w->context = c; w->vhost = vhost; w->protocol = &vhost->protocols[0];
   w->ws = w->vr_data.Role.Pointer;
   w->desc.sockfd = -1; w->position_in_fds_table = LWS_NO_FDS_POS;
  }
  w->client_hostname_copy = w->vr_data.Hostname.Pointer;
  for (i = 0; i < 8; ++i) ((char *)w->vr_data.Strings[i].Pointer)[0] = 0;
  ((char *)w->vr_data.Hostname.Pointer)[0] = 0;
  memset(w->vr_data.Stash.Pointer, 0, sizeof(struct client_info_stash));
  memset(w->ws, 0, sizeof(*w->ws));
  w->ws->rx_ubuf = w->vr_data.Rx.Pointer;
  w->ws->rx_ubuf_alloc = (w->protocol->rx_buffer_size ? w->protocol->rx_buffer_size : c->pt_serv_buf_size) + LWS_PRE;
  w->trunc_alloc = w->vr_data.Tx.Pointer; w->trunc_alloc_len = 65552;
  w->trunc_len = w->trunc_offset = 0; w->could_have_pending = 0;
  w->rxflow_bitmap = 0; w->rxflow_change_to = LWS_RXFLOW_ALLOW;
  w->hdr_parsing_completed = 0; w->pending_timeout = NO_PENDING_TIMEOUT;
  w->already_did_cce = w->told_user_closed = 0;
  w->socket_is_permanently_unusable = 0;
#if defined(LWS_WITH_TLS)
  w->tls.use_ssl = 0;
#endif
  if (w->vr_data.ResetPending == 1) {
   const unsigned char *saved = w->vr_data.Rx.Pointer;
   memcpy(w->vr_data.Strings[0].Pointer, saved, w->vr_data.ResetAddressBytes);
   saved += w->vr_data.ResetAddressBytes;
   memcpy(w->vr_data.Strings[1].Pointer, saved, w->vr_data.ResetPathBytes);
   saved += w->vr_data.ResetPathBytes;
   memcpy(w->vr_data.Strings[2].Pointer, saved, w->vr_data.ResetHostBytes);
   w->c_port = (int)w->vr_data.ResetPort;
   w->vr_data.ResetPending = 2; /* target retained; NEVER authorizes another fd */
  }
  w->wsistate = LWSIFR_CLIENT | LRS_UNCONNECTED; w->role_ops = &role_ops_h1;
  w->vr_data.Used = 0; w->vr_data.ConnectStep = w->vr_data.OptionStep = 0;
  w->vr_data.CloseStep = w->vr_data.ForceClose = 0;
  w->vr_data.Created = w->vr_data.Destroyed = w->vr_data.FdDetached = 0;
  w->vr_data.FramePending = w->vr_data.FrameBytes = w->vr_data.FrameWp = 0;
  w->vr_data.HandshakeStep = w->vr_data.HandshakeBytes = w->vr_data.HandshakeYield = 0;
  w->vr_data.CloseAfterTx = 0;
  memset(w->vr_data.HandshakeNonce, 0, sizeof(w->vr_data.HandshakeNonce));
  memset(&w->vr_data.Tls, 0, sizeof(w->vr_data.Tls));
  w->vr_data.Tls.Ssl.Fd = w->vr_data.Tls.Bio.Fd = -1;
  w->vr_data.RouteStep = w->vr_data.RouteBytes = w->vr_data.RouteSent = w->vr_data.RouteReceived = 0;
  memset(w->vr_data.RouteReply, 0, sizeof(w->vr_data.RouteReply));
 }
 return VR_LWS_KNOWN_SUCCESS1;
}

int
lws_vr_data_cleanup1(struct lws_context *c, unsigned int index, const VrLwsBudgetRef1 *budget)
{
 struct lws_vr_wsi_slot1 *slot;
 struct lws *w;
 unsigned int i;
 uint32_t sub;
 uint64_t bytes;
 int status;
 if (index >= 2 || !budget->Creating || c->vr.Dispatching || c->vr.View->Control->CallbackDepth)
  return VR_LWS_PROTOCOL_FAULT1;
 slot = &c->vr.Data[index]; w = slot->Object;
 if (!w) return lws_vr_free1(c->vr.View, budget, &slot->Allocation, LWS_VR_AT_DATA_WSI1);
 if (w->vr_data.Used) {
  lws_vr_request_close1(w, LWS_CLOSE_STATUS_NOSTATUS_CONTEXT_DESTROY);
  status = lws_vr_close_progress1(w);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 }
 if (w->http.ah && lws_header_table_detach(w, 0)) return VR_LWS_PROTOCOL_FAULT1;
 if (w->position_in_fds_table != LWS_NO_FDS_POS) return VR_LWS_PROTOCOL_FAULT1;
 status = lws_vr_client_fd_dispose1(w, budget);
 if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 if (w->vr_data.Stash.Stage == LWS_VR_ACQUIRED1)
  memset(w->vr_data.Stash.Pointer, 0, sizeof(struct client_info_stash));
 w->stash = NULL; w->client_hostname_copy = NULL; w->buflist = NULL;
 if (w->vr_data.Role.Stage == LWS_VR_ACQUIRED1) {
  struct _lws_websocket_related *role = w->vr_data.Role.Pointer;
  role->rx_ubuf = NULL;
 }
 w->trunc_alloc = NULL; w->trunc_len = w->trunc_offset = 0;
 /* Reverse preparation order; metadata is outside each actual child. */
 for (i = 15; i != 0; --i) {
  struct lws_vr_resource1 *r = vr_data_child1(w, i - 1, &sub, &bytes);
  if (i == 1) w->ws = NULL;
  status = lws_vr_free1(c->vr.View, budget, r, LWS_VR_AT_DATA_WSI1);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 }
 slot->Object = NULL;
 status = lws_vr_free1(c->vr.View, budget, &slot->Allocation, LWS_VR_AT_DATA_WSI1);
 if (slot->Allocation.Stage == LWS_VR_FREE_RETURNED1 || slot->Allocation.Stage == LWS_VR_RETIRED1)
  --c->count_wsi_allocated; /* EXTERNAL context/slot only after physical WSI free */
 else if (slot->Allocation.Stage == LWS_VR_ACQUIRED1)
  slot->Object = w; /* exact no-free continuation, no access to freed WSI */
 return status;
}
