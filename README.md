1. 操作系统要求
* Linux（内核 ≥ 2.6，支持 epoll、sendfile）
* 推荐：Ubuntu 18.04+、CentOS 7+、Debian 9+ 等常见发行版。
* 不支持 Windows（除非使用 WSL2 或 Cygwin，但未测试）。

2. 编译器与工具
* g++ 版本 ≥ 5.4（支持 C++11 和 -pthread）
* 安装示例（Ubuntu/Debian）：
```
sudo apt update
sudo apt install g++ make
```
1. 准备工作
* 创建用户文件
```
echo "alice 123456" > users.txt
echo "bob 654321" >> users.txt
```
* 创建服务器根目录（示例）
```
mkdir -p /home/ftp_data
```
* 编译服务器和客户端
* 编译服务器
```
g++ -std=c++11 -pthread ftps1.cpp -o ftpserver
```
* 编译客户端
```
g++ -std=c++11 ftpc1.cpp -o ftpclient
```
* 启动服务器
```
./ftpserver /home/ftp_data ./users.txt 2122
参数1：根目录 /home/ftp_data

参数2：用户文件 ./users.txt

参数3：端口 2122（可省略，默认为2122）
```
服务器输出示例：
```
FTP 服务器端口 2122
根目录: /home/ftp_data/
用户文件: ./users.txt
```
* 启动客户端并登录
```
./ftpclient 127.0.0.1 2122
```
客户端输出：
```
220 服务器准备完毕
ftp>
登录：

text
ftp> USER alice
331 输入命令
ftp> PASS 123456
230 登陆成功
```
* 基本命令示例
* 查看当前目录
```
ftp> PWD
257 当前目录是/home/ftp_data
```
* 列出文件
```
ftp> LIST
227 Entering Passive Mode (127,0,0,1,195,92)
150 数据连接打开
file1.txt
file2.c
226 LIST完成
```
* 切换目录
```
ftp> CWD subdir
250 目录切换
ftp> PWD
257 当前目录是/home/ftp_data/subdir
```
* 返回根目录
```
ftp> CWD /
250 目录切换
```
* 上传文件（STOR）
* 上传本地文件到服务器当前目录（同名）
```
# 假设客户端当前目录下有一个 test.txt
ftp> STOR test.txt
```
* 上传本地文件到服务器指定子目录并重命名
```
ftp> STOR /home/zxl666/桌面/1.c uploads/newname.c
```
* 自动创建 uploads 目录

* 服务器保存为 newname.c

*  下载文件（RETR）
* 下载服务器文件到客户端当前目录（同名）
```
ftp> RETR file1.txt
```
* 下载到指定本地路径
```
ftp> RETR file1.txt /home/zxl666/Desktop/copy.txt
```
* 断点续传示例
* 上传续传
* 假设第一次上传 large.zip 到 remote.zip 中途中断，第二次执行相同命令：
```
ftp> STOR large.zip remote.zip
```
* 客户端自动发送 SIZE remote.zip 获取已上传大小（例如 1048576 字节）

* 发送 REST 1048576

* 服务器返回 350，然后客户端从本地文件的第 1048576 字节开始发送。

* 下载续传
* 如果本地已有部分 down.zip：

```
ftp> RETR remote.zip down.zip
```
* 客户端自动获取本地 down.zip 大小（例如 524288 字节）

* 发送 REST 524288

* 服务器从该偏移开始发送，客户端以追加模式写入。


*  退出
```
    ftp> quit
221 结束
```
* 断点续传
下载测试
下载一个大文件，中途中断。
再次执行相同 RETR，应显示“本地已存在...，remote大小=...，local大小=...,尝试续传”。


