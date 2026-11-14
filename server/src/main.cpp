#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "devcard/server.h"

int main(int argc, char** argv) {
  devcard::ServerOptions options;
  options.data_dir = devcard::default_data_dir();

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      options.port = std::stoi(argv[++i]);
    } else if (arg == "--data-dir" && i + 1 < argc) {
      options.data_dir = argv[++i];
    } else if (arg == "--help") {
      std::cout << "devcard-server [--port N] [--data-dir PATH]\n";
      return 0;
    }
  }

  try {
    auto server = devcard::start_server(options);
    std::cout << "devcard-server listening http://" << options.host << ":"
              << server->port() << " data=" << options.data_dir << std::endl;
    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
  } catch (const std::exception& error) {
    std::cerr << "启动失败: " << error.what() << std::endl;
    return 1;
  }
  return 0;
}
