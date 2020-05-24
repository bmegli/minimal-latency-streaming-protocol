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


enum {PACKET_MAX_PAYLOAD=1400, PACKET_HEADER_SIZE=8, PAYLOAD_HEADER_SIZE=MLSP_MAX_SUBFRAMES*4};

//some higher level libraries may have optimized routines
//with reads exceeding end of buffer
//e.g. see FFmpeg AV_INPUT_BUFFER_PADDING_SIZE
//this constant allows reserving larger buffer for such case
//this means that the library user may consume
//library data without copying it even in such case
enum {BUFFER_PADDING_SIZE = 32};

/* packet structure
	u16 framenumber
	u16 packets
	u16 packet
	u16 size
	u8[size] payload data
*/

/* data structure (payload)
 * u32 subframe1_size
 * u32 subframe2_size
 * ...
 * u32 subframe_MLSP_MAX_SUBFRAMES_size
 * subframe_1 data
 * subframe_2 data
 * ...
 * subframe_MLSP_MAX_SUBFRAMES_data
/*

/* Subframes overhead
 * - MLSP_MAX_SUBFRAMES * 4 bytes (e.g. 12 bytes for 3 subframes)
 * - on frame level
 * - each frame payload usually spans over multiple packets
 * - but the cost is not paid on packet level, only once on frame level
 * - it may be optimized later (e.g. no or minimal cost paid for single subframe, etc)
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
	uint8_t *received_packets;
	int received_packets_size;
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
static int mlsp_encode_header(struct mlsp *m, uint16_t framenumber, uint16_t packets, uint16_t packet_number, uint16_t packet_size);
static int mlsp_encode_payload_header(struct mlsp *m, const struct mlsp_frame *frame, int offset);
static int mlsp_send_udp(struct mlsp *m, int data_size);
static int mlsp_decode_header(struct mlsp_packet *udp, uint8_t *data, int size);
static int mlsp_decode_payload(const struct mlsp_collected_frame *collected, struct mlsp_frame *frame);
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

	int rx_size;
	socklen_t len = sizeof(rx_size);

	if (getsockopt(m->socket_udp, SOL_SOCKET, SO_RCVBUF, &rx_size, &len) == 0 )
		fprintf(stderr, "mlsp: SO_RCV_BUF is %d\n", rx_size);		


	rx_size = 212992 *10;
/*
	if(setsockopt(m->socket_udp, SOL_SOCKET, SO_RCVBUF, &rx_size, sizeof(rx_size)) == -1)
		fprintf(stderr, "mlsp: failed to set SO_RCV_BUF\n");
*/
	if (getsockopt(m->socket_udp, SOL_SOCKET, SO_RCVBUF, &rx_size, &len) == 0 )
		fprintf(stderr, "mlsp: SO_RCV_BUF set to %d\n", rx_size);		

	return m;
}

void mlsp_close(struct mlsp *m)
{
	if(m == NULL)
		return;

	if(close(m->socket_udp) == -1)
		fprintf(stderr, "mlsp: error while closing socket\n");

	free(m->collected.data);
	free(m->collected.received_packets);
	free(m);
}

static struct mlsp *mlsp_close_and_return_null(struct mlsp *m)
{
	mlsp_close(m);
	return NULL;
}

//the complexity of implementation stems from the facts:
//- library allows specyfing pointers to multiple subframe data
//- in such case we avoid copying subframes into new buffer "in preparation"
//- but instead pacektize them to MTUs as we are sending data
//this is important only if we are sending large amounts of data
//(which is the design goal and original intention behind library development)
//
//TODO - nontheless simplify implementation (keeping the functionality)
int mlsp_send(struct mlsp *m, const struct mlsp_frame *frame)
{
	uint32_t payload = PAYLOAD_HEADER_SIZE;

	for(int i=0;i<MLSP_MAX_SUBFRAMES;++i)
		payload += frame->size[i];

	//if size is not divisible by MAX_PAYLOAD we have additional packet with the rest
	const uint16_t packets = payload / PACKET_MAX_PAYLOAD + ((payload % PACKET_MAX_PAYLOAD) != 0);
	//last packet is smaller unless it is exactly MAX_PAYLOAD size
	const uint16_t last_packet_size = ((payload % PACKET_MAX_PAYLOAD) !=0 ) ? payload % PACKET_MAX_PAYLOAD : PACKET_MAX_PAYLOAD;

	int sub = 0;
	int sub_copied = 0;

	for(uint16_t p=0;p<packets;++p)
	{
		//last packet may be smaller
		uint16_t size = (p < packets-1) ? PACKET_MAX_PAYLOAD : last_packet_size;
		uint16_t size_left = size;

		int offset = mlsp_encode_header(m, frame->framenumber, packets, p, size);

		if(p == 0) //payload header is only in the first packet
		{
			offset += mlsp_encode_payload_header(m, frame, offset);
			size_left -= PAYLOAD_HEADER_SIZE;
		}

		//keep on copying data until there is place
		for(;sub < MLSP_MAX_SUBFRAMES; )
		{
			int sub_left = frame->size[sub] - sub_copied;

			//more data left in current subframe than in packet, copy and send
			if(sub_left > size_left)
			{
				memcpy(m->data+offset, frame->data[sub]+sub_copied, size_left);
				sub_copied += size_left;
				offset += size_left;
				size_left = 0;
				break;
			}
			//otherwise copy everything left and keep copying from next subframes
			memcpy(m->data+offset, frame->data[sub]+sub_copied, sub_left);
			size_left -= sub_left;
			offset += sub_left;
			sub_copied = 0;
			++sub;
		}

		if( mlsp_send_udp(m, size + PACKET_HEADER_SIZE) != MLSP_OK )
			return MLSP_ERROR;

		//printf("mlsp: sent %d bytes\n", size + PACKET_HEADER_SIZE);
	}

	return MLSP_OK;
}

static int mlsp_encode_header(struct mlsp *m, uint16_t framenumber, uint16_t packets, uint16_t packet_number, uint16_t packet_size)
{
	memcpy(m->data, &framenumber, sizeof(framenumber));
	memcpy(m->data+2, &packets, sizeof(packets));
	memcpy(m->data+4, &packet_number, sizeof(packet_number));
	memcpy(m->data+6, &packet_size, sizeof(packet_size));
	return PACKET_HEADER_SIZE;
}

static int mlsp_encode_payload_header(struct mlsp *m, const struct mlsp_frame *frame, int offset)
{
	//encode subframe sizes
	for(int i=0;i<MLSP_MAX_SUBFRAMES;++i, offset+=sizeof(frame->size[0]))
		memcpy(m->data+offset, &frame->size[i], sizeof(frame->size[0]));

	return PAYLOAD_HEADER_SIZE;
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

		if(mlsp_decode_header(&udp, m->data, recv_len) != MLSP_OK)
		{
			fprintf(stderr, "mlsp: ignoring malformed packet\n");
			continue;
		}

		if(udp.framenumber < m->collected.framenumber)
		{
			fprintf(stderr, "mlsp: ignoring packet with older framenumber\n");
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

		if(m->collected.received_packets[udp.packet])
		{
			fprintf(stderr, "mlsp: ignoring packeet (duplicate)\n");
			continue;
		}

		m->collected.received_packets[udp.packet] = 1;

		memcpy(m->collected.data + udp.packet*PACKET_MAX_PAYLOAD, udp.data, udp.size);

		++m->collected.collected_packets;
		m->collected.actual_size += udp.size;

		if(m->collected.collected_packets == udp.packets)
		{
			if(mlsp_decode_payload(&m->collected, &m->frame) == MLSP_OK)
				return &m->frame;

			//we collected mallformed packet, reset the receiver
			mlsp_receive_reset(m);
		}
	}
}

static int mlsp_decode_header(struct mlsp_packet *udp, uint8_t *data, int size)
{
	if(size < 8)
	{
		fprintf(stderr, "mlsp: packet size smaller than MLSP header\n");
		return MLSP_ERROR;
	}
	memcpy(&udp->framenumber, data, sizeof(udp->framenumber));
	memcpy(&udp->packets, data+2, sizeof(udp->packets));
	memcpy(&udp->packet, data+4, sizeof(udp->packet));
	memcpy(&udp->size, data+6, sizeof(udp->size));

	if(size - PACKET_HEADER_SIZE > PACKET_MAX_PAYLOAD)
	{
		fprintf(stderr, "mlsp: packet paylod size would exceed max paylod\n");
		return MLSP_ERROR;
	}
	if(size - PACKET_HEADER_SIZE != udp->size)
	{
		fprintf(stderr, "mlsp: decoded paylod size doesn't match received packet size\n");
		return MLSP_ERROR;
	}

	if(udp->packet >= udp->packets)
	{
		fprintf(stderr, "mlsp: decoded packet would exceed frame packets\n");
		return MLSP_ERROR;
	}

	udp->data=data+PACKET_HEADER_SIZE;
	return MLSP_OK;
}

static int mlsp_decode_payload(const struct mlsp_collected_frame *collected, struct mlsp_frame *frame)
{
	const int SINGLE_SIZE = sizeof(frame->size[0]);
	int offset = PAYLOAD_HEADER_SIZE;

	if(collected->actual_size < PAYLOAD_HEADER_SIZE)
	{
		fprintf(stderr, "mlsp: decoded payload size smaller than MLSP payload header\n");
		return MLSP_ERROR;
	}

	frame->framenumber = collected->framenumber;

	for(int i=0;i<MLSP_MAX_SUBFRAMES;++i)
	{
		memcpy(&frame->size[i], collected->data + i * SINGLE_SIZE, SINGLE_SIZE);
		frame->data[i] = collected->data + offset;
		offset += frame->size[i];
	}

	if(offset != collected->actual_size)
	{
		fprintf(stderr, "mlsp: payload frames size with header doesn't match encoded size\n");
		return MLSP_ERROR;
	}

	return MLSP_OK;
}

static int mlsp_new_frame(struct mlsp *m, struct mlsp_packet *udp)
{
	if(m->collected.framenumber && m->collected.packets != m->collected.collected_packets)
	{
		fprintf(stderr, "mlsp: ignoring incomplete frame: %d/%d\n", m->collected.collected_packets, m->collected.packets);
		for(int i=0;i<m->collected.packets;++i)
			fprintf(stderr, "%d", m->collected.received_packets[i]);
		fprintf(stderr, "\n");
	}

	m->collected.framenumber = udp->framenumber;
	m->collected.actual_size = 0;
	m->collected.packets = udp->packets;
	m->collected.collected_packets = 0;

	if(m->collected.reserved_size < udp->packets * PACKET_MAX_PAYLOAD)
	{
		free(m->collected.data);
		if ( (m->collected.data = malloc ( udp->packets * PACKET_MAX_PAYLOAD + BUFFER_PADDING_SIZE ) ) == NULL)
		{
			fprintf(stderr, "mlsp: not enough memory for frame\n");
			return MLSP_ERROR;
		}
		m->collected.reserved_size = udp->packets * PACKET_MAX_PAYLOAD;
	}

	if(m->collected.received_packets_size < udp->packets)
	{
		free(m->collected.received_packets);
		if ( (m->collected.received_packets = malloc ( udp->packets) ) == NULL )
		{
			fprintf(stderr, "mlsp: not enough memory for recevied frame packets flags\n");
			return MLSP_ERROR;
		}
		m->collected.received_packets_size = udp->packets;
	}

	memset(m->collected.received_packets, 0, udp->packets);

	return MLSP_OK;
}
void mlsp_receive_reset(struct mlsp *m)
{
	m->collected.framenumber = 0;
	m->collected.actual_size = 0;
	m->collected.collected_packets = 0;
	if(m->collected.received_packets)
		memset(m->collected.received_packets, 0, m->collected.received_packets_size);
}
