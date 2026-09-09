/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "avi_writer.hpp"

#include <cstring>

#ifdef _WIN32
#define avi_fseek _fseeki64
#define avi_ftell _ftelli64
#else
#define avi_fseek fseeko
#define avi_ftell ftello
#endif

namespace
{
/* One RIFF segment stays below this so its 32-bit size field (and the
 * players that stop trusting it past 2 GB) never overflow. Same limit as
 * ffmpeg's AVI muxer. */
constexpr uint64_t kMaxSegmentSize = 0x40000000;
/* Super index entries reserved per stream: one per segment, so this is the
 * longest recording in gigabytes. */
constexpr uint32_t kSuperIndexEntries = 256;

constexpr uint32_t AVIF_HASINDEX = 0x00000010;
constexpr uint32_t AVIF_ISINTERLEAVED = 0x00000100;
constexpr uint32_t AVIF_TRUSTCKTYPE = 0x00000800;
constexpr uint32_t AVIIF_KEYFRAME = 0x00000010;

constexpr uint8_t AVI_INDEX_OF_INDEXES = 0x00;
constexpr uint8_t AVI_INDEX_OF_CHUNKS = 0x01;

uint32_t fourcc(const char *s)
{
    return (uint32_t)(uint8_t)s[0] | ((uint32_t)(uint8_t)s[1] << 8) |
           ((uint32_t)(uint8_t)s[2] << 16) | ((uint32_t)(uint8_t)s[3] << 24);
}
} // namespace

AVIWriter::~AVIWriter()
{
    close();
}

void AVIWriter::write(const void *data, size_t size)
{
    if (!file || write_failed || size == 0)
        return;
    if (fwrite(data, 1, size, file) != size)
        write_failed = true;
    file_size += size;
}

void AVIWriter::write8(uint8_t v)
{
    write(&v, 1);
}

void AVIWriter::write16(uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    write(b, 2);
}

void AVIWriter::write32(uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    write(b, 4);
}

void AVIWriter::write64(uint64_t v)
{
    write32((uint32_t)v);
    write32((uint32_t)(v >> 32));
}

void AVIWriter::writeFourCC(const char *s)
{
    write(s, 4);
}

int64_t AVIWriter::tell()
{
    return file ? (int64_t)avi_ftell(file) : 0;
}

void AVIWriter::seek(int64_t pos)
{
    if (file && avi_fseek(file, pos, SEEK_SET) != 0)
        write_failed = true;
}

/* Overwrites bytes written earlier; unlike write() this adds nothing to the
 * running size. */
void AVIWriter::patch(int64_t pos, const void *data, size_t size)
{
    if (!file || write_failed)
        return;
    int64_t here = tell();
    seek(pos);
    if (fwrite(data, 1, size, file) != size)
        write_failed = true;
    seek(here);
}

void AVIWriter::patch32(int64_t pos, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    patch(pos, b, 4);
}

int64_t AVIWriter::beginList(const char *type)
{
    writeFourCC("LIST");
    int64_t size_pos = tell();
    write32(0);
    writeFourCC(type);
    return size_pos;
}

int64_t AVIWriter::beginChunk(const char *id)
{
    writeFourCC(id);
    int64_t size_pos = tell();
    write32(0);
    return size_pos;
}

/* Fills in the size of a chunk or list begun above and pads it to an even
 * length, which RIFF requires (the size field excludes the pad byte). */
void AVIWriter::endChunk(int64_t size_pos)
{
    int64_t end = tell();
    uint32_t size = (uint32_t)(end - (size_pos + 4));
    patch32(size_pos, size);
    if (size & 1)
        write8(0);
}

bool AVIWriter::open(const std::string &filename, int width, int height,
                     uint32_t fps_rate, uint32_t fps_scale,
                     int audio_rate, int channels)
{
    close();

    if (width <= 0 || height <= 0 || fps_rate == 0 || fps_scale == 0)
        return false;

    file = fopen(filename.c_str(), "wb");
    if (!file)
        return false;
    setvbuf(file, nullptr, _IOFBF, 1 << 20);

    write_failed = false;
    file_size = 0;

    video_width = width;
    video_height = height;
    video_pitch = (width * 3 + 3) & ~3; // DIB rows are 4-byte aligned
    video_rate = fps_rate;
    video_scale = fps_scale;

    has_audio = audio_rate > 0 && channels > 0;
    audio_sample_rate = has_audio ? audio_rate : 0;
    audio_channels = has_audio ? channels : 0;

    video = Stream{};
    video.chunk_id = fourcc("00db");
    audio = Stream{};
    audio.chunk_id = fourcc("01wb");

    segment_count = 0;
    first_segment_frames = 0;
    legacy_entries.clear();

    startSegment();

    if (write_failed)
    {
        fclose(file);
        file = nullptr;
        return false;
    }
    return true;
}

void AVIWriter::writeSuperIndex(Stream &stream)
{
    int64_t size_pos = beginChunk("indx");
    stream.super_index_pos = tell();
    stream.super_entries = 0;
    write16(4);                    // wLongsPerEntry
    write8(0);                     // bIndexSubType
    write8(AVI_INDEX_OF_INDEXES);  // bIndexType
    write32(0);                    // nEntriesInUse, patched per segment
    write32(stream.chunk_id);      // dwChunkId
    write32(0);                    // dwReserved[3]
    write32(0);
    write32(0);
    for (uint32_t i = 0; i < kSuperIndexEntries; i++)
    {
        write64(0); // qwOffset
        write32(0); // dwSize
        write32(0); // dwDuration
    }
    endChunk(size_pos);
}

void AVIWriter::writeHeaders()
{
    const uint32_t block_align = (uint32_t)audio_channels * 2;
    const uint64_t frame_size = (uint64_t)video_pitch * video_height;
    const double fps = (double)video_rate / video_scale;
    uint32_t max_bytes_per_sec = (uint32_t)(frame_size * fps);
    if (has_audio)
        max_bytes_per_sec += (uint32_t)audio_sample_rate * block_align;

    int64_t hdrl_pos = beginList("hdrl");

    // MainAVIHeader
    int64_t avih_pos = beginChunk("avih");
    write32((uint32_t)(1000000.0 * video_scale / video_rate)); // dwMicroSecPerFrame
    write32(max_bytes_per_sec);                                // dwMaxBytesPerSec
    write32(0);                                                // dwPaddingGranularity
    write32(AVIF_HASINDEX | AVIF_ISINTERLEAVED | AVIF_TRUSTCKTYPE); // dwFlags
    avih_total_frames_pos = tell();
    write32(0);                     // dwTotalFrames: frames in the first segment
    write32(0);                     // dwInitialFrames
    write32(has_audio ? 2 : 1);     // dwStreams
    write32((uint32_t)frame_size);  // dwSuggestedBufferSize
    write32((uint32_t)video_width); // dwWidth
    write32((uint32_t)video_height); // dwHeight
    write32(0);                     // dwReserved[4]
    write32(0);
    write32(0);
    write32(0);
    endChunk(avih_pos);

    // Video stream
    int64_t strl_pos = beginList("strl");
    int64_t strh_pos = beginChunk("strh");
    writeFourCC("vids");             // fccType
    writeFourCC("DIB ");             // fccHandler
    write32(0);                      // dwFlags
    write16(0);                      // wPriority
    write16(0);                      // wLanguage
    write32(0);                      // dwInitialFrames
    write32(video_scale);            // dwScale
    write32(video_rate);             // dwRate
    write32(0);                      // dwStart
    video.strh_length_pos = tell();
    write32(0);                      // dwLength: total frames, patched on close
    write32((uint32_t)frame_size);   // dwSuggestedBufferSize
    write32(0xFFFFFFFF);             // dwQuality
    write32(0);                      // dwSampleSize
    write16(0);                      // rcFrame
    write16(0);
    write16((uint16_t)video_width);
    write16((uint16_t)video_height);
    endChunk(strh_pos);

    int64_t strf_pos = beginChunk("strf");
    write32(40);                       // biSize
    write32((uint32_t)video_width);    // biWidth
    write32((uint32_t)video_height);   // biHeight (positive: bottom-up)
    write16(1);                        // biPlanes
    write16(24);                       // biBitCount
    write32(0);                        // biCompression = BI_RGB
    write32((uint32_t)frame_size);     // biSizeImage
    write32(0);                        // biXPelsPerMeter
    write32(0);                        // biYPelsPerMeter
    write32(0);                        // biClrUsed
    write32(0);                        // biClrImportant
    endChunk(strf_pos);

    writeSuperIndex(video);
    endChunk(strl_pos);

    // Audio stream
    if (has_audio)
    {
        strl_pos = beginList("strl");
        strh_pos = beginChunk("strh");
        writeFourCC("auds");                               // fccType
        write32(0);                                        // fccHandler
        write32(0);                                        // dwFlags
        write16(0);                                        // wPriority
        write16(0);                                        // wLanguage
        write32(0);                                        // dwInitialFrames
        write32(block_align);                              // dwScale
        write32((uint32_t)audio_sample_rate * block_align); // dwRate
        write32(0);                                        // dwStart
        audio.strh_length_pos = tell();
        write32(0);                                        // dwLength: sample frames, patched on close
        write32((uint32_t)audio_sample_rate * block_align); // dwSuggestedBufferSize
        write32(0xFFFFFFFF);                               // dwQuality
        write32(block_align);                              // dwSampleSize
        write16(0);                                        // rcFrame
        write16(0);
        write16(0);
        write16(0);
        endChunk(strh_pos);

        strf_pos = beginChunk("strf");
        write16(1);                                        // wFormatTag = WAVE_FORMAT_PCM
        write16((uint16_t)audio_channels);                 // nChannels
        write32((uint32_t)audio_sample_rate);              // nSamplesPerSec
        write32((uint32_t)audio_sample_rate * block_align); // nAvgBytesPerSec
        write16((uint16_t)block_align);                    // nBlockAlign
        write16(16);                                       // wBitsPerSample
        write16(0);                                        // cbSize
        endChunk(strf_pos);

        writeSuperIndex(audio);
        endChunk(strl_pos);
    }

    // OpenDML extended header: the frame count across every segment.
    int64_t odml_pos = beginList("odml");
    int64_t dmlh_pos = beginChunk("dmlh");
    dmlh_total_frames_pos = tell();
    write32(0);
    for (int i = 0; i < 61; i++) // reserved, sized like other writers
        write32(0);
    endChunk(dmlh_pos);
    endChunk(odml_pos);

    endChunk(hdrl_pos);
}

void AVIWriter::startSegment()
{
    riff_start = tell();
    writeFourCC("RIFF");
    write32(0); // patched in endSegment
    writeFourCC(segment_count == 0 ? "AVI " : "AVIX");

    if (segment_count == 0)
        writeHeaders();

    movi_size_pos = beginList("movi");
    movi_pos = movi_size_pos + 4;

    video.segment_entries.clear();
    video.segment_length = 0;
    audio.segment_entries.clear();
    audio.segment_length = 0;
}

/* AVISTDINDEX for one stream, covering the chunks of the current segment;
 * its position and extent are then recorded in the header's super index. */
void AVIWriter::writeStandardIndex(Stream &stream)
{
    const char *id = (&stream == &video) ? "ix00" : "ix01";

    int64_t chunk_start = tell();
    int64_t size_pos = beginChunk(id);
    write16(2);                       // wLongsPerEntry
    write8(0);                        // bIndexSubType
    write8(AVI_INDEX_OF_CHUNKS);      // bIndexType
    write32((uint32_t)stream.segment_entries.size()); // nEntriesInUse
    write32(stream.chunk_id);         // dwChunkId
    write64((uint64_t)movi_pos);      // qwBaseOffset
    write32(0);                       // dwReserved3
    for (auto &entry : stream.segment_entries)
    {
        write32(entry.offset);
        write32(entry.size); // bit 31 clear: every chunk is a keyframe
    }
    endChunk(size_pos);
    int64_t chunk_end = tell();

    if (stream.super_entries >= kSuperIndexEntries)
    {
        write_failed = true; // recording longer than the reserved index
        return;
    }

    // AVISUPERINDEX entry: qwOffset, dwSize, dwDuration
    const uint64_t offset = (uint64_t)chunk_start;
    const uint32_t size = (uint32_t)(chunk_end - chunk_start);
    const uint32_t duration = (uint32_t)stream.segment_length;
    uint8_t entry[16];
    for (int i = 0; i < 8; i++)
        entry[i] = (uint8_t)(offset >> (8 * i));
    for (int i = 0; i < 4; i++)
    {
        entry[8 + i] = (uint8_t)(size >> (8 * i));
        entry[12 + i] = (uint8_t)(duration >> (8 * i));
    }
    patch(stream.super_index_pos + 24 + (int64_t)stream.super_entries * 16, entry, 16);
    stream.super_entries++;
    patch32(stream.super_index_pos + 4, stream.super_entries); // nEntriesInUse
}

/* The AVI 1.0 index of the first segment, for players without OpenDML. */
void AVIWriter::writeLegacyIndex()
{
    int64_t size_pos = beginChunk("idx1");
    for (auto &entry : legacy_entries)
    {
        write32(entry.chunk_id);
        write32(AVIIF_KEYFRAME);
        write32(entry.offset);
        write32(entry.size);
    }
    endChunk(size_pos);
    legacy_entries.clear();
}

void AVIWriter::endSegment()
{
    writeStandardIndex(video);
    if (has_audio)
        writeStandardIndex(audio);
    endChunk(movi_size_pos);

    if (segment_count == 0)
    {
        first_segment_frames = (uint32_t)video.segment_length;
        writeLegacyIndex();
    }

    patch32(riff_start + 4, (uint32_t)(tell() - (riff_start + 8)));
    segment_count++;
}

/* Bytes the end of the current segment still needs: its standard indexes
 * (and, in the first segment, the idx1 index). */
uint64_t AVIWriter::segmentReserve() const
{
    uint64_t reserve = 32 + 8 * (video.segment_entries.size() + 1);
    if (has_audio)
        reserve += 32 + 8 * (audio.segment_entries.size() + 1);
    if (segment_count == 0)
        reserve += 8 + 16 * (legacy_entries.size() + 1);
    return reserve + 1024;
}

void AVIWriter::writeChunk(Stream &stream, const void *data, uint32_t size, uint64_t length)
{
    if (!file || write_failed)
        return;

    uint64_t segment_size = (uint64_t)(tell() - riff_start);
    if (segment_size + size + 8 + segmentReserve() > kMaxSegmentSize)
    {
        endSegment();
        startSegment();
    }

    int64_t chunk_pos = tell();
    uint32_t id = stream.chunk_id;
    write32(id);
    write32(size);
    write(data, size);
    if (size & 1)
        write8(0);

    stream.segment_entries.push_back({ (uint32_t)(chunk_pos + 8 - movi_pos), size });
    if (segment_count == 0)
        legacy_entries.push_back({ id, (uint32_t)(chunk_pos - movi_pos), size });

    stream.segment_length += length;
    stream.total_length += length;
}

void AVIWriter::addVideoFrame(const uint8_t *bgr_bottom_up)
{
    writeChunk(video, bgr_bottom_up, (uint32_t)frameSize(), 1);
}

void AVIWriter::addAudio(const int16_t *samples, int frames)
{
    if (!has_audio || frames <= 0)
        return;
    writeChunk(audio, samples, (uint32_t)frames * audio_channels * 2, (uint64_t)frames);
}

void AVIWriter::close()
{
    if (!file)
        return;

    if (!write_failed)
    {
        endSegment();
        patch32(avih_total_frames_pos, first_segment_frames);
        patch32(dmlh_total_frames_pos, (uint32_t)video.total_length);
        patch32(video.strh_length_pos, (uint32_t)video.total_length);
        if (has_audio)
            patch32(audio.strh_length_pos, (uint32_t)audio.total_length);
    }

    fclose(file);
    file = nullptr;
}
