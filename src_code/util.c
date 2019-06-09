//
// Latest edit by TeeKee on 2017/7/23.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include "util.h"
#include "http_request.h"
#include "epoll.h"

int read_conf(char* filename, tk_conf_t* conf){
    // 以只读方式打开文件
    FILE* fp = fopen(filename, "r");              //打开失败返回0
    if(!fp)                               
        return TK_CONF_ERROR;                     //宏定义，-1

    char buff[BUFLEN];                            //#define BUFLEN 8192
    int buff_len = BUFLEN;
    char* curr_pos = buff;                        //指针当前位置，初始为数组起始位置
    char* delim_pos = NULL;                       //分界符，用于分隔各个参数
    int i = 0;                                    
    int pos = 0;
    int line_len = 0;
    while(fgets(curr_pos, buff_len - pos, fp)){   //char *fgets(char *str, int n, FILE *stream) 从指定的流 stream 读取一行信息，并把它存储在 str 所指向的字符串内。当读取 (n-1) 个字符时，或者读取到换行符时，或者到达文件末尾时，它会停止，具体视情况而定。
        // 定位每行第一个界定符位置
        delim_pos = strstr(curr_pos, DELIM);      // char *strstr(const char *haystack, const char *needle) 在字符串 haystack 中查找第一次出现字符串 needle 的位置，不包含终止符 '\0'。
        if(!delim_pos)                            //没找到返回NULL
            return TK_CONF_ERROR;
        if(curr_pos[strlen(curr_pos) - 1] == '\n'){
            curr_pos[strlen(curr_pos) - 1] = '\0';        //改为字符串结束符
        }

        // 得到root信息
        if(strncmp("root", curr_pos, 4) == 0){            // int strncmp(const char *str1, const char *str2, size_t n) 把 str1 和 str2 进行比较，最多比较前 n 个字节。
            delim_pos = delim_pos + 1;
            while(*delim_pos != '#'){                     //conf->root = ./
                conf->root[i++] = *delim_pos;
                ++delim_pos;
            }
        }

        // 得到port值
        if(strncmp("port", curr_pos, 4) == 0)
            conf->port = atoi(delim_pos + 1);            //conf->port = 3000，int atoi(const char *str) 把参数 str 所指向的字符串转换为一个整数（类型为 int 型）。

        // 得到thread数量
        if(strncmp("thread_num", curr_pos, 9) == 0)
            conf->thread_num = atoi(delim_pos + 1);      //conf->thread_num = 4

        // line_len得到当前行行长
        line_len = strlen(curr_pos);

        // 当前位置跳转至下一行首部
        curr_pos += line_len;                   //最终三行信息均被存在curr_pos指向的字符串中
    }
    fclose(fp);
    return TK_CONF_OK;
}

void handle_for_sigpipe(){
    struct sigaction sa;                        //int sigaction(int signum, const struct sigaction *act,struct sigaction *oldact);
    memset(&sa, '\0', sizeof(sa));              //void *memset(void *str, int c, size_t n) 复制字符 c（一个无符号字符）到参数 str 所指向的字符串的前 n 个字符。
    sa.sa_handler = SIG_IGN;                    //新的信号处理函数
    sa.sa_flags = 0;                            //用来设置信号处理的其他相关操作
    if(sigaction(SIGPIPE, &sa, NULL))           //signum参数指出要捕获的信号类型，act参数指定新的信号处理方式，oldact参数输出先前信号的处理方式（如果不为NULL的话）
        return;
}

int socket_bind_listen(int port){
    // 检查port值，取正确区间范围
    port = ((port <= 1024) || (port >= 65535)) ? 6666 : port;

    // 创建socket(IPv4 + TCP)，返回监听描述符
    int listen_fd = 0;
    if((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        return -1;

    // 消除bind时"Address already in use"错误
    int optval = 1;
    if(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int)) == -1){
        return -1;
    }

    // 设置服务器IP和Port，和监听描述符绑定
    struct sockaddr_in server_addr;
    bzero((char*)&server_addr, sizeof(server_addr));              //置字节字符串前n个字节为零且包括‘\0’。
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)port);
    if(bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
        return -1;

    // 开始监听，最大等待队列长为LISTENQ
    if(listen(listen_fd, LISTENQ) == -1)
        return -1;

    // 无效监听描述符
    if(listen_fd == -1){
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

int make_socket_non_blocking(int fd){
    int flag = fcntl(fd, F_GETFL, 0);
    if(flag == -1)
        return -1;

    flag |= O_NONBLOCK;
    if(fcntl(fd, F_SETFL, flag) == -1)
        return -1;
}

void accept_connection(int listen_fd, int epoll_fd, char* path){
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(struct sockaddr_in));
    socklen_t client_addr_len = 0;
    int accept_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_addr_len);
    if(accept_fd == -1)
        perror("accept");

    // 设为非阻塞模式
    int rc = make_socket_non_blocking(accept_fd);

    // 申请tk_http_request_t类型节点并初始化
    tk_http_request_t* request = (tk_http_request_t*)malloc(sizeof(tk_http_request_t));
    tk_init_request_t(request, accept_fd, epoll_fd, path);

    // 文件描述符可以读，边缘触发(Edge Triggered)模式，保证一个socket连接在任一时刻只被一个线程处理
    tk_epoll_add(epoll_fd, accept_fd, request, (EPOLLIN | EPOLLET | EPOLLONESHOT));

    // 新增时间信息
    tk_add_timer(request, TIMEOUT_DEFAULT, tk_http_close_conn);
}

