#include <iostream>
#include <fstream>
#include <cstring>
#include <cerrno>
#include <memory>
#include <utility>
#include <climits>

using namespace std;

struct Fat32Extend {
    /* DOS2.0 */
    int16_t bytesPerSector;
    int8_t sectorsPerCluster;
    int16_t reservedSectors;
    int8_t allocationTables;
    int16_t maxNumOfRoots;
    int16_t totalLogicalSectors;
    int8_t media;
    int16_t sectorsPerFileAllocationTable;
    /* DOS3.31 */
    int16_t sectorsPerTrack;
    int16_t numOfHeads;
    int32_t hiddenSectors;
    int32_t totalLogicalSectors2;
    /* FAT32 Extended BIOS Parameter Block */
    int32_t sectorsPerFileAllocationTable2;
    int16_t driveDescription;
    int8_t versionLow;
    int8_t versionHigh;
    int32_t rootCluster;
    int16_t fsInformationSector;
    int16_t firstLogicalSector;
    int8_t reserved[12];
    int8_t physicalDriveNumber;
    int8_t reserved2;
    int8_t extendedBootSignature;
    int32_t volumeID;
    int8_t volumeLabel[11];
    int8_t fsType[8];
} __attribute__((packed));

struct BootSector {
    int8_t jump[3];
    int8_t name[8];
    union {
        Fat32Extend fat32;
    } extend;
} __attribute__((packed));

struct FSInformationSector {
    int32_t sig1;
    int8_t reserved[480];
    int32_t sig2;
    uint32_t freeClusters;
    uint32_t nextCluster;
    int8_t reserved2[12];
    int32_t sig3;
} __attribute__((packed));

template<typename T>
string toHex(T value) {
    char buf[16];
    int width = sizeof(T) * 2;
    int mask = (2 << (width * 4 - 1)) - 1;
    sprintf(buf, "%0*x", width, value & mask);
    return string(buf);
}

template<typename T>
string toString(T value) {
    T *addr = &value;
    return string(reinterpret_cast<char *>(addr), sizeof(T));
}

struct ShortDirEntry {
    int8_t fileName[8];
    int8_t extendName[3];
    int8_t attr;
    int8_t reserved;
    int8_t createTimeMs;
    int16_t createTime;
    int16_t createDate;
    int16_t accessDate;
    int16_t startClusterHigh;
    int16_t modifyTime;
    int16_t modifyDate;
    int16_t startClusterLow;
    int32_t length;
};


class FileAllocationTable {
public:
    class Cluster {
    private:
        const FileAllocationTable& fat;
        uint32_t startSector;
    public:
        Cluster(const FileAllocationTable& fs, uint32_t cluster):fat(fs) {
            int sectorsPerFAT = fat.fs.sectorsPerFileAllocationTable;
            if (sectorsPerFAT == 0)
                sectorsPerFAT = fat.fs.sectorsPerFileAllocationTable2;
            // 目录起始扇区 = 保留扇区数 +  隐藏扇区数+ 一个FAT的扇区数 × FAT表个数 + (起始簇号-2) x 每簇的扇区数
            startSector = fat.fs.reservedSectors + fat.fs.hiddenSectors + sectorsPerFAT * fat.fs.allocationTables + (cluster - 2) * fat.fs.sectorsPerCluster;
        }
        void read(char *buffer, long size = -1) {
            if (size == -1) {
                size = fat.clusterBytes();
            }
            if (size > fat.clusterBytes())
                size = fat.clusterBytes();
            fat.readSector(buffer, startSector, size);
        }
        uint32_t getSector() {
            return startSector;
        }
    };

    class iterator {
    public:
        friend FileAllocationTable;
        uint32_t current;
        const FileAllocationTable& table;
        iterator(const FileAllocationTable& table, uint32_t current): table(table), current(current) {

        }
        iterator& operator++() {
            if (current != 0x0fffffff)
                current = table.table[current];
            return *this;
        }
        bool operator==(const iterator& ano) const {
            return current == ano.current;
        }
        Cluster operator*() {
            return Cluster{table, current};
        }
    };

    class File {
    public:
        class DirEntryIterator {
        private:

            File &file;
            long fileCursor;
            ShortDirEntry *entries = nullptr;
            int bytesPerSector;
            int filesPerSector;
            int currentMaxFileNum = 0;
            int nextFileOffset = 0;
            void loadFile() {
                file.read(reinterpret_cast<char *>(entries), nextFileOffset, bytesPerSector);
                nextFileOffset += bytesPerSector;
                currentMaxFileNum += filesPerSector;
            }
        public:
            DirEntryIterator(File& file, long fileCursor=0): fileCursor(fileCursor), file(file) {
                bytesPerSector = file.fat.fs.bytesPerSector;
                filesPerSector = bytesPerSector / sizeof(ShortDirEntry);
                if (fileCursor == -1) {
                    this->fileCursor = -1;
                } else {
                    entries = new ShortDirEntry[filesPerSector];
                }
            }

            ~DirEntryIterator() {
                delete[] entries;
            }

            bool operator!=(const DirEntryIterator& ano) {
                if (ano.fileCursor == -1) {
                    return this->operator*().fileName[0] != 0;
                }
                return fileCursor != ano.fileCursor;
            }

            DirEntryIterator& operator++() {
                fileCursor += 1;
                if (currentMaxFileNum == fileCursor) {
                    loadFile();
                }
                return *this;
            }

            ShortDirEntry operator*() {
                if (fileCursor == currentMaxFileNum) {
                    loadFile();
                }
                return entries[fileCursor % filesPerSector];
            }
        };

        ShortDirEntry entry;
    private:
        const FileAllocationTable& fat;
    public:
        File(const FileAllocationTable& fat, ShortDirEntry entry)
                : fat(fat), entry(entry) {
        }

        uint32_t size() const {
            return entry.length;
        }

        bool isDir() const {
            return entry.attr & 0x10;
        }

        DirEntryIterator begin() {
            return DirEntryIterator(*this);
        }

        DirEntryIterator end() {
            return DirEntryIterator(*this, -1);
        }

        void read(char *buffer, long start, long length) {
            auto clusterBytes = fat.clusterBytes();
            long cursor = 0;
            uint32_t startCluster = (entry.startClusterHigh << 16) + entry.startClusterLow;
            auto iter = fat.get(startCluster);
            startCluster = start / clusterBytes;
            while (startCluster) {
                ++iter;
                startCluster--;
            }
            uint32_t startBytes = start % clusterBytes;
            char temp[clusterBytes];
            length = min(length, isDir()?LONG_MAX:(long)size());
            while (cursor < length) {
                if (startBytes != 0) {
                    long remainSize = clusterBytes - startBytes;
                    (*iter).read(temp, remainSize);
                    memcpy(buffer, temp + startBytes, remainSize);
                    buffer += remainSize;
                    startBytes = 0;
                    cursor += remainSize;
                    buffer += remainSize;
                } else {
                    uint32_t toRead = min(length - cursor, (long)clusterBytes);
                    (*iter).read(buffer, toRead);
                    cursor += toRead;
                    buffer += toRead;
                }
                ++iter;
            }
        }

    };

public:
    uint32_t tableSize;
    uint32_t* table;
    BootSector bootSector{};
    Fat32Extend &fs = bootSector.extend.fat32;
    ifstream &disk;
public:
    FileAllocationTable(ifstream& disk): disk(disk){
        if (!disk.is_open()) {
            cerr << strerror(errno) << endl;
            exit(1);
        }
        disk.read(reinterpret_cast<char *>(&bootSector), sizeof(bootSector));
        cout << "Boot Sector" << endl;
        cout << "name: " << string(reinterpret_cast<const char *>(bootSector.name), 8) << endl;
        cout << "bytes per sector: " << bootSector.extend.fat32.bytesPerSector << endl;
        cout << "sectors per cluster: " << (int) bootSector.extend.fat32.sectorsPerCluster << endl;
        cout << "reserved sectors: " << bootSector.extend.fat32.reservedSectors << endl;
        cout << "allocation tables: " << (int) bootSector.extend.fat32.allocationTables << endl;
        cout << "media type: " << toHex(bootSector.extend.fat32.media) << endl;
        cout << "hidden sectors: " << bootSector.extend.fat32.hiddenSectors << endl;
        int localSectors = bootSector.extend.fat32.totalLogicalSectors;
        if (localSectors == 0)
            localSectors = bootSector.extend.fat32.totalLogicalSectors2;
        int sectorsPerFAT = bootSector.extend.fat32.sectorsPerFileAllocationTable;
        if (sectorsPerFAT == 0)
            sectorsPerFAT = bootSector.extend.fat32.sectorsPerFileAllocationTable2;
        cout << "sectors pre FAT: " << sectorsPerFAT << endl;
        cout << "total sectors: " << localSectors << " (" << (localSectors >> 1) << "K)" << endl;
        cout << "version: " << (int) bootSector.extend.fat32.versionHigh << "." << (int) bootSector.extend.fat32.versionLow
             << endl;
        cout << "root cluster: " << bootSector.extend.fat32.rootCluster << endl;
        cout << "FS information sector: " << bootSector.extend.fat32.fsInformationSector << endl;
        cout << "first logical sector: " << bootSector.extend.fat32.firstLogicalSector << endl;
        cout << "filesystem type: " << string(reinterpret_cast<const char *>(bootSector.extend.fat32.fsType), 8) << endl;

        // Fs information sector
        disk.seekg(bootSector.extend.fat32.fsInformationSector * bootSector.extend.fat32.bytesPerSector);
        FSInformationSector informationSector{};
        disk.read(reinterpret_cast<char *>(&informationSector), sizeof(FSInformationSector));
        cout << "FS Information Sector" << endl;
        cout << "signature 1: " << toString(informationSector.sig1) << endl;
        cout << "signature 2: " << toString(informationSector.sig2) << endl;
        cout << "free clusters: " << informationSector.freeClusters << "(0x" << toHex(informationSector.freeClusters) << ")"
             << endl;
        cout << "next cluster: " << informationSector.nextCluster << "(0x" << toHex(informationSector.nextCluster) << ")"
             << endl;
        cout << "signature 3: " << toHex(informationSector.sig3) << endl;

        tableSize = sectorsPerFAT * fs.bytesPerSector / 4;
        disk.seekg(fs.reservedSectors * fs.bytesPerSector);
        table = new uint32_t[tableSize];
        disk.read(reinterpret_cast<char *>(table), tableSize * 4);
    }

    ~FileAllocationTable() {
        delete[] table;
    }

    void readSector(char *buf, long sector, long size) const {
        disk.seekg(sector * fs.bytesPerSector);
        disk.read(buf, min(fs.bytesPerSector, (int16_t)size));
    }

    iterator get(uint32_t cluster) const {
        return iterator{*this, cluster};
    }

    uint32_t clusterBytes() const {
        return fs.sectorsPerCluster * fs.bytesPerSector;
    }


    File root() {
        int32_t rootCluster = fs.rootCluster;
        int rootClusterCount = 1;
        do {
            rootClusterCount++;
            rootCluster = table[rootCluster];
        } while (rootCluster <= 0x0FFFFFEF );
        ShortDirEntry rootEntry {
                .attr = 0x10,
                .startClusterHigh = static_cast<int16_t>(fs.rootCluster >> 16),
                .startClusterLow = static_cast<int16_t>(fs.rootCluster & 0xFFFF),
                .length = rootClusterCount * fs.bytesPerSector * fs.sectorsPerCluster
        };
        rootEntry.fileName[0] = '/';
        rootEntry.fileName[1] = 0;
        rootEntry.extendName[0] = 0;
        return File{*this, rootEntry};
    }

    int maxCluster() {
        int maxCluster = 2;
        for (int i = 2; i < tableSize; i++) {
            if (table[i] <= 0x0FFFFFEF) {
                maxCluster = max((uint32_t)maxCluster, table[i]);
            }
        }
        return maxCluster;
    }
};

int files=0;
uint32_t maxClusterF=0;
void walk(FileAllocationTable& fs, FileAllocationTable::File& root, int level=0) {
    for(auto iter = root.begin(); iter != root.end(); ++iter) {
        ShortDirEntry entry = *iter;
        if (!isprint(entry.fileName[0]) || entry.attr == 0x0F) {
            continue;
        }
        uint32_t startCluster = (entry.startClusterHigh << 16) + entry.startClusterLow;
        while (startCluster>2 && startCluster != 0x0FFFFFFF) {
            maxClusterF = max(maxClusterF, startCluster);
            startCluster = fs.table[startCluster];
        }
        FileAllocationTable::File next{fs, entry};
        for (int i = 0; i < level; i++) {
            cout <<"=";
        }
        cout << string(reinterpret_cast<const char *>(entry.fileName), 8) << "." << string(reinterpret_cast<const char *>(entry.extendName), 3) << endl;
        if (entry.fileName[0] == '.') {
            if (entry.fileName[1] == '.') {
                if (entry.fileName[2] == ' ') {
                    continue;
                }
            } else if (entry.fileName[1] == ' ') {
                continue;
            }
        }
        files++;
        if (next.isDir()) {
            walk(fs, next, level+1);
        }
    }
}

int main(int args, char **argv) {
    if (args == 1) {
        exit(1);
    }

    ifstream disk(argv[1], ios::binary);
    FileAllocationTable fs(disk);
    auto root = fs.root();
    walk(fs, root);
    uint32_t maxCluster = fs.maxCluster();
    uint32_t maxSector = (*fs.get(maxCluster)).getSector();
    cout << "Max cluster: " << maxCluster << endl;
    cout << "Max clusterF: " << maxClusterF << endl;
    maxCluster = max(maxCluster, maxClusterF);
    cout << "End size: " << (maxSector + 1) * fs.fs.bytesPerSector << endl;
    cout << "Files: " << files << endl;
    return 0;
}
