#include <netinet/in.h>
#include "dropboxClient.h"
#include "dropboxUtil.h"
#include <iostream>
#include <memory>
#include <sys/socket.h>
#include <strings.h>
#include "dropboxServer.h"
#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include "Inotify-master/FileSystemEvent.h"
#include "Inotify-master/Inotify.h"
#include <sstream>
#include <thread>
#include <chrono>
#include <netdb.h>
#include <set>

namespace fs = boost::filesystem;

//=============================================================================
// Globais
//=============================================================================


/*
 * ----------------------------------------------------------------------------
 * command_mutex
 * ----------------------------------------------------------------------------
 * O mutex de envio de comandos ao servidor.  Apenas uma das threads do
 * cliente poderá enviar comandos de cada vez.  Uma vez que um comando inicie,
 * ele terá que ser concluído até que a outra thread possa mandar comandos.
 *
 * Isso é necessário porque todas as threads usam o mesmo socket com o servidor
 * para enviar comandos. Como é necessário que haja sincronização entre os
 * comandos enviados pelo cliente, com o servidor esperando pelos comandos,
 * temos que garantir que um comando do cliente não pode ser interrompido
 * no meio.
 * ----------------------------------------------------------------------------
 */
std::mutex command_mutex;


/*
 * ----------------------------------------------------------------------------
 * user_id
 * ----------------------------------------------------------------------------
 * A string que representa o usuário.  Ela é definida por argumento no início
 * do programa.  Ela é usada para criar o diretório local e remoto, e
 * identifica o cliente para fins de exclusão mútua no servidor.
 * ----------------------------------------------------------------------------
 */
std::string user_id;


/*
 * ----------------------------------------------------------------------------
 * user_id
 * ----------------------------------------------------------------------------
 * O diretório local do usuário.
 * ----------------------------------------------------------------------------
 */
fs::path user_dir;


/*
 * ----------------------------------------------------------------------------
 * port_number
 * ----------------------------------------------------------------------------
 * O número da porta do servidor.
 * ----------------------------------------------------------------------------
 */
uint16_t port_number;


/*
 * ----------------------------------------------------------------------------
 * server_address
 * ----------------------------------------------------------------------------
 * O struct com as informações do endereço do servidor.
 * ----------------------------------------------------------------------------
 */
sockaddr_in server_address{};


/*
 * ----------------------------------------------------------------------------
 * socket_fd
 * ----------------------------------------------------------------------------
 * O socket com o qual o cliente se comunica e envia comandos ao servidor.
 * ----------------------------------------------------------------------------
 */
int socket_fd;


/*
 * ----------------------------------------------------------------------------
 * inotify
 * ----------------------------------------------------------------------------
 * Objeto que vai ficar escutando mudanças no diretório do cliente.
 * ----------------------------------------------------------------------------
 */
Inotify inotify(IN_CREATE | IN_MOVED_FROM | IN_MOVED_TO |
                IN_DELETE | IN_CLOSE_WRITE);


//=============================================================================
// Funções
//=============================================================================

/*
 * ----------------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------------
 * A função mais espera pelo:
 *  - user_id
 *  - hostname
 *  - porta
 *
 * A função manda criar o diretório de sincronização, bem como cria as
 * threads para enviar comandos e observar o diretório de sincronização.
 * ----------------------------------------------------------------------------
 */
int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "Argumentos insuficientes\n";
        std::cerr << "./client <user_id> <hostname> <port_number>";
        std::exit(1);
    }

    user_id = std::string(argv[1]);

    std::string hostname = std::string(argv[2]);

    char *end;
    port_number = static_cast<uint16_t>(std::strtol(argv[3], &end, 10));

    // Tenta se conectar ao servidor.
    if (connect_server(hostname, port_number) == ConnectionResult::Error) {
        std::cerr << "Erro ao se conectar com o servidor\n";
        std::exit(1);
    }

    // Cria o diretório de sincronização
    create_sync_dir();

    // Sincroniza arquivos com o servidor
    sync_client();

    // Manda a global inotify cuidar do diretório de sincronização
    inotify.watchDirectoryRecursively(user_dir);

    // Cria thread para mater o cliente sincronizado com o servidor.
    // std::thread get_dir_sync_thread;
    // get_dir_sync_thread = std::thread(run_get_sync_dir_thread);
    // if (!get_dir_sync_thread.joinable()) {
    //     std::cerr << "Erro ao criar thread de get_dir_sync\n";
    //     close_connection();
    //     return 1;
    // };
    // get_dir_sync_thread.detach();


    // Cria a thread de sincronização, para o inotify
    std::thread sync_thread;
    sync_thread = std::thread(run_sync_thread);
    if (!sync_thread.joinable()) {
        std::cerr << "Erro ao criar thread de sincronização\n";
        close_connection();
        return 1;
    }
    sync_thread.detach();


    // Exibe a interface de comandos ao usuário
    run_interface();
}


#pragma clang diagnostic push // Não precisamos de warnings para loops infinitos
#pragma clang diagnostic ignored "-Wmissing-noreturn"
/*
 * ----------------------------------------------------------------------------
 * run_sync_thread
 * ----------------------------------------------------------------------------
 * Escuta por eventos no diretório do usuário. Caso um deles ocorra, envia
 * o comando de associado ao servidor.
 *
 * Antes de cada comando ser enviado, temos que travar o mutex de comandos.
 *
 * O mutex é destravado quando o objeto "lock" sai de escopo e seu destrutor
 * é invocado.
 * ----------------------------------------------------------------------------
 */
void run_sync_thread() {

    // Padrão regex de arquivos que não queremos enviar ao servidor, pois são
    // usados pelos programas para armazenar mudanças temporárias nos arquivos
    // que estão sendo manipulados.
    //
    // Não queremos enviar qualquer arquivo do tipo .goutputstream-*,
    // nem arquivos que comecem com "~"
    boost::regex invalid_files_pattern{"^(\\.goutputstream|~)"};

    while (true) {
        FileSystemEvent event = inotify.getNextEvent();
        auto mask = event.mask;

        // O nome do arquivo que causou o evento, sem o caminho absoluto
        std::string filename = event.path.filename().string();

        std::cout << filename << " causou o evento\n";

        if (mask & IN_MOVED_FROM ||
            mask & IN_DELETE ||
            mask & IN_MOVED_TO ||
            mask & IN_CREATE ||
            mask & IN_MODIFY) {

            if (boost::regex_search(filename, invalid_files_pattern)) {
                // Se o arquivo que causou o evento for temporário, pular o evento.
                //std::cout << "Arquivo " << event.path.string() << " não será enviado ao servidor\n";
                continue;
            }
        }

        if (mask & IN_MOVED_FROM || mask & IN_DELETE) {
            std::lock_guard<std::mutex> lock(command_mutex);
            send_delete_command(filename);
        }
        else if (mask & IN_MOVED_TO || mask & IN_CREATE || mask & IN_MODIFY) {
            // O evento deve ser causado por um arquivo comum, e não um link simbólico ou diretório.
            if (fs::is_regular_file(event.path)) {
                std::lock_guard<std::mutex> lock(command_mutex);
                // O caminho absoluto é necessário na hora de enviar arquivos.
                send_file(event.path.string());
            }
        }

        // IN_ACCESS <- Quando um arquivo existente for acessado para escrita,
        // teremos que obter o lock.  O lock só poderá ser destravado quando
        // esse arquivo específico for fechado, ou seja quando houver
        // IN_CLOSE_WRITE.
    }
}
#pragma clang diagnostic pop


#pragma clang diagnostic push // Desbilita warnings sobre loop infinito
#pragma clang diagnostic ignored "-Wmissing-noreturn"
/*
 * ----------------------------------------------------------------------------
 * run_get_sync_dir_thread
 * ----------------------------------------------------------------------------
 * Roda a função "sync_client()" a cada 5 segundos.
 * ----------------------------------------------------------------------------
 */
void run_get_sync_dir_thread() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::lock_guard<std::mutex> lock(command_mutex);
        sync_client();
    }
}
#pragma clang diagnostic pop


/*
 * ----------------------------------------------------------------------------
 * connect_server
 * ----------------------------------------------------------------------------
 * Tenta conectar o cliente ao servidor.
 *
 * Envia o user_id e espera a resposta.
 *
 * Se não houver 2 outros dispositivos do mesmo user_id conectados, a conexão
 * provavelmente será bem sucedida.
 *
 * Retorna um enum ConnectionResult com o resultado.
 * ----------------------------------------------------------------------------
 */
ConnectionResult connect_server(std::string hostname, uint16_t port) {
    hostent *server = gethostbyname(hostname.c_str());

    if (server == nullptr) {
        std::cerr << "Erro ao obter o servidor\n";
        return ConnectionResult::Error;
    }

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        std::cerr << "Erro ao criar o socket do cliente\n";
        return ConnectionResult::Error;
    }

    bzero((void *) &server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    server_address.sin_addr = *(in_addr *) server->h_addr_list[0];

    std::cout << "Tentando se conecar com o servidor (UserId: " << user_id << ")\n";
    if (connect(socket_fd, (sockaddr *) &server_address, sizeof(server_address)) < 0) {
        std::cerr << "Erro ao conectar com o servidor\n";
        return ConnectionResult::Error;
    }

    // Envia o tipo de conexão ao servidor
    ConnectionType type = ConnectionType::Normal;
    ssize_t bytes = write_socket(socket_fd, (const void *) &type, sizeof(type));
    if (bytes == -1) {
        std::cerr << "Erro enviando o tipo de conexão ao servidor";
        return ConnectionResult::Error;
    }

    send_string(socket_fd, user_id);

    // Recebe o sinal de ok do servidor
    bool ok = false;
    read_socket(socket_fd, (void *) &ok, sizeof(ok));

    if (!ok) {
        return ConnectionResult::Error;
    }

    return ConnectionResult::Success;
}


/*
 * ----------------------------------------------------------------------------
 * print_interface
 * ----------------------------------------------------------------------------
 * Imprime os comandos possíveis ao usuário.
 * ----------------------------------------------------------------------------
 */
void print_interface() {
    std::cout << "Digite o comando:\n";
    std::cout << "\tupload <path/filename.ext>\n";
    std::cout << "\tdownload <filename.ext>\n";
    std::cout << "\tdelete <filename.ext>\n";
    std::cout << "\tlist_server\n";
    std::cout << "\tlist_client\n";
    std::cout << "\tget_sync_dir\n";
    std::cout << "\texit\n";
}


/*
 * ----------------------------------------------------------------------------
 * run_interface
 * ----------------------------------------------------------------------------
 * Fica esperando os comandos do usuário e envia esses comandos ao servidor.
 *
 * Se o comando precisar de um argumento e ele não for fornecido, não haverá
 * tratamento de exceções e o programa irá falhar.
 *
 * A cada iteração a interface será imprimida na tela.
 *
 * Uma vez que um comando for digitado, o mutex de comandos será travado até
 * que o comando seja concluído.
 * ----------------------------------------------------------------------------
 */
void run_interface() {
    std::string delim(" ");
    std::string input;
    std::string command;
    std::string argument;

    do {
        print_interface();
        std::getline(std::cin, input);

        // Trava o mutex de comando
        std::lock_guard<std::mutex> lock(command_mutex);

        command = input.substr(0, input.find(delim));

        if (command == "upload") {
            argument = input.substr(command.size() + 1);
            std::cout << "Upload " << argument << "\n";
            send_file(argument);
        }
        else if (command == "download") {
            argument = input.substr(command.size() + 1);
            std::cout << "Download " << argument << "\n";
            get_file(argument);
        }
        else if (command == "delete") {
            argument = input.substr(command.size() + 1);
            std::cout << "Delete " << argument << "\n";
            delete_file(argument);
        }
        else if (command == "exit") {
            close_connection();
        }
        else if (command == "list_server") {
            std::cout << "ListServer\n";
            list_server_files();
        }
        else if (command == "list_client") {
            std::cout << "ListClient\n";
            list_local_files();
        }
        else if (command == "get_sync_dir") {
            std::cout << "GetSyncDir\n";
            sync_client();
        }
        else {
            std::cout << "Comando não reconhecido\n";
        }

        // Destrava o mutex de comando
    }
    while (command != "exit");
}


/*
 * ----------------------------------------------------------------------------
 * send_file
 * ----------------------------------------------------------------------------
 * Envia o arquivo ao servidor.
 *
 * O caminho absoluto do arquivo deverá ser fornecido.
 *
 * São enviados o tamanho do arquivo, bem como sua data de modificação.  O
 * servidor responde se precisa do arquivo.  Em caso positivo, seus bytes são
 * enviados.
 * ----------------------------------------------------------------------------
 */
void send_file(std::string absolute_filename) {
    ssize_t bytes;
    FILE *file;

    fs::path absolute_path(absolute_filename);

    if (fs::exists(absolute_path) && fs::is_regular_file(absolute_path)) {
        if ((file = fopen(absolute_filename.c_str(), "rb")) != nullptr) {

            // Envia o comando
            Command command = Upload;
            write_socket(socket_fd, (const void *) &command, sizeof(command));

            // Envia o nome do arquivo
            std::string filename = absolute_path.filename().string();
            send_string(socket_fd, filename);

            // Envia o tanho do arquivo
            size_t file_size = fs::file_size(absolute_path);
            write_socket(socket_fd, (const void *) &file_size, sizeof(file_size));

            // Envia a data de modificação do arquivo
            time_t time = fs::last_write_time(absolute_path);
            write_socket(socket_fd, (const void *) &time, sizeof(time));

            // Recebe a confirmação de upload do servidor.
            if (!read_bool(socket_fd)) {
                std::cout << "Arquivo " << absolute_path.string() << " não precisa ser enviado\n";
                fclose(file);
                return;
            }

            bool file_open_ok = read_bool(socket_fd);
            if (!file_open_ok) {
                std::cerr << "O arquivo não conseguiu ser aberto no servidor\n";
                fclose(file);
                return;
            }
            //std::cout << "Preparando para enviar os bytes do arquivo\n";

            // Se o servidor quiser o arquivo, envia os bytes
            send_file(socket_fd, file, file_size);

            fclose(file);
        }
        std::cout << "Arquivo " << absolute_path.string() << " enviado\n";
    }
    else {
        std::cerr << "Arquivo " << absolute_filename << " não existe\n";
    }
}


/*
 * ----------------------------------------------------------------------------
 * get_file
 * ----------------------------------------------------------------------------
 * Função auxiliar para baixar o arquivo para o diretório de execução do
 * cliente.
 * ----------------------------------------------------------------------------
 */
void get_file(std::string filename) {
    get_file(filename, true);
}


/*
 * ----------------------------------------------------------------------------
 * get_file
 * ----------------------------------------------------------------------------
 * Obtém o arquivo do servidor.
 *
 * Caso "current_path" seja verdadeior, o arquivo será baixado no diretório
 * onde o cliente está sendo executado.
 *
 * Caso "current_path" seja falso, esse arquivo será baixado no diretório de
 * sincronização do cliente.
 * ----------------------------------------------------------------------------
 */
void get_file(std::string filename, bool current_path) {

    Command command = Download;
    write_socket(socket_fd, (const void *) &command, sizeof(command));

    send_string(socket_fd, filename);

    bool exists = read_bool(socket_fd);
    if (!exists) {
        std::cerr << "Servidor informou que arquivo não existe\n";
        return;
    }

    size_t file_size;
    read_socket(socket_fd, (void *) &file_size, sizeof(file_size));

    fs::path absolute_path;
    if (current_path) {
        absolute_path = fs::current_path() / fs::path(filename);
    }
    else {
        absolute_path = user_dir / fs::path(filename);
    }

    FILE *file = fopen(absolute_path.c_str(), "wb");
    if (file == nullptr) {
        std::cout << "Erro ao abrir o arquivo para escrita\n";
        send_bool(socket_fd, false);
        return;
    }
    send_bool(socket_fd, true);

    read_file(socket_fd, file, file_size);
    fclose(file);


    time_t time;
    read_socket(socket_fd, (void *) &time, sizeof(time));

    fs::last_write_time(absolute_path, time);

    std::cout << "Arquivo " << filename << " recebido com sucesso\n";
}


/*
 * ----------------------------------------------------------------------------
 * close_connection
 * ----------------------------------------------------------------------------
 * Desconecta o usuário do servidor e fecha o socket.  Encerra o programa.
 * ----------------------------------------------------------------------------
 */
void close_connection() {
    Command command = Exit;
    write_socket(socket_fd, (const void *) &command, sizeof(command));
    close(socket_fd);
}


/*
 * ----------------------------------------------------------------------------
 * list_server_files
 * ----------------------------------------------------------------------------
 * Imprime informações dos arquivos presentes no diretório do usuário no
 * servidor.
 * ----------------------------------------------------------------------------
 */
void list_server_files() {

    // Obtém o vetor com os FileInfo
    std::vector<FileInfo> server_files = get_server_files();

    std::cout << "=====================\n";
    std::cout << "Arquivos no servidor:\n";
    std::cout << "=====================\n\n";
    for (auto &file : server_files) {
        std::cout << file.string() << "\n";
    }
}


/*
 * ----------------------------------------------------------------------------
 * list_local_files
 * ----------------------------------------------------------------------------
 * Imprime informações dos arquivos locais
 *
 * Essas informações são obtidas a partir do sistema de arquivos.
 * ----------------------------------------------------------------------------
 */
void list_local_files() {
    fs::directory_iterator end_iter;
    fs::directory_iterator dir_iter(user_dir);

    char date_buffer[20];

    std::cout << "====================\n";
    std::cout << "Arquivos no cliente:\n";
    std::cout << "====================\n\n";
    while (dir_iter != end_iter) {
        if (fs::is_regular_file(dir_iter->path())) {

            std::cout << "Nome: " << dir_iter->path().filename() << "\n";
            std::cout << "Tamanho: " << fs::file_size(dir_iter->path()) << " bytes\n";

            time_t date = fs::last_write_time(dir_iter->path());
            strftime(date_buffer, 20, "%Y-%m-%d %H:%M:%S", localtime(&date));

            std::cout << "Modificado: " << date_buffer << "\n\n";
        }
        ++dir_iter;
    }
}


/*
 * ----------------------------------------------------------------------------
 * create_sync_dir
 * ----------------------------------------------------------------------------
 * Cria o diretório de sincronização no diretório do usuário.  Também define a
 * global "user_dir" que referencia esse diretório.
 * ----------------------------------------------------------------------------
 */
void create_sync_dir() {
    fs::path home_dir(getenv("HOME"));
    fs::path sync_dir("sync_dir_" + user_id);
    fs::path fullpath = home_dir / sync_dir;

    // Define a global "user_dir"
    user_dir = fullpath;

    if (!fs::exists(fullpath)) {
        fs::create_directory(fullpath);
    }
}


/*
 * ----------------------------------------------------------------------------
 * get_server_files
 * ----------------------------------------------------------------------------
 * Envia o comando de ListServer ao servidor e lê todos os FileInfo do usuário
 * presentes no servidor.
 *
 * Primeiramente lê o tamanho do vetor, depois lê cada um dos structs FileInfo
 * presentes nesse vetor.
 *
 * Esse vetor pode ser usado para o cliente fazer a sincronização ou
 * simplesmente imprimir os arquivos do servidor.
 *
 * Retorna o vetor de FileInfo
 * ----------------------------------------------------------------------------
 */
std::vector<FileInfo> get_server_files() {

    // Envia o comando para listar os arquivos.
    Command command = ListServer;
    write_socket(socket_fd, (const void *) &command, sizeof(command));

    // Lê o tamanho do vetor
    size_t n;
    read_socket(socket_fd, (void *) &n, sizeof(n));

    std::vector<FileInfo> files;
    files.reserve(n);

    // Recebe os membros do vetor e o recria localmente.
    for (int i = 0; i < n; ++i) {
        FileInfo file_info;
        read_socket(socket_fd, (void *) &file_info, sizeof(file_info));
        files.push_back(file_info);
    }

    // Retorna o vetor.
    return std::move(files);
}


/*
 * ----------------------------------------------------------------------------
 * delete_file
 * ----------------------------------------------------------------------------
 * Apaga um arquivo localmente.
 *
 * A thread do inotify se encarregará de enviar o comando de "delete" para o
 * servidor quando o arquivo for movido localmente.
 * ----------------------------------------------------------------------------
 */
void delete_file(std::string filename) {
    fs::path filepath = user_dir / fs::path(filename);

    bool deleted = fs::remove(filepath);

    if (deleted) {
        std::cout << "Arquivo " << filename << " removido\n";
        //send_delete_command(filename);
    }
    else {
        std::cout << "Arquivo " << filename << " não existe no diretório de sincronização\n";
    }
}


/*
 * ----------------------------------------------------------------------------
 * send_delete_command
 * ----------------------------------------------------------------------------
 * Envia o comando Delete ao servidor.
 *
 * Esta função deve ser chamada pela thread do inotify.
 * ----------------------------------------------------------------------------
 */
void send_delete_command(std::string filename) {
    Command command = Delete;

    // Envia o comando de Delete para o servidor
    if (write_socket(socket_fd, (void *) &command, sizeof(command))) {
        send_string(socket_fd, filename);
    }
}


/*
 * ----------------------------------------------------------------------------
 * sync_client
 * ----------------------------------------------------------------------------
 * Sincroniza os arquivos do cliente com os do servidor, e vice-versa.
 *
 * Essa função é implementada enviando diversos comandos ao servidor.
 * ----------------------------------------------------------------------------
 */
void sync_client() {

    // Obtém a lista de arquivos do servidor.
    std::vector<FileInfo> server_files = get_server_files();

    // Conjunto dos nomes dos arquivos do presentes no servidor.
    //
    // É mais conveniente localizar nomes num "set" do que num vetor de
    // FileInfo
    std::set<std::string> files_on_server;

    // Conjunto dos nomes dos arquivos para enviar ao servidor.
    std::set<std::string> files_to_send_to_server;

    for (FileInfo &file_info : server_files) {
        fs::path absolute_path = user_dir / fs::path(file_info.filename());

        // Acrescenta ao conjuto dos arquivos do servidor.
        files_on_server.insert(file_info.filename());

        bool exists = fs::exists(absolute_path);
        if ((exists && (fs::last_write_time(absolute_path) < file_info.last_modified())) || !exists) {
            get_file(file_info.filename(), false);

        }
        else if (fs::last_write_time(absolute_path) > file_info.last_modified()) {
            // Se o arquivo local é mais novo do que o do servidor, ele
            // deve ser enviado para o servidor.

            fs::path filename = user_dir / fs::path(file_info.filename());
            files_to_send_to_server.insert(filename.string());
        }
    }

    // Determina quais arquivos enviar para o servidor.
    fs::directory_iterator end_iter;
    fs::directory_iterator dir_iter(user_dir);
    while (dir_iter != end_iter) {
        if (fs::is_regular_file(dir_iter->path())) {
            std::string filename = dir_iter->path().filename().string();

            // Se o arquivo no diretório do cliente não existe nos arquivos
            // enviados pelo servidor, devemos inserir seu nome para enviar.
            auto it = files_on_server.find(filename);
            if (it == files_on_server.end()) {
                files_to_send_to_server.insert(dir_iter->path().string());
            }
        }
        ++dir_iter;
    }

    //std::cout << "\n\nArquivo para enviar para o servidor\n";
    for (auto &filename : files_to_send_to_server) {
        //std::cout << "Enviando " << filename << " para o servidor\n";
        send_file(filename);
    }
}
