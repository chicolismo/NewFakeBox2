#ifndef __DROPBOX_SERVER_H__
#define __DROPBOX_SERVER_H__
#include <string>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <vector>
#include "dropboxUtil.h"
#include <thread>

enum ServerCommand { Heartbeat };

class Replica {
public:
    std::string hostname;
    uint16_t port;
    SSL *ssl;
    int socket_fd;
    bool active;
    bool master;
    int priority;
    //std::mutex server_mutex;
    std::mutex server_mutex;
    time_t last_heartbeat;

    //Replica();
    //// Copy constructor.
    //Replica(const Replica &replica);

};


void initialize_clients();
void create_user_dir(std::string user_id);
void update_files(std::string user_id, std::string filename, size_t file_size, time_t timestamp);
bool connect_client(const std::string &user_id, SSL *client_ssl);
void disconnect_client(const std::string &user_id, SSL *client_ssl, int client_socket_fd);
void sync_server(std::string user_id, SSL *client_ssl);
void receive_file(std::string user_id, std::string filename, SSL *client_ssl);
void send_file(std::string user_id, std::string filename, SSL *client_ssl);
void delete_file(std::string user_id, std::string filename, SSL *client_ssl);
void run_normal_thread(SSL *client_ssl, int client_socket_fd);
void run_user_interface(std::string user_id, SSL *client_ssl, int client_socket_fd);
void send_file_infos(std::string user_id, SSL *client_ssl);
void lock_user(std::string user_id);
void unlock_user(std::string user_id);
void run_connect_to_other_servers_thread();
void run_server_thread(SSL *other_server_ssl, int other_server_socket_fd, std::string hostname, uint16_t port);
ConnectionResult connect_server(Replica *replica, SSL_CTX *context);

FileInfo *get_file_info(std::string user_id, std::string filename);

void updateReplicas();

std::vector<Replica*> read_replicas();

void server_sigpipe_handler();

void hold_file_for_client(std::string user_id, SSL *client_ssl, int client_socket_fd, std::string filename);

void release_file_for_client(std::string user_id, SSL *client_ssl, int client_socket_fd, std::string filename);

#endif
