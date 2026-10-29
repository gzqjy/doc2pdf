#ifndef POLE_H
#define POLE_H

#include <string>
#include <vector>
#include <list>

namespace POLE {

class StorageIO;

class Storage {
    friend class Stream;
public:
    Storage(const char* filename);
    ~Storage();

    bool open();
    void close();
    bool is_valid() const;
    std::list<std::string> entries(const std::string& path = "/") const;
    bool is_directory(const std::string& name) const;
    bool exists(const std::string& name) const;

private:
    StorageIO* io;
    Storage(const Storage&);
    Storage& operator=(const Storage&);
};

class Stream {
public:
    Stream(Storage* storage, const std::string& name);
    ~Stream();

    unsigned long size() const;
    unsigned long read(unsigned char* data, unsigned long maxlen);
    unsigned long tell() const;
    void seek(unsigned long pos);
    bool fail() const;

private:
    StorageIO* io;
    unsigned long stream_size;
    unsigned long stream_pos;
    std::vector<unsigned long> blocks;
    bool is_mini;
    bool m_fail;

    Stream(const Stream&);
    Stream& operator=(const Stream&);
};

} // namespace POLE

#endif // POLE_H
