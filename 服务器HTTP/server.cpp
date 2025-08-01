#include "server.h"
#include <future>
#include <sstream>
#include <fstream>
#include <filesystem>

// 线程池实现
ThreadPool::ThreadPool(size_t threads) : stop(false) {
    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back([this] {
            for (;;) {
                std::function<void()> task;

                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex);
                    this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                    if (this->stop && this->tasks.empty()) return;
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }

                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }

    condition.notify_all();
    for (std::thread &worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

// 添加任务到线程池
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type> {
    using return_type = typename std::result_of<F(Args...)>::type;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<return_type> result = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);

        // 如果线程池已停止，则不能添加新任务
        if (stop) {
            throw std::runtime_error("enqueue on stopped ThreadPool");
        }

        tasks.emplace([task]() { (*task)(); });
    }
    condition.notify_one();
    return result;
}

// 解析HTTP请求
bool HttpRequest::parse(const std::string& request) {
    std::istringstream iss(request);
    std::string line;

    // 解析请求行
    if (!std::getline(iss, line)) {
        return false;
    }
    std::istringstream request_line(line);
    request_line >> method >> path >> version;

    // 解析请求头
    while (std::getline(iss, line) && line != "\r") {
        if (line.empty()) continue;

        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 2); // 跳过冒号和空格
            // 移除行尾的回车符
            if (!value.empty() && value.back() == '\r') {
                value.pop_back();
            }
            headers[key] = value;
        }
    }

    // 解析请求体
    if (headers.find("Content-Length") != headers.end()) {
        try {
            int content_length = std::stoi(headers["Content-Length"]);
            char buffer[content_length + 1];
            iss.read(buffer, content_length);
            buffer[content_length] = '\0';
            body = std::string(buffer);
        } catch (...) {
            return false;
        }
    }

    return true;
}

// 生成HTTP响应字符串
std::string HttpResponse::toString() const {
    std::stringstream ss;

    // 响应行
    ss << version << " " << status_code << " " << status_message << "\r\n";

    // 响应头
    for (const auto& [key, value] : headers) {
        ss << key << ": " << value << "\r\n";
    }

    // 空行分隔响应头和响应体
    ss << "\r\n";

    // 响应体
    ss << body;

    return ss.str();
}

// 设置默认响应头
void HttpResponse::setDefaultHeaders() {
    headers["Content-Type"] = "text/html; charset=utf-8";
    headers["Server"] = "Lightweight HTTP Server";
    headers["Content-Length"] = std::to_string(body.size());
}

// HTTP服务器实现
HttpServer::HttpServer(int port) : port_(port), running_(false) {
    // 创建线程池，默认使用8个线程
    thread_pool_ = std::make_unique<ThreadPool>(8);

    // 注册默认路由
    addRoute("/", [this](const HttpRequest& request, HttpResponse& response) {
        this->defaultHandler(request, response);
    });
}

HttpServer::~HttpServer() {
    stop();
}

// 启动服务器
void HttpServer::start() {
    createSocket();
    running_ = true;

    // 创建epoll
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ == -1) {
        throw std::runtime_error("epoll_create1 failed");
    }

    // 将监听套接字添加到epoll
    struct epoll_event event;
    event.data.fd = server_fd_;
    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &event) == -1) {
        throw std::runtime_error("epoll_ctl failed");
    }

    // 事件循环
    const int MAX_EVENTS = 100;
    struct epoll_event events[MAX_EVENTS];

    while (running_) {
        int num_events = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        if (num_events == -1) {
            if (running_) {
                std::cerr << "epoll_wait failed" << std::endl;
            }
            continue;
        }

        for (int i = 0; i < num_events; ++i) {
            if (events[i].data.fd == server_fd_) {
                // 处理新连接
                handleNewConnection();
            } else {
                // 处理请求
                int client_fd = events[i].data.fd;
                thread_pool_->enqueue([this, client_fd]() {
                    handleRequest(client_fd);
                });
            }
        }
    }

    close(epoll_fd_);
    close(server_fd_);
}

// 停止服务器
void HttpServer::stop() {
    running_ = false;
    // 唤醒epoll_wait
    close(epoll_fd_);
    close(server_fd_);
}

// 注册路由处理函数
void HttpServer::addRoute(const std::string& path, HandlerFunc handler) {
    routes_[path] = handler;
}

// 创建服务器套接字
void HttpServer::createSocket() {
    // 创建套接字
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ == -1) {
        throw std::runtime_error("socket creation failed");
    }

    // 设置套接字选项，允许端口重用
    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        throw std::runtime_error("setsockopt failed");
    }

    // 绑定端口
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
        throw std::runtime_error("bind failed");
    }

    // 监听端口
    if (listen(server_fd_, 10) < 0) {
        throw std::runtime_error("listen failed");
    }

    // 设置为非阻塞模式
    setNonBlocking(server_fd_);
}

// 设置套接字为非阻塞
void HttpServer::setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        throw std::runtime_error("fcntl F_GETFL failed");
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::runtime_error("fcntl F_SETFL failed");
    }
}

// 处理新连接
void HttpServer::handleNewConnection() {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_addr_len);
    if (client_fd == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "accept failed" << std::endl;
        }
        return;
    }

    // 设置客户端套接字为非阻塞
    setNonBlocking(client_fd);

    // 添加到epoll
    struct epoll_event event;
    event.data.fd = client_fd;
    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) == -1) {
        std::cerr << "epoll_ctl failed for client socket" << std::endl;
        close(client_fd);
        return;
    }
}

// 处理请求
void HttpServer::handleRequest(int client_fd) {
    // 读取请求数据
    char buffer[4096];
    std::string request_data;
    int bytes_read;

    while ((bytes_read = read(client_fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        request_data += buffer;

        // 检查是否读取完请求（简单判断，实际应用需要更复杂的处理）
        if (request_data.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }

    if (bytes_read == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        std::cerr << "read failed" << std::endl;
        close(client_fd);
        return;
    }

    if (request_data.empty()) {
        close(client_fd);
        return;
    }

    // 解析请求
    HttpRequest request;
    if (!request.parse(request_data)) {
        HttpResponse response;
        response.version = "HTTP/1.1";
        response.status_code = 400;
        response.status_message = "Bad Request";
        response.body = "<html><body><h1>400 Bad Request</h1></body></html>";
        response.setDefaultHeaders();
        sendResponse(client_fd, response);
        close(client_fd);
        return;
    }

    // 查找路由处理函数
    HttpResponse response;
    response.version = "HTTP/1.1";

    auto it = routes_.find(request.path);
    if (it != routes_.end()) {
        // 调用路由处理函数
        it->second(request, response);
    } else {
        // 默认处理
        defaultHandler(request, response);
    }

    // 设置默认响应头
    response.setDefaultHeaders();

    // 发送响应
    sendResponse(client_fd, response);

    // 关闭连接（HTTP 1.0）
    close(client_fd);
}

// 发送响应
void HttpServer::sendResponse(int client_fd, const HttpResponse& response) {
    std::string response_str = response.toString();
    const char* data = response_str.c_str();
    int data_len = response_str.size();
    int bytes_sent = 0;

    while (bytes_sent < data_len) {
        int sent = write(client_fd, data + bytes_sent, data_len - bytes_sent);
        if (sent == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "write failed" << std::endl;
            }
            break;
        }
        bytes_sent += sent;
    }
}

// 默认路由处理
void HttpServer::defaultHandler(const HttpRequest& request, HttpResponse& response) {
    // 检查是否请求的是静态文件
    std::string file_path = "./www" + request.path;
    if (request.path == "/") {
        file_path += "index.html";
    }

    std::ifstream file(file_path, std::ios::binary);
    if (file.good()) {
        // 读取文件内容
        std::stringstream buffer;
        buffer << file.rdbuf();
        response.body = buffer.str();
        response.status_code = 200;
        response.status_message = "OK";

        // 根据文件扩展名设置Content-Type
        std::string extension = file_path.substr(file_path.find_last_of(".") + 1);
        if (extension == "html" || extension == "htm") {
            response.headers["Content-Type"] = "text/html; charset=utf-8";
        } else if (extension == "css") {
            response.headers["Content-Type"] = "text/css";
        } else if (extension == "js") {
            response.headers["Content-Type"] = "application/javascript";
        } else if (extension == "jpg" || extension == "jpeg") {
            response.headers["Content-Type"] = "image/jpeg";
        } else if (extension == "png") {
            response.headers["Content-Type"] = "image/png";
        } else if (extension == "gif") {
            response.headers["Content-Type"] = "image/gif";
        } else {
            response.headers["Content-Type"] = "application/octet-stream";
        }
    } else {
        // 文件不存在，返回404
        response.status_code = 404;
        response.status_message = "Not Found";
        response.body = "<html><body><h1>404 Not Found</h1></body></html>";
    }
}

// 显式实例化模板函数
template auto ThreadPool::enqueue<void(HttpServer::*)(int), HttpServer*, int>(void(HttpServer::*&&)(int), HttpServer*&&, int&&) -> std::future<void>;