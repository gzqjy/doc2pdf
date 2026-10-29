#include "doc2pdf/pole.h"

#include <fstream>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cstdint>

namespace POLE {

struct DirEntry {
    std::string name;
    uint8_t type;
    uint32_t start_sec;
    uint64_t size;
    int32_t left_child;
    int32_t right_child;
    int32_t dir_child;
};

class StorageIO {
public:
    std::string filename;
    std::ifstream file;
    uint32_t b_shift;
    uint32_t s_shift;
    uint32_t num_bat;
    uint32_t dir_first;
    uint32_t mini_cutoff;
    uint32_t mini_bat_first;
    uint32_t num_mini_bat;
    uint32_t difat_first;
    uint32_t num_difat;

    uint32_t sector_size;
    uint32_t mini_sector_size;

    std::vector<uint32_t> bbat;
    std::vector<uint32_t> sbat;
    std::vector<DirEntry> entries;
    std::vector<uint8_t> sb_data; // Mini-stream data

    StorageIO(const char* fn) : filename(fn), b_shift(9), s_shift(6) {}

    bool load() {
        file.open(filename.c_str(), std::ios::binary);
        if (!file.is_open()) return false;

        file.seekg(0, std::ios::end);
        uint64_t total_size = file.tellg();
        file.seekg(0, std::ios::beg);
        if (total_size < 512) return false;

        uint8_t header[512];
        file.read((char*)header, 512);

        static const uint8_t magic[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
        if (std::memcmp(header, magic, 8) != 0) return false;

        b_shift = *(uint16_t*)&header[30];
        s_shift = *(uint16_t*)&header[32];
        num_bat = *(uint32_t*)&header[44];
        dir_first = *(uint32_t*)&header[48];
        mini_cutoff = *(uint32_t*)&header[56];
        mini_bat_first = *(uint32_t*)&header[60];
        num_mini_bat = *(uint32_t*)&header[64];
        difat_first = *(uint32_t*)&header[68];
        num_difat = *(uint32_t*)&header[72];

        sector_size = 1U << b_shift;
        mini_sector_size = 1U << s_shift;

        // 1. 读取 BBAT (Big Block Allocation Table)
        std::vector<uint32_t> bat_sectors;
        bat_sectors.reserve(num_bat);
        for (int i = 0; i < 109 && bat_sectors.size() < num_bat; ++i) {
            uint32_t s = *(uint32_t*)&header[76 + i * 4];
            if (s < 0xFFFFFFFE) bat_sectors.push_back(s);
        }

        // 读取 DIFAT 链表（支持 >6.8MB 的超大文件）
        uint32_t d_curr = difat_first;
        for (uint32_t d = 0; d < num_difat && d_curr < 0xFFFFFFFC; ++d) {
            std::vector<uint8_t> d_sec(sector_size);
            readSector(d_curr, d_sec.data());
            uint32_t ents_per_sec = (sector_size / 4) - 1;
            for (uint32_t k = 0; k < ents_per_sec && bat_sectors.size() < num_bat; ++k) {
                uint32_t s = *(uint32_t*)&d_sec[k * 4];
                if (s < 0xFFFFFFFE) bat_sectors.push_back(s);
            }
            d_curr = *(uint32_t*)&d_sec[ents_per_sec * 4];
        }

        bbat.clear();
        bbat.reserve(bat_sectors.size() * (sector_size / 4));
        for (uint32_t s : bat_sectors) {
            std::vector<uint8_t> sec_buf(sector_size);
            readSector(s, sec_buf.data());
            const uint32_t* p = (const uint32_t*)sec_buf.data();
            for (size_t k = 0; k < sector_size / 4; ++k) {
                bbat.push_back(p[k]);
            }
        }

        // 2. 读取目录项
        std::vector<uint8_t> dir_data = readChain(dir_first, bbat, sector_size);
        entries.clear();
        for (size_t i = 0; i + 128 <= dir_data.size(); i += 128) {
            const uint8_t* p = &dir_data[i];
            uint16_t nameLen = *(uint16_t*)&p[64];
            if (nameLen > 2) {
                std::string name;
                for (size_t c = 0; c < (nameLen - 2) / 2; ++c) {
                    uint16_t w = p[c * 2] | (p[c * 2 + 1] << 8);
                    if (w < 128) name.push_back((char)w);
                    else name.push_back('_');
                }
                uint8_t type = p[66];
                uint32_t sSec = *(uint32_t*)&p[116];
                uint64_t sSize = *(uint64_t*)&p[120];
                int32_t lc = *(int32_t*)&p[68];
                int32_t rc = *(int32_t*)&p[72];
                int32_t dc = *(int32_t*)&p[76];
                entries.push_back({ name, type, sSec, sSize, lc, rc, dc });
            } else {
                entries.push_back({ "", 0, 0, 0, -1, -1, -1 });
            }
        }

        // 3. 读取 SBAT (Small Block Allocation Table) 和 Root 对应的 MiniStream
        if (!entries.empty() && entries[0].type == 5) {
            sb_data = readChain(entries[0].start_sec, bbat, sector_size);
            if (sb_data.size() > entries[0].size) {
                sb_data.resize(entries[0].size);
            }
        }

        std::vector<uint8_t> sbat_data = readChain(mini_bat_first, bbat, sector_size);
        sbat.resize(sbat_data.size() / 4);
        if (!sbat_data.empty()) {
            std::memcpy(sbat.data(), sbat_data.data(), sbat.size() * 4);
        }

        return true;
    }

    void readSector(uint32_t sec, uint8_t* dest) {
        uint64_t offset = (uint64_t)(sec + 1) * sector_size;
        file.seekg(offset, std::ios::beg);
        file.read((char*)dest, sector_size);
    }

    std::vector<uint8_t> readChain(uint32_t start_sec, const std::vector<uint32_t>& table, uint32_t block_size) {
        std::vector<uint8_t> result;
        uint32_t curr = start_sec;
        while (curr < 0xFFFFFFFC && curr < table.size()) {
            uint64_t offset = (uint64_t)(curr + 1) * block_size;
            file.seekg(offset, std::ios::beg);
            size_t old_size = result.size();
            result.resize(old_size + block_size);
            file.read((char*)&result[old_size], block_size);
            curr = table[curr];
        }
        return result;
    }

    int findEntry(const std::string& name) {
        std::string target = name;
        if (!target.empty() && target[0] == '/') target = target.substr(1);
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].name == target) return (int)i;
        }
        return -1;
    }
};

Storage::Storage(const char* fn) : io(new StorageIO(fn)) {}
Storage::~Storage() { close(); delete io; }

bool Storage::open() { return io->load(); }
void Storage::close() {
    if (io->file.is_open()) io->file.close();
}
bool Storage::is_valid() const { return io->file.is_open() && !io->entries.empty(); }

std::list<std::string> Storage::entries(const std::string& /*path*/) const {
    std::list<std::string> list;
    for (const auto& e : io->entries) {
        if (!e.name.empty()) list.push_back(e.name);
    }
    return list;
}

bool Storage::is_directory(const std::string& name) const {
    int idx = io->findEntry(name);
    return idx >= 0 && (io->entries[idx].type == 1 || io->entries[idx].type == 5);
}

bool Storage::exists(const std::string& name) const {
    return io->findEntry(name) >= 0;
}

Stream::Stream(Storage* storage, const std::string& name)
    : io(storage->io), stream_size(0), stream_pos(0), is_mini(false), m_fail(false) {
    int idx = io->findEntry(name);
    if (idx < 0) {
        m_fail = true;
        return;
    }

    const auto& ent = io->entries[idx];
    stream_size = ent.size;
    is_mini = (stream_size < io->mini_cutoff && ent.type != 5);

    uint32_t curr = ent.start_sec;
    const auto& tbl = is_mini ? io->sbat : io->bbat;
    while (curr < 0xFFFFFFFC && curr < tbl.size()) {
        blocks.push_back(curr);
        curr = tbl[curr];
    }
}

Stream::~Stream() {}

unsigned long Stream::size() const { return stream_size; }
unsigned long Stream::tell() const { return stream_pos; }
bool Stream::fail() const { return m_fail; }

void Stream::seek(unsigned long pos) {
    if (pos > stream_size) stream_pos = stream_size;
    else stream_pos = pos;
}

unsigned long Stream::read(unsigned char* data, unsigned long maxlen) {
    if (m_fail || stream_pos >= stream_size) return 0;
    unsigned long to_read = std::min(maxlen, stream_size - stream_pos);
    unsigned long block_size = is_mini ? io->mini_sector_size : io->sector_size;

    unsigned long bytes_read = 0;
    while (bytes_read < to_read) {
        unsigned long block_idx = stream_pos / block_size;
        unsigned long block_offset = stream_pos % block_size;
        if (block_idx >= blocks.size()) break;

        unsigned long chunk = std::min(to_read - bytes_read, block_size - block_offset);
        if (is_mini) {
            uint64_t mini_off = (uint64_t)blocks[block_idx] * block_size + block_offset;
            if (mini_off + chunk <= io->sb_data.size()) {
                std::memcpy(data + bytes_read, &io->sb_data[mini_off], chunk);
            }
        } else {
            uint64_t file_off = (uint64_t)(blocks[block_idx] + 1) * block_size + block_offset;
            io->file.seekg(file_off, std::ios::beg);
            io->file.read((char*)data + bytes_read, chunk);
        }

        stream_pos += chunk;
        bytes_read += chunk;
    }

    return bytes_read;
}

} // namespace POLE
