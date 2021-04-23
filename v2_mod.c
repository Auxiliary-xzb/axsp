#include <asm-generic/socket.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>

#define SHOW 0
#define MAXFD	128
#define MAXLINE 1024

typedef struct treeNode {
    //在这里保存链接套接字是因为struct epoll_event
    //本身所能携带的数据（即data域）是一个结构体    
    int fd;
    //记录套接字需要等待的事件，这里将套接字的写
    //和读分开。
    int events;
    //这个为传递给回调函数的参数
    void *arg;
    void (*callBack)(int fd, void *arg);
    char cliname[128];
    int cliport;
    //用于标记节点是否在epoll本身的红黑树上
    int inTree;
    //首先要声明一点，该缓冲区的作用是回射服务器的
    //需要。因为服务器将读写事件分离，那么在写时就
    //必须要知道读取了什么。
    char buf[MAXLINE];
    int readedLen;
}node;

static node event_g[MAXFD+1];

void Listen(int epollfd, int port);
void acceptConnection(int epollfd, void *arg);
void readMessage(int epollfd, void *arg);
void writeMessage(int epollfd, void *arg);

static void setNode(node *realNode, int fd, void(*callBack)(int fd, void *arg), void *arg);
static void addNode(int epollfd, int events, node *realNode);
static void modNode(int epollfd, int events, node *realNode);
static void newNode(node *newNode, char cliname[], int len, int port);

void Listen(int epollfd, int port){
    int lisfd;
    struct sockaddr_in servaddr;

    lisfd = socket(AF_INET, SOCK_STREAM, 0);
//    int flags = fcntl(lisfd, F_GETFD);
//    flags |= O_NONBLOCK;
//    fcntl(lisfd, F_SETFD, flags);
//
    int opt = 1;
    bzero(&servaddr, sizeof(servaddr));
    setsockopt(lisfd, SOL_SOCKET, SO_REUSEADDR, (void *)&opt, sizeof(opt));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

    (void)bind(lisfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    listen(lisfd, 5);

    setNode(&event_g[MAXFD], lisfd, acceptConnection, &event_g[MAXFD]);    
    addNode(epollfd, EPOLLIN, &event_g[MAXFD]);
}

void acceptConnection(int epollfd, void *arg) {
    node *listnNode = (node *)arg;

    int lisfd = listnNode->fd;

    struct sockaddr_in cliaddr;
    //该值需要初始化，否则第一次调用会
    //返回0.0.0.0且端口也为0
    socklen_t clilen = sizeof(cliaddr);;

    bzero(&cliaddr, sizeof(cliaddr));
    int connfd = accept(lisfd, (struct sockaddr *)&cliaddr, &clilen);

//    int flags = fcntl(connfd, F_GETFD);
//    flags |= O_NONBLOCK;
//    fcntl(connfd, F_SETFD, flags);

    char name[128];
    inet_ntop(AF_INET, &cliaddr.sin_addr, name, sizeof(name));
    int cliport = ntohs(cliaddr.sin_port);
    printf("connection from %s:%d, connect fd is %d.\n", name, cliport, connfd);

    int i;
    for (i = 0; i < MAXFD; i++) {
	if (event_g[i].inTree == 0)
	    break;
    }

    newNode(&event_g[i], name, sizeof(name), cliport);
    setNode(&event_g[i], connfd, readMessage, &event_g[i]); 
    addNode(epollfd, EPOLLIN, &event_g[i]);
}

void readMessage(int epollfd, void *arg) {
    node *connNode = (node *)arg;

    int connfd = connNode->fd;
    /*
     * 这里如果使用局部buffer的话那么
     * 在套接字可写时就无法访问到了
     * 如果使用全局的话，在多线程就出
     * 现麻烦了。所以直接放在结构体里。
     * 同样的也要写端知道读取了多大，
     * 因此也放在结构体中
     */
    int n = read(connfd, connNode->buf, 4);

    if (n > 0) {
	connNode->readedLen = n;

#if 0
	printf("server has readed %d bytes from %s:%d.\n", connNode->readedLen, connNode->cliname, connNode->cliport);
#endif
	for (int i = 0; i != n; i++) {
	    printf("%c", connNode->buf[i]);
	}
	printf("\n");
	setNode(connNode, connfd, writeMessage, connNode);
	modNode(epollfd, EPOLLOUT, connNode);
    } else if (n == 0) {
	close(connfd);
    } else {
	close(connfd);
	printf("%d read error.", connfd);
    }
}

void writeMessage(int epollfd, void *arg) {
    node *connNode = (node *)arg;
    int connfd = connNode->fd;

#if 0
    printf("start writing. %d bytes will be write.\n", connNode->readedLen); 
#endif
    char buf[MAXLINE];
    for (int i = 0; i != connNode->readedLen; i++) {
	buf[i] = toupper(connNode->buf[i]);
    }

#if 0
    printf("end writing.\n");
#endif 
    write(connfd, buf, connNode->readedLen);   
#if 0
    printf("%s:%d has writed %d bytes.\n", connNode->cliname, connNode->cliport, connNode->readedLen);
#endif

    setNode(connNode, connfd, readMessage, connNode);
    modNode(epollfd, EPOLLIN, connNode);
}

static void newNode(node *newNode, char cliname[], int len, int port) {
    newNode->arg = NULL;
    newNode->callBack = NULL;
    newNode->events = 0;
    newNode->fd = 0;
    newNode->inTree = 0;
    newNode->readedLen = 0;
    newNode->cliport = port;
    bzero(newNode->buf, sizeof(newNode->buf));
    strncpy(newNode->cliname, cliname, len);
}

/* 
 * 这相当于更新操作
 */
static void setNode(node *realNode, int fd, void (*callBack)(int fd, void *arg), void *arg) {
    node *oldNode = (node *)arg;

    realNode->fd = oldNode->fd = fd;
    realNode->events = oldNode->events;
    realNode->inTree = oldNode->inTree;
    realNode->callBack = callBack;
    realNode->readedLen = oldNode->readedLen;
    realNode->arg = arg;    
    strncpy(realNode->buf, oldNode->buf, oldNode->readedLen);
}

static void addNode(int epollfd, int events, node *realNode) {
    struct epoll_event tmp;

    if (realNode->inTree == 0)    
	realNode->inTree = 1;
    realNode->events = events;

    //tmp.events = events | EPOLLET;
    tmp.events = events;
    tmp.data.ptr = realNode;

    epoll_ctl(epollfd, EPOLL_CTL_ADD, realNode->fd, &tmp);
#if 0
    printf("Add node %d for %s.\n", realNode->fd, (events & EPOLLIN) ? "EPOLLIN" : "EPOLLOUT");
#endif
}

static void modNode(int epollfd, int events, node *realNode) {
    struct epoll_event tmp;

    if (realNode->inTree == 0)    
	realNode->inTree = 1;
    realNode->events = events;

    //tmp.events = events | EPOLLET;
    tmp.events = events;
    tmp.data.ptr = realNode;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, realNode->fd, &tmp);
#if SHOW
    printf("Mod node %d for %s.\n", realNode->fd, (events & EPOLLIN) ? "EPOLLIN" : "EPOLLOUT");
#endif
}

int main(int argc, char *argv[]) {
    int epollfd, port;
    struct epoll_event revent[1024];

    if (argc < 2) {
	port  = 9527;
    } else {
	port = atoi(argv[2]);
    }

    epollfd = epoll_create(MAXFD);

    Listen(epollfd, port);

    for (;;) {
	int ret = epoll_wait(epollfd, revent, MAXFD, -1);

	for (int i = 0; i != ret; i++) {
	    node *ev = (node *)revent[i].data.ptr;

	    if ((revent[i].events & EPOLLIN) && (ev->events & EPOLLIN)) {
#if 0
		printf("%d can read.\n", ev->fd);
#endif
		ev->callBack(epollfd, (void *)ev);
	    }

	    if ((revent[i].events & EPOLLOUT) && (ev->events & EPOLLOUT)) {
#if 0
		printf("%d can write.\n", ev->fd);
#endif
		ev->callBack(epollfd, (void *)ev);
	    }
	}
    }
    return 0;
}
