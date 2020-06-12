/*
 * MLSP Minimal Latency Streaming Protocol C library implementation
 *
 * Copyright 2019-2020 (C) Bartosz Meglicki <meglickib@gmail.com>
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

//some higher level libraries may have optimized routines
//with reads exceeding end of buffer
//e.g. see FFmpeg AV_INPUT_BUFFER_PADDING_SIZE
//this constant allows reserving larger buffer for such case
//this means that the library user may consume
//library data without copying it even in such case
enum {BUFFER_PADDING_SIZE = 32};

/* packet structure
 * u16 framenumber
 * u8 subframes
 * u8 subframe
 * u16 packets
 * u16 packet
 * u8[] payload data
 */

//library level packet
struct mlsp_packet
{
	uint16_t framenumber;
	uint8_t subframes; //total subframes in frame
	uint8_t subframe; //current subframe
	uint16_t packets; //total packets in frame
	uint16_t packet; //current packet
	uint8_t *data;
	uint16_t size; //data size, not in protocol
};

//subframe during collection
struct mlsp_collected_frame
{
	uint8_t *data;
	int actual_size;
	int reserved_size;
	int packets; //total packets in frame
	int collected_packets;
	uint8_t *received_packets; //flags received packets
	int received_packets_size;
};

struct mlsp
{
	int socket_udp;
	struct sockaddr_in address_udp;
	int subframes; //number of logical subframes in frame
	uint16_t framenumber; //currently assembled frame framenumber
	uint8_t data[PACKET_HEADER_SIZE + PACKET_MAX_PAYLOAD]; //single library level packet
	struct mlsp_collected_frame collected[MLSP_MAX_SUBFRAMES]; //frame during collection
	uint8_t received_subframes[MLSP_MAX_SUBFRAMES]; //flags received subframes
	struct mlsp_frame frame; //single user level packet
};

static struct mlsp *mlsp_init_common(const struct mlsp_config *config);
static struct mlsp *mlsp_close_and_return_null(struct mlsp *m);
static int mlsp_send_udp(struct mlsp *m, int data_size);
static int mlsp_decode_header(const struct mlsp *m, int size, struct mlsp_packet *udp);
static void mlsp_decode_payload(const struct mlsp *m, const struct mlsp_packet *udp,
                                struct mlsp_frame *frame);
static void mlsp_new_frame(struct mlsp *m, uint16_t framenumber);
static int mlsp_new_subframe(struct mlsp_collected_frame *collected, struct mlsp_packet *udp);

static struct mlsp *mlsp_init_common(const struct mlsp_config *config)
{
	struct mlsp *m, zero_mlsp = {0};

	if(config->subframes > MLSP_MAX_SUBFRAMES)
	{
		fprintf(stderr, "mlsp: the maximum number of subframes (compile time) exceed\n");
		return NULL;
	}
	
	if( ( m = (struct mlsp*)malloc(sizeof(struct mlsp))) == NULL )
	{
		fprintf(stderr, "mlsp: not enough memory for mlsp\n");
		return NULL;
	}

	*m = zero_mlsp; //set all members of dynamically allocated struct to 0 in a portable way
	m->subframes = config->subframes > 0 ? config->subframes : 1;
	
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
	struct mlsp *m = mlsp_init_common(config);

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

	for(int i=0;i<m->subframes;++i)
	{
		free(m->collected[i].data);
		free(m->collected[i].received_packets);
	}
	free(m);
}

static struct mlsp *mlsp_close_and_return_null(struct mlsp *m)
{
	mlsp_close(m);
	return NULL;
}

int mlsp_send_subframe(struct mlsp *m, uint16_t framenumber, uint8_t subframe, const uint8_t *data, uint32_t data_size)
{
	//if size is not divisible by MAX_PAYLOAD we have additional packet with the rest
	const uint16_t packets = data_size / PACKET_MAX_PAYLOAD + ((data_size % PACKET_MAX_PAYLOAD) != 0);
	//last packet is smaller unless it is exactly MAX_PAYLOAD size
	const uint16_t last_packet_size = ((data_size % PACKET_MAX_PAYLOAD) !=0 ) ? data_size % PACKET_MAX_PAYLOAD : PACKET_MAX_PAYLOAD;

	for(uint16_t p=0;p<packets;++p)
	{
		//encode header
		memcpy(m->data, &framenumber, sizeof(framenumber));
		m->data[2] = m->subframes;
		m->data[3] = subframe;
		memcpy(m->data+4, &packets, sizeof(packets));
		memcpy(m->data+6, &p, sizeof(p));
		
		//encode payload, last packet may be smaller
		uint16_t size = (p < packets-1) ? PACKET_MAX_PAYLOAD : last_packet_size;
		memcpy(m->data+8, data + p * PACKET_MAX_PAYLOAD, size);

		if( mlsp_send_udp(m, size + PACKET_HEADER_SIZE) != MLSP_OK )
			return MLSP_ERROR;
	}

	return MLSP_OK;	
}

int mlsp_send(struct mlsp *m, const struct mlsp_frame *frame)
{
	for(int i = 0;i<m->subframes;++i)
		if( mlsp_send_subframe(m, frame->framenumber, i, frame->data[i], frame->size[i]) != MLSP_OK )
			return MLSP_ERROR;
	
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

		if(mlsp_decode_header(m, recv_len, &udp) != MLSP_OK)
			continue;
		
		if(m->framenumber < udp.framenumber)
			mlsp_new_frame(m, udp.framenumber);

		struct mlsp_collected_frame *collected = &m->collected[udp.subframe];
		
		if( collected->data == NULL || collected->packets != udp.packets)
			if( ( *error = mlsp_new_subframe(collected, &udp) ) != MLSP_OK)
				return NULL;

		if(collected->received_packets[udp.packet])
		{
			fprintf(stderr, "mlsp: ignoring packet (duplicate)\n");
			continue;
		}

		collected->received_packets[udp.packet] = 1;
		memcpy(collected->data + udp.packet*PACKET_MAX_PAYLOAD, udp.data, udp.size);

		++collected->collected_packets;
		collected->actual_size += udp.size;

		if(collected->collected_packets == udp.packets)
		{
			m->received_subframes[udp.subframe] = 1;

			int received = 0;

			for(int i=0;i<udp.subframes;++i)
				received += m->received_subframes[i];

			if(received != udp.subframes)
				continue;
			
			mlsp_decode_payload(m, &udp, &m->frame);
			
			return &m->frame;
		}
	}
}

static int mlsp_decode_header(const struct mlsp *m, int size, struct mlsp_packet *udp)
{
	uint8_t *data = m->data;
	
	if(size < PACKET_HEADER_SIZE)
	{
		fprintf(stderr, "mlsp: packet size smaller than MLSP header\n");
		return MLSP_ERROR;
	}
	
	memcpy(&udp->framenumber, data, sizeof(udp->framenumber));
	udp->subframes = data[2];
	udp->subframe = data[3];
	memcpy(&udp->packets, data+4, sizeof(udp->packets));
	memcpy(&udp->packet, data+6, sizeof(udp->packet));

	udp->size = size - PACKET_HEADER_SIZE;
	
	if(udp->size > PACKET_MAX_PAYLOAD)
	{
		fprintf(stderr, "mlsp: packet paylod size would exceed max paylod\n");
		return MLSP_ERROR;
	}

	if(udp->subframe >= udp->subframes)
	{
		fprintf(stderr, "mlsp: decoded packet would exceed frame subframes\n");
		return MLSP_ERROR;
	}

	if(udp->packet >= udp->packets)
	{
		fprintf(stderr, "mlsp: decoded packet would exceed frame packets\n");
		return MLSP_ERROR;
	}

	if(udp->framenumber < m->framenumber)
	{
		fprintf(stderr, "mlsp: ignoring packet with older framenumber\n");
		return MLSP_ERROR;
	}
	
	if(udp->subframes > m->subframes || udp->subframe >= m->subframes)
	{
		fprintf(stderr, "mlsp: ignoring packet with incorrect subframe(s)\n");
		return MLSP_ERROR;
	}

	udp->data = data + PACKET_HEADER_SIZE;
	return MLSP_OK;
}

static void mlsp_decode_payload(const struct mlsp *m, const struct mlsp_packet *udp,
                               struct mlsp_frame *frame)
{
	frame->framenumber = m->framenumber;

	for(int i=0;i<m->subframes;++i)
	{	//note - we accept lower number of subframes from sender then initialized for receiver
		frame->size[i] = i < udp->subframes ? m->collected[i].actual_size : 0;
		frame->data[i] = i < udp->subframes ? m->collected[i].data : NULL;
	}
}

static void mlsp_new_frame(struct mlsp *m, uint16_t framenumber)
{
	if(m->framenumber)
		for(int s=0;s<m->subframes;++s)
			if(!m->received_subframes[s])
			{
				fprintf(stderr, "mlsp: ignoring incomplete frame %d/%d: %d/%d\n", framenumber, s,
				m->collected[s].collected_packets, m->collected[s].packets);

				for(int i=0;i<m->collected[s].packets;++i)
					fprintf(stderr, "%d", m->collected[s].received_packets[i]);
				fprintf(stderr, "\n");
			}
	
	m->framenumber = framenumber;
	memset(m->received_subframes, 0, MLSP_MAX_SUBFRAMES);

	for(int s=0;s<m->subframes;++s)
	{
		m->collected[s].actual_size = 0;
		m->collected[s].packets = 0;
		m->collected[s].collected_packets = 0;

		if(m->collected[s].received_packets)
			memset(m->collected[s].received_packets, 0, m->collected[s].received_packets_size);
	}
}

static int mlsp_new_subframe(struct mlsp_collected_frame *collected, struct mlsp_packet *udp)
{	
	collected->actual_size = 0;
	collected->packets = udp->packets;
	collected->collected_packets = 0;

	if(collected->reserved_size < udp->packets * PACKET_MAX_PAYLOAD)
	{
		free(collected->data);
		if ( (collected->data = malloc ( udp->packets * PACKET_MAX_PAYLOAD + BUFFER_PADDING_SIZE ) ) == NULL)
		{
			fprintf(stderr, "mlsp: not enough memory for subframe\n");
			return MLSP_ERROR;
		}
		collected->reserved_size = udp->packets * PACKET_MAX_PAYLOAD;
	}

	if(collected->received_packets_size < udp->packets)
	{
		free(collected->received_packets);
		if ( (collected->received_packets = malloc ( udp->packets) ) == NULL )
		{
			fprintf(stderr, "mlsp: not enough memory for recevied subframe packets flags\n");
			return MLSP_ERROR;
		}
		collected->received_packets_size = udp->packets;
	}

	memset(collected->received_packets, 0, udp->packets);

	return MLSP_OK;
}
void mlsp_receive_reset(struct mlsp *m)
{
	m->framenumber = 0;
	mlsp_new_frame(m, 0);
}
