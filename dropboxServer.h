#ifndef __DROPBOX_SERVER_H__
#define __DROPBOX_SERVER_H__
#include <string>
#include <openssl/ssl.h>
#include <openssl/err.h>


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

#endif
