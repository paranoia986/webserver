# webserver

---

## Intro

This webserver is a simple webserver which is implemented in c++ and use cmake and conan as build tools.
In addition,it is modified from the tiny-web-server which is also a opensource project.

This project wants to be inited , tested , pacakage managed and built by conan and cmake.

## Principles

1. should print the realtime information for the each mod and step being executed.

## Mods

1. mysql
2. http
3. https (to be implemented)
4. lock
5. threadpool
6. log
7. root(root resources)
8. timer
9. webserver
10. config parser(to be implemented)

## plans to be done

1. add test for each mod
3. add the different functions for mysql
   .etc

## command

1. generate
1.1 切换参数并准备构建(necessary)
```bash
conan install . -o io_uring=True -s build_type=Debug --build=missing
```
* -o io_uring=True：确保 Conan 将 IO_URING 变量设置为 ON 并写入工具链。

1.2 切换到 epoll 回退模式
```bash
conan install . -o io_uring=False -s build_type=Debug --build=missing
```
* 此时 Conan 会修改配置，使 CMakeLists.txt 中的 IO_URING 宏变为 0。

2. build
```bash
conan build . -s build_type=Debug
```
* 这个指令会自动调用 cmake，并根据你刚才 install 时选择的参数进行编译。

* 它会自动找到 build/Debug/generators 目录下的工具链。
