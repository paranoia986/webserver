#ifndef PATH_UTIL_H
#define PATH_UTIL_H

#include <unistd.h>
#include <limits.h>
#include <string>
#include <cstdlib>
#include <cstdio>

inline std::string get_executable_path() {
    char exe_path[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", exe_path, PATH_MAX);
    
    if (count > 0) {
        exe_path[count] = '\0';
    } else {
        perror("Fatal Error: 无法获取可执行文件绝对路径 (readlink failed)");
        exit(1); 
    }

    std::string full_path(exe_path);
    size_t last_slash = full_path.find_last_of('/');
    return full_path.substr(0, last_slash);
}

#endif // PATH_UTIL_H