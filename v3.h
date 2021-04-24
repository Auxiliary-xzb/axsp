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
#include <pthread.h>

#define MAXFD	128
#define MAXLINE 1024

typedef struct tree_node_t {
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
}tree_node_t;

typedef struct pthread_task_t {
    int epolldf;
    void (*task)(int epollfd, void *arg);
    void *arg;
}pthread_task_t;

typedef struct pthread_pool {
    //线程池允许的最小线程数
    int min_thr_num;
    //线程池允许的最大线程数
    int max_thr_num;
    //线程池中忙碌的线程数
    int bsy_thr_num;
    //线程池中活动的线程数，因为线程数
    //会根据进程中线程的使用情况来扩张
    //和收缩，那么存活的线程量就不一定
    //和最大线程量相同了
    int alv_thr_num;
    //管理线程要同时访问忙线程和存活线
    //程的总量来判断是否需要增加或者收
    //缩线程，所以需要互斥锁。
    //但是这里存在一个死锁问题，即管理
    //线程需要同时请求两个数据，如果为
    //两个量设置两个互斥锁，那么可能出
    //现死锁。但是分析他们的使用过程发
    //现，只有在管理线程工作时才会同时
    //访问他们，其他时候只会修改busy，
    //因此只需要一把锁即可
    pthread_mutex_t mutex_bsy_alv;

    //线程池由数组实现，因此要存储首地址
    pthread_t *pthreads;
    //管理线程用来执行扩容和收缩工作
    pthread_t adjust_thread;
    //访问该结构的互斥锁
    pthread_mutex_t mutex_thr;

    //任务请求队列（循环队列），用户只需要负责将请求
    //来，由线程池保存。之后的调用工作则
    //于用户无关
    pthread_task_t *task_list;
    //任务队列的最大值
    int max_que_size; 
    //任务对了当前含有的任务量
    int cur_que_size;
    //任务队列的头指针
    int front_queue;
    //任务队列的尾指针
    int rear_queue;
    //条件队列涉及到拿出和放入就是生产者
    //和消费者模型，因此需要两个条件变量
    //一个告知可以拿了，一个告知可以放了
    pthread_cond_t queue_not_empty;    
    pthread_cond_t queue_not_full;

    //该条件变量用于通知管理线程
    pthread_cond_t do_adjust;
    //增大和减少时的步长
    int step;
}pthread_pool;

void getListener(int epollfd, int port);
void acceptConnection(int epollfd, void *arg);
void readMessage(int epollfd, void *arg);
void writeMessage(int epollfd, void *arg);

void *adjustThreadPool(void *thrpool);
pthread_pool *createThreadPool(int thrmin, int thrmax, int quemax);
void *waitThreadTask(void *thrpool);
void addThreadTask(pthread_pool *pool, pthread_task_t *task);

