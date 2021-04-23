#include <asm-generic/socket.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    int lisfd, connfd;
    struct sockaddr_in servaddr, cliaddr;
    socklen_t clilen;
    char name[128];

    lisfd = socket(AF_INET, SOCK_STREAM, 0);

    int opt = 1;
    bzero(&servaddr, sizeof(servaddr));
    setsockopt(lisfd, SOL_SOCKET, SO_REUSEADDR, (void *)&opt, sizeof(opt));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(9527);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

    bind(lisfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    listen(lisfd, 5);
    /*
     * 单个的epoll_event一定是给ctrl用的
     * 多个的epoll_event一定是给wati用的
     */
    struct epoll_event tmp, revent[1024];
    tmp.events = EPOLLIN;
    tmp.data.fd = lisfd;

    /*
     * ctrl有一个触发模式，水平触发和边沿触发
     * 水平触发：可以理解为只要缓冲区非空，那么它就一直触发
     *		 简单一句话等待数据
     * 边沿触发：只有在发生了填充缓冲区（即可以读数据了）
     *		 可写缓冲区（即可以写缓冲区）的事件了才会触发
     *		 简单一句话，等待事件
     *
     * 这个对于后面的程序很重要，我就是忘记了边沿触发导致了
     * 后面多线程服务器在设置非阻塞模式时一直提示监听套接字
     * 可写，正常应该不会理会监听套接字的写缓冲区的。因为
     * 它不可能到来时间
     */
    int epollfd = epoll_create(1024);
    epoll_ctl(epollfd, EPOLL_CTL_ADD, lisfd, &tmp);

    for (;;) {
	int ret = epoll_wait(epollfd, revent, 1024, -1);

	/*
	 * wait返回的是一个数组，所以要执行遍历操作.
	 */
	for (int i = 0; i != ret; i++) {
	    /*
	     * 如果是监听套接字那么就接受新的链接
	     * 并且设置新连接监听它的读事件
	     */
	    if (revent[i].data.fd == lisfd) {
		clilen = sizeof(cliaddr);
		printf("event is %s.\n", revent[i].events & EPOLLIN ? "EPOLLIN" : "EPOLLOUT");
		connfd = accept(lisfd, (struct sockaddr *)&cliaddr, &clilen);

		printf("connection from %s:%d\n", 
			inet_ntop(AF_INET, &cliaddr.sin_addr, name, sizeof(name)),
			ntohs(cliaddr.sin_port));		
		tmp.data.fd = connfd;
		tmp.events = EPOLLIN;
		epoll_ctl(epollfd, EPOLL_CTL_ADD, connfd, &tmp);
	    } else {
		/*
		 * 如果监听到了已经链接套接字的读事件就读取
		 * 转为大写后返回给用户
		 */
		connfd = revent[i].data.fd;

		char buf[1024];
		int n = read(connfd, buf, sizeof(buf));
		for (int i = 0; i != n; i++) {
		    buf[i] = toupper(buf[i]);
		}
		write(connfd, buf, n);
	    }
	}
    }
    return 0;
}
