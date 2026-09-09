/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

/* A small AVI writer for the ports that have no Video for Windows: one
 * uncompressed 24-bit bottom-up RGB video stream and an optional 16-bit PCM
 * audio stream.
 *
 * The file is written as OpenDML ("AVI 2.0"): the first RIFF "AVI " segment
 * carries the headers, a legacy idx1 index and up to 1 GB of data, and the
 * recording then continues in RIFF "AVIX" segments, each with its own
 * standard index that the super index in the header points at. That keeps a
 * long uncompressed recording in a single file rather than the 2 GB splits the
 * win32 VfW writer makes. Every common player and ffmpeg read this layout. */
class AVIWriter
{
  public:
    AVIWriter() = default;
    ~AVIWriter();
    AVIWriter(const AVIWriter &) = delete;
    AVIWriter &operator=(const AVIWriter &) = delete;

    /* fps = fps_rate / fps_scale. audio_rate of 0 records no audio stream. */
    bool open(const std::string &filename, int width, int height,
              uint32_t fps_rate, uint32_t fps_scale,
              int audio_rate = 0, int audio_channels = 2);

    /* Exactly frameSize() bytes: pitch()-byte rows, bottom row first, BGR. */
    void addVideoFrame(const uint8_t *bgr_bottom_up);
    /* Interleaved 16-bit samples; `frames` sample frames (one per channel). */
    void addAudio(const int16_t *samples, int frames);

    void close();

    bool isOpen() const { return file != nullptr; }
    /* A write failed (disk full, ...); nothing more is written. */
    bool failed() const { return write_failed; }

    int width() const { return video_width; }
    int height() const { return video_height; }
    int pitch() const { return video_pitch; }
    int frameSize() const { return video_pitch * video_height; }
    uint32_t videoFrames() const { return video.total_length; }
    uint64_t audioFrames() const { return audio.total_length; }
    uint64_t bytesWritten() const { return file_size; }

  private:
    struct IndexEntry
    {
        uint32_t offset; // of the chunk data, relative to the movi list
        uint32_t size;   // of the chunk data
    };

    struct Stream
    {
        uint32_t chunk_id = 0;
        int64_t strh_length_pos = 0;
        int64_t super_index_pos = 0;
        uint32_t super_entries = 0;
        std::vector<IndexEntry> segment_entries;
        uint64_t segment_length = 0; // frames or sample frames in this segment
        uint64_t total_length = 0;
    };

    void write(const void *data, size_t size);
    void write8(uint8_t v);
    void write16(uint16_t v);
    void write32(uint32_t v);
    void write64(uint64_t v);
    void writeFourCC(const char *fourcc);
    void patch(int64_t pos, const void *data, size_t size);
    void patch32(int64_t pos, uint32_t v);
    int64_t tell();
    void seek(int64_t pos);

    int64_t beginList(const char *type);
    int64_t beginChunk(const char *id);
    void endChunk(int64_t size_pos);

    void writeHeaders();
    void writeSuperIndex(Stream &stream);
    void writeStandardIndex(Stream &stream);
    void writeLegacyIndex();
    void startSegment();
    void endSegment();
    uint64_t segmentReserve() const;
    void writeChunk(Stream &stream, const void *data, uint32_t size, uint64_t length);

    FILE *file = nullptr;
    bool write_failed = false;
    uint64_t file_size = 0;

    int video_width = 0;
    int video_height = 0;
    int video_pitch = 0;
    uint32_t video_rate = 0;
    uint32_t video_scale = 0;

    int audio_sample_rate = 0;
    int audio_channels = 0;
    bool has_audio = false;

    Stream video;
    Stream audio;

    int segment_count = 0;
    int64_t riff_start = 0;
    int64_t movi_size_pos = 0;
    int64_t movi_pos = 0; // position of the "movi" FOURCC, the index base
    int64_t avih_total_frames_pos = 0;
    int64_t dmlh_total_frames_pos = 0;
    uint32_t first_segment_frames = 0;

    struct LegacyEntry
    {
        uint32_t chunk_id;
        uint32_t offset; // of the chunk header, relative to the movi list
        uint32_t size;
    };
    std::vector<LegacyEntry> legacy_entries;
};
