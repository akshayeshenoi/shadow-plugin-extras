/*
 * See LICENSE for licensing information
 */

#include "pcap_replay.h"

#define MAGIC 0xFFEEDDCC

const gchar* USAGE = "USAGE: <node-type> <server-host> <server-port> <pcap_client_ip> <pcap_nw_addr> <pcap_nw_mask> <timeout> <pcap_trace1> <pcap_trace2>..\n";

/* pcap_activateClient() is called when the epoll descriptor has an event for the client */
void _pcap_activateClient(Pcap_Replay* pcapReplay, gint sd, uint32_t events) {
	pcapReplay->slogf(G_LOG_LEVEL_DEBUG, __FUNCTION__, "Activate client!");
 
	char receivedPacket[MTU];
	struct epoll_event ev;
	ssize_t numBytes;

	/* Save a pointer to the packet to send */
	Custom_Packet_t *pckt_to_send = pcapReplay->nextPacket;
	
	/* LOG event */
	if(events & EPOLLOUT) {
		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "Client EPOLLOUT is set");
	}
	if(events & EPOLLIN) {
		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "Client EPOLLIN is set");
	}
	if((events & EPOLLIN) && (events & EPOLLOUT)) {
		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "Client EPOLLIN & EPOLLOUT are set");
	}

	/* Process events */ 
	if (sd == pcapReplay->client.tfd_sendtimer) { // time to send the next packet
		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "Sending packet!");
		Custom_Packet_t* pckt_to_send = pcapReplay->nextPacket;

		numBytes = send_packet(pckt_to_send, pcapReplay->client.server_sd_tcp);

		/* log result */
		if(numBytes > 0) {
			pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
					"Successfully sent a '%d' (bytes) packet to the server", numBytes);
		} else if(numBytes == 0) {
			/* What is this TODO */
			pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
						"The last packet to send was an ACK. Skipped sending.", numBytes);
		} else {
			pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
						"Unable to send message");
			exit(1);
		}

		//  now prepare next packet
		if(!get_next_packet(pcapReplay, TRUE)) {
			/* No packet found! */
			pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "No packet found! Just going to wait..");
			// TODO fix this?
		}

		struct timespec timeToWait;
		timeval_subtract (&timeToWait, &pckt_to_send->timestamp, &pcapReplay->nextPacket->timestamp);

		// sleep for timeToWait time 
		struct itimerspec itimerspecWait;
		itimerspecWait.it_interval.tv_nsec = 0;
		itimerspecWait.it_interval.tv_sec = 0;
		itimerspecWait.it_value = timeToWait;
		if (timerfd_settime(pcapReplay->client.tfd_sendtimer, 0, &itimerspecWait, NULL) < 0) {
			pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "Can't set timerFD");
			exit(1);
		}

		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "Sleeping for %d %d!", timeToWait.tv_sec, timeToWait.tv_nsec);

		free(pckt_to_send);

	} else if(sd == pcapReplay->client.server_sd_tcp && (events & EPOLLIN)) { // receive a message from the server
		memset(receivedPacket, 0, (size_t)MTU);
		numBytes = recv(sd, receivedPacket, (size_t)MTU, 0);

		/* log result */
		if(numBytes > 0) {
			pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
						"Successfully received a packet from server: %d bytes", numBytes);
		} else if(numBytes==0) {
			/* The connection have been closed by the distant peer.
			 * The client need to close the connection and restart (or quit because of timeout */
			pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
						"Server closed connection? Restarting..");
			if(restart_client(pcapReplay)) {
				pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, 
							"Successfully restarted the client !");
				return;
			} else{
				deinstanciate(pcapReplay,sd);
				return;
			}
		} else{
			pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
						"Unable to receive message");
		}
	}

	/*  If the timeout is reached, close the plugin ! */
	GDateTime* dt = g_date_time_new_now_local();
	if(g_date_time_to_unix(dt) >= pcapReplay->timeout) {
		/* tell epoll we no longer want to watch this socket */
		pcapReplay->slogf(G_LOG_LEVEL_INFO, __FUNCTION__,  "Timeout reached!");
		deinstanciate(pcapReplay,sd);
	}
}

/* pcap_activateServer() is called when the epoll descriptor has an event for the server */
void _pcap_activateServer(Pcap_Replay* pcapReplay, gint sd, uint32_t events) {
	pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "Activate server !");

	char receivedPacket[MTU];
	struct epoll_event ev;
	ssize_t numBytes;

	/* LOG event */
	if(events & EPOLLOUT) {
		pcapReplay->slogf(G_LOG_LEVEL_DEBUG, __FUNCTION__, "Server EPOLLOUT is set");
	}
	if(events & EPOLLIN) {
		pcapReplay->slogf(G_LOG_LEVEL_DEBUG, __FUNCTION__, "Server EPOLLIN is set");
	}
	if((events & EPOLLIN) && (events & EPOLLOUT)) {
		pcapReplay->slogf(G_LOG_LEVEL_DEBUG, __FUNCTION__, "Server EPOLLIN & EPOLLOUT are set");
	}

	/* Process events */
	if(sd == pcapReplay->server.sd_tcp) {
		/* data on a listening socket means a new client connection */
		assert(events & EPOLLIN);

		/* accept new connection from a remote client */
		struct sockaddr_in clientaddr;
    	socklen_t clientaddr_size = sizeof(clientaddr);
		int newClientSD = accept(sd,  (struct sockaddr *)&clientaddr, &clientaddr_size);

			
		int len=20;
		char ip_add[len];

		inet_ntop(AF_INET, &(clientaddr.sin_addr), ip_add, len);


		pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__,
						"Client connected on server with address : %s ", ip_add	);


		/* now register this new socket so we know when its ready for recv */
		memset(&ev, 0, sizeof(struct epoll_event));
		ev.events = EPOLLIN;
		ev.data.fd = newClientSD;
		epoll_ctl(pcapReplay->ed, EPOLL_CTL_ADD, newClientSD, &ev);

		// save reference to client
		pcapReplay->server.client_sd_tcp = newClientSD;

		// get the time delay between the first packet sent by client and the server
		// the server must transmit only after this time.
		// so first, get the timestamp of the first client packet
		// note that we set the isClient flag as true here because we are interested
		// in the first packet sent by the client, despite being the server!
		if(!get_next_packet(pcapReplay, TRUE)) {
			/* No packet found! */
			pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "No packet found! Error");
			// TODO quit here
		}

		struct timeval first_clientpkt_time = pcapReplay->nextPacket->timestamp;

		//  now get the first server packet
		if(!get_next_packet(pcapReplay, FALSE)) {
			/* No packet found! */
			pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "No packet found! Error");
			// TODO quit here
		}

		struct timeval first_serverpkt_time = pcapReplay->nextPacket->timestamp;
		struct timespec timeToWait;

		timeval_subtract (&timeToWait, &first_clientpkt_time,&first_serverpkt_time);

		// create timerfd and sleep for timetowait
		pcapReplay->server.tfd_sendtimer = timerfd_create(CLOCK_MONOTONIC, 0);

		struct itimerspec itimerspecWait;
		itimerspecWait.it_interval.tv_nsec = 0;
		itimerspecWait.it_interval.tv_sec = 0;
		itimerspecWait.it_value = timeToWait;
		if (timerfd_settime(pcapReplay->server.tfd_sendtimer, 0, &itimerspecWait, NULL) < 0) {
			pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "Can't set timerFD");
		}
		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "Sleeping for %d %d!", timeToWait.tv_sec, timeToWait.tv_nsec);

		// finally monitor by epoll
		memset(&ev, 0, sizeof(struct epoll_event));
		ev.events = EPOLLIN;
		ev.data.fd = pcapReplay->server.tfd_sendtimer;
		epoll_ctl(pcapReplay->ed, EPOLL_CTL_ADD, pcapReplay->server.tfd_sendtimer, &ev);
		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "Epoll timer created!");
	}
	else if (sd == pcapReplay->server.tfd_sendtimer) { // time to send the next packet
		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "Sending packet!");
		Custom_Packet_t* pckt_to_send = pcapReplay->nextPacket;

		numBytes = send_packet(pckt_to_send, pcapReplay->server.client_sd_tcp);

		/* log result */
		if(numBytes > 0) {
			pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
					"Successfully sent a '%d' (bytes) packet to the client", numBytes);
		} else if(numBytes == 0) {
		/* What is this TODO */
		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
					"The last packet to send was an ACK. Skipped sending.", numBytes);
		} else {
			pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
						"Unable to send message");
			exit(1);
		}

		//  now prepare next packet
		if(!get_next_packet(pcapReplay, FALSE)) {
			/* No packet found! */
			pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "No packet found! Just going to wait..");
			// TODO fix this
		}

		struct timespec timeToWait;
		timeval_subtract (&timeToWait, &pckt_to_send->timestamp, &pcapReplay->nextPacket->timestamp);

		// sleep for timeToWait time 
		struct itimerspec itimerspecWait;
		itimerspecWait.it_interval.tv_nsec = 0;
		itimerspecWait.it_interval.tv_sec = 0;
		itimerspecWait.it_value = timeToWait;
		if (timerfd_settime(pcapReplay->server.tfd_sendtimer, 0, &itimerspecWait, NULL) < 0) {
			pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "Can't set timerFD");
		}

		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "Sleeping for %d %d!", timeToWait.tv_sec, timeToWait.tv_nsec);

		free(pckt_to_send);
	}

	// TODO
	// use timerfd to use epoll for sleep https://gist.github.com/ianpartridge/cb65a2bd79ba0746b9d68aa2afaed7f1
	// process send when sd = timerfd and event is epollin
	// also process UDP stuff

	// else if (sd == pcapReplay->server.sd_udp) {
	// 	/* activity on udp socket */
	// 	/*  Prepare to receive message */
	// 		memset(receivedPacket, 0, (size_t)MTU);
	// 		numBytes = recv(sd, receivedPacket, (size_t)MTU, 0);
	// 		pcapReplay->isFirstPacketReceived=TRUE;
	// 		/* log result */
	// 		if(numBytes > 0) {
	// 			pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
	// 						"Successfully received a packet from the client", numBytes);
	// 		}
	// }
	else if(events & EPOLLIN) { // receive a message from the client	
		memset(receivedPacket, 0, (size_t)MTU);
		numBytes = recv(sd, receivedPacket, (size_t)MTU, 0);

		/* log result */
		if(numBytes > 0) {
			pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
					"Successfully received a message for the client: %d bytes", numBytes);
		} else if(numBytes == 0) {
			/* Client closed the remote connection
				* Restart the server & wait for a new connection */
			if(restart_server(pcapReplay)) {
				pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "Successfully restarted the server !");
				return;
			} else{
				deinstanciate(pcapReplay,sd);
				return;
			}
		} else {
			pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
					"Unable to receive message");
		}
	}

	/* If timeout expired, close connection and exit plugin */
	GDateTime* dt = g_date_time_new_now_local();
	if(g_date_time_to_unix(dt) >= pcapReplay->timeout) {
		/* tell epoll we want to write the response now */
		pcapReplay->slogf(G_LOG_LEVEL_INFO, __FUNCTION__,  "Timeout reached!");
		deinstanciate(pcapReplay,sd);
	}
}

gboolean pcap_StartClient(Pcap_Replay* pcapReplay) {
	g_assert(pcapReplay && (pcapReplay->magic == MAGIC));

	/* use epoll to asynchronously watch events for all of our sockets */
	if(pcapReplay->isRestarting==FALSE) {
		pcapReplay->ed = epoll_create(1);
		if(pcapReplay->ed == -1) {
			pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "Error in main epoll_create");
			close(pcapReplay->ed);
			return FALSE;
		}
	}

	/* create the client socket and get a socket descriptor */
	pcapReplay->client.server_sd_tcp = socket(AF_INET, SOCK_STREAM, 0);
	if(pcapReplay->client.server_sd_tcp == -1) {
		pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__,
					"Unable to start control socket: error in socket");
		return FALSE;
	}

	/* Set TCP_NODELAY option to avoid Nagle algo */
	int optval = 1;
	if(setsockopt(pcapReplay->client.server_sd_tcp, IPPROTO_TCP, TCP_NODELAY, (char *) &optval, sizeof(optval)) != 0){
		pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__,
					"Unable to set options to the socket !");
		return FALSE;
	}

	/* get the server ip address */
	if(g_ascii_strncasecmp(pcapReplay->serverHostName->str, "localhost", 9) == 0) {
		pcapReplay->serverIP = htonl(INADDR_LOOPBACK);
	} else {
		struct addrinfo* info;
		int ret = getaddrinfo(pcapReplay->serverHostName->str, NULL, NULL, &info);
		if(ret < 0) {
			pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__,
					"Unable to getaddrinfo() on hostname \"%s\"", pcapReplay->serverHostName->str);
			return FALSE;
		}

		pcapReplay->serverIP = ((struct sockaddr_in*)(info->ai_addr))->sin_addr.s_addr;
		freeaddrinfo(info);
	}

	/* our client socket address information for connecting to the server */
	struct sockaddr_in serverAddress;
	memset(&serverAddress, 0, sizeof(serverAddress));
	serverAddress.sin_family = AF_INET;
	serverAddress.sin_addr.s_addr = pcapReplay->serverIP;
	serverAddress.sin_port =pcapReplay->serverPortTCP;

	/* connect to server. since we are blocking, we expect this to return only after connect */
	gint res = connect(pcapReplay->client.server_sd_tcp, (struct sockaddr *) &serverAddress, sizeof(serverAddress));
	if (res == -1) {
		pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__, "Unable to start control socket: error in connect");
		return FALSE;
	}

	// make socket non blocking
	res = fcntl(pcapReplay->client.server_sd_tcp, F_SETFL, fcntl(pcapReplay->client.server_sd_tcp, F_GETFL, 0) | O_NONBLOCK);
	if (res == -1){
		pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__, "Error converting to nonblock");
	}

	pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "Connected to server!");

	/* specify the events to watch for on this socket.
	 * to start out, the client wants to know when it can send a message. */
	_pcap_epoll(pcapReplay, EPOLL_CTL_ADD, EPOLLIN, pcapReplay->client.server_sd_tcp);

	return TRUE;
}

gboolean pcap_StartClientTor(Pcap_Replay* pcapReplay) {
	g_assert(pcapReplay && (pcapReplay->magic == MAGIC));

	/* use epoll to asynchronously watch events for all of our sockets */
	if(pcapReplay->isRestarting==FALSE) {
		pcapReplay->ed = epoll_create(1);
		if(pcapReplay->ed == -1) {
			pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "Error in main epoll_create");
			close(pcapReplay->ed);
			return FALSE;
		}
	}
	
	/* create the client socket and get a socket descriptor */
	pcapReplay->client.server_sd_tcp = socket(AF_INET, SOCK_STREAM, 0);
	if(pcapReplay->client.server_sd_tcp == -1) {
		pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__,
				"unable to start control socket: error in socket");
		return FALSE;
	}

	/* Set TCP_NODELAY option to avoid Nagle algo */
	int optval = 1;
	if(setsockopt(pcapReplay->client.server_sd_tcp, IPPROTO_TCP, TCP_NODELAY, (char *) &optval, sizeof(optval)) != 0){
		pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__,
					"Unable to set options to the socket !");
		return FALSE;
	}

	/* our client socket address information for connecting to the Tor proxy */
	struct sockaddr_in proxyAddress;
	memset(&proxyAddress, 0, sizeof(proxyAddress));
	proxyAddress.sin_family = AF_INET;
	proxyAddress.sin_addr.s_addr = pcapReplay->proxyIP;
	proxyAddress.sin_port = pcapReplay->proxyPort;

	pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
				"Trying to connect to Tor socket (port 9000)");

	/* connect to server. since we are blocking, we expect this to return only after connect */
	gint res = connect(pcapReplay->client.server_sd_tcp, (struct sockaddr *) &proxyAddress, sizeof(proxyAddress));
	if (res == -1) {
		pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__, "unable to start control socket: error in connect");
		return FALSE;
	}
	pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "Connected to Tor socket port 9000 !");

	// make socket non blocking
	res = fcntl(pcapReplay->client.server_sd_tcp, F_SETFL, fcntl(pcapReplay->client.server_sd_tcp, F_GETFL, 0) | O_NONBLOCK);
	if (res == -1){
		pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__, "Error converting to nonblock");
	}

	/* Initiate the connection to the Tor proxy.
	 * The client needs to do the Socks5 handshake to communicate
	 * through Tor using the proxy */
	initiate_conn_to_proxy(pcapReplay);

	pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "Connected to server!");

	/* specify the events to watch for on this socket.
	 * to start out, the client wants to know when it can send a message. */
	_pcap_epoll(pcapReplay, EPOLL_CTL_ADD, EPOLLIN, pcapReplay->client.server_sd_tcp);

	return TRUE;
}

gboolean pcap_StartServer(Pcap_Replay* pcapReplay) {
	g_assert(pcapReplay && (pcapReplay->magic == MAGIC));

	/* use epoll to asynchronously watch events for all of our sockets */
	if(pcapReplay->isRestarting==FALSE) {
		/* Don't close&restart epoll descriptor when isRestarting!*/
		pcapReplay->ed = epoll_create(1);
		if(pcapReplay->ed == -1) {
			pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "Error in main epoll_create");
			close(pcapReplay->ed);
			return FALSE;
		}
	}

	/******************** TCP SERVER ********************/

	/* Create the server socket and get a socket descriptor */
	pcapReplay->server.sd_tcp = socket(AF_INET, (SOCK_STREAM | SOCK_NONBLOCK), 0);
	if(pcapReplay->server.sd_tcp == -1) {
		pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__,
					"Unable to start control socket: error in socket");
		return FALSE;
	}

	/* Set TCP_NODELAY option to avoid Nagle algo */
	int optval = 1;
	if(setsockopt(pcapReplay->server.sd_tcp, IPPROTO_TCP, TCP_NODELAY, (char *) &optval, sizeof(optval)) != 0){
		pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__,
					"Unable to set options to the socket !");
		return FALSE;
	}


	/* Setup the socket address info, client has outgoing connection to server */
	struct sockaddr_in bindAddress;
	memset(&bindAddress, 0, sizeof(bindAddress));
	bindAddress.sin_family = AF_INET;
	bindAddress.sin_addr.s_addr = INADDR_ANY;
	bindAddress.sin_port = pcapReplay->serverPortTCP;

	/* Bind the socket to the server port */
	gint res = bind(pcapReplay->server.sd_tcp, (struct sockaddr *) &bindAddress, sizeof(bindAddress));
	if (res == -1) {
		pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__,
				"unable to start server: error in bind");
		return FALSE;
	}

	/* set as server socket that will listen for clients */
	res = listen(pcapReplay->server.sd_tcp, 100);
	if (res == -1) {
		pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__,
				"unable to start server: error in listen tcp");
		return FALSE;
	}

	/* Specify the events to watch for on this socket.
	 * To start out, the server wants to know when a client is connecting. */
	_pcap_epoll(pcapReplay, EPOLL_CTL_ADD, EPOLLIN, pcapReplay->server.sd_tcp);

	/******************** UDP SERVER ********************/

	/* Create the server socket and get a socket descriptor */
	pcapReplay->server.sd_udp = socket(AF_INET, (SOCK_DGRAM | SOCK_NONBLOCK), 0);
	if(pcapReplay->server.sd_udp == -1) {
		pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__,
					"Unable to start control socket: error in socket");
		return FALSE;
	}

	/* Setup the socket address info, client has outgoing connection to server */
	// listen to udp on the tcp port + 1
	memset(&bindAddress, 0, sizeof(bindAddress));
	bindAddress.sin_family = AF_INET;
	bindAddress.sin_addr.s_addr = INADDR_ANY;
	bindAddress.sin_port = pcapReplay->serverPortUDP;

	/* Bind the socket to the server port */
	res = bind(pcapReplay->server.sd_udp, (struct sockaddr *) &bindAddress, sizeof(bindAddress));
	if (res == -1) {
		pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__,
				"unable to start server: error in bind");
		return FALSE;
	}

	/* Specify the events to watch for on this socket.
	 * To start out, the server wants to know when a client is connecting. */
	_pcap_epoll(pcapReplay, EPOLL_CTL_ADD, EPOLLIN, pcapReplay->server.sd_udp);

	return TRUE;
}

/* The pcap_replay_new() function creates a new instance of the pcap replayer plugin 
 * The instance can either be a server waiting for a client or a client connecting to the pcap server. */
Pcap_Replay* pcap_replay_new(gint argc, gchar* argv[], PcapReplayLogFunc slogf) {
	/* Expected args:
		./pcap_replay-exe <node-type> <server-host> <server-port> <pcap_client_ip> <pcap_nw_addr> <pcap_nw_mask> <timeout> <pcap_trace1> <pcap_trace2>.. 
		node-type: client | client-tor | client-vpn | server
	*/
	g_assert(slogf);
	gboolean is_instanciation_done = FALSE; 
	gint arg_idx = 1;

	Pcap_Replay* pcapReplay = g_new0(Pcap_Replay, 1);

	pcapReplay->magic = MAGIC;
	pcapReplay->slogf = slogf;
	pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__,
					"Creating a new instance of the pcap replayer plugin:");
	

	const GString* nodeType = g_string_new(argv[arg_idx++]); // client or server ?
	const GString* client_str = g_string_new("client");
	const GString* clientVpn_str = g_string_new("client-vpn");
	const GString* clientTor_str = g_string_new("client-tor");
	const GString* server_str = g_string_new("server");

	if(g_string_equal(nodeType,clientTor_str)) {
		/* If tor client, get SocksPort */
		pcapReplay->proxyPort = htons(atoi(argv[arg_idx++]));
		/* use loopback addr to connect to the Socks Tor proxy */
		pcapReplay->proxyIP = htonl(INADDR_LOOPBACK); 
	}

	/* Get the remote server name & port */
	pcapReplay->serverHostName = g_string_new(argv[arg_idx++]);
	pcapReplay->serverPortTCP = (in_port_t) htons(atoi(argv[arg_idx]));
	pcapReplay->serverPortUDP = (in_port_t) htons(atoi(argv[arg_idx++]) + 1);

	pcapReplay->isAllowedToSend = TRUE;
	pcapReplay->isFirstPacketReceived=TRUE;
	pcapReplay->isRestarting = FALSE;

	// Get client IP addr used in the pcap file
	if(inet_aton(argv[arg_idx++], &pcapReplay->client_IP_in_pcap) == 0) {
		pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__,
					"Cannot get the client IP used in pcap file : Err in the arguments ");
		pcap_replay_free(pcapReplay);
		return NULL;
	}

	// Get local network address
	if(inet_aton(argv[arg_idx++], &pcapReplay->pcap_local_nw_addr) == 0) {
		pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__,
					"Cannot get the client IP used in pcap file : Err in the arguments ");
		pcap_replay_free(pcapReplay);
		return NULL;
	}

	// Get local network address mask (2^(32 - mask))
	pcapReplay->pcap_local_nw_mask = (guint32) 1 << (32 - atoi(argv[arg_idx++]));

	// return NULL;
	// Get the timeout of the experiment
	GDateTime* dt = g_date_time_new_now_local();
	pcapReplay->timeout = atoi(argv[arg_idx++]) + g_date_time_to_unix(dt);

	// Get pcap paths and then open the file using pcap_open()
	pcapReplay->nmb_pcap_file = argc-arg_idx;
	// We open all the pcap file here in order to know directly if there is an error ;)
	// The paths of the pcap file are stored in a queue as well as the pcap_t pointers.
	// Path & pcap_t pointers are stored in the same order !
	pcapReplay->pcapFilePathQueue = g_queue_new();
	pcapReplay->pcapStructQueue = g_queue_new();

	for(gint i=arg_idx; i < arg_idx+pcapReplay->nmb_pcap_file ;i++) {
		// Open the pcap file 
		pcap_t *pcap = NULL;
		char ebuf[PCAP_ERRBUF_SIZE];
		if ((pcap = pcap_open_offline(argv[i], ebuf)) == NULL) {
			pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__,
					"Unable to open the pcap file :");
			return NULL;
		} else {
			pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
					"Pcap file opened (%s) ",argv[i]);
		}
		//Add the file paths & pcap_t pointer to the queues
		g_queue_push_tail(pcapReplay->pcapFilePathQueue, g_string_new(argv[i]));
		g_queue_push_tail(pcapReplay->pcapStructQueue, pcap);
	}

	// Attach the first pcap_t struct to the instance state
	// The pcap files are used in the order the appear in arguments
	pcapReplay->pcap = (pcap_t*) g_queue_peek_head(pcapReplay->pcapStructQueue);

	/* If the first argument is equal to "client" 
	 * Then create a new client instance of the  pcap replayer plugin */
	if(g_string_equal(nodeType,client_str)) {
		pcapReplay->isClient = TRUE;
		pcapReplay->isVpn = FALSE;
		pcapReplay->isTorClient = FALSE;
		// Start the client (socket,connect)
		if(!pcap_StartClient(pcapReplay)) {
			pcap_replay_free(pcapReplay);
			return NULL;
		} 
	} 
	/* If the first argument is equal to "client-vpn" 
	 * Then create a new client instance of the  pcap replayer plugin, and set the isVpn to true */
	else if(g_string_equal(nodeType,clientVpn_str)) {
		pcapReplay->isClient = TRUE;
		pcapReplay->isVpn = TRUE;
		pcapReplay->isTorClient = FALSE;
		// Start the client (socket,connect)
		if(!pcap_StartClient(pcapReplay)) {
			pcap_replay_free(pcapReplay);
			return NULL;
		} 
	}
	/* If the first argument is equal to "client-tor" 
	 * Then create a new tor client instance of the pcap replayer plugin */
	else if(g_string_equal(nodeType,clientTor_str)) {
		pcapReplay->isClient = TRUE;
		pcapReplay->isVpn = FALSE;
		pcapReplay->isTorClient = TRUE;
		// Start the client (socket,connect)
		if(!pcap_StartClientTor(pcapReplay)) {
			pcap_replay_free(pcapReplay);
			return NULL;
		}
	}
	/* If the first argument is equal to "server" 
	 * Then create a new server instance of the pcap replayer plugin */
	else if(g_string_equal(nodeType,server_str)) {
		pcapReplay->isClient = FALSE;
		// Start the server (socket, bind, listen)
		if(!pcap_StartServer(pcapReplay)) {
			pcap_replay_free(pcapReplay);
			return NULL;
		} 	
	} else{
		pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__,
					"First argument is not equals to either 'client'| 'client-vpn' | 'client-tor|'server'. Exiting !");
		pcap_replay_free(pcapReplay);
		return NULL;
	}
	is_instanciation_done = TRUE;

	// if client, prepare first packet to send and start timer
	if(pcapReplay->isClient) {
		if (!get_next_packet(pcapReplay, TRUE)) {
			// If there is no packet matching the IP.source & IP.dest & port.dest, then exits !
			is_instanciation_done=FALSE;
			pcap_replay_free(pcapReplay);
			pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
					"Cannot find one packet (in the pcap file) matching the IPs/Ports arguments ");
			return NULL;
		}

		// create timerfd and start sending right away
		pcapReplay->client.tfd_sendtimer = timerfd_create(CLOCK_MONOTONIC, 0);

		struct itimerspec itimerspecWait;
		itimerspecWait.it_interval.tv_nsec = 0;
		itimerspecWait.it_interval.tv_sec = 0;
		itimerspecWait.it_value.tv_nsec = 1;
		itimerspecWait.it_value.tv_sec = 0;
		if (timerfd_settime(pcapReplay->client.tfd_sendtimer, 0, &itimerspecWait, NULL) < 0) {
			pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "Can't set timerFD");
			exit(1);
		}

		// finally monitor by epoll
		struct epoll_event ev;
		memset(&ev, 0, sizeof(struct epoll_event));
		ev.events = EPOLLIN;
		ev.data.fd = pcapReplay->client.tfd_sendtimer;
		epoll_ctl(pcapReplay->ed, EPOLL_CTL_ADD, pcapReplay->client.tfd_sendtimer, &ev);
		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "Epoll timer created!");
	}

	// Free the Strings used for comparaison
	g_string_free((GString*)nodeType, TRUE);
	g_string_free((GString*)client_str, TRUE);
	g_string_free((GString*)clientVpn_str, TRUE);
	g_string_free((GString*)clientTor_str, TRUE);
	g_string_free((GString*)server_str, TRUE);

	if(!is_instanciation_done) {
		//pcap_replay_free(pcapReplay);
		pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__,
				"Cannot instanciate the pcap plugin ! Exiting plugin");
		pcap_replay_free(pcapReplay);
		return NULL;
	}
	/* Everything went OK !
	 * pcapReplay is now a client connected to the server
	 * Or a server waiting for a client connection */
	return pcapReplay;
}

void pcap_replay_ready(Pcap_Replay* pcapReplay) {
	g_assert(pcapReplay && (pcapReplay->magic == MAGIC));

	/* Collect the events that are ready 
	 * Then activate client or server with corresponding events (EPOLLIN &| EPOLLOUT)*/
	struct epoll_event epevs[100];
	gint nfds = epoll_wait(pcapReplay->ed, epevs, 100, 0);
	pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "NFD %d", nfds);
	if(nfds == -1) {
		pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "error in epoll_wait");
	} else {
		for(gint i = 0; i < nfds; i++) {
			gint d = epevs[i].data.fd;
			uint32_t e = epevs[i].events;
			if(d == pcapReplay->client.server_sd_tcp || d == pcapReplay->client.tfd_sendtimer) {
				_pcap_activateClient(pcapReplay, d, e);
			} else {
				_pcap_activateServer(pcapReplay, d, e);
			}
		}
	}
}

void _pcap_epoll(Pcap_Replay* pcapReplay, gint operation, guint32 events, int sd) {
	g_assert(pcapReplay && (pcapReplay->magic == MAGIC));

	struct epoll_event ev;
	memset(&ev, 0, sizeof(struct epoll_event));
	ev.events = events;
	ev.data.fd = sd;

	gint res = epoll_ctl(pcapReplay->ed, operation, sd, &ev);
	if(res == -1) {
		pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__, "Error in epoll_ctl");
	}
}

gint pcap_replay_getEpollDescriptor(Pcap_Replay* pcapReplay) {
	g_assert(pcapReplay && (pcapReplay->magic == MAGIC));
	return pcapReplay->ed;
}

gboolean pcap_replay_isDone(Pcap_Replay* pcapReplay) {
	g_assert(pcapReplay && (pcapReplay->magic == MAGIC));
	return pcapReplay->isDone;
}

void pcap_replay_free(Pcap_Replay* pcapReplay) {
	g_assert(pcapReplay && (pcapReplay->magic == MAGIC));

	if(pcapReplay->ed) {
		close(pcapReplay->ed);
	}
	if(pcapReplay->client.server_sd_tcp) {
		close(pcapReplay->client.server_sd_tcp);
	}
	if(pcapReplay->server.sd_tcp) {
		close(pcapReplay->server.sd_tcp);
	}
	if(pcapReplay->server.sd_udp) {
		close(pcapReplay->server.sd_udp);
	}
	if(pcapReplay->serverHostName) {
		g_string_free(pcapReplay->serverHostName, TRUE);
	}
	while(!g_queue_is_empty(pcapReplay->pcapStructQueue)) {
		pcap_t * pcap = g_queue_pop_head(pcapReplay->pcapStructQueue);
		if(pcap) {
			pcap_close(pcap);
		}
	}
	while(!g_queue_is_empty(pcapReplay->pcapFilePathQueue)) {
		GString * s = g_queue_pop_head(pcapReplay->pcapFilePathQueue);
		if(s) {
			g_string_free(s,TRUE);
		}
	}
	g_queue_free(pcapReplay->pcapStructQueue);
	g_queue_free(pcapReplay->pcapFilePathQueue);
	pcapReplay->magic = 0;
	g_free(pcapReplay);
}

gboolean get_next_packet(Pcap_Replay* pcapReplay, gboolean isClient) {
	/* Get first the first pcap packet matching the IP:PORT received in argv 
	 * Example : 
	 * If in the pcap file the client have the IP:Port address 192.168.1.2:5555 
	 * and the server have the IP:Port address 192.168.1.3:80. 
	 * Then, if the plugin is instanciated as a client, the client needs to resend
	 * the packet with ip.source=192.168.1.2 & ip.destination=192.168.1.3 & port.dest=80
	 * to the remote server.
	 * On the contrary, if the plugin is instanciated as a server, the server needs to wait
	 * for a client connection. When the a client is connected, it starts to resend packets 
	 * with ip.source=192.168.1.3 & ip.dest=192.168.1.2 & port.dest=5555 */

	struct pcap_pkthdr *header;
	const u_char *pkt_data;
	int size = 0;
	gboolean exists = FALSE;

	//tcp info
	const struct sniff_ethernet *ethernet; /* The ethernet header */
	const struct sniff_ip *ip; /* The IP header */
	const struct sniff_tcp *tcp; /* The TCP header */
	u_int size_ip_header;
	u_int size_tcp_header;
	u_int size_payload;
	char *payload;

	while((size = pcap_next_ex(pcapReplay->pcap, &header, &pkt_data)) >= 0) {
		// There exists a next packet in the pcap file
		// Retrieve header information
		ethernet = (struct sniff_ethernet*)(pkt_data);
		// ensure we are dealing with an ipv4 packet
		if (ethernet->ether_type != 8) {
			continue;
		}

		ip = (struct sniff_ip*)(pkt_data + SIZE_ETHERNET);
		size_ip_header = IP_HL(ip)*4;
		// ensure that we are dealing with tcp
		// or if we're interested in tunneling udp over tcp in the case of vpn
		if (ip->ip_p == '\x06') {
			// tcp
			// check if it is a control message and skip
			tcp = (struct sniff_tcp*)(pkt_data + SIZE_ETHERNET + size_ip_header);
			if (!(tcp->th_flags & TH_PUSH)) {
				// does not have any payload
				continue;
			}

			// if vpn, then encapsulate the entire TCP packet
			if (pcapReplay->isVpn) {
				payload = (char *)(pkt_data + SIZE_ETHERNET + size_ip_header);
				size_payload = ntohs(ip->ip_len) - size_ip_header;
			}
			else {
				// only extract payload
				size_tcp_header = TH_OFF(tcp)*4;
				payload = (char *)(pkt_data + SIZE_ETHERNET + size_ip_header + size_tcp_header);
				size_payload = ntohs(ip->ip_len) - (size_ip_header + size_tcp_header);
			}
		}
		else if (ip->ip_p == '\x11' && pcapReplay->isVpn) {
			// udp over tcp
			payload = (char *)(pkt_data + SIZE_ETHERNET + size_ip_header);
			size_payload = ntohs(ip->ip_len) - size_ip_header;
		}
		else {
			// not of interest
			continue;
		}

		// Client scenario
		if(isClient) {
			// next packet must be one with ip.src == client_IP_in_pcap.s_addr
			if(ip->ip_src.s_addr == pcapReplay->client_IP_in_pcap.s_addr) {
				// AND ip.dst must not be a local address (e.g., should not belong to 192.168.0.0/16)
				// or (ip.dst < pcapReplay->local_net_addr OR ip.dst > pcapReplay->local_net_addr + pcapReplay->local_net_mask)
				if( ntohl(ip->ip_dst.s_addr) < ntohl(pcapReplay->pcap_local_nw_addr.s_addr)
					|| ntohl(ip->ip_dst.s_addr) > ntohl(pcapReplay->pcap_local_nw_addr.s_addr) + pcapReplay->pcap_local_nw_mask) {

					pcapReplay->nextPacket = g_new0(Custom_Packet_t, 1);
					pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "Found at least one matching packet in next_packet. Destination -> %s",
						inet_ntoa(ip->ip_dst));
					exists = TRUE;
					pcapReplay->nextPacket->timestamp.tv_sec = header->ts.tv_sec;
					pcapReplay->nextPacket->timestamp.tv_usec = header->ts.tv_usec;
					pcapReplay->nextPacket->payload_size = size_payload;
					pcapReplay->nextPacket->payload = payload;
					break;
				}
			}	
		} 
		// Server scenario 
		else {
			// ip.src must not be a local address (e.g., should not belong to 192.168.0.0/16)
			// or (ip.src < pcapReplay->local_net_addr OR ip.src > pcapReplay->local_net_addr + pcapReplay->local_net_mask)
			if(ntohl(ip->ip_src.s_addr) < ntohl(pcapReplay->pcap_local_nw_addr.s_addr)
				|| ntohl(ip->ip_src.s_addr) > ntohl(pcapReplay->pcap_local_nw_addr.s_addr) + pcapReplay->pcap_local_nw_mask) {
				// AND next packet must be one with ip.dst == client_IP_in_pcap.s_addr (reverse flow) 
				if(ip->ip_dst.s_addr == pcapReplay->client_IP_in_pcap.s_addr) {

					pcapReplay->nextPacket = g_new0(Custom_Packet_t, 1);
					pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "Found at least one matching packet in next_packet. Destination -> %s",
						inet_ntoa(ip->ip_dst));
					exists = TRUE;
					pcapReplay->nextPacket->timestamp.tv_sec = header->ts.tv_sec;
					pcapReplay->nextPacket->timestamp.tv_usec = header->ts.tv_usec;
					pcapReplay->nextPacket->payload_size = size_payload;
					pcapReplay->nextPacket->payload = payload;
					break;
				}
			}		
		}
	} 
	return exists;
}

void deinstanciate(Pcap_Replay* pcapReplay, gint sd) {
	epoll_ctl(pcapReplay->ed, EPOLL_CTL_DEL, sd, NULL);
	close(sd);
	pcapReplay->client.server_sd_tcp = 0;
	pcapReplay->isDone = TRUE;
	pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
					"Plugin deinstanciated, exiting plugin !");
}

int timeval_subtract (struct timespec *result, struct timeval *y, struct timeval *x)
{
	/* Perform the carry for the later subtraction by updating y. */
	if (x->tv_usec < y->tv_usec) {
		int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
		y->tv_usec -= 1000000 * nsec;
		y->tv_sec += nsec;
	}
	if (x->tv_usec - y->tv_usec > 1000000) {
		int nsec = (x->tv_usec - y->tv_usec) / 1000000;
		y->tv_usec += 1000000 * nsec;
		y->tv_sec -= nsec;																																																																		
	}

	/* Compute the time remaining to wait.
	 * tv_usec is certainly positive. */
	result->tv_sec = x->tv_sec - y->tv_sec;
	result->tv_nsec = (x->tv_usec - y->tv_usec)*1000;

	/* Return 1 if result is negative. */
	return x->tv_sec < y->tv_sec;
}

gboolean change_pcap_file_to_send(Pcap_Replay* pcapReplay) {
	pcap_t * pcap = g_queue_pop_head(pcapReplay->pcapStructQueue);
	pcap_close(pcap);

	pcap= NULL;
	char ebuf[PCAP_ERRBUF_SIZE];
	GString * pcap_to_reset = g_queue_peek_head(pcapReplay->pcapFilePathQueue);

	if ((pcap = pcap_open_offline(pcap_to_reset->str, ebuf)) == NULL) {
		pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__,
				"Unable to re-open the pcap file : %s",pcap_to_reset->str);
		return FALSE;
	} else{
		pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__,
				"Successfully reset pcap file : %s",pcap_to_reset->str);

		g_queue_push_tail(pcapReplay->pcapFilePathQueue,  g_queue_pop_head(pcapReplay->pcapFilePathQueue));
		g_queue_push_tail(pcapReplay->pcapStructQueue, pcap);																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																				

		pcapReplay->pcap =  g_queue_peek_head(pcapReplay->pcapStructQueue);
		return TRUE;
	}
}

gboolean restart_server(Pcap_Replay* pcapReplay) {
	shutdown(pcapReplay->server.sd_tcp,2);
	shutdown(pcapReplay->server.sd_udp,2);
	close(pcapReplay->server.sd_tcp);
	close(pcapReplay->server.sd_udp);
	return FALSE;
}

gboolean restart_client(Pcap_Replay* pcapReplay) {
	struct timespec timewait; // Time to wait before isRestarting
	timewait.tv_sec=60;
	timewait.tv_nsec=0;

	/* UNCOMMENT IF YOU WANT THE CONNECTION TO BE CLOSED 
	 * AND RESTARTED AFTER SENDING EACH PCAP FILE */

	// Finish the connection if not already done 
	shutdown(pcapReplay->client.server_sd_tcp,2);
	close(pcapReplay->client.server_sd_tcp);
	return FALSE;

	// // renew pcap descriptor in use
	// if(!change_pcap_file_to_send(pcapReplay)) {
	// 	pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "Cannot change pcap file to send ! Exiting");
	// 	return FALSE;
	// };

	// if(!get_next_packet(pcapReplay)) {
	// 	pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__, "Cannot find a matching packet in the pcap file ! Exiting");
	// 	return FALSE;
	// }

	// // Sleep for timeToWait before isRestarting the client 
	// nanosleep((const struct timespec*)&timewait,NULL); 

	/* UNCOMMENT IF YOU WANT THE CONNECTION TO BE CLOSED 
	 * AND RESTARTED AFTER EACH PCAP FILE */
	/*

	pcapReplay->isRestarting = TRUE;
	if(pcapReplay->isTorClient==FALSE) {
		// The current instance is a normal client, then restart as a normal client
		if(!pcap_StartClient(pcapReplay)) {
			pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__,
					"Unable to restart the client (re-connect to server failed) ! Exiting ");
			return FALSE;
		} else{
			return TRUE;
		}
	} else {
		// The current instance is a Tor client, then restart as a Tor client
		if(!pcap_StartClientTor(pcapReplay)) {
			pcapReplay->slogf(G_LOG_LEVEL_CRITICAL, __FUNCTION__,
					"Unable to restart the Tor client (re-connect to server failed) ! Exiting ");
			return FALSE;
		} else{
			return TRUE;
		}
	}
	*/

	// return TRUE;
}

gboolean initiate_conn_to_proxy(Pcap_Replay* pcapReplay) {
	/* This function is used to connect to the Tor proxy.
	 * The client needs to respect the Socks5 protocol to create 
	 * a remote connection through Tor. The negociation between the client 
	 * and the proxy is done in 4 steps (described hereafter).
	 *
	 * - Note that this function is called right after the client connect() 
	 *   to the Tor proxy. Then the client is already abble to communicate with the proxy
	 * - This function will ask the Tor proxy to create a remote connection to the server.
	 *   When the negociation is over, the client will be able to send data to the proxy 
	 *   and the proxy will be in charge of "forwarding" the data to the remote server.
	 * - Also note that is a *VERY VERY BASIC* implementation of the Socks negociation protocol. 
	 *   This means that if something goes wrong when connecting to the proxy, the shadow experiment will fail!
	 *   See shd-tgen-transport.c for more information about the negociation protocol !
	 **/

	/* Step 1)
	* Send authentication (5,1,0) to Tor proxy (Socks V.5) */
	pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
				"Start Socks5 negociation protocol : Send Auth message to the Tor proxy.");

	gssize bytesSent = send_to_proxy(pcapReplay, "\x05\x01\x00", 3);
	g_assert(bytesSent == 3);
	pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
				"Step 1 : Finished : Authentication to Tor proxy succeeded.");


	/* Step 2) 
	 * socks choice client <-- server
	 *
	 * \x05 (version 5)
	 * \x00 (auth method choice - \xFF means none supported)
	 */
	gchar step2_buffer[8];
	memset(step2_buffer, 0, 8);
	gssize bytesReceived = recv_from_proxy(pcapReplay, step2_buffer, 2);
	g_assert(bytesReceived == 2);

	// Verify answer (2)
	if(step2_buffer[0] == 0x05 && step2_buffer[1] == 0x00) {
		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
				"Step 2 : Finished : Socks choice supported by the proxy");
	} else {
		pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__,
				"Step 2 : Failed :Socks choice unsupported by the proxy ! Exiting ");
		return FALSE;
	}

	/* Step 3)
	 * socks request sent in (1) client --> server
	 * \x05 (version 5)
	 * \x01 (tcp stream)
	 * \x00 (reserved) 
	 *
	 *	--> the client asks the server to connect to a remote server 
	 * 3a) ip address client --> server
	 *  \x01 (ipv4)
	 * in_addr_t (4 bytes)
	 * in_port_t (2 bytes)

	 * 3b) hostname client --> server
	 * \x03 (domain name)
	 * \x__ (1 byte name len)
	 * (name)
	 * in_port_t (2 bytes)
	 * 
	 * We use method 3a !
	 */

	struct addrinfo* info;
	int ret = getaddrinfo(pcapReplay->serverHostName->str, NULL, NULL, &info);
	if(ret >= 0) {
		pcapReplay->serverIP = ((struct sockaddr_in*)(info->ai_addr))->sin_addr.s_addr;
	}
	freeaddrinfo(info);

	/* case 3a - IPv4 */
	in_addr_t ip = pcapReplay->serverIP;
	in_addr_t port = pcapReplay->serverPortTCP;

	gchar step3_buffer[16];
	memset(step3_buffer, 0, 16);

	g_memmove(&step3_buffer[0], "\x05\x01\x00\x01", 4);
	g_memmove(&step3_buffer[4], &ip, 4);
	g_memmove(&step3_buffer[8], &port, 2);

	bytesSent = send_to_proxy(pcapReplay, step3_buffer, 10);
	g_assert(bytesSent == 10);

	pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
				"Step 3 : Finished : Send TCP create connection to remote server to the proxy.");

	/*
	* Step 4 : socks response client <-- server
	* \x05 (version 5)
	* \x00 (request granted)
	* \x00 (reserved)
	*
	* --> the server can tell us that we need to reconnect elsewhere
	*
	* 4a) ip address client <-- server
	* \x01 (ipv4)
	* in_addr_t (4 bytes)
	* in_port_t (2 bytes)
	*
	* 4b hostname client <-- server
	* \x03 (domain name)
	* \x__ (1 byte name len)
	* (name)
	* in_port_t (2 bytes)
	*/

	gchar step4_buffer[256];
	memset(step4_buffer, 0, 256);
   	bytesReceived = recv_from_proxy(pcapReplay, step4_buffer, 256);
	g_assert(bytesReceived >= 4);

	if(step4_buffer[0] == 0x05 && step4_buffer[1] == 0x00) {
		// Request Granted by the proxy !
		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
				"Step 4 : Finished : TCP connection to remote server created.");
 	} else {
		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
				"Step 4 : Error : TCP connection to remote server cannot be created.");
		return FALSE;
	}
	return TRUE;
}


ssize_t send_packet(Custom_Packet_t* cp, gint sd) {
	// Send the payload of the custom packet through the socket sd
	char message[cp->payload_size];
	memcpy(message, (const char*) cp->payload, (size_t)cp->payload_size);
	return send(sd, message, (size_t)cp->payload_size, 0);
}

gssize send_to_proxy(Pcap_Replay* pcapReplay, gpointer buffer, gsize length) {
	/* This function is used to send commands to the proxy 
	 * Used during the negociation phase */
	gssize bytes = write(pcapReplay->client.server_sd_tcp, buffer, length);

	if(bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
				"unable to send command to the Tor proxy ! Exiting");
		return -1;
	}
	if(bytes >= 0) {
		pcapReplay->slogf(G_LOG_LEVEL_DEBUG, __FUNCTION__,
				"Command sent to proxy : %s ",buffer);
	}
	return bytes;
}

gssize recv_from_proxy(Pcap_Replay* pcapReplay, gpointer buffer, gsize length) {
	/* This function is used to receive commands from the proxy 
	 * Used during the negociation phase */
	gssize bytes = read(pcapReplay->client.server_sd_tcp, buffer, length);

	if(bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		pcapReplay->slogf(G_LOG_LEVEL_ERROR, __FUNCTION__,
				"unable to receive command from the Tor proxy ! Exiting");
	}
	if(bytes >= 0) {
		pcapReplay->slogf(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
				"Command received from the proxy");
	}
	return bytes;
}
