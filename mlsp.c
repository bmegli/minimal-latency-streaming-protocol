/*
 * MLSP Minimal Latency Streaming Protocol C library implementation
 *
 * Copyright 2019 (C) Bartosz Meglicki <meglickib@gmail.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

#include "mlsp.h"

#include <stdio.h> //fprintf
#include <stdlib.h> //malloc
#include <string.h> //memcpy
#include <errno.h> //errno
#include <unistd.h> //close
#include <netinet/in.h> //socaddr_in
#include <arpa/inet.h> //inet_pton, etc


enum {PACKET_MAX_PAYLOAD=1400, PACKET_HEADER_SIZE=8};

/* packet structure
	u16 framenumber
	u16 packets
	u16 packet
	u16 size
	u8[size] data
*/

//library level packet
struct mlsp_packet
{
	uint16_t framenumber;
	uint16_t packets; //total packets in frame
	uint16_t packet; //current packet
	uint16_t size;
	uint8_t *data;
};

//frame during collection
struct mlsp_collected_frame
{
	uint16_t framenumber;
	uint8_t *data;
	int actual_size;
	int reserved_size;
	int packets; //total packets in frame
	int collected_packets;
};

struct mlsp
{
	int socket_udp;
	struct sockaddr_in address_udp;
	uint8_t data[PACKET_HEADER_SIZE + PACKET_MAX_PAYLOAD]; //single library level packet
	struct mlsp_collected_frame collected; //frame during collection
	struct mlsp_frame frame; //single user level packet
};

static struct mlsp *mlsp_init_common(const struct mlsp_config *config);
static struct mlsp *mlsp_close_and_return_null(struct mlsp *m);
static int mlsp_send_udp(struct mlsp *m, int data_size);
static int mlsp_decode_packet(struct mlsp_packet *udp, uint8_t *data, int size);
static int mlsp_new_frame(struct mlsp *m, struct mlsp_packet *packet);

static struct mlsp *mlsp_init_common(const struct mlsp_config *config)
{
	struct mlsp *m, zero_mlsp = {0};

	if( ( m = (struct mlsp*)malloc(sizeof(struct mlsp))) == NULL )
	{
		fprintf(stderr, "mlsp: not enough memory for mlsp\n");
		return NULL;
	}

	*m = zero_mlsp; //set all members of dynamically allocated struct to 0 in a portable way

	//create a UDP socket
	if ( (m->socket_udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP) ) == -1)
	{
		fprintf(stderr, "mlsp: failed to initialize UDP socket\n");
		return mlsp_close_and_return_null(m);
	}

	memset((char *) &m->address_udp, 0, sizeof(m->address_udp));
	m->address_udp.sin_family = AF_INET;
	m->address_udp.sin_port = htons(config->port);

	//if address was specified set it but don't forget to also:
	//- check if address was specified for client
	//- use INADDR_ANY if address was not specified for server
	if (config->ip != NULL && config->ip[0] != '\0' && !inet_pton(AF_INET, config->ip, &m->address_udp.sin_addr) )
	{
		fprintf(stderr, "mlsp: failed to initialize UDP address\n");
		return mlsp_close_and_return_null(m);
	}

	return m;

}

struct mlsp *mlsp_init_client(const struct mlsp_config *config)
{
	struct mlsp *m=mlsp_init_common(config);

	if(m == NULL)
		return NULL;

	if(config->ip == NULL || config->ip[0] == '\0')
	{
		fprintf(stderr, "mlsp: missing address argument for client\n");
		return mlsp_close_and_return_null(m);
	}

	return m;
}

struct mlsp *mlsp_init_server(const struct mlsp_config *config)
{
	struct mlsp *m=mlsp_init_common(config);
	struct timeval tv;

	if(m == NULL)
		return NULL;

	if(config->ip == NULL || config->ip[0] == '\0')
		m->address_udp.sin_addr.s_addr = htonl(INADDR_ANY);

	//set timeout if necessary
	if(config->timeout_ms > 0)
	{	//TODO - simplify
		tv.tv_sec = config->timeout_ms / 1000;
		tv.tv_usec = (config->timeout_ms-(config->timeout_ms/1000)*1000) * 1000;

		if (setsockopt(m->socket_udp, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0)
		{
			fprintf(stderr, "mlsp: failed to set timetout for socket\n");
			return mlsp_close_and_return_null(m);
		}
	}

	if( bind(m->socket_udp, (struct sockaddr*)&m->address_udp, sizeof(m->address_udp) ) == -1 )
	{
		fprintf(stderr, "mlsp: failed to bind socket to address\n");
		return mlsp_close_and_return_null(m);
	}

	return m;
}

void mlsp_close(struct mlsp *m)
{
	if(m == NULL)
		return;

	if(close(m->socket_udp) == -1)
		fprintf(stderr, "mlsp: error while closing socket\n");

	free(m->collected.data);

	free(m);
}

static struct mlsp *mlsp_close_and_return_null(struct mlsp *m)
{
	mlsp_close(m);
	return NULL;
}


int mlsp_send(struct mlsp *m, const struct mlsp_frame *frame)
{
	//if size is not divisible by MAX_PAYLOAD we have additional packet with the rest
	const uint16_t packets = frame->size / PACKET_MAX_PAYLOAD + ((frame->size % PACKET_MAX_PAYLOAD) != 0);

	for(uint16_t p=0;p<packets;++p)
	{
		//encode header
		memcpy(m->data, &frame->framenumber, sizeof(frame->framenumber));
		memcpy(m->data+2, &packets, sizeof(packets));
		memcpy(m->data+4, &p, sizeof(p));
		uint16_t size = PACKET_MAX_PAYLOAD;
		//last packet is smaller unless it is exactly MAX_PAYLOAD size
		if(p == packets-1 && (frame->size % PACKET_MAX_PAYLOAD) !=0 )
			size = frame->size % PACKET_MAX_PAYLOAD;
		memcpy(m->data+6, &size, sizeof(size));
		//encode payload
		memcpy(m->data+8, frame->data + p * PACKET_MAX_PAYLOAD, size);

		if( mlsp_send_udp(m, size + PACKET_HEADER_SIZE) != MLSP_OK )
			return MLSP_ERROR;
		//temp
		printf("mlsp: sent %d bytes\n", size + PACKET_HEADER_SIZE);
	}

	return MLSP_OK;
}

static int mlsp_send_udp(struct mlsp *m, int data_size)
{
	int result;
	int written=0;

	while(written<data_size)
	{
		if ((result = sendto(m->socket_udp, m->data+written, data_size-written, 0, (struct sockaddr*)&m->address_udp, sizeof(m->address_udp))) == -1)
		{
			fprintf(stderr, "mlsp: failed to send udp data\n");
			return MLSP_ERROR;
		}
		written += result;
	}
	return MLSP_OK;
}

struct mlsp_frame *mlsp_receive(struct mlsp *m, int *error)
{
	int recv_len;
	struct mlsp_packet udp;

	while(1)
	{
		if((recv_len = recvfrom(m->socket_udp, m->data, PACKET_MAX_PAYLOAD+PACKET_HEADER_SIZE, 0, NULL, NULL)) == -1)
		{
			if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINPROGRESS)
				*error = MLSP_TIMEOUT;
			else
				*error = MLSP_ERROR;

			return NULL;
		}

		if( mlsp_decode_packet(&udp, m->data, recv_len) != MLSP_OK)
		{
			fprintf(stderr, "mlsp: ignoring malformed packet\n");
			continue;
		}

		//frame switching
		if(m->collected.framenumber < udp.framenumber || m->collected.data==NULL || m->collected.packets != udp.packets)
			if( ( *error = mlsp_new_frame(m, &udp) ) != MLSP_OK)
				return NULL;

		if(udp.packet*PACKET_MAX_PAYLOAD + udp.size > m->collected.reserved_size)
		{
			fprintf(stderr, "mlsp: ignoring packet (would exceed buffer)\n");
			continue;
		}

		memcpy(m->collected.data + udp.packet*PACKET_MAX_PAYLOAD, udp.data, udp.size);

		++m->collected.collected_packets;
		m->collected.actual_size += udp.size;

		if(m->collected.collected_packets == udp.packets)
		{
			m->frame.framenumber = m->collected.framenumber;
			m->frame.data=m->collected.data;
			m->frame.size = m->collected.actual_size;
			return &m->frame;
		}
	}
}

static int mlsp_decode_packet(struct mlsp_packet *udp, uint8_t *data, int size)
{
	if(size < 8)
		return MLSP_ERROR;
	memcpy(&udp->framenumber, data, sizeof(udp->framenumber));
	memcpy(&udp->packets, data+2, sizeof(udp->packets));
	memcpy(&udp->packet, data+4, sizeof(udp->packet));
	memcpy(&udp->size, data+6, sizeof(udp->size));

	if(size - PACKET_HEADER_SIZE > PACKET_MAX_PAYLOAD)
		return MLSP_ERROR;

	if(size - PACKET_HEADER_SIZE != udp->size)
		return MLSP_ERROR;

	udp->data=data+PACKET_HEADER_SIZE;
	return MLSP_OK;
}

static int mlsp_new_frame(struct mlsp *m, struct mlsp_packet *udp)
{
	m->collected.framenumber = udp->framenumber;
	m->collected.actual_size = 0;
	m->collected.packets = udp->packets;
	m->collected.collected_packets = 0;

	if(m->collected.reserved_size < udp->packets * PACKET_MAX_PAYLOAD)
	{
		free(m->collected.data);
		if ( (m->collected.data = malloc ( udp->packets * PACKET_MAX_PAYLOAD ) ) == NULL)
		{
			fprintf(stderr, "mlsp: not enough memory for frame\n");
			return MLSP_ERROR;
		}
		m->collected.reserved_size = udp->packets * PACKET_MAX_PAYLOAD;
	}

	return MLSP_OK;
}
void mlsp_receive_reset(struct mlsp *m)
{
	m->collected.framenumber = 0;
	m->collected.actual_size = 0;
	m->collected.collected_packets = 0;
}
