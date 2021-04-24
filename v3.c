#include "v3.h"
#include <pthread.h>
#include <errno.h>
#include <sys/epoll.h>

//可能多个链接请求任务加入到线程池中，所以必须让accept保证
//同一时刻只有一个线程在执行accpet
static pthread_mutex_t _acceptLock = PTHREAD_MUTEX_INITIALIZER;

//保存所有挂载在树上的节点的额外参数的信息
static pthread_mutex_t _listenFDlock = PTHREAD_MUTEX_INITIALIZER;
static tree_node_t hadListenedFD[MAXFD+1];
static int counter;

static void setNode(tree_node_t *oldNode, int fd, void(*callBack)(int epollfd, void *newNode), void *newNode);
static void addNode(int epollfd, int events, tree_node_t *newNode);
static void modNode(int epollfd, int events, tree_node_t *newNode);
static void newNode(tree_node_t *newNode, char cliname[], int len, int port);

void getListener(int epollfd, int port){
    int lisfd;
    struct sockaddr_in servaddr;

    lisfd = socket(AF_INET, SOCK_STREAM, 0);
    printf("listen fd is %d\n", lisfd);

    int opt = 1;
    bzero(&servaddr, sizeof(servaddr));
    setsockopt(lisfd, SOL_SOCKET, SO_REUSEADDR, (void *)&opt, sizeof(opt));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

    int flags = fcntl(lisfd, F_GETFD); 
    flags |= O_NONBLOCK;
    fcntl(lisfd, F_SETFD, flags);

    (void)bind(lisfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    listen(lisfd, 5);

    setNode(&hadListenedFD[0], lisfd, acceptConnection, &hadListenedFD[0]);    
    addNode(epollfd, EPOLLIN, &hadListenedFD[0]);
}

void acceptConnection(int epollfd, void *arg) {
    tree_node_t *listnNode = (tree_node_t *)arg;

    int lisfd = listnNode->fd;

    int connfd = -1;
    struct sockaddr_in cliaddr;
    socklen_t clilen = sizeof(cliaddr);

    bzero(&cliaddr, sizeof(cliaddr));
    /*
     * 要保证同一时间只有一个线程进行链接，因为监听
     * 套接字相当于一个共享资源
     */
    pthread_mutex_lock(&_acceptLock);
    if ((connfd = accept(lisfd, (struct sockaddr *)&cliaddr, &clilen)) == -1) {
	printf("errno %d error is %s\n", errno, strerror(errno));
	printf("%d\n", lisfd);
	abort();
    }
    counter++;
    pthread_mutex_unlock(&_acceptLock);

    int flags = fcntl(connfd, F_GETFD); 
    flags |= O_NONBLOCK;
    fcntl(connfd, F_SETFD, flags);

    pthread_mutex_lock(&_listenFDlock);
    char name[128];
    inet_ntop(AF_INET, &cliaddr.sin_addr, name, sizeof(name));
    int cliport = ntohs(cliaddr.sin_port);
    printf("[%d]:connection from %s:%d\n", counter, name, cliport);

    int i;
    for (i = 1; i < MAXFD; i++) {
	if (hadListenedFD[i].inTree == 0){
	    printf("%d\n", i);
	    break;
	}
    }

    newNode(&hadListenedFD[i], name, sizeof(name), cliport);
    setNode(&hadListenedFD[i], connfd, readMessage, &hadListenedFD[i]); 
    addNode(epollfd, EPOLLIN, &hadListenedFD[i]);
    pthread_mutex_unlock(&_listenFDlock);
}

void readMessage(int epollfd, void *arg) {
    tree_node_t *connNode = (tree_node_t *)arg;

    int connfd = connNode->fd;
    /*
     * 这里如果使用局部buffer的话那么
     * 在套接字可写时就无法访问到了
     * 如果使用全局的话，在多线程就出
     * 现麻烦了。所以直接放在结构体里。
     * 同样的也要写端知道读取了多大，
     * 因此也放在结构体中
     */
    int n = read(connfd, connNode->buf, sizeof(connNode->buf));

    if (n > 0) {
	connNode->readedLen = n;

	for (int i = 0; i != n; i++) {
	    printf("%c", connNode->buf[i]);
	}
	printf("\n");

	pthread_mutex_lock(&_listenFDlock);
	setNode(connNode, connfd, writeMessage, connNode);
	modNode(epollfd, EPOLLOUT, connNode);
	pthread_mutex_unlock(&_listenFDlock);
    } else if (n == 0) {
	close(connfd);
    } else {
	close(connfd);
	printf("[%s:%d]%d read error.\n", connNode->cliname, connNode->cliport,connfd);
    }
}

void writeMessage(int epollfd, void *arg) {
    tree_node_t *connNode = (tree_node_t *)arg;
    int connfd = connNode->fd;

    char buf[MAXLINE];
    for (int i = 0; i != connNode->readedLen; i++) {
	buf[i] = toupper(connNode->buf[i]);
    }

    write(connfd, buf, connNode->readedLen);   

    pthread_mutex_lock(&_listenFDlock);
    setNode(connNode, connfd, readMessage, connNode);
    modNode(epollfd, EPOLLIN, connNode);
    pthread_mutex_unlock(&_listenFDlock);
}

/*
 * 创建一个新的需要挂在到树上的节点，这是一个什么
 * 都不会声明的空节点
 */
static void newNode(tree_node_t *newNode, char cliname[], int len, int port) {
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
 * 每次在将节点挂上时都需要对其等待的事件做额外的声明，
 * 有两种情况：1.新节点挂上树，此时只需要只需要新节点的
 *		 基本信息和回调函数、文件描述符即可。
 *	       2.对于从树上拿下需要再次挂上树的节点则需
 *	         要保存其原有信息，然后再修改。
 * 所以都可以表示成使用新节点保存旧节点的方式。
 */
static void setNode(tree_node_t *old, int fd, void (*callBack)(int fd, void *arg), void *newNode) {
    tree_node_t *new = (tree_node_t *)newNode;

    new->fd = old->fd = fd;
    new->events = old->events;
    new->inTree = old->inTree;
    new->callBack = callBack;
    new->readedLen = old->readedLen;
    new->arg = new;
    strncpy(new->buf, old->buf, old->readedLen);
}

/*
 * 将修改好的节点或者新节点添加到树上
 */
static void addNode(int epollfd, int events, tree_node_t *newNode) {
    struct epoll_event tmp;

    if (newNode->inTree == 0)    
	newNode->inTree = 1;
    newNode->events = events;

    tmp.events = events | EPOLLET;
    tmp.data.ptr = newNode;

    epoll_ctl(epollfd, EPOLL_CTL_ADD, newNode->fd, &tmp);
}

static void modNode(int epollfd, int events, tree_node_t *newNode) {
    struct epoll_event tmp;

    if (newNode->inTree == 0)    
	newNode->inTree = 1;
    newNode->events = events;

    tmp.events = events | EPOLLET;
    tmp.data.ptr = newNode;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, newNode->fd, &tmp);
}

pthread_pool *createThreadPool(int thrmin, int thrmax, int quemax){
    pthread_pool *pool = (pthread_pool *)malloc(sizeof(pthread_pool));

    pool->bsy_thr_num = 0;
    pool->alv_thr_num = thrmin;
    pool->max_thr_num = thrmax;
    pool->min_thr_num = thrmin;

    pool->max_que_size = quemax;
    pool->cur_que_size = 0;
    pool->front_queue = 0;
    pool->rear_queue = 0;

    pool->step = 10;

    pthread_mutex_init(&pool->mutex_thr , NULL);
    pthread_mutex_init(&pool->mutex_bsy_alv, NULL);
    pthread_cond_init(&pool->queue_not_full, NULL);
    pthread_cond_init(&pool->queue_not_empty, NULL);
    pthread_cond_init(&pool->do_adjust, NULL);

    pool->pthreads = (pthread_t *)malloc(sizeof(pthread_t)*thrmax);
    pool->task_list = (pthread_task_t *)malloc(sizeof(pthread_task_t)*quemax);
    bzero(pool->task_list, sizeof(pthread_task_t)*quemax);

    pthread_create(&pool->adjust_thread, NULL, adjustThreadPool, (void *)pool);

    int i;
    for (i = 0; i < thrmin; i++) {
	pthread_create(&pool->pthreads[i], NULL, waitThreadTask, (void *)pool);
    }

    return pool;
}

/*
 * 管理者线程在线程池工作良好的情况下不需要执行任何操作，只会
 * 等待线程池的通知，在接受到通知之后才进行线程池的扩容或收缩
 */
void *adjustThreadPool(void *thrpool) {
    pthread_pool * pool = (pthread_pool *)thrpool;

    //当管理者线程开启时就一定是忙碌的线程
    pthread_mutex_lock(&pool->mutex_bsy_alv);
    pool->bsy_thr_num++;
    pthread_mutex_unlock(&pool->mutex_bsy_alv);

    //管理者的生命周期和整个程序的生命周期相同，因此在死循环中
    while(1) {

	/*
	 * 管理者线程通常情况下处于休眠状态，所以将其放在一个
	 * 死循环中等待通知，只有通知到来时才会执行任务。
	 */
	pthread_mutex_lock(&pool->mutex_thr);
	while (1) {
	    pthread_cond_wait(&pool->do_adjust, &pool->mutex_thr);
	    //一旦被唤醒就跳出死循环开始判断需要做的操作
	    break;
	}
	
	//首先根据忙碌的线程判断是否需要扩容或收缩
	if (((float)pool->bsy_thr_num / (float)pool->alv_thr_num) > 0.75) {
	    pool->pthreads = (pthread_t *)realloc(pool->pthreads, pool->alv_thr_num + pool->step);
	    pool->max_thr_num += pool->step;
	} else if (((float)pool->bsy_thr_num / (float)pool->alv_thr_num) < 0.15)  {
	    int index = pool->max_thr_num - pool->step;
	    for (int i = pool->alv_thr_num - 1; i > index; i--) 
		free(&pool->pthreads[i]);
	}

	/*
	 * 如果通知者不需要扩容或者收缩，则代表线程池中的
	 * 绝大部分线程都处于工作中了，需要添加一些线程等
	 * 待在线程池中。 
	 */
	for (int i = 0; i < pool->step; i++) {
	    pthread_create(&(pool->pthreads[pool->alv_thr_num + i]), NULL, waitThreadTask, NULL); 
	    printf("Add thread to pool, fd is %u.\n", pool->pthreads[pool->alv_thr_num + i]);
	}
	pool->alv_thr_num += pool->step;
	pthread_mutex_unlock(&pool->mutex_thr);
    }
    return NULL;
}

/*
 * 等待任务线程中的所有线程都等待在任务队列上，只有
 * 当队列中存在任务时才会被唤醒，由内核决定拿取任务
 * 的线程。
 * 等待任务的线程就是生产者和消费者模型中的消费者
 */
void *waitThreadTask(void *thrpool) {
    pthread_pool * pool = (pthread_pool *)thrpool;
    pthread_task_t t;

    /*
     * 这里设计的是每个线程处理完任务之后就返回
     * 线程池中继续等待拿去下一个任务，因此将获
     * 取任务处理任务的过程放在一个死循环中。
     */
    while(1) {
	pthread_mutex_lock(&pool->mutex_thr);
	//当任务队列中没有任务时则等待队列不为空
	while (pool->cur_que_size == 0) {
	    pthread_cond_wait(&(pool->queue_not_empty), &(pool->mutex_thr));
	}

	t.epolldf = (pool->task_list[pool->front_queue]).epolldf;
	t.task = (pool->task_list[pool->front_queue]).task;
	t.arg = (pool->task_list[pool->front_queue]).arg;

	pool->front_queue = (pool->front_queue + 1) % pool->max_que_size;
	pool->cur_que_size--;

	//拿了东西就告知对方，你可以放了
	pthread_cond_signal((&pool->queue_not_full));

	//如果当前开启的线程都用完了就唤醒管理者线程
	if ( pool->alv_thr_num == pool->bsy_thr_num ) {
	    printf("Wakeup manager.\n");
	    pthread_cond_signal(&pool->do_adjust);
	}

	pthread_mutex_unlock(&(pool->mutex_thr));

	//在执行任务前后都需要更新忙碌线程数量
	pthread_mutex_lock(&(pool->mutex_bsy_alv));
	pool->bsy_thr_num++;
	pthread_mutex_unlock(&(pool->mutex_bsy_alv));

	(*t.task)(t.epolldf, t.arg);

	pthread_mutex_lock(&(pool->mutex_bsy_alv));
	pool->bsy_thr_num--;
	pthread_mutex_unlock(&(pool->mutex_bsy_alv));
    }
    return NULL;
}

/*
 * 向线程池的任务队列中添加任务就相当于生产者和消费者
 * 模型中的生产者。
 */
void addThreadTask(pthread_pool *pool, pthread_task_t *task) {
    pthread_mutex_lock((&pool->mutex_thr));

    //如果队列满了就等待
    while (pool->cur_que_size == pool->max_que_size) {
	pthread_cond_wait(&(pool->queue_not_full), &(pool->mutex_thr));
    }

    //将任务添加到任务队列
    bcopy(task, &(pool->task_list[pool->rear_queue]), sizeof(*task));
    pool->rear_queue = (pool->rear_queue + 1) % pool->max_que_size;
    pool->cur_que_size++;

    //加入队列之后通知消费者可以从队列中拿去任务了
    pthread_cond_broadcast(&(pool->queue_not_empty));
    pthread_mutex_unlock(&(pool->mutex_thr));
}

int main(int argc, char *argv[]) {
    int epollfd, port;
    struct epoll_event revent[1024];

    port  = 9527;
    pthread_pool *pool = createThreadPool(10, 100, 100);

    epollfd = epoll_create(MAXFD);

    //主线程用来监听
    getListener(epollfd, port);
    pthread_task_t work;

    for (;;) {
	int ret = epoll_wait(epollfd, revent, MAXFD, -1);

	for (int i = 0; i != ret; i++) {
	    tree_node_t *ev = (tree_node_t *)revent[i].data.ptr;

	    /*
	     * 当监听到描述符的可写或者可读事件时则获取描述符在树上节点
	     * 存储的额外信心进行判断，如果发生的事件和希望处理的事件
	     * 一致那么就调用相应的处理函数。
	     */
	    if ( ((revent[i].events & EPOLLIN) && (ev->events & EPOLLIN))  
		    || ((revent[i].events & EPOLLOUT) && (ev->events & EPOLLOUT)) ) {

		work.epolldf = epollfd;
		work.task = ev->callBack;
		work.arg = ev;
		addThreadTask(pool, &work);
	    }
	}
    }
    return 0;
}


/*
 *  犯了一个很低级的错误，本意是想让函数帮我
 *  分配空间的，所以直接传递了一个指针，但是
 *  在函数中就是修改的这个指针的值啊，所以要
 *  让函数修改指针的值（而不是指针指向的值）
 *  那就应该传递指针的指针
 *
 *  但是这样很麻烦，所以就用返回值的形式将分
 *  配的空间返回。
 */
//void pthreadPoolCreate(pthread_pool *pool, int thrmin, int thrmax, int quemax);
//pthread_pool pool;
//pthreadPoolCreate(&pool, 20, 100, 100);


/*
 * 这里我忘记了一个知识点，就是epoll的触发模式
 * 默认情况下它是水平触发的，即它读了一次缓存区
 * 之后就一直读，但是默认处理链接时肯定只需要是
 * 读取监听套接字一次就行了，只有变换时才读第二
 * 次
 * 所以应该设置epoll为边沿触发
 */
