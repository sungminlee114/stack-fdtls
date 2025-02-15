/*******************************************************************************
 *
 * Copyright (c) 2011, 2012, 2013, 2014, 2015 Olaf Bergmann (TZI) and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v. 1.0 which accompanies this distribution.
 *
 * The Eclipse Public License is available at http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at 
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Olaf Bergmann  - initial API and implementation
 *    Hauke Mehrtens - memory optimization, ECC integration
 *    Achim Kraus    - session recovery
 *    Sachin Agrawal - rehandshake support
 *
 *******************************************************************************/

#include "tinydtls.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_ASSERT_H
#include <assert.h>
#endif

#include "dtls-numeric.h"
#include "netq.h"
#include "dtls.h"

#include "dtls-alert.h"
#include "dtls-support.h"

#ifdef WITH_SHA256
#  include "sha2/sha2.h"
#endif

/* Log configuration */
#define LOG_MODULE "dtls"
#define LOG_LEVEL  LOG_LEVEL_DTLS
#include "dtls-log.h"

#define dtls_set_version(H,V) dtls_int_to_uint16((H)->version, (V))
#define dtls_set_content_type(H,V) ((H)->content_type = (V) & 0xff)
#define dtls_set_length(H,V)  ((H)->length = (V))

#define dtls_get_content_type(H) ((H)->content_type & 0xff)
#define dtls_get_version(H) dtls_uint16_to_int((H)->version)
#define dtls_get_epoch(H) dtls_uint16_to_int((H)->epoch)
#define dtls_get_sequence_number(H) dtls_uint48_to_ulong((H)->sequence_number)
#define dtls_get_fragment_length(H) dtls_uint24_to_int((H)->fragment_length)

static void
delete_peer(dtls_peer_t **peers, dtls_peer_t *peer)
{
  struct dtls_peer_t *l, *r;
  if(peers == NULL || *peers == NULL || peer == NULL) {
    return;
  }
  r = NULL;
  for(l = *peers; l != NULL; l = l->next) {
    if(l == peer) {
      if(r == NULL) {
	/* First on list */
	*peers = l->next;
      } else {
	/* Not first on list */
	r->next = l->next;
      }
      l->next = NULL;
      return;
    }
    r = l;
  }
}

static void
add_peer(dtls_peer_t **peers, dtls_peer_t *peer)
{
  peer->next = *peers;
  *peers = peer;
}

#define DTLS_RH_LENGTH sizeof(dtls_record_header_t)
#define DTLS_HS_LENGTH sizeof(dtls_handshake_header_t)
#define DTLS_CH_LENGTH sizeof(dtls_client_hello_t) /* no variable length fields! */
#define DTLS_COOKIE_LENGTH_MAX 32
#define DTLS_CH_LENGTH_MAX sizeof(dtls_client_hello_t) + DTLS_COOKIE_LENGTH_MAX + 12 + 26
#define DTLS_HV_LENGTH sizeof(dtls_hello_verify_t)
#define DTLS_SH_LENGTH (2 + DTLS_RANDOM_LENGTH + 1 + 2 + 1)
#define DTLS_CE_LENGTH (3 + 3 + 27 + DTLS_EC_KEY_SIZE + DTLS_EC_KEY_SIZE)
#define DTLS_SKEXEC_LENGTH (1 + 2 + 1 + 1 + DTLS_EC_KEY_SIZE + DTLS_EC_KEY_SIZE + 1 + 1 + 2 + 70)
#define DTLS_SKEXECPSK_LENGTH_MIN 2
#define DTLS_SKEXECPSK_LENGTH_MAX 2 + DTLS_PSK_MAX_CLIENT_IDENTITY_LEN
#define DTLS_CKXPSK_LENGTH_MIN 2
#define DTLS_CKXEC_LENGTH (1 + 1 + DTLS_EC_KEY_SIZE + DTLS_EC_KEY_SIZE)
#define DTLS_CV_LENGTH (1 + 1 + 2 + 1 + 1 + 1 + 1 + DTLS_EC_KEY_SIZE + 1 + 1 + DTLS_EC_KEY_SIZE)
#define DTLS_FIN_LENGTH 12

#define HS_HDR_LENGTH  DTLS_RH_LENGTH + DTLS_HS_LENGTH
#define HV_HDR_LENGTH  HS_HDR_LENGTH + DTLS_HV_LENGTH

#define HIGH(V) (((V) >> 8) & 0xff)
#define LOW(V)  ((V) & 0xff)

#define DTLS_RECORD_HEADER(M) ((dtls_record_header_t *)(M))
#define DTLS_HANDSHAKE_HEADER(M) ((dtls_handshake_header_t *)(M))

#define HANDSHAKE(M) ((dtls_handshake_header_t *)((M) + DTLS_RH_LENGTH))
#define CLIENTHELLO(M) ((dtls_client_hello_t *)((M) + HS_HDR_LENGTH))

/* The length check here should work because dtls_*_to_int() works on
 * unsigned char. Otherwise, broken messages could cause severe
 * trouble. Note that this macro jumps out of the current program flow
 * when the message is too short. Beware!
 */
#define SKIP_VAR_FIELD(P,L) {						\
    if (L < dtls_uint8_to_int(P) + sizeof(uint8_t))			\
      goto error;			       		 	        \
    L -= dtls_uint8_to_int(P) + sizeof(uint8_t);			\
    P += dtls_uint8_to_int(P) + sizeof(uint8_t);  			\
  }

/* some constants for the PRF */
#define PRF_LABEL(Label) prf_label_##Label
#define PRF_LABEL_SIZE(Label) (sizeof(PRF_LABEL(Label)) - 1)

static const unsigned char prf_label_master[] = "master secret";
static const unsigned char prf_label_key[] = "key expansion";
static const unsigned char prf_label_client[] = "client";
static const unsigned char prf_label_server[] = "server";
static const unsigned char prf_label_finished[] = " finished";

#if DTLS_ECC
/* first part of Raw public key, the is the start of the Subject Public Key */
static const unsigned char cert_asn1_header[] = {
  0x30, 0x59, /* SEQUENCE, length 89 bytes */
    0x30, 0x13, /* SEQUENCE, length 19 bytes */
      0x06, 0x07, /* OBJECT IDENTIFIER ecPublicKey (1 2 840 10045 2 1) */
        0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01,
      0x06, 0x08, /* OBJECT IDENTIFIER prime256v1 (1 2 840 10045 3 1 7) */
        0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07,
      0x03, 0x42, 0x00, /* BIT STRING, length 66 bytes, 0 bits unused */
         0x04 /* uncompressed, followed by the r und s values of the public key */
};
#endif

void
dtls_init() {
  dtls_support_init();
  dtls_crypto_init();
  dtls_hmac_storage_init();
  netq_init();
  dtls_peer_init();
}

/* Calls cb_alert() with given arguments if defined, otherwise an
 * error message is logged and the result is -1. This is just an
 * internal helper.
 */
#define CALL(Context, which, ...)					\
  ((Context)->h && (Context)->h->which					\
   ? (Context)->h->which((Context), ##__VA_ARGS__)			\
   : -1)

static int
dtls_send_multi(dtls_context_t *ctx, dtls_peer_t *peer,
		dtls_security_parameters_t *security , session_t *session,
		unsigned char type, uint8_t *buf_array[],
		size_t buf_len_array[], size_t buf_array_len);

/** 
 * Sends the fragment of length \p buflen given in \p buf to the
 * specified \p peer. The data will be MAC-protected and encrypted
 * according to the selected cipher and split into one or more DTLS
 * records of the specified \p type. This function returns the number
 * of bytes that were sent, or \c -1 if an error occurred.
 *
 * \param ctx    The DTLS context to use.
 * \param peer   The remote peer.
 * \param type   The content type of the record. 
 * \param buf    The data to send.
 * \param buflen The actual length of \p buf.
 * \return Less than zero on error, the number of bytes written otherwise.
 */
static int
dtls_send(dtls_context_t *ctx, dtls_peer_t *peer, unsigned char type,
	  uint8_t *buf, size_t buflen) {
  if(peer) {
    return dtls_send_multi(ctx, peer, dtls_security_params(peer),
                           &peer->session, type, &buf, &buflen, 1);
  }
  return 0;
}

/**
 * Stops ongoing retransmissions of handshake messages for @p peer.
 */
static void dtls_stop_retransmission(dtls_context_t *context, dtls_peer_t *peer);

dtls_peer_t *
dtls_get_peer(const dtls_context_t *ctx, const session_t *session) {
  dtls_peer_t *p;
  if(ctx && session) {
    p = ctx->peers;
    while(p) {
      if (dtls_session_equals(&(p->session), session)) {
        return p;
      }
      p = p->next;
    }
  }
  return NULL;
}

/**
 * Adds @p peer to list of peers in @p ctx. This function returns @c 0
 * on success, or a negative value on error (e.g. due to insufficient
 * storage).
 */
static int
dtls_add_peer(dtls_context_t *ctx, dtls_peer_t *peer) {
  if(peer) {
    add_peer(&ctx->peers, peer);
  }
  return 0;
}

int
dtls_write(struct dtls_context_t *ctx, 
	   session_t *dst, uint8_t *buf, size_t len) {
  
  dtls_peer_t *peer = dtls_get_peer(ctx, dst);

  /* Check if peer connection already exists */
  if (!peer) { /* no ==> create one */
    int res;

    /* dtls_connect() returns a value greater than zero if a new
     * connection attempt is made, 0 for session reuse. */
    res = dtls_connect(ctx, dst);

    return (res >= 0) ? 0 : res;
  } else { /* a session exists, check if it is in state connected */
    
    if (peer->state != DTLS_STATE_CONNECTED) {
      return 0;
    } else {
      return dtls_send(ctx, peer, DTLS_CT_APPLICATION_DATA, buf, len);
    }
  }
}

static int
dtls_get_cookie(uint8_t *msg, size_t msglen, uint8_t **cookie) {
  /* To access the cookie, we have to determine the session id's
   * length and skip the whole thing. */
  if (msglen < DTLS_HS_LENGTH + DTLS_CH_LENGTH + sizeof(uint8_t))
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);

  if (dtls_uint16_to_int(msg + DTLS_HS_LENGTH) != DTLS_VERSION)
    return dtls_alert_fatal_create(DTLS_ALERT_PROTOCOL_VERSION);

  msglen -= DTLS_HS_LENGTH + DTLS_CH_LENGTH;
  msg += DTLS_HS_LENGTH + DTLS_CH_LENGTH;

  SKIP_VAR_FIELD(msg, msglen); /* skip session id */

  if (msglen < (*msg & 0xff) + sizeof(uint8_t))
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  
  *cookie = msg + sizeof(uint8_t);
  return dtls_uint8_to_int(msg);

 error:
  return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
}

static int
dtls_create_cookie(dtls_context_t *ctx, 
		   session_t *session,
		   uint8_t *msg, size_t msglen,
		   uint8_t *cookie, int *clen) {
  unsigned char buf[DTLS_HMAC_MAX];
  size_t len, e;

  /* create cookie with HMAC-SHA256 over:
   * - SECRET
   * - session parameters (only IP address?)
   * - client version 
   * - random gmt and bytes
   * - session id
   * - cipher_suites 
   * - compression method
   */

  /* We use our own buffer as hmac_context instead of a dynamic buffer
   * created by dtls_hmac_new() to separate storage space for cookie
   * creation from storage that is used in real sessions. Note that
   * the buffer size must fit with the default hash algorithm (see
   * implementation of dtls_hmac_context_new()). */

  dtls_hmac_context_t hmac_context;
  dtls_hmac_init(&hmac_context, ctx->cookie_secret, DTLS_COOKIE_SECRET_LENGTH);

  dtls_hmac_update(&hmac_context,
		   (unsigned char *)dtls_session_get_address(session),
                   dtls_session_get_address_size(session));

  /* feed in the beginning of the Client Hello up to and including the
     session id */
  e = sizeof(dtls_client_hello_t);
  e += (*(msg + DTLS_HS_LENGTH + e) & 0xff) + sizeof(uint8_t);
  if (e + DTLS_HS_LENGTH > msglen)
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);

  dtls_hmac_update(&hmac_context, msg + DTLS_HS_LENGTH, e);
  
  /* skip cookie bytes and length byte */
  e += *(uint8_t *)(msg + DTLS_HS_LENGTH + e) & 0xff;
  e += sizeof(uint8_t);
  if (e + DTLS_HS_LENGTH > msglen)
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);

  dtls_hmac_update(&hmac_context, 
		   msg + DTLS_HS_LENGTH + e,
		   dtls_get_fragment_length(DTLS_HANDSHAKE_HEADER(msg)) - e);

  len = dtls_hmac_finalize(&hmac_context, buf);

  if (len < *clen) {
    memset(cookie + len, 0, *clen - len);
    *clen = len;
  }
  
  memcpy(cookie, buf, *clen);
  return 0;
}

#ifdef DTLS_CHECK_CONTENTTYPE
/* used to check if a received datagram contains a DTLS message */
static char const content_types[] = { 
  DTLS_CT_CHANGE_CIPHER_SPEC,
  DTLS_CT_ALERT,
  DTLS_CT_HANDSHAKE,
  DTLS_CT_APPLICATION_DATA,
  0 				/* end marker */
};
#endif

/**
 * Checks if \p msg points to a valid DTLS record. If
 * 
 */
static unsigned int
is_record(uint8_t *msg, size_t msglen) {
  unsigned int rlen = 0;

  if (msglen >= DTLS_RH_LENGTH	/* FIXME allow empty records? */
#ifdef DTLS_CHECK_CONTENTTYPE
      && strchr(content_types, msg[0])
#endif
      && msg[1] == HIGH(DTLS_VERSION)
      && msg[2] == LOW(DTLS_VERSION)) 
    {
      rlen = DTLS_RH_LENGTH + 
	dtls_uint16_to_int(DTLS_RECORD_HEADER(msg)->length);
      
      /* we do not accept wrong length field in record header */
      if (rlen > msglen)	
	rlen = 0;
  } 
  
  return rlen;
}

/**
 * Initializes \p buf as record header. The caller must ensure that \p
 * buf is capable of holding at least \c sizeof(dtls_record_header_t)
 * bytes. Increments sequence number counter of \p security.
 * \return pointer to the next byte after the written header.
 * The length will be set to 0 and has to be changed before sending.
 */ 
static inline uint8_t *
dtls_set_record_header(uint8_t type, dtls_security_parameters_t *security,
		       uint8_t *buf) {
  
  dtls_int_to_uint8(buf, type);
  buf += sizeof(uint8_t);

  dtls_int_to_uint16(buf, DTLS_VERSION);
  buf += sizeof(uint16_t);

  if (security) {
    dtls_int_to_uint16(buf, security->epoch);
    buf += sizeof(uint16_t);

    dtls_int_to_uint48(buf, security->rseq);
    buf += 6; /* 48 bits */

    /* increment record sequence counter by 1 */
    security->rseq++;
  } else {
    memset(buf, 0, sizeof(uint16_t) + 6); /* 16 + 48 bits */
    buf += sizeof(uint16_t) + 6; /* 16 + 48 bits */
  }

  memset(buf, 0, sizeof(uint16_t));
  return buf + sizeof(uint16_t);
}

static int
dtls_encrypt_x(dtls_context_t * ctx, dtls_peer_t *peer, dtls_security_parameters_t *security , session_t *session, unsigned char type, 
uint8_t* buf_array[], size_t buf_len_array[], size_t buf_array_len, uint8_t* sendbuf, size_t s_len);

static int
dtls_prepare_record_x(dtls_peer_t *peer, dtls_security_parameters_t *security,
		    unsigned char type,
		    uint8_t *data_array[], size_t data_len_array[],
		    size_t data_array_len,
		    uint8_t *sendbuf, size_t *rlen);

int
dtls_encrypt_data(dtls_context_t * ctx, session_t *dst, uint8_t *buf,size_t len, uint8_t*sendbuf, size_t s_len){
	dtls_peer_t * peer = dtls_get_peer(ctx, dst);

	if(!peer){
		printf("no peer! and dtls_connect executed!\n");
		int res;
    peer = dtls_new_peer(dst);
		res = dtls_connect_peer(ctx, peer);
		return (res >= 0) ? 0 : res;
	} else {
		if(peer->state != DTLS_STATE_CONNECTED) {
			return 0;
		} else {
			// have to change func name
			return dtls_encrypt_x(ctx,peer,dtls_security_params(peer) ,&peer->session,DTLS_CT_APPLICATION_DATA,&buf,&len,1,sendbuf,s_len);
		}
	}
}

static int
dtls_encrypt_x(dtls_context_t * ctx, dtls_peer_t *peer, dtls_security_parameters_t *security , session_t *session,
 unsigned char type, uint8_t* buf_array[], size_t buf_len_array[], size_t buf_array_len, uint8_t* sendbuf, size_t s_len) {
	//unsigned char sendbuf[DTLS_MAX_BUF];
	size_t len = s_len;
  //printf("sizeof:%d\n",len);
	int res;
	size_t overall_len = 0;

  res = dtls_prepare_record_x(peer, security, type, buf_array, buf_len_array, buf_array_len, sendbuf, &len);
  if(res < 0){
    printf("res < 0 \n");
    return 0;
  }

  //res = CALL(ctx, write, session, sendbuf, len);

  //clock_wait(0.1*CLOCK_SECOND);
  /* Guess number of bytes application data actually sent:
   * dtls_prepare_record() tells us in len the number of bytes to
   * send, res will contain the bytes actually sent. */
  return res <= 0 ? res : overall_len - (len - res);

}

static int
dtls_prepare_record_x(dtls_peer_t *peer, dtls_security_parameters_t *security,
		    unsigned char type,
		    uint8_t* data_array[], size_t data_len_array[],
		    size_t data_array_len,
		    uint8_t* sendbuf, size_t *rlen) {

  uint8_t *p, *start;
  int res;
  unsigned int i;
  if (*rlen < DTLS_RH_LENGTH) {
    dtls_alert("The sendbuf (%zu bytes) is too small\n", *rlen);
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  p = dtls_set_record_header(type, security, sendbuf);
  start = p;

  if (!security || security->cipher == TLS_NULL_WITH_NULL_NULL) {
    /* no cipher suite */

    res = 0;
    for (i = 0; i < data_array_len; i++) {
      /* check the minimum that we need for packets that are not encrypted */
      if (*rlen < res + DTLS_RH_LENGTH + data_len_array[i]) {
        dtls_debug("dtls_prepare_record: send buffer too small\n");
        return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
      }

      memcpy(p, data_array[i], data_len_array[i]);
      p += data_len_array[i];
      res += data_len_array[i];
    }
  } else { /* TLS_PSK_WITH_AES_128_CCM_8 or TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8 */
    /**
     * length of additional_data for the AEAD cipher which consists of
     * seq_num(2+6) + type(1) + version(2) + length(2)
     */
#define A_DATA_LEN 13
    unsigned char nonce[DTLS_CCM_BLOCKSIZE];
    unsigned char A_DATA[A_DATA_LEN];

    memcpy(p, &DTLS_RECORD_HEADER(sendbuf)->epoch, 8);
    p += 8;
    res = 8;

    for (i = 0; i < data_array_len; i++) {
      /* check the minimum that we need for packets that are not encrypted */
      if (*rlen < res + DTLS_RH_LENGTH + data_len_array[i]) {
        dtls_debug("dtls_prepare_record: send buffer too small\n");
        return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
      }

      memcpy(p, data_array[i], data_len_array[i]);
      p += data_len_array[i];
      res += data_len_array[i];
    }

    memset(nonce, 0, DTLS_CCM_BLOCKSIZE);
    memcpy(nonce, dtls_kb_local_iv(security, peer->role),
	   dtls_kb_iv_size(security, peer->role));
    memcpy(nonce + dtls_kb_iv_size(security, peer->role), start, 8); /* epoch + seq_num */
    dtls_debug_dump("..iv", dtls_kb_local_iv(security, peer->role), 4);
    dtls_debug_dump("..epoch+seq:", start, 8);
    dtls_debug_dump("..nonce:", nonce, DTLS_CCM_BLOCKSIZE);
    dtls_debug_dump("key:", dtls_kb_local_write_key(security, peer->role),
	  dtls_kb_key_size(security, peer->role));

    /* re-use N to create additional data according to RFC 5246, Section 6.2.3.3:
     *
     * additional_data = seq_num + TLSCompressed.type +
     *                   TLSCompressed.version + TLSCompressed.length;
     */
    memcpy(A_DATA, &DTLS_RECORD_HEADER(sendbuf)->epoch, 8); /* epoch and seq_num */
    memcpy(A_DATA + 8,  &DTLS_RECORD_HEADER(sendbuf)->content_type, 3); /* type and version */
    dtls_int_to_uint16(A_DATA + 11, res - 8); /* length */
  //    int rtimer_c = rtimer_arch_now();
#if AES_CRYPTO_STATS
    powertrace_print("START ENCRYPT");
#endif
    res = dtls_encrypt(start + 8, res - 8, start + 8, nonce,
		       dtls_kb_local_write_key(security, peer->role),
		       dtls_kb_key_size(security, peer->role),
		       A_DATA, A_DATA_LEN);
#if AES_CRYPTO_STATS
    powertrace_print("END ENCRYPT");
#endif
    //rtimer_c = rtimer_arch_now() - rtimer_c;
  //  printf("rtimer_c :%d\n",rtimer_c);
    if (res < 0)
      return res;

    res += 8;			/* increment res by size of nonce_explicit */
    dtls_debug_dump("message:", start, res);
  }
  /* fix length of fragment in sendbuf */
  dtls_int_to_uint16(sendbuf + 11, res);
  dtls_debug_hexdump("prepare packet",sendbuf,13+8);

  *rlen = DTLS_RH_LENGTH + res;
  return 0;
}

int
create_virtual_peer(dtls_context_t *ctx, session_t *sess,unsigned char *psk_id, size_t len){
	//create peer
	dtls_peer_t *peer;
  memset(sess,0,sizeof(session_t));
  uip_ipaddr_t ipaddr;
  uip_ipaddr(&ipaddr, 127,0,0,1);
  sess -> addr = ipaddr;
	peer = dtls_new_peer(sess);

	if(!peer){
		dtls_crit("cannot create new peer\n");
    return -1;
	}

	peer-> role = DTLS_SERVER;
	peer-> handshake_params = dtls_handshake_new();
	peer-> state = DTLS_STATE_CONNECTED;

	dtls_cipher_t newchiper = TLS_PSK_WITH_AES_128_CCM_8;
	dtls_handshake_parameters_t *handshake = peer->handshake_params;
	handshake->cipher = newchiper;
  //handshake->compression = TLS_COMPRESSION_NULL;

	dtls_security_parameters_t *security = dtls_security_params(peer);
	security->cipher = newchiper;
	security->epoch = 1;
	security->rseq = 1;
	security->compression = TLS_COMPRESSION_NULL;

	handshake -> keyx.psk.id_length = len;
  memcpy(handshake->keyx.psk.identity, psk_id, len);
  dtls_add_peer(ctx, peer);
  //calculate_key_block_self(ctx,sess);
  //dtls_security_params_switch(peer);

  return 0;
}



/**
 * Initializes \p buf as handshake header. The caller must ensure that \p
 * buf is capable of holding at least \c sizeof(dtls_handshake_header_t)
 * bytes. Increments message sequence number counter of \p peer.
 * \return pointer to the next byte after \p buf
 */ 
static inline uint8_t *
dtls_set_handshake_header(uint8_t type, dtls_peer_t *peer, 
			  int length, 
			  int frag_offset, int frag_length, 
			  uint8_t *buf) {
  
  dtls_int_to_uint8(buf, type);
  buf += sizeof(uint8_t);

  dtls_int_to_uint24(buf, length);
  buf += 3; /* 24 bits */

  if (peer && peer->handshake_params) {
    /* and copy the result to buf */
    dtls_int_to_uint16(buf, peer->handshake_params->hs_state.mseq_s);

    /* increment handshake message sequence counter by 1 */
    peer->handshake_params->hs_state.mseq_s++;
  } else {
    memset(buf, 0, sizeof(uint16_t));
  }
  buf += sizeof(uint16_t);
  
  dtls_int_to_uint24(buf, frag_offset);
  buf += 3; /* 24 bits */

  dtls_int_to_uint24(buf, frag_length);
  buf += 3; /* 24 bits */
  
  return buf;
}

/** only one compression method is currently defined */
static uint8_t compression_methods[] = {
  TLS_COMPRESSION_NULL
};

/** returns true if the cipher matches TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8 */
static inline int is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(dtls_cipher_t cipher)
{
#if DTLS_ECC
  return cipher == TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8;
#else
  return 0;
#endif /* DTLS_ECC */
}

/** returns true if the cipher matches TLS_PSK_WITH_AES_128_CCM_8 */
static inline int is_tls_psk_with_aes_128_ccm_8(dtls_cipher_t cipher)
{
#ifdef DTLS_PSK
  return cipher == TLS_PSK_WITH_AES_128_CCM_8;
#else
  return 0;
#endif /* DTLS_PSK */
}

/** returns true if the application is configured for psk */
static inline int is_psk_supported(dtls_context_t *ctx)
{
#ifdef DTLS_PSK
  return ctx && ctx->h && ctx->h->get_psk_info;
#else
  return 0;
#endif /* DTLS_PSK */
}

/** returns true if the application is configured for ecdhe_ecdsa */
static inline int is_ecdsa_supported(dtls_context_t *ctx, int is_client)
{
#if DTLS_ECC
  return ctx && ctx->h && ((!is_client && ctx->h->get_ecdsa_key) || 
			   (is_client && ctx->h->verify_ecdsa_key));
#else
  return 0;
#endif /* DTLS_ECC */
}

/** Returns true if the application is configured for ecdhe_ecdsa with
  * client authentication */
static inline int is_ecdsa_client_auth_supported(dtls_context_t *ctx)
{
#if DTLS_ECC
  return ctx && ctx->h && ctx->h->get_ecdsa_key && ctx->h->verify_ecdsa_key;
#else
  return 0;
#endif /* DTLS_ECC */
}

/**
 * Returns @c 1 if @p code is a cipher suite other than @c
 * TLS_NULL_WITH_NULL_NULL that we recognize.
 *
 * @param ctx   The current DTLS context
 * @param code The cipher suite identifier to check
 * @param is_client 1 for a dtls client, 0 for server
 * @return @c 1 iff @p code is recognized,
 */ 
static int
known_cipher(dtls_context_t *ctx, dtls_cipher_t code, int is_client) {
  int psk;
  int ecdsa;

  psk = is_psk_supported(ctx);
  ecdsa = is_ecdsa_supported(ctx, is_client);
  return (psk && is_tls_psk_with_aes_128_ccm_8(code)) ||
	 (ecdsa && is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(code));
}

/**
 * This method detects if we already have a established DTLS session with
 * peer and the peer is attempting to perform a fresh handshake by sending
 * messages with epoch = 0. This is to handle situations mentioned in
 * RFC 6347 - section 4.2.8.
 *
 * @param msg  The packet received from Client
 * @param msglen Packet length
 * @param peer peer who is the sender for this packet
 * @return @c 1 if this is a rehandshake attempt by
 * client
 */
static int
hs_attempt_with_existing_peer(uint8_t *msg, size_t msglen,
    dtls_peer_t *peer)
{
    if ((peer) && (peer->state == DTLS_STATE_CONNECTED)) {
      if (msg[0] == DTLS_CT_HANDSHAKE) {
        uint16_t msg_epoch = dtls_uint16_to_int(DTLS_RECORD_HEADER(msg)->epoch);
        if (msg_epoch == 0) {
          dtls_handshake_header_t * hs_header = DTLS_HANDSHAKE_HEADER(msg + DTLS_RH_LENGTH);
          if (hs_header->msg_type == DTLS_HT_CLIENT_HELLO ||
              hs_header->msg_type == DTLS_HT_HELLO_REQUEST) {
            return 1;
          }
        }
      }
    }
    return 0;
}
/** Dump out the cipher keys and IVs used for the symetric cipher. */
static inline void
dtls_debug_keyblock(dtls_security_parameters_t *config, dtls_peer_t *peer)
{
  if(!peer) {
    return;
  }

  dtls_debug("key_block (%d bytes):\n", dtls_kb_size(config, peer->role));
  dtls_debug_dump("  client_MAC_secret",
		  dtls_kb_client_mac_secret(config, peer->role),
		  dtls_kb_mac_secret_size(config, peer->role));

  dtls_debug_dump("  server_MAC_secret",
		  dtls_kb_server_mac_secret(config, peer->role),
		  dtls_kb_mac_secret_size(config, peer->role));

  dtls_debug_dump("  client_write_key",
		  dtls_kb_client_write_key(config, peer->role),
		  dtls_kb_key_size(config, peer->role));

  dtls_debug_dump("  server_write_key",
		  dtls_kb_server_write_key(config, peer->role),
		  dtls_kb_key_size(config, peer->role));

  dtls_debug_dump("  client_IV",
		  dtls_kb_client_iv(config, peer->role),
		  dtls_kb_iv_size(config, peer->role));

  dtls_debug_dump("  server_IV",
		  dtls_kb_server_iv(config, peer->role),
		  dtls_kb_iv_size(config, peer->role));
}

int
calculate_key_block_self(dtls_context_t *ctx,
		    session_t *sess){

  dtls_peer_t * peer = dtls_get_peer(ctx,sess);
  dtls_handshake_parameters_t *handshake = peer->handshake_params;
  dtls_security_parameters_t *security = dtls_security_params_next(peer);

  unsigned char *pre_master_secret;
  int pre_master_len = 0;
  uint8_t master_secret[DTLS_MASTER_SECRET_LENGTH];

  if (!security) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  pre_master_secret = security->key_block;

  if(handshake->cipher == TLS_PSK_WITH_AES_128_CCM_8){
    unsigned char psk[DTLS_PSK_MAX_KEY_LEN];
    int len;

    len = CALL(ctx, get_psk_info, sess, DTLS_PSK_KEY,
	       handshake->keyx.psk.identity,
	       handshake->keyx.psk.id_length,
	       psk, DTLS_PSK_MAX_KEY_LEN);

    if (len < 0) {
      dtls_crit("no psk key for session available\n");
      return len;
    }
  /* Temporarily use the key_block storage space for the pre master secret. */
    pre_master_len = dtls_psk_pre_master_secret(psk, len,
						pre_master_secret,
						MAX_KEYBLOCK_LENGTH);

    dtls_debug_hexdump("psk", psk, len);

    memset(psk, 0, DTLS_PSK_MAX_KEY_LEN);
    if (pre_master_len < 0) {
      dtls_crit("the psk was too long, for the pre master secret\n");
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }

  } else {
    dtls_crit("calculate_key_block: unknown cipher\n");
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  dtls_debug_dump("client_random", handshake->tmp.random.client, DTLS_RANDOM_LENGTH);
  dtls_debug_dump("server_random", handshake->tmp.random.server, DTLS_RANDOM_LENGTH);
  dtls_debug_dump("pre_master_secret", pre_master_secret, pre_master_len);

  #ifdef CUSTOM_PRF
    dtls_prf_custom(pre_master_secret, pre_master_len,
             PRF_LABEL(master), PRF_LABEL_SIZE(master),
             handshake->tmp.random.client, DTLS_RANDOM_LENGTH,
             handshake->tmp.random.server, DTLS_RANDOM_LENGTH,
             master_secret,
             DTLS_MASTER_SECRET_LENGTH);
  #else
    dtls_prf(pre_master_secret, pre_master_len,
             PRF_LABEL(master), PRF_LABEL_SIZE(master),
             handshake->tmp.random.client, DTLS_RANDOM_LENGTH,
             handshake->tmp.random.server, DTLS_RANDOM_LENGTH,
             master_secret,
             DTLS_MASTER_SECRET_LENGTH);
  #endif

  dtls_debug_dump("master_secret", master_secret, DTLS_MASTER_SECRET_LENGTH);

  /* create key_block from master_secret
   * key_block = PRF(master_secret,
                    "key expansion" + tmp.random.server + tmp.random.client) */
  #ifdef CUSTOM_PRF
    dtls_prf_custom(master_secret,
            DTLS_MASTER_SECRET_LENGTH,
            PRF_LABEL(key), PRF_LABEL_SIZE(key),
            handshake->tmp.random.server, DTLS_RANDOM_LENGTH,
            handshake->tmp.random.client, DTLS_RANDOM_LENGTH,
            security->key_block,
            dtls_kb_size(security, peer->role));
  #else
    dtls_prf(master_secret,
            DTLS_MASTER_SECRET_LENGTH,
            PRF_LABEL(key), PRF_LABEL_SIZE(key),
            handshake->tmp.random.server, DTLS_RANDOM_LENGTH,
            handshake->tmp.random.client, DTLS_RANDOM_LENGTH,
            security->key_block,
            dtls_kb_size(security, peer->role));
  #endif /*CUSTOM_PRF*/

  memcpy(handshake->tmp.master_secret, master_secret, DTLS_MASTER_SECRET_LENGTH);
  dtls_debug_keyblock(security, peer);
  security->cipher = handshake->cipher;
  security->compression = handshake->compression;
  security->rseq = 1;
  security->epoch = 1;
  dtls_security_params_switch(peer);
  dtls_debug_dump("test iv", dtls_kb_local_iv(security, peer->role), 4);
  dtls_debug_dump("test key:", dtls_kb_local_write_key(security, peer->role),
      dtls_kb_key_size(security, peer->role));
  return 0;
}

/** returns the name of the goven handshake type number.
  * see IANA for a full list of types:
  * https://www.iana.org/assignments/tls-parameters/tls-parameters.xml#tls-parameters-7
  */
static char *dtls_handshake_type_to_name(int type)
{
  switch (type) {
  case DTLS_HT_HELLO_REQUEST:
    return "hello_request";
  case DTLS_HT_CLIENT_HELLO:
    return "client_hello";
  case DTLS_HT_SERVER_HELLO:
    return "server_hello";
  case DTLS_HT_HELLO_VERIFY_REQUEST:
    return "hello_verify_request";
  case DTLS_HT_CERTIFICATE:
    return "certificate";
  case DTLS_HT_SERVER_KEY_EXCHANGE:
    return "server_key_exchange";
  case DTLS_HT_CERTIFICATE_REQUEST:
    return "certificate_request";
  case DTLS_HT_SERVER_HELLO_DONE:
    return "server_hello_done";
  case DTLS_HT_CERTIFICATE_VERIFY:
    return "certificate_verify";
  case DTLS_HT_CLIENT_KEY_EXCHANGE:
    return "client_key_exchange";
  case DTLS_HT_FINISHED:
    return "finished";
  default:
    return "unknown";
  }
}

/**
 * Calculate the pre master secret and after that calculate the master-secret.
 */
static int
calculate_key_block(dtls_context_t *ctx, 
		    dtls_handshake_parameters_t *handshake,
		    dtls_peer_t *peer,
		    session_t *session,
		    dtls_peer_type role) {
  unsigned char *pre_master_secret;
  int pre_master_len = 0;
  dtls_security_parameters_t *security;
  uint8_t master_secret[DTLS_MASTER_SECRET_LENGTH];

  if (!peer || !handshake) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }
  security = dtls_security_params_next(peer);
  if (!security) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  pre_master_secret = security->key_block;

  switch (handshake->cipher) {
#ifdef DTLS_PSK
  case TLS_PSK_WITH_AES_128_CCM_8: {
    unsigned char psk[DTLS_PSK_MAX_KEY_LEN];
    int len;

    len = CALL(ctx, get_psk_info, session, DTLS_PSK_KEY,
	       handshake->keyx.psk.identity,
	       handshake->keyx.psk.id_length,
	       psk, DTLS_PSK_MAX_KEY_LEN);
    if (len < 0) {
      dtls_crit("no psk key for session available\n");
      return len;
    }
  /* Temporarily use the key_block storage space for the pre master secret. */
    pre_master_len = dtls_psk_pre_master_secret(psk, len,
						pre_master_secret,
						MAX_KEYBLOCK_LENGTH);

    dtls_debug_hexdump("psk", psk, len);

    memset(psk, 0, DTLS_PSK_MAX_KEY_LEN);
    if (pre_master_len < 0) {
      dtls_crit("the psk was too long, for the pre master secret\n");
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }

    break;
  }
#endif /* DTLS_PSK */
#if DTLS_ECC
  case TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8: {
    pre_master_len = dtls_ecdh_pre_master_secret(handshake->keyx.ecdsa.own_eph_priv,
						 handshake->keyx.ecdsa.other_eph_pub_x,
						 handshake->keyx.ecdsa.other_eph_pub_y,
						 sizeof(handshake->keyx.ecdsa.own_eph_priv),
						 pre_master_secret,
						 MAX_KEYBLOCK_LENGTH);
    if (pre_master_len < 0) {
      dtls_crit("the curve was too long, for the pre master secret\n");
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }
    break;
  }
#endif /* DTLS_ECC */
  default:
    dtls_crit("calculate_key_block: unknown cipher\n");
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  dtls_debug_dump("client_random", handshake->tmp.random.client, DTLS_RANDOM_LENGTH);
  dtls_debug_dump("server_random", handshake->tmp.random.server, DTLS_RANDOM_LENGTH);
  dtls_debug_dump("pre_master_secret", pre_master_secret, pre_master_len);

  dtls_prf(pre_master_secret, pre_master_len,
	   PRF_LABEL(master), PRF_LABEL_SIZE(master),
	   handshake->tmp.random.client, DTLS_RANDOM_LENGTH,
	   handshake->tmp.random.server, DTLS_RANDOM_LENGTH,
	   master_secret,
	   DTLS_MASTER_SECRET_LENGTH);

  dtls_debug_dump("master_secret", master_secret, DTLS_MASTER_SECRET_LENGTH);

  /* create key_block from master_secret
   * key_block = PRF(master_secret,
                    "key expansion" + tmp.random.server + tmp.random.client) */

  dtls_prf(master_secret,
	   DTLS_MASTER_SECRET_LENGTH,
	   PRF_LABEL(key), PRF_LABEL_SIZE(key),
	   handshake->tmp.random.server, DTLS_RANDOM_LENGTH,
	   handshake->tmp.random.client, DTLS_RANDOM_LENGTH,
	   security->key_block,
	   dtls_kb_size(security, role));

  memcpy(handshake->tmp.master_secret, master_secret, DTLS_MASTER_SECRET_LENGTH);
  dtls_debug_keyblock(security, peer);

  security->cipher = handshake->cipher;
  security->compression = handshake->compression;
  security->rseq = 0;

  return 0;
}

/* TODO: add a generic method which iterates over a list and searches for a specific key */
static int verify_ext_eliptic_curves(uint8_t *data, size_t data_length) {
  int i, curve_name;

  /* length of curve list */
  i = dtls_uint16_to_int(data);
  data += sizeof(uint16_t);
  if (i + sizeof(uint16_t) != data_length) {
    dtls_warn("the list of the supported elliptic curves should be tls extension length - 2\n");
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }

  for (i = data_length - sizeof(uint16_t); i > 0; i -= sizeof(uint16_t)) {
    /* check if this curve is supported */
    curve_name = dtls_uint16_to_int(data);
    data += sizeof(uint16_t);

    if (curve_name == TLS_EXT_ELLIPTIC_CURVES_SECP256R1)
      return 0;
  }

  dtls_warn("no supported elliptic curve found\n");
  return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
}

static int verify_ext_cert_type(uint8_t *data, size_t data_length) {
  int i, cert_type;

  /* length of cert type list */
  i = dtls_uint8_to_int(data);
  data += sizeof(uint8_t);
  if (i + sizeof(uint8_t) != data_length) {
    dtls_warn("the list of the supported certificate types should be tls extension length - 1\n");
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }

  for (i = data_length - sizeof(uint8_t); i > 0; i -= sizeof(uint8_t)) {
    /* check if this cert type is supported */
    cert_type = dtls_uint8_to_int(data);
    data += sizeof(uint8_t);

    if (cert_type == TLS_CERT_TYPE_RAW_PUBLIC_KEY)
      return 0;
  }

  dtls_warn("no supported certificate type found\n");
  return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
}

static int verify_ext_ec_point_formats(uint8_t *data, size_t data_length) {
  int i, cert_type;

  /* length of ec_point_formats list */
  i = dtls_uint8_to_int(data);
  data += sizeof(uint8_t);
  if (i + sizeof(uint8_t) != data_length) {
    dtls_warn("the list of the supported ec_point_formats should be tls extension length - 1\n");
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }

  for (i = data_length - sizeof(uint8_t); i > 0; i -= sizeof(uint8_t)) {
    /* check if this ec_point_format is supported */
    cert_type = dtls_uint8_to_int(data);
    data += sizeof(uint8_t);

    if (cert_type == TLS_EXT_EC_POINT_FORMATS_UNCOMPRESSED)
      return 0;
  }

  dtls_warn("no supported ec_point_format found\n");
  return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
}

/*
 * Check for some TLS Extensions used by the ECDHE_ECDSA cipher.
 */
static int
dtls_check_tls_extension(dtls_peer_t *peer,
			 uint8_t *data, size_t data_length, int client_hello)
{
  uint16_t i, j;
  int ext_elliptic_curve = 0;
  int ext_client_cert_type = 0;
  int ext_server_cert_type = 0;
  int ext_ec_point_formats = 0;
  dtls_handshake_parameters_t *handshake;

  if (!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  handshake = peer->handshake_params;

  if (data_length < sizeof(uint16_t)) { 
    /* no tls extensions specified */
    if (is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(handshake->cipher)) {
      goto error;
    }
    return 0;
  }

  /* get the length of the tls extension list */
  j = dtls_uint16_to_int(data);
  data += sizeof(uint16_t);
  data_length -= sizeof(uint16_t);

  if (data_length < j)
    goto error;

  /* check for TLS extensions needed for this cipher */
  while (data_length) {
    if (data_length < sizeof(uint16_t) * 2)
      goto error;

    /* get the tls extension type */
    i = dtls_uint16_to_int(data);
    data += sizeof(uint16_t);
    data_length -= sizeof(uint16_t);

    /* get the length of the tls extension */
    j = dtls_uint16_to_int(data);
    data += sizeof(uint16_t);
    data_length -= sizeof(uint16_t);

    if (data_length < j)
      goto error;

    switch (i) {
      case TLS_EXT_ELLIPTIC_CURVES:
        ext_elliptic_curve = 1;
        if (verify_ext_eliptic_curves(data, j))
          goto error;
        break;
      case TLS_EXT_CLIENT_CERTIFICATE_TYPE:
        ext_client_cert_type = 1;
        if (client_hello) {
	  if (verify_ext_cert_type(data, j))
            goto error;
        } else {
	  if (dtls_uint8_to_int(data) != TLS_CERT_TYPE_RAW_PUBLIC_KEY)
	    goto error;
        }
        break;
      case TLS_EXT_SERVER_CERTIFICATE_TYPE:
        ext_server_cert_type = 1;
        if (client_hello) {
	  if (verify_ext_cert_type(data, j))
            goto error;
        } else {
	  if (dtls_uint8_to_int(data) != TLS_CERT_TYPE_RAW_PUBLIC_KEY)
	    goto error;
        }
        break;
      case TLS_EXT_EC_POINT_FORMATS:
        ext_ec_point_formats = 1;
        if (verify_ext_ec_point_formats(data, j))
          goto error;
        break;
      case TLS_EXT_ENCRYPT_THEN_MAC:
	/* As only AEAD cipher suites are currently available, this
	 * extension can be skipped. 
	 */
	dtls_info("skipped encrypt-then-mac extension\n");
	break;
      default:
        dtls_warn("unsupported tls extension: %i\n", i);
        break;
    }
    data += j;
    data_length -= j;
  }
  if (is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(handshake->cipher) && client_hello) {
    if (!ext_elliptic_curve || !ext_client_cert_type || !ext_server_cert_type
	|| !ext_ec_point_formats) {
      dtls_warn("not all required tls extensions found in client hello\n");
      goto error;
    }
  } else if (is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(handshake->cipher) && !client_hello) {
    if (!ext_client_cert_type || !ext_server_cert_type) {
      dtls_warn("not all required tls extensions found in server hello\n");
      goto error;
    }
  }
  return 0;

error:
  if (client_hello && peer->state == DTLS_STATE_CONNECTED) {
    return dtls_alert_create(DTLS_ALERT_LEVEL_WARNING, DTLS_ALERT_NO_RENEGOTIATION);
  } else {
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }
}

/**
 * Parses the ClientHello from the client and updates the internal handshake
 * parameters with the new data for the given \p peer. When the ClientHello
 * handshake message in \p data does not contain a cipher suite or
 * compression method, it is copied from the the current security parameters.
 *
 * \param ctx   The current DTLS context.
 * \param peer  The remote peer whose security parameters are about to change.
 * \param data  The handshake message with a ClientHello. 
 * \param data_length The actual size of \p data.
 * \return \c -Something if an error occurred, \c 0 on success.
 */
static int
dtls_update_parameters(dtls_context_t *ctx, 
		       dtls_peer_t *peer,
		       uint8_t *data, size_t data_length) {
  int i, j;
  int ok;
  dtls_handshake_parameters_t *config;
  dtls_security_parameters_t *security;

  if (!peer) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }
  config = peer->handshake_params;
  if (!config) {
    goto error;
  }
  security = dtls_security_params(peer);

  assert(config);
  assert(data_length > DTLS_HS_LENGTH + DTLS_CH_LENGTH);

  /* skip the handshake header and client version information */
  data += DTLS_HS_LENGTH + sizeof(uint16_t);
  data_length -= DTLS_HS_LENGTH + sizeof(uint16_t);

  /* store client random in config */
  memcpy(config->tmp.random.client, data, DTLS_RANDOM_LENGTH);
  data += DTLS_RANDOM_LENGTH;
  data_length -= DTLS_RANDOM_LENGTH;

  /* Caution: SKIP_VAR_FIELD may jump to error: */
  SKIP_VAR_FIELD(data, data_length);	/* skip session id */
  SKIP_VAR_FIELD(data, data_length);	/* skip cookie */

  i = dtls_uint16_to_int(data);
  if (data_length < i + sizeof(uint16_t)) {
    /* Looks like we do not have a cipher nor compression. This is ok
     * for renegotiation, but not for the initial handshake. */

    if (!security || security->cipher == TLS_NULL_WITH_NULL_NULL)
      goto error;

    config->cipher = security->cipher;
    config->compression = security->compression;

    return 0;
  }

  data += sizeof(uint16_t);
  data_length -= sizeof(uint16_t) + i;

  ok = 0;
  while (i && !ok) {
    config->cipher = dtls_uint16_to_int(data);
    ok = known_cipher(ctx, config->cipher, 0);
    i -= sizeof(uint16_t);
    data += sizeof(uint16_t);
  }

  /* skip remaining ciphers */
  data += i;

  if (!ok) {
    /* reset config cipher to a well-defined value */
    config->cipher = TLS_NULL_WITH_NULL_NULL;
    dtls_warn("No matching cipher found\n");
    goto error;
  }

  if (data_length < sizeof(uint8_t)) { 
    /* no compression specified, take the current compression method */
    if (security)
      config->compression = security->compression;
    else
      config->compression = TLS_COMPRESSION_NULL;
    return 0;
  }

  i = dtls_uint8_to_int(data);
  if (data_length < i + sizeof(uint8_t))
    goto error;

  data += sizeof(uint8_t);
  data_length -= sizeof(uint8_t) + i;

  ok = 0;
  while (i && !ok) {
    for (j = 0; j < sizeof(compression_methods) / sizeof(uint8_t); ++j)
      if (dtls_uint8_to_int(data) == compression_methods[j]) {
	config->compression = compression_methods[j];
	ok = 1;
      }
    i -= sizeof(uint8_t);
    data += sizeof(uint8_t);    
  }

  if (!ok) {
    /* reset config cipher to a well-defined value */
    goto error;
  }
  
  return dtls_check_tls_extension(peer, data, data_length, 1);
error:
  if (peer->state == DTLS_STATE_CONNECTED) {
    return dtls_alert_create(DTLS_ALERT_LEVEL_WARNING, DTLS_ALERT_NO_RENEGOTIATION);
  } else {
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }
}

/**
 * Parse the ClientKeyExchange and update the internal handshake state with
 * the new data.
 */
static inline int
check_client_keyexchange(dtls_context_t *ctx, 
			 dtls_handshake_parameters_t *handshake,
			 uint8_t *data, size_t length) {
  if (!handshake) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

#if DTLS_ECC
  if (is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(handshake->cipher)) {

    if (length < DTLS_HS_LENGTH + DTLS_CKXEC_LENGTH) {
      dtls_debug("The client key exchange is too short\n");
      return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
    }
    data += DTLS_HS_LENGTH;

    if (dtls_uint8_to_int(data) != 1 + 2 * DTLS_EC_KEY_SIZE) {
      dtls_alert("expected 65 bytes long public point\n");
      return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
    }
    data += sizeof(uint8_t);

    if (dtls_uint8_to_int(data) != 4) {
      dtls_alert("expected uncompressed public point\n");
      return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
    }
    data += sizeof(uint8_t);

    memcpy(handshake->keyx.ecdsa.other_eph_pub_x, data,
	   sizeof(handshake->keyx.ecdsa.other_eph_pub_x));
    data += sizeof(handshake->keyx.ecdsa.other_eph_pub_x);

    memcpy(handshake->keyx.ecdsa.other_eph_pub_y, data,
	   sizeof(handshake->keyx.ecdsa.other_eph_pub_y));
    data += sizeof(handshake->keyx.ecdsa.other_eph_pub_y);
  }
#endif /* DTLS_ECC */
#ifdef DTLS_PSK
  if (is_tls_psk_with_aes_128_ccm_8(handshake->cipher)) {
    int id_length;

    if (length < DTLS_HS_LENGTH + DTLS_CKXPSK_LENGTH_MIN) {
      dtls_debug("The client key exchange is too short\n");
      return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
    }
    data += DTLS_HS_LENGTH;

    id_length = dtls_uint16_to_int(data);
    data += sizeof(uint16_t);

    if (DTLS_HS_LENGTH + DTLS_CKXPSK_LENGTH_MIN + id_length != length) {
      dtls_debug("The identity has a wrong length\n");
      return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
    }

    if (id_length > DTLS_PSK_MAX_CLIENT_IDENTITY_LEN) {
      dtls_warn("please use a smaller client identity\n");
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }

    handshake->keyx.psk.id_length = id_length;
    memcpy(handshake->keyx.psk.identity, data, id_length);
  }
#endif /* DTLS_PSK */
  return 0;
}

static inline void
update_hs_hash(dtls_peer_t *peer, uint8_t *data, size_t length) {
  dtls_debug_dump("add MAC data", data, length);
  dtls_hash_update(&peer->handshake_params->hs_state.hs_hash, data, length);
}

static void
copy_hs_hash(dtls_peer_t *peer, dtls_hash_ctx *hs_hash) {
  memcpy(hs_hash, &peer->handshake_params->hs_state.hs_hash,
	 sizeof(peer->handshake_params->hs_state.hs_hash));
}

static inline size_t
finalize_hs_hash(dtls_peer_t *peer, uint8_t *buf) {
  return dtls_hash_finalize(buf, &peer->handshake_params->hs_state.hs_hash);
}

static inline void
clear_hs_hash(dtls_peer_t *peer) {
  assert(peer);
  dtls_debug("clear MAC\n");
  dtls_hash_init(&peer->handshake_params->hs_state.hs_hash);
}

/** 
 * Checks if \p record + \p data contain a Finished message with valid
 * verify_data. 
 *
 * \param ctx    The current DTLS context.
 * \param peer   The remote peer of the security association.
 * \param data   The cleartext payload of the message.
 * \param data_length Actual length of \p data.
 * \return \c 0 if the Finished message is valid, \c negative number otherwise.
 */
static int
check_finished(dtls_context_t *ctx, dtls_peer_t *peer,
	       uint8_t *data, size_t data_length) {
  size_t digest_length, label_size;
  const unsigned char *label;
  unsigned char buf[DTLS_HMAC_MAX];

  if (data_length < DTLS_HS_LENGTH + DTLS_FIN_LENGTH)
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);

  if (!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  /* Use a union here to ensure that sufficient stack space is
   * reserved. As statebuf and verify_data are not used at the same
   * time, we can re-use the storage safely.
   */
  union {
    unsigned char statebuf[DTLS_HASH_CTX_SIZE];
    unsigned char verify_data[DTLS_FIN_LENGTH];
  } b;

  /* temporarily store hash status for roll-back after finalize */
  memcpy(b.statebuf, &peer->handshake_params->hs_state.hs_hash, DTLS_HASH_CTX_SIZE);

  digest_length = finalize_hs_hash(peer, buf);
  /* clear_hash(); */

  /* restore hash status */
  memcpy(&peer->handshake_params->hs_state.hs_hash, b.statebuf, DTLS_HASH_CTX_SIZE);

  if (peer->role == DTLS_CLIENT) {
    label = PRF_LABEL(server);
    label_size = PRF_LABEL_SIZE(server);
  } else { /* server */
    label = PRF_LABEL(client);
    label_size = PRF_LABEL_SIZE(client);
  }

  dtls_prf(peer->handshake_params->tmp.master_secret,
	   DTLS_MASTER_SECRET_LENGTH,
	   label, label_size,
	   PRF_LABEL(finished), PRF_LABEL_SIZE(finished),
	   buf, digest_length,
	   b.verify_data, sizeof(b.verify_data));

  dtls_debug_dump("d:", data + DTLS_HS_LENGTH, sizeof(b.verify_data));
  dtls_debug_dump("v:", b.verify_data, sizeof(b.verify_data));

  /* compare verify data and create DTLS alert code when they differ */
  return dtls_equals(data + DTLS_HS_LENGTH, b.verify_data, sizeof(b.verify_data))
    ? 0
    : dtls_alert_create(DTLS_ALERT_LEVEL_FATAL, DTLS_ALERT_HANDSHAKE_FAILURE);
}

/**
 * Prepares the payload given in \p data for sending with
 * dtls_send(). The \p data is encrypted and compressed according to
 * the current security parameters of \p peer.  The result of this
 * operation is put into \p sendbuf with a prepended record header of
 * type \p type ready for sending. As some cipher suites add a MAC
 * before encryption, \p data must be large enough to hold this data
 * as well (usually \c dtls_kb_digest_size(CURRENT_CONFIG(peer)).
 *
 * \param peer    The remote peer the packet will be sent to.
 * \param security  The encryption paramater used to encrypt
 * \param type    The content type of this record.
 * \param data_array Array with payloads in correct order.
 * \param data_len_array sizes of the payloads in correct order.
 * \param data_array_len The number of payloads given.
 * \param sendbuf The output buffer where the encrypted record
 *                will be placed.
 * \param rlen    This parameter must be initialized with the 
 *                maximum size of \p sendbuf and will be updated
 *                to hold the actual size of the stored packet
 *                on success. On error, the value of \p rlen is
 *                undefined. 
 * \return Less than zero on error, or greater than zero success.
 */
static int
dtls_prepare_record(dtls_peer_t *peer, dtls_security_parameters_t *security,
		    unsigned char type,
		    uint8_t *data_array[], size_t data_len_array[],
		    size_t data_array_len,
		    uint8_t *sendbuf, size_t *rlen) {
  uint8_t *p, *start;
  int res;
  unsigned int i;
  
  if (*rlen < DTLS_RH_LENGTH) {
    dtls_alert("The sendbuf (%zu bytes) is too small\n", *rlen);
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  p = dtls_set_record_header(type, security, sendbuf);
  start = p;

  if (!security || security->cipher == TLS_NULL_WITH_NULL_NULL) {
    /* no cipher suite */

    res = 0;
    for (i = 0; i < data_array_len; i++) {
      /* check the minimum that we need for packets that are not encrypted */
      if (*rlen < res + DTLS_RH_LENGTH + data_len_array[i]) {
        dtls_debug("dtls_prepare_record: send buffer too small\n");
        return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
      }

      memcpy(p, data_array[i], data_len_array[i]);
      p += data_len_array[i];
      res += data_len_array[i];
    }
  } else { /* TLS_PSK_WITH_AES_128_CCM_8 or TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8 */
    /** 
     * length of additional_data for the AEAD cipher which consists of
     * seq_num(2+6) + type(1) + version(2) + length(2)
     */
#define A_DATA_LEN 13
    unsigned char nonce[DTLS_CCM_BLOCKSIZE];
    unsigned char A_DATA[A_DATA_LEN];

    if(!peer) {
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }

    if (is_tls_psk_with_aes_128_ccm_8(security->cipher)) {
      dtls_debug("dtls_prepare_record(): encrypt using TLS_PSK_WITH_AES_128_CCM_8\n");
    } else if (is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(security->cipher)) {
      dtls_debug("dtls_prepare_record(): encrypt using TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8\n");
    } else {
      dtls_debug("dtls_prepare_record(): encrypt using unknown cipher\n");
    }

    /* set nonce       
       from RFC 6655:
   	The "nonce" input to the AEAD algorithm is exactly that of [RFC5288]:
   	the "nonce" SHALL be 12 bytes long and is constructed as follows:
   	(this is an example of a "partially explicit" nonce; see Section
   	3.2.1 in [RFC5116]).

                       struct {
             opaque salt[4];
             opaque nonce_explicit[8];
                       } CCMNonce;

         [...]

  	 In DTLS, the 64-bit seq_num is the 16-bit epoch concatenated with the
   	 48-bit seq_num.

   	 When the nonce_explicit is equal to the sequence number, the CCMNonce
   	 will have the structure of the CCMNonceExample given below.

   	            struct {
   	             uint32 client_write_IV; // low order 32-bits
   	             uint64 seq_num;         // TLS sequence number
   	            } CCMClientNonce.


   	            struct {
   	             uint32 server_write_IV; // low order 32-bits
   	             uint64 seq_num; // TLS sequence number
   	            } CCMServerNonce.


   	            struct {
   	             case client:
   	               CCMClientNonce;
   	             case server:
   	               CCMServerNonce:
   	            } CCMNonceExample;
    */

    memcpy(p, &DTLS_RECORD_HEADER(sendbuf)->epoch, 8);
    p += 8;
    res = 8;

    for (i = 0; i < data_array_len; i++) {
      /* check the minimum that we need for packets that are not encrypted */
      if (*rlen < res + DTLS_RH_LENGTH + data_len_array[i]) {
        dtls_debug("dtls_prepare_record: send buffer too small\n");
        return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
      }

      memcpy(p, data_array[i], data_len_array[i]);
      p += data_len_array[i];
      res += data_len_array[i];
    }

    memset(nonce, 0, DTLS_CCM_BLOCKSIZE);
    memcpy(nonce, dtls_kb_local_iv(security, peer->role),
	   dtls_kb_iv_size(security, peer->role));
    memcpy(nonce + dtls_kb_iv_size(security, peer->role), start, 8); /* epoch + seq_num */

    dtls_debug_dump("nonce:", nonce, DTLS_CCM_BLOCKSIZE);
    dtls_debug_dump("key:", dtls_kb_local_write_key(security, peer->role),
		    dtls_kb_key_size(security, peer->role));
    
    /* re-use N to create additional data according to RFC 5246, Section 6.2.3.3:
     * 
     * additional_data = seq_num + TLSCompressed.type +
     *                   TLSCompressed.version + TLSCompressed.length;
     */
    memcpy(A_DATA, &DTLS_RECORD_HEADER(sendbuf)->epoch, 8); /* epoch and seq_num */
    memcpy(A_DATA + 8,  &DTLS_RECORD_HEADER(sendbuf)->content_type, 3); /* type and version */
    dtls_int_to_uint16(A_DATA + 11, res - 8); /* length */
    
    res = dtls_encrypt(start + 8, res - 8, start + 8, nonce,
		       dtls_kb_local_write_key(security, peer->role),
		       dtls_kb_key_size(security, peer->role),
		       A_DATA, A_DATA_LEN);

    if (res < 0)
      return res;

    res += 8;			/* increment res by size of nonce_explicit */
    dtls_debug_dump("message:", start, res);
  }

  /* fix length of fragment in sendbuf */
  dtls_int_to_uint16(sendbuf + 11, res);
  
  *rlen = DTLS_RH_LENGTH + res;
  return 0;
}

static int
dtls_send_handshake_msg_hash(dtls_context_t *ctx,
			     dtls_peer_t *peer,
			     session_t *session,
			     uint8_t header_type,
			     uint8_t *data, size_t data_length,
			     int add_hash)
{
  uint8_t buf[DTLS_HS_LENGTH];
  uint8_t *data_array[2];
  size_t data_len_array[2];
  int i = 0;
  dtls_security_parameters_t *security = peer ? dtls_security_params(peer) : NULL;

  dtls_set_handshake_header(header_type, peer, data_length, 0,
			    data_length, buf);

  if (add_hash && peer && peer->handshake_params) {
    update_hs_hash(peer, buf, sizeof(buf));
  }
  data_array[i] = buf;
  data_len_array[i] = sizeof(buf);
  i++;

  if (data != NULL) {
    if (add_hash && peer && peer->handshake_params) {
      update_hs_hash(peer, data, data_length);
    }
    data_array[i] = data;
    data_len_array[i] = data_length;
    i++;
  }
  dtls_debug("send handshake packet of type: %s (%i)\n",
	     dtls_handshake_type_to_name(header_type), header_type);
  return dtls_send_multi(ctx, peer, security, session, DTLS_CT_HANDSHAKE,
			 data_array, data_len_array, i);
}

static int
dtls_send_handshake_msg(dtls_context_t *ctx,
			dtls_peer_t *peer,
			uint8_t header_type,
			uint8_t *data, size_t data_length)
{
  if (!peer) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }
  return dtls_send_handshake_msg_hash(ctx, peer, &peer->session,
				      header_type, data, data_length, 1);
}

/** 
 * Returns true if the message @p Data is a handshake message that
 * must be included in the calculation of verify_data in the Finished
 * message.
 * 
 * @param Type The message type. Only handshake messages but the initial 
 * Client Hello and Hello Verify Request are included in the hash,
 * @param Data The PDU to examine.
 * @param Length The length of @p Data.
 * 
 * @return @c 1 if @p Data must be included in hash, @c 0 otherwise.
 *
 * @hideinitializer
 */
#define MUST_HASH(Type, Data, Length)					\
  ((Type) == DTLS_CT_HANDSHAKE &&					\
   ((Data) != NULL) && ((Length) > 0)  &&				\
   ((Data)[0] != DTLS_HT_HELLO_VERIFY_REQUEST) &&			\
   ((Data)[0] != DTLS_HT_CLIENT_HELLO ||				\
    ((Length) >= HS_HDR_LENGTH &&					\
     (dtls_uint16_to_int(DTLS_RECORD_HEADER(Data)->epoch > 0) ||	\
      (dtls_uint16_to_int(HANDSHAKE(Data)->message_seq) > 0)))))

/**
 * Sends the data passed in @p buf as a DTLS record of type @p type to
 * the given peer. The data will be encrypted and compressed according
 * to the security parameters for @p peer.
 *
 * @param ctx    The DTLS context in effect.
 * @param peer   The remote party where the packet is sent.
 * @param type   The content type of this record.
 * @param buf    The data to send.
 * @param buflen The number of bytes to send from @p buf.
 * @return Less than zero in case of an error or the number of
 *   bytes that have been sent otherwise.
 */
static int
dtls_send_multi(dtls_context_t *ctx, dtls_peer_t *peer,
		dtls_security_parameters_t *security , session_t *session,
		unsigned char type, uint8_t *buf_array[],
		size_t buf_len_array[], size_t buf_array_len)
{
  /* We cannot use ctx->sendbuf here as it is reserved for collecting
   * the input for this function, i.e. buf == ctx->sendbuf.
   *
   * TODO: check if we can use the receive buf here. This would mean
   * that we might not be able to handle multiple records stuffed in
   * one UDP datagram */
  unsigned char sendbuf[DTLS_MAX_BUF];
  size_t len = sizeof(sendbuf);
  int res;
  unsigned int i;
  size_t overall_len = 0;

  res = dtls_prepare_record(peer, security, type, buf_array, buf_len_array, buf_array_len, sendbuf, &len);

  if (res < 0)
    return res;

  /* if (peer && MUST_HASH(peer, type, buf, buflen)) */
  /*   update_hs_hash(peer, buf, buflen); */

  dtls_debug_hexdump("send header", sendbuf, sizeof(dtls_record_header_t));
  for (i = 0; i < buf_array_len; i++) {
    dtls_debug_hexdump("send unencrypted", buf_array[i], buf_len_array[i]);
    overall_len += buf_len_array[i];
  }

  if ((type == DTLS_CT_HANDSHAKE && buf_array[0][0] != DTLS_HT_HELLO_VERIFY_REQUEST) ||
      type == DTLS_CT_CHANGE_CIPHER_SPEC) {
    /* copy handshake messages other than HelloVerify into retransmit buffer */
    netq_t *n = netq_node_new(overall_len);
    if (n) {
      dtls_tick_t now;
      dtls_ticks(&now);
      n->t = now + 2 * DTLS_TICKS_PER_SECOND;
      n->retransmit_cnt = 0;
      n->timeout = 2 * DTLS_TICKS_PER_SECOND;
      n->peer = peer;
      n->epoch = (security) ? security->epoch : 0;
      n->type = type;
      n->length = 0;
      for (i = 0; i < buf_array_len; i++) {
        memcpy(n->data + n->length, buf_array[i], buf_len_array[i]);
        n->length += buf_len_array[i];
      }

      if (!netq_insert_node(&ctx->sendqueue, n)) {
	dtls_warn("cannot add packet to retransmit buffer\n");
	netq_node_free(n);
      } else {
        dtls_set_retransmit_timer(ctx, n->timeout);
	dtls_debug("copied to sendqueue\n");
      }
    } else 
      dtls_warn("retransmit buffer full\n");
  }

  /* FIXME: copy to peer's sendqueue (after fragmentation if
   * necessary) and initialize retransmit timer */
  res = CALL(ctx, write, session, sendbuf, len);

  /* Guess number of bytes application data actually sent:
   * dtls_prepare_record() tells us in len the number of bytes to
   * send, res will contain the bytes actually sent. */
  return res <= 0 ? res : overall_len - (len - res);
}

static inline int
dtls_send_alert(dtls_context_t *ctx, dtls_peer_t *peer, dtls_alert_level_t level,
		dtls_alert_t description) {
  uint8_t msg[] = { level, description };

  dtls_send(ctx, peer, DTLS_CT_ALERT, msg, sizeof(msg));
  return 0;
}

int 
dtls_close(dtls_context_t *ctx, const session_t *remote) {
  int res = -1;
  dtls_peer_t *peer;

  peer = dtls_get_peer(ctx, remote);

  if (peer) {
    res = dtls_send_alert(ctx, peer, DTLS_ALERT_LEVEL_FATAL, DTLS_ALERT_CLOSE_NOTIFY);
    /* indicate tear down */
    peer->state = DTLS_STATE_CLOSING;
  }
  return res;
}

static void dtls_destroy_peer(dtls_context_t *ctx, dtls_peer_t *peer, int unlink)
{
  if (ctx == NULL || peer == NULL) {
    return;
  }
  if (peer->state != DTLS_STATE_CLOSED && peer->state != DTLS_STATE_CLOSING)
    dtls_close(ctx, &peer->session);
  if (unlink) {
    delete_peer(&ctx->peers, peer);
    dtls_debug_session("removed peer", &peer->session);
  }
  dtls_free_peer(peer);
}

/**
 * Checks a received Client Hello message for a valid cookie. When the
 * Client Hello contains no cookie, the function fails and a Hello
 * Verify Request is sent to the peer (using the write callback function
 * registered with \p ctx). The return value is \c -1 on error, \c 0 when
 * undecided, and \c 1 if the Client Hello was good. 
 * 
 * \param ctx     The DTLS context.
 * \param peer    The remote party we are talking to, if any.
 * \param session Transport address of the remote peer.
 * \param state   Current state of the connection.
 * \param msg     The received datagram.
 * \param msglen  Length of \p msg.
 * \return \c 1 if msg is a Client Hello with a valid cookie, \c 0 or
 * \c -1 otherwise.
 */
static int
dtls_verify_peer(dtls_context_t *ctx, 
		 dtls_peer_t *peer, 
		 session_t *session,
		 const dtls_state_t state,
		 uint8_t *data, size_t data_length)
{
  uint8_t buf[DTLS_HV_LENGTH + DTLS_COOKIE_LENGTH];
  uint8_t *p = buf;
  int len = DTLS_COOKIE_LENGTH;
  uint8_t *cookie = NULL;
  int err;
#undef mycookie
#define mycookie (buf + DTLS_HV_LENGTH)

  /* Store cookie where we can reuse it for the HelloVerify request. */
  err = dtls_create_cookie(ctx, session, data, data_length, mycookie, &len);
  if (err < 0)
    return err;

  dtls_debug_dump("create cookie", mycookie, len);

  assert(len == DTLS_COOKIE_LENGTH);
    
  /* Perform cookie check. */
  len = dtls_get_cookie(data, data_length, &cookie);
  if (len < 0) {
    dtls_warn("error while fetching the cookie, err: %i\n", err);
    return err;
  }

  dtls_debug_dump("compare with cookie", cookie, len);

  /* check if cookies match */
  if (len == DTLS_COOKIE_LENGTH && memcmp(cookie, mycookie, len) == 0) {
    dtls_debug("found matching cookie\n");
    return 0;
  }

  if (len > 0) {
    dtls_debug_dump("invalid cookie", cookie, len);
  } else {
    dtls_debug("cookie len is 0!\n");
  }

  /* ClientHello did not contain any valid cookie, hence we send a
   * HelloVerify request. */

  dtls_int_to_uint16(p, DTLS_VERSION);
  p += sizeof(uint16_t);

  dtls_int_to_uint8(p, DTLS_COOKIE_LENGTH);
  p += sizeof(uint8_t);

  assert(p == mycookie);

  p += DTLS_COOKIE_LENGTH;

  /* TODO use the same record sequence number as in the ClientHello,
     see 4.2.1. Denial-of-Service Countermeasures */
  err = dtls_send_handshake_msg_hash(ctx,
		     state == DTLS_STATE_CONNECTED ? peer : NULL,
		     session,
		     DTLS_HT_HELLO_VERIFY_REQUEST,
		     buf, p - buf, 0);
  if (err < 0) {
    dtls_warn("cannot send HelloVerify request\n");
  }
  return err; /* HelloVerify is sent, now we cannot do anything but wait */

#undef mycookie
}

#if DTLS_ECC
static int
dtls_check_ecdsa_signature_elem(uint8_t *data, size_t data_length,
				unsigned char **result_r,
				unsigned char **result_s)
{
  int i;
  uint8_t *data_orig = data;

  if (dtls_uint8_to_int(data) != TLS_EXT_SIG_HASH_ALGO_SHA256) {
    dtls_alert("only sha256 is supported in certificate verify\n");
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }
  data += sizeof(uint8_t);
  data_length -= sizeof(uint8_t);

  if (dtls_uint8_to_int(data) != TLS_EXT_SIG_HASH_ALGO_ECDSA) {
    dtls_alert("only ecdsa signature is supported in client verify\n");
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }
  data += sizeof(uint8_t);
  data_length -= sizeof(uint8_t);

  if (data_length < dtls_uint16_to_int(data)) {
    dtls_alert("signature length wrong\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }
  data += sizeof(uint16_t);
  data_length -= sizeof(uint16_t);

  if (dtls_uint8_to_int(data) != 0x30) {
    dtls_alert("wrong ASN.1 struct, expected SEQUENCE\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }
  data += sizeof(uint8_t);
  data_length -= sizeof(uint8_t);

  if (data_length < dtls_uint8_to_int(data)) {
    dtls_alert("signature length wrong\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }
  data += sizeof(uint8_t);
  data_length -= sizeof(uint8_t);

  if (dtls_uint8_to_int(data) != 0x02) {
    dtls_alert("wrong ASN.1 struct, expected Integer\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }
  data += sizeof(uint8_t);
  data_length -= sizeof(uint8_t);

  i = dtls_uint8_to_int(data);
  data += sizeof(uint8_t);
  data_length -= sizeof(uint8_t);

  /* Sometimes these values have a leeding 0 byte */
  *result_r = data + i - DTLS_EC_KEY_SIZE;

  data += i;
  data_length -= i;

  if (dtls_uint8_to_int(data) != 0x02) {
    dtls_alert("wrong ASN.1 struct, expected Integer\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }
  data += sizeof(uint8_t);
  data_length -= sizeof(uint8_t);

  i = dtls_uint8_to_int(data);
  data += sizeof(uint8_t);
  data_length -= sizeof(uint8_t);

  /* Sometimes these values have a leeding 0 byte */
  *result_s = data + i - DTLS_EC_KEY_SIZE;

  data += i;
  data_length -= i;

  return data - data_orig;
}

static int
check_client_certificate_verify(dtls_context_t *ctx, 
				dtls_peer_t *peer,
				uint8_t *data, size_t data_length)
{
  int ret;
  unsigned char *result_r;
  unsigned char *result_s;
  dtls_hash_ctx hs_hash;
  unsigned char sha256hash[DTLS_HMAC_DIGEST_SIZE];
  dtls_handshake_parameters_t *config;

  if(!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }
  config = peer->handshake_params;

  assert(is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(config->cipher));

  data += DTLS_HS_LENGTH;

  if (data_length < DTLS_HS_LENGTH + DTLS_CV_LENGTH) {
    dtls_alert("the packet length does not match the expected\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }

  ret = dtls_check_ecdsa_signature_elem(data, data_length, &result_r, &result_s);
  if (ret < 0) {
    return ret;
  }
  data += ret;
  data_length -= ret;

  copy_hs_hash(peer, &hs_hash);

  dtls_hash_finalize(sha256hash, &hs_hash);

  ret = dtls_ecdsa_verify_sig_hash(config->keyx.ecdsa.other_pub_x, config->keyx.ecdsa.other_pub_y,
			    sizeof(config->keyx.ecdsa.other_pub_x),
			    sha256hash, sizeof(sha256hash),
			    result_r, result_s);

  if (ret < 0) {
    dtls_alert("wrong signature err: %i\n", ret);
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }
  return 0;
}
#endif /* DTLS_ECC */

static int
dtls_send_server_hello(dtls_context_t *ctx, dtls_peer_t *peer)
{
  /* Ensure that the largest message to create fits in our source
   * buffer. (The size of the destination buffer is checked by the
   * encoding function, so we do not need to guess.) */
  uint8_t buf[DTLS_SH_LENGTH + 2 + 5 + 5 + 8 + 6];
  uint8_t *p;
  int ecdsa;
  uint8_t extension_size;
  dtls_tick_t now;
  dtls_handshake_parameters_t *handshake;

  if(!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  handshake = peer->handshake_params;

  ecdsa = is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(handshake->cipher);

  extension_size = (ecdsa) ? 2 + 5 + 5 + 6 : 0;

  /* Handshake header */
  p = buf;

  /* ServerHello */
  dtls_int_to_uint16(p, DTLS_VERSION);
  p += sizeof(uint16_t);

  /* Set server random: First 4 bytes are the server's Unix timestamp,
   * followed by 28 bytes of generate random data. */
  dtls_ticks(&now);
  dtls_int_to_uint32(handshake->tmp.random.server, now / DTLS_TICKS_PER_SECOND);
  dtls_fill_random(handshake->tmp.random.server + 4, 28);

  memcpy(p, handshake->tmp.random.server, DTLS_RANDOM_LENGTH);
  p += DTLS_RANDOM_LENGTH;

  *p++ = 0;			/* no session id */

  if (handshake->cipher != TLS_NULL_WITH_NULL_NULL) {
    /* selected cipher suite */
    dtls_int_to_uint16(p, handshake->cipher);
    p += sizeof(uint16_t);

    /* selected compression method */
    *p++ = compression_methods[handshake->compression];
  }

  if (extension_size) {
    /* length of the extensions */
    dtls_int_to_uint16(p, extension_size - 2);
    p += sizeof(uint16_t);
  }

  if (ecdsa) {
    /* client certificate type extension */
    dtls_int_to_uint16(p, TLS_EXT_CLIENT_CERTIFICATE_TYPE);
    p += sizeof(uint16_t);

    /* length of this extension type */
    dtls_int_to_uint16(p, 1);
    p += sizeof(uint16_t);

    dtls_int_to_uint8(p, TLS_CERT_TYPE_RAW_PUBLIC_KEY);
    p += sizeof(uint8_t);

    /* client certificate type extension */
    dtls_int_to_uint16(p, TLS_EXT_SERVER_CERTIFICATE_TYPE);
    p += sizeof(uint16_t);

    /* length of this extension type */
    dtls_int_to_uint16(p, 1);
    p += sizeof(uint16_t);

    dtls_int_to_uint8(p, TLS_CERT_TYPE_RAW_PUBLIC_KEY);
    p += sizeof(uint8_t);

    /* ec_point_formats */
    dtls_int_to_uint16(p, TLS_EXT_EC_POINT_FORMATS);
    p += sizeof(uint16_t);

    /* length of this extension type */
    dtls_int_to_uint16(p, 2);
    p += sizeof(uint16_t);

    /* number of supported formats */
    dtls_int_to_uint8(p, 1);
    p += sizeof(uint8_t);

    dtls_int_to_uint8(p, TLS_EXT_EC_POINT_FORMATS_UNCOMPRESSED);
    p += sizeof(uint8_t);
  }

  assert(p - buf <= sizeof(buf));

  /* TODO use the same record sequence number as in the ClientHello,
     see 4.2.1. Denial-of-Service Countermeasures */
  return dtls_send_handshake_msg(ctx, peer, DTLS_HT_SERVER_HELLO,
				 buf, p - buf);
}

#if DTLS_ECC
#define DTLS_EC_SUBJECTPUBLICKEY_SIZE (2 * DTLS_EC_KEY_SIZE + sizeof(cert_asn1_header))

static int
dtls_send_certificate_ecdsa(dtls_context_t *ctx, dtls_peer_t *peer,
			    const dtls_ecdsa_key_t *key)
{
  uint8_t buf[DTLS_CE_LENGTH];
  uint8_t *p;

  /* Certificate 
   *
   * Start message construction at beginning of buffer. */
  p = buf;

  /* length of this certificate */
  dtls_int_to_uint24(p, DTLS_EC_SUBJECTPUBLICKEY_SIZE);
  p += 3; /* 24 bits */

  memcpy(p, &cert_asn1_header, sizeof(cert_asn1_header));
  p += sizeof(cert_asn1_header);

  memcpy(p, key->pub_key_x, DTLS_EC_KEY_SIZE);
  p += DTLS_EC_KEY_SIZE;

  memcpy(p, key->pub_key_y, DTLS_EC_KEY_SIZE);
  p += DTLS_EC_KEY_SIZE;

  assert(p - buf <= sizeof(buf));

  return dtls_send_handshake_msg(ctx, peer, DTLS_HT_CERTIFICATE,
				 buf, p - buf);
}

static uint8_t *
dtls_add_ecdsa_signature_elem(uint8_t *p, uint32_t *point_r, uint32_t *point_s)
{
  int len_r;
  int len_s;

#define R_KEY_OFFSET (1 + 1 + 2 + 1 + 1 + 1 + 1)
#define S_KEY_OFFSET(len_s) (R_KEY_OFFSET + (len_s) + 1 + 1)
  /* store the pointer to the r component of the signature and make space */
  len_r = dtls_ec_key_from_uint32_asn1(point_r, DTLS_EC_KEY_SIZE, p + R_KEY_OFFSET);
  len_s = dtls_ec_key_from_uint32_asn1(point_s, DTLS_EC_KEY_SIZE, p + S_KEY_OFFSET(len_r));

#undef R_KEY_OFFSET
#undef S_KEY_OFFSET

  /* sha256 */
  dtls_int_to_uint8(p, TLS_EXT_SIG_HASH_ALGO_SHA256);
  p += sizeof(uint8_t);

  /* ecdsa */
  dtls_int_to_uint8(p, TLS_EXT_SIG_HASH_ALGO_ECDSA);
  p += sizeof(uint8_t);

  /* length of signature */
  dtls_int_to_uint16(p, len_r + len_s + 2 + 2 + 2);
  p += sizeof(uint16_t);

  /* ASN.1 SEQUENCE */
  dtls_int_to_uint8(p, 0x30);
  p += sizeof(uint8_t);

  dtls_int_to_uint8(p, len_r + len_s + 2 + 2);
  p += sizeof(uint8_t);

  /* ASN.1 Integer r */
  dtls_int_to_uint8(p, 0x02);
  p += sizeof(uint8_t);

  dtls_int_to_uint8(p, len_r);
  p += sizeof(uint8_t);

  /* the pint r was added here */
  p += len_r;

  /* ASN.1 Integer s */
  dtls_int_to_uint8(p, 0x02);
  p += sizeof(uint8_t);

  dtls_int_to_uint8(p, len_s);
  p += sizeof(uint8_t);

  /* the pint s was added here */
  p += len_s;

  return p;
}

static int
dtls_send_server_key_exchange_ecdh(dtls_context_t *ctx, dtls_peer_t *peer,
				   const dtls_ecdsa_key_t *key)
{
  /* The ASN.1 Integer representation of an 32 byte unsigned int could be
   * 33 bytes long add space for that */
  uint8_t buf[DTLS_SKEXEC_LENGTH + 2];
  uint8_t *p;
  uint8_t *key_params;
  uint8_t *ephemeral_pub_x;
  uint8_t *ephemeral_pub_y;
  uint32_t point_r[9];
  uint32_t point_s[9];
  dtls_handshake_parameters_t *config;

  if(!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }
  config = peer->handshake_params;

  /* ServerKeyExchange
   *
   * Start message construction at beginning of buffer. */
  p = buf;

  key_params = p;
  /* ECCurveType curve_type: named_curve */
  dtls_int_to_uint8(p, 3);
  p += sizeof(uint8_t);

  /* NamedCurve namedcurve: secp256r1 */
  dtls_int_to_uint16(p, TLS_EXT_ELLIPTIC_CURVES_SECP256R1);
  p += sizeof(uint16_t);

  dtls_int_to_uint8(p, 1 + 2 * DTLS_EC_KEY_SIZE);
  p += sizeof(uint8_t);

  /* This should be an uncompressed point, but I do not have access to the spec. */
  dtls_int_to_uint8(p, 4);
  p += sizeof(uint8_t);

  /* store the pointer to the x component of the pub key and make space */
  ephemeral_pub_x = p;
  p += DTLS_EC_KEY_SIZE;

  /* store the pointer to the y component of the pub key and make space */
  ephemeral_pub_y = p;
  p += DTLS_EC_KEY_SIZE;

  dtls_ecdsa_generate_key(config->keyx.ecdsa.own_eph_priv,
			  ephemeral_pub_x, ephemeral_pub_y,
			  DTLS_EC_KEY_SIZE);

  /* sign the ephemeral and its paramaters */
  dtls_ecdsa_create_sig(key->priv_key, DTLS_EC_KEY_SIZE,
		       config->tmp.random.client, DTLS_RANDOM_LENGTH,
		       config->tmp.random.server, DTLS_RANDOM_LENGTH,
		       key_params, p - key_params,
		       point_r, point_s);

  p = dtls_add_ecdsa_signature_elem(p, point_r, point_s);

  assert(p - buf <= sizeof(buf));

  return dtls_send_handshake_msg(ctx, peer, DTLS_HT_SERVER_KEY_EXCHANGE,
				 buf, p - buf);
}
#endif /* DTLS_ECC */

#ifdef DTLS_PSK
static int
dtls_send_server_key_exchange_psk(dtls_context_t *ctx, dtls_peer_t *peer,
				  const unsigned char *psk_hint, size_t len)
{
  uint8_t buf[DTLS_SKEXECPSK_LENGTH_MAX];
  uint8_t *p;

  p = buf;

  assert(len <= DTLS_PSK_MAX_CLIENT_IDENTITY_LEN);
  if (len > DTLS_PSK_MAX_CLIENT_IDENTITY_LEN) {
    /* should never happen */
    dtls_warn("psk identity hint is too long\n");
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  dtls_int_to_uint16(p, len);
  p += sizeof(uint16_t);

  memcpy(p, psk_hint, len);
  p += len;

  assert(p - buf <= sizeof(buf));

  return dtls_send_handshake_msg(ctx, peer, DTLS_HT_SERVER_KEY_EXCHANGE,
				 buf, p - buf);
}
#endif /* DTLS_PSK */

#if DTLS_ECC
static int
dtls_send_server_certificate_request(dtls_context_t *ctx, dtls_peer_t *peer)
{
  uint8_t buf[8];
  uint8_t *p;

  /* ServerHelloDone 
   *
   * Start message construction at beginning of buffer. */
  p = buf;

  /* certificate_types */
  dtls_int_to_uint8(p, 1);
  p += sizeof(uint8_t);

  /* ecdsa_sign */
  dtls_int_to_uint8(p, TLS_CLIENT_CERTIFICATE_TYPE_ECDSA_SIGN);
  p += sizeof(uint8_t);

  /* supported_signature_algorithms */
  dtls_int_to_uint16(p, 2);
  p += sizeof(uint16_t);

  /* sha256 */
  dtls_int_to_uint8(p, TLS_EXT_SIG_HASH_ALGO_SHA256);
  p += sizeof(uint8_t);

  /* ecdsa */
  dtls_int_to_uint8(p, TLS_EXT_SIG_HASH_ALGO_ECDSA);
  p += sizeof(uint8_t);

  /* certificate_authoritiess */
  dtls_int_to_uint16(p, 0);
  p += sizeof(uint16_t);

  assert(p - buf <= sizeof(buf));

  return dtls_send_handshake_msg(ctx, peer, DTLS_HT_CERTIFICATE_REQUEST,
				 buf, p - buf);
}
#endif /* DTLS_ECC */

static int
dtls_send_server_hello_done(dtls_context_t *ctx, dtls_peer_t *peer)
{

  /* ServerHelloDone 
   *
   * Start message construction at beginning of buffer. */

  return dtls_send_handshake_msg(ctx, peer, DTLS_HT_SERVER_HELLO_DONE,
				 NULL, 0);
}

static int
dtls_send_server_hello_msgs(dtls_context_t *ctx, dtls_peer_t *peer)
{
  int res;

  res = dtls_send_server_hello(ctx, peer);

  if (res < 0) {
    dtls_debug("dtls_server_hello: cannot prepare ServerHello record\n");
    return res;
  }

  if(!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

#if DTLS_ECC
  if (is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(peer->handshake_params->cipher)) {
    const dtls_ecdsa_key_t *ecdsa_key;

    res = CALL(ctx, get_ecdsa_key, &peer->session, &ecdsa_key);
    if (res < 0) {
      dtls_crit("no ecdsa certificate to send in certificate\n");
      return res;
    }

    res = dtls_send_certificate_ecdsa(ctx, peer, ecdsa_key);

    if (res < 0) {
      dtls_debug("dtls_server_hello: cannot prepare Certificate record\n");
      return res;
    }

    res = dtls_send_server_key_exchange_ecdh(ctx, peer, ecdsa_key);

    if (res < 0) {
      dtls_debug("dtls_server_hello: cannot prepare Server Key Exchange record\n");
      return res;
    }

    if (is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(peer->handshake_params->cipher) &&
	is_ecdsa_client_auth_supported(ctx)) {
      res = dtls_send_server_certificate_request(ctx, peer);

      if (res < 0) {
        dtls_debug("dtls_server_hello: cannot prepare certificate Request record\n");
        return res;
      }
    }
  }
#endif /* DTLS_ECC */

#ifdef DTLS_PSK
  if (is_tls_psk_with_aes_128_ccm_8(peer->handshake_params->cipher)) {
    unsigned char psk_hint[DTLS_PSK_MAX_CLIENT_IDENTITY_LEN];
    int len;

    /* The identity hint is optional, therefore we ignore the result
     * and check psk only. */
    len = CALL(ctx, get_psk_info, &peer->session, DTLS_PSK_HINT,
	       NULL, 0, psk_hint, DTLS_PSK_MAX_CLIENT_IDENTITY_LEN);

    if (len < 0) {
      dtls_debug("dtls_server_hello: cannot create ServerKeyExchange\n");
      return len;
    }

    if (len > 0) {
      res = dtls_send_server_key_exchange_psk(ctx, peer, psk_hint, (size_t)len);

      if (res < 0) {
	dtls_debug("dtls_server_key_exchange_psk: cannot send server key exchange record\n");
	return res;
      }
    }
  }
#endif /* DTLS_PSK */

  res = dtls_send_server_hello_done(ctx, peer);

  if (res < 0) {
    dtls_debug("dtls_server_hello: cannot prepare ServerHelloDone record\n");
    return res;
  }
  return 0;
}

static inline int 
dtls_send_ccs(dtls_context_t *ctx, dtls_peer_t *peer) {
  uint8_t buf[1] = {1};

  return dtls_send(ctx, peer, DTLS_CT_CHANGE_CIPHER_SPEC, buf, 1);
}

    
static int
dtls_send_client_key_exchange(dtls_context_t *ctx, dtls_peer_t *peer)
{
  uint8_t buf[DTLS_CKXEC_LENGTH];
  uint8_t *p;
  dtls_handshake_parameters_t *handshake;

  if(!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }
  handshake = peer->handshake_params;

  p = buf;

  switch (handshake->cipher) {
#ifdef DTLS_PSK
  case TLS_PSK_WITH_AES_128_CCM_8: {
    int len;

    len = CALL(ctx, get_psk_info, &peer->session, DTLS_PSK_IDENTITY,
	       handshake->keyx.psk.identity, handshake->keyx.psk.id_length,
	       buf + sizeof(uint16_t),
	       min(sizeof(buf) - sizeof(uint16_t),
		   sizeof(handshake->keyx.psk.identity)));
    if (len < 0) {
      dtls_crit("no psk identity set in kx\n");
      return len;
    }

    if (len + sizeof(uint16_t) > DTLS_CKXEC_LENGTH) {
      memset(&handshake->keyx.psk, 0, sizeof(dtls_handshake_parameters_psk_t));
      dtls_warn("the psk identity is too long\n");
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }
    handshake->keyx.psk.id_length = (unsigned int)len;
    memcpy(handshake->keyx.psk.identity, p + sizeof(uint16_t), len);

    dtls_int_to_uint16(p, handshake->keyx.psk.id_length);
    p += sizeof(uint16_t);

    memcpy(p, handshake->keyx.psk.identity, handshake->keyx.psk.id_length);
    p += handshake->keyx.psk.id_length;

    break;
  }
#endif /* DTLS_PSK */
#if DTLS_ECC
  case TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8: {
    uint8_t *ephemeral_pub_x;
    uint8_t *ephemeral_pub_y;

    dtls_int_to_uint8(p, 1 + 2 * DTLS_EC_KEY_SIZE);
    p += sizeof(uint8_t);

    /* This should be an uncompressed point, but I do not have access to the spec. */
    dtls_int_to_uint8(p, 4);
    p += sizeof(uint8_t);

    ephemeral_pub_x = p;
    p += DTLS_EC_KEY_SIZE;
    ephemeral_pub_y = p;
    p += DTLS_EC_KEY_SIZE;

    dtls_ecdsa_generate_key(peer->handshake_params->keyx.ecdsa.own_eph_priv,
    			    ephemeral_pub_x, ephemeral_pub_y,
    			    DTLS_EC_KEY_SIZE);

    break;
  }
#endif /* DTLS_ECC */
  default:
    dtls_crit("cipher not supported\n");
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  assert(p - buf <= sizeof(buf));

  return dtls_send_handshake_msg(ctx, peer, DTLS_HT_CLIENT_KEY_EXCHANGE,
				 buf, p - buf);
}

#if DTLS_ECC
static int
dtls_send_certificate_verify_ecdh(dtls_context_t *ctx, dtls_peer_t *peer,
				   const dtls_ecdsa_key_t *key)
{
  /* The ASN.1 Integer representation of an 32 byte unsigned int could be
   * 33 bytes long add space for that */
  uint8_t buf[DTLS_CV_LENGTH + 2];
  uint8_t *p;
  uint32_t point_r[9];
  uint32_t point_s[9];
  dtls_hash_ctx hs_hash;
  unsigned char sha256hash[DTLS_HMAC_DIGEST_SIZE];

  if (!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  /* ServerKeyExchange
   *
   * Start message construction at beginning of buffer. */
  p = buf;

  copy_hs_hash(peer, &hs_hash);

  dtls_hash_finalize(sha256hash, &hs_hash);

  /* sign the ephemeral and its paramaters */
  dtls_ecdsa_create_sig_hash(key->priv_key, DTLS_EC_KEY_SIZE,
			     sha256hash, sizeof(sha256hash),
			     point_r, point_s);

  p = dtls_add_ecdsa_signature_elem(p, point_r, point_s);

  assert(p - buf <= sizeof(buf));

  return dtls_send_handshake_msg(ctx, peer, DTLS_HT_CERTIFICATE_VERIFY,
				 buf, p - buf);
}
#endif /* DTLS_ECC */

static int
dtls_send_finished(dtls_context_t *ctx, dtls_peer_t *peer,
		   const unsigned char *label, size_t labellen)
{
  int length;
  uint8_t hash[DTLS_HMAC_MAX];
  uint8_t buf[DTLS_FIN_LENGTH];
  dtls_hash_ctx hs_hash;
  uint8_t *p = buf;

  if (!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  copy_hs_hash(peer, &hs_hash);

  length = dtls_hash_finalize(hash, &hs_hash);

  dtls_prf(peer->handshake_params->tmp.master_secret,
	   DTLS_MASTER_SECRET_LENGTH,
	   label, labellen,
	   PRF_LABEL(finished), PRF_LABEL_SIZE(finished), 
	   hash, length,
	   p, DTLS_FIN_LENGTH);

  dtls_debug_dump("server finished MAC", p, DTLS_FIN_LENGTH);

  p += DTLS_FIN_LENGTH;

  assert(p - buf <= sizeof(buf));

  return dtls_send_handshake_msg(ctx, peer, DTLS_HT_FINISHED,
				 buf, p - buf);
}

static int
dtls_send_client_hello(dtls_context_t *ctx, dtls_peer_t *peer,
                       uint8_t cookie[], size_t cookie_length) {
  uint8_t buf[DTLS_CH_LENGTH_MAX];
  uint8_t *p = buf;
  uint8_t cipher_size;
  uint8_t extension_size;
  int psk;
  int ecdsa;
  dtls_tick_t now;
  dtls_handshake_parameters_t *handshake;

  if (!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }
  handshake = peer->handshake_params;

  psk = is_psk_supported(ctx);
  ecdsa = is_ecdsa_supported(ctx, 1);

  cipher_size = 2 + ((ecdsa) ? 2 : 0) + ((psk) ? 2 : 0);
  extension_size = (ecdsa) ? 2 + 6 + 6 + 8 + 6: 0;

  if (cipher_size == 0) {
    dtls_crit("no cipher callbacks implemented\n");
  }

  dtls_int_to_uint16(p, DTLS_VERSION);
  p += sizeof(uint16_t);

  if (cookie_length > DTLS_COOKIE_LENGTH_MAX) {
    dtls_warn("the cookie is too long\n");
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }

  if (cookie_length == 0) {
    /* Set client random: First 4 bytes are the client's Unix timestamp,
     * followed by 28 bytes of generate random data. */
    dtls_ticks(&now);
    dtls_int_to_uint32(handshake->tmp.random.client, now / DTLS_TICKS_PER_SECOND);
    dtls_fill_random(handshake->tmp.random.client + sizeof(uint32_t),
         DTLS_RANDOM_LENGTH - sizeof(uint32_t));
  }
  /* we must use the same Client Random as for the previous request */
  memcpy(p, handshake->tmp.random.client, DTLS_RANDOM_LENGTH);
  p += DTLS_RANDOM_LENGTH;

  /* session id (length 0) */
  dtls_int_to_uint8(p, 0);
  p += sizeof(uint8_t);

  /* cookie */
  dtls_int_to_uint8(p, cookie_length);
  p += sizeof(uint8_t);
  if (cookie_length != 0) {
    memcpy(p, cookie, cookie_length);
    p += cookie_length;
  }

  /* add known cipher(s) */
  dtls_int_to_uint16(p, cipher_size - 2);
  p += sizeof(uint16_t);

  if (ecdsa) {
    dtls_int_to_uint16(p, TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8);
    p += sizeof(uint16_t);
  }
  if (psk) {
    dtls_int_to_uint16(p, TLS_PSK_WITH_AES_128_CCM_8);
    p += sizeof(uint16_t);
  }

  /* compression method */
  dtls_int_to_uint8(p, 1);
  p += sizeof(uint8_t);

  dtls_int_to_uint8(p, TLS_COMPRESSION_NULL);
  p += sizeof(uint8_t);

  if (extension_size) {
    /* length of the extensions */
    dtls_int_to_uint16(p, extension_size - 2);
    p += sizeof(uint16_t);
  }

  if (ecdsa) {
    /* client certificate type extension */
    dtls_int_to_uint16(p, TLS_EXT_CLIENT_CERTIFICATE_TYPE);
    p += sizeof(uint16_t);

    /* length of this extension type */
    dtls_int_to_uint16(p, 2);
    p += sizeof(uint16_t);

    /* length of the list */
    dtls_int_to_uint8(p, 1);
    p += sizeof(uint8_t);

    dtls_int_to_uint8(p, TLS_CERT_TYPE_RAW_PUBLIC_KEY);
    p += sizeof(uint8_t);

    /* client certificate type extension */
    dtls_int_to_uint16(p, TLS_EXT_SERVER_CERTIFICATE_TYPE);
    p += sizeof(uint16_t);

    /* length of this extension type */
    dtls_int_to_uint16(p, 2);
    p += sizeof(uint16_t);

    /* length of the list */
    dtls_int_to_uint8(p, 1);
    p += sizeof(uint8_t);

    dtls_int_to_uint8(p, TLS_CERT_TYPE_RAW_PUBLIC_KEY);
    p += sizeof(uint8_t);

    /* elliptic_curves */
    dtls_int_to_uint16(p, TLS_EXT_ELLIPTIC_CURVES);
    p += sizeof(uint16_t);

    /* length of this extension type */
    dtls_int_to_uint16(p, 4);
    p += sizeof(uint16_t);

    /* length of the list */
    dtls_int_to_uint16(p, 2);
    p += sizeof(uint16_t);

    dtls_int_to_uint16(p, TLS_EXT_ELLIPTIC_CURVES_SECP256R1);
    p += sizeof(uint16_t);

    /* ec_point_formats */
    dtls_int_to_uint16(p, TLS_EXT_EC_POINT_FORMATS);
    p += sizeof(uint16_t);

    /* length of this extension type */
    dtls_int_to_uint16(p, 2);
    p += sizeof(uint16_t);

    /* number of supported formats */
    dtls_int_to_uint8(p, 1);
    p += sizeof(uint8_t);

    dtls_int_to_uint8(p, TLS_EXT_EC_POINT_FORMATS_UNCOMPRESSED);
    p += sizeof(uint8_t);
  }

  assert(p - buf <= sizeof(buf));

  if (cookie_length != 0)
    clear_hs_hash(peer);

  return dtls_send_handshake_msg_hash(ctx, peer, &peer->session,
				      DTLS_HT_CLIENT_HELLO,
				      buf, p - buf, cookie_length != 0);
}

static int
check_server_hello(dtls_context_t *ctx, 
		      dtls_peer_t *peer,
		      uint8_t *data, size_t data_length)
{
  dtls_handshake_parameters_t *handshake;

  /* This function is called when we expect a ServerHello (i.e. we
   * have sent a ClientHello).  We might instead receive a HelloVerify
   * request containing a cookie. If so, we must repeat the
   * ClientHello with the given Cookie.
   */
  if (data_length < DTLS_HS_LENGTH + DTLS_HS_LENGTH) {
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }

  if (!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }
  handshake = peer->handshake_params;

  update_hs_hash(peer, data, data_length);

  /* FIXME: check data_length before accessing fields */

  /* Get the server's random data and store selected cipher suite
   * and compression method (like dtls_update_parameters().
   * Then calculate master secret and wait for ServerHelloDone. When received,
   * send ClientKeyExchange (?) and ChangeCipherSpec + ClientFinished. */
    
  /* check server version */
  data += DTLS_HS_LENGTH;
  data_length -= DTLS_HS_LENGTH;
    
  if (dtls_uint16_to_int(data) != DTLS_VERSION) {
    dtls_alert("unknown DTLS version\n");
    return dtls_alert_fatal_create(DTLS_ALERT_PROTOCOL_VERSION);
  }

  data += sizeof(uint16_t);	      /* skip version field */
  data_length -= sizeof(uint16_t);

  /* store server random data */
  memcpy(handshake->tmp.random.server, data, DTLS_RANDOM_LENGTH);
  /* skip server random */
  data += DTLS_RANDOM_LENGTH;
  data_length -= DTLS_RANDOM_LENGTH;

  SKIP_VAR_FIELD(data, data_length); /* skip session id */
    
  /* Check cipher suite. As we offer all we have, it is sufficient
   * to check if the cipher suite selected by the server is in our
   * list of known cipher suites. Subsets are not supported. */
  handshake->cipher = dtls_uint16_to_int(data);
  if (!known_cipher(ctx, handshake->cipher, 1)) {
    dtls_alert("unsupported cipher 0x%02x 0x%02x\n",
	     data[0], data[1]);
    return dtls_alert_fatal_create(DTLS_ALERT_INSUFFICIENT_SECURITY);
  }
  data += sizeof(uint16_t);
  data_length -= sizeof(uint16_t);

  /* Check if NULL compression was selected. We do not know any other. */
  if (dtls_uint8_to_int(data) != TLS_COMPRESSION_NULL) {
    dtls_alert("unsupported compression method 0x%02x\n", data[0]);
    return dtls_alert_fatal_create(DTLS_ALERT_INSUFFICIENT_SECURITY);
  }
  data += sizeof(uint8_t);
  data_length -= sizeof(uint8_t);

  return dtls_check_tls_extension(peer, data, data_length, 0);

error:
  return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
}

static int
check_server_hello_verify_request(dtls_context_t *ctx,
				  dtls_peer_t *peer,
				  uint8_t *data, size_t data_length)
{
  dtls_hello_verify_t *hv;
  int res;

  if (data_length < DTLS_HS_LENGTH + DTLS_HV_LENGTH)
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);

  hv = (dtls_hello_verify_t *)(data + DTLS_HS_LENGTH);

  res = dtls_send_client_hello(ctx, peer, hv->cookie, hv->cookie_length);

  if (res < 0)
    dtls_warn("cannot send ClientHello\n");

  return res;
}

#if DTLS_ECC
static int
check_server_certificate(dtls_context_t *ctx, 
			 dtls_peer_t *peer,
			 uint8_t *data, size_t data_length)
{
  int err;
  dtls_handshake_parameters_t *config;

  if (!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }
  config = peer->handshake_params;

  update_hs_hash(peer, data, data_length);

  assert(is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(config->cipher));

  data += DTLS_HS_LENGTH;

  if (dtls_uint24_to_int(data) != DTLS_EC_SUBJECTPUBLICKEY_SIZE) {
    dtls_alert("expect length of %lu bytes for certificate\n",
	       (unsigned long)DTLS_EC_SUBJECTPUBLICKEY_SIZE);
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }
  data += 3; /* 24 bits */

  if (memcmp(data, cert_asn1_header, sizeof(cert_asn1_header))) {
    dtls_alert("got an unexpected Subject public key format\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }
  data += sizeof(cert_asn1_header);

  memcpy(config->keyx.ecdsa.other_pub_x, data,
	 sizeof(config->keyx.ecdsa.other_pub_x));
  data += sizeof(config->keyx.ecdsa.other_pub_x);

  memcpy(config->keyx.ecdsa.other_pub_y, data,
	 sizeof(config->keyx.ecdsa.other_pub_y));
  data += sizeof(config->keyx.ecdsa.other_pub_y);

  err = CALL(ctx, verify_ecdsa_key, &peer->session,
	     config->keyx.ecdsa.other_pub_x,
	     config->keyx.ecdsa.other_pub_y,
	     sizeof(config->keyx.ecdsa.other_pub_x));
  if (err < 0) {
    dtls_warn("The certificate was not accepted\n");
    return err;
  }

  return 0;
}

static int
check_server_key_exchange_ecdsa(dtls_context_t *ctx,
				dtls_peer_t *peer,
				uint8_t *data, size_t data_length)
{
  int ret;
  unsigned char *result_r;
  unsigned char *result_s;
  unsigned char *key_params;
  dtls_handshake_parameters_t *config;

  if (!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }
  config = peer->handshake_params;

  update_hs_hash(peer, data, data_length);

  assert(is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(config->cipher));

  data += DTLS_HS_LENGTH;

  if (data_length < DTLS_HS_LENGTH + DTLS_SKEXEC_LENGTH) {
    dtls_alert("the packet length does not match the expected\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }
  key_params = data;

  if (dtls_uint8_to_int(data) != TLS_EC_CURVE_TYPE_NAMED_CURVE) {
    dtls_alert("Only named curves supported\n");
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }
  data += sizeof(uint8_t);
  data_length -= sizeof(uint8_t);

  if (dtls_uint16_to_int(data) != TLS_EXT_ELLIPTIC_CURVES_SECP256R1) {
    dtls_alert("secp256r1 supported\n");
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }
  data += sizeof(uint16_t);
  data_length -= sizeof(uint16_t);

  if (dtls_uint8_to_int(data) != 1 + 2 * DTLS_EC_KEY_SIZE) {
    dtls_alert("expected 65 bytes long public point\n");
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }
  data += sizeof(uint8_t);
  data_length -= sizeof(uint8_t);

  if (dtls_uint8_to_int(data) != 4) {
    dtls_alert("expected uncompressed public point\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }
  data += sizeof(uint8_t);
  data_length -= sizeof(uint8_t);

  memcpy(config->keyx.ecdsa.other_eph_pub_x, data, sizeof(config->keyx.ecdsa.other_eph_pub_y));
  data += sizeof(config->keyx.ecdsa.other_eph_pub_y);
  data_length -= sizeof(config->keyx.ecdsa.other_eph_pub_y);

  memcpy(config->keyx.ecdsa.other_eph_pub_y, data, sizeof(config->keyx.ecdsa.other_eph_pub_y));
  data += sizeof(config->keyx.ecdsa.other_eph_pub_y);
  data_length -= sizeof(config->keyx.ecdsa.other_eph_pub_y);

  ret = dtls_check_ecdsa_signature_elem(data, data_length, &result_r, &result_s);
  if (ret < 0) {
    return ret;
  }
  data += ret;
  data_length -= ret;

  ret = dtls_ecdsa_verify_sig(config->keyx.ecdsa.other_pub_x, config->keyx.ecdsa.other_pub_y,
			    sizeof(config->keyx.ecdsa.other_pub_x),
			    config->tmp.random.client, DTLS_RANDOM_LENGTH,
			    config->tmp.random.server, DTLS_RANDOM_LENGTH,
			    key_params,
			    1 + 2 + 1 + 1 + (2 * DTLS_EC_KEY_SIZE),
			    result_r, result_s);

  if (ret < 0) {
    dtls_alert("wrong signature\n");
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }
  return 0;
}
#endif /* DTLS_ECC */

#ifdef DTLS_PSK
static int
check_server_key_exchange_psk(dtls_context_t *ctx,
			      dtls_peer_t *peer,
			      uint8_t *data, size_t data_length)
{
  dtls_handshake_parameters_t *config;
  uint16_t len;

  if (!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }
  config = peer->handshake_params;

  update_hs_hash(peer, data, data_length);

  assert(is_tls_psk_with_aes_128_ccm_8(config->cipher));

  data += DTLS_HS_LENGTH;

  if (data_length < DTLS_HS_LENGTH + DTLS_SKEXECPSK_LENGTH_MIN) {
    dtls_alert("the packet length does not match the expected\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }

  len = dtls_uint16_to_int(data);
  data += sizeof(uint16_t);

  if (len != data_length - DTLS_HS_LENGTH - sizeof(uint16_t)) {
    dtls_warn("the length of the server identity hint is worng\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }

  if (len > DTLS_PSK_MAX_CLIENT_IDENTITY_LEN) {
    dtls_warn("please use a smaller server identity hint\n");
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  /* store the psk_identity_hint in config->keyx.psk for later use */
  config->keyx.psk.id_length = len;
  memcpy(config->keyx.psk.identity, data, len);
  return 0;
}
#endif /* DTLS_PSK */

static int
check_certificate_request(dtls_context_t *ctx, 
			  dtls_peer_t *peer,
			  uint8_t *data, size_t data_length)
{
  unsigned int i;
  int auth_alg;
  int sig_alg;
  int hash_alg;

  if (!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }

  update_hs_hash(peer, data, data_length);

  assert(is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(peer->handshake_params->cipher));

  data += DTLS_HS_LENGTH;

  if (data_length < DTLS_HS_LENGTH + 5) {
    dtls_alert("the packet length does not match the expected\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }

  i = dtls_uint8_to_int(data);
  data += sizeof(uint8_t);
  if (i + 1 > data_length) {
    dtls_alert("the cerfificate types are too long\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }

  auth_alg = 0;
  for (; i > 0 ; i -= sizeof(uint8_t)) {
    if (dtls_uint8_to_int(data) == TLS_CLIENT_CERTIFICATE_TYPE_ECDSA_SIGN
	&& auth_alg == 0)
      auth_alg = dtls_uint8_to_int(data);
    data += sizeof(uint8_t);
  }

  if (auth_alg != TLS_CLIENT_CERTIFICATE_TYPE_ECDSA_SIGN) {
    dtls_alert("the request authentication algorithm is not supproted\n");
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }

  i = dtls_uint16_to_int(data);
  data += sizeof(uint16_t);
  if (i + 1 > data_length) {
    dtls_alert("the signature and hash algorithm list is too long\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }

  hash_alg = 0;
  sig_alg = 0;
  for (; i > 0 ; i -= sizeof(uint16_t)) {
    int current_hash_alg;
    int current_sig_alg;

    current_hash_alg = dtls_uint8_to_int(data);
    data += sizeof(uint8_t);
    current_sig_alg = dtls_uint8_to_int(data);
    data += sizeof(uint8_t);

    if (current_hash_alg == TLS_EXT_SIG_HASH_ALGO_SHA256 && hash_alg == 0 && 
        current_sig_alg == TLS_EXT_SIG_HASH_ALGO_ECDSA && sig_alg == 0) {
      hash_alg = current_hash_alg;
      sig_alg = current_sig_alg;
    }
  }

  if (hash_alg != TLS_EXT_SIG_HASH_ALGO_SHA256 ||
      sig_alg != TLS_EXT_SIG_HASH_ALGO_ECDSA) {
    dtls_alert("no supported hash and signature algorithem\n");
    return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
  }

  /* common names are ignored */

  peer->handshake_params->do_client_auth = 1;
  return 0;
}

static int
check_server_hellodone(dtls_context_t *ctx, 
		      dtls_peer_t *peer,
		      uint8_t *data, size_t data_length)
{
  dtls_handshake_parameters_t *handshake;
  int res;
#if DTLS_ECC
  const dtls_ecdsa_key_t *ecdsa_key;
#endif /* DTLS_ECC */

  if (!peer || !peer->handshake_params) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }
  handshake = peer->handshake_params;

  /* calculate master key, send CCS */

  update_hs_hash(peer, data, data_length);

#if DTLS_ECC
  if (handshake->do_client_auth) {

    res = CALL(ctx, get_ecdsa_key, &peer->session, &ecdsa_key);
    if (res < 0) {
      dtls_crit("no ecdsa certificate to send in certificate\n");
      return res;
    }

    res = dtls_send_certificate_ecdsa(ctx, peer, ecdsa_key);

    if (res < 0) {
      dtls_debug("dtls_server_hello: cannot prepare Certificate record\n");
      return res;
    }
  }
#endif /* DTLS_ECC */

  /* send ClientKeyExchange */
  res = dtls_send_client_key_exchange(ctx, peer);

  if (res < 0) {
    dtls_debug("cannot send KeyExchange message\n");
    return res;
  }

#if DTLS_ECC
  if (handshake->do_client_auth) {

    res = dtls_send_certificate_verify_ecdh(ctx, peer, ecdsa_key);

    if (res < 0) {
      dtls_debug("dtls_server_hello: cannot prepare Certificate record\n");
      return res;
    }
  }
#endif /* DTLS_ECC */

  res = calculate_key_block(ctx, handshake, peer,
			    &peer->session, peer->role);
  if (res < 0) {
    return res;
  }

  res = dtls_send_ccs(ctx, peer);
  if (res < 0) {
    dtls_debug("cannot send CCS message\n");
    return res;
  }

  /* and switch cipher suite */
  dtls_security_params_switch(peer);

  /* Client Finished */
  return dtls_send_finished(ctx, peer, PRF_LABEL(client), PRF_LABEL_SIZE(client));
}

static int
decrypt_verify(dtls_peer_t *peer, uint8_t *packet, size_t length,
	       uint8_t **cleartext)
{
  dtls_record_header_t *header = DTLS_RECORD_HEADER(packet);
  dtls_security_parameters_t *security;
  int clen;

  if (!peer) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }
  security = dtls_security_params_epoch(peer, dtls_get_epoch(header));

  *cleartext = (uint8_t *)packet + sizeof(dtls_record_header_t);
  clen = length - sizeof(dtls_record_header_t);

  if (!security) {
    dtls_alert("No security context for epoch: %i\n", dtls_get_epoch(header));
    return -1;
  }

  if (security->cipher == TLS_NULL_WITH_NULL_NULL) {
    /* no cipher suite selected */
    return clen;
  } else { /* TLS_PSK_WITH_AES_128_CCM_8 or TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8 */
    /** 
     * length of additional_data for the AEAD cipher which consists of
     * seq_num(2+6) + type(1) + version(2) + length(2)
     */
#define A_DATA_LEN 13
    unsigned char nonce[DTLS_CCM_BLOCKSIZE];
    unsigned char A_DATA[A_DATA_LEN];

    if (clen < 16)		/* need at least IV and MAC */
      return -1;

    memset(nonce, 0, DTLS_CCM_BLOCKSIZE);
    memcpy(nonce, dtls_kb_remote_iv(security, peer->role),
	   dtls_kb_iv_size(security, peer->role));

    /* read epoch and seq_num from message */
    memcpy(nonce + dtls_kb_iv_size(security, peer->role), *cleartext, 8);
    *cleartext += 8;
    clen -= 8;

    dtls_debug_dump("nonce", nonce, DTLS_CCM_BLOCKSIZE);
    dtls_debug_dump("key", dtls_kb_remote_write_key(security, peer->role),
		    dtls_kb_key_size(security, peer->role));
    dtls_debug_dump("ciphertext", *cleartext, clen);

    /* re-use N to create additional data according to RFC 5246, Section 6.2.3.3:
     * 
     * additional_data = seq_num + TLSCompressed.type +
     *                   TLSCompressed.version + TLSCompressed.length;
     */
    memcpy(A_DATA, &DTLS_RECORD_HEADER(packet)->epoch, 8); /* epoch and seq_num */
    memcpy(A_DATA + 8,  &DTLS_RECORD_HEADER(packet)->content_type, 3); /* type and version */
    dtls_int_to_uint16(A_DATA + 11, clen - 8); /* length without nonce_explicit */

    clen = dtls_decrypt(*cleartext, clen, *cleartext, nonce,
		       dtls_kb_remote_write_key(security, peer->role),
		       dtls_kb_key_size(security, peer->role),
		       A_DATA, A_DATA_LEN);
    if (clen < 0)
      dtls_warn("decryption failed\n");
    else {
      dtls_debug("decrypt_verify(): found %i bytes cleartext\n", clen);
      dtls_security_params_free_other(peer);
      dtls_debug_dump("cleartext", *cleartext, clen);
    }
  }
  return clen;
}

static int
dtls_send_hello_request(dtls_context_t *ctx, dtls_peer_t *peer)
{
  if (!peer) {
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
  }
  return dtls_send_handshake_msg_hash(ctx, peer, &peer->session,
				      DTLS_HT_HELLO_REQUEST,
				      NULL, 0, 0);
}

int
dtls_renegotiate(dtls_context_t *ctx, const session_t *dst)
{
  dtls_peer_t *peer = NULL;
  int err;

  peer = dtls_get_peer(ctx, dst);

  if (!peer) {
    return -1;
  }
  if (peer->state != DTLS_STATE_CONNECTED)
    return -1;

  peer->handshake_params = dtls_handshake_new();
  if (!peer->handshake_params)
    return -1;

  peer->handshake_params->hs_state.mseq_r = 0;
  peer->handshake_params->hs_state.mseq_s = 0;

  if (peer->role == DTLS_CLIENT) {
    /* send ClientHello with empty Cookie */
    err = dtls_send_client_hello(ctx, peer, NULL, 0);
    if (err < 0)
      dtls_warn("cannot send ClientHello\n");
    else
      peer->state = DTLS_STATE_CLIENTHELLO;
    return err;
  } else if (peer->role == DTLS_SERVER) {
    return dtls_send_hello_request(ctx, peer);
  }

  return -1;
}

static int
handle_handshake_record(dtls_context_t *ctx, dtls_peer_t *peer, session_t *session,
		 const dtls_peer_type role, const dtls_state_t state,
		 uint8_t *data, size_t data_length) {

  int err = 0;

  dtls_debug_dump("handle_handshake_record", data, data_length);	//AD

  /* This will clear the retransmission buffer if we get an expected
   * handshake message. We have to make sure that no handshake message
   * should get expected when we still should retransmit something, when
   * we do everything accordingly to the DTLS 1.2 standard this should
   * not be a problem. */
  if (peer) {
    dtls_stop_retransmission(ctx, peer);
  }

  /* The following switch construct handles the given message with
   * respect to the current internal state for this peer. In case of
   * error, it is left with return 0. */

  dtls_debug("handle handshake packet of type: %s (%i)\n",
	     dtls_handshake_type_to_name(data[0]), data[0]);
  switch (data[0]) {

  /************************************************************************
   * Client states
   ************************************************************************/
  case DTLS_HT_HELLO_VERIFY_REQUEST:

    if (state != DTLS_STATE_CLIENTHELLO) {
      return dtls_alert_fatal_create(DTLS_ALERT_UNEXPECTED_MESSAGE);
    }

    err = check_server_hello_verify_request(ctx, peer, data, data_length);
    if (err < 0) {
      dtls_warn("error in check_server_hello_verify_request err: %i\n", err);
      return err;
    }

    break;
  case DTLS_HT_SERVER_HELLO:

    if (state != DTLS_STATE_CLIENTHELLO) {
      return dtls_alert_fatal_create(DTLS_ALERT_UNEXPECTED_MESSAGE);
    }

    if(!peer || !peer->handshake_params) {
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }

    err = check_server_hello(ctx, peer, data, data_length);
    if (err < 0) {
      dtls_warn("error in check_server_hello err: %i\n", err);
      return err;
    }
    if (is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(peer->handshake_params->cipher))
      peer->state = DTLS_STATE_WAIT_SERVERCERTIFICATE;
    else
      peer->state = DTLS_STATE_WAIT_SERVERHELLODONE;
    /* update_hs_hash(peer, data, data_length); */

    break;

#if DTLS_ECC
  case DTLS_HT_CERTIFICATE:

    if ((role == DTLS_CLIENT && state != DTLS_STATE_WAIT_SERVERCERTIFICATE) ||
        (role == DTLS_SERVER && state != DTLS_STATE_WAIT_CLIENTCERTIFICATE)) {
      return dtls_alert_fatal_create(DTLS_ALERT_UNEXPECTED_MESSAGE);
    }

    if(!peer) {
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }

    err = check_server_certificate(ctx, peer, data, data_length);
    if (err < 0) {
      dtls_warn("error in check_server_certificate err: %i\n", err);
      return err;
    }
    if (role == DTLS_CLIENT) {
      peer->state = DTLS_STATE_WAIT_SERVERKEYEXCHANGE;
    } else if (role == DTLS_SERVER){
      peer->state = DTLS_STATE_WAIT_CLIENTKEYEXCHANGE;
    }
    /* update_hs_hash(peer, data, data_length); */

    break;
#endif /* DTLS_ECC */

  case DTLS_HT_SERVER_KEY_EXCHANGE:

    if(!peer || !peer->handshake_params) {
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }

#if DTLS_ECC
    if (is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(peer->handshake_params->cipher)) {
      if (state != DTLS_STATE_WAIT_SERVERKEYEXCHANGE) {
        LOG_DBG("DBG::3231 \n");
        return dtls_alert_fatal_create(DTLS_ALERT_UNEXPECTED_MESSAGE);
      }
      err = check_server_key_exchange_ecdsa(ctx, peer, data, data_length);
    }
#endif /* DTLS_ECC */
#ifdef DTLS_PSK
    if (is_tls_psk_with_aes_128_ccm_8(peer->handshake_params->cipher)) {
      if (state != DTLS_STATE_WAIT_SERVERHELLODONE) {
        LOG_DBG("DBG::3240 \n");
        return dtls_alert_fatal_create(DTLS_ALERT_UNEXPECTED_MESSAGE);
      }
      err = check_server_key_exchange_psk(ctx, peer, data, data_length);
    }
#endif /* DTLS_PSK */

    if (err < 0) {
      dtls_warn("error in check_server_key_exchange err: %i\n", err);
      return err;
    }
    peer->state = DTLS_STATE_WAIT_SERVERHELLODONE;
    /* update_hs_hash(peer, data, data_length); */

    break;

  case DTLS_HT_SERVER_HELLO_DONE:

    if (state != DTLS_STATE_WAIT_SERVERHELLODONE) {
      LOG_DBG("DBG::3259 \n");
      return dtls_alert_fatal_create(DTLS_ALERT_UNEXPECTED_MESSAGE);
    }

    if(!peer) {
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }

    err = check_server_hellodone(ctx, peer, data, data_length);
    if (err < 0) {
      dtls_warn("error in check_server_hellodone err: %i\n", err);
      return err;
    }
    peer->state = DTLS_STATE_WAIT_CHANGECIPHERSPEC;
    /* update_hs_hash(peer, data, data_length); */

    break;

  case DTLS_HT_CERTIFICATE_REQUEST:

    if (state != DTLS_STATE_WAIT_SERVERHELLODONE) {
      LOG_DBG("DBG::3280 \n");
      return dtls_alert_fatal_create(DTLS_ALERT_UNEXPECTED_MESSAGE);
    }

    err = check_certificate_request(ctx, peer, data, data_length);
    if (err < 0) {
      dtls_warn("error in check_certificate_request err: %i\n", err);
      return err;
    }

    break;

  case DTLS_HT_FINISHED:
    /* expect a Finished message from server */

    if (state != DTLS_STATE_WAIT_FINISHED) {
      return dtls_alert_fatal_create(DTLS_ALERT_UNEXPECTED_MESSAGE);
    }

    if(!peer || !peer->handshake_params) {
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }

    err = check_finished(ctx, peer, data, data_length);
    if (err < 0) {
      dtls_warn("error in check_finished err: %i\n", err);
      return err;
    }
    if (role == DTLS_SERVER) {
      /* send ServerFinished */
      update_hs_hash(peer, data, data_length);

      /* send change cipher spec message and switch to new configuration */
      err = dtls_send_ccs(ctx, peer);
      if (err < 0) {
        dtls_warn("cannot send CCS message\n");
        return err;
      }

      dtls_security_params_switch(peer);

      err = dtls_send_finished(ctx, peer, PRF_LABEL(server), PRF_LABEL_SIZE(server));
      if (err < 0) {
        dtls_warn("sending server Finished failed\n");
        return err;
      }
    }
    dtls_handshake_free(peer->handshake_params);
    peer->handshake_params = NULL;
    dtls_debug("Handshake complete\n");
    peer->state = DTLS_STATE_CONNECTED;

    /* return here to not increase the message receive counter */
    return err;

  /************************************************************************
   * Server states
   ************************************************************************/

  case DTLS_HT_CLIENT_KEY_EXCHANGE:
    /* handle ClientHello, update msg and msglen and goto next if not finished */

    if (state != DTLS_STATE_WAIT_CLIENTKEYEXCHANGE) {
      return dtls_alert_fatal_create(DTLS_ALERT_UNEXPECTED_MESSAGE);
    }

    if(!peer || !peer->handshake_params) {
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }

    err = check_client_keyexchange(ctx, peer->handshake_params, data, data_length);
    if (err < 0) {
      dtls_warn("error in check_client_keyexchange err: %i\n", err);
      return err;
    }
    update_hs_hash(peer, data, data_length);

    if (is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(peer->handshake_params->cipher) &&
	is_ecdsa_client_auth_supported(ctx))
      peer->state = DTLS_STATE_WAIT_CERTIFICATEVERIFY;
    else
      peer->state = DTLS_STATE_WAIT_CHANGECIPHERSPEC;
    break;

#if DTLS_ECC
  case DTLS_HT_CERTIFICATE_VERIFY:

    if (state != DTLS_STATE_WAIT_CERTIFICATEVERIFY) {
      return dtls_alert_fatal_create(DTLS_ALERT_UNEXPECTED_MESSAGE);
    }

    if(!peer || !peer->handshake_params) {
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }

    err = check_client_certificate_verify(ctx, peer, data, data_length);
    if (err < 0) {
      dtls_warn("error in check_client_certificate_verify err: %i\n", err);
      return err;
    }

    update_hs_hash(peer, data, data_length);
    peer->state = DTLS_STATE_WAIT_CHANGECIPHERSPEC;
    break;
#endif /* DTLS_ECC */

  case DTLS_HT_CLIENT_HELLO:

    if ((peer && state != DTLS_STATE_CONNECTED && state != DTLS_STATE_WAIT_CLIENTHELLO) ||
	(!peer && state != DTLS_STATE_WAIT_CLIENTHELLO)) {
      return dtls_alert_fatal_create(DTLS_ALERT_UNEXPECTED_MESSAGE);
    }

    /* When no DTLS state exists for this peer, we only allow a
       Client Hello message with

       a) a valid cookie, or
       b) no cookie.

       Anything else will be rejected. Fragementation is not allowed
       here as it would require peer state as well.
    */
    err = dtls_verify_peer(ctx, peer, session, state, data, data_length);
    if (err < 0) {
      dtls_warn("error in dtls_verify_peer err: %i\n", err);
      return err;
    }

    if (err > 0) {
      dtls_debug("server hello verify was sent\n");
      break;
    }

    /* At this point, we have a good relationship with this peer. This
     * state is left for re-negotiation of key material. */
     /* As per RFC 6347 - section 4.2.8 if this is an attempt to
      * rehandshake, we can delete the existing key material
      * as the client has demonstrated reachibility by completing
      * the cookie exchange */
    if (peer && state == DTLS_STATE_WAIT_CLIENTHELLO) {
       dtls_debug("removing the peer\n");
       delete_peer(&ctx->peers, peer);

       dtls_free_peer(peer);
       peer = NULL;
    }
    if (!peer) {
      dtls_debug("creating new peer\n");
      dtls_security_parameters_t *security;

      /* msg contains a Client Hello with a valid cookie, so we can
       * safely create the server state machine and continue with
       * the handshake. */
      peer = dtls_new_peer(session);
      if (!peer) {
        dtls_alert("cannot create peer\n");
        return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
      }
      peer->role = DTLS_SERVER;

      /* Initialize record sequence number to 1 for new peers. The first
       * record with sequence number 0 is a stateless Hello Verify Request.
       */
      security = dtls_security_params(peer);
      security->rseq = 1;

      if (dtls_add_peer(ctx, peer) < 0) {
	dtls_alert("cannot add peer\n");
	dtls_free_peer(peer);
        return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
      }
    }
    if (!peer->handshake_params) {
      dtls_handshake_header_t *hs_header = DTLS_HANDSHAKE_HEADER(data);

      peer->handshake_params = dtls_handshake_new();
      if (!peer->handshake_params)
        return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);

      peer->handshake_params->hs_state.mseq_r = dtls_uint16_to_int(hs_header->message_seq);
      peer->handshake_params->hs_state.mseq_s = 1;
    }

    clear_hs_hash(peer);

    /* First negotiation step: check for PSK
     *
     * Note that we already have checked that msg is a Handshake
     * message containing a ClientHello. dtls_get_cipher() therefore
     * does not check again.
     */
    err = dtls_update_parameters(ctx, peer, data, data_length);
    if (err < 0) {
      dtls_warn("error updating security parameters\n");
      return err;
    }

    /* update finish MAC */
    update_hs_hash(peer, data, data_length);

    err = dtls_send_server_hello_msgs(ctx, peer);
    if (err < 0) {
      return err;
    }
    if (is_tls_ecdhe_ecdsa_with_aes_128_ccm_8(peer->handshake_params->cipher) &&
	is_ecdsa_client_auth_supported(ctx))
      peer->state = DTLS_STATE_WAIT_CLIENTCERTIFICATE;
    else
      peer->state = DTLS_STATE_WAIT_CLIENTKEYEXCHANGE;

    /* after sending the ServerHelloDone, we expect the
     * ClientKeyExchange (possibly containing the PSK id),
     * followed by a ChangeCipherSpec and an encrypted Finished.
     */

    break;

  case DTLS_HT_HELLO_REQUEST:

    if (state != DTLS_STATE_CONNECTED) {
      /* we should just ignore such packets when in handshake */
      return 0;
    }

    if(!peer) {
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }

    if (!peer->handshake_params) {
      peer->handshake_params = dtls_handshake_new();
      if (!peer->handshake_params)
        return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);

      peer->handshake_params->hs_state.mseq_r = 0;
      peer->handshake_params->hs_state.mseq_s = 0;
    }

    /* send ClientHello with empty Cookie */
    err = dtls_send_client_hello(ctx, peer, NULL, 0);
    if (err < 0) {
      dtls_warn("cannot send ClientHello\n");
      return err;
    }
    peer->state = DTLS_STATE_CLIENTHELLO;
    break;

  default:
    dtls_crit("unhandled message %d\n", data[0]);
    return dtls_alert_fatal_create(DTLS_ALERT_UNEXPECTED_MESSAGE);
  }

  if (peer && peer->handshake_params && err >= 0) {
    peer->handshake_params->hs_state.mseq_r++;
  }

  return err;
}

/*
 * Handle handshake messages concatenated into a single record layer message.
 * Currently only handling a flight starting with server_hello.
 * Consider to generalize for all types of messages.
 */
static int
handle_handshake_msg(dtls_context_t *ctx, dtls_peer_t *peer, session_t *session,
		const dtls_peer_type role, const dtls_state_t state,
		uint8_t *data, size_t data_length) {
	int res = 0;
	int record_length = 0;
	int data_index = 0;

	switch (data[0]) {

	case DTLS_HT_SERVER_HELLO:
		while(data_index < data_length) {
			record_length = dtls_uint24_to_int(data+data_index+1) + DTLS_HS_LENGTH;
			res = handle_handshake_record(ctx, peer, session, role, state,
					data+data_index, record_length);
			if (res < 0)
				return res;
			data_index = data_index + record_length;
		}
		return res;
		break;

	default:
		res = handle_handshake_record(ctx, peer, session, role, state,
				data, data_length);
		return res;
	}

}

static int
handle_handshake(dtls_context_t *ctx, dtls_peer_t *peer, session_t *session,
		 const dtls_peer_type role, const dtls_state_t state,
		 uint8_t *data, size_t data_length)
{
  dtls_handshake_header_t *hs_header;
  int res;

  if (data_length < DTLS_HS_LENGTH) {
    dtls_warn("handshake message too short\n");
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);
  }
  hs_header = DTLS_HANDSHAKE_HEADER(data);

  dtls_debug("received handshake packet of type: %s (%i)\n",
	     dtls_handshake_type_to_name(hs_header->msg_type), hs_header->msg_type);

  if (!peer || !peer->handshake_params) {
    /* This is the initial ClientHello */
    if (hs_header->msg_type != DTLS_HT_CLIENT_HELLO && !peer) {
      dtls_warn("If there is no peer only ClientHello is allowed\n");
      return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
    }

    /* This is a ClientHello or Hello Request send when doing TLS renegotiation */
    if (hs_header->msg_type == DTLS_HT_CLIENT_HELLO ||
	hs_header->msg_type == DTLS_HT_HELLO_REQUEST) {
      return handle_handshake_msg(ctx, peer, session, role, state, data,
				  data_length);
    } else {
      dtls_warn("ignore unexpected handshake message\n");
      return 0;
    }
  }

  if (dtls_uint16_to_int(hs_header->message_seq) < peer->handshake_params->hs_state.mseq_r) {
    dtls_warn("The message sequence number is too small, expected %i, got: %i\n",
	      peer->handshake_params->hs_state.mseq_r, dtls_uint16_to_int(hs_header->message_seq));
    return 0;
  } else if (dtls_uint16_to_int(hs_header->message_seq) > peer->handshake_params->hs_state.mseq_r) {
    /* A packet in between is missing, buffer this packet. */
    netq_t *n;

    /* TODO: only add packet that are not too new. */
    if (data_length > DTLS_MAX_BUF) {
      dtls_warn("the packet is too big to buffer for reoder\n");
      return 0;
    }

    netq_t *node = netq_head(&peer->handshake_params->reorder_queue);
    while (node) {
      dtls_handshake_header_t *node_header = DTLS_HANDSHAKE_HEADER(node->data);
      if (dtls_uint16_to_int(node_header->message_seq) == dtls_uint16_to_int(hs_header->message_seq)) {
        dtls_warn("a packet with this sequence number is already stored\n");
        return 0;
      }
      node = netq_next(node);
    }

    n = netq_node_new(data_length);
    if (!n) {
      dtls_warn("no space in reoder buffer\n");
      return 0;
    }

    n->peer = peer;
    n->length = data_length;
    memcpy(n->data, data, data_length);

    if (!netq_insert_node(&peer->handshake_params->reorder_queue, n)) {
      dtls_warn("cannot add packet to reoder buffer\n");
      netq_node_free(n);
    }
    dtls_info("Added packet for reordering\n");
    return 0;
  } else if (dtls_uint16_to_int(hs_header->message_seq) == peer->handshake_params->hs_state.mseq_r) {
    /* Found the expected packet, use this and all the buffered packet */
    int next = 1;

    res = handle_handshake_msg(ctx, peer, session, role, state, data, data_length);
    if (res < 0)
      return res;

    /* We do not know in which order the packet are in the list just search the list for every packet. */
    while (next && peer->handshake_params) {
      next = 0;
      netq_t *node = netq_head(&peer->handshake_params->reorder_queue);
      while (node) {
        dtls_handshake_header_t *node_header = DTLS_HANDSHAKE_HEADER(node->data);

        if (dtls_uint16_to_int(node_header->message_seq) == peer->handshake_params->hs_state.mseq_r) {
          netq_remove(&peer->handshake_params->reorder_queue, node);
          next = 1;
          res = handle_handshake_msg(ctx, peer, session, role, peer->state, node->data, node->length);
          if (res < 0) {
            return res;
          }

          break;
        } else {
          node = netq_next(node);
        }
      }
    }
    return res;
  }
  assert(0);
  return 0;
}

static int
handle_ccs(dtls_context_t *ctx, dtls_peer_t *peer, 
	   uint8_t *record_header, uint8_t *data, size_t data_length)
{
  int err;

  /* A CCS message is handled after a KeyExchange message was
   * received from the client. When security parameters have been
   * updated successfully and a ChangeCipherSpec message was sent
   * by ourself, the security context is switched and the record
   * sequence number is reset. */

  if (!peer || peer->state != DTLS_STATE_WAIT_CHANGECIPHERSPEC) {
    dtls_warn("expected ChangeCipherSpec during handshake\n");
    return 0;
  }

  if (data_length < 1 || data[0] != 1)
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);

  /* Just change the cipher when we are on the same epoch */
  if (peer->role == DTLS_SERVER) {
    err = calculate_key_block(ctx, peer->handshake_params, peer,
			      &peer->session, peer->role);
    if (err < 0) {
      return err;
    }
  }
  
  peer->state = DTLS_STATE_WAIT_FINISHED;

  return 0;
}  

/** 
 * Handles incoming Alert messages. This function returns \c 1 if the
 * connection should be closed and the peer is to be invalidated.
 */
static int
handle_alert(dtls_context_t *ctx, dtls_peer_t *peer, 
	     uint8_t *record_header, uint8_t *data, size_t data_length) {
  int free_peer = 0;		/* indicates whether to free peer */

  if (data_length < 2)
    return dtls_alert_fatal_create(DTLS_ALERT_DECODE_ERROR);

  dtls_info("** Alert: level %d, description %d\n", data[0], data[1]);

  if (!peer) {
    dtls_warn("got an alert for an unknown peer, we probably already removed it, ignore it\n");
    return 0;
  }

  /* The peer object is invalidated for FATAL alerts and close
   * notifies. This is done in two steps.: First, remove the object
   * from our list of peers. After that, the event handler callback is
   * invoked with the still existing peer object. Finally, the storage
   * used by peer is released.
   */
  if (data[0] == DTLS_ALERT_LEVEL_FATAL || data[1] == DTLS_ALERT_CLOSE_NOTIFY) {
    dtls_alert("%d invalidate peer\n", data[1]);

    delete_peer(&ctx->peers, peer);

    dtls_debug_session("removed peer", &peer->session);

    free_peer = 1;

  }

  (void)CALL(ctx, event, &peer->session, 
	     (dtls_alert_level_t)data[0], (unsigned short)data[1]);
  switch (data[1]) {
  case DTLS_ALERT_CLOSE_NOTIFY:
    /* If state is DTLS_STATE_CLOSING, we have already sent a
     * close_notify so, do not send that again. */
    if (peer->state != DTLS_STATE_CLOSING) {
      peer->state = DTLS_STATE_CLOSING;
      dtls_send_alert(ctx, peer, DTLS_ALERT_LEVEL_FATAL, DTLS_ALERT_CLOSE_NOTIFY);
    } else
      peer->state = DTLS_STATE_CLOSED;
    break;
  default:
    ;
  }
  
  if (free_peer) {
    dtls_stop_retransmission(ctx, peer);
    dtls_destroy_peer(ctx, peer, 1);
  }

  return free_peer;
}

static int dtls_alert_send_from_err(dtls_context_t *ctx, dtls_peer_t *peer,
				    session_t *session, int err)
{
  int level;
  int desc;

  if (err < -(1 << 8) && err > -(3 << 8)) {
    level = ((-err) & 0xff00) >> 8;
    desc = (-err) & 0xff;
    if (!peer) {
      peer = dtls_get_peer(ctx, session);
    }
    if (peer) {
      peer->state = DTLS_STATE_CLOSING;
      return dtls_send_alert(ctx, peer, level, desc);
    }
  } else if (err == -1) {
    if (!peer) {
      peer = dtls_get_peer(ctx, session);
    }
    if (peer) {
      peer->state = DTLS_STATE_CLOSING;
      return dtls_send_alert(ctx, peer, DTLS_ALERT_LEVEL_FATAL, DTLS_ALERT_INTERNAL_ERROR);
    }
  }
  return -1;
}

/** 
 * Handles incoming data as DTLS message from given peer.
 */
int
dtls_handle_message(dtls_context_t *ctx, 
		    session_t *session,
		    uint8_t *msg, int msglen) {
  dtls_peer_t *peer = NULL;
  unsigned int rlen;		/* record length */
  uint8_t *data; 			/* (decrypted) payload */
  int data_length;		/* length of decrypted payload 
				   (without MAC and padding) */
  int err;

  /* check if we have DTLS state for addr/port/ifindex */
  peer = dtls_get_peer(ctx, session);

  if (!peer) {
    dtls_debug("dtls_handle_message: PEER NOT FOUND\n");
    dtls_debug_session("peer addr", session);
  } else {
    dtls_debug("dtls_handle_message: FOUND PEER\n");
  }

  while ((rlen = is_record(msg,msglen))) {
    dtls_peer_type role;
    dtls_state_t state;

    dtls_debug("got packet %d (%d bytes)\n", msg[0], rlen);
    if (peer) {
      data_length = decrypt_verify(peer, msg, rlen, &data);
      if (data_length < 0) {
        if (hs_attempt_with_existing_peer(msg, rlen, peer)) {
          data = msg + DTLS_RH_LENGTH;
          data_length = rlen - DTLS_RH_LENGTH;
          state = DTLS_STATE_WAIT_CLIENTHELLO;
          role = DTLS_SERVER;       
        } else {
	  int err =  dtls_alert_fatal_create(DTLS_ALERT_DECRYPT_ERROR);
          dtls_info("decrypt_verify() failed\n");
	  if (peer->state < DTLS_STATE_CONNECTED) {
	    dtls_alert_send_from_err(ctx, peer, &peer->session, err);
	    peer->state = DTLS_STATE_CLOSED;
	    dtls_stop_retransmission(ctx, peer);
	    dtls_destroy_peer(ctx, peer, 1);
	  }
          return err;
        }
      } else {
        role = peer->role;
        state = peer->state;
      }
    } else {
      /* is_record() ensures that msg contains at least a record header */
      data = msg + DTLS_RH_LENGTH;
      data_length = rlen - DTLS_RH_LENGTH;
      state = DTLS_STATE_WAIT_CLIENTHELLO;
      role = DTLS_SERVER;
    }

    dtls_debug_hexdump("receive header", msg, sizeof(dtls_record_header_t));
    dtls_debug_hexdump("receive unencrypted", data, data_length);

    /* Handle received record according to the first byte of the
     * message, i.e. the subprotocol. We currently do not support
     * combining multiple fragments of one type into a single
     * record. */

    switch (msg[0]) {

    case DTLS_CT_CHANGE_CIPHER_SPEC:
      if (peer) {
        dtls_stop_retransmission(ctx, peer);
      }
      err = handle_ccs(ctx, peer, msg, data, data_length);
      if (err < 0) {
	dtls_warn("error while handling ChangeCipherSpec message\n");
	dtls_alert_send_from_err(ctx, peer, session, err);

        /* invalidate peer */
        if(peer) {
          dtls_destroy_peer(ctx, peer, 1);
        }
        peer = NULL;

	return err;
      }
      break;

    case DTLS_CT_ALERT:
      if (peer) {
        dtls_stop_retransmission(ctx, peer);
      }
      err = handle_alert(ctx, peer, msg, data, data_length);
      if (err < 0 || err == 1) {
         dtls_warn("received alert, peer has been invalidated\n");
         /* handle alert has invalidated peer */
         peer = NULL;
         return err < 0 ?err:-1;
      }
      break;

    case DTLS_CT_HANDSHAKE:
      /* Handshake messages other than Finish must use the current
       * epoch, Finish has epoch + 1. */

      if (peer && dtls_security_params(peer)) {
	uint16_t expected_epoch = dtls_security_params(peer)->epoch;
	uint16_t msg_epoch = 
	  dtls_uint16_to_int(DTLS_RECORD_HEADER(msg)->epoch);

	/* The new security parameters must be used for all messages
	 * that are sent after the ChangeCipherSpec message. This
	 * means that the client's Finished message uses epoch + 1
	 * while the server is still in the old epoch.
	 */
	if (role == DTLS_SERVER && state == DTLS_STATE_WAIT_FINISHED) {
	  expected_epoch++;
	}

	if (expected_epoch != msg_epoch) {
          if (hs_attempt_with_existing_peer(msg, rlen, peer)) {
            state = DTLS_STATE_WAIT_CLIENTHELLO;
            role = DTLS_SERVER;
          } else {
	    dtls_warn("Wrong epoch, expected %i, got: %i\n",
		    expected_epoch, msg_epoch);
	    break;
	  }
	}
      }

      err = handle_handshake(ctx, peer, session, role, state, data, data_length);
      if (err < 0) {
	dtls_warn("error while handling handshake packet\n");
	dtls_alert_send_from_err(ctx, peer, session, err);
	return err;
      }
      if (peer && peer->state == DTLS_STATE_CONNECTED) {
	/* stop retransmissions */
	dtls_stop_retransmission(ctx, peer);
	CALL(ctx, event, &peer->session, 0, DTLS_EVENT_CONNECTED);
      }
      break;

    case DTLS_CT_APPLICATION_DATA:
      // dtls_info("** application data:\n");
      if (!peer) {
        dtls_warn("no peer available, send an alert\n");
        // TODO: should we send a alert here?
        return -1;
      }
      dtls_stop_retransmission(ctx, peer);
      CALL(ctx, read, &peer->session, data, data_length);
      break;
    default:
      dtls_info("dropped unknown message of type %d\n",msg[0]);
    }

    /* advance msg by length of ciphertext */
    msg += rlen;
    msglen -= rlen;
  }

  return 0;
}

dtls_context_t *
dtls_new_context(void *app_data) {
  dtls_context_t *c;
  dtls_tick_t now;

  dtls_ticks(&now);

  c = dtls_context_acquire();
  if (!c) {
    goto error;
  }

  memset(c, 0, sizeof(dtls_context_t));
  c->app = app_data;

  if (dtls_fill_random(c->cookie_secret, DTLS_COOKIE_SECRET_LENGTH))
    c->cookie_secret_age = now;
  else 
    goto error;
  
  return c;

 error:
  dtls_alert("cannot create DTLS context\n");
  if (c)
    dtls_free_context(c);
  return NULL;
}

void dtls_reset_peer(dtls_context_t *ctx, dtls_peer_t *peer)
{
    dtls_stop_retransmission(ctx, peer);
    dtls_destroy_peer(ctx, peer, 1);
}

void
dtls_free_context(dtls_context_t *ctx) {
  dtls_peer_t *p, *tmp;

  if (!ctx) {
    return;
  }

  if (ctx->peers) {
    //      LL_FOREACH_SAFE(ctx->peers, p, tmp) {
    p = ctx->peers;
    while(p) {
      tmp = p->next;
      dtls_destroy_peer(ctx, p, 1);
      p = tmp;
    }
  }

  dtls_context_release(ctx);
}

int
dtls_connect_peer(dtls_context_t *ctx, dtls_peer_t *peer) {
  int res;

  assert(peer);
  if (!peer)
    return -1;

  /* check if the same peer is already in our list */
  if (peer == dtls_get_peer(ctx, &peer->session)) {
    dtls_debug("found peer, try to re-connect\n");
    return dtls_renegotiate(ctx, &peer->session);
  }
    
  /* set local peer role to client, remote is server */
  peer->role = DTLS_CLIENT;

  if (dtls_add_peer(ctx, peer) < 0) {
    dtls_alert("cannot add peer\n");
    return -1;
  }

  /* send ClientHello with empty Cookie */
  peer->handshake_params = dtls_handshake_new();
      if (!peer->handshake_params)
        return -1;

  peer->handshake_params->hs_state.mseq_r = 0;
  peer->handshake_params->hs_state.mseq_s = 0;
  res = dtls_send_client_hello(ctx, peer, NULL, 0);
  if (res < 0)
    dtls_warn("cannot send ClientHello\n");
  else 
    peer->state = DTLS_STATE_CLIENTHELLO;

  return res;
}

int
dtls_connect(dtls_context_t *ctx, const session_t *dst) {
  dtls_peer_t *peer;
  int res;

  peer = dtls_get_peer(ctx, dst);
  
  if (!peer)
    peer = dtls_new_peer(dst);

  if (!peer) {
    dtls_crit("cannot create new peer\n");
    return -1;
  }

  res = dtls_connect_peer(ctx, peer);

  /* Invoke event callback to indicate connection attempt or
   * re-negotiation. */
  if (res > 0) {
    CALL(ctx, event, &peer->session, 0, DTLS_EVENT_CONNECT);
  } else if (res == 0) {
    CALL(ctx, event, &peer->session, 0, DTLS_EVENT_RENEGOTIATE);
  }
  
  return res;
}

static void
dtls_retransmit(dtls_context_t *context, netq_t *node) {
  if (!context || !node)
    return;

  /* re-initialize timeout when maximum number of retransmissions are not reached yet */
  if (node->retransmit_cnt < DTLS_DEFAULT_MAX_RETRANSMIT && node->peer) {
      unsigned char sendbuf[DTLS_MAX_BUF];
      size_t len = sizeof(sendbuf);
      int err;
      unsigned char *data = node->data;
      size_t length = node->length;
      dtls_tick_t now;
      dtls_security_parameters_t *security = dtls_security_params_epoch(node->peer, node->epoch);

      dtls_ticks(&now);
      node->retransmit_cnt++;
      node->t = now + (node->timeout << node->retransmit_cnt);
      netq_insert_node(&context->sendqueue, node);
      
      if (node->type == DTLS_CT_HANDSHAKE) {
	dtls_handshake_header_t *hs_header = DTLS_HANDSHAKE_HEADER(data);

	dtls_debug("** retransmit handshake packet of type: %s (%i)\n",
	           dtls_handshake_type_to_name(hs_header->msg_type), hs_header->msg_type);
      } else {
	dtls_debug("** retransmit packet\n");
      }
      
      err = dtls_prepare_record(node->peer, security, node->type, &data, &length,
				1, sendbuf, &len);
      if (err < 0) {
	dtls_warn("can not retransmit packet, err: %i\n", err);
	return;
      }
      dtls_debug_hexdump("retransmit header", sendbuf,
			 sizeof(dtls_record_header_t));
      dtls_debug_hexdump("retransmit unencrypted", node->data, node->length);

      (void)CALL(context, write, &node->peer->session, sendbuf, len);
      return;
  }

  /* no more retransmissions, remove node from system */
  
  dtls_debug("** removed transaction\n");

  /* And finally delete the node */
  netq_node_free(node);
}

static void
dtls_stop_retransmission(dtls_context_t *context, dtls_peer_t *peer) {
  netq_t *node;

  if(peer == NULL) {
    return;
  }

  node = netq_head(&context->sendqueue);
  while (node) {
    if (dtls_session_equals(&node->peer->session, &peer->session)) {
      netq_t *tmp = node;
      node = netq_next(node);
      netq_remove(&context->sendqueue, tmp);
      netq_node_free(tmp);
    } else
      node = netq_next(node);    
  }
}

void
dtls_check_retransmit(dtls_context_t *context, dtls_tick_t *next, int all) {
  dtls_tick_t now;
  netq_t *node = netq_head(&context->sendqueue);

  dtls_ticks(&now);
  while (node && node->t <= now) {
    netq_pop_first(&context->sendqueue);
    dtls_retransmit(context, node);
    node = netq_head(&context->sendqueue);
    /* Check if we chould send out multiple or not */
    if(!all) break;
  }

  if (next) {
    *next = node ? node->t : 0;
  }
}
