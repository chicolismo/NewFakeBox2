#include <sys/types.h>
#include <sys/socket.h>
#include <iostream>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <memory.h>
#include <thread>
#include "dropboxServer.h"
#include "dropboxUtil.h"
#include "dropboxClient.h"
#include <boost/filesystem.hpp>
#include <chrono>
#include <fstream>
#include <netdb.h>
#include <csignal>

/*
 *
 * TODO: Quando o cliente excluir um arquivo, a sua estrutura correspondente
 * deve indicar que o arquivo está deletado.
 *
 * TODO: Para determinar se precisamos do arquivo, também devemos comparar seu
 * tamanho, já que o inotify pode enviar o arquivo recém colado, antes de
 * terminarmos de copiar seu conteúdo para o diretório de destino.
 *
 */

namespace fs = boost::filesystem;

// Globais
std::string server_ip;
fs::path server_dir;
uint16_t port_number;
int server_priority;
bool primary = false;

sockaddr_in address{};
ClientDict clients;

std::vector<Replica*> replicas;


std::mutex connection_mutex;
std::mutex user_lock_mutex;

/*
 * -----------------------------------------------------------------------------
 * main
 * -----------------------------------------------------------------------------
 * A função main espera 1 argumento que é em qual porta o servidor vai rodar
 *
 * Ela também fica escutando novas conexões ao seu socket e para cada nova
 * conexão cria uma thread nova.
 *
 * Essa thread se encarrega de escutar e responder a comandos do cliente.  O
 * socket do cliente é passado a essa thread.  Outras conexões ficam sendo
 * aguardadas.
 * -----------------------------------------------------------------------------
 */
int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Informe o ip e a porta\n";
        std::exit(1);
    }

    // Salva o ip do servidor na global
    server_ip = std::string(argv[1]);

    char *end;
    port_number = static_cast<uint16_t >(std::strtol(argv[2], &end, 10));

    // Instalando sigpipe handler
    signal(SIGPIPE, reinterpret_cast<__sighandler_t>(server_sigpipe_handler));

    replicas = read_replicas();
    /*
    for (auto &replica : replicas) {
        std::cout << "Hostname: " << replica.hostname << "\n";
        std::cout << "Port number: " << replica.port << "\n";
    }
    */

    bzero((void *) &address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port_number);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    // Inicializando engine SSL
    OpenSSL_add_all_algorithms();
    SSL_library_init();
    SSL_load_error_strings();
    const SSL_METHOD *method = SSLv23_server_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (ctx == nullptr) {
        ERR_print_errors_fp(stderr);
        abort();
    }
    if (SSL_CTX_use_certificate_file(ctx, "CertFile.pem", SSL_FILETYPE_PEM) != 1) {
        std::cerr << "Erro ao aplicar o certificado\n";
        std::exit(1);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, "KeyFile.pem", SSL_FILETYPE_PEM) != 1) {
        std::cerr << "Erro ao aplicar a chave\n";
        std::exit(1);
    }

    // Criando o socket
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    // Binding
    if (bind(socket_fd, (sockaddr *) &address, sizeof(address)) < 0) {
        std::cerr << "Erro ao fazer o binding\n";
        close(socket_fd);
        std::exit(1);
    }

    // Listening
    listen(socket_fd, 5);

    // Determina o diretório atual
    server_dir = fs::current_path();

    // Inicializa os clientes
    initialize_clients();

    std::cout << "O servidor está aguardando conexões na porta " << port_number << "\n";

    std::thread connect_to_other_servers;
    connect_to_other_servers = std::thread(run_connect_to_other_servers_thread);

    // Aguardando conexões
    while (true) {
        int new_socket_fd;
        {
            sockaddr_in client_address{};
            socklen_t client_len = sizeof(client_address);
            new_socket_fd = accept(socket_fd, (sockaddr *) &client_address, &client_len);
        }

        if (new_socket_fd == -1) {
            std::cerr << "Erro ao aceitar o socket do cliente\n";
            continue;
        }

        // Adicionando SSL ao socket
        SSL *new_ssl= SSL_new(ctx);
        SSL_set_fd(new_ssl, new_socket_fd);
        if (SSL_accept(new_ssl) < 1) {
            std::cerr << "Erro ao aceitar o ssl.\n";
            close(socket_fd);
            SSL_clear(new_ssl);
        }

        ConnectionType type;
        bzero(&type, sizeof(type));
        ssize_t bytes = read_socket(new_ssl, (void *) &type, sizeof(type));
        if (bytes == -1) {
            std::cerr << "Erro ao ler o tipo de conexão do cliente\n";
            close(socket_fd);
            std::exit(1);
        }

        // Se o cliente está se conectando normalmente
        std::thread thread;
        if (type == Normal) {
            std::cout << "Running normal thread\n";
            thread = std::thread(run_normal_thread, new_ssl, new_socket_fd);
            thread.detach();
        }
        else if (type == ServerConnection) {
            std::cout << "Running server thread\n";
            std::string hostname = receive_string(new_ssl);
            uint16_t port;
            read_socket(new_ssl, (void *) &port, sizeof(port));

            thread = std::thread(run_server_thread, new_ssl, new_socket_fd, hostname, port);
            thread.detach();
        }
        else {
            close(new_socket_fd);
        }

    }
    close(socket_fd);
}


/*
 * -----------------------------------------------------------------------------
 *  run_normal_thread
 * -----------------------------------------------------------------------------
 *
 * Recebe um novo socket e lê o user_id da nova conexão.
 *
 * Tenta fazer a conexão e em caso de sucesso fica manda o socket para a função
 * que escuta pelos comandos do usuário
 *
 * No caso da conexão ser mal sucedida, envia a informação para o cliente e
 * encerra a thread.
 * ----------------------------------------------------------------------------
 */
void run_normal_thread(SSL *client_ssl, int client_socket_fd) {

    // Temos que ler o user_id
    std::string user_id = receive_string(client_ssl);

    std::cout << user_id << " está tentando se conectar\n";

    // Tenta conectar
    bool is_connected = false;
    if (!primary) {
        is_connected = false;
    }
    else {
        is_connected = connect_client(user_id, client_socket_fd, client_ssl);
    }
    write_socket(client_ssl, (const void *) &is_connected, sizeof(is_connected));

    // Se a conexão for bem sucedida, rodar função que espera pelos comandos
    if (is_connected) {
        std::cout << user_id << " se conectou ao servidor\n";

        run_user_interface(user_id, client_ssl, client_socket_fd);
    } else {
        SSL_shutdown(client_ssl);
        close(client_socket_fd);
    }

    // Se a conexão for mal sucedida, retorna;
    return;
}


/*
 * ----------------------------------------------------------------------------
 * initialize_clients
 * ----------------------------------------------------------------------------
 * Inicializa o dicionário de clientes.
 *
 * O diretório local onde o servidor está sendo executado é percorrido em busca
 * de subdiretórios.  Cada subdiretório é considerado como sendo um cliente, e
 * seus arquivos, os arquivos do cliente.
 *
 * A variável "clients" é uma global do tipo "std::map<std::string, Client*>",
 * ou seja, é um dicionário de chaves do tipo "strings" e valores do tipo
 * ponteiro de "Client".
 * ----------------------------------------------------------------------------
 */
void initialize_clients() {
    fs::directory_iterator end_iter;
    fs::directory_iterator dir_iter(server_dir);

    while (dir_iter != end_iter) {
        if (fs::is_directory(dir_iter->path())) {
            std::string user_id(fs::basename(dir_iter->path().string()));

            clients[user_id] = new Client(user_id);

            fs::directory_iterator client_dir_iter(dir_iter->path());

            while (client_dir_iter != end_iter) {
                if (fs::is_regular_file(client_dir_iter->path())) {
                    FileInfo file_info;
                    fs::path filepath(client_dir_iter->path());
                    file_info.set_filename(filepath.filename().string());
                    file_info.set_extension(fs::extension(filepath));
                    file_info.set_last_modified(fs::last_write_time(filepath));
                    file_info.set_bytes(fs::file_size(filepath));
                    clients[user_id]->files.push_back(file_info);
                }
                ++client_dir_iter;
            }
        }
        ++dir_iter;
    }
}


/*
 * ----------------------------------------------------------------------------
 * connect_client
 * ----------------------------------------------------------------------------
 * Conecta o novo cliente, se já não houver outros 2 dispositivos desse
 * mesmo cliente conectados.
 *
 * Essa função só permite uma thread de cada vez, pois manipula uma variável
 * global.
 *
 * Retorna um booleano indicando o sucesso da conexão.
 * ----------------------------------------------------------------------------
 */
bool connect_client(const std::string &user_id, int client_socket_fd, SSL *client_ssl) {
    // Trava a função para apenas uma thread de cada vez.
    std::lock_guard<std::mutex> lock(connection_mutex);

    bool ok = false;

    create_user_dir(user_id);
    auto it = clients.find(user_id);

    if (it == clients.end()) {
        clients[user_id] = new Client(user_id);
        clients[user_id]->is_logged = true;
        ok = true;
    }
    else {
        for (auto &device : it->second->connected_devices) {
            if (device == EMPTY_DEVICE) {
                it->second->is_logged = true;
                device = client_socket_fd;
                ok = true;
                break;
            }
        }
    }
    return ok;
}


/*
 * ----------------------------------------------------------------------------
 * disconnect_client
 * ----------------------------------------------------------------------------
 * Encerra a conexão do cliente e fecha seu socket.
 *
 * Essa função só permite uma thread de cada vez, pois manipula uma variável
 * global.
 * ----------------------------------------------------------------------------
 */
void disconnect_client(const std::string &user_id, SSL *client_ssl, int client_socket_fd) {
    std::lock_guard<std::mutex> lock(connection_mutex);

    auto it = clients.find(user_id);

    if (it == clients.end()) {
        std::cerr << "Usuário " << user_id << " não encontrado para desconectar\n";
        return;
    }
    else {
        std::cout << "Desconectando " << user_id << "\n";

        if (it->second->connected_devices[0] == EMPTY_DEVICE) {
            it->second->connected_devices[1] = EMPTY_DEVICE;
            it->second->is_logged = false;
        }
        else if (it->second->connected_devices[1] == EMPTY_DEVICE) {
            it->second->connected_devices[0] = EMPTY_DEVICE;
            it->second->is_logged = false;
        }
        else if (it->second->connected_devices[0] == client_socket_fd) {
            it->second->connected_devices[0] = EMPTY_DEVICE;
        }
        else {
            it->second->connected_devices[1] = EMPTY_DEVICE;
        }
        SSL_shutdown(client_ssl);
        close(client_socket_fd);
    }
}


/*
 * -----------------------------------------------------------------------------
 * create_user_dir
 * -----------------------------------------------------------------------------
 * Cria o diretório do usuário no servidor, caso não exista.
 * -----------------------------------------------------------------------------
 */
void create_user_dir(std::string user_id) {
    fs::path user_dir = server_dir / fs::path(user_id);
    if (!fs::exists(user_dir)) {
        fs::create_directory(user_dir);
    }
}


/*
 * -----------------------------------------------------------------------------
 * run_user_interface
 * -----------------------------------------------------------------------------
 * Aguarda comandos das threads dos usuários
 *
 * Essa função fica aguardando a thread do usuário escrever um comando no
 * socket.
 *
 * Uma vez que essa thread envia um comando, o mutex do cliente é travado, isso
 * garante que apenas um cliente do mesmo usuário consiga executar um comando
 * de cada vez.
 *
 * É importante fazer isso, porque alguns comandos fazem alterações em
 * estruturas que podem ser compartilhadas por mais de uma thread.
 *
 * Na prática, apenas uma das threads com o mesmo user_id pode entrar dentro do
 * switch por vez.
 *
 * Ao fim da execução do comando, o mutex é destravado, e outra thread (com o
 * mesmo user_id) pode executar o próximo comando.
 * -----------------------------------------------------------------------------
 */
void run_user_interface(const std::string user_id, SSL *client_ssl, int client_socket_fd) {
    Command command = Exit;

    do {
        read_socket(client_ssl, (void *) &command, sizeof(command));

        // uma vez recebido o comando, devemos travar o usuário
        //lock_user(user_id);

        std::string filename{};

        switch (command) {
            case IsAlive:
                // Apenas é necessário ler o comando.
                break;

            case Upload:
                //std::cout << "Upload Requested\n";
                filename = receive_string(client_ssl);

                //std::cout << "Arquivo a ser rebido: " << filename << "\n";
                receive_file(user_id, filename, client_ssl);
                break;

            case Download:
                //std::cout << "Download Requested\n";
                filename = receive_string(client_ssl);
                send_file(user_id, filename, client_ssl);
                break;

            case Delete:
                //std::cout << "Delete Requested\n";
                filename = receive_string(client_ssl);
                delete_file(user_id, filename, client_ssl);
                break;

            case ListServer:
                //std::cout << "ListServer Requested\n";
                send_file_infos(user_id, client_ssl);
                break;

            case Exit:
                //std::cout << "Exit Requested\n";
                disconnect_client(user_id, client_ssl, client_socket_fd);
                break;

            case Hold:
                filename = receive_string(client_ssl);
                hold_file_for_client(user_id, client_ssl, client_socket_fd, filename);
                break;

            case Release:
                filename = receive_string(client_ssl);
                release_file_for_client(user_id, client_ssl, client_socket_fd, filename);
                break;

            default:
                std::cout << "Comando não reconhecido\n";
                break;
        }

        //unlock_user(user_id);
    }
    while (command != Exit);
}


/*
 * -----------------------------------------------------------------------------
 * receive_file
 * -----------------------------------------------------------------------------
 * Recebe um arquivo do usuário.
 *
 * Uma vez que o nome do arquivo foi fornecido, essa função recebe o tamanho do
 * arquivo em bytes e a data de modificação.  Caso o arquivo não exista no
 * servidor ou seja mais recente, uma notificação para enviar o arquivo será
 * enviada ao cliente.  Depois a função tentará abrir o arquivo.  O sucesso ou
 * não da abertura do arquivo é informado ao cliente.  Em caso de sucesso na
 * hora de abrir o arquivo ele será recebido do cliente.
 * -----------------------------------------------------------------------------
 */
void receive_file(std::string user_id, std::string filename, SSL *client_ssl) {
    bool notify = read_bool(client_ssl);
    
    if (notify) {
        FileInfo *fi = get_file_info(user_id, filename);
        if (fi != nullptr && fi->holder == client_ssl) {
            send_bool(client_ssl, true);
            std::cout << "Pode receber, cliente está como o token\n";
        }
        else {
            send_bool(client_ssl, false);
            std::cout << "Não pode receber, cliente não tem o token\n";
            return;
        }
    }

    fs::path absolute_path = server_dir / fs::path(user_id) / fs::path(filename);

    //std::cout << "O caminho absoluto até o arquivo no servidor é " << absolute_path.string() << "\n";

    // Vamos ler o tamanho do arquivo!
    size_t file_size;
    read_socket(client_ssl, (void *) &file_size, sizeof(file_size));

    std::cout << "Tamanho do arquivo recebido: " << file_size << " bytes\n";

    // Recebendo a data de modificação
    time_t time;
    read_socket(client_ssl, (void *) &time, sizeof(time));

    // Se alguém tem o token do arquivo, temos que testar.
    FileInfo *file_info = get_file_info(user_id, filename);
    if (!(file_info == nullptr || file_info->holder == nullptr  || file_info->holder == client_ssl)) {
        send_bool(client_ssl, false);
        return;
    }

    // Temos que ver se o arquivo existe e se é mais antigo e se devemos recebê-lo.
    bool should_download = !(fs::exists(absolute_path) && (fs::last_write_time(absolute_path) >= time));
    send_bool(client_ssl, should_download);

    if (!should_download) {
        return;
    }

    // Vamos tentar abrir o arquivo
    FILE *file = fopen(absolute_path.c_str(), "wb");
    if (file == nullptr) {
        std::cerr << "Arquivo " << absolute_path << " não pode ser aberto\n";
        send_bool(client_ssl, false);
        return;
    }

    send_bool(client_ssl, true);
    // Vamos receber os bytes do arquivo.
    std::cout << "Preparando para receber os bytes do arquivo\n";

    // TODO: Receber o arquivo
    read_file(client_ssl, file, file_size);
    fclose(file);

    std::cout << "Arquivo " << absolute_path.string() << " recebido\n";

    // escreve a data de modificação do arquivo
    fs::last_write_time(absolute_path, time);

    // Atualiza lista de arquivos do usuário
    update_files(user_id, filename, file_size, time);

    if (primary) {
        for (auto &replica : replicas) {
            if (replica->active) {
                // Trava o mutex da replica. Apenas um comando pode ser enviado de
                // cada vez para a mesma réplica.
                std::lock_guard<std::mutex> lock(replica->server_mutex);

                send_file_server(user_id, filename, replica->ssl);
            }
        }
    }
}
// }}}


void send_file_server(std::string user_id, std::string filename, SSL *replica_ssl) {

    // Determina o caminho absoluto do arquivo no servidor
    fs::path absolute_path = server_dir / fs::path(user_id) / fs::path(filename);

    FILE *file;
    bool file_ok;

    // Se o arquivo existir, tenta abri-lo
    if ((file_ok = fs::exists(absolute_path)) == true) {
        file = fopen(absolute_path.c_str(), "rb");

        if (file == nullptr) {
            file_ok = false;
        }
    }

    if (file_ok) {
        std::cout << "Arquivo ok\n";
    }
    else {
        std::cout << "Arquivo não ok\n";
        return;
    }

    //-------------------------------------------------------------------------
    // Envia o comando e os dados do arquivo para a réplica
    //-------------------------------------------------------------------------
    ServerCommand command = ServerCommand::ServerUpload;

    // Envia o comando
    write_socket(replica_ssl, (const void *) &command, sizeof(command));
    
    // Envia o user_id
    send_string(replica_ssl, user_id);

    // Envia o nome do arquivo
    send_string(replica_ssl, filename);

    // Envia o tamanho do arquivo
    size_t file_size = fs::file_size(absolute_path);
    write_socket(replica_ssl, (const void *) &file_size, sizeof(file_size));

    // Recebe a confirmação que o cliente conseguiu criar o arquivo localmente,
    // e está esperando os bytes.
    bool ok = read_bool(replica_ssl);

    if (ok) {
        // Envia os bytes do arquivo ao cliente
        send_file(replica_ssl, file, file_size);
    }
    fclose(file);

    // Envia ao cliente a data de modificação do arquivo, para que ele possa
    // modificar sua cópia local com a data correta.
    //
    time_t timestamp = fs::last_write_time(absolute_path);
    std::cout << "Last write time a ser enviado: " << timestamp << "\n";
    // Envia a data de modificação
    write_socket(replica_ssl, (const void *) &timestamp, sizeof(timestamp));
    std::cout << "Data de criação enviada\n";

}


/*
 * -----------------------------------------------------------------------------
 * send_file
 * -----------------------------------------------------------------------------
 * Envia um arquivo para um usuário.  Se o arquivo existir e puder ser lido, a
 * função notifica o cliente.  Então ela envia o tamanho e a data de
 * modificação.  Ela lê a resposta do cliente indicando se ele conseguiu abrir
 * o arquivo para escrita localmente.  Em caso afirmativo, o arquivo é enviado
 * ao cliente.  Por fim, a função envia a data de modificação para o cliente,
 * a fim de manter o arquivo sincronizado.
 * -----------------------------------------------------------------------------
 */
void send_file(std::string user_id, std::string filename, SSL *client_ssl) {

    // Determina o caminho absoluto do arquivo no servidor
    fs::path absolute_path = server_dir / fs::path(user_id) / fs::path(filename);

    FILE *file;
    bool file_ok;

    // Se o arquivo existir, tenta abri-lo
    if ((file_ok = fs::exists(absolute_path)) == true) {
        file = fopen(absolute_path.c_str(), "rb");

        if (file == nullptr) {
            file_ok = false;
        }
    }

    // Indica ao usuário se o arquivo existe ou se foi possível abri-lo
    send_bool(client_ssl, file_ok);

    if (file_ok) {
        std::cout << "Arquivo ok\n";
    }
    else {
        std::cout << "Arquivo não ok\n";
        return;
    }

    // Caso o arquivo esteja ok, envia o tamanho do arquivo
    size_t file_size = fs::file_size(absolute_path);
    write_socket(client_ssl, (const void *) &file_size, sizeof(file_size));

    // Recebe a confirmação que o cliente conseguiu criar o arquivo localmente,
    // e está esperando os bytes.
    bool ok = read_bool(client_ssl);

    if (ok) {
        // Envia os bytes do arquivo ao cliente
        send_file(client_ssl, file, file_size);
    }
    fclose(file);

    // Envia ao cliente a data de modificação do arquivo, para que ele possa
    // modificar sua cópia local com a data correta.
    //
    time_t timestamp = fs::last_write_time(absolute_path);
    std::cout << "Last write time a ser enviado: " << timestamp << "\n";
    // Envia a data de modificação
    write_socket(client_ssl, (const void *) &timestamp, sizeof(timestamp));
    std::cout << "Data de criação enviada\n";

}


/*
 * -----------------------------------------------------------------------------
 * delete_file
 * -----------------------------------------------------------------------------
 * Exclui no servidor o arquivo cujo nome foi passado pelo usuário.  Se o
 * arquivo for excluído, o vetor de FileInfo do usuário é atualizado para
 * remover o arquivo. Se o arquivo não existir, não faz nada.
 * -----------------------------------------------------------------------------
 */
void delete_file(std::string user_id, std::string filename, SSL *client_ssl) {

    auto it = clients.find(user_id);
    if (it == clients.end()) {
        return;
    }

    // Apenas o cliente com o token pode excluir arquivos.
    FileInfo *file_info = get_file_info(user_id, filename);
    if (!(file_info == nullptr || file_info->holder == nullptr || file_info->holder == client_ssl)) {
        return;
    }

    fs::path user_dir(user_id);
    fs::path file_path(filename);
    fs::path full_path = server_dir / user_dir / file_path;

    bool deleted = fs::remove(full_path);
    if (deleted) {
        std::cout << "Arquivo " << full_path << " removido do servidor\n";

        bool found = false;
        int counter = 0;

        for (FileInfo &info : it->second->files) {
            if (info.filename() == filename) {
                //std::cout << filename << " Encontrado nos filenames\n";
                found = true;
                break;
            }
            ++counter;
        }
        if (found) {
            it->second->files.erase(it->second->files.begin() + counter);
        }

        // Envia o comando de excluir o arquivo para as réplicas.
        if (primary) {
            for (auto &replica : replicas) {
                if (replica->active) {
                    // Trava o mutex da replica. Apenas um comando pode ser enviado de
                    // cada vez para a mesma réplica.
                   
                    std::lock_guard<std::mutex> lock(replica->server_mutex);

                    ServerCommand command = ServerCommand::ServerDelete;

                    // Envia o comando para excluir
                    write_socket(replica->ssl, (const void *) &command, sizeof(command));

                    // Envia o user_id
                    send_string(replica->ssl, user_id);

                    // Envia o nome do arquivo
                    send_string(replica->ssl, filename);
                }
            }
        }

    }
    else {
        std::cout << "Arquivo " << full_path << " não existe\n";
    }

}


/*
 * ----------------------------------------------------------------------------
 * update_files
 * ----------------------------------------------------------------------------
 * Atualiza o vetor de FileInfo do client com novas informações, ou insere um
 * novo FileInfo, caso o registro ainda não exista.
 * ----------------------------------------------------------------------------
 */
void update_files(std::string user_id,
                  std::string filename,
                  size_t file_size,
                  time_t timestamp) {

    auto it = clients.find(user_id);

    // Verifica se o cliente existe no dicionário
    if (it == clients.end()) {
        std::cerr << "Erro ao atualizar os arquivos do cliente, cliente "
                  << user_id << " não encontrado.\n";
        return;
    }

    Client *client = it->second;

    // Procura o FileInfo a ser atualizado
    FileInfo *file = nullptr;
    for (FileInfo &file_info : client->files) {
        if (file_info.filename() == filename) {
            file = &file_info;
            break;
        }
    }

    if (file != nullptr) {
        // Se encontrar um registro, ele será atualizado
        file->set_bytes(file_size);
        file->set_last_modified(timestamp);
    }
    else {
        // Se não encontrar, cria um novo FileInfo e insere no vetor
        FileInfo new_file{};
        new_file.set_filename(filename);
        new_file.set_extension(fs::path(filename).extension().string());
        new_file.set_bytes(file_size);
        new_file.set_last_modified(timestamp);
        client->files.push_back(new_file);
    }
}


/*
 * ----------------------------------------------------------------------------
 * send_file_infos
 * ----------------------------------------------------------------------------
 * Envia todos os registros de FileInfo do usuário para o cliente.
 *
 * Primeiramente é enviado o tamanho do vetor, e depois cada um dos structs
 * é enviado.
 * ----------------------------------------------------------------------------
 */
void send_file_infos(std::string user_id, SSL *client_ssl) {
    auto it = clients.find(user_id);

    // Testa se o cliente foi encontrado
    if (it == clients.end()) {
        std::cerr << "Erro ao enviar a lista de file_infos, client " << user_id
                  << " não encontrado\n";
        return;
    }

    Client *client = it->second;
    size_t n = client->files.size();

    // Envia o tamanho da lista
    write_socket(client_ssl, (const void *) &n, sizeof(n));

    for (int i = 0; i < n; ++i) {
        write_socket(client_ssl, (const void *) &client->files[i], sizeof(client->files[i]));
    }
}


/*
 * ----------------------------------------------------------------------------
 * lock_user
 * ----------------------------------------------------------------------------
 * Trava todas as outras threads de um mesmo usuário.
 *
 * Como essa função altera uma variável local, ela também faz uso de um mutex,
 * para permitir apenas uma thread por vez de entrar na seção crítica.
 * ----------------------------------------------------------------------------
 */
void lock_user(std::string user_id) {
    std::lock_guard<std::mutex> lock(user_lock_mutex);

    auto it = clients.find(user_id);
    if (it != clients.end()) {
        it->second->user_mutex.lock();
    }
}


/*
 * ----------------------------------------------------------------------------
 * lock_user
 * ----------------------------------------------------------------------------
 * Destrava as threads do usuário.
 *
 * Também usa mutex para fazer exclusão mútua de todas as threads.
 * ----------------------------------------------------------------------------
 */
void unlock_user(std::string user_id) {
    std::lock_guard<std::mutex> lock(user_lock_mutex);

    auto it = clients.find(user_id);
    if (it != clients.end()) {
        it->second->user_mutex.unlock();
    }
}


ConnectionResult connect_server(Replica *replica, SSL_CTX *context) {
    sockaddr_in server_address{};

    hostent *host_server = gethostbyname(replica->hostname.c_str());

    if (host_server == nullptr) {
        //std::cerr << "Erro ao obter o servidor " << replica->hostname << ":" << replica->port << "\n";
        return ConnectionResult::Error;
    }

    replica->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (replica->socket_fd == -1) {
        //std::cerr << "Erro ao criar o socket do cliente com o servidor " << replica->hostname << ":" << replica->port << "\n";
        return ConnectionResult::Error;
    }

    bzero((void *) &server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(replica->port);
    server_address.sin_addr = *(in_addr *) host_server->h_addr_list[0];

    if (connect(replica->socket_fd, (sockaddr *) &server_address, sizeof(server_address)) < 0) {
        //std::cerr << "Erro ao conectar com o servidor " << replica->hostname << ":" << replica->port << "\n";
        return ConnectionResult::Error;
    }

    // Anexando ssl (global) ao socket
    replica->ssl = SSL_new(context);

    SSL_set_fd(replica->ssl, replica->socket_fd);
    if (SSL_connect(replica->ssl) == -1) {
        std::cerr << "Erro ao conectar com ssl\n";
        ERR_print_errors_fp(stderr);
        return ConnectionResult::SSLError;
    }

    // Envia o tipo de conexão ao servidor
    ConnectionType type = ConnectionType::ServerConnection;
    ssize_t bytes = write_socket(replica->ssl, (const void *) &type, sizeof(type));
    if (bytes == -1) {
        std::cerr << "Erro enviando o tipo de conexão ao servidor";
        return ConnectionResult::Error;
    }

    send_string(replica->ssl, server_ip);
    write_socket(replica->ssl, (const void *) &port_number, sizeof(port_number));

    return ConnectionResult::Success;
}

std::vector<Replica*> read_replicas() {
    std::vector<Replica*> replicas;

    std::ifstream replicas_file;
    replicas_file.open("servers.txt");

    int priority = 0;
    while (!replicas_file.eof()) {
        ++priority;

        Replica *replica = new Replica();

        replicas_file >> replica->hostname;
        replicas_file >> replica->port;
        replica->priority = priority;

        if (replicas_file.eof()) {
            break;
        }

        time_t now; time(&now);
        if (replica->hostname != server_ip || replica->port != port_number) {
            replica->socket_fd = -1;
            replica->ssl = nullptr;
            replica->active = false;
            replica->last_heartbeat = now;
            replicas.push_back(replica);
        } else {
            // Estamos lendo as informações do próprio servidor.
            // Atribui a prioridade global do servidor.
            server_priority = priority;
        }
    }
    replicas_file.close();

    return std::move(replicas);
}


void run_connect_to_other_servers_thread() {
    while (true) {
        //std::cout << "Sou master: " << (primary ? "Sim" : "Não") << "\n";

        int actives = 0;
        for (auto &replica : replicas) {
            if (replica->active) {
                // Temos que testar se a réplica não deu timeout
                
                time_t now; time(&now);
                time_t time_elapsed = now - replica->last_heartbeat;
                //std::cout << "Time elapsed " << time_elapsed << "\n";

                if (time_elapsed > 4) {
                    replica->active = false;
                    continue;
                }


                // Se não deu timeout, devemos enviar nosso heartbeat para ela
                // Envia o heartbeat para a réplica
                std::lock_guard<std::mutex> lock(replica->server_mutex);

                // Envia o comando
                ServerCommand command = Heartbeat;
                write_socket(replica->ssl, (const void *) &command, sizeof(command));

                // Podemos estar como nullptr dentro da lista de replicas do outro servidor.
                bool ok = read_bool(replica->ssl);

                if (ok) {
                    // Envia o timestamp
                    time_t now; time(&now);
                    write_socket(replica->ssl, (const void *) &now, sizeof(now));
                    //std::cout << "Last heartbeat: " << replica->last_heartbeat << "\n";
                }


                ++actives;
                primary = replica->priority < server_priority;

            } else {

                const SSL_METHOD *method = SSLv23_client_method();
                SSL_CTX *ctx = SSL_CTX_new(method);

                // O contexto deve ser criado com sucesso!
                if (ctx == nullptr) {
                    ERR_print_errors_fp(stderr);
                    abort();
                }

                ConnectionResult result = connect_server(replica, ctx);
                if (result == ConnectionResult::Success) {
                    replica->active = true;
                }
            }
        }

        if (actives == 0) {
            primary = true;
        }

        // Espera 2 segundos para tentar de novo
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}


Replica *get_replica(const std::string &hostname, const uint16_t &port) {
    for (auto &replica : replicas) {
        if (replica->hostname == hostname && replica->port == port) {
            return replica;
        }
    }
    return nullptr;
}

/*
 * Função que escuta comando dos outros servidores
 */
void run_server_thread(SSL *other_server_ssl, int other_server_socket_fd, std::string hostname, uint16_t port) {
    ServerCommand command;
    std::string user_id;
    std::string filename;

    while (true) {

        if (recv(other_server_socket_fd, (void *) &command, sizeof(command), MSG_PEEK | MSG_DONTWAIT) == 0) {
            // O servidor não está mais ativo...
            Replica *replica = get_replica(hostname, port);
            replica->active = false;
            return;
        }

        read_socket(other_server_ssl, (void *) &command, sizeof(command));

        //std::cout << "Recebendo comando da replica\n";

        Replica *replica = get_replica(hostname, port);
        if (replica == nullptr) {
            //std::cout << "replica é nullptr\n";
            send_bool(other_server_ssl, false);
            continue;
        }
        else {
            send_bool(other_server_ssl, true);
        }

        std::lock_guard<std::mutex> lock(replica->server_mutex);


        switch (command) {

        case Heartbeat:
            time_t heartbeat;
            read_socket(other_server_ssl, (void *) &heartbeat, sizeof(heartbeat));

            // Talvez ainda não nos conectamos com essa réplica que está mandando
            // mensagens para nós
            if (replica != nullptr) {
                replica->last_heartbeat = heartbeat;
            }
            break;

        case ServerDelete:
            std::cout << "recebendo comando de delete\n";
            user_id = receive_string(other_server_ssl);
            filename = receive_string(other_server_ssl);
            delete_file_server(user_id, filename, other_server_ssl);
            break;

        case ServerUpload:
            std::cout << "recebendo comando de upload\n";
            user_id = receive_string(other_server_ssl);
            filename = receive_string(other_server_ssl);
            receive_file_server(user_id, filename, other_server_ssl);
            break;

        default:
            break;
        }
    }
}

void delete_file_server(const std::string &user_id, const std::string &filename, SSL *other_server_ssl) {
    auto it = clients.find(user_id);
    if (it == clients.end()) {
        return;
    }

    fs::path user_dir(user_id);
    fs::path file_path(filename);
    fs::path full_path = server_dir / user_dir / file_path;

    bool deleted = fs::remove(full_path);
    if (deleted) {
        std::cout << "Arquivo " << full_path << " removido do servidor\n";

        bool found = false;
        int counter = 0;

        for (FileInfo &info : it->second->files) {
            if (info.filename() == filename) {
                //std::cout << filename << " Encontrado nos filenames\n";
                found = true;
                break;
            }
            ++counter;
        }
        if (found) {
            it->second->files.erase(it->second->files.begin() + counter);
        }
    }
    else {
        std::cout << "Arquivo " << full_path << " não existe\n";
    }
}


void receive_file_server(const std::string &user_id, const std::string &filename, SSL *other_server_ssl) {
    fs::path absolute_path = server_dir / fs::path(user_id) / fs::path(filename);

    //std::cout << "O caminho absoluto até o arquivo no servidor é " << absolute_path.string() << "\n";

    // Vamos ler o tamanho do arquivo!
    size_t file_size;
    read_socket(other_server_ssl, (void *) &file_size, sizeof(file_size));

    std::cout << "Tamanho do arquivo recebido: " << file_size << " bytes\n";

    // Vamos tentar abrir o arquivo
    FILE *file = fopen(absolute_path.c_str(), "wb");
    if (file == nullptr) {
        std::cerr << "Arquivo " << absolute_path << " não pode ser aberto\n";
        send_bool(other_server_ssl, false);
        return;
    }

    send_bool(other_server_ssl, true);
    // Vamos receber os bytes do arquivo.
    std::cout << "Preparando para receber os bytes do arquivo\n";

    // TODO: Receber o arquivo
    read_file(other_server_ssl, file, file_size);
    fclose(file);

    std::cout << "Arquivo " << absolute_path.string() << " recebido\n";

    time_t time;
    read_socket(other_server_ssl, (void *) &time, sizeof(time));

    // escreve a data de modificação do arquivo
    fs::last_write_time(absolute_path, time);

    // Atualiza lista de arquivos do usuário
    update_files(user_id, filename, file_size, time);
}


void server_sigpipe_handler() {
    // Não faz nada por enquanto...
}


//void updateReplicas() {
    //for (auto &replica : replicas) {
        //std::vector<FileInfo> server_files = get_server_files();

    //}
//}

FileInfo *get_file_info(std::string user_id, std::string filename) {
    auto it = clients.find(user_id);

    // Verifica se o cliente existe no dicionário
    if (it == clients.end()) {
        return nullptr;
    }

    Client *client = it->second;

    // Procura o FileInfo a ser atualizado
    FileInfo *file = nullptr;
    for (FileInfo &file_info : client->files) {
        if (file_info.filename() == filename) {
            file = &file_info;
            break;
        }
    }
    return file;
}


void hold_file_for_client(std::string user_id, SSL *client_ssl, int client_socket_fd, std::string filename) {

    std::cout << "Start Hold lock user " << filename << "\n";
    
    FileInfo *fi = get_file_info(user_id, filename);
    
    if (fi != nullptr && fi->holder == nullptr) {
        std::cout << "Token concedido para " << filename << " " << (long) (client_ssl) << "\n";
        fi->holder = client_ssl;
    }
    
    std::cout << "End Hold lock user " << filename << "\n";
}

void release_file_for_client(std::string user_id, SSL *client_ssl, int client_socket_fd, std::string filename) {

    std::cout << "Start Release lock user " << filename << "\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));

    FileInfo *fi = get_file_info(user_id, filename);
    
    if (fi != nullptr && fi->holder == client_ssl) {
        std::cout << "Token liberado por " << filename << " " << (long) (client_ssl) << "\n";
        fi->holder = nullptr;
    }
    
    std::cout << "End Release lock user " << filename << "\n";
}


