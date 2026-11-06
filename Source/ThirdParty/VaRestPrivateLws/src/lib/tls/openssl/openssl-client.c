/*
 * libwebsockets - openSSL-specific client tls code
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

extern int openssl_websocket_private_data_index,
    openssl_SSL_CTX_private_data_index;

#if !defined(USE_WOLFSSL)

static int
OpenSSL_client_verify_callback(int preverify_ok, X509_STORE_CTX *x509_ctx)
{
	SSL *ssl;
	int n;
	struct lws *wsi;

	/* keep old behaviour accepting self-signed server certs */
	if (!preverify_ok) {
		int err = X509_STORE_CTX_get_error(x509_ctx);

		if (err != X509_V_OK) {
			ssl = X509_STORE_CTX_get_ex_data(x509_ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
			wsi = SSL_get_ex_data(ssl, openssl_websocket_private_data_index);

			if ((err == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT ||
					err == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN) &&
					wsi->tls.use_ssl & LCCSCF_ALLOW_SELFSIGNED) {
				lwsl_notice("accepting self-signed certificate (verify_callback)\n");
				X509_STORE_CTX_set_error(x509_ctx, X509_V_OK);
				return 1;	// ok
			} else if ((err == X509_V_ERR_CERT_NOT_YET_VALID ||
					err == X509_V_ERR_CERT_HAS_EXPIRED) &&
					wsi->tls.use_ssl & LCCSCF_ALLOW_EXPIRED) {
				if (err == X509_V_ERR_CERT_NOT_YET_VALID)
					lwsl_notice("accepting not yet valid certificate (verify_callback)\n");
				else if (err == X509_V_ERR_CERT_HAS_EXPIRED)
					lwsl_notice("accepting expired certificate (verify_callback)\n");
				X509_STORE_CTX_set_error(x509_ctx, X509_V_OK);
				return 1;	// ok
			}
		}
	}

	ssl = X509_STORE_CTX_get_ex_data(x509_ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
	wsi = SSL_get_ex_data(ssl, openssl_websocket_private_data_index);

	n = lws_get_context_protocol(wsi->context, 0).callback(wsi,
			LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION,
			x509_ctx, ssl, preverify_ok);

	/* keep old behaviour if something wrong with server certs */
	/* if ssl error is overruled in callback and cert is ok,
	 * X509_STORE_CTX_set_error(x509_ctx, X509_V_OK); must be set and
	 * return value is 0 from callback */
	if (!preverify_ok) {
		int err = X509_STORE_CTX_get_error(x509_ctx);

		if (err != X509_V_OK) {	/* cert validation error was not handled in callback */
			int depth = X509_STORE_CTX_get_error_depth(x509_ctx);
			const char* msg = X509_verify_cert_error_string(err);
			lwsl_err("SSL error: %s (preverify_ok=%d;err=%d;depth=%d)\n", msg, preverify_ok, err, depth);
			return preverify_ok;	// not ok
		}
	}
	/* convert callback return code from 0 = OK to verify callback return value 1 = OK */
	return !n;
}
#endif


int
lws_ssl_client_bio_create(struct lws *wsi)
{
 if (lws_vr_tracked1(wsi->context)) {
  int status = lws_vr_tls_wsi_create1(wsi);
  wsi->context->vr.LastDisposition = status;
  return status == VR_LWS_KNOWN_SUCCESS1 ? 0 : status == VR_LWS_NO_RESOURCE_FAILURE1 || status == VR_LWS_PROTOCOL_FAULT1 ? -1 : 1;
 }
#if defined LWS_HAVE_X509_VERIFY_PARAM_set1_host
	X509_VERIFY_PARAM *param;
#endif
	char hostname[128], *p;
#if defined(LWS_HAVE_SSL_set_alpn_protos) && \
    defined(LWS_HAVE_SSL_get0_alpn_selected)
	uint8_t openssl_alpn[40];
	const char *alpn_comma = wsi->context->tls.alpn_default;
	int n;
#endif

#if defined(LWS_ROLE_H1) || defined(LWS_ROLE_H2)
	if (lws_hdr_copy(wsi, hostname, sizeof(hostname),
			 _WSI_TOKEN_CLIENT_HOST) <= 0)
#endif
	{
		lwsl_err("%s: Unable to get hostname\n", __func__);

		return -1;
	}

	/*
	 * remove any :port part on the hostname... necessary for network
	 * connection but typical certificates do not contain it
	 */
	p = hostname;
	while (*p) {
		if (*p == ':') {
			*p = '\0';
			break;
		}
		p++;
	}

	wsi->tls.ssl = SSL_new(wsi->vhost->tls.ssl_client_ctx);
	if (!wsi->tls.ssl) {
		lwsl_err("SSL_new failed: %s\n",
		         ERR_error_string(lws_ssl_get_error(wsi, 0), NULL));
		lws_ssl_elaborate_error();
		return -1;
	}

#if defined (LWS_HAVE_SSL_SET_INFO_CALLBACK)
	if (wsi->vhost->tls.ssl_info_event_mask)
		SSL_set_info_callback(wsi->tls.ssl, lws_ssl_info_callback);
#endif

#if defined LWS_HAVE_X509_VERIFY_PARAM_set1_host
	if (!(wsi->tls.use_ssl & LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK)) {
		param = SSL_get0_param(wsi->tls.ssl);
		/* Enable automatic hostname checks */
		X509_VERIFY_PARAM_set_hostflags(param,
						X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
		X509_VERIFY_PARAM_set1_host(param, hostname, 0);
	}
#endif

#if !defined(USE_WOLFSSL)
#ifndef USE_OLD_CYASSL
	/* OpenSSL_client_verify_callback will be called @ SSL_connect() */
	SSL_set_verify(wsi->tls.ssl, SSL_VERIFY_PEER, OpenSSL_client_verify_callback);
#endif
#endif

#if !defined(USE_WOLFSSL)
	SSL_set_mode(wsi->tls.ssl,  SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#endif
	/*
	 * use server name indication (SNI), if supported,
	 * when establishing connection
	 */
#ifdef USE_WOLFSSL
#ifdef USE_OLD_CYASSL
#ifdef CYASSL_SNI_HOST_NAME
	CyaSSL_UseSNI(wsi->tls.ssl, CYASSL_SNI_HOST_NAME, hostname, strlen(hostname));
#endif
#else
#ifdef WOLFSSL_SNI_HOST_NAME
	wolfSSL_UseSNI(wsi->tls.ssl, WOLFSSL_SNI_HOST_NAME, hostname, strlen(hostname));
#endif
#endif
#else
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
	SSL_set_tlsext_host_name(wsi->tls.ssl, hostname);
#endif
#endif

#ifdef USE_WOLFSSL
	/*
	 * wolfSSL/CyaSSL does certificate verification differently
	 * from OpenSSL.
	 * If we should ignore the certificate, we need to set
	 * this before SSL_new and SSL_connect is called.
	 * Otherwise the connect will simply fail with error code -155
	 */
#ifdef USE_OLD_CYASSL
	if (wsi->tls.use_ssl == 2)
		CyaSSL_set_verify(wsi->tls.ssl, SSL_VERIFY_NONE, NULL);
#else
	if (wsi->tls.use_ssl == 2)
		wolfSSL_set_verify(wsi->tls.ssl, SSL_VERIFY_NONE, NULL);
#endif
#endif /* USE_WOLFSSL */

	wsi->tls.client_bio = BIO_new_socket((int)(long long)wsi->desc.sockfd, BIO_NOCLOSE);
	SSL_set_bio(wsi->tls.ssl, wsi->tls.client_bio, wsi->tls.client_bio);

#ifdef USE_WOLFSSL
#ifdef USE_OLD_CYASSL
	CyaSSL_set_using_nonblock(wsi->tls.ssl, 1);
#else
	wolfSSL_set_using_nonblock(wsi->tls.ssl, 1);
#endif
#else
	BIO_set_nbio(wsi->tls.client_bio, 1); /* nonblocking */
#endif

#if defined(LWS_HAVE_SSL_set_alpn_protos) && \
    defined(LWS_HAVE_SSL_get0_alpn_selected)
	if (wsi->vhost->tls.alpn)
		alpn_comma = wsi->vhost->tls.alpn;
#if defined(LWS_ROLE_H1) || defined(LWS_ROLE_H2)
	if (lws_hdr_copy(wsi, hostname, sizeof(hostname),
			 _WSI_TOKEN_CLIENT_ALPN) > 0)
		alpn_comma = hostname;
#endif

	lwsl_info("client conn using alpn list '%s'\n", alpn_comma);

	n = lws_alpn_comma_to_openssl(alpn_comma, openssl_alpn,
				      sizeof(openssl_alpn) - 1);

	SSL_set_alpn_protos(wsi->tls.ssl, openssl_alpn, n);
#endif

	SSL_set_ex_data(wsi->tls.ssl, openssl_websocket_private_data_index, wsi);

	return 0;
}

enum lws_ssl_capable_status
lws_tls_client_connect(struct lws *wsi)
{
 if (lws_vr_tracked1(wsi->context)) {
  struct lws_vr_tls_wsi1 *t = &wsi->vr_data.Tls;
  int actual, error, decoded;
  if (t->Step != 11 || !wsi->tls.ssl) return LWS_SSL_CAPABLE_ERROR;
  if (t->Connected) return LWS_SSL_CAPABLE_DONE;
  if (lws_vr_io1(wsi, VR_LWS_NAMED_CONNECT1) != VR_LWS_KNOWN_SUCCESS1) {
   lws_callback_on_writable(wsi); /* genuine next-turn no-attempt continuation */
   return LWS_SSL_CAPABLE_MORE_SERVICE;
  }
  ERR_clear_error();
  errno = 0; actual = SSL_connect(wsi->tls.ssl); error = errno;
  t->Result = actual; t->Error = error;
  lws_vr_io_record1(wsi->context, VR_LWS_NAMED_CONNECT1, actual, error);
  if (actual == 1) { t->Connected = 1; return LWS_SSL_CAPABLE_DONE; }
  decoded = SSL_get_error(wsi->tls.ssl, actual); t->Decoded = decoded;
  if (decoded == SSL_ERROR_WANT_READ) {
   lws_change_pollfd(wsi, LWS_POLLOUT, LWS_POLLIN); return LWS_SSL_CAPABLE_MORE_SERVICE_READ;
  }
  if (decoded == SSL_ERROR_WANT_WRITE) {
   lws_change_pollfd(wsi, 0, LWS_POLLOUT); return LWS_SSL_CAPABLE_MORE_SERVICE_WRITE;
  }
  return LWS_SSL_CAPABLE_ERROR;
 }

#if defined(LWS_HAVE_SSL_set_alpn_protos) && defined(LWS_HAVE_SSL_get0_alpn_selected)
	const unsigned char *prot;
	char a[32];
	unsigned int len;
#endif
	int m, n = SSL_connect(wsi->tls.ssl);

	if (n == 1) {
#if defined(LWS_HAVE_SSL_set_alpn_protos) && defined(LWS_HAVE_SSL_get0_alpn_selected)
		SSL_get0_alpn_selected(wsi->tls.ssl, &prot, &len);

		if (len >= sizeof(a))
			len = sizeof(a) - 1;
		memcpy(a, (const char *)prot, len);
		a[len] = '\0';

		lws_role_call_alpn_negotiated(wsi, (const char *)a);
#endif
		lwsl_info("client connect OK\n");
		return LWS_SSL_CAPABLE_DONE;
	}

	m = lws_ssl_get_error(wsi, n);

	if (m == SSL_ERROR_SYSCALL)
		return LWS_SSL_CAPABLE_ERROR;

	if (m == SSL_ERROR_WANT_READ || SSL_want_read(wsi->tls.ssl))
		return LWS_SSL_CAPABLE_MORE_SERVICE_READ;

	if (m == SSL_ERROR_WANT_WRITE || SSL_want_write(wsi->tls.ssl))
		return LWS_SSL_CAPABLE_MORE_SERVICE_WRITE;

	if (!n) /* we don't know what he wants, but he says to retry */
		return LWS_SSL_CAPABLE_MORE_SERVICE;

	return LWS_SSL_CAPABLE_ERROR;
}

int
lws_tls_client_confirm_peer_cert(struct lws *wsi, char *ebuf, int ebuf_len)
{
 if (lws_vr_tracked1(wsi->context)) {
  long actual = SSL_get_verify_result(wsi->tls.ssl);
  if (actual == X509_V_OK) return 0;
  if ((wsi->tls.use_ssl & LCCSCF_ALLOW_SELFSIGNED) &&
      (actual == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT || actual == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN)) return 0;
  /* No ordinary unconditional future-cert/ALLOW_EXPIRED widening. Engine's
   * actual verification callback already supplied IP-SAN/pins decisions. */
  return -1;
 }

#if !defined(USE_WOLFSSL)
	struct lws_context_per_thread *pt = &wsi->context->pt[(int)wsi->tsi];
	char *p = (char *)&pt->serv_buf[0];
	char *sb = p;
	int n;

	lws_latency_pre(wsi->context, wsi);
	n = SSL_get_verify_result(wsi->tls.ssl);
	lws_latency(wsi->context, wsi,
		"SSL_get_verify_result LWS_CONNMODE..HANDSHAKE", n, n > 0);

	lwsl_debug("get_verify says %d\n", n);

	if (n == X509_V_OK)
		return 0;

	if ((n == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT ||
	     n == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN) &&
	     (wsi->tls.use_ssl & LCCSCF_ALLOW_SELFSIGNED)) {
		lwsl_info("accepting self-signed certificate\n");

		return 0;
	}
	if ((n == X509_V_ERR_CERT_NOT_YET_VALID ||
	     n == X509_V_ERR_CERT_HAS_EXPIRED) &&
	     (wsi->tls.use_ssl & LCCSCF_ALLOW_EXPIRED)) {
		lwsl_info("accepting expired certificate\n");
		return 0;
	}
	if (n == X509_V_ERR_CERT_NOT_YET_VALID) {
		lwsl_info("Cert is from the future... "
			    "probably our clock... accepting...\n");
		return 0;
	}
	lws_snprintf(ebuf, ebuf_len,
		"server's cert didn't look good, X509_V_ERR = %d: %s\n",
		 n, ERR_error_string(n, sb));
	lwsl_info("%s\n", ebuf);
	lws_ssl_elaborate_error();

	return -1;

#else /* USE_WOLFSSL */
	return 0;
#endif
}

int
lws_tls_client_create_vhost_context(struct lws_vhost *vh,
				    const struct lws_context_creation_info *info,
				    const char *cipher_list,
				    const char *ca_filepath,
				    const char *cert_filepath,
				    const char *private_key_filepath)
{
	SSL_METHOD *method;
	unsigned long error;
	int n;

	/* basic openssl init already happened in context init */

	/* choose the most recent spin of the api */
#if defined(LWS_HAVE_TLS_CLIENT_METHOD)
	method = (SSL_METHOD *)TLS_client_method();
#elif defined(LWS_HAVE_TLSV1_2_CLIENT_METHOD)
	method = (SSL_METHOD *)TLSv1_2_client_method();
#else
	method = (SSL_METHOD *)SSLv23_client_method();
#endif

	if (!method) {
		error = ERR_get_error();
		lwsl_err("problem creating ssl method %lu: %s\n",
			error, ERR_error_string(error,
				      (char *)vh->context->pt[0].serv_buf));
		return 1;
	}
	/* create context */
	vh->tls.ssl_client_ctx = SSL_CTX_new(method);
	if (!vh->tls.ssl_client_ctx) {
		error = ERR_get_error();
		lwsl_err("problem creating ssl context %lu: %s\n",
			error, ERR_error_string(error,
				      (char *)vh->context->pt[0].serv_buf));
		return 1;
	}

#ifdef SSL_OP_NO_COMPRESSION
	SSL_CTX_set_options(vh->tls.ssl_client_ctx, SSL_OP_NO_COMPRESSION);
#endif

	SSL_CTX_set_options(vh->tls.ssl_client_ctx,
			    SSL_OP_CIPHER_SERVER_PREFERENCE);

	if (cipher_list)
		SSL_CTX_set_cipher_list(vh->tls.ssl_client_ctx, cipher_list);

#ifdef LWS_SSL_CLIENT_USE_OS_CA_CERTS
	if (!lws_check_opt(vh->options, LWS_SERVER_OPTION_DISABLE_OS_CA_CERTS))
		/* loads OS default CA certs */
		SSL_CTX_set_default_verify_paths(vh->tls.ssl_client_ctx);
#endif

	/* openssl init for cert verification (for client sockets) */
	if (!ca_filepath) {
		if (!SSL_CTX_load_verify_locations(
			vh->tls.ssl_client_ctx, NULL, LWS_OPENSSL_CLIENT_CERTS))
			lwsl_err("Unable to load SSL Client certs from %s "
			    "(set by LWS_OPENSSL_CLIENT_CERTS) -- "
			    "client ssl isn't going to work\n",
			    LWS_OPENSSL_CLIENT_CERTS);
	} else
		if (!SSL_CTX_load_verify_locations(
			vh->tls.ssl_client_ctx, ca_filepath, NULL)) {
			lwsl_err(
				"Unable to load SSL Client certs "
				"file from %s -- client ssl isn't "
				"going to work\n", ca_filepath);
			lws_ssl_elaborate_error();
		}
		else
			lwsl_info("loaded ssl_ca_filepath\n");

	/*
	 * callback allowing user code to load extra verification certs
	 * helping the client to verify server identity
	 */

	/* support for client-side certificate authentication */
	if (cert_filepath) {
		if (lws_tls_use_any_upgrade_check_extant(cert_filepath) != LWS_TLS_EXTANT_YES &&
		    (info->options & LWS_SERVER_OPTION_IGNORE_MISSING_CERT))
			return 0;

		lwsl_notice("%s: doing cert filepath %s\n", __func__, cert_filepath);
		n = SSL_CTX_use_certificate_chain_file(vh->tls.ssl_client_ctx,
						       cert_filepath);
		if (n < 1) {
			lwsl_err("problem %d getting cert '%s'\n", n,
				 cert_filepath);
			lws_ssl_elaborate_error();
			return 1;
		}
		lwsl_notice("Loaded client cert %s\n", cert_filepath);
	}
	if (private_key_filepath) {
		lwsl_notice("%s: doing private key filepath\n", __func__);
		lws_ssl_bind_passphrase(vh->tls.ssl_client_ctx, info);
		/* set the private key from KeyFile */
		if (SSL_CTX_use_PrivateKey_file(vh->tls.ssl_client_ctx,
		    private_key_filepath, SSL_FILETYPE_PEM) != 1) {
			lwsl_err("use_PrivateKey_file '%s'\n",
				 private_key_filepath);
			lws_ssl_elaborate_error();
			return 1;
		}
		lwsl_notice("Loaded client cert private key %s\n",
			    private_key_filepath);

		/* verify private key */
		if (!SSL_CTX_check_private_key(vh->tls.ssl_client_ctx)) {
			lwsl_err("Private SSL key doesn't match cert\n");
			return 1;
		}
	}

	return 0;
}


/* Selected tracked OpenSSL profile. Provider/source/code/full-return premises
 * are independently CLOSED; these owning continuations do not appoint them. */
static int
vr_tls_named1(struct lws *w, struct lws_vr_resource1 *r, uint32_t operation)
{
 int status = lws_vr_debit1(w->context->vr.View, w->context->vr.Budget, r, operation);
 if (status != VR_LWS_KNOWN_SUCCESS1) {
  w->context->vr.LastDisposition = status; w->context->vr.IoDeferred = 1;
  lws_callback_on_writable(w);
 }
 return status;
}
static int
vr_tls_free_local1(struct lws_context *c, struct lws_vr_resource1 *r,
 const VrLwsBudgetRef1 *b, int bio, int64_t *raw)
{
 int status, actual;
 if (r->Stage == LWS_VR_ACQUIRED1) {
  status = lws_vr_debit1(c->vr.View, b, r, VR_LWS_NAMED_DISPOSE1);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  r->Stage = LWS_VR_FREE_ENTERED1;
  if (bio) {
   actual = BIO_free((BIO *)r->Pointer); *raw = actual;
   r->Stage = actual == 1 ? LWS_VR_FREE_RETURNED1 : LWS_VR_DISPOSE_UNKNOWN1;
   varest_ws_c2l_record_bio_dispose_return1(c->vr.View, &r->Row, r->Pointer, actual, r->Row.Cell);
  } else {
   X509_free((X509 *)r->Pointer);
   r->Stage = LWS_VR_FREE_RETURNED1; /* DERIVED void full return, not raw int1 */
   varest_ws_c2l_record_void_dispose_return1(c->vr.View, &r->Row, r->Pointer, 0, r->Row.Cell);
  }
 }
 return lws_vr_ref_ack1(c->vr.View, b, r);
}

int
lws_vr_tls_vhost_create1(struct lws_vhost *vh, const struct lws_context_creation_info *info)
{
 struct lws_context *c = vh->context;
 struct lws_vr_tls_vhost1 *t = &vh->vr.Tls;
 const VrLwsBudgetRef1 *b = c->vr.Budget;
 const char *cipher = info->client_ssl_cipher_list ? info->client_ssl_cipher_list : info->ssl_cipher_list;
 const SSL_METHOD *method;
 VrLwsResult1 engine = {0};
 VrLwsHookOutcome1 borrow = {VR_LWS_HOOK_LOCAL_INVALID1,0,0,0};
 int status, actual;
 if (!b || b->Active || !b->Creating || b->Creating->Phase != VR_LWS_PHASE_STARTUP1 || t->Step)
  return VR_LWS_PROTOCOL_FAULT1; /* constructor cannot restart */
 t->Step = 1;
 status = lws_vr_reserve1(c->vr.View, &c->vr.Vhosts[c->vr.VhostAttempts - 1].Allocation.Row,
  8, 2, b, &t->Ctx);
 if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 status = lws_vr_debit1(c->vr.View, b, &t->Ctx, VR_LWS_NAMED_ACQUIRE1);
 if (status != VR_LWS_KNOWN_SUCCESS1) return status;
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
 method = TLS_client_method();
#else
 method = SSLv23_client_method();
#endif
 t->Ctx.Pointer = SSL_CTX_new(method);
 t->Ctx.Stage = t->Ctx.Pointer ? LWS_VR_ACQUIRED1 : LWS_VR_NO_RESOURCE1;
 varest_ws_c2l_record_pointer_acquire1(c->vr.View, &t->Ctx.Row, t->Ctx.Pointer, 0, 0, t->Ctx.Row.Cell);
 vh->tls.ssl_client_ctx = (SSL_CTX *)t->Ctx.Pointer;
 status = lws_vr_ack_retire1(c->vr.View, b, &t->Ctx);
 if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 if (!t->Ctx.Pointer) return VR_LWS_NO_RESOURCE_FAILURE1;
 /* Each actual selected setter is ONE setup11, no global init/index/path load. */
 status = lws_vr_debit1(c->vr.View, b, &t->Ctx, VR_LWS_NAMED_TLS_SETUP1);
 if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 t->ControlBits = SSL_CTX_set_options(vh->tls.ssl_client_ctx, SSL_OP_NO_COMPRESSION); t->OptionStep = 1;
 status = lws_vr_debit1(c->vr.View, b, &t->Ctx, VR_LWS_NAMED_TLS_SETUP1);
 if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 t->ControlBits = SSL_CTX_set_options(vh->tls.ssl_client_ctx, SSL_OP_CIPHER_SERVER_PREFERENCE); t->OptionStep = 2;
 if (cipher) {
  status = lws_vr_debit1(c->vr.View, b, &t->Ctx, VR_LWS_NAMED_TLS_SETUP1);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  actual = SSL_CTX_set_cipher_list(vh->tls.ssl_client_ctx, cipher); t->Result = actual; t->OptionStep = 3;
  if (actual != 1) { t->Step = UINT32_MAX; return VR_LWS_NO_RESOURCE_FAILURE1; }
 }
 status = varest_ws_c2l_install_engine_roots1(vh, vh->tls.ssl_client_ctx, c->vr.View,
  &t->Ctx.Row, b->Creating, &t->RootStage, &engine);
 /* Not an Engine-success certificate. Advance ONLY after WHOLE CPP return. */
 if (t->RootStage == VR_LWS_ENGINE_ROOTS_ENGINE_RETURNED1)
  t->RootStage = VR_LWS_ENGINE_ROOTS_BRIDGE_RETURNED1;
 t->Failure = (uint32_t)status;
 if (status != VR_LWS_KNOWN_SUCCESS1 || t->RootStage != VR_LWS_ENGINE_ROOTS_BRIDGE_RETURNED1)
  return status == VR_LWS_KNOWN_SUCCESS1 ? VR_LWS_PROTOCOL_FAULT1 : status;
 t->Pem = NULL; t->PemBytes = 0;
 status = varest_ws_c2l_borrow_extra_trusted_pem1(vh, vh->tls.ssl_client_ctx, c->vr.View,
  &t->Ctx.Row, &t->Pem, &t->PemBytes, &borrow);
 if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 if (borrow.Disposition != VR_LWS_HOOK_COMPLETED1 || borrow.NativeStatus || borrow.NativeCalls || borrow.Reserved ||
     t->PemBytes > 65536 || (t->PemBytes && !t->Pem)) return VR_LWS_PROTOCOL_FAULT1;
 if (!t->PemBytes) { t->Step = 9; return VR_LWS_KNOWN_SUCCESS1; }
 status = lws_vr_reserve1(c->vr.View, &t->Ctx.Row, 8, 5, b, &t->MemBio);
 if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 status = lws_vr_debit1(c->vr.View, b, &t->MemBio, VR_LWS_NAMED_ACQUIRE1);
 if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 t->MemBio.Pointer = BIO_new_mem_buf(t->Pem, (int)t->PemBytes);
 t->MemBio.Stage = t->MemBio.Pointer ? LWS_VR_ACQUIRED1 : LWS_VR_NO_RESOURCE1;
 varest_ws_c2l_record_pointer_acquire1(c->vr.View, &t->MemBio.Row, t->MemBio.Pointer, 0, 0, t->MemBio.Row.Cell);
 status = lws_vr_ack_retire1(c->vr.View, b, &t->MemBio);
 if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 if (!t->MemBio.Pointer) return VR_LWS_NO_RESOURCE_FAILURE1;
 /* At most16 parsed certs plus ONE actual final-NULL/excess probe. One BIO. */
 for (t->Parsed = 0; t->Parsed <= 16; ++t->Parsed) {
  status = lws_vr_reserve1(c->vr.View, &t->Ctx.Row, 8, 6, b, &t->Local);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  status = lws_vr_debit1(c->vr.View, b, &t->Local, VR_LWS_NAMED_ACQUIRE1);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  t->Local.Pointer = PEM_read_bio_X509((BIO *)t->MemBio.Pointer, NULL, NULL, NULL);
  t->Local.Stage = t->Local.Pointer ? LWS_VR_ACQUIRED1 : LWS_VR_NO_RESOURCE1;
  varest_ws_c2l_record_pointer_acquire1(c->vr.View, &t->Local.Row, t->Local.Pointer, 0, 0, t->Local.Row.Cell);
  status = lws_vr_ack_retire1(c->vr.View, b, &t->Local);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  if (!t->Local.Pointer) { lws_vr_resource_init1(&t->Local); break; }
  if (t->Parsed == 16) return VR_LWS_NO_RESOURCE_FAILURE1; /* actual excess ref remains owned for cleanup */
  {
   struct lws_vr_resource1 *r = &t->Store[t->Parsed];
   X509_STORE *store;
   status = lws_vr_reserve1(c->vr.View, &t->Ctx.Row, 8, 7, b, r);
   if (status != VR_LWS_KNOWN_SUCCESS1) return status;
   store = SSL_CTX_get_cert_store(vh->tls.ssl_client_ctx); /* borrowed within this full call */
   if (!store) return VR_LWS_NO_RESOURCE_FAILURE1; /* reservation is genuinely unattempted */
   status = lws_vr_debit1(c->vr.View, b, r, VR_LWS_NAMED_ACQUIRE1);
   if (status != VR_LWS_KNOWN_SUCCESS1) return status;
   actual = X509_STORE_add_cert(store, (X509 *)t->Local.Pointer);
   r->Pointer = t->Local.Pointer; r->Stage = actual == 1 ? LWS_VR_ACQUIRED1 : actual == 0 ? LWS_VR_NO_RESOURCE1 : LWS_VR_UNKNOWN1;
   t->Result = actual; t->StoreIndex = t->Parsed + 1;
   varest_ws_c2l_record_store_ref_acquire1(c->vr.View, &r->Row, r->Pointer, actual, r->Row.Cell);
   status = lws_vr_ref_ack1(c->vr.View, b, r);
   if (status != VR_LWS_KNOWN_SUCCESS1) return status;
   /* Actual0 is best-effort duplicate/add failure: no store ref, keep parsing. */
  }
  status = vr_tls_free_local1(c, &t->Local, b, 0, &t->Result);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 }
 status = vr_tls_free_local1(c, &t->MemBio, b, 1, &t->Result);
 if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 t->Pem = NULL; t->PemBytes = 0; t->Step = 9;
 /* Error-queue decoding/clearing only after all raw records; bounded provider H. */
 ERR_clear_error();
 return VR_LWS_KNOWN_SUCCESS1;
}

int
lws_vr_tls_vhost_cleanup1(struct lws_vhost *vh, const VrLwsBudgetRef1 *b)
{
 struct lws_context *c = vh->context;
 struct lws_vr_tls_vhost1 *t = &vh->vr.Tls;
 unsigned int i;
 int status;
 status = vr_tls_free_local1(c, &t->Local, b, 0, &t->Result);
 if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 status = vr_tls_free_local1(c, &t->MemBio, b, 1, &t->Result);
 if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 t->Pem = NULL; t->PemBytes = 0;
 for (i = 0; i < 2; ++i) {
  struct lws *w = c->vr.Data[i].Object;
  if (w && w->vhost == vh && (w->tls.ssl ||
      (w->vr_data.Tls.Ssl.Stage != LWS_VR_EMPTY1 && w->vr_data.Tls.Ssl.Stage != LWS_VR_RETIRED1)))
   return VR_LWS_FAILED_WITH_OUTSTANDING1;
 }
 /* A never-attempted/failed store row must retire BEFORE parent disposal;
  * unknown ownership cannot be converted into a known parent-release fact. */
 for (i = 0; i < 16; ++i) if (t->Store[i].Stage != LWS_VR_ACQUIRED1) {
  status = lws_vr_ref_ack1(c->vr.View, b, &t->Store[i]);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 }
 if (t->Ctx.Stage == LWS_VR_ACQUIRED1) {
  status = lws_vr_debit1(c->vr.View, b, &t->Ctx, VR_LWS_NAMED_DISPOSE1);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  t->Ctx.Stage = LWS_VR_FREE_ENTERED1;
  SSL_CTX_free((SSL_CTX *)t->Ctx.Pointer);
  t->Ctx.Stage = LWS_VR_FREE_RETURNED1; vh->tls.ssl_client_ctx = NULL;
  /* Matching provider graph/last-alias/full-return theorem is mandatory.
   * Direct stores transfer ONLY here, never their own X509_free. */
  for (i = 0; i < 16; ++i) if (t->Store[i].Stage == LWS_VR_ACQUIRED1) {
   t->Store[i].Stage = LWS_VR_FREE_RETURNED1;
   varest_ws_c2l_record_parent_release_return1(c->vr.View, &t->Store[i].Row,
    t->Store[i].Pointer, t->Store[i].Row.Cell);
  }
  varest_ws_c2l_record_void_dispose_return1(c->vr.View, &t->Ctx.Row, t->Ctx.Pointer, 0, t->Ctx.Row.Cell);
 }
 for (i = 0; i < 16; ++i) {
  status = lws_vr_ref_ack1(c->vr.View, b, &t->Store[i]);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
 }
 return lws_vr_ref_ack1(c->vr.View, b, &t->Ctx);
}

static int
vr_tls_verify1(int preverify_ok, X509_STORE_CTX *store)
{
 SSL *ssl = X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx());
 struct lws *w = ssl ? (struct lws *)SSL_get_app_data(ssl) : NULL;
 int verdict, error;
 if (!w || !lws_vr_data_valid1(w) || w->tls.ssl != ssl ||
     w->vr_data.Tls.Ssl.Pointer != ssl || w->vr_data.Tls.Ssl.Stage != LWS_VR_ACQUIRED1)
  return 0;
 error = X509_STORE_CTX_get_error(store);
 /* Same narrow ordinary self-signed override, only under the genuine Conn's
  * existing insecure flag mapping. No expired/future-cert widening. */
 if (!preverify_ok && (w->tls.use_ssl & LCCSCF_ALLOW_SELFSIGNED) &&
     (error == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT || error == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN))
  return 1;
 verdict = lws_vr_callback1(w, LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION,
  store, ssl, (size_t)preverify_ok);
 if (verdict) return 0;
 if (!preverify_ok && X509_STORE_CTX_get_error(store) != X509_V_OK) return 0;
 return 1; /* actual callback preserves Engine IP-SAN/pins policy */
}

int
lws_vr_tls_wsi_create1(struct lws *w)
{
 struct lws_context *c = w->context;
 struct lws_vr_tls_wsi1 *t = &w->vr_data.Tls;
 const VrLwsBudgetRef1 *b = c->vr.Budget;
 const char *name = w->vr_data.Strings[0].Pointer; /* ORIGINAL host, never relay */
 X509_VERIFY_PARAM *param;
 unsigned char address[16];
 int status, actual;
 if (!lws_vr_data_valid1(w) || !b || !b->Active || b->Creating ||
     w->vr_data.RouteStep != 3 || !w->vhost->tls.ssl_client_ctx || !name ||
     (w->tls.use_ssl & LCCSCF_ALLOW_EXPIRED)) return VR_LWS_PROTOCOL_FAULT1;
 if (t->Step == 0) {
  if (t->Ssl.Stage == LWS_VR_EMPTY1) {
   status = lws_vr_reserve1(c->vr.View, &w->vr_allocation->Row, 8, 3, b, &t->Ssl);
   if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  }
  if (t->Ssl.Stage == LWS_VR_RESERVED1) {
   status = vr_tls_named1(w, &t->Ssl, VR_LWS_NAMED_ACQUIRE1);
   if (status != VR_LWS_KNOWN_SUCCESS1) return status;
   t->Ssl.Pointer = SSL_new(w->vhost->tls.ssl_client_ctx);
   t->Ssl.Stage = t->Ssl.Pointer ? LWS_VR_ACQUIRED1 : LWS_VR_NO_RESOURCE1;
   varest_ws_c2l_record_pointer_acquire1(c->vr.View, &t->Ssl.Row, t->Ssl.Pointer, 0, 0, t->Ssl.Row.Cell);
   w->tls.ssl = (SSL *)t->Ssl.Pointer;
  }
  status = lws_vr_ack_retire1(c->vr.View, b, &t->Ssl);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  if (!t->Ssl.Pointer) return VR_LWS_NO_RESOURCE_FAILURE1;
  t->NameIsIp = inet_pton(AF_INET, name, address) == 1 || inet_pton(AF_INET6, name, address) == 1;
  t->Step = 2;
 }
 if (t->Step == 2) {
  status = vr_tls_named1(w, &t->Ssl, VR_LWS_NAMED_TLS_SETUP1);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  actual = SSL_set_app_data(w->tls.ssl, w); t->Result = actual; t->Step = 3;
  if (actual != 1) { t->Step = UINT32_MAX; return VR_LWS_NO_RESOURCE_FAILURE1; }
 }
 if (t->Step == 3) {
  if (!(w->tls.use_ssl & LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK)) {
   param = SSL_get0_param(w->tls.ssl);
   if (!param) return VR_LWS_NO_RESOURCE_FAILURE1;
   status = vr_tls_named1(w, &t->Ssl, VR_LWS_NAMED_TLS_SETUP1);
   if (status != VR_LWS_KNOWN_SUCCESS1) return status;
   X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
  }
  t->Step = 4;
 }
 if (t->Step == 4) {
  if (!(w->tls.use_ssl & LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK)) {
   param = SSL_get0_param(w->tls.ssl);
   if (!param) return VR_LWS_NO_RESOURCE_FAILURE1;
   status = vr_tls_named1(w, &t->Ssl, VR_LWS_NAMED_TLS_SETUP1);
   if (status != VR_LWS_KNOWN_SUCCESS1) return status;
   actual = t->NameIsIp ? X509_VERIFY_PARAM_set1_ip_asc(param, name) : X509_VERIFY_PARAM_set1_host(param, name, 0);
   t->Result = actual; t->Step = 5;
   if (actual != 1) { t->Step = UINT32_MAX; return VR_LWS_NO_RESOURCE_FAILURE1; }
  } else t->Step = 5;
 }
 if (t->Step == 5) {
  status = vr_tls_named1(w, &t->Ssl, VR_LWS_NAMED_TLS_SETUP1);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  SSL_set_verify(w->tls.ssl, SSL_VERIFY_PEER, vr_tls_verify1); t->Step = 6;
 }
 if (t->Step == 6) {
  status = vr_tls_named1(w, &t->Ssl, VR_LWS_NAMED_TLS_SETUP1);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  t->Result = (int64_t)SSL_set_mode(w->tls.ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER); t->Step = 7;
 }
 if (t->Step == 7) {
  if (!t->NameIsIp) {
   status = vr_tls_named1(w, &t->Ssl, VR_LWS_NAMED_TLS_SETUP1);
   if (status != VR_LWS_KNOWN_SUCCESS1) return status;
   actual = SSL_set_tlsext_host_name(w->tls.ssl, name); t->Result = actual; t->Step = 8;
   if (actual != 1) { t->Step = UINT32_MAX; return VR_LWS_NO_RESOURCE_FAILURE1; }
  } else t->Step = 8;
 }
 if (t->Step == 8) {
  if (t->Bio.Stage == LWS_VR_EMPTY1) {
   status = lws_vr_reserve1(c->vr.View, &t->Ssl.Row, 8, 4, b, &t->Bio);
   if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  }
  if (t->Bio.Stage == LWS_VR_RESERVED1) {
   status = vr_tls_named1(w, &t->Bio, VR_LWS_NAMED_ACQUIRE1);
   if (status != VR_LWS_KNOWN_SUCCESS1) return status;
   t->Bio.Pointer = BIO_new_socket(w->desc.sockfd, BIO_NOCLOSE);
   t->Bio.Stage = t->Bio.Pointer ? LWS_VR_ACQUIRED1 : LWS_VR_NO_RESOURCE1;
   varest_ws_c2l_record_pointer_acquire1(c->vr.View, &t->Bio.Row, t->Bio.Pointer, 0, 0, t->Bio.Row.Cell);
   w->tls.client_bio = (BIO *)t->Bio.Pointer;
  }
  status = lws_vr_ack_retire1(c->vr.View, b, &t->Bio);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  if (!t->Bio.Pointer) return VR_LWS_NO_RESOURCE_FAILURE1; /* BEFORE set_bio */
  t->Step = 9;
 }
 if (t->Step == 9) {
  status = vr_tls_named1(w, &t->Ssl, VR_LWS_NAMED_TLS_SETUP1);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  t->Transferred = 1; /* entered; no direct BIO disposal if return is unknown */
  SSL_set_bio(w->tls.ssl, w->tls.client_bio, w->tls.client_bio);
  t->Transferred = 2; t->Step = 10; /* actual full void return; provider theorem still required */
 }
 if (t->Step == 10) {
  status = vr_tls_named1(w, &t->Ssl, VR_LWS_NAMED_TLS_SETUP1);
  if (status != VR_LWS_KNOWN_SUCCESS1) return status;
  t->Result = BIO_set_nbio(w->tls.client_bio, 1); t->Step = 11;
  if (t->Result != 1) { t->Step = UINT32_MAX; return VR_LWS_NO_RESOURCE_FAILURE1; }
 }
 return t->Step == 11 ? VR_LWS_KNOWN_SUCCESS1 : VR_LWS_PROTOCOL_FAULT1;
}
