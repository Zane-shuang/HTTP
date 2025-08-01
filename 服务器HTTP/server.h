#ifndef SERVER_H
#define SERVER_H

#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <queue>
#include <functional>
#include <memory>
#include <condition_variable>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unordered_map>

// 线程池类
class ThreadPool {
public:
    ThreadPool(size_t threads);
    ~ThreadPool();

    // 添加任务到线程池
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type>;

private:
    // 工作线程
    std::vector<std::thread> workers;
    // 任务队列
    std::queue<std::function<void()>> tasks;

    // 同步变量
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

// HTTP请求处理类
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    // 解析HTTP请求
    bool parse(const std::string& request);
};

// HTTP响应处理类
struct HttpResponse {
    std::string version;
    int status_code;
    std::string status_message;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    // 生成响应字符串
    std::string toString() const;

    // 设置默认响应头
    void setDefaultHeaders();
};

// HTTP服务器类
class HttpServer {
public:
    HttpServer(int port);
    ~HttpServer();

    // 启动服务器
    void start();
    // 停止服务器
    void stop();

    // 注册路由处理函数
    using HandlerFunc = std::function<void(const HttpRequest&, HttpResponse&)>;
    void addRoute(const std::string& path, HandlerFunc handler);

private:
    int port_;
    int server_fd_;
    int epoll_fd_;
    bool running_;
    std::unique_ptr<ThreadPool> thread_pool_;
    std::unordered_map<std::string, HandlerFunc> routes_;

    // 创建服务器套接字
    void createSocket();
    // 设置套接字为非阻塞
    void setNonBlocking(int fd);
    // 处理新连接
    void handleNewConnection();
    // 处理请求
    void handleRequest(int client_fd);
    // 发送响应
    void sendResponse(int client_fd, const HttpResponse& response);
    // 默认路由处理
    void defaultHandler(const HttpRequest& request, HttpResponse& response);
};

#endif // SERVER_H