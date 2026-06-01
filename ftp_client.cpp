#include <iostream>
#include <string>
#include <sstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include<sys/sendfile.h>
#include <sys/stat.h>
#include <errno.h>
#include<algorithm>
#include<sys/file.h>
using namespace std;
int ctrl_fd = -1;
int data_fd = -1;
string recvResponse() 
{
    char buf[4096] = {0};
    ssize_t n = recv(ctrl_fd, buf, sizeof(buf)-1, 0);
    if (n <= 0) 
    return "";
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
    if (l == string::npos || r == string::npos) return false;
    string nums = pasv_resp.substr(l+1, r-l-1);
    replace(nums.begin(), nums.end(), ',', ' ');
    istringstream iss(nums);
    int a,b,c,d,p1,p2;
    iss >> a >> b >> c >> d >> p1 >> p2;
    string ip = to_string(a) + "." + to_string(b) + "." + to_string(c) + "." + to_string(d);
    int port = p1 * 256 + p2;
    data_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (data_fd < 0)
     return 
    false;
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
void receiveFile(const string& filename) 
{
    if (data_fd < 0) return;
    int fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) 
    {
        perror("open file");
        close(data_fd);
        data_fd = -1;
        return;
    }
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
    cout << "File saved as " << filename << endl;
}
void sendFile(const string& filename) 
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
    char buf[8192];
    while (true)
     {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) 
        break;
       sendfile(data_fd,fd,0,n);
    }
    close(fd);
    close(data_fd);
    data_fd = -1;
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
        string cmd_to_send = line;
        if (line.substr(0,4) == "RETR" || line.substr(0,4) == "STOR") 
        {
            istringstream iss(line);
            string cmd, arg;
            iss >> cmd >> arg;
            cmd_to_send = cmd + " " + arg;
        }
        sendCmd(cmd_to_send);
        string resp = recvResponse();
        cout << resp << endl;
        if (resp.substr(0,3) == "227")
         {

            setupDataConnection(resp);
        } else if (resp.substr(0,3) == "150")
         {
            string cmd = line.substr(0, line.find(' '));
            if (cmd == "LIST")
             {
                receiveList();
            } else if (cmd == "RETR")
             {
                string remote, local;
                istringstream iss(line);
                string c;
                iss >> c >> remote;
                if (iss >> local) 
                {
                    receiveFile(local);
                } 
                else 
                {
                    receiveFile(remote);
                }
            } 
            else if (cmd == "STOR") 
            {
                string local;
                istringstream iss(line);
                string c;
                iss >> c >> local;
                sendFile(local);
            }
            string final = recvResponse();
            cout << final << endl;
        }
    }
    close(ctrl_fd);
    return 0;
}