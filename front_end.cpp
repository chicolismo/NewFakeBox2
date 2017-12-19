#include "front_end.h"
#include "dropboxUtil.h"
#include <iostream>
#include <fstream>
#include <netinet/in.h>
#include <netdb.h>
#include <sstream>

std::string Server::to_string() const {
    std::stringstream stream;
    stream << hostname << ":" << port;
    return stream.str();
}

std::vector<Server> read_servers() {
    std::vector<Server> servers;

    // Arquivo de leitura, para obter todas as réplicas dos servidores
    std::ifstream servers_file;

    servers_file.open("servers.txt");
    while (!servers_file.eof()) {
        Server server;
        servers_file >> server.hostname;
        servers_file >> server.port;
        servers.push_back(server);
    }

    servers_file.close();

    return std::move(servers);
}


void print_servers(const std::vector<Server> &servers) {
    for (auto &server : servers) {
        std::cout << "Hostname: " << server.hostname << "\n";
        std::cout << "Port Number: " << server.port << "\n";
        std::cout << "Socket: " << server.socket_fd << "\n";
        std::cout << "SSL: " << server.ssl << "\n";
    }
}


/*
 * ----------------------------------------------------------------------------
 * connect_servers
 * ----------------------------------------------------------------------------
 * Tenta se conectar a algum dos servidores. Apenas o que for o principal
 * poderá aceitar a conexão.  A cada tentativa de conexão, a lista de
 * servidores será percorrida e o servidor que estiver ativo e for o
 * principal será usado para a conexão.
 * ----------------------------------------------------------------------------
 */
ConnectionResult connect_servers(const std::string &user_id, int *socket_fd, SSL **ssl) {
    bool success = false;

    auto servers = read_servers();

    for (auto &server : servers) {
        const SSL_METHOD *method = SSLv23_client_method();
        SSL_CTX *ctx = SSL_CTX_new(method);

        // O contexto deve ser criado com sucesso!
        if (ctx == nullptr) {
            ERR_print_errors_fp(stderr);
            abort();
        }

        ConnectionResult result = connect_server(server, user_id, ctx);

        if (result == ConnectionResult::Error) {
            SSL_CTX_free(ctx);
            //std::cerr << "Erro ao se conectar com um servidor\n";
        }
        else if (result == ConnectionResult::SSLError) {
            SSL_CTX_free(ctx);
            //std::cerr << "Erro ao se conectar com um servidor via SSL\n";
        }
        else if (result == ConnectionResult::CantAccept) {
            // Tentou se conectar com um servidor backup. Não pode concluir a conexão.
            //std::cerr << "Servidor é um backup e não pode concluir a conexão\n";
        }
        else {
            *socket_fd = server.socket_fd;
            *ssl = server.ssl;
            success = true;
            break;
        }
    }

    return success ? ConnectionResult::Success : ConnectionResult::Error;
}


ConnectionResult connect_server(Server &server, const std::string &user_id, SSL_CTX *context) {
    sockaddr_in server_address{};

    hostent *host_server = gethostbyname(server.hostname.c_str());

    if (host_server == nullptr) {
        std::cerr << "Erro ao obter o servidor " << server.to_string() << "\n";
        return ConnectionResult::Error;
    }

    server.socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server.socket_fd == -1) {
        std::cerr << "Erro ao criar o socket do cliente com o servidor " << server.to_string() << "\n";
        return ConnectionResult::Error;
    }

    bzero((void *) &server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(server.port);
    server_address.sin_addr = *(in_addr *) host_server->h_addr_list[0];

    // std::cout << "Tentando se conectar com o servidor " << server.to_string() << " -> user_id(" << user_id << ")\n";
    if (connect(server.socket_fd, (sockaddr *) &server_address, sizeof(server_address)) < 0) {
        std::cerr << "Erro ao conectar com o servidor " << server.to_string() << "\n";
        return ConnectionResult::Error;
    }

    // Anexando ssl (global) ao socket
    server.ssl = SSL_new(context);

    SSL_set_fd(server.ssl, server.socket_fd);
    if (SSL_connect(server.ssl) == -1) {
        std::cerr << "Erro ao conectar com ssl\n";
        ERR_print_errors_fp(stderr);
        return ConnectionResult::SSLError;
    }

    // Envia o tipo de conexão ao servidor
    ConnectionType type = ConnectionType::Normal;
    ssize_t bytes = write_socket(server.ssl, (const void *) &type, sizeof(type));
    if (bytes == -1) {
        std::cerr << "Erro enviando o tipo de conexão ao servidor";
        return ConnectionResult::Error;
    }

    send_string(server.ssl, user_id);

    // Recebe o sinal de ok do servidor
    bool ok = false;
    read_socket(server.ssl, (void *) &ok, sizeof(ok));

    if (!ok) {
        return ConnectionResult::Error;
    }

    return ConnectionResult::Success;
}

