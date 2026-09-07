#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct SubtitleCue {
    double startSec=0.0;
    double endSec=0.0;
    std::wstring text;
};

bool ParseSrtSubtitleText(const std::string& utf8,std::vector<SubtitleCue>& cues);
std::wstring SubtitleTextAt(const std::vector<SubtitleCue>& cues,double seconds);

// Step 04G-2: consume only complete SRT cue blocks from pendingUtf8 and append
// them to published. With flushTail=true, also consumes the final unterminated
// block at EOF. This lets the player publish cues while FFmpeg is still scanning
// the rest of a long movie instead of waiting for the whole track to finish.
size_t ParseAvailableSrtSubtitleText(std::string& pendingUtf8,
                                    std::vector<SubtitleCue>& published,
                                    bool flushTail=false);

class SubtitlePlayer {
public:
    SubtitlePlayer();
    ~SubtitlePlayer();
    SubtitlePlayer(const SubtitlePlayer&)=delete;
    SubtitlePlayer& operator=(const SubtitlePlayer&)=delete;
    SubtitlePlayer(SubtitlePlayer&&) noexcept=default;
    SubtitlePlayer& operator=(SubtitlePlayer&&) noexcept=default;

    // Non-blocking progressive extraction. FFmpeg runs on a detached worker.
    // Complete SRT cue blocks are published incrementally, so subtitles can become
    // visible before FFmpeg reaches EOF. Selecting Off/switch/reload cancels the
    // previous extraction without waiting on the UI/render thread.
    void LoadAsync(const std::wstring& mediaPath,int absoluteStreamIndex);
    void Clear();
    bool Loaded()const;
    bool Loading()const;
    size_t CueCount()const;
    std::wstring TextAt(double seconds)const;

private:
    struct State;
    std::shared_ptr<State> m_state;
};
