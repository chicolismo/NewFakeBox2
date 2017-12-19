#ifndef __FRONT_END_H__
#define __FRONT_END_H__

#include <string>
#include <vector>
#include <openssl/ossl_typ.h>
#include "dropboxClient.h"

// Cada servidor possui um hostname, a porta onde se encontra
// e também um socket, que será usado pelo cliente para
// se comunicar com ele
struct Server {
    std::string hostname;
    uint16_t port;
    int socket_fd;
    SSL *ssl;
    std::string to_string() const;
};

std::vector<Server> read_servers();
void print_servers(const std::vector<Server> &servers);

ConnectionResult connect_servers(const std::string &user_id, int *socket_fd, SSL **ssl);
ConnectionResult connect_server(Server &server, const std::string &user_id, SSL_CTX *context);

#endif //__FRONT_END_H__
