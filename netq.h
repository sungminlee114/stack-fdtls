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
 *
 *******************************************************************************/

#ifndef _DTLS_NETQ_H_
#define _DTLS_NETQ_H_

#include "tinydtls.h"
#include "dtls.h"

/**
 * \defgroup netq Network Packet Queue
 * The netq utility functions implement an ordered queue of data packets
 * to send over the network and can also be used to queue received packets
 * from the network.
 * @{
 */

#ifndef NETQ_MAXCNT
#if DTLS_ECC
#define NETQ_MAXCNT 5 /**< maximum number of elements in netq structure */
#elif defined(DTLS_PSK)
#define NETQ_MAXCNT 3 /**< maximum number of elements in netq structure */
#endif
#endif

#ifndef DTLS_MAX_BUF
#error SET_DTLS_MAX_BUF
#endif

/** 
 * Datagrams in the netq_t structure have a fixed maximum size of
 * DTLS_MAX_BUF to simplify memory management on constrained nodes. */ 
typedef unsigned char netq_packet_t[DTLS_MAX_BUF];

typedef struct netq_t {
  struct netq_t *next;

  dtls_tick_t t;	        /**< when to send PDU for the next time */
  unsigned int timeout;		/**< randomized timeout value */

  dtls_peer_t *peer;		/**< remote address */
  uint16_t epoch;
  uint8_t type;
  unsigned char retransmit_cnt;	/**< retransmission counter, will be removed when zero */

  size_t length;		/**< actual length of data */
  netq_packet_t data;		/**< the datagram to send */
} netq_t;

/**
 * Adds a node to the given queue, ordered by their time-stamp t.
 * This function returns @c 0 on error, or non-zero if @p node has
 * been added successfully.
 *
 * @param queue A pointer to the queue head where @p node will be added.
 * @param node  The new item to add.
 * @return @c 0 on error, or non-zero if the new item was added.
 */
int netq_insert_node(netq_t **queue, netq_t *node);

/** Destroys specified node and releases any memory that was allocated
 * for the associated datagram. */
void netq_node_free(netq_t *node);

/** Removes all items from given queue and frees the allocated storage */
void netq_delete_all(netq_t **queue);

/** Creates a new node suitable for adding to a netq_t queue. */
netq_t *netq_node_new(size_t size);

/**
 * Returns a pointer to the first item in given queue or NULL if
 * empty. 
 */
netq_t *netq_head(netq_t **queue);

netq_t *netq_next(netq_t *p);
void netq_remove(netq_t **queue, netq_t *p);

/**
 * Removes the first item in given queue and returns a pointer to the
 * removed element. If queue is empty when netq_pop_first() is called,
 * this function returns NULL.
 */
netq_t *netq_pop_first(netq_t **queue);

void netq_init(void);

/**@}*/

#endif /* _DTLS_NETQ_H_ */
