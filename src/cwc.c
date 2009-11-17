/*
 *  tvheadend, CWC interface
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <rpc/des_crypt.h>

#include "tvhead.h"
#include "tcp.h"
#include "psi.h"
#include "tsdemux.h"
#include "ffdecsa/FFdecsa.h"
#include "transports.h"
#include "cwc.h"
#include "notify.h"
#include "atomic.h"
#include "dtable.h"

/**
 *
 */

#define CWC_KEEPALIVE_INTERVAL 30

#define CWS_NETMSGSIZE 240
#define CWS_FIRSTCMDNO 0xe0

typedef enum {
  MSG_CLIENT_2_SERVER_LOGIN = CWS_FIRSTCMDNO,
  MSG_CLIENT_2_SERVER_LOGIN_ACK,
  MSG_CLIENT_2_SERVER_LOGIN_NAK,
  MSG_CARD_DATA_REQ,
  MSG_CARD_DATA,
  MSG_SERVER_2_CLIENT_NAME,
  MSG_SERVER_2_CLIENT_NAME_ACK,
  MSG_SERVER_2_CLIENT_NAME_NAK,
  MSG_SERVER_2_CLIENT_LOGIN,
  MSG_SERVER_2_CLIENT_LOGIN_ACK,
  MSG_SERVER_2_CLIENT_LOGIN_NAK,
  MSG_ADMIN,
  MSG_ADMIN_ACK,
  MSG_ADMIN_LOGIN,
  MSG_ADMIN_LOGIN_ACK,
  MSG_ADMIN_LOGIN_NAK,
  MSG_ADMIN_COMMAND,
  MSG_ADMIN_COMMAND_ACK,
  MSG_ADMIN_COMMAND_NAK,
  MSG_KEEPALIVE = CWS_FIRSTCMDNO + 0x1d
} net_msg_type_t;


/**
 *
 */
TAILQ_HEAD(cwc_queue, cwc);
LIST_HEAD(cwc_transport_list, cwc_transport);
TAILQ_HEAD(cwc_message_queue, cwc_message);
static struct cwc_queue cwcs;
static pthread_cond_t cwc_config_changed;


/**
 *
 */
typedef struct cwc_transport {
  th_descrambler_t ct_head;

  th_transport_t *ct_transport;

  struct cwc *ct_cwc;

  LIST_ENTRY(cwc_transport) ct_link;


  /**
   * Sequence number generated on write (based on cwc_seq), 
   * used to pair reply message (i.e when i CT_STATE_WAIT_REPLY)
   * with cwc_transport
   */
  uint16_t ct_seq;


  /**
   * Current ECM
   */
  uint8_t ct_ecm[256];
  int ct_ecmsize;

  int ct_ecm_reply_pending; /* Waiting for a ECM reply */

  /**
   * Status of the key(s) in ct_keys
   */
  enum {
    CT_UNKNOWN,
    CT_RESOLVED,
    CT_FORBIDDEN
  } ct_keystate;

  void *ct_keys;

  /**
   * CSA
   */
  int ct_cluster_size;
  uint8_t *ct_tsbcluster;
  int ct_fill;

} cwc_transport_t;


/**
 *
 */
typedef struct cwc_message {
  TAILQ_ENTRY(cwc_message) cm_link;
  int cm_len;
  uint8_t cm_data[CWS_NETMSGSIZE];
} cwc_message_t;


/**
 *
 */
typedef struct cwc_provider {
  uint32_t id;
  uint8_t sa[8];
} cwc_provider_t;

/**
 *
 */
typedef struct cwc {
  int cwc_fd;
  int cwc_connected;

  int cwc_retry_delay;

  pthread_cond_t cwc_cond;

  pthread_mutex_t cwc_writer_mutex; 
  pthread_cond_t cwc_writer_cond; 
  int cwc_writer_running;
  struct cwc_message_queue cwc_writeq;

  TAILQ_ENTRY(cwc) cwc_link; /* Linkage protected via global_lock */

  struct cwc_transport_list cwc_transports;

  uint16_t cwc_caid;

  int cwc_seq;

  uint8_t cwc_key[16];

  uint8_t cwc_buf[256];
  int cwc_bufptr;

  /* Card Unique Address */
  uint8_t cwc_ua[8];

  /* Provider IDs */
  cwc_provider_t cwc_providers[256];
  int cwc_num_providers;

  /* From configuration */

  uint8_t cwc_confedkey[14];
  char *cwc_username;
  char *cwc_password;
  char *cwc_password_salted;   /* salted version */
  char *cwc_comment;
  char *cwc_hostname;
  int cwc_port;
  char *cwc_id;
  int cwc_emm;

  const char *cwc_errtxt;

  int cwc_enabled;
  int cwc_running;
  int cwc_reconfigure;
} cwc_t;


/**
 *
 */

static void cwc_transport_destroy(th_descrambler_t *td);
extern char *cwc_krypt(const char *key, const char *salt);


/**
 *
 */
static void 
des_key_parity_adjust(uint8_t *key, uint8_t len)
{
  uint8_t i, j, parity;
  
  for (i = 0; i < len; i++) {
    parity = 1;
    for (j = 1; j < 8; j++) if ((key[i] >> j) & 0x1) parity = ~parity & 0x01;
    key[i] |= parity;
  }
}


/**
 *
 */
static uint8_t *
des_key_spread(uint8_t *normal)
{
  static uint8_t spread[16];

  spread[ 0] = normal[ 0] & 0xfe;
  spread[ 1] = ((normal[ 0] << 7) | (normal[ 1] >> 1)) & 0xfe;
  spread[ 2] = ((normal[ 1] << 6) | (normal[ 2] >> 2)) & 0xfe;
  spread[ 3] = ((normal[ 2] << 5) | (normal[ 3] >> 3)) & 0xfe;
  spread[ 4] = ((normal[ 3] << 4) | (normal[ 4] >> 4)) & 0xfe;
  spread[ 5] = ((normal[ 4] << 3) | (normal[ 5] >> 5)) & 0xfe;
  spread[ 6] = ((normal[ 5] << 2) | (normal[ 6] >> 6)) & 0xfe;
  spread[ 7] = normal[ 6] << 1;
  spread[ 8] = normal[ 7] & 0xfe;
  spread[ 9] = ((normal[ 7] << 7) | (normal[ 8] >> 1)) & 0xfe;
  spread[10] = ((normal[ 8] << 6) | (normal[ 9] >> 2)) & 0xfe;
  spread[11] = ((normal[ 9] << 5) | (normal[10] >> 3)) & 0xfe;
  spread[12] = ((normal[10] << 4) | (normal[11] >> 4)) & 0xfe;
  spread[13] = ((normal[11] << 3) | (normal[12] >> 5)) & 0xfe;
  spread[14] = ((normal[12] << 2) | (normal[13] >> 6)) & 0xfe;
  spread[15] = normal[13] << 1;

  des_key_parity_adjust(spread, 16);
  return spread;
}

/**
 *
 */
static void 
des_random_get(uint8_t *buffer, uint8_t len)
{
  uint8_t idx = 0;
  int randomNo = 0;
  
  for (idx = 0; idx < len; idx++) {
      if (!(idx % 3)) randomNo = rand();
      buffer[idx] = (randomNo >> ((idx % 3) << 3)) & 0xff;
    }
}

/**
 *
 */
static int
des_encrypt(uint8_t *buffer, int len, uint8_t *deskey)
{
  uint8_t checksum = 0;
  uint8_t noPadBytes;
  uint8_t padBytes[7];
  char ivec[8];
  uint16_t i;

  if (!deskey) return len;
  noPadBytes = (8 - ((len - 1) % 8)) % 8;
  if (len + noPadBytes + 1 >= CWS_NETMSGSIZE-8) return -1;
  des_random_get(padBytes, noPadBytes);
  for (i = 0; i < noPadBytes; i++) buffer[len++] = padBytes[i];
  for (i = 2; i < len; i++) checksum ^= buffer[i];
  buffer[len++] = checksum;
  des_random_get((uint8_t *)ivec, 8);
  memcpy(buffer+len, ivec, 8);
  for (i = 2; i < len; i += 8) {
    cbc_crypt((char *)deskey  , (char *) buffer+i, 8, DES_ENCRYPT, ivec);
    ecb_crypt((char *)deskey+8, (char *) buffer+i, 8, DES_DECRYPT);
    ecb_crypt((char *)deskey  , (char *) buffer+i, 8, DES_ENCRYPT);
    memcpy(ivec, buffer+i, 8);
  }
  len += 8;
  return len;
}

/**
 *
 */
static int 
des_decrypt(uint8_t *buffer, int len, uint8_t *deskey)
{
  char ivec[8];
  char nextIvec[8];
  int i;
  uint8_t checksum = 0;

  if (!deskey) return len;
  if ((len-2) % 8 || (len-2) < 16) return -1;
  len -= 8;
  memcpy(nextIvec, buffer+len, 8);
  for (i = 2; i < len; i += 8)
    {
      memcpy(ivec, nextIvec, 8);
      memcpy(nextIvec, buffer+i, 8);
      ecb_crypt((char *)deskey  , (char *) buffer+i, 8, DES_DECRYPT);
      ecb_crypt((char *)deskey+8, (char *) buffer+i, 8, DES_ENCRYPT);
      cbc_crypt((char *)deskey  , (char *) buffer+i, 8, DES_DECRYPT, ivec);
    } 
  for (i = 2; i < len; i++) checksum ^= buffer[i];
  if (checksum) return -1;
  return len;
}

/**
 *
 */
static void
des_make_login_key(cwc_t *cwc, uint8_t *k)
{
  uint8_t des14[14];
  int i;

  for (i = 0; i < 14; i++) 
    des14[i] = cwc->cwc_confedkey[i] ^ k[i];
  memcpy(cwc->cwc_key, des_key_spread(des14), 16);
}

/**
 *
 */
static void
des_make_session_key(cwc_t *cwc)
{
  uint8_t des14[14], *k2 = (uint8_t *)cwc->cwc_password_salted;
  int i, l = strlen(cwc->cwc_password_salted);

  memcpy(des14, cwc->cwc_confedkey, 14);

  for (i = 0; i < l; i++)
    des14[i % 14] ^= k2[i];

  memcpy(cwc->cwc_key, des_key_spread(des14), 16);
}

/**
 * Note, this function is called from multiple threads so beware of
 * locking / race issues (Note how we use atomic_add() to generate
 * the ID)
 */
static int
cwc_send_msg(cwc_t *cwc, const uint8_t *msg, size_t len, int sid, int enq)
{
  cwc_message_t *cm = malloc(sizeof(cwc_message_t));
  uint8_t *buf = cm->cm_data;
  int seq, n;

  if(len + 12 > CWS_NETMSGSIZE)
    return -1;

  memset(buf, 0, 12);
  memcpy(buf + 12, msg, len);

  len += 12;

  seq = atomic_add(&cwc->cwc_seq, 1);

  buf[2] = seq >> 8;
  buf[3] = seq;
  buf[4] = sid >> 8;
  buf[5] = sid;

  if((len = des_encrypt(buf, len, cwc->cwc_key)) < 0) {
    free(buf);
    return -1;
  }

  buf[0] = (len - 2) >> 8;
  buf[1] =  len - 2;


  if(enq) {
    cm->cm_len = len;
    pthread_mutex_lock(&cwc->cwc_writer_mutex);
    TAILQ_INSERT_TAIL(&cwc->cwc_writeq, cm, cm_link);
    pthread_cond_signal(&cwc->cwc_writer_cond);
    pthread_mutex_unlock(&cwc->cwc_writer_mutex);
  } else {
    n = write(cwc->cwc_fd, buf, len);
    free(cm);
  }
  return seq & 0xffff;
}



/**
 * Card data command
 */

static void
cwc_send_data_req(cwc_t *cwc)
{
  uint8_t buf[CWS_NETMSGSIZE];

  buf[0] = MSG_CARD_DATA_REQ;
  buf[1] = 0;
  buf[2] = 0;

  cwc_send_msg(cwc, buf, 3, 0, 0);
}


/**
 * Send keep alive
 */
static void
cwc_send_ka(cwc_t *cwc)
{
  uint8_t buf[CWS_NETMSGSIZE];

  buf[0] = MSG_KEEPALIVE;
  buf[1] = 0;
  buf[2] = 0;

  cwc_send_msg(cwc, buf, 3, 0, 0);
}

static void cwc_comet_status_update(cwc_t *cwc){
  htsmsg_t *m = htsmsg_create_map();

  htsmsg_add_str(m, "id", cwc->cwc_id);
  htsmsg_add_u32(m, "connected", !!cwc->cwc_connected);
  notify_by_msg("cwcStatus", m);
}

/**
 * Handle reply to card data request
 */
static int
cwc_decode_card_data_reply(cwc_t *cwc, uint8_t *msg, int len)
{
  int plen, i;
  unsigned int nprov;
  const char *n;

  msg += 12;
  len -= 12;

  if(len < 3) {
    tvhlog(LOG_INFO, "cwc", "Invalid card data reply");
    return -1;
  }

  plen = (msg[1] & 0xf) << 8 | msg[2];

  if(plen < 14) {
    tvhlog(LOG_INFO, "cwc", "Invalid card data reply (message)");
    return -1;
  }

  nprov = msg[14];

  if(plen < nprov * 11) {
    tvhlog(LOG_INFO, "cwc", "Invalid card data reply (provider list)");
    return -1;
  }

  cwc->cwc_connected = 1;
  cwc_comet_status_update(cwc);
  cwc->cwc_caid = (msg[4] << 8) | msg[5];
  n = psi_caid2name(cwc->cwc_caid) ?: "Unknown";

  memcpy(cwc->cwc_ua, &msg[6], 8);

  tvhlog(LOG_INFO, "cwc", "%s: Connected as user 0x%02x "
	 "to a %s-card [0x%04x : %02x.%02x.%02x.%02x.%02x.%02x.%02x.%02x] "
	 "with %d providers", 
	 cwc->cwc_hostname,
	 msg[3], n, cwc->cwc_caid, 
	 cwc->cwc_ua[0], cwc->cwc_ua[1], cwc->cwc_ua[2], cwc->cwc_ua[3], cwc->cwc_ua[4], cwc->cwc_ua[5], cwc->cwc_ua[6], cwc->cwc_ua[7],
	 nprov);

  msg  += 15;
  plen -= 12;

  cwc->cwc_num_providers = nprov;

  for(i = 0; i < nprov; i++) {
    cwc->cwc_providers[i].id = (msg[0] << 16) | (msg[1] << 8) | msg[2];
    cwc->cwc_providers[i].sa[0] = msg[3];
    cwc->cwc_providers[i].sa[1] = msg[4];
    cwc->cwc_providers[i].sa[2] = msg[5];
    cwc->cwc_providers[i].sa[3] = msg[6];
    cwc->cwc_providers[i].sa[4] = msg[7];
    cwc->cwc_providers[i].sa[5] = msg[8];
    cwc->cwc_providers[i].sa[6] = msg[9];
    cwc->cwc_providers[i].sa[7] = msg[10];

    tvhlog(LOG_INFO, "cwc", "%s: Provider ID #%d: 0x%06x %02x.%02x.%02x.%02x.%02x.%02x.%02x.%02x",
	   cwc->cwc_hostname, i + 1,
	   cwc->cwc_providers[i].id,
	   cwc->cwc_providers[i].sa[0],
	   cwc->cwc_providers[i].sa[1],
	   cwc->cwc_providers[i].sa[2],
	   cwc->cwc_providers[i].sa[3],
	   cwc->cwc_providers[i].sa[4],
	   cwc->cwc_providers[i].sa[5],
	   cwc->cwc_providers[i].sa[6],
	   cwc->cwc_providers[i].sa[7]);

    msg += 11;
  }

  tvhlog(LOG_INFO, "cwc", "%s: Will %sforward EMMs",
	 cwc->cwc_hostname,
	 cwc->cwc_emm ? "" : "not "); 

  return 0;
}


/**
 * Login command
 */
static void
cwc_send_login(cwc_t *cwc)
{
  uint8_t buf[CWS_NETMSGSIZE];
  int ul = strlen(cwc->cwc_username) + 1;
  int pl = strlen(cwc->cwc_password_salted) + 1;

  buf[0] = MSG_CLIENT_2_SERVER_LOGIN;
  buf[1] = 0;
  buf[2] = ul + pl;
  memcpy(buf + 3,      cwc->cwc_username, ul);
  memcpy(buf + 3 + ul, cwc->cwc_password_salted, pl);
 
  cwc_send_msg(cwc, buf, ul + pl + 3, 0, 0);
}

/**
 * Handle running reply
 * global_lock is held
 */
static int
cwc_running_reply(cwc_t *cwc, uint8_t msgtype, uint8_t *msg, int len)
{
  cwc_transport_t *ct;
  uint16_t seq = (msg[2] << 8) | msg[3];
  th_transport_t *t;
  int j;

  len -= 12;
  msg += 12;

  switch(msgtype) {
  case 0x80:
  case 0x81:
    LIST_FOREACH(ct, &cwc->cwc_transports, ct_link) {
      if(ct->ct_seq == seq && ct->ct_ecm_reply_pending)
	break;
    }

    if(ct == NULL)
      return 0;

    t = ct->ct_transport;
    ct->ct_ecm_reply_pending = 0;

    if(len < 19) {

      if(ct->ct_keystate != CT_FORBIDDEN) {
	tvhlog(LOG_ERR, "cwc",
	       "Can not descramble service \"%s\", access denied",
	       t->tht_svcname);
	ct->ct_keystate = CT_FORBIDDEN;
      }

      return 0;
    }

    tvhlog(LOG_DEBUG, "cwc",
	   "Received ECM reply for service \"%s\" "
	   "even: %02x.%02x.%02x.%02x.%02x.%02x.%02x.%02x"
	   " odd: %02x.%02x.%02x.%02x.%02x.%02x.%02x.%02x ",
	   t->tht_svcname,
	   msg[3 + 0], msg[3 + 1], msg[3 + 2], msg[3 + 3], msg[3 + 4],
	   msg[3 + 5], msg[3 + 6], msg[3 + 7], msg[3 + 8], msg[3 + 9],
	   msg[3 + 10],msg[3 + 11],msg[3 + 12],msg[3 + 13],msg[3 + 14],
	   msg[3 + 15]);

    if(ct->ct_keystate != CT_RESOLVED)
      tvhlog(LOG_INFO, "cwc",
	     "Obtained key for for service \"%s\"",t->tht_svcname);
    
    ct->ct_keystate = CT_RESOLVED;
    pthread_mutex_lock(&t->tht_stream_mutex);

    for(j = 0; j < 8; j++)
      if(msg[3 + j]) {
	set_even_control_word(ct->ct_keys, msg + 3);
	break;
      }

    for(j = 0; j < 8; j++)
      if(msg[3 + 8 +j]) {
	set_odd_control_word(ct->ct_keys, msg + 3 + 8);
	break;
      }

    pthread_mutex_unlock(&t->tht_stream_mutex);
    break;
  default:
    // EMM
    break;
  }
  return 0;
}


/**
 *
 */
static int
cwc_must_break(cwc_t *cwc)
{
  return !cwc->cwc_running || !cwc->cwc_enabled || cwc->cwc_reconfigure;
}

/**
 *
 */
static int
cwc_read(cwc_t *cwc, void *buf, size_t len, int timeout)
{
  int r;

  pthread_mutex_unlock(&global_lock);
  r = tcp_read_timeout(cwc->cwc_fd, buf, len, timeout);
  pthread_mutex_lock(&global_lock);

  if(cwc_must_break(cwc))
    return ECONNABORTED;

  return r;
}


/**
 *
 */
static int
cwc_read_message(cwc_t *cwc, const char *state, int timeout)
{
  char buf[2];
  int msglen, r;

  if((r = cwc_read(cwc, buf, 2, timeout))) {
    tvhlog(LOG_INFO, "cwc", "%s: %s: Read error (header): %s",
	   cwc->cwc_hostname, state, strerror(r));
    return -1;
  }

  msglen = (buf[0] << 8) | buf[1];
  if(msglen >= CWS_NETMSGSIZE) {
    tvhlog(LOG_INFO, "cwc", "%s: %s: Invalid message size: %d",
	   cwc->cwc_hostname, state, msglen);
    return -1;
  }

  /* We expect the rest of the message to arrive fairly quick,
     so just wait 1 second here */

  if((r = cwc_read(cwc, cwc->cwc_buf + 2, msglen, 1000))) {
    tvhlog(LOG_INFO, "cwc", "%s: %s: Read error: %s",
	   cwc->cwc_hostname, state, strerror(r));
    return -1;
  }

  if((msglen = des_decrypt(cwc->cwc_buf, msglen + 2, cwc->cwc_key)) < 15) {
    tvhlog(LOG_INFO, "cwc", "%s: %s: Decrypt failed",
	   state, cwc->cwc_hostname);
    return -1;
  }
  return msglen;
}

/**
 *
 */
static void *
cwc_writer_thread(void *aux)
{
  cwc_t *cwc = aux;
  cwc_message_t *cm;
  struct timespec ts;
  int r;

  pthread_mutex_lock(&cwc->cwc_writer_mutex);

  while(cwc->cwc_writer_running) {

    if((cm = TAILQ_FIRST(&cwc->cwc_writeq)) != NULL) {
      TAILQ_REMOVE(&cwc->cwc_writeq, cm, cm_link);
      pthread_mutex_unlock(&cwc->cwc_writer_mutex);
      //      int64_t ts = getmonoclock();
      r = write(cwc->cwc_fd, cm->cm_data, cm->cm_len);
      //      printf("Write took %lld usec\n", getmonoclock() - ts);
      free(cm);
      pthread_mutex_lock(&cwc->cwc_writer_mutex);
      continue;
    }


    /* If nothing is to be sent in CWC_KEEPALIVE_INTERVAL seconds we
       need to send a keepalive */
    ts.tv_sec  = time(NULL) + CWC_KEEPALIVE_INTERVAL;
    ts.tv_nsec = 0;
    r = pthread_cond_timedwait(&cwc->cwc_writer_cond, 
			       &cwc->cwc_writer_mutex, &ts);
    if(r == ETIMEDOUT)
      cwc_send_ka(cwc);
  }

  pthread_mutex_unlock(&cwc->cwc_writer_mutex);
  return NULL;
}



/**
 *
 */
static void
cwc_session(cwc_t *cwc)
{
  int r;
  pthread_t writer_thread_id;

  /**
   * Get login key
   */
  if((r = cwc_read(cwc, cwc->cwc_buf, 14, 5000))) {
    tvhlog(LOG_INFO, "cwc", "%s: No login key received: %s",
	   cwc->cwc_hostname, strerror(r));
    return;
  }

  des_make_login_key(cwc, cwc->cwc_buf);

  /**
   * Login
   */
  cwc_send_login(cwc);
  
  if(cwc_read_message(cwc, "Wait login response", 5000) < 0)
    return;

  if(cwc->cwc_buf[12] != MSG_CLIENT_2_SERVER_LOGIN_ACK) {
    tvhlog(LOG_INFO, "cwc", "%s: Login failed", cwc->cwc_hostname);
    return;
  }

  des_make_session_key(cwc);

  /**
   * Request card data
   */
  cwc_send_data_req(cwc);
  if((r = cwc_read_message(cwc, "Request card data", 5000)) < 0)
    return;

  if(cwc->cwc_buf[12] != MSG_CARD_DATA) {
    tvhlog(LOG_INFO, "cwc", "%s: Card data request failed", cwc->cwc_hostname);
    return;
  }

  if(cwc_decode_card_data_reply(cwc, cwc->cwc_buf, r) < 0)
    return;

  /**
   * Ok, connection good, reset retry delay to zero
   */
  cwc->cwc_retry_delay = 0;

  /**
   * We do all requests from now on in a separate thread
   */
  cwc->cwc_writer_running = 1;
  pthread_cond_init(&cwc->cwc_writer_cond, NULL);
  pthread_mutex_init(&cwc->cwc_writer_mutex, NULL);
  TAILQ_INIT(&cwc->cwc_writeq);
  pthread_create(&writer_thread_id, NULL, cwc_writer_thread, cwc);

  /**
   * Mainloop
   */
  while(!cwc_must_break(cwc)) {

    if((r = cwc_read_message(cwc, "Decoderloop", 
			     CWC_KEEPALIVE_INTERVAL * 2 * 1000)) < 0)
      break;
    cwc_running_reply(cwc, cwc->cwc_buf[12], cwc->cwc_buf, r);
  }

  /**
   * Collect the writer thread
   */
  shutdown(cwc->cwc_fd, SHUT_RDWR);
  cwc->cwc_writer_running = 0;
  pthread_cond_signal(&cwc->cwc_writer_cond);
  pthread_join(writer_thread_id, NULL);
  tvhlog(LOG_DEBUG, "cwc", "Write thread joined");
}


/**
 *
 */
static void *
cwc_thread(void *aux)
{
  cwc_transport_t *ct;
  cwc_t *cwc = aux;
  int fd, d;
  char errbuf[100];
  th_transport_t *t;
  char hostname[256];
  int port;
  struct timespec ts;
  int attempts = 0;

  pthread_mutex_lock(&global_lock);

  while(cwc->cwc_running) {
    
    while(cwc->cwc_running && cwc->cwc_enabled == 0)
      pthread_cond_wait(&cwc->cwc_cond, &global_lock);

    snprintf(hostname, sizeof(hostname), "%s", cwc->cwc_hostname);
    port = cwc->cwc_port;

    tvhlog(LOG_INFO, "cwc", "Attemping to connect to %s:%d", hostname, port);

    pthread_mutex_unlock(&global_lock);

    fd = tcp_connect(hostname, port, errbuf, sizeof(errbuf), 10);

    pthread_mutex_lock(&global_lock);

    if(fd == -1) {
      attempts++;
      tvhlog(LOG_INFO, "cwc", 
	     "Connection attempt to %s:%d failed: %s",
	     hostname, port, errbuf);
    } else {

      if(cwc->cwc_running == 0) {
	close(fd);
	break;
      }

      tvhlog(LOG_INFO, "cwc", "Connected to %s:%d", hostname, port);
      attempts = 0;

      cwc->cwc_fd = fd;
      cwc->cwc_reconfigure = 0;

      cwc_session(cwc);

      cwc->cwc_fd = -1;
      close(fd);
      cwc->cwc_caid = 0;
      cwc->cwc_connected = 0;
      cwc_comet_status_update(cwc);
      tvhlog(LOG_INFO, "cwc", "Disconnected from %s",  cwc->cwc_hostname);
    }

    if(subscriptions_active()) {
      if(attempts == 1)
	continue; // Retry immediately
      d = 3;
    } else {
      d = 60;
    }

    ts.tv_sec = time(NULL) + d;
    ts.tv_nsec = 0;

    tvhlog(LOG_INFO, "cwc", 
	   "%s: Automatic connection attempt in in %d seconds",
	   cwc->cwc_hostname, d);

    pthread_cond_timedwait(&cwc_config_changed, &global_lock, &ts);
  }

  tvhlog(LOG_INFO, "cwc", "%s destroyed", cwc->cwc_hostname);

  while((ct = LIST_FIRST(&cwc->cwc_transports)) != NULL) {
    t = ct->ct_transport;
    pthread_mutex_lock(&t->tht_stream_mutex);
    cwc_transport_destroy(&ct->ct_head);
    pthread_mutex_unlock(&t->tht_stream_mutex);
  }

  free((void *)cwc->cwc_password);
  free((void *)cwc->cwc_password_salted);
  free((void *)cwc->cwc_username);
  free((void *)cwc->cwc_hostname);
  free(cwc);

  pthread_mutex_unlock(&global_lock);
  return NULL;
}


/**
 *
 */
static int
verify_provider(cwc_t *cwc, uint32_t providerid)
{
  int i;

  if(providerid == 0)
    return 1;
  
  for(i = 0; i < cwc->cwc_num_providers; i++) 
    if(providerid == cwc->cwc_providers[i].id)
      return 1;
  return 0;
}


/**
 *
 */
void
cwc_emm(uint8_t *data, int len)
{
  cwc_t *cwc;

  lock_assert(&global_lock);

  TAILQ_FOREACH(cwc, &cwcs, cwc_link)
    if(cwc->cwc_emm && cwc->cwc_writer_running &&
       cwc->cwc_caid == 0x0b00 && data[0] == 0x82 /* Conax */ ) {
      int i;
      for (i=0; i < cwc->cwc_num_providers; i++)
	if (memcmp(&data[3], &cwc->cwc_providers[i].sa[1], 7) == 0)
	  cwc_send_msg(cwc, data, len, 0, 1);
    }
}


/**
 *
 */
static void
cwc_table_input(struct th_descrambler *td, struct th_transport *t,
		struct th_stream *st, uint8_t *data, int len)
{
  cwc_transport_t *ct = (cwc_transport_t *)td;
  uint16_t sid = t->tht_dvb_service_id;
  cwc_t *cwc = ct->ct_cwc;

  if(cwc->cwc_caid != st->st_caid)
    return;

  if(!verify_provider(cwc, st->st_providerid))
    return;

  if((data[0] & 0xf0) != 0x80)
    return;

  switch(data[0]) {
  case 0x80:
  case 0x81:
    /* ECM */

    if(ct->ct_ecm_reply_pending)
      break;

    if(ct->ct_ecmsize == len && !memcmp(ct->ct_ecm, data, len))
      break; /* key already sent */


    if(cwc->cwc_fd == -1) {
      // New key, but we are not connected (anymore), can not descramble
      ct->ct_keystate = CT_UNKNOWN;
      break;
    }

    tvhlog(LOG_DEBUG, "cwc", "Sending ECM for service %s", t->tht_svcname);

    memcpy(ct->ct_ecm, data, len);
    ct->ct_ecmsize = len;
    ct->ct_seq = cwc_send_msg(cwc, data, len, sid, 1);
    ct->ct_ecm_reply_pending = 1;
    break;

  default:
    /* EMM */
    if (cwc->cwc_emm)
      cwc_send_msg(cwc, data, len, sid, 1);
    break;
  }
}


/**
 *
 */
static int
cwc_descramble(th_descrambler_t *td, th_transport_t *t, struct th_stream *st,
	       uint8_t *tsb)
{
  cwc_transport_t *ct = (cwc_transport_t *)td;
  int r, i;
  unsigned char *vec[3];
  uint8_t *t0;

  if(ct->ct_keystate == CT_FORBIDDEN)
    return 1;

  if(ct->ct_keystate != CT_RESOLVED)
    return -1;

  memcpy(ct->ct_tsbcluster + ct->ct_fill * 188, tsb, 188);
  ct->ct_fill++;

  if(ct->ct_fill != ct->ct_cluster_size)
    return 0;

  ct->ct_fill = 0;

  vec[0] = ct->ct_tsbcluster;
  vec[1] = ct->ct_tsbcluster + ct->ct_cluster_size * 188;
  vec[2] = NULL;

  r = decrypt_packets(ct->ct_keys, vec);
  if(r == 0)
    return 0;

  t0 = ct->ct_tsbcluster;
  for(i = 0; i < r; i++) {
    ts_recv_packet2(t, t0);
    t0 += 188;
  }

  i = ct->ct_cluster_size - r;
  assert(i >= 0);

  if(i > 0) {
    memmove(ct->ct_tsbcluster, t0, i * 188);
    ct->ct_fill = i;
  }

  return 0;
}

/**
 * global_lock is held
 * tht_stream_mutex is held
 */
static void 
cwc_transport_destroy(th_descrambler_t *td)
{
  cwc_transport_t *ct = (cwc_transport_t *)td;

  LIST_REMOVE(td, td_transport_link);

  LIST_REMOVE(ct, ct_link);

  free_key_struct(ct->ct_keys);
  free(ct->ct_tsbcluster);
  free(ct);
}

/**
 *
 */
static inline th_stream_t *
cwc_find_stream_by_caid(th_transport_t *t, int caid)
{
  th_stream_t *st;

  LIST_FOREACH(st, &t->tht_components, st_link) {
    if(st->st_caid == caid)
      return st;
  }
  return NULL;
}


/**
 * Check if our CAID's matches, and if so, link
 *
 * global_lock is held
 */
void
cwc_transport_start(th_transport_t *t)
{
  cwc_t *cwc;
  cwc_transport_t *ct;
  th_descrambler_t *td;
  th_stream_t *st;

  lock_assert(&global_lock);

  TAILQ_FOREACH(cwc, &cwcs, cwc_link) {
    if(cwc->cwc_caid == 0)
      continue;

    if((st = cwc_find_stream_by_caid(t, cwc->cwc_caid)) == NULL)
      continue;

    ct = calloc(1, sizeof(cwc_transport_t));
    ct->ct_cluster_size = get_suggested_cluster_size();
    ct->ct_tsbcluster = malloc(ct->ct_cluster_size * 188);

    ct->ct_keys = get_key_struct();
    ct->ct_cwc = cwc;
    ct->ct_transport = t;
     
    td = &ct->ct_head;
    td->td_stop       = cwc_transport_destroy;
    td->td_table      = cwc_table_input;
    td->td_descramble = cwc_descramble;
    LIST_INSERT_HEAD(&t->tht_descramblers, td, td_transport_link);

    LIST_INSERT_HEAD(&cwc->cwc_transports, ct, ct_link);
  }
}


/**
 *
 */
static void
cwc_destroy(cwc_t *cwc)
{
  lock_assert(&global_lock);
  TAILQ_REMOVE(&cwcs, cwc, cwc_link);  
  cwc->cwc_running = 0;
  pthread_cond_signal(&cwc->cwc_cond);
}


/**
 *
 */
static cwc_t *
cwc_entry_find(const char *id, int create)
{
  pthread_attr_t attr;
  pthread_t ptid;
  char buf[20];
  cwc_t *cwc;
  static int tally;

  if(id != NULL) {
    TAILQ_FOREACH(cwc, &cwcs, cwc_link)
      if(!strcmp(cwc->cwc_id, id))
	return cwc;
  }
  if(create == 0)
    return NULL;

  if(id == NULL) {
    tally++;
    snprintf(buf, sizeof(buf), "%d", tally);
    id = buf;
  } else {
    tally = MAX(atoi(id), tally);
  }

  cwc = calloc(1, sizeof(cwc_t));
  pthread_cond_init(&cwc->cwc_cond, NULL);
  cwc->cwc_id = strdup(id); 
  cwc->cwc_running = 1;
  TAILQ_INSERT_TAIL(&cwcs, cwc, cwc_link);  

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&ptid, &attr, cwc_thread, cwc);
  pthread_attr_destroy(&attr);

  return cwc;
}


/**
 *
 */
static htsmsg_t *
cwc_record_build(cwc_t *cwc)
{
  htsmsg_t *e = htsmsg_create_map();
  char buf[100];

  htsmsg_add_str(e, "id", cwc->cwc_id);
  htsmsg_add_u32(e, "enabled",  !!cwc->cwc_enabled);
  htsmsg_add_u32(e, "connected", !!cwc->cwc_connected);

  htsmsg_add_str(e, "hostname", cwc->cwc_hostname ?: "");
  htsmsg_add_u32(e, "port", cwc->cwc_port);

  htsmsg_add_str(e, "username", cwc->cwc_username ?: "");
  htsmsg_add_str(e, "password", cwc->cwc_password ?: "");
  snprintf(buf, sizeof(buf),
	   "%02x:%02x:%02x:%02x:%02x:%02x:%02x:"
	   "%02x:%02x:%02x:%02x:%02x:%02x:%02x",
	   cwc->cwc_confedkey[0x0],
	   cwc->cwc_confedkey[0x1],
	   cwc->cwc_confedkey[0x2],
	   cwc->cwc_confedkey[0x3],
	   cwc->cwc_confedkey[0x4],
	   cwc->cwc_confedkey[0x5],
	   cwc->cwc_confedkey[0x6],
	   cwc->cwc_confedkey[0x7],
	   cwc->cwc_confedkey[0x8],
	   cwc->cwc_confedkey[0x9],
	   cwc->cwc_confedkey[0xa],
	   cwc->cwc_confedkey[0xb],
	   cwc->cwc_confedkey[0xc],
	   cwc->cwc_confedkey[0xd]);	   
  
  htsmsg_add_str(e, "deskey", buf);
  htsmsg_add_u32(e, "emm", cwc->cwc_emm);
  htsmsg_add_str(e, "comment", cwc->cwc_comment ?: "");

  return e;
}


/**
 *
 */
static int
nibble(char c)
{
  switch(c) {
  case '0' ... '9':
    return c - '0';
  case 'a' ... 'f':
    return c - 'a' + 10;
  case 'A' ... 'F':
    return c - 'A' + 10;
  default:
    return 0;
  }
}


/**
 *
 */
static htsmsg_t *
cwc_entry_update(void *opaque, const char *id, htsmsg_t *values, int maycreate)
{
  cwc_t *cwc;
  const char *s;
  uint32_t u32;
  uint8_t key[14];
  int u, l, i;

  if((cwc = cwc_entry_find(id, maycreate)) == NULL)
    return NULL;

  lock_assert(&global_lock);
  
  if((s = htsmsg_get_str(values, "username")) != NULL) {
    free(cwc->cwc_username);
    cwc->cwc_username = strdup(s);
  }

  if((s = htsmsg_get_str(values, "password")) != NULL) {
    free(cwc->cwc_password);
    free(cwc->cwc_password_salted);
    cwc->cwc_password = strdup(s);
    cwc->cwc_password_salted = strdup(cwc_krypt(s, "$1$abcdefgh$"));
  }

  if((s = htsmsg_get_str(values, "comment")) != NULL) {
    free(cwc->cwc_comment);
    cwc->cwc_comment = strdup(s);
  }

  if((s = htsmsg_get_str(values, "hostname")) != NULL) {
    free(cwc->cwc_hostname);
    cwc->cwc_hostname = strdup(s);
  }

  if(!htsmsg_get_u32(values, "enabled", &u32))
    cwc->cwc_enabled = u32;

  if(!htsmsg_get_u32(values, "port", &u32))
    cwc->cwc_port = u32;

  if((s = htsmsg_get_str(values, "deskey")) != NULL) {
    for(i = 0; i < 14; i++) {
      while(*s != 0 && !isxdigit(*s)) s++;
      if(*s == 0)
	break;
      u = nibble(*s++);
      while(*s != 0 && !isxdigit(*s)) s++;
      if(*s == 0)
	break;
      l = nibble(*s++);
      key[i] = (u << 4) | l;
    }
    memcpy(cwc->cwc_confedkey, key, 14);
  }

  if(!htsmsg_get_u32(values, "emm", &u32))
    cwc->cwc_emm = u32;

  cwc->cwc_reconfigure = 1;

  if(cwc->cwc_fd != -1)
    shutdown(cwc->cwc_fd, SHUT_RDWR);

  pthread_cond_signal(&cwc->cwc_cond);

  pthread_cond_broadcast(&cwc_config_changed);

  return cwc_record_build(cwc);
}
  


/**
 *
 */
static int
cwc_entry_delete(void *opaque, const char *id)
{
  cwc_t *cwc;

  pthread_cond_broadcast(&cwc_config_changed);

  if((cwc = cwc_entry_find(id, 0)) == NULL)
    return -1;
  cwc_destroy(cwc);
  return 0;
}


/**
 *
 */
static htsmsg_t *
cwc_entry_get_all(void *opaque)
{
  htsmsg_t *r = htsmsg_create_list();
  cwc_t *cwc;

  TAILQ_FOREACH(cwc, &cwcs, cwc_link)
    htsmsg_add_msg(r, NULL, cwc_record_build(cwc));

  return r;
}

/**
 *
 */
static htsmsg_t *
cwc_entry_get(void *opaque, const char *id)
{
  cwc_t *cwc;


  if((cwc = cwc_entry_find(id, 0)) == NULL)
    return NULL;
  return cwc_record_build(cwc);
}

/**
 *
 */
/**
 *
 */
static htsmsg_t *
cwc_entry_create(void *opaque)
{
  pthread_cond_broadcast(&cwc_config_changed);
  return cwc_record_build(cwc_entry_find(NULL, 1));
}




/**
 *
 */
static const dtable_class_t cwc_dtc = {
  .dtc_record_get     = cwc_entry_get,
  .dtc_record_get_all = cwc_entry_get_all,
  .dtc_record_create  = cwc_entry_create,
  .dtc_record_update  = cwc_entry_update,
  .dtc_record_delete  = cwc_entry_delete,
  .dtc_read_access = ACCESS_ADMIN,
  .dtc_write_access = ACCESS_ADMIN,
};



/**
 *
 */
void
cwc_init(void)
{
  dtable_t *dt;

  TAILQ_INIT(&cwcs);

  pthread_cond_init(&cwc_config_changed, NULL);

  dt = dtable_create(&cwc_dtc, "cwc", NULL);
  dtable_load(dt);
}
