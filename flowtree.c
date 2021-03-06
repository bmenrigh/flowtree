#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <errno.h>

/* We want to favor the BSD structs over the Linux ones */
#ifndef __USE_BSD
#define __USE_BSD
#endif

#ifndef __FAVOR_BSD
#define __FAVOR_BSD
#endif

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

/* The threading stuff */
#include <pthread.h>

/* The AVL tree */
#include "pavl.h"

/* The listen loop and thread(s) */
int terminate = 0;

/* Network stuff */
#define LISTENADDR "132.239.1.114"
#define LISTENPORT 2055
#define SOCKBUFF 1024 * 1024 /* 1 MB */
#define RECVBUFFSIZE 65536

#define SENDSRC "127.0.0.1"
#define SENDDST "127.0.0.1"
#define SENDPORT 2056
#define SENDBUFFSIZE 65536
int send_fh;


/* ===
 * Netflow structs and other values
 * http://www.cisco.com/en/US/docs/net_mgmt/netflow_collection_engine/
 * 3.6/user/guide/format.html#wp1006108
 * ===
 */

/* === Netflow v5 === */
struct netflow_v5 {
  uint16_t version;
  uint16_t flow_count;
  uint32_t uptime;
  uint32_t unix_sec;
  uint32_t nsec;
  uint32_t flow_sequence;
  uint8_t engine_type;
  uint8_t engine_id;
  uint16_t sample_rate;
} __attribute__((__packed__));

struct netflow_v5_record {
  in_addr_t src_addr;
  in_addr_t dst_addr;
  in_addr_t next_hop;
  uint16_t src_int; 
  uint16_t dst_int;
  uint32_t num_packets;
  uint32_t num_bytes;
  uint32_t start_time;
  uint32_t end_time;
  uint16_t src_port;
  uint16_t dst_port;
  uint8_t pad1;
  uint8_t tcp_flags;
  uint8_t protocol;
  uint8_t tos;
  uint16_t src_as;
  uint16_t dst_as;
  uint8_t src_mask;
  uint8_t dst_mask;
  uint16_t pad2;
} __attribute__((__packed__));  


/* === Netflow v7 === */
struct netflow_v7 {
  uint16_t version;
  uint16_t flow_count;
  uint32_t uptime;
  uint32_t unix_sec;
  uint32_t nsec;
  uint32_t flow_sequence;
  uint32_t reserved;
} __attribute__((__packed__));

struct netflow_v7_record {
  in_addr_t src_addr;
  in_addr_t dst_addr;
  in_addr_t next_hop;
  uint16_t src_int; 
  uint16_t dst_int;
  uint32_t num_packets;
  uint32_t num_bytes;
  uint32_t start_time;
  uint32_t end_time;
  uint16_t src_port;
  uint16_t dst_port;
  uint8_t flags1;
  uint8_t tcp_flags;
  uint8_t protocol;
  uint8_t tos;
  uint16_t src_as;
  uint16_t dst_as;
  uint8_t src_mask;
  uint8_t dst_mask;
  uint16_t flags2;
  uint32_t flow_src;
} __attribute__((__packed__));  


/* ===
 * The unified flow struct that all other formats will be converted to
 * ===
 */
struct unified_flow {
  in_addr_t flow_src;
  time_t recv_time;
  uint16_t src_int;
  uint16_t dst_int;
  struct in_addr src_addr;
  struct in_addr dst_addr;
  uint8_t protocol;
  uint16_t src_port;
  uint16_t dst_port;
  uint8_t tcp_flags;
  uint32_t num_packets;
  uint32_t num_bytes;
  time_t start_time;
  time_t end_time;
};


/* ===
 * The flow summary to insert into the flow trees
 * ===
 */
struct flow_source_summary {
  in_addr_t flow_src;
  uint16_t src_int;
  uint16_t dst_int;
  uint64_t num_packets;
  uint64_t num_bytes;  
  uint64_t num_flows;  
  struct flow_source_summary *next;
};

struct flow_summary {
  time_t time_added;
  time_t time_updated;
  struct in_addr src_addr;
  struct in_addr dst_addr;
  uint8_t protocol;
  uint16_t src_port;
  uint16_t dst_port;
  uint8_t tcp_flags;
  time_t start_time;
  time_t end_time;
  uint8_t source_count;
  struct flow_source_summary *sources;
};


/* ===
 * The exclude list structs and vars
 * ===
 */
struct exclude_node {
  in_addr_t addr_start;
  in_addr_t addr_end;
  uint64_t exclude_count;
};

struct pavl_table *exclude_tree;  


/* ===
 * Function prototypes
 * ===
 */
int main(int, char * const []);
void sig_terminate(int);
void packet_callback(const struct sockaddr_in *, const u_char *,
		     const size_t, const time_t);
void parse_netflow_v5(const struct sockaddr_in *, const u_char *,
		      const size_t, const time_t);
void parse_netflow_v7(const struct sockaddr_in *, const u_char *,
		      const size_t, const time_t);
void flow_callback(const struct unified_flow *);
int compare_flows(const void *, const void *, void *);
int compare_excludes(const void *, const void *, void *);
void * copy_flow(const void *, void *);
void add_exclusion(const in_addr_t, const in_addr_t);
int is_excluded(const in_addr_t);
void *thread_flow_janitor(void *);
void free_source_list(struct flow_source_summary *);
void print_flow_json(const struct flow_summary *);


/* ===
 * Datastructure stuff
 * ===
 */
#define TREES 65536

struct hash_node_tree {
  struct pavl_table *tree;
  pthread_mutex_t tree_mutex;
};

struct hash_node_tree flow_hash_trees[TREES];  


/* === The purge parameters === */
#define MIN_FLOW_AGE 60
#define MAX_FLOW_AGE 300


#define ROL16(x, a) ((((x) << (a))  & 0xFFFF) | (((x) & 0xFFFF) >> (16 - (a))))

#define TREEHASH(f) (((((struct flow_summary *)				\
			(f))->src_addr.s_addr) &			\
		      0xFFFF) ^						\
		     (ROL16((((((struct flow_summary *)			\
				(f))->src_addr.s_addr) &		\
			      0xFFFF0000) >> 16), 7)) ^			\
		     ((((struct flow_summary *)				\
			(f))->dst_addr.s_addr) &			\
		      0xFFFF) ^						\
		     (ROL16((((((struct flow_summary *)			\
				(f))->dst_addr.s_addr) &		\
			      0xFFFF0000) >> 16), 13)) ^		\
		     (((struct flow_summary *)				\
		       (f))->src_port) ^				\
		     (ROL16((((struct flow_summary *)			\
			      (f))->dst_port), 3)) ^			\
		     (((struct flow_summary *)(f))->protocol))


/* ===
 * Some stats vars
 * ===
 */
uint64_t stat_flow_packets = 0, stat_total_flows = 0, stat_excluded_flows = 0;
uint64_t stat_new_flows = 0, stat_dup_flows = 0, stat_current_flows = 0;
uint64_t stat_proto_flows[256];
pthread_mutex_t stat_current_mutex = PTHREAD_MUTEX_INITIALIZER;


/* ===
 * The global time vars
 * ===
 */
#define STATS_RATE 60
time_t last_stats_update;
time_t start_time;


int main(int argc, char * const argv[]) {

  /* === Signal vars === */
  struct sigaction sa_new, sa_old;
  sigset_t sigmask, emptysigmask;

  /* === Socket vars === */
  struct sockaddr_in bind_addrin, peer_addrin, send_addrin;
  in_addr_t bind_addr;
  in_addr_t send_addr;
  int sock_fh;
  int setsockbuff = SOCKBUFF, getsockbuff;
  socklen_t sockbufflen = sizeof(getsockbuff);
  socklen_t peeraddrlen = sizeof(peer_addrin);

  /* === Network data vars === */
  u_char buffer[RECVBUFFSIZE];
  ssize_t msgsize;
  fd_set read_fd;
  struct timespec sel_timespec;
  int select_ret;
  time_t recv_time;

  /* === Thread vars === */
  pthread_t flow_janitor;
  int thread_ret;

  /* === Misc vars === */
  time_t cur_time;
  uint32_t time_diff;
  int i;

  /* Before we start listening we need to setup a signal
   * handler so we can cleanly exit */
  memset(&sa_new, 0, sizeof(struct sigaction));
  sa_new.sa_handler = sig_terminate;
  sigaction(SIGTERM, &sa_new, &sa_old);
  memset(&sa_new, 0, sizeof(struct sigaction));
  sa_new.sa_handler = sig_terminate;
  sigaction(SIGINT, &sa_new, &sa_old);

  /* Setup the masks for pselect() */
  sigemptyset(&emptysigmask);
  sigemptyset(&sigmask);
  sigaddset(&sigmask, SIGTERM);
  sigaddset(&sigmask, SIGINT);


  /* Make our listen socket */
  if ((sock_fh = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
    fprintf(stderr, "Creation of listen socket failed.\n");
    return 1;
  }

  /* Try to set the socket buffer */
  if (setsockopt(sock_fh, SOL_SOCKET, SO_RCVBUF,
		 &setsockbuff, sizeof(setsockbuff)) == -1) {
    fprintf(stderr, "Setting listen socket receive buffer failed.\n");
    return 1;
  }

  /* Now find out what our socket buffer really is set to */
  if (getsockopt(sock_fh, SOL_SOCKET, SO_RCVBUF,
		 &getsockbuff, &sockbufflen) == -1) {
    fprintf(stderr, "Unable to get listen socket receive buffer.\n");
    return 1;
  }
  else {
    fprintf(stderr, "Listen socket receive buffer is %d bytes\n", getsockbuff);
  }


  /* Make our send socket */
  if ((send_fh = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
    fprintf(stderr, "Creation of send socket failed.\n");
    return 1;
  }

  /* Try to set the send socket buffer */
  if (setsockopt(send_fh, SOL_SOCKET, SO_SNDBUF,
		 &setsockbuff, sizeof(setsockbuff)) == -1) {
    fprintf(stderr, "Setting send socket send buffer failed.\n");
    return 1;
  }

  /* Now find out what our socket buffer really is set to */
  if (getsockopt(send_fh, SOL_SOCKET, SO_SNDBUF,
		 &getsockbuff, &sockbufflen) == -1) {
    fprintf(stderr, "Unable to get send socket send buffer.\n");
    return 1;
  }
  else {
    fprintf(stderr, "Send socket send buffer is %d bytes\n", getsockbuff);
  }


  /* Setup the binding struct */
  bind_addr = inet_addr(LISTENADDR);
  memset(&bind_addrin, 0, sizeof(bind_addrin));
  bind_addrin.sin_family = AF_INET;
  bind_addrin.sin_port = htons(LISTENPORT);
  bind_addrin.sin_addr.s_addr = bind_addr;

  /* Do the bind */
  if (bind(sock_fh, (const struct sockaddr *)&bind_addrin,
	   sizeof(bind_addrin)) == -1) {
    fprintf(stderr, "Binding to socket failed.\n");
    perror("bind");
    return 1;
  }  


  /* Setup the send binding struct */
  send_addr = inet_addr(SENDSRC);
  memset(&send_addrin, 0, sizeof(send_addrin));
  send_addrin.sin_family = AF_INET;
  send_addrin.sin_port = 0;
  send_addrin.sin_addr.s_addr = send_addr;

  /* Do the send bind */
  if (bind(send_fh, (const struct sockaddr *)&send_addrin,
	   sizeof(send_addrin)) == -1) {
    fprintf(stderr, "Binding to sending socket failed.\n");
    perror("bind");
    return 1;
  }  

  
  /* Create the exclude tree */
  exclude_tree = pavl_create(compare_excludes, NULL, NULL);

  /* Populate the exclusion tree */
  add_exclusion(inet_network("132.239.1.114"), inet_network("132.239.1.116"));
  add_exclusion(inet_network("132.239.1.199"), inet_network("132.239.1.204"));
  add_exclusion(inet_network("44.0.0.0"), inet_network("44.255.255.255"));


  /* Create the flow trees */
  for (i = 0; i < TREES; i++) {
    flow_hash_trees[i].tree = pavl_create(compare_flows, NULL, NULL);
    pthread_mutex_init(&(flow_hash_trees[i].tree_mutex), NULL);
  }

  /* Record what time we started */
  start_time = time(NULL);
  last_stats_update = start_time;

  /* Before listening, start the janitor thread */
  thread_ret = pthread_create(&flow_janitor, NULL, thread_flow_janitor, NULL);
  
  /* Testing receive, will do better in final code */
  while (terminate == 0) {

    /* Check if we need to update the stats */
    cur_time = time(NULL);
    time_diff = cur_time - last_stats_update;

    if (time_diff >= STATS_RATE) {
      last_stats_update = cur_time;

      time_diff = cur_time - start_time;

      if (stat_new_flows == 0) {
	fprintf(stderr, "--\n");
	fprintf(stderr, "NO FLOWS\n");
      }
      else {
	fprintf(stderr, "--\n");
	fprintf(stderr, "flowtree stats:\n");
	fprintf(stderr, "===============\n");
	fprintf(stderr, "runtime: %d seconds; total packets: %lu; "
		"total flows: %lu\n", (int)time_diff,
		stat_flow_packets, stat_total_flows);
	fprintf(stderr, "packet rate: %.02f pps; "
		"flow rate: %.02f fps; new flow rate %.02f fps\n",
		(double)stat_flow_packets / (double)time_diff,
		(double)(stat_total_flows) / (double)time_diff,
		(double)stat_new_flows / (double)time_diff);
	fprintf(stderr, "excluded flows: %lu (%.02f%%)\n",
		stat_excluded_flows, ((double)stat_excluded_flows /
				      (double)stat_total_flows) * 100);

	/* === *** ACQUIRE STATS LOCK *** === */
	pthread_mutex_lock(&stat_current_mutex);

	fprintf(stderr, "currently tracking flows: %lu\n", stat_current_flows);

	/* === *** UNLOCK STATS LOCK *** === */
	pthread_mutex_unlock(&stat_current_mutex);

	fprintf(stderr, "total unique flows: %lu (%.02f%%)\n",
		stat_new_flows, ((double)stat_new_flows /
				 (double)(stat_total_flows)) * 100);
	fprintf(stderr, "unique tcp flows: %lu (%.02f%%)\n",
		stat_proto_flows[6], ((double)stat_proto_flows[6] /
				      (double)stat_new_flows) * 100);
	fprintf(stderr, "unique udp flows: %lu (%.02f%%)\n",
		stat_proto_flows[17], ((double)stat_proto_flows[17] /
				       (double)stat_new_flows) * 100);
	fprintf(stderr, "unique icmp flows: %lu (%.02f%%)\n",
		stat_proto_flows[1], ((double)stat_proto_flows[1] /
				      (double)stat_new_flows) * 100);
	fprintf(stderr, "unique eth-in-ip flows: %lu (%.02f%%)\n",
		stat_proto_flows[97], ((double)stat_proto_flows[97] /
				       (double)stat_new_flows) * 100);
	fprintf(stderr, "unique 6in4 flows: %lu (%.02f%%)\n",
		stat_proto_flows[41], ((double)stat_proto_flows[41] /
				       (double)stat_new_flows) * 100);
	fprintf(stderr, "unique pim flows: %lu (%.02f%%)\n",
		stat_proto_flows[103], ((double)stat_proto_flows[103] /
					(double)stat_new_flows) * 100);
	fprintf(stderr, "unique igmp flows: %lu (%.02f%%)\n",
		stat_proto_flows[2], ((double)stat_proto_flows[2] /
				      (double)stat_new_flows) * 100);
	fprintf(stderr, "unique ip in ip flows: %lu (%.02f%%)\n",
		stat_proto_flows[4], ((double)stat_proto_flows[4] /
				      (double)stat_new_flows) * 100);
	fprintf(stderr, "unique eigrp flows: %lu (%.02f%%)\n",
		stat_proto_flows[88], ((double)stat_proto_flows[88] /
				       (double)stat_new_flows) * 100);
	fprintf(stderr, "unique esp flows: %lu (%.02f%%)\n",
		stat_proto_flows[50], ((double)stat_proto_flows[50] /
				       (double)stat_new_flows) * 100);
	fprintf(stderr, "unique ah flows: %lu (%.02f%%)\n",
		stat_proto_flows[51], ((double)stat_proto_flows[51] /
				       (double)stat_new_flows) * 100);
	fprintf(stderr, "unique gre flows: %lu (%.02f%%)\n",
		stat_proto_flows[47], ((double)stat_proto_flows[47] /
				       (double)stat_new_flows) * 100);
      }
    }


    /* prep for the select */
    FD_ZERO(&read_fd);
    FD_SET(sock_fh, &read_fd);
    sel_timespec.tv_sec = 0;
    sel_timespec.tv_nsec = 100000000; /* .1 seconds */

    /* Block signals */ /* NOT THREAD SAFE */
    sigprocmask(SIG_BLOCK, &emptysigmask, NULL);

    /* See if we have data */
    if ((select_ret = pselect(sock_fh + 1, &read_fd, NULL, NULL,
			      &sel_timespec, &emptysigmask)) == -1) {
      if (errno != EINTR) {
	fprintf(stderr, "Call to pselect() failed.\n");
	perror("pselect");
	return 1;
      }
    }

    /* Nothing became ready */
    if (select_ret <= 0) {
      continue;
    }


    /* Select says we have a message, grab it */
    if ((msgsize = recvfrom(sock_fh, buffer, RECVBUFFSIZE, 0,
			    (struct sockaddr *)&peer_addrin,
			    &peeraddrlen)) == -1) {
      fprintf(stderr, "recvfrom() call failed!\n");
      perror("recvfrom");
      return 1;
    }
    else {
      /*
      fprintf(stderr, "Got a packet from %s:%d; size=%d\n",
	      inet_ntoa(peer_addrin.sin_addr), ntohs(peer_addrin.sin_port),
	      (int)msgsize);
      */

      /* We need to fix the byte order for the peer */
      peer_addrin.sin_addr.s_addr = ntohl(peer_addrin.sin_addr.s_addr);


      /* Update the counter */
      stat_flow_packets += 1;

      recv_time = time(NULL);
      packet_callback(&peer_addrin, buffer, msgsize, recv_time);
    }
    
  }

  /* === Stopped listening, must have gotten signal === */
  fprintf(stderr, "Waiting for threads to finish before exiting...\n");
  pthread_join(flow_janitor, NULL);

  close(sock_fh);

  return 0;
}


void packet_callback(const struct sockaddr_in *peer, const u_char *flow,
		     const size_t flow_size, const time_t recv_time) {

  /* Check for netflow v5 */
  if (flow_size > sizeof(struct netflow_v5)) {
    if (ntohs(((struct netflow_v5 *)flow)->version) == 5) {
      /* Maybe more checks should be added later... */

      parse_netflow_v5(peer, flow, flow_size, recv_time);
      return;
    }
  }

  /* Check for netflow v7 */
  if (flow_size > sizeof(struct netflow_v7)) {
    if (ntohs(((struct netflow_v7 *)flow)->version) == 7) {
      /* Maybe more checks should be added later... */

      parse_netflow_v7(peer, flow, flow_size, recv_time);
      return;
    }
  }

  /* Other version of netflow / sflow / jflow will be handled later */
  fprintf(stderr, "Got an uknown flow format\n");


}


void parse_netflow_v5(const struct sockaddr_in *peer, const u_char *flow,
		      const size_t flow_size, const time_t recv_time) {

  struct unified_flow current_flow;
  struct netflow_v5_record * record_v5;

  /* ===
   * Misc vars
   * ===
   */
  int records = 0;
  int i;

  /* ===
   * Do more sanity checks to make sure we have a netflow v5 record
   * === 
   */
  if (flow_size < sizeof(struct netflow_v5)) {
    fprintf(stderr, "v5 flow not big enough\n");
    return;
  }

  if (ntohs(((struct netflow_v5 *)flow)->version) != 5) {
    fprintf(stderr, "not v5\n");
    return;
  }

  records = ntohs(((struct netflow_v5 *)flow)->flow_count);
  if (flow_size != sizeof(struct netflow_v5) +
      (records * sizeof(struct netflow_v5_record))) {
    
    fprintf(stderr,
	    "wrong size; flow_count=%d; flow_size=%d; v5=%d, v5r=%d\n",
	    (int)ntohs(((struct netflow_v5 *)flow)->flow_count),
	    (int)flow_size, (int)sizeof(struct netflow_v5),
	    (int)sizeof(struct netflow_v5_record));
    return;
  }
  /*fprintf(stderr, "Got a valid looking netflow v5 packet\n");*/
  

  /* ===
   * Looks like valid netflow v5 so parse it
   * === 
   */
  
  /* Now loop through the records */
  record_v5 = (struct netflow_v5_record *)(flow + sizeof(struct netflow_v5));
  for (i = 0; i < records; i++) {
    
    /* Fill in our current flow info */
    current_flow.flow_src = peer->sin_addr.s_addr;
    current_flow.recv_time = recv_time;
    current_flow.src_int = ntohs(record_v5[i].src_int);
    current_flow.dst_int = ntohs(record_v5[i].dst_int);
    current_flow.src_addr.s_addr = ntohl(record_v5[i].src_addr);
    current_flow.dst_addr.s_addr = ntohl(record_v5[i].dst_addr);
    current_flow.protocol = record_v5[i].protocol;
    current_flow.src_port = ntohs(record_v5[i].src_port);
    current_flow.dst_port = ntohs(record_v5[i].dst_port);
    current_flow.tcp_flags = record_v5[i].tcp_flags;
    current_flow.num_packets = ntohl(record_v5[i].num_packets);
    current_flow.num_bytes = ntohl(record_v5[i].num_bytes);

    /* Time calculations require a bit of math, namely
     * curtime - ((uptime - start) / 1000)
     */
    current_flow.start_time = ntohl(((struct netflow_v5 *)flow)->unix_sec) -
      (((ntohl(((struct netflow_v5 *)flow)->uptime) -		\
	 ntohl(record_v5[i].start_time)) & 0xFFFFFFFF) / 1000);
    current_flow.end_time = ntohl(((struct netflow_v5 *)flow)->unix_sec) -
      (((ntohl(((struct netflow_v5 *)flow)->uptime) -		\
	 ntohl(record_v5[i].end_time)) & 0xFFFFFFFF) / 1000);

    /* Now handle the current unified flow */
    flow_callback(&current_flow);
  }
}


void parse_netflow_v7(const struct sockaddr_in *peer, const u_char *flow,
		      const size_t flow_size, const time_t recv_time) {

  struct unified_flow current_flow;
  struct netflow_v7_record * record_v7;

  /* ===
   * Misc vars
   * ===
   */
  int records = 0;
  int i;

  /* ===
   * Do more sanity checks to make sure we have a netflow v7 record
   * === 
   */
  if (flow_size < sizeof(struct netflow_v7)) {
    fprintf(stderr, "v7 flow not big enough\n");
    return;
  }

  if (ntohs(((struct netflow_v7 *)flow)->version) != 7) {
    fprintf(stderr, "not v7\n");
    return;
  }

  records = ntohs(((struct netflow_v7 *)flow)->flow_count);
  if (flow_size != sizeof(struct netflow_v7) +
      (records * sizeof(struct netflow_v7_record))) {
    
    fprintf(stderr,
	    "wrong size; flow_count=%d; flow_size=%d; v7=%d, v7r=%d\n",
	    (int)ntohs(((struct netflow_v7 *)flow)->flow_count),
	    (int)flow_size, (int)sizeof(struct netflow_v7),
	    (int)sizeof(struct netflow_v7_record));
    return;
  }
  /*fprintf(stderr, "Got a valid looking netflow v7 packet\n");*/
  

  /* ===
   * Looks like valid netflow v7 so parse it
   * === 
   */
  
  /* Now loop through the records */
  record_v7 = (struct netflow_v7_record *)(flow + sizeof(struct netflow_v7));
  for (i = 0; i < records; i++) {
    
    /* Fill in our current flow info */
    current_flow.flow_src = ntohl(record_v7[i].flow_src);
    current_flow.recv_time = recv_time;
    current_flow.src_int = ntohs(record_v7[i].src_int);
    current_flow.dst_int = ntohs(record_v7[i].dst_int);
    current_flow.src_addr.s_addr = ntohl(record_v7[i].src_addr);
    current_flow.dst_addr.s_addr = ntohl(record_v7[i].dst_addr);
    current_flow.protocol = record_v7[i].protocol;
    current_flow.src_port = ntohs(record_v7[i].src_port);
    current_flow.dst_port = ntohs(record_v7[i].dst_port);
    current_flow.tcp_flags = record_v7[i].tcp_flags;
    current_flow.num_packets = ntohl(record_v7[i].num_packets);
    current_flow.num_bytes = ntohl(record_v7[i].num_bytes);

    /* Time calculations require a bit of math, namely
     * curtime - ((uptime - start) / 1000)
     */
    current_flow.start_time = ntohl(((struct netflow_v7 *)flow)->unix_sec) -
      (((ntohl(((struct netflow_v7 *)flow)->uptime) -		\
	 ntohl(record_v7[i].start_time)) & 0xFFFFFFFF) / 1000);
    current_flow.end_time = ntohl(((struct netflow_v7 *)flow)->unix_sec) -
      (((ntohl(((struct netflow_v7 *)flow)->uptime) -		\
	 ntohl(record_v7[i].end_time)) & 0xFFFFFFFF) / 1000);

    /* Now handle the current unified flow */
    flow_callback(&current_flow);
  }
}


void flow_callback(const struct unified_flow *current_flow) {

  /* ===
   * Flow tree and summary vars
   * ===
   */
  struct flow_summary cur_flow_summary;
  struct flow_summary *flow_summary_copy;
  struct flow_summary **flow_summary_probe;
  struct flow_source_summary *new_flow_source_summary;
  struct flow_source_summary **cur_flow_source_summary;
  int tree_num;

  /* ===
   * Misc vars
   * ===
   */
  struct in_addr temp_inaddr_src, temp_inaddr_dst;
  int source_updated;

  /* ===
   * Update the stats that we got a flow
   * ===
   */
  stat_total_flows += 1;


  temp_inaddr_src.s_addr = htonl(current_flow->src_addr.s_addr);
  temp_inaddr_dst.s_addr = htonl(current_flow->dst_addr.s_addr);
  /*
    fprintf(stderr, "Got proto %d flow from %s:%d",
    current_flow->protocol,
    inet_ntoa(temp_inaddr_src),
    current_flow->src_port);
    fprintf(stderr, " to %s:%d (%u to %u)\n",
    inet_ntoa(temp_inaddr_dst),
    current_flow->dst_port,
    (unsigned int)current_flow->start_time,
    (unsigned int)current_flow->end_time);
  */


  /* ===
   * Check to see if this flow is excluded
   * === 
   */
  if ((is_excluded(current_flow->src_addr.s_addr) == 1) ||
      (is_excluded(current_flow->dst_addr.s_addr) == 1)) {

    /*
    fprintf(stderr, "excluded %s -> ", inet_ntoa(temp_inaddr_src));
    fprintf(stderr, "%s\n", inet_ntoa(temp_inaddr_dst));
    */

    stat_excluded_flows += 1;

    return;
  }


  /* ===
   * Now insert or update the flow in the tree
   * === 
   */

  /* Setup the current flow summary struct */
  cur_flow_summary.time_added = current_flow->recv_time;
  cur_flow_summary.time_updated = current_flow->recv_time;
  cur_flow_summary.src_addr = current_flow->src_addr;
  cur_flow_summary.dst_addr = current_flow->dst_addr;
  cur_flow_summary.protocol = current_flow->protocol;
  cur_flow_summary.src_port = current_flow->src_port;
  cur_flow_summary.dst_port = current_flow->dst_port;
  cur_flow_summary.tcp_flags = current_flow->tcp_flags;
  cur_flow_summary.start_time = current_flow->start_time;
  cur_flow_summary.end_time = current_flow->end_time;
  cur_flow_summary.source_count = 0; /* gets updated later */
  cur_flow_summary.sources = NULL;

  /* Now make an insert-ready copy */
  flow_summary_copy = copy_flow(&cur_flow_summary, NULL);

  /* Figure out which tree to use */
  tree_num = TREEHASH(flow_summary_copy);
   
  /* === *** ACQUIRE TREE LOCK *** === */
  pthread_mutex_lock(&(flow_hash_trees[tree_num].tree_mutex));

  /* Search and possibly insert this flow */
  flow_summary_probe =
    (struct flow_summary **)pavl_probe(flow_hash_trees[tree_num].tree,
				       flow_summary_copy);
  
  /* Figure out what happened */
  if (flow_summary_probe == NULL) {
    fprintf(stderr, "There was a failure inserting the flow into tree.\n");

    /* === *** RELEASE LOCK *** === */
    pthread_mutex_unlock(&(flow_hash_trees[tree_num].tree_mutex));

    return;
  }


  /* Now find out if it was already there or we just inserted it */
  if (*flow_summary_probe == flow_summary_copy) {
    /* well that was easy, nothing fancy to do now */

    /* should increment new flow counters */
    stat_new_flows++;

    /* === *** ACQUIRE STATS LOCK *** === */
    pthread_mutex_lock(&stat_current_mutex);

    stat_current_flows++;

    /* === *** UNLOCK STATS LOCK *** === */
    pthread_mutex_unlock(&stat_current_mutex);

    stat_proto_flows[(*flow_summary_probe)->protocol] += 1;
  }
  else {
    /* fprintf(stderr, "Flow already in tree; flows=%u\n",
       (unsigned int)pavl_count(flow_hash_trees[tree_num].tree));
    */

    /* update the stats */
    stat_dup_flows++;

      
    /* update some summay stuff about this flow */
    (*flow_summary_probe)->tcp_flags |= flow_summary_copy->tcp_flags;
    if ((*flow_summary_probe)->start_time > flow_summary_copy->start_time) {
      (*flow_summary_probe)->start_time = flow_summary_copy->start_time;
    }
    if ((*flow_summary_probe)->end_time < flow_summary_copy->end_time) {
      (*flow_summary_probe)->end_time = flow_summary_copy->end_time;
    }
    (*flow_summary_probe)->time_updated = flow_summary_copy->time_updated;
   
    /*
    fprintf(stderr, "Sources: %d\n",
	    (*flow_summary_probe)->source_count);
    */
    
    /* We don't need the copy anymore */
    free(flow_summary_copy);
    flow_summary_copy = NULL;
  }
    
  /*
    fprintf(stderr, "Stats: new=%lu, dup=%lu, cur=%lu, tcp=%lu, "
    "udp=%lu, icmp=%lu; oth=%lu\n",
    stat_new_flows, stat_dup_flows, stat_current_flows,
    stat_tcp_flows, stat_udp_flows, stat_icmp_flows,
    stat_other_flows);
  */
  
  /* ===
   * The flow is now in the tree, we need to update the flow source info
   * === 
   */
  
  /* Find the spot to update or where to insert */
  source_updated = 0;
  cur_flow_source_summary = &((*flow_summary_probe)->sources);
  while (*cur_flow_source_summary != NULL) {
    
    if (current_flow->flow_src < (*cur_flow_source_summary)->flow_src) {
      /* We are going to need to insert a new flow source here */
      break;
    }
    else if (current_flow->flow_src ==
	     (*cur_flow_source_summary)->flow_src) {
      /* We need to update this flow source */
      (*cur_flow_source_summary)->num_packets += current_flow->num_packets;
      (*cur_flow_source_summary)->num_bytes += current_flow->num_bytes;
      (*cur_flow_source_summary)->num_flows += 1;
      
      source_updated = 1;
      break;
    }
    else {
      /* Go on */
      cur_flow_source_summary = &((*cur_flow_source_summary)->next);
    }
  }
  
  /* If we didn't do an update then we need to insert a new flow source */
  if (source_updated == 0) {
    new_flow_source_summary = malloc(sizeof(struct flow_source_summary));
    
    /* Set the new fields */
    new_flow_source_summary->flow_src = current_flow->flow_src;
    new_flow_source_summary->src_int = current_flow->src_int;
    new_flow_source_summary->dst_int = current_flow->dst_int;
    new_flow_source_summary->num_packets = current_flow->num_packets;
    new_flow_source_summary->num_bytes = current_flow->num_bytes;
    new_flow_source_summary->num_flows = 1;
    
    /* Now insert this into the list */
    new_flow_source_summary->next = *cur_flow_source_summary;
    *cur_flow_source_summary = new_flow_source_summary;
    
    /* Update the source count for the flow */
    (*flow_summary_probe)->source_count += 1;
  }

  /* === *** RELEASE TREE LOCK *** === */
  pthread_mutex_unlock(&(flow_hash_trees[tree_num].tree_mutex));
  
}


void sig_terminate(int signo) {
  /* It is dangerous to do much more than this in a signal handler */
  terminate = 1;
}


int compare_flows(const void *a, const void *b, void *param) {

  const struct flow_summary *fa = a;
  const struct flow_summary *fb = b;

  if (fa->protocol > fb->protocol) {
    return 1;
  }
  else if (fa->protocol < fb->protocol) {
    return -1;
  }
  else if (fa->src_addr.s_addr > fb->src_addr.s_addr) {
    return 1;
  }
  else if (fa->src_addr.s_addr < fb->src_addr.s_addr) {
    return -1;
  }
  else if (fa->dst_addr.s_addr > fb->dst_addr.s_addr) {
    return 1;
  }
  else if (fa->dst_addr.s_addr < fb->dst_addr.s_addr) {
    return -1;
  }
  else if (fa->src_port > fb->src_port) {
    return 1;
  }
  else if (fa->src_port < fb->src_port) {
    return -1;
  }
  else if (fa->dst_port > fb->dst_port) {
    return 1;
  }
  else if (fa->dst_port < fb->dst_port) {
    return -1;
  }
  else {
    return 0;
  }
}


void * copy_flow(const void *a, void *param) {
  
  struct flow_summary *f = malloc(sizeof(struct flow_summary));

  if (f == NULL) {
    return NULL;
  }
  else {
    memcpy(f, a, sizeof(struct flow_summary));
  }
  
  return f;
}


int compare_excludes(const void *a, const void *b, void *param) {

  const struct exclude_node *ea = a;
  const struct exclude_node *eb = b;

  if (ea->addr_start > eb->addr_end) {
    return 1;
  }
  else if (ea->addr_end < eb->addr_start) {
    return -1;
  }
  else {
    return 0;
  }

}


void add_exclusion(const in_addr_t addr_start, const in_addr_t addr_end) {

  struct exclude_node *ex;
  struct exclude_node *ex_del;
  struct exclude_node **ex_probe;

  ex = malloc(sizeof(struct exclude_node));

  ex->addr_start = addr_start;
  ex->addr_end = addr_end;
  ex->exclude_count = 0;

  /* Search for and possibly insert this exclude */
  ex_probe = (struct exclude_node **)pavl_probe(exclude_tree, ex);
  
  /* Figure out what happened */
  if (ex_probe == NULL) {
    fprintf(stderr, "There was a failure inserting the exclude into tree.\n");
    return;
  }

  /* Now find out if it was already there or we just inserted it */
  if (*ex_probe == ex) {
    /* Great, it was inserted, nothing to do now */
    return;
  }
  else {

    /* First combine */
    if ((*ex_probe)->addr_start < ex->addr_start) {
      ex->addr_start = (*ex_probe)->addr_start;
    }
    if ((*ex_probe)->addr_end > ex->addr_end) {
      ex->addr_end = (*ex_probe)->addr_end;
    }

    /* Now remove the old exclude */
    ex_del = (struct exclude_node *)pavl_delete(exclude_tree, *ex_probe);

    /* Now insert this new combined exclude */
    add_exclusion(ex->addr_start, ex->addr_end);
    
    /* cleanup */
    free(ex);
    free(ex_del);

    return;

  }
}


int is_excluded(const in_addr_t check_addr) {

  struct exclude_node check_ex;
  struct exclude_node *ex_search;

  check_ex.addr_start = check_addr;
  check_ex.addr_end = check_addr;
  check_ex.exclude_count = 0;

  ex_search = (struct exclude_node *)pavl_find(exclude_tree, &check_ex);

  if (ex_search == NULL) {
    return 0;
  }
  else {
    ex_search->exclude_count += 1;
    return 1;
  }

}


void *thread_flow_janitor(void * arg) {

  /* Misc vars */
  struct timeval sleep_time;
  time_t cur_time;
  int tree_num;
  struct pavl_traverser traverser;
  struct flow_summary *flow_last, *flow_cur;
  int deleted;

  while (terminate == 0) {

    /* sleep 5 sec between purges */
    sleep_time.tv_sec = 5;
    sleep_time.tv_usec = 0;
    select(0, NULL, NULL, NULL, &sleep_time);

    /* fprintf(stderr, "thread still here\n"); */

    deleted = 0;
    cur_time = time(NULL);
    for (tree_num = 0; tree_num < TREES; tree_num++) {

      /* === *** ACQUIRE TREE LOCK *** === */
      pthread_mutex_lock(&(flow_hash_trees[tree_num].tree_mutex));      

      pavl_t_init(&traverser, flow_hash_trees[tree_num].tree);

      flow_last = (struct flow_summary *)pavl_t_next(&traverser);
      flow_cur = (struct flow_summary *)pavl_t_next(&traverser);

      while (flow_last != NULL) {
      
	if ((cur_time - flow_last->time_updated > MIN_FLOW_AGE) ||
	    (cur_time - flow_last->time_added > MAX_FLOW_AGE)) {

	  /* Do the deletion */
	  flow_last =
	    (struct flow_summary *)pavl_delete(flow_hash_trees[tree_num].tree,
					       flow_last);

	  /* ===
	   * *** THIS FLOW NEEDS TO BE OUTPUTTED SOMEWHERE
	   * OR IT WILL BE LOST FOREVER! ***
	   * (outputting comes later)
	   * ===
	   */
	  print_flow_json(flow_last);

	  /* Free the flow sources list */
	  free_source_list(flow_last->sources);
	  
	  /* Now free the flow */
	  free(flow_last);

	  deleted++;
	}	  

	/* Move on */
	flow_last = flow_cur;
	flow_cur = (struct flow_summary *)pavl_t_next(&traverser);
      }

      /* === *** RELEASE TREE LOCK *** === */
      pthread_mutex_unlock(&(flow_hash_trees[tree_num].tree_mutex));

    } /* END for tree_num */

    /* === *** ACQUIRE STATS LOCK *** === */
    pthread_mutex_lock(&stat_current_mutex);
    
    /* Update current stats */
    stat_current_flows -= deleted;

    /* === *** UNLOCK STATS LOCK *** === */
    pthread_mutex_unlock(&stat_current_mutex);
    

  } /* END while terminate */


  return NULL;
}


void free_source_list(struct flow_source_summary *f_source) {

  struct flow_source_summary *cur_f_source;

  /* Free the list and the data */
  while (f_source != NULL) {
    cur_f_source = f_source; /* work on the current flow source node */
    f_source = f_source->next; /* grab the next one */

    /* Get rid of the struct */
    free(cur_f_source);
  }

  return;
}


void print_flow_json(const struct flow_summary *flow) {

  /* === Misc vars === */
  struct flow_source_summary *flow_source;
  struct in_addr temp_inaddr_src, temp_inaddr_dst, temp_inaddr_flow;
  char outbuff[SENDBUFFSIZE + 1];
  int outindex;
  struct sockaddr_in send_addrin;

  temp_inaddr_src.s_addr = htonl(flow->src_addr.s_addr);
  temp_inaddr_dst.s_addr = htonl(flow->dst_addr.s_addr);


  /* This is some ugly-ass code.  I can't think of a way to do it securely
   * and cleanly though ...
   */

  outindex = 0;

  outindex +=
    snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	     "{\n");
  outindex +=
    snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	     "\t\"src_addr\": \"%s\",\n", inet_ntoa(temp_inaddr_src));
  outindex +=
    snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	     "\t\"dst_addr\": \"%s\",\n", inet_ntoa(temp_inaddr_dst));
  outindex +=
    snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	     "\t\"protocol\": %d,\n", flow->protocol);
  outindex +=
    snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	     "\t\"src_port\": %d,\n", flow->src_port);
  outindex +=
    snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	     "\t\"dst_port\": %d,\n", flow->dst_port);
  outindex +=
    snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	     "\t\"tcp_flags\": %d,\n", flow->tcp_flags);
  outindex +=
    snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	     "\t\"start_time\": %d,\n", (int)(flow->start_time));
  outindex +=
    snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	     "\t\"end_time\": %d,\n", (int)(flow->end_time));
  outindex +=
    snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	     "\t\"source_count\": %d,\n", flow->source_count);
  outindex +=
    snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	     "\t\"source_stats\": [\n");

  flow_source = flow->sources;
  while (flow_source != NULL) {

    temp_inaddr_flow.s_addr = htonl(flow_source->flow_src);

    outindex +=
      snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	       "\t\t{\n");
    outindex +=
      snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	       "\t\t\"flow_source\": \"%s\",\n", inet_ntoa(temp_inaddr_flow));
    outindex +=
      snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	       "\t\t\"src_int\": %d,\n", flow_source->src_int);
    outindex +=
      snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	       "\t\t\"dst_int\": %d,\n", flow_source->dst_int);
    outindex +=
      snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	       "\t\t\"num_packets\": %lu,\n", flow_source->num_packets);
    outindex +=
      snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	       "\t\t\"num_bytes\": %lu,\n", flow_source->num_bytes);
    outindex +=
      snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	       "\t\t\"num_flows\": %lu\n", flow_source->num_flows);

    flow_source = flow_source->next;

    if (flow_source == NULL) {
      outindex +=
	snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
		 "\t\t}\n");
    }
    else {
      outindex +=
	snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
		 "\t\t},\n");
    }
  }
  outindex +=
    snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	     "\t]\n");
  outindex +=
    snprintf(outbuff + outindex, SENDBUFFSIZE - outindex - 1,
	     "}\n");
  
  /* Terminate the string */
  outbuff[outindex] = '\0';


  /*fprintf(stdout, "%s", outbuff);*/

  send_addrin.sin_family = AF_INET;
  send_addrin.sin_port = htons(SENDPORT);
  inet_aton(SENDDST, &(send_addrin.sin_addr));

  sendto(send_fh, outbuff, outindex, 0,
	 (const struct sockaddr *)&send_addrin, sizeof(send_addrin));
   
  

}
