#include <iostream>
#include <string>
#include <sstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <errno.h>
#include <algorithm>
using namespace std;
int ctrl_fd = -1;
int data_fd = -1;
string recvResponse() 
{
    char buf[4096] = {0};
    ssize_t n = recv(ctrl_fd, buf, sizeof(buf)-1, 0);
    if (n <= 0) return "";
    return string(buf);
}
void sendCmd(const string& cmd)
 {
    string msg = cmd + "\r\n";
    send(ctrl_fd, msg.c_str(), msg.size(), 0);
}
bool setupDataConnection(const string& pasv_resp)
 {
    size_t l = pasv_resp.find('(');
    size_t r = pasv_resp.find(')');
    if (l == string::npos || r == string::npos) 
    return false;
    string nums = pasv_resp.substr(l+1, r-l-1);
    replace(nums.begin(), nums.end(), ',', ' ');
    istringstream iss(nums);
    int a,b,c,d,p1,p2;
    iss >> a >> b >> c >> d >> p1 >> p2;
    string ip = to_string(a) + "." + to_string(b) + "." + to_string(c) + "." + to_string(d);
    int port = p1 * 256 + p2;
    data_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (data_fd < 0) 
    return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    if (connect(data_fd, (sockaddr*)&addr, sizeof(addr)) < 0)
     {
        close(data_fd);
        data_fd = -1;
        return false;
    }
    return true;
}
bool autoPassive()
 {
    if (data_fd != -1) 
    return true; 
    sendCmd("PASV");
    string resp = recvResponse();
    cout << resp << endl;
    if (resp.substr(0,3) != "227")
     {
        cerr << "自动 PASV 失败" << endl;
        return false;
    }
    if (!setupDataConnection(resp))
     {
        cerr << "数据连接建立失败" << endl;
        return false;
    }
    return true;
}
void receiveList()
 {
    if (data_fd < 0) 
    return;
    char buf[8192];
    while (true) 
    {
        ssize_t n = recv(data_fd, buf, sizeof(buf), 0);
        if (n <= 0) 
        break;
        cout.write(buf, n);
        cout.flush();
    }
    close(data_fd);
    data_fd = -1;
}
void receiveFile(const string& filename, off_t resume_offset)
 {
    if (data_fd < 0) 
    return;
    int open_flags = O_WRONLY | O_CREAT;
    if (resume_offset > 0) 
    open_flags |= O_APPEND;
    else 
    open_flags |= O_TRUNC;
    int fd = open(filename.c_str(), open_flags, 0644);
    if (fd < 0) 
    {
        perror("open file");
        close(data_fd);
        data_fd = -1;
        return;
    }
    if (resume_offset > 0) 
    lseek(fd, resume_offset, SEEK_SET);
    char buf[8192];
    while (true) 
    {
        ssize_t n = recv(data_fd, buf, sizeof(buf), 0);
        if (n <= 0)
         break;
        write(fd, buf, n);
    }
    close(fd);
    close(data_fd);
    data_fd = -1;
    cout << "文件保存为 " << filename << " (断点: " << resume_offset << ")" << endl;
}
void sendFile(const string& filename, off_t resume_offset) 
{
    if (data_fd < 0)
     return;
    int fd = open(filename.c_str(), O_RDONLY);
    if (fd < 0) 
    {
        perror("open file");
        close(data_fd);
        data_fd = -1;
        return;
    }
    if (resume_offset > 0)
     lseek(fd, resume_offset, SEEK_SET);
    off_t offset = resume_offset;
    struct stat st;
    fstat(fd, &st);
    off_t remaining = st.st_size - resume_offset;
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
    data_fd = -1;
}
off_t getServerFileSize(const string& remote)
 {
    sendCmd("SIZE " + remote);
    string resp = recvResponse();
    cout << resp << endl;
    if (resp.substr(0,3) == "213")
     {
        string size_str = resp.substr(4);
        size_t pos = size_str.find_first_not_of("0123456789");
        if (pos != string::npos) 
        size_str = size_str.substr(0, pos);
        return stoll(size_str);
    }
    return -1;
}
off_t getLocalFileSize(const string& filename)
 {
    struct stat st;
  if(stat(filename.c_str(), &st)==0) 
    return st.st_size;
    return 0;
}
int main(int argc, char* argv[]) 
{
    if (argc != 3) 
    {
        cerr << "Usage: " << argv[0] << " <ip> <port>" << endl;
        return 1;
    }
    ctrl_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(argv[2]));
    inet_pton(AF_INET, argv[1], &addr.sin_addr);
    if (connect(ctrl_fd, (sockaddr*)&addr, sizeof(addr)) < 0) 
    {
        perror("connect");
        return 1;
    }
    cout << recvResponse() << endl;
    string line;
    while (true) 
    {
        cout << "ftp> " << flush;
        getline(cin, line);
        if (line.empty())
         continue;
        if (line == "quit" || line == "exit") 
        {
            sendCmd("QUIT");
            cout << recvResponse() << endl;
            break;
        }
        string cmd, arg1, arg2;
        istringstream iss(line);
        iss >> cmd;
        transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);
        if (cmd == "RETR") 
        {
            iss >> arg1 >> arg2;
            if (arg1.empty()) 
            {
                cout << "用法: RETR <远程文件> [本地路径]" << endl;
                continue;
            }
            string remote = arg1;
            string local = arg2.empty() ? arg1 : arg2;
            off_t local_size = getLocalFileSize(local);
           size_t remote_size=getLocalFileSize(remote);
            if (local_size > 0) 
            {
                cout << "本地已存在，local" << local << "，大小=" << local_size << " 字节，尝试续传" << endl;
                sendCmd("REST " + to_string(local_size));
                string rest_resp = recvResponse();
                cout << rest_resp << endl;
                if (rest_resp.substr(0,3) != "350")
                 {
                    cerr << "服务器不支持断点续传，将从头下载（可能覆盖）" << endl;
                    local_size = 0;
                }
            } 
            else 
            {
                local_size = 0;
            }
            if (!autoPassive()) continue;
            sendCmd("RETR " + remote);
            string resp = recvResponse();
            cout << resp << endl;
            if (resp.substr(0,3) == "150") 
            {
                receiveFile(local, local_size);
                string final = recvResponse();
                cout << final << endl;
            } 
            else if (resp.substr(0,3) == "425")
             {
                cerr << "数据连接失效，请重试" << endl;
                if (data_fd != -1) 
                close(data_fd);
                data_fd = -1;
            }
        }
        else if (cmd == "STOR") 
        {
            iss >> arg1 >> arg2;
            if (arg1.empty())
             {
                cout << "用法: STOR <本地文件> [远程路径]" << endl;
                continue;
            }
            string local = arg1;
            string remote = arg2.empty() ? arg1 : arg2;
            size_t local_size=getLocalFileSize(local);
            if (access(local.c_str(), F_OK) == -1)
             {
                cerr << "错误: 本地文件 '" << local << "' 不存在。" << endl;
                continue;
            }
            off_t server_size = getServerFileSize(remote);
            off_t restart_pos = 0;
            if (server_size >= 0)
             {
                cout << "服务器上已存在 " << remote << "，remote大小=" << server_size << " 字节。local大小：" <<local_size<< endl;
                if (server_size > 0) 
                {
                    restart_pos = server_size;
                    sendCmd("REST " + to_string(restart_pos));
                    string rest_resp = recvResponse();
                    cout << rest_resp << endl;
                    if (rest_resp.substr(0,3) != "350")
                     {
                        cerr << "服务器不支持断点续传，将从头覆盖上传" << endl;
                        restart_pos = 0;
                    }
                }
            } 
            else
             {
                restart_pos = 0;
            }
            if (!autoPassive())
             continue;
            sendCmd("STOR " + remote);
            string resp = recvResponse();
            cout << resp << endl;
            if (resp.substr(0,3) == "150")
             {
                sendFile(local, restart_pos);
                string final = recvResponse();
                cout << final << endl;
            } else if (resp.substr(0,3) == "425") 
            {
                cerr << "数据连接失效，请重试" << endl;
                if (data_fd != -1) 
                close(data_fd);
                data_fd = -1;
            }
        }
        else if (cmd == "LIST")
         {
            if (!autoPassive()) 
            continue;
            sendCmd("LIST");
            string resp = recvResponse();
            cout << resp << endl;
            if (resp.substr(0,3) == "150")
             {
                receiveList();
                string final = recvResponse();
                cout << final << endl;
            } 
            else if (resp.substr(0,3) == "425")
             {
                cerr << "数据连接失效，请重试" << endl;
                if (data_fd != -1)
                 close(data_fd);
                data_fd = -1;
            }
        }
        else if (cmd == "PASV")
         {
            sendCmd("PASV");
            string resp = recvResponse();
            cout << resp << endl;
            if (resp.substr(0,3) == "227")
             {
                setupDataConnection(resp);
            }
        }
        else
         {
            sendCmd(line);
            string resp = recvResponse();
            cout << resp << endl;
        }
    }
    close(ctrl_fd);
    return 0;
}