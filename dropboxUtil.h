#ifndef __DROPBOX_UTIL_H__
#define __DROPBOX_UTIL_H__

#define MAX_DEVICES 2
#define MAX_NAME_SIZE 256
#define BUFFER_SIZE 1024
#define EMPTY_DEVICE (-1)

#include <string>
#include <map>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <openssl/ossl_typ.h>

enum ConnectionType { Normal, Sync, ServerConnection };

// Possíveis resultados da tentativa de conexão
enum ConnectionResult { Success, Error, SSLError, CantAccept };

enum Command { Upload, Download, Delete, ListServer, Exit, IsAlive };

struct FileInfo {
    char filename_[MAX_NAME_SIZE];
    char extension_[MAX_NAME_SIZE];
    time_t last_modified_;
    size_t bytes_;

    explicit FileInfo();

    void set_filename(const char *filename);
    void set_filename(const std::string &filename);
    std::string filename() const;

    void set_extension(const char *extension);
    void set_extension(const std::string &extension);
    std::string extension() const;

    void set_last_modified(time_t time);
    time_t last_modified() const;

    void set_bytes(size_t bytes);
    size_t bytes() const;

    std::string string() const;
};


struct Client {
    std::string user_id;
    bool is_logged;
    long connected_devices[MAX_DEVICES];
    int sockets[MAX_DEVICES];
    std::vector<FileInfo> files;

    //Semaphore sem;

    std::mutex user_mutex;

    // Methods
    explicit Client(std::string user_id);
    explicit Client(const char *user_id);
};


typedef std::map<std::string, Client *> ClientDict;

bool read_socket(int socket_fd, void *buffer, size_t count);
bool write_socket(int socket_fd, const void *buffer, size_t count);

void send_string(int socket_fd, const std::string &input);
std::string receive_string(int socket_fd);

void send_bool(int socket_fd, bool value);
bool read_bool(int socket_fd);

bool send_file(int to_socket_fd, FILE *in_file, size_t file_size);
bool read_file(int from_socket_fd, FILE *out_file, size_t file_size);

// Versões SSL das mesmas funções
bool read_socket(SSL *ssl, void *buffer, size_t count);
bool write_socket(SSL *ssl, const void *buffer, size_t count);
void send_string(SSL *ssl, const std::string &input);
std::string receive_string(SSL *ssl);
void send_bool(SSL *ssl, bool value);
bool read_bool(SSL *ssl);
bool send_file(SSL *to_ssl, FILE *in_file, size_t file_size);
bool read_file(SSL *from_ssl, FILE *out_file, size_t file_size);

void show_certificate(SSL *ssl);

#endif
