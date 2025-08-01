#include <iostream>
#include <string>
#include "server.h"

int main(int argc, char* argv[]) {
    // 默认端口号
    int port = 8080;

    // 从命令行参数获取端口号
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    try {
        // 创建HTTP服务器实例
        HttpServer server(port);
        std::cout << "服务器启动成功，监听端口: " << port << std::endl;
        std::cout << "按Ctrl+C停止服务器..." << std::endl;

        // 启动服务器
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "服务器启动失败: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}