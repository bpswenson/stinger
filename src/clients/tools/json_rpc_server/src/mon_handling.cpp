extern "C" {
  #include "stinger_core/stinger.h"
  #include "stinger_core/stinger_shared.h"
  #include "stinger_utils/stinger_utils.h"
  #include "stinger_core/xmalloc.h"
}

#include "stinger_net/proto/stinger-batch.pb.h"
#include "stinger_net/proto/stinger-connect.pb.h"
#include "stinger_net/stinger_alg_state.h"

#include "stinger_net/send_rcv.h"
#include "stinger_net/stinger_server_state.h"

#include "rpc_state.h"

#if !defined(MTA)
#define MTA(x)
#endif

#include <netdb.h>
#include <cstdio>
#include <limits>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <unistd.h>

/* POSIX only for now, note that mongoose would be a good place to 
get cross-platform threading and sockets code */
#include <pthread.h> 

#define LOG_AT_W
#include "stinger_core/stinger_error.h"

using namespace gt::stinger;

typedef struct {
  int sock;
  const char * name;
} mon_handler_params_t;

bool
map_update(ServerToMon & server_to_mon, stinger_t ** stinger_copy,
  std::vector<StingerAlgState *> & algs, 
  std::map<std::string, StingerAlgState *> & alg_map)
{

  if (!(*stinger_copy)) {
    LOG_D_A("Mapping stinger %s %ld", server_to_mon.stinger_loc().c_str(), server_to_mon.stinger_size());
    *stinger_copy = stinger_shared_private(server_to_mon.stinger_loc().c_str(), server_to_mon.stinger_size());
  }

  if (!(*stinger_copy)) {
    LOG_E("Failed to map STINGER");
    return false;
  }

  LOG_D("Mapping all algs");
  for(int64_t d = 0; d < server_to_mon.dep_name_size(); d++) {
    StingerAlgState * alg_state = new StingerAlgState();
    
    alg_state->data = shmmap(
      server_to_mon.dep_data_loc(d).c_str(), O_RDONLY, S_IRUSR, PROT_READ, 
      server_to_mon.dep_data_per_vertex(d) * ((*stinger_copy)->max_nv), MAP_PRIVATE);

    if(!alg_state->data) {
      LOG_E_A("Failed to map data for %s, but continuing", server_to_mon.dep_name(d).c_str());
      delete alg_state;
      continue;
    }

    alg_state->name = server_to_mon.dep_name(d);
    alg_state->data_loc = server_to_mon.dep_data_loc(d);
    alg_state->data_description = server_to_mon.dep_description(d);
    alg_state->data_per_vertex = server_to_mon.dep_data_per_vertex(d);

    algs.push_back(alg_state);
    alg_map[server_to_mon.dep_name(d)] = alg_state;
  }

  if(*stinger_copy)
    return true;
  else
    return false;
}

void *
mon_handler(void * args)
{
  mon_handler_params_t * params = (mon_handler_params_t *)args;
  JSON_RPCServerState & server_state = JSON_RPCServerState::get_server_state();

  Connect connect;
  connect.set_type(CLIENT_MONITOR);
  if(!send_message(params->sock, connect)) {
    LOG_E_A("Error sending message to the server on %ld with message:\n%s", (long) params->sock, connect.DebugString().c_str());
    free(args);
    return NULL;
  }

  MonToServer mon_to_server;
  mon_to_server.set_mon_name(params->name);
  mon_to_server.set_action(REGISTER_MON);
  if(!send_message(params->sock, mon_to_server)) {
    LOG_E("Error sending message to the server");
    free(args);
    return NULL;
  }

  ServerToMon server_to_mon;
  if(recv_message(params->sock, server_to_mon)) {
    if(server_to_mon.result() != MON_SUCCESS) {
      LOG_E("Error registering with the server");
      free(args);
      return NULL;
    } else {
      stinger_t * new_stinger = NULL;
      std::vector<StingerAlgState *> * algs = new std::vector<StingerAlgState *>();
      std::map<std::string, StingerAlgState *> * alg_map = new std::map<std::string, StingerAlgState *>();
      map_update(server_to_mon, &new_stinger, *algs, *alg_map);
      server_state.update_algs(new_stinger, server_to_mon.stinger_loc(), server_to_mon.stinger_size(), algs, alg_map, server_to_mon.batch());
      while(1) {
	LOG_V_A("%s : beginning update cycle", params->name);
	mon_to_server.set_action(BEGIN_UPDATE);
	if(!send_message(params->sock, mon_to_server)) {
	  LOG_E("Error sending message to the server");
	  exit(-1);
	} else if((!recv_message(params->sock, server_to_mon)) || server_to_mon.result() != MON_SUCCESS) {
	  LOG_E_A("Error updating - communication to server failed: %s", server_to_mon.DebugString().c_str());
	} else {
	  // stinger_t * new_stinger;
	  std::vector<StingerAlgState *> * algs = new std::vector<StingerAlgState *>();
	  std::map<std::string, StingerAlgState *> * alg_map = new std::map<std::string, StingerAlgState *>();
	  map_update(server_to_mon, &new_stinger, *algs, *alg_map);
	  server_state.sync();
	  mon_to_server.set_action(END_UPDATE);
	  if(!send_message(params->sock, mon_to_server)) {
	    LOG_E("Error sending message to the server");
	    exit(-1);
	  }
	  server_state.update_algs(new_stinger, server_to_mon.stinger_loc(), server_to_mon.stinger_size(), algs, alg_map, server_to_mon.batch());
	  if(!recv_message(params->sock, server_to_mon) || server_to_mon.result() != MON_SUCCESS) {
	    LOG_E("Error updating - communication to server failed");
	  }
	}
      }
    }
  } else {
    LOG_E("Error receiving message from the server");
    free(args);
    return NULL;
  }
  free(args);
}

int64_t
mon_connect(int port, const char * host, const char * name)
{
#ifdef STINGER_USE_TCP
  struct sockaddr_in sock_addr;
  memset(&sock_addr, 0, sizeof(struct sockaddr_in));
  struct hostent * hp = gethostbyname(host);

  if(!hp) {
    LOG_E_A("Error resolving host %s", host);
    return -1;
  }

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if(-1 == sock) {
    LOG_E("Error opening socket");
    return -1;
  }

  memcpy(&sock_addr.sin_addr.s_addr, hp->h_addr, hp->h_length);
  sock_addr.sin_family = AF_INET;

  if(port) {
    sock_addr.sin_port = htons(port);
  } else {
    sock_addr.sin_port = htons(10103);
  }

  LOG_D_A("Socket open, connecting to host %s %s", host, inet_ntoa(**(struct in_addr **)hp->h_addr_list));
#else
  struct sockaddr_un sock_addr;
  memset(&sock_addr, 0, sizeof(sock_addr));
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if(-1 == sock) {
    LOG_E("Error opening socket");
    return NULL;
  }
  strncpy(sock_addr.sun_path, "/tmp/stinger.sock", sizeof(sock_addr.sun_path)-1);
  sock_addr.sun_family = AF_UNIX;

  LOG_D_A("Socket open, connecting to host %s", host);
#endif

  if(-1 == connect(sock, (sockaddr *)&sock_addr, sizeof(sock_addr))) {
    LOG_E("Error connecting socket");
    return -1;
  }

  pthread_t mon_handler_thread;
  mon_handler_params_t * mon_params = (mon_handler_params_t *)xcalloc(1, sizeof(mon_handler_params_t));
  mon_params->sock = sock;
  mon_params->name = name;
  pthread_create(&mon_handler_thread, NULL, mon_handler, mon_params);

  return 0;
}
