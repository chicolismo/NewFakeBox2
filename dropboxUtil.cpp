#include "dropboxUtil.h"

#include <utility>
#include <memory.h>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <iostream>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <memory.h>
#include <sstream>

//=============================================================================
// Client
//=============================================================================
Client::Client(std::string user_id) {
    this->user_id = std::move(user_id);
    this->is_logged = false;
    for (int &connected_device : connected_devices) {
        connected_device = EMPTY_DEVICE;
    }
}

Client::Client(const char *user_id) : Client(std::string(user_id)) {}

//=============================================================================
// FileInfo
//=============================================================================
FileInfo::FileInfo() {
    bzero(filename_, MAX_NAME_SIZE);
    bzero(extension_, MAX_NAME_SIZE);
    last_modified_ = 0;
    bytes_ = 0;
}

void FileInfo::set_filename(const char *filename) {
    std::strncpy(filename_, filename, MAX_NAME_SIZE);
}

void FileInfo::set_filename(const std::string &filename) {
    std::strcpy(filename_, filename.c_str());
}

void FileInfo::set_extension(const char *extension) {
    std::strncpy(extension_, extension, MAX_NAME_SIZE);
}

std::string FileInfo::filename() const {
    return std::string(filename_);
}

void FileInfo::set_extension(const std::string &extension) {
    std::strcpy(extension_, extension.c_str());
}

std::string FileInfo::extension() const {
    return std::string(extension_);
}

void FileInfo::set_last_modified(time_t time) {
    last_modified_ = time;
}

time_t FileInfo::last_modified() const {
    return last_modified_;
}

void FileInfo::set_bytes(size_t bytes) {
    bytes_ = bytes;
}

size_t FileInfo::bytes() const {
    return bytes_;
}

std::string FileInfo::string() const {
    char date_buffer[20];
	
    time_t date = last_modified();
    strftime(date_buffer, 20, "%Y-%m-%d %H:%M:%S", localtime(&date));
    
    std::stringstream result;

    result << "Nome: " << filename() << "\n"
           << "Tamanho: " << bytes() << " bytes\n"
           << "Modificado: " << date_buffer << "\n";

    return result.str();
}

// Abstração da leitura do socket
bool read_socket(int socket_fd, void *buffer, size_t count) {
    auto *ptr = (char *) buffer;

    ssize_t bytes_read = 0;
    while (bytes_read < count) {
        ssize_t bytes = recv(socket_fd, ptr, count - bytes_read, 0);
        if (bytes < 1) {
            return false;
        }
        ptr += bytes;
        bytes_read += bytes;
    }
    return true;
}


// Abstração de escrita no socket
bool write_socket(int socket_fd, const void *buffer, size_t count) {
    auto *ptr = (char *) buffer;

    ssize_t bytes_written = 0;
    while (bytes_written < count) {
        ssize_t bytes = send(socket_fd, ptr, count - bytes_written, 0);
        if (bytes < 0) {
            return false;
        }
        ptr += bytes;
        bytes_written += bytes;
    }
    return true;
}


void send_string(int socket_fd, const std::string &input) {
    size_t size = input.length() + 1;
    char buffer[size];
    bzero((void *) buffer, size);
    std::strcpy(buffer, input.c_str());

    ssize_t bytes;

    // Envia o tamanho
    if (write_socket(socket_fd, (const void *) &size, sizeof(size))) {

        // Envia os bytes
        if (!write_socket(socket_fd, (const void *) buffer, size)) {
            std::cerr << "Erro ao tentar enviar a string " << input << "\n";
        }
    }
}


std::string receive_string(int socket_fd) {
    ssize_t bytes;

    // Lê o tamanho
    size_t size;
    bool ok = read_socket(socket_fd, (void *) &size, sizeof(size));

    if (ok) {
        // Lê os bytes
        char buffer[size];
        if (!read_socket(socket_fd, (void *) buffer, size)) {
            std::cout << "Erro ao receber a string\n";
        }
        return std::string(buffer);
    }
    else {

        std::cerr << "Erro ao receber o tamanho da string\n";
        return "";
    }
}

bool read_bool(int socket_fd) {
    bool value;
    read_socket(socket_fd, (void *) &value, sizeof(value));
    return value;
}

void send_bool(int socket_fd, bool value) {
    write_socket(socket_fd, (const void *) &value, sizeof(value));
}

bool send_file(int to_socket_fd, FILE *in_file, size_t file_size) {
    char buffer[BUFFER_SIZE];
    bzero(buffer, BUFFER_SIZE);

    size_t bytes_read_from_file;
    while ((bytes_read_from_file = fread(buffer, sizeof(char), BUFFER_SIZE, in_file)) > 0) {

        ssize_t bytes_sent = 0;
        while (bytes_sent < bytes_read_from_file) {
            if ((bytes_sent += send(to_socket_fd, buffer + bytes_sent, bytes_read_from_file, 0)) < 0) {
                fprintf(stderr, "Erro ao enviar o arquivo. Errno = %d\n", errno);
                return false;
            }
        }
        bzero(buffer, BUFFER_SIZE);
    }
    bool ok = read_bool(to_socket_fd);
    
    if (ok) {
        std::cout << "Arquivo enviado!\n";
    } else {
        std::cerr << "Deu merda\n";
    }

    return true;
}

bool read_file(int from_socket_fd, FILE *out_file, size_t file_size) {
    //std::cout << "Tamanho do arquivo: " << file_size << "\n";

    char buffer[BUFFER_SIZE];
    bzero(buffer, BUFFER_SIZE);

    size_t bytes_received = 0;

    while (bytes_received < file_size) {
        //std::cout << "Outer loop: " << bytes_received << "\n";

        ssize_t bytes_read_from_socket = 0;
        while ((bytes_read_from_socket = recv(from_socket_fd, buffer, BUFFER_SIZE, 0)) > 0) {
            
            // TODO: Temos que testar isto.
            ssize_t bytes_written_to_file = fwrite(buffer, sizeof(char), bytes_read_from_socket, out_file);

            if (bytes_written_to_file < bytes_read_from_socket) {
                std::cerr << "Erro na escrita do arquivo.\n";
            }

            bzero(buffer, BUFFER_SIZE);
            bytes_received += bytes_read_from_socket;

            //std::cout << "Inner loop: Bytes recebidos: " << bytes_received << "\n";

            if (bytes_received == file_size) {
                break;
            }
        }

        if (bytes_read_from_socket < 0) {
            if (errno == EAGAIN) {
                printf("recv() timed out.\n");
            }
            else {
                fprintf(stderr, "recv() failed due to errno = %d\n", errno);
            }
            return false;
        }
    }
    
    send_bool(from_socket_fd, true);

    std::cout << "Arquivo recebido!\n";
    return true;
}
