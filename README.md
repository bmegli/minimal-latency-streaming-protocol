# MLSP Minimal Latency Streaming Protocol C library

This library implements custom UDP protocol for streaming frame-like data spanning multiple MTUs.
The assumption is that user is only interested in the latest data. 

## Purpose

- minimize latency
- no buffering
- private experiments

The only buffering that is allowed is natural arising from sequece:
- we are collecting frame N
- some packet from frame N+1 arrives before frame N is completed
- we may still try to collect frame N until full frame N+1 arrives

## Platforms

Unix-like systems (due to network implementation, can be easily made cross-platform).


## State

Working proof-of-concept. 

Currently whenever packet from frame N+1 arrives, data from frame N is discarded.

## Using

Error checking ommited for clarity.

Client:

```C
struct mlsp *streamer;
struct mlsp_config streamer_config= {"127.0.0.1", 9766}; //host, port

streamer=mlsp_init_client(&streamer_config);

while(keep_working)
{
	//...
	//prepare your date in some way
	//...

	//here passing only pointer to framedata, no copies are needed
	mlsp_frame frame={ frame_number, framedata, framedata_size};
	mlsp_send(streamer, &frame)
}

mlsp_close(streamer);
```

Server:

```C
int error;
struct mlsp *streamer;
struct mlsp_config streamer_config= {NULL, 9766, 500}; //listen on any, port, 500 ms timeout

streamer=mlsp_init_server(&streamer_config);

//here we will be getting data
mlsp_frame *streamer_frame;

while(keep_working)
{
	streamer_frame = mlsp_receive(streamer, &error);

	if(streamer_frame == NULL)
	{
		if(error==MLSP_TIMEOUT)
		{
			//accept also new streaming sequence
			mlsp_receive_reset(streamer);
			continue;
		}
		break; //error
	}
	//...
	//do something with the streamer_frame
	//the ownership remains with library so consume or copy
	//...
	
}

mlsp_close(streamer);
```


