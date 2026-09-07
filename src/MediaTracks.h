#pragma once
#include <string>
#include <vector>

enum class MediaTrackKind { Audio, Subtitle };

struct MediaTrackInfo {
    MediaTrackKind kind = MediaTrackKind::Audio;
    int streamIndex = -1;              // absolute ffmpeg stream index (0:<index>)
    std::wstring codec;
    std::wstring language;
    std::wstring title;
    std::wstring channelLayout;
    int channels = 0;
    bool defaultDisposition = false;
    bool forcedDisposition = false;
    bool textSubtitleSupported = false;
    bool bitmapSubtitle = false;
};

// Pure parser used both by the player and the portable self-test.
std::vector<MediaTrackInfo> ParseFFprobeTrackDump(const std::string& text);

class MediaTrackCatalog {
public:
    bool Probe(const std::wstring& mediaPath);
    void Clear();

    const std::vector<MediaTrackInfo>& AudioTracks() const { return m_audio; }
    const std::vector<MediaTrackInfo>& SubtitleTracks() const { return m_subtitles; }
    const MediaTrackInfo* FindAudio(int streamIndex) const;
    const MediaTrackInfo* FindSubtitle(int streamIndex) const;
    int DefaultAudioStream() const;

    static bool IsTextSubtitleCodec(const std::wstring& codec);
    static bool IsBitmapSubtitleCodec(const std::wstring& codec);
    static std::wstring MenuLabel(const MediaTrackInfo& track);
    static std::wstring ShortLabel(const MediaTrackInfo& track);

private:
    std::vector<MediaTrackInfo> m_audio;
    std::vector<MediaTrackInfo> m_subtitles;
};
