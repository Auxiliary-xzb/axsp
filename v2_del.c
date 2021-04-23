//epoll内部就是一个红黑树。
//create就是创建一个树
//ctrl就是在树上挂上节点，只是这个节点的主要部分是一个
//    描述的事件一个事件，然后额外的给我们了一个void *指针允
//    许我们将任何额外的东西和事件一起挂载到树上。但
//    是epoll完全不会修改你的额外东西。
//wait就是在事件发生时将树上的节点给我们，但是节
//    点仍然在树上。

//所以实现epoll完成事件响应机制就很明了了。
//1.告诉epoll要监听的事件，由于额外的东西不会被
//  修改，所以就可以将事件来临后的所有操作都记录
//  在节点上，在事件到来后直接获取这些操作并执行
//  就可以了。这样程序员就只需要告知事件来的时候
//  做什么操作就行了，之后什么时候调用，什么时候
//  事件会来临都不用管了。
//2.事件到来的时候，从树上拿到节点的额外数据后就
//  找到它已经包含的事件处理操作的函数指针和想要
//  传递给这个函数的参数，开始调用这个处理程序就
//  行了

//在网络中就只需要处理读和写，所以我们下面就只需
//要关注读写事件来的时候的逻辑就行了。
//1.将监听套接字挂上时，并说明在监听套接字可读的
//  时候就调用accept
//2.在accept新链接后就将新的连接套接字挂载到树上
//  并且说明在套接字可读的时候调用readMessage。
//3.在读取数据后就将套接字连接再次挂到树上，并且
//  说明在套接字可写的时候调用sendMessage。

//epoll的两个模式，水平模式在非阻塞和阻塞io的情
//况下都是适用的，并且是默认情况下epoll就是水平
//模式的。但是边沿模式就只有是在非阻塞时才有效
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

#define MAXFD	128
#define MAXLINE 1024

//所以这里就不用什么event名称了
//直接用node就清晰很多
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

//用于设置挂载到红黑树上的节点，最后一个参数为传递给
//回调函数的参数，
static void setNode(node *realNode, int fd, void(*callBack)(int fd, void *arg), void *arg);
static void addNode(int epollfd, int events, node *realNode);
static void delNode(int epollfd, node *realNode);
static void newNode(node *newNode, char cliname[], int len, int port);

void Listen(int epollfd, int port){
    int lisfd;
    struct sockaddr_in servaddr;

    lisfd = socket(AF_INET, SOCK_STREAM, 0);

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
    socklen_t clilen;

    bzero(&cliaddr, sizeof(cliaddr));
    int connfd = accept(lisfd, (struct sockaddr *)&cliaddr, &clilen);

    int flags = fcntl(connfd, F_GETFD);
    flags |= O_NONBLOCK;
    fcntl(connfd, F_SETFD, flags);

    char name[128];
    inet_ntop(AF_INET, &cliaddr.sin_addr, name, sizeof(name));
    int cliport = ntohs(cliaddr.sin_port);
    printf("connection from %s:%d\n", name, cliport);

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

    //将节点删除，在挂载一个新的
    //当然也可以修改已挂载节点 
    delNode(epollfd, connNode);
    /*
     * 这里如果使用局部buffer的话那么
     * 在套接字可写时就无法访问到了
     * 如果使用全局的话，在多线程就出
     * 现麻烦了。所以直接放在结构体里。
     * 同样的也要让写端知道读取了多大，
     * 因此也放在结构体中
     */
    int n = read(connfd, connNode->buf, sizeof(connNode->buf));

    if (n > 0) {
	connNode->readedLen = n;
#if 0
	printf("server has readed %d bytes from %s:%d.\n", connNode->readedLen, connNode->cliname, connNode->cliport);
#endif
	setNode(connNode, connfd, writeMessage, connNode);
	addNode(epollfd, EPOLLOUT, connNode);
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

    delNode(epollfd, connNode);

    setNode(connNode, connfd, readMessage, connNode);
    addNode(epollfd, EPOLLIN, connNode);
}

/*
 * 创建一个全新的树节点，提供给后来的设置使用
 */
static void newNode(node *newNode, char cliname[], int len, int port) {
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
 * 设置一个新的节点或者设置一个从树上获取的
 * 节点
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

/*
 * 将设置好的节点挂载上树
 */
static void addNode(int epollfd, int events, node *realNode) {
    struct epoll_event tmp;

    if (realNode->inTree == 0)    
	realNode->inTree = 1;
    realNode->events = events;

    tmp.events = events | EPOLLET;
    tmp.data.ptr = realNode;

    epoll_ctl(epollfd, EPOLL_CTL_ADD, realNode->fd, &tmp);
    printf("Add node %d for %s.\n", realNode->fd, (events & EPOLLIN) ? "EPOLLIN" : "EPOLLOUT");
}

/*
 * 删除指定的节点
 */
static void delNode(int epollfd, node *realNode) {
    struct epoll_event tmp = {0,{0}}; 
    realNode->inTree = 0;
    realNode->events = 0;

    //为什么这里一定要删除是因为添加操作一定是
    //在描述符没有挂载在红黑树上时才行，所以如
    //果该描述符仍然在红黑树上，那么它没有被更
    //改，并且一直在监听读时间。
    epoll_ctl(epollfd, EPOLL_CTL_DEL, realNode->fd, &tmp);
    printf("del %d node success.\n", realNode->fd);
}

int main(int argc, char *argv[]) {
    int epollfd, port;
    struct epoll_event revent[1024];

    if (argc < 2) {
	port  = 8888;
    } else {
	port = atoi(argv[2]);
    }

    epollfd = epoll_create(MAXFD);

    Listen(epollfd, port);

    for (;;) {
	int ret = epoll_wait(epollfd, revent, MAXFD, 1000);

	for (int i = 0; i != ret; i++) {
	    node *ev = (node *)revent[i].data.ptr;

	    if ((revent[i].events & EPOLLIN) && (ev->events & EPOLLIN)) {
		printf("%d can read.\n", ev->fd);
		ev->callBack(epollfd, (void *)ev);
	    }

	    if ((revent[i].events & EPOLLOUT) && (ev->events & EPOLLOUT)) {
		printf("%d can write.\n", ev->fd);
		ev->callBack(epollfd, (void *)ev);
	    }
	}
    }
    return 0;
}
