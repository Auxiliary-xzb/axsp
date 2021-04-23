#include "v3.h"
#include <pthread.h>
#include <sys/epoll.h>

static pthread_mutex_t acceptLock = PTHREAD_MUTEX_INITIALIZER;
static node event_g[MAXFD+1];

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
    printf("listen fd is %d\n", lisfd);

    int opt = 1;
    bzero(&servaddr, sizeof(servaddr));
    setsockopt(lisfd, SOL_SOCKET, SO_REUSEADDR, (void *)&opt, sizeof(opt));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

    (void)bind(lisfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    listen(lisfd, 5);

    setNode(&event_g[0], lisfd, acceptConnection, &event_g[0]);    
    addNode(epollfd, EPOLLIN, &event_g[0]);
}

void acceptConnection(int epollfd, void *arg) {
    node *listnNode = (node *)arg;

    int lisfd = listnNode->fd;

    int connfd;
    struct sockaddr_in cliaddr;
    socklen_t clilen = sizeof(cliaddr);

    bzero(&cliaddr, sizeof(cliaddr));
    pthread_mutex_lock(&acceptLock);
    if ((connfd = accept(lisfd, (struct sockaddr *)&cliaddr, &clilen)) == -1) {
	perror("Accept error:");
	printf("%d\n", lisfd);
	abort();
    }
    pthread_mutex_unlock(&acceptLock);
   
    //int flags = fcntl(connfd, F_GETFD); 
    //flags |= O_NONBLOCK;
    //fcntl(connfd, F_SETFD, flags);

    char name[128];
    inet_ntop(AF_INET, &cliaddr.sin_addr, name, sizeof(name));
    int cliport = ntohs(cliaddr.sin_port);
    printf("connection from %s:%d\n", name, cliport);

    int i;
    for (i = 1; i < MAXFD; i++) {
	if (event_g[i].inTree == 0)
	    break;
    }

    newNode(&event_g[i], name, sizeof(name), cliport);
    setNode(&event_g[i], connfd, readMessage, &event_g[i]); 
    addNode(epollfd, EPOLLIN, &event_g[i]);
    printf("listen read............................\n");
}

void readMessage(int epollfd, void *arg) {
    node *connNode = (node *)arg; 
    int connfd = connNode->fd;

    delNode(epollfd, connNode);
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
#if 1
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
 #if 0
 	printf("wrtie buf[%d]:%c", i, connNode->buf[i]);
 #endif
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

//    tmp.events = events | EPOLLET;
    tmp.events = events;
    tmp.data.ptr = realNode;

    epoll_ctl(epollfd, EPOLL_CTL_ADD, realNode->fd, &tmp);
#if 1
    printf("Add node %d for %s.\n", realNode->fd, (events & EPOLLIN) ? "EPOLLIN" : "EPOLLOUT");
#endif
}

static void delNode(int epollfd, node *realNode) {
    struct epoll_event tmp = {0,{0}}; 
    realNode->inTree = 0;
    realNode->events = 0;

    //为什么这里一定要删除是因为添加操作一定是
    //在描述符没有挂载在红黑树上时才行，所以如
    //果该描述符仍然在红黑树上，那么它没有被更
    //改，并且一直在监听读时间。
    epoll_ctl(epollfd, EPOLL_CTL_DEL, realNode->fd, &tmp);
#if 0
    printf("del %d node success.\n", realNode->fd);
#endif
}

pthread_pool *pthreadPoolCreate(int thrmin, int thrmax, int quemax){
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
    pool->task_list = (pthread_task *)malloc(sizeof(pthread_task)*quemax);
    bzero(pool->task_list, sizeof(pthread_task)*quemax);

    pthread_create(&pool->adjust_thread, NULL, adjustPthread, (void *)pool);

    int i;
    for (i = 0; i < thrmin; i++) {
	pthread_create(&pool->pthreads[i], NULL, pthreadWaitTask, (void *)pool);
    }

    return pool;
}

void *adjustPthread(void *thrpool) {
    pthread_pool * pool = (pthread_pool *)thrpool;

#if 2
    printf("Manager start.\n");
#endif

    //当管理者线程开启时就一定是忙碌的线程
    pthread_mutex_lock(&pool->mutex_bsy_alv);
    pool->bsy_thr_num++;
    pthread_mutex_unlock(&pool->mutex_bsy_alv);
    //要一直监听，所以放在一个死循环中
    while(1) {

	//管理线程只有在条件满足时才会执行,但是
	//它自己不会主动判断条件，它只负责被唤醒
	//所以把它放在一个死循环中。
	pthread_mutex_lock(&pool->mutex_thr);
	while (1) {
	    pthread_cond_wait(&pool->do_adjust, &pool->mutex_thr);
	    //一旦被唤醒就跳出死循环开始判断需要做的操作
	    break;
	}

	//首先根据忙碌的线程判断是否需要扩容。
	if (((float)pool->bsy_thr_num / (float)pool->alv_thr_num) > 0.75) {
	    //增大
	    pool->pthreads = (pthread_t *)realloc(pool->pthreads, pool->alv_thr_num + pool->step);
	    pool->max_thr_num += pool->step;
#if 2
	    printf("Manager increase the threads pool.\n");	
#endif
	} else if (((float)pool->bsy_thr_num / (float)pool->alv_thr_num) < 0.15)  {
	    //这里只减少一部分线程
	    int index = pool->max_thr_num - pool->step;
	    for (int i = pool->alv_thr_num - 1;
		    i > index; i--) 
		free(&pool->pthreads[i]);
#if 0
	    printf("Manager decrease the threads pool.\n");
#endif
	}

	//如果当前忙碌的线程和存活的线程数相同则需要
	//开启新的存活线程
	for (int i = 0; i < pool->step; i++) {
	    pthread_create(&pool->pthreads[pool->alv_thr_num + i], NULL, pthreadWaitTask, NULL);
#if 0
	    printf("add new live thread to pool.\n");
#endif 
	}
	pool->alv_thr_num += pool->step;
	pthread_mutex_unlock(&pool->mutex_thr);
    }
    return NULL;
}

//生产者
void *pthreadWaitTask(void *thrpool) {
    pthread_pool * pool = (pthread_pool *)thrpool;

    //每一个线程完成一次工作后就返回线程池
    //中继续等待，因此放在一个死循环中
    while(1) {
	pthread_mutex_lock(&pool->mutex_thr);
	//任务对了中没有任务时就开始等待
	while (pool->cur_que_size == 0) {
#if 0
	    printf("A thread waiting a task.\n");
#endif
	    pthread_cond_wait(&(pool->queue_not_empty), &(pool->mutex_thr));
	}

#if 1
	printf("Get a job\n");
#endif
	//任务队列中有任务时就开始执行
	pthread_task t = pool->task_list[pool->front_queue];
	pool->front_queue = (pool->front_queue + 1) % pool->max_que_size;
	pool->cur_que_size--;

	//拿了东西就告知对方，你可以放了
	pthread_cond_signal((&pool->queue_not_full));

	//如果当前开启的线程都用完了就唤醒管理者线程
	if ( pool->alv_thr_num == pool->bsy_thr_num ) {
#if 1
	    printf("Manager got siganl to work.\n");
#endif
	    pthread_cond_signal(&pool->do_adjust);
	}

	pthread_mutex_unlock(&(pool->mutex_thr));

	//先将当前的busy值增加，然后执行函数，
	//执行完后将busy减少
	pthread_mutex_lock(&(pool->mutex_bsy_alv));
#if 0
	printf("thread[%d] get a work.\n", pool->bsy_thr_num - 1);
#endif 
	pool->bsy_thr_num++;
	pthread_mutex_unlock(&(pool->mutex_bsy_alv));

	//这里就是懒了，task的参数包含了回调函数
	//需要的epollfd和实际参数，实际的解包工作
	//应该交给回调函数自己去做，但是我不想再
	//修改函数原型了，好麻烦。
	//所以就在这里先解包了，这样不好破坏了多
	//线程模型的结构。
	//t.task(t.arg);  好的结构应该让回调函数自己解包
	pthread_task *a = (pthread_task *)t.arg;
	taskArgument *arg = (taskArgument *)a->arg;
	node *c = (node *)arg->realArg;	
	int epollfd = arg->epolldf;
#if 0
	printf("the fd argument is %d\n", c->fd);
#endif
	t.task(epollfd, arg->realArg);

	pthread_mutex_lock(&(pool->mutex_bsy_alv));
#if 1
	printf("thread[%d] has done a work.\n", pool->bsy_thr_num - 1);
#endif
	pool->bsy_thr_num--;
	pthread_mutex_unlock(&(pool->mutex_bsy_alv));
    }
    return NULL;
}

void pthreadAddTask(pthread_pool *pool, pthread_task *task) {
    pthread_mutex_lock((&pool->mutex_thr));

    //如果队列满了就等待
    while (pool->cur_que_size == pool->max_que_size) {
	pthread_cond_wait(&(pool->queue_not_full), &(pool->mutex_thr));
    }

    //将任务添加到人物队列
    bcopy(task, &(pool->task_list[pool->rear_queue]), sizeof(*task));
    pool->rear_queue = (pool->rear_queue + 1) % pool->max_que_size;
#if 1
    printf("file fd is %d, event is %s.\n",((node*)(((taskArgument *)task->arg)->realArg))->fd,
	    (((node*)(((taskArgument *)task->arg)->realArg))->events & EPOLLIN) ? "EPLLIN" : "EPOLLOUT");
    printf("A task add to queue .There has %d task.\n", pool->cur_que_size);
#endif
    pool->cur_que_size++;

    //加入队列之后告诉别人你可以拿了
    pthread_cond_broadcast(&(pool->queue_not_empty));
    pthread_mutex_unlock(&(pool->mutex_thr));
}

int main(int argc, char *argv[]) {
    int epollfd, port;
    struct epoll_event revent[1024];

    if (argc < 2) {
	port  = 9527;
    } else {
	port = atoi(argv[2]);
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

    pthread_pool *pool = pthreadPoolCreate(20, 100, 100);

    epollfd = epoll_create(MAXFD);

    //主线程用来监听
    Listen(epollfd, port);

    taskArgument arg = {0, NULL};
    pthread_task task = {NULL, NULL};	

    for (;;) {
	int ret = epoll_wait(epollfd, revent, MAXFD, 1000);

	for (int i = 0; i != ret; i++) {
	    node *ev = (node *)revent[i].data.ptr;

	    bzero(&task, sizeof(task));
	    bzero(&arg, sizeof(arg));

	    //将epoll描述服和传递给毁掉函数的
	    //参数都封装到一个结构体中
	    arg.epolldf = epollfd;
	    arg.realArg = (void *)ev;
	    task.task = ev->callBack;
	    task.arg = (void *)&arg;

	    //先只处理写和读
	    if  ((revent[i].events & EPOLLIN) && (ev->events & EPOLLIN)) {
#if 1
		printf("fd is %d.......................................\n", ev->fd);
#endif
		pthreadAddTask(pool, &task);
	    }
	    if ((revent[i].events & EPOLLOUT) && (ev->events & EPOLLOUT)) {
		pthreadAddTask(pool, &task);
	    }
	}
    }
    return 0;
}



/*
 * 这里我忘记了一个知识点，就是epoll的触发模式
 * 默认情况下它是水平触发的，即它读了一次缓存区
 * 之后就一直读，但是默认处理链接时肯定只需要是
 * 读取监听套接字一次就行了，只有变换时才读第二
 * 次
 * 所以应该设置epoll为边沿触发
 */
