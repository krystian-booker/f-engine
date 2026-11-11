#pragma once
// Included by MiniAudio.cpp after miniaudio.h: a miniaudio virtual file
// system that reads the engine's files where they are.
//
// miniaudio opens sounds by name. A game shipped as a .pak has no files with
// those names, and the engine used to extract each track to a temporary file
// on first play, on the game thread, before the call returned: a hitch at
// every change of music. With this VFS nothing is extracted:
//   * an entry stored as it is (every audio format: the packer does not
//     compress them) is read as a window of the archive file, so music
//     streams from the .pak exactly as from a loose file;
//   * a compressed entry (an old or hand-made archive) is inflated into
//     memory when it is opened;
//   * anything else goes to miniaudio's own file access.
// Sounds are started with MA_SOUND_FLAG_ASYNC, so the open itself runs on
// miniaudio's job thread; each open file has its own stream and state, and
// the engine's path functions are thread safe.
#include "engine/core/Paths.h"
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

namespace fe {
struct PackVfs {
    ma_vfs_callbacks callbacks{}; // first: miniaudio casts an ma_vfs* to this
    ma_default_vfs fallback{};

    struct File {
        enum class Kind { Loose, Window, Memory } kind = Kind::Loose;
        ma_vfs_file loose = nullptr;
        std::ifstream stream; // Window: the archive
        std::uint64_t base = 0, size = 0, cursor = 0;
        std::string memory; // Memory: the entry's bytes
    };

    PackVfs() {
        ma_default_vfs_init(&fallback, nullptr);
        callbacks.onOpen = open;
        callbacks.onOpenW = openWide;
        callbacks.onClose = close;
        callbacks.onRead = read;
        callbacks.onWrite = write;
        callbacks.onSeek = seek;
        callbacks.onTell = tell;
        callbacks.onInfo = info;
    }
    PackVfs(const PackVfs&) = delete;
    PackVfs& operator=(const PackVfs&) = delete;
    [[nodiscard]] ma_vfs* handle() { return this; }

private:
    static PackVfs& self(ma_vfs* vfs) { return *static_cast<PackVfs*>(vfs); }
    static File& fileOf(ma_vfs_file file) { return *static_cast<File*>(file); }

    static ma_result open(ma_vfs* vfs, const char* path, ma_uint32 mode, ma_vfs_file* out) {
        if (!path || !out) {
            return MA_INVALID_ARGS;
        }
        auto file = std::make_unique<File>();
        try {
            if (!(mode & MA_OPEN_MODE_WRITE)) {
                if (const auto span = storedSpan(path)) {
                    file->kind = File::Kind::Window;
                    file->stream.open(span->archive, std::ios::binary);
                    if (!file->stream) {
                        return MA_DOES_NOT_EXIST;
                    }
                    file->base = span->offset;
                    file->size = span->size;
                    *out = file.release();
                    return MA_SUCCESS;
                }
                if (isPacked(path)) {
                    file->kind = File::Kind::Memory;
                    file->memory = readFile(path);
                    file->size = file->memory.size();
                    *out = file.release();
                    return MA_SUCCESS;
                }
            }
        } catch (const std::exception&) {
            return MA_DOES_NOT_EXIST;
        }
        const auto result = ma_vfs_open(&self(vfs).fallback, path, mode, &file->loose);
        if (result != MA_SUCCESS) {
            return result;
        }
        *out = file.release();
        return MA_SUCCESS;
    }
    static ma_result openWide(ma_vfs* vfs, const wchar_t* path, ma_uint32 mode, ma_vfs_file* out) {
        if (!path || !out) {
            return MA_INVALID_ARGS;
        }
        auto file = std::make_unique<File>();
        const auto result = ma_vfs_open_w(&self(vfs).fallback, path, mode, &file->loose);
        if (result != MA_SUCCESS) {
            return result;
        }
        *out = file.release();
        return MA_SUCCESS;
    }
    static ma_result close(ma_vfs* vfs, ma_vfs_file handle) {
        const std::unique_ptr<File> file(&fileOf(handle));
        return file->kind == File::Kind::Loose ? ma_vfs_close(&self(vfs).fallback, file->loose) : MA_SUCCESS;
    }
    static ma_result read(ma_vfs* vfs, ma_vfs_file handle, void* out, size_t bytes, size_t* done) {
        auto& file = fileOf(handle);
        if (file.kind == File::Kind::Loose) {
            return ma_vfs_read(&self(vfs).fallback, file.loose, out, bytes, done);
        }
        const auto count = static_cast<size_t>(std::min<std::uint64_t>(bytes, file.size - file.cursor));
        if (file.kind == File::Kind::Memory) {
            std::memcpy(out, file.memory.data() + file.cursor, count);
        } else {
            file.stream.clear();
            file.stream.seekg(static_cast<std::streamoff>(file.base + file.cursor));
            if (!file.stream.read(static_cast<char*>(out), static_cast<std::streamsize>(count))) {
                return MA_IO_ERROR;
            }
        }
        file.cursor += count;
        if (done) {
            *done = count;
        }
        return count == 0 && bytes > 0 ? MA_AT_END : MA_SUCCESS;
    }
    static ma_result write(ma_vfs* vfs, ma_vfs_file handle, const void* in, size_t bytes, size_t* done) {
        auto& file = fileOf(handle);
        return file.kind == File::Kind::Loose ? ma_vfs_write(&self(vfs).fallback, file.loose, in, bytes, done)
                                              : MA_NOT_IMPLEMENTED;
    }
    static ma_result seek(ma_vfs* vfs, ma_vfs_file handle, ma_int64 offset, ma_seek_origin origin) {
        auto& file = fileOf(handle);
        if (file.kind == File::Kind::Loose) {
            return ma_vfs_seek(&self(vfs).fallback, file.loose, offset, origin);
        }
        const auto from = origin == ma_seek_origin_start     ? ma_int64{0}
                          : origin == ma_seek_origin_current ? static_cast<ma_int64>(file.cursor)
                                                             : static_cast<ma_int64>(file.size);
        const auto target = from + offset;
        if (target < 0 || static_cast<std::uint64_t>(target) > file.size) {
            return MA_BAD_SEEK;
        }
        file.cursor = static_cast<std::uint64_t>(target);
        return MA_SUCCESS;
    }
    static ma_result tell(ma_vfs* vfs, ma_vfs_file handle, ma_int64* cursor) {
        auto& file = fileOf(handle);
        if (file.kind == File::Kind::Loose) {
            return ma_vfs_tell(&self(vfs).fallback, file.loose, cursor);
        }
        *cursor = static_cast<ma_int64>(file.cursor);
        return MA_SUCCESS;
    }
    static ma_result info(ma_vfs* vfs, ma_vfs_file handle, ma_file_info* out) {
        auto& file = fileOf(handle);
        if (file.kind == File::Kind::Loose) {
            return ma_vfs_info(&self(vfs).fallback, file.loose, out);
        }
        out->sizeInBytes = file.size;
        return MA_SUCCESS;
    }
};
} // namespace fe
