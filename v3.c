#include "v3.h"
#include <pthread.h>
#include <errno.h>
#include <sys/epoll.h>

static pthread_mutex_t _acceptLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t _listenFDlock = PTHREAD_MUTEX_INITIALIZER;
static tree_node_t hadListenedFD[MAXFD+1];
static int counter;

//用于设置挂载到红黑树上的节点，最后一个参数为传递给
//回调函数的参数，
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
	if (hadListenedFD[i].inTree == 0)
	    break;
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
	printf("%d read error.", connfd);
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
 * 这相当于更新操作
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

    //tmp.events = events | EPOLLET;
    tmp.events = events;
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

    //pthread_create(&pool->adjust_thread, NULL, adjustThreadPool, (void *)pool);

    int i;
    for (i = 0; i < thrmin; i++) {
	pthread_create(&pool->pthreads[i], NULL, waitThreadTask, (void *)pool);
    }

    return pool;
}

void *adjustThreadPool(void *thrpool) {
    pthread_pool * pool = (pthread_pool *)thrpool;

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
	} else if (((float)pool->bsy_thr_num / (float)pool->alv_thr_num) < 0.15)  {
	    //这里只减少一部分线程
	    int index = pool->max_thr_num - pool->step;
	    for (int i = pool->alv_thr_num - 1;
		    i > index; i--) 
		free(&pool->pthreads[i]);
	}

	//如果当前忙碌的线程和存活的线程数相同则需要
	//开启新的存活线程
	for (int i = 0; i < pool->step; i++) {
	    pthread_create(&pool->pthreads[pool->alv_thr_num + i], NULL, waitThreadTask, NULL); 
	}
	pool->alv_thr_num += pool->step;
	pthread_mutex_unlock(&pool->mutex_thr);
    }
    return NULL;
}

//生产者
void *waitThreadTask(void *thrpool) {
    pthread_pool * pool = (pthread_pool *)thrpool;
    pthread_task_t t;
    
    //每一个线程完成一次工作后就返回线程池
    //中继续等待，因此放在一个死循环中
    while(1) {
	pthread_mutex_lock(&pool->mutex_thr);
	//任务对了中没有任务时就开始等待
	while (pool->cur_que_size == 0) {
	    pthread_cond_wait(&(pool->queue_not_empty), &(pool->mutex_thr));
	}

	//任务队列中有任务时就开始执行
    t.epolldf = (pool->task_list[pool->front_queue]).epolldf;
    t.task = (pool->task_list[pool->front_queue]).task;
    t.arg = (pool->task_list[pool->front_queue]).arg;
    
	pool->front_queue = (pool->front_queue + 1) % pool->max_que_size;
	pool->cur_que_size--;

	//拿了东西就告知对方，你可以放了
	pthread_cond_signal((&pool->queue_not_full));

	//如果当前开启的线程都用完了就唤醒管理者线程
	if ( pool->alv_thr_num == pool->bsy_thr_num ) {
        printf("hjahhaha\n");
        //pthread_cond_signal(&pool->do_adjust);
	}
	
	pthread_mutex_unlock(&(pool->mutex_thr));

	//先将当前的busy值增加，然后执行函数，
	//执行完后将busy减少
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

void addThreadTask(pthread_pool *pool, pthread_task_t *task) {
    pthread_mutex_lock((&pool->mutex_thr));

    //如果队列满了就等待
    while (pool->cur_que_size == pool->max_que_size) {
	pthread_cond_wait(&(pool->queue_not_full), &(pool->mutex_thr));
    }

    //将任务添加到人物队列
    bcopy(task, &(pool->task_list[pool->rear_queue]), sizeof(*task));
    pool->rear_queue = (pool->rear_queue + 1) % pool->max_que_size;
    pool->cur_que_size++;

    //加入队列之后告诉别人你可以拿了
    pthread_cond_broadcast(&(pool->queue_not_empty));
    pthread_mutex_unlock(&(pool->mutex_thr));
}

int main(int argc, char *argv[]) {
    int epollfd, port;
    struct epoll_event revent[1024];
    
	port  = 9527;

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

    pthread_pool *pool = createThreadPool(10, 100, 100);

    epollfd = epoll_create(MAXFD);

    //主线程用来监听
    getListener(epollfd, port);
    pthread_task_t work;

    for (;;) {
        int ret = epoll_wait(epollfd, revent, MAXFD, -1);

        for (int i = 0; i != ret; i++) {
            tree_node_t *ev = (tree_node_t *)revent[i].data.ptr;

            if ( ((revent[i].events & EPOLLIN) && (ev->events & EPOLLIN))  
                ||  ((revent[i].events & EPOLLOUT) && (ev->events & EPOLLOUT)) ) {

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
 * 这里我忘记了一个知识点，就是epoll的触发模式
 * 默认情况下它是水平触发的，即它读了一次缓存区
 * 之后就一直读，但是默认处理链接时肯定只需要是
 * 读取监听套接字一次就行了，只有变换时才读第二
 * 次
 * 所以应该设置epoll为边沿触发
 */
