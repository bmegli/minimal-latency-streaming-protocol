#include "mlsp.h"

#include <stdio.h> //fprintf
#include <stdlib.h> //malloc
#include <string.h> //memcpy
#include <unistd.h> //close
#include <netinet/in.h> //socaddr_in
#include <arpa/inet.h> //inet_pton, etc

enum {FRAME_MAX_PAYLOAD=1400, FRAME_HEADER_SIZE=8};

/* packet structure
    u16 framenumber 
    u16 packets
    u16 packet 
    u16 size  
    u8[size] data
*/


static struct mlsp *mlsp_close_and_return_null(struct mlsp *m);
static int mlsp_send_udp(struct mlsp *m, int data_size);

struct mlsp
{
	int socket_udp;
	struct sockaddr_in destination_udp;
	uint8_t data[FRAME_HEADER_SIZE + FRAME_MAX_PAYLOAD];
};

struct mlsp *mlsp_init(const struct mlsp_config *config)
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
        
	m->destination_udp.sin_family = AF_INET;
	if (! inet_pton(AF_INET, config->ip, &m->destination_udp.sin_addr) )
	{
		fprintf(stderr, "mlsp: failed to initialize UDP destination\n");
		return mlsp_close_and_return_null(m);
	}
		
	m->destination_udp.sin_port = htons(config->port);		

	return m;
}

void mlsp_close(struct mlsp *m)
{
	if(m == NULL)
		return;
		
	if(close(m->socket_udp) == -1)
		fprintf(stderr, "mlsp: error while closing socket\n");
		
	free(m);
}

static struct mlsp *mlsp_close_and_return_null(struct mlsp *m)
{
	mlsp_close(m);
	return NULL;
}

/* packet structure
    u16 framenumber 
    u16 packets
    u16 packet 
    u16 size  
    u8[size] data
*/

int mlsp_send(struct mlsp *m, uint16_t framenumber, uint8_t* data, int data_size)
{
	const uint16_t packets = data_size / FRAME_MAX_PAYLOAD;
	
    for(uint16_t p=0;p<=packets;++p)
    {
        //encode header
        memcpy(m->data, &framenumber, sizeof(framenumber));
        memcpy(m->data+2, &packets, sizeof(packets));
        memcpy(m->data+4, &p, sizeof(p));
        uint16_t size = (p != packets) ? FRAME_MAX_PAYLOAD : data_size % FRAME_MAX_PAYLOAD;
        memcpy(m->data+6, &size, sizeof(size));
        //encode payload
        memcpy(m->data+8, data + p * FRAME_MAX_PAYLOAD, size);
        
		if( mlsp_send_udp(m, size + FRAME_HEADER_SIZE) != MLSP_OK )
			return MLSP_ERROR;
		//temp
		printf("mlsp: sent %d bytes\n", size + FRAME_HEADER_SIZE);
    }

	return MLSP_OK;

}

static int mlsp_send_udp(struct mlsp *m, int data_size)
{
	int result;
	int written=0;
	
	while(written<data_size)
	{
		if ((result = sendto(m->socket_udp, m->data+written, data_size-written, 0, (struct sockaddr*)&m->destination_udp, sizeof(m->destination_udp))) == -1)
		{
			fprintf(stderr, "mlsp: failed to send udp data\n");
			return MLSP_ERROR;
		}
		written += result;		
	}
	return MLSP_OK;
}