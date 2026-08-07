/**
 * @file test_camera.cpp
 * @brief Camera Daemon Integration Test
 */

#include <iostream>
#include <fstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <json/json.h>

struct SHMHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t buffer_count;
    uint32_t buffer_size;
    uint32_t write_index;
    uint32_t read_index;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t fps;
};

bool test_shm_metadata() {
    std::cout << "[Test] Checking SHM metadata file..." << std::endl;
    
    std::ifstream file("/run/aipc/shm/cam0_main.raw.meta");
    if (!file.is_open()) {
        std::cerr << "✗ Metadata file not found" << std::endl;
        return false;
    }
    
    // Parse JSON (simplified, real test would use JSON library)
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    
    if (content.find("\"width\"") == std::string::npos) {
        std::cerr << "✗ Metadata missing width" << std::endl;
        return false;
    }
    
    std::cout << "✓ Metadata file OK" << std::endl;
    return true;
}

bool test_shm_header() {
    std::cout << "[Test] Checking SHM header..." << std::endl;
    
    int fd = open("/run/aipc/shm/cam0_main.raw", O_RDONLY);
    if (fd < 0) {
        std::cerr << "✗ Cannot open SHM file" << std::endl;
        return false;
    }
    
    // Map header only
    void* addr = mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        std::cerr << "✗ Cannot mmap SHM" << std::endl;
        close(fd);
        return false;
    }
    
    SHMHeader* header = (SHMHeader*)addr;
    
    // Validate magic
    if (header->magic != 0x41495043) {  // 'AIPC'
        std::cerr << "✗ Invalid magic: 0x" << std::hex << header->magic << std::endl;
        munmap(addr, 4096);
        close(fd);
        return false;
    }
    
    std::cout << "✓ SHM header valid" << std::endl;
    std::cout << "  Magic: 0x" << std::hex << header->magic << std::dec << std::endl;
    std::cout << "  Version: " << header->version << std::endl;
    std::cout << "  Buffer count: " << header->buffer_count << std::endl;
    std::cout << "  Resolution: " << header->width << "x" << header->height << std::endl;
    std::cout << "  FPS: " << header->fps << std::endl;
    std::cout << "  Write index: " << header->write_index << std::endl;
    
    munmap(addr, 4096);
    close(fd);
    
    return true;
}

bool test_frame_data() {
    std::cout << "[Test] Checking frame data..." << std::endl;
    
    int fd = open("/run/aipc/shm/cam0_main.raw", O_RDONLY);
    if (fd < 0) {
        std::cerr << "✗ Cannot open SHM file" << std::endl;
        return false;
    }
    
    // Get file size
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return false;
    }
    
    // Map entire file
    void* addr = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        return false;
    }
    
    SHMHeader* header = (SHMHeader*)addr;
    
    if (header->write_index == 0) {
        std::cout << "⚠ No frames written yet" << std::endl;
        munmap(addr, st.st_size);
        close(fd);
        return true;
    }
    
    std::cout << "✓ Frames available: " << header->write_index << std::endl;
    
    // Read latest frame metadata
    uint32_t idx = (header->write_index - 1) % header->buffer_count;
    uint8_t* meta_region = (uint8_t*)addr + 4096;
    
    uint64_t* sequence = (uint64_t*)(meta_region + idx * 64);
    uint64_t* timestamp = (uint64_t*)(meta_region + idx * 64 + 8);
    
    std::cout << "  Latest frame sequence: " << *sequence << std::endl;
    std::cout << "  Latest frame timestamp: " << *timestamp << " ns" << std::endl;
    
    munmap(addr, st.st_size);
    close(fd);
    
    return true;
}

int main() {
    std::cout << "===================================" << std::endl;
    std::cout << "AIPC Camera Daemon Integration Test" << std::endl;
    std::cout << "===================================" << std::endl;
    std::cout << std::endl;
    
    bool all_passed = true;
    
    // Run tests
    all_passed &= test_shm_metadata();
    all_passed &= test_shm_header();
    all_passed &= test_frame_data();
    
    std::cout << std::endl;
    std::cout << "===================================" << std::endl;
    
    if (all_passed) {
        std::cout << "✓ All tests passed!" << std::endl;
        std::cout << "===================================" << std::endl;
        return 0;
    } else {
        std::cout << "✗ Some tests failed" << std::endl;
        std::cout << "===================================" << std::endl;
        return 1;
    }
}

