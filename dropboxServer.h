#ifndef __DROPBOX_SERVER_H__
#define __DROPBOX_SERVER_H__
#include <string>

void initialize_clients();
void create_user_dir(std::string user_id);
void update_files(std::string user_id, std::string filename, size_t file_size, time_t timestamp);
bool connect_client(std::string user_id, int client_socket_fd);
void disconnect_client(std::string user_id, int client_socket_fd);
void sync_server(std::string user_id, int client_socket_fd);
void receive_file(std::string user_id, std::string filename, int client_socket_fd);
void send_file(std::string user_id, std::string filename, int client_socket_fd);
void delete_file(std::string user_id, std::string filename, int client_socket_fd);
void run_normal_thread(int client_socket_fd);
void run_user_interface(const std::string user_id, int client_socket_fd);
void send_file_infos(std::string user_id, int client_socket_fd);
void lock_user(std::string user_id);
void unlock_user(std::string user_id);

#endif
