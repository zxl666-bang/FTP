#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <mutex>
#include <random>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <queue>
#include <thread>
#include <condition_variable>
#include <functional>
#define MAX_EVENTS 128
#define BUFFER_SIZE 8192
#define DEFAULT_FTP_PORT 2122
#define PASSIVE_PORT_START 50000
#define PASSIVE_PORT_END   50100
using namespace std;
class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(queue_mutex);
                        condition.wait(lock, [this] { return stop || !tasks.empty(); });
                        if (stop && tasks.empty()) return;
                        task = move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
        }
    }
    void enqueue(function<void()> task) {
        {
            unique_lock<mutex> lock(queue_mutex);
            if (stop) throw runtime_error("线程中止");
            tasks.push(move(task));
        }
        condition.notify_one();
    }
    ~ThreadPool() {
        {
            unique_lock<mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (thread &worker : workers) worker.join();
    }
private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queue_mutex;
    condition_variable condition;
    bool stop;
};
unordered_map<string, string> user_db;
string DIR_ROOT;
string USER_FILE;
set<int> used_ports;
mutex port_mutex;
unordered_map<string, bool> online_users;
mutex online_mutex;
void sendResponse(int fd, int code, const string& msg) 
{
    string resp = to_string(code) + " " + msg + "\r\n";
    send(fd, resp.c_str(), resp.size(), 0);
}
bool loadUsers() 
{
    ifstream file(USER_FILE);
    if (!file.is_open())
     {
        cerr << "无法打开用户文件: " << USER_FILE << endl;
        return false;
    }
    string line;
    while (getline(file, line)) 
    {
        if (line.empty())
         continue;
        istringstream iss(line);
        string name, pass;
        if (iss >> name >> pass)
            user_db[name] = pass;
    }
    return true;
}
int allocatePassivePort() 
{
    lock_guard<mutex> lock(port_mutex);
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dist(PASSIVE_PORT_START, PASSIVE_PORT_END);
    for (int i = 0; i < 100; ++i) 
    {
        int port = dist(gen);
        if (used_ports.find(port) == used_ports.end()) 
        {
            used_ports.insert(port);
            return port;
        }
    }
    return -1;
}
void releasePassivePort(int port) 
{
    lock_guard<mutex> lock(port_mutex);
    used_ports.erase(port);
}
bool createDirectoriesForPath(const string& path) 
{
    if (path.empty())
     return true;
    size_t last_slash = path.find_last_of('/');
    if (last_slash == string::npos)
     return true;
    string dir = path.substr(0, last_slash);
    if (dir.empty()) 
    return true;
    if (!createDirectoriesForPath(dir))
     return false;
    if (mkdir(dir.c_str(), 0755) == -1 && errno != EEXIST)
     return false;
    return true;
}
string resolvePath(const string& current_dir, const string& arg)
{
    if (arg.empty()) 
    return current_dir;
    if (arg == "/") 
    return DIR_ROOT;
    if (arg[0] == '/')
     return "";   
    string full = current_dir;
    if (full.back() != '/') 
    full += '/';
    full += arg;
    vector<string> parts;
    stringstream ss(full);
    string part;
    while (getline(ss, part, '/')) 
    {
        if (part.empty() || part == ".")
         continue;
        if (part == "..") 
        {
            if (!parts.empty()) 
            parts.pop_back();
        }
         else
         {
            parts.push_back(part);
        }
    }
    string normalized;
    for (const auto& p : parts) 
    normalized += "/" + p;
    if (normalized.empty())
     normalized = "/";
    if (normalized.find(DIR_ROOT) != 0) 
    return "";
    return normalized;
}
class ClientSession 
{
public:
    int fd;
    bool logged_in;
    string username;
    string current_dir;
    int data_listen_fd;
    int data_port;
    off_t restart_pos;
    string line_buf;
    mutex mtx;
    ClientSession(int sock) : fd(sock), logged_in(false),
        current_dir(DIR_ROOT), data_listen_fd(-1), data_port(-1), restart_pos(0) {}
    ~ClientSession() {
        if (logged_in && !username.empty()) {
            lock_guard<mutex> lock(online_mutex);
            online_users.erase(username);
        }
    }
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;
};
void handleCommand(ClientSession* session, const string& cmd);
void processCommand(ClientSession* s, const string& line)
 {
    lock_guard<mutex> lock(s->mtx);
    string cmd, arg;
    size_t space = line.find(' ');
    if (space == string::npos) 
    {
        cmd = line;
        arg = "";
    } 
    else
     {
        cmd = line.substr(0, space);
        arg = line.substr(space + 1);
    }
    transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);
    if (cmd == "QUIT") 
    {
        if (s->logged_in && !s->username.empty()) 
        {
            lock_guard<mutex> lock(online_mutex);
            online_users.erase(s->username);
        }
        sendResponse(s->fd, 221, "结束");
        close(s->fd);
        return;
    }
    else if (cmd == "USER") 
    {
        s->username = arg;
        sendResponse(s->fd, 331, "输入命令");
    }
    else if (cmd == "PASS") 
    {
        if (s->username.empty())
            sendResponse(s->fd, 530, "无响应");
        else {
            auto it = user_db.find(s->username);
            if (it != user_db.end() && it->second == arg) 
            {
                lock_guard<mutex> lock_online(online_mutex);
                if (online_users.find(s->username) != online_users.end())
                 {
                    sendResponse(s->fd, 530, "不可重复登陆");
                    s->username.clear();
                } 
                else 
                {
                    s->logged_in = true;
                    online_users[s->username] = true;
                    sendResponse(s->fd, 230, "登陆成功");
                }
            }
             else
             {
                sendResponse(s->fd, 530, "登陆失败");
                s->username.clear();
            }
        }
    }
    else if (cmd == "NOOP") 
    {
        sendResponse(s->fd, s->logged_in ? 200 : 530, s->logged_in ? "OK" : "登陆失败");
    }
    else if (cmd == "PWD") 
    {
        if (!s->logged_in)
         { 
            sendResponse(s->fd, 530, "登陆失败"); 
            return;
         }
        sendResponse(s->fd, 257, "当前目录是" + s->current_dir);
    }
    else if (cmd == "CWD")
     {
        if (!s->logged_in) 
        { sendResponse(s->fd, 530, "登陆失败"); return; }
        string target = resolvePath(s->current_dir, arg);
        if (target.empty()) 
        { sendResponse(s->fd, 550, "路径无效或不在根目录内"); return; }
        struct stat st;
        if (stat(target.c_str(), &st) == -1 || !S_ISDIR(st.st_mode)) 
        {
            sendResponse(s->fd, 550, "不是目录或不存在");
            return;
        }
        s->current_dir = target;
        sendResponse(s->fd, 250, "目录切换");
    }
    else if (cmd == "SIZE")
{
    if (!s->logged_in) 
    { 
        sendResponse(s->fd, 530, "登陆失败"); 
        return; 
    }
    string fullpath = resolvePath(s->current_dir, arg);
    if (fullpath.empty())
    {
        sendResponse(s->fd, 213, "0");
        return;
    }
    struct stat st;
    off_t size = 0;
    if (stat(fullpath.c_str(), &st) == 0)
    {
        size = st.st_size;
    }
    sendResponse(s->fd, 213, to_string(size));
}
    else if (cmd == "TYPE") 
{
    sendResponse(s->fd, 200, "Type set OK");
}
    else if (cmd == "PASV") 
    {
        if (!s->logged_in)
         { 
            sendResponse(s->fd, 530, "登陆失败"); 
            return;
         }
        int port = allocatePassivePort();
        if (port == -1) 
        { 
            sendResponse(s->fd, 425, "无空闲端口");
             return;
         }
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd == -1)
         {
             releasePassivePort(port); 
             sendResponse(s->fd, 425, "socket错误"); 
             return; 
        }
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) == -1) 
        {
            close(listen_fd);
            releasePassivePort(port);
            sendResponse(s->fd, 425, "绑定失败");
            return;
        }
        if (listen(listen_fd, 1) == -1) 
        {
            close(listen_fd);
            releasePassivePort(port);
            sendResponse(s->fd, 425, "监听失败");
            return;
        }
        s->data_listen_fd = listen_fd;
        s->data_port = port;
        sockaddr_in local;
        socklen_t len = sizeof(local);
        getsockname(s->fd, (sockaddr*)&local, &len);
        unsigned char* ip = (unsigned char*)&local.sin_addr;
        int p1 = port / 256, p2 = port % 256;
        string msg = "Entering Passive Mode (" + to_string(ip[0]) + "," + to_string(ip[1]) + "," +
                     to_string(ip[2]) + "," + to_string(ip[3]) + "," + to_string(p1) + "," + to_string(p2) + ")";
        sendResponse(s->fd, 227, msg);
    }
    else if (cmd == "LIST") 
    {
        if (!s->logged_in)
         { 
            sendResponse(s->fd, 530, "登陆失败"); 
            return; 
        }
        if (s->data_listen_fd == -1) 
        { 
            sendResponse(s->fd, 425, "先PASV");
             return;
         }
        int data_fd = accept(s->data_listen_fd, nullptr, nullptr);
        if (data_fd == -1)
         {
            sendResponse(s->fd, 425, "数据链接失败");
            close(s->data_listen_fd);
            releasePassivePort(s->data_port);
            s->data_listen_fd = -1;
            return;
        }
        DIR* dir = opendir(s->current_dir.c_str());
        if (!dir) 
        {
            sendResponse(s->fd, 550, "无法打开目录");
            close(data_fd);
            close(s->data_listen_fd);
            releasePassivePort(s->data_port);
            s->data_listen_fd = -1;
            return;
        }
        sendResponse(s->fd, 150, "数据连接打开");
        string listing;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr)
         {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) 
            continue;
            listing += entry->d_name;
            listing += "\r\n";
        }
        closedir(dir);
        send(data_fd, listing.c_str(), listing.size(), 0);
        close(data_fd);
        close(s->data_listen_fd);
        releasePassivePort(s->data_port);
        s->data_listen_fd = -1;
        sendResponse(s->fd, 226, "LIST完成");
    }
    else if (cmd == "REST") 
    {
        if (!s->logged_in) 
        { 
            sendResponse(s->fd, 530, "登陆失败");
             return;
         }
        s->restart_pos = atoll(arg.c_str());
        if (s->restart_pos < 0)
         s->restart_pos = 0;
        sendResponse(s->fd, 350, "断点在" + to_string(s->restart_pos));
    }
    else if (cmd == "RETR")
     {
        if (!s->logged_in) 
        { 
            sendResponse(s->fd, 530, "登陆失败");
             return;
         }
        if (s->data_listen_fd == -1) 
        { 
            sendResponse(s->fd, 425, "先PASV");
             return; 
        }
        string fullpath = resolvePath(s->current_dir, arg);
        if (fullpath.empty()) 
        {
            sendResponse(s->fd, 550, "路径无效");
            close(s->data_listen_fd);
            releasePassivePort(s->data_port);
            s->data_listen_fd = -1;
            return;
        }
        int fd = open(fullpath.c_str(), O_RDONLY);
        if (fd == -1) 
        {
            sendResponse(s->fd, 550, "无法找到文件");
            close(s->data_listen_fd);
            releasePassivePort(s->data_port);
            s->data_listen_fd = -1;
            return;
        }
        struct stat st;
        fstat(fd, &st);
        if (S_ISDIR(st.st_mode)) 
        {
            close(fd);
            sendResponse(s->fd, 550, "不是文件");
            close(s->data_listen_fd);
            releasePassivePort(s->data_port);
            s->data_listen_fd = -1;
            return;
        }
        if (s->restart_pos > st.st_size) 
        {
            close(fd);
            sendResponse(s->fd, 550, "断点过大");
            close(s->data_listen_fd);
            releasePassivePort(s->data_port);
            s->data_listen_fd = -1;
            return;
        }
        int data_fd = accept(s->data_listen_fd, nullptr, nullptr);
        if (data_fd == -1)
         {
            sendResponse(s->fd, 425, "数据链接失败");
            close(fd);
            close(s->data_listen_fd);
            releasePassivePort(s->data_port);
            s->data_listen_fd = -1;
            return;
        }
        sendResponse(s->fd, 150, "数据连接打开");
        off_t offset = s->restart_pos;
        off_t remaining = st.st_size - offset;
        while (remaining > 0) 
        {
            ssize_t sent = sendfile(data_fd, fd, &offset, remaining);
            if (sent == -1) 
            {
                if (errno == EAGAIN)
                 { 
                    usleep(1000); 
                    continue;
                 }
                break;
            }
            remaining -= sent;
        }
        close(fd);
        close(data_fd);
        close(s->data_listen_fd);
        releasePassivePort(s->data_port);
        s->data_listen_fd = -1;
        sendResponse(s->fd, 226, "文件下载完毕");
        s->restart_pos = 0;
    }
    else if (cmd == "STOR") 
    {
        if (!s->logged_in) 
        { 
            sendResponse(s->fd, 530, "登陆失败"); 
            return;
         }
        if (s->data_listen_fd == -1)
         { 
            sendResponse(s->fd, 425, "先PASV"); 
            return; 
        }
        string fullpath = resolvePath(s->current_dir, arg);
        if (fullpath.empty()) 
        {
            sendResponse(s->fd, 550, "路径无效");
            close(s->data_listen_fd);
            releasePassivePort(s->data_port);
            s->data_listen_fd = -1;
            return;
        }
        struct stat st;
        bool file_exists = (stat(fullpath.c_str(), &st) == 0);
        off_t existing_size = file_exists ? st.st_size : 0;
        if (s->restart_pos > existing_size) 
        {
            sendResponse(s->fd, 550, "断点过大");
            close(s->data_listen_fd);
            releasePassivePort(s->data_port);
            s->data_listen_fd = -1;
            return;
        }
        if (!createDirectoriesForPath(fullpath))
         {
            sendResponse(s->fd, 550, "无法创建目录");
            close(s->data_listen_fd);
            releasePassivePort(s->data_port);
            s->data_listen_fd = -1;
            return;
        }
        int open_flags = O_WRONLY | O_CREAT;
        if (s->restart_pos == 0) 
        open_flags |= O_TRUNC;
        int fd = open(fullpath.c_str(), open_flags, 0644);
        if (fd == -1) 
        {
            sendResponse(s->fd, 550, "无法创建文件");
            close(s->data_listen_fd);
            releasePassivePort(s->data_port);
            s->data_listen_fd = -1;
            return;
        }
        if (s->restart_pos > 0) 
        {
            if (lseek(fd, s->restart_pos, SEEK_SET) == -1) 
            {
                sendResponse(s->fd, 550, "无法定位断点");
                close(fd);
                close(s->data_listen_fd);
                releasePassivePort(s->data_port);
                s->data_listen_fd = -1;
                return;
            }
        }
        int data_fd = accept(s->data_listen_fd, nullptr, nullptr);
        if (data_fd == -1)
         {
            sendResponse(s->fd, 425, "数据链接失败");
            close(fd);
            close(s->data_listen_fd);
            releasePassivePort(s->data_port);
            s->data_listen_fd = -1;
            return;
        }
        sendResponse(s->fd, 150, "数据链接打开");
        char buf[BUFFER_SIZE];
        bool error = false;
        while (true) 
        {
            ssize_t n = recv(data_fd, buf, sizeof(buf), 0);
            if (n == 0)
             break;
            if (n == -1) 
            {
                if (errno == EINTR) 
                continue;
                error = true;
                break;
            }
            ssize_t written = 0;
            while (written < n)
             {
                ssize_t w = write(fd, buf + written, n - written);
                if (w == -1) 
                {
                    if (errno == EINTR)
                     continue;
                    error = true;
                    break;
                }
                written += w;
            }
            if (error) break;
        }
        close(fd);
        close(data_fd);
        close(s->data_listen_fd);
        releasePassivePort(s->data_port);
        s->data_listen_fd = -1;
        sendResponse(s->fd, error ? 451 : 226, error ? "上传失败" : "上传成功");
        s->restart_pos = 0;
    }
    else 
    {
        sendResponse(s->fd, 502, "命令无效");
    }
}
void handleCommand(ClientSession* session, const string& cmd) 
{
    processCommand(session, cmd);
}
int main(int argc, char* argv[]) 
{
    signal(SIGPIPE, SIG_IGN);
    if (argc >= 2)
     DIR_ROOT = argv[1];
    else
    {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) 
        DIR_ROOT = cwd;
        else
         DIR_ROOT = ".";
    }
    if (DIR_ROOT.back() != '/')
     DIR_ROOT += '/';
    if (argc >= 3) USER_FILE = argv[2];
    else USER_FILE = "./信息.txt";
    int ftp_port = DEFAULT_FTP_PORT;
    if (argc >= 4) 
    {
        ftp_port = atoi(argv[3]);
        if (ftp_port <= 0 || ftp_port > 65535) 
        ftp_port = DEFAULT_FTP_PORT;
    }
    if (!loadUsers())
     { 
        cerr << "加载用户信息失败" << endl; 
        return 1;
     }
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) 
    {
         perror("socket");
          return 1;
     }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(ftp_port);
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) == -1) 
    { 
        perror("bind"); 
        close(listen_fd);
         return 1; 
    }
    if (listen(listen_fd, 128) == -1) 
    { 
        perror("listen");
         close(listen_fd); 
         return 1;
     }
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    int epfd = epoll_create(1);
    if (epfd == -1)
     { 
        perror("epoll_create");
         close(listen_fd);
         return 1;
     }
    epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);
    ThreadPool pool(8);
    unordered_map<int, ClientSession*> sessions;
    cout << "FTP 服务器端口 " << ftp_port << endl;
    cout << "根目录: " << DIR_ROOT << endl;
    cout << "用户文件: " << USER_FILE << endl;
    epoll_event events[MAX_EVENTS];
    while (true) 
    {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n == -1) 
        { 
            perror("epoll_wait");
             break;
         }
        for (int i = 0; i < n; ++i)
         {
            int fd = events[i].data.fd;
            if (fd == listen_fd)
            {
                while (true) 
                {
                    sockaddr_in client_addr;
                    socklen_t len = sizeof(client_addr);
                    int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &len);
                    if (client_fd == -1) 
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) 
                        break;
                        else 
                        break;
                    }
                    int fl = fcntl(client_fd, F_GETFL, 0);
                    fcntl(client_fd, F_SETFL, fl | O_NONBLOCK);
                    epoll_event ev_client;
                    ev_client.events = EPOLLIN | EPOLLET;
                    ev_client.data.fd = client_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev_client);
                    ClientSession* session = new ClientSession(client_fd);
                    sessions[client_fd] = session;
                    sendResponse(client_fd, 220, "服务器准备完毕");
                    cout << "新客户端接入 " << inet_ntoa(client_addr.sin_addr) << endl;
                }
            } 
            else
             {
                ClientSession* session = sessions[fd];
                if (!session) 
                continue;
                char buf[4096];
                bool closed = false;
                while (true) 
                {
                    ssize_t ret = recv(fd, buf, sizeof(buf), 0);
                    if (ret == 0) 
                    { 
                        closed = true; 
                        break; 
                    }
                    else if (ret == -1)
                     {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) 
                        break;
                        else
                        {
                             closed = true; 
                             break;
                     }
                    } 
                    else 
                    {
                        lock_guard<mutex> lock(session->mtx);
                        session->line_buf.append(buf, ret);
                        size_t pos;
                        while ((pos = session->line_buf.find('\n')) != string::npos) 
                        {
                            string line = session->line_buf.substr(0, pos);
                            session->line_buf.erase(0, pos + 1);
                            if (!line.empty() && line.back() == '\r') 
                            line.pop_back();
                            pool.enqueue([session, line]() { handleCommand(session, line); });
                        }
                    }
                }
                if (closed)
                 {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    delete session;
                    sessions.erase(fd);
                    cout << "客户端断开连接" << endl;
                }
            }
        }
    }
    close(listen_fd);
    close(epfd);
    return 0;
}