#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <event.h>

#define MAX_CACHED 8000
#define MAX_CONNECTIONS 2

unsigned int daddr;
unsigned short dport;
char *host;

struct maps{
	evutil_socket_t *self_sock;
	evutil_socket_t *other_sock;
	struct bufferevent *other_bev;
};
void read_cb(struct bufferevent *bev, void *ctx);

//Sets a socket descriptor to nonblocking mode
void setnonblock(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	flags |= O_NONBLOCK;
	fcntl(fd, F_SETFL, flags);
}

//Translates hostname to IP address
int hostname_to_ip(char *hostname , char *ip)
{
	struct hostent *he;
	struct in_addr **addr_list;
	int i;

	if((he = gethostbyname(hostname)) == NULL)
	{
		// get the host info
		herror("gethostbyname");
		return 1;
	}

	addr_list = (struct in_addr **) he->h_addr_list;

	for(i = 0; addr_list[i] != NULL; i++)
	{
		//Return the first one;
		strcpy(ip , inet_ntoa(*addr_list[i]) );
		return 0;
	}

	return 1;
}


void error_cb(struct bufferevent *bev, short error, void *map_b)
{
    struct maps *map = ((struct maps *)map_b);
    if ((error & BEV_EVENT_EOF) || (error & BEV_EVENT_READING)) {
        /* EOF */
		if(*map->other_sock > 0){
			//Says eof has been reached
			//checks if mapped sock is at eof
			shutdown(*map->other_sock, SHUT_RD);
			*map->self_sock = -1;
			free(map);
		} else {
			close(*map->self_sock);
			close(bufferevent_getfd(map->other_bev));
			bufferevent_free(bev);
			bufferevent_free(map->other_bev);
			free(map);
		}
    } else if ((error & BEV_EVENT_WRITING) || (error & BEV_EVENT_ERROR)){
		close(*map->self_sock);
		close(bufferevent_getfd(map->other_bev));
		bufferevent_free(bev);
		bufferevent_free(map->other_bev);
		free(map);
    }
}

void write_cb(struct bufferevent *bev, void *map_b)
{
	struct bufferevent *partner = ((struct maps *)map_b)->other_bev;

	bufferevent_setcb(bev, read_cb, NULL, error_cb, partner);
	bufferevent_setwatermark(bev, EV_WRITE, 0, MAX_CACHED);
	if (partner)
		bufferevent_enable(partner, EV_READ);
}

void read_cb(struct bufferevent *bev, void *map_b)
{
	struct bufferevent *partner = ((struct maps *)map_b)->other_bev;
	struct evbuffer *src;
	size_t len;

	src = bufferevent_get_input(bev);
	if (!partner) {
		len = evbuffer_get_length(src);
		evbuffer_drain(src, len);
		return;
	}
	bufferevent_write_buffer(partner, src);

	if (evbuffer_get_length(src) >= MAX_CACHED) {
		bufferevent_setcb(partner, read_cb, write_cb, error_cb, bev);
		bufferevent_setwatermark(partner, EV_WRITE, MAX_CACHED/2, MAX_CACHED);
		bufferevent_disable(bev, EV_READ);
	}
}

void accept_cb(evutil_socket_t fd, short what, void *base_b)
{
	struct event_base *base = event_base_new();
	struct evdns_base *dns_base;
	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);
	
	//sem_wait(&connections);
	evutil_socket_t source_sock = accept(fd, (struct sockaddr *)&ss, &slen);
	if (source_sock < 0) {
		perror("accept");
	} else if (source_sock > FD_SETSIZE) {
		close(fd);
	} else {
		setnonblock(source_sock);
		
		struct bufferevent *bev_in, *bev_out;
		bev_in = bufferevent_socket_new(base_b, source_sock, BEV_OPT_CLOSE_ON_FREE);
		bev_out = bufferevent_socket_new(base_b, -1, BEV_OPT_CLOSE_ON_FREE);
		
		dns_base = evdns_base_new(base_b, 1);
		bufferevent_socket_connect_hostname(bev_out, dns_base, AF_INET, host, dport);
		evutil_socket_t server_sock = bufferevent_getfd(bev_out);
		setnonblock(server_sock);

		struct maps *map_in, *map_out;
		map_in = (struct maps *)malloc(sizeof(struct maps));
		map_out = (struct maps *)malloc(sizeof(struct maps));
	
		map_in->self_sock = &source_sock;
		map_in->other_sock = &server_sock;
		map_in->other_bev = bev_out;
		
		map_out->self_sock = &server_sock;
		map_out->other_sock = &source_sock;
		map_out->other_bev = bev_in;

		bufferevent_setcb(bev_in, read_cb, NULL, error_cb, map_in);
		bufferevent_setwatermark(bev_in, EV_READ|EV_WRITE, 0, MAX_CACHED);
		bufferevent_setcb(bev_out, read_cb, NULL, error_cb, map_out);
        bufferevent_setwatermark(bev_out, EV_READ|EV_WRITE, 0, MAX_CACHED);
                
		struct timeval *timeout = (struct timeval *)malloc(sizeof(struct timeval));
		timeout->tv_usec = 0;
		timeout->tv_sec = 10;
		bufferevent_set_timeouts(bev_in, NULL, timeout);
		bufferevent_set_timeouts(bev_out, NULL, timeout);
		
		bufferevent_priority_set(bev_in, 0);
		bufferevent_priority_set(bev_out, 0);
		bufferevent_enable(bev_out, EV_READ|EV_WRITE);
		bufferevent_enable(bev_in, EV_READ|EV_WRITE);

		event_base_dispatch(base);
		free(timeout);
   	}
}


int main(int argc, char **argv)
{
	int socketlisten;
	struct sockaddr_in addresslisten;
	int reuse = 1;

	if(argc != 4)
	{
		printf("Usage: %s destination-host destination-port listening-port\n", argv[0]);
		return 0;
	}

	//sem_init(&connections, 0, MAX_CONNECTIONS);

	daddr = inet_addr(argv[1]);
	host = argv[1];

	if(daddr == INADDR_NONE)
	{
		char temp[100];
		hostname_to_ip(argv[1], temp);
		daddr = inet_addr(temp);
	}

	dport = atoi(argv[2]);

	event_init();
	struct event_base *base = event_base_new();

	//Create listening socket
	socketlisten = socket(AF_INET, SOCK_STREAM, 0);
	if (socketlisten < 0)
	{
		fprintf(stderr,"Failed to create listen socket");
		return 1;
	}

	//Init listening structure
	memset(&addresslisten, 0, sizeof(addresslisten));

	addresslisten.sin_family = AF_INET;
	addresslisten.sin_addr.s_addr = INADDR_ANY;
	addresslisten.sin_port = htons(atoi(argv[3]));

	//Binding
	if (bind(socketlisten,
			(struct sockaddr *)&addresslisten,
			sizeof(addresslisten)) < 0)
	{
		fprintf(stderr,"Failed to bind\n");
		return 1;
	}

	//Listen
	if (listen(socketlisten, 5) < 0)
	{
		fprintf(stderr,"Failed to listen to socket\n");
		return 1;
	}

	//Set some socket properties
	setsockopt(socketlisten, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	setnonblock(socketlisten);

	//Add accept callback
	struct event *accept_event = event_new(base, socketlisten, EV_READ | EV_PERSIST, accept_cb, base);
	event_add(accept_event, NULL);
	event_base_dispatch(base);

	//If dispatch returns for some reason....
	event_free(accept_event);
	close(socketlisten);
	return 0;
}
