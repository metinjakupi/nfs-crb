// A read-only HTTP game view; saves and verified startup modules remain in OPFS.
// Called on the game worker before boot_load, outside the scheduler baton.
#pragma once

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

static bool web_stream_prepare(std::string &exe) {
    const char *base = getenv("RECOMP_WEB_ASSET_BASE");
    const char *index = getenv("RECOMP_WEB_ASSET_INDEX");
    if (!base || !index)
        return true; // Existing full imports continue to use OPFS.
    std::ifstream input(index);
    if (!input)
        return false;
    backend_t backend = wasmfs_create_fetch_backend(base, 1024 * 1024);
    if (!backend || wasmfs_create_directory("/stream", 0777, backend) != 0)
        return false;
    const std::string local = exe.substr(0, exe.find_last_of('/'));
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() < 3 || line[1] != '\t')
            return false;
        const bool pinned = line[0] == 'L';
        const std::string relative = line.substr(2);
        if (relative.empty() || relative[0] == '/' || relative.find('\\') != std::string::npos)
            return false;
        size_t from = 0;
        while (from < relative.size()) {
            size_t end = relative.find('/', from);
            const std::string part = relative.substr(from, end - from);
            if (part.empty() || part == "." || part == "..")
                return false;
            if (end == std::string::npos)
                break;
            const std::string dir = "/stream/" + relative.substr(0, end);
            if (mkdir(dir.c_str(), 0777) && errno != EEXIST)
                return false;
            from = end + 1;
        }
        const std::string path = "/stream/" + relative;
        if (pinned) {
            if (symlink((local + "/" + relative).c_str(), path.c_str()))
                return false;
        } else {
            // Inherit the mounted fetch directory's backend so its relative URL is retained.
            int fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDONLY, 0444);
            if (fd < 0)
                return false;
            close(fd);
        }
    }
    exe = "/stream/" + exe.substr(exe.find_last_of('/') + 1);
    return true;
}
