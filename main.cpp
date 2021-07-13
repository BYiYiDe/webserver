#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量

// 添加文件描述符到epoll中
extern void addfd( int epollfd, int fd, bool one_shot );
extern void removefd( int epollfd, int fd );

void addsig(int sig, void( handler )(int)){ // 信号
    struct sigaction sa;//注册信号的参数
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) { //参数要传递端口号
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi( argv[1] );//获取端口号
    addsig( SIGPIPE, SIG_IGN ); //忽略
    /*对一个已经收到FIN包的socket调用read方法, 如果接收缓冲已空, 则返回0, 这就是常说的表示连接关闭. 
    但第一次对其调用write方法时, 如果发送缓冲没问题, 会返回正确写入(发送). 但发送的报文会导致对端发送RST报文, 
    因为对端的socket已经调用了close, 完全关闭, 既不发送, 也不接收数据. 所以, 第二次调用write方法(假设在收到RST之后), 会生成SIGPIPE信号, 导致进程退出.*/


    threadpool<http_conn>* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        return 1;
    }

    http_conn* users = new http_conn[ MAX_FD ];//创建一个数组用于保存所有客户端信息

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );//创建监听套接字

    int ret = 0;
    struct sockaddr_in address;//
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    // 端口复用
    int reuse = 1;//1:表示复用
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );//设置端口复用
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );//绑定
    ret = listen( listenfd, 5 );//监听

    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];//事件数量
    int epollfd = epoll_create( 5 );//不要传0就行
    // 添加到epoll对象中
    addfd( epollfd, listenfd, false );//将 监听的文件描述符添加到epoll对象中
    http_conn::m_epollfd = epollfd;

    while(true) {
        
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 ); //阻塞
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) {//EINTR被中断
            printf( "epoll failure\n" );
            break;
        }

        printf("all_users_num: %d \n", http_conn::m_user_count);

        // 循环遍历事件数组
        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;
            
            if( sockfd == listenfd ) {// 有客户端连接进来
                
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );//连接
                
                if ( connfd < 0 ) {//判断连接成功
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) {//目前的连接数满了，要把这个连接关闭。要给客户端写一个信息说服务器正忙
                    close(connfd);
                    continue;
                }
                //直接拿连接的描述符作为索引。因为连接描述符是递增的
                users[connfd].init( connfd, client_address);//将新的客户数据初始化，并放到数组之中

            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) { //对方异常断开或者错误等发生

                users[sockfd].close_conn();//关闭连接。

            } else if(events[i].events & EPOLLIN) {// 是不是有 读 的事件发生

                if(users[sockfd].read()) {//一次性把所有数据都读完
                    //客户端发来后，先读完。读完后，再排进任务队列。在process()中解析请求和发出响应
                    pool->append(users + sockfd);//交给工作队列。users + sockfd：当前任务的地址
                } else {
                    users[sockfd].close_conn();//关闭
                }

            }  else if( events[i].events & EPOLLOUT ) { // 是不是有写 事件发生

                if( !users[sockfd].write() ) { //一次性写完所有数据
                    users[sockfd].close_conn();
                }

            }
        }
    }
    
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}