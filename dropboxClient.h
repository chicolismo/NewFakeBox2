#ifndef __DROPBOX_CLIENT_H__
#define __DROPBOX_CLIENT_H__

#include <string>
#include <vector>
#include <openssl/ossl_typ.h>
#include "dropboxUtil.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

#define CONNECTION_SUCCESS = 0
#define CONNECTION_ERROR = (-1)


void print_interface();
void run_interface();
void run_sync_thread();
void run_get_sync_dir_thread();
void create_sync_dir();
void list_local_files();
void list_server_files();
std::vector<FileInfo> get_server_files();
ConnectionResult connect_server(std::string host, uint16_t port, SSL_CTX *context);
void sync_client();
void send_file(std::string filename);
void get_file(std::string filename);
void delete_file(std::string filename);
void send_delete_command(std::string filename);
void close_connection();
void get_file(std::string filename, bool current_path);
void run_check_server_status_thread();
void check_server();
void sigpipe_handler();

void hold_file(std::string filename);
void release_file(std::string filename);

#endif
