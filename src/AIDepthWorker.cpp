#include "AIDepthWorker.h"

#include "AIDepthIpc.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <sstream>

using namespace DmpAIDepthIpc;

namespace {
struct ScopedMutex {
    HANDLE h = nullptr;
    bool owned = false;
    explicit ScopedMutex(HANDLE handle, DWORD timeoutMs = 0) : h(handle) {
        if (!h) return;
        const DWORD r = WaitForSingleObject(h, timeoutMs);
        owned = (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED);
    }
    ~ScopedMutex() { if (owned && h) ReleaseMutex(h); }
};

static std::filesystem::path ModuleDirectory() {
    wchar_t p[32768]{};
    const DWORD n = GetModuleFileNameW(nullptr, p, static_cast<DWORD>(std::size(p)));
    if (!n || n >= std::size(p)) return std::filesystem::current_path();
    return std::filesystem::path(p).parent_path();
}

static std::string SafeText(const char* p, size_t capacity) {
    if (!p || !capacity) return {};
    size_t n = 0;
    while (n < capacity && p[n]) ++n;
    return std::string(p, p + n);
}
}

AIDepthWorker::AIDepthWorker() = default;
AIDepthWorker::~AIDepthWorker() { Stop(); }

std::wstring AIDepthWorker::QuoteArg(const std::wstring& value) {
    std::wstring out = L"\"";
    for (wchar_t c : value) {
        if (c == L'\"') out += L"\\\"";
        else out += c;
    }
    out += L"\"";
    return out;
}

bool AIDepthWorker::Start(const std::wstring& enginePath, const std::wstring& runtimeCachePath,
                          uint32_t maxFrameWidth, uint32_t maxFrameHeight) {
    Stop();
    std::lock_guard<std::mutex> lock(m_mutex);

    if (enginePath.empty() || !std::filesystem::exists(std::filesystem::path(enginePath))) {
        m_failed = true;
        m_lastError = "TensorRT-RTX AI depth engine is missing.";
        return false;
    }
    if (!maxFrameWidth || !maxFrameHeight) {
        m_failed = true;
        m_lastError = "Invalid AI depth IPC frame capacity.";
        return false;
    }

    const auto helper = ModuleDirectory() / L"ai_depth" / L"DMPAIDepthWorker.exe";
    if (!std::filesystem::exists(helper)) {
        m_failed = true;
        m_lastError = "AI depth sidecar executable is missing: " + helper.string();
        return false;
    }

    const uint64_t inputCapacity = uint64_t(maxFrameWidth) * maxFrameHeight * 4ull;
    m_mappingBytes = MappingBytes(inputCapacity);
    const uint64_t token = (uint64_t(GetCurrentProcessId()) << 32) ^ uint64_t(GetTickCount64());
    const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(token);
    const std::wstring mapName = L"Local\\DMPAIDepthMap_" + suffix;
    const std::wstring frameName = L"Local\\DMPAIDepthFrame_" + suffix;
    const std::wstring resultName = L"Local\\DMPAIDepthResult_" + suffix;
    const std::wstring stopName = L"Local\\DMPAIDepthStop_" + suffix;
    const std::wstring mutexName = L"Local\\DMPAIDepthMutex_" + suffix;

    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        static_cast<DWORD>(m_mappingBytes >> 32), static_cast<DWORD>(m_mappingBytes & 0xffffffffu), mapName.c_str());
    if (!m_mapping) { m_lastError = "CreateFileMappingW failed."; m_failed = true; CloseHandlesLocked(); return false; }
    m_view = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(m_mappingBytes));
    if (!m_view) { m_lastError = "MapViewOfFile failed."; m_failed = true; CloseHandlesLocked(); return false; }
    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, frameName.c_str());
    m_resultEvent = CreateEventW(nullptr, FALSE, FALSE, resultName.c_str());
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, stopName.c_str());
    m_ipcMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!m_frameEvent || !m_resultEvent || !m_stopEvent || !m_ipcMutex) {
        m_lastError = "Failed to create AI depth IPC synchronization objects.";
        m_failed = true;
        CloseHandlesLocked();
        return false;
    }

    std::memset(m_view, 0, static_cast<size_t>(m_mappingBytes));
    auto* h = static_cast<SharedHeader*>(m_view);
    h->magic = Magic;
    h->version = Version;
    h->capacityWidth = maxFrameWidth;
    h->capacityHeight = maxFrameHeight;
    h->inputCapacityBytes = inputCapacity;
    h->outputCapacityFloats = uint64_t(OutputWidth) * OutputHeight;
    h->childState = Starting;

    std::wstring cmd = QuoteArg(helper.wstring());
    cmd += L" --map " + QuoteArg(mapName);
    cmd += L" --frame-event " + QuoteArg(frameName);
    cmd += L" --result-event " + QuoteArg(resultName);
    cmd += L" --stop-event " + QuoteArg(stopName);
    cmd += L" --mutex " + QuoteArg(mutexName);
    cmd += L" --engine " + QuoteArg(enginePath);
    cmd += L" --cache " + QuoteArg(runtimeCachePath);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    const std::wstring helperDir = helper.parent_path().wstring();
    ZeroMemory(&m_process, sizeof(m_process));
    if (!CreateProcessW(helper.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, helperDir.c_str(), &si, &m_process)) {
        std::ostringstream s;
        s << "CreateProcessW for AI depth sidecar failed, Win32=" << GetLastError();
        m_lastError = s.str();
        m_failed = true;
        CloseHandlesLocked();
        return false;
    }

    m_generation = 1;
    m_submitSequence = 0;
    m_lastCopiedSequence = 0;
    m_droppedPending = 0;
    m_lastSubmitTimestamp100ns = -1;
    m_started = true;
    m_ready = false;
    m_failed = false;
    m_loggedReady = false;
    m_loggedFailure = false;
    m_lastError.clear();
    m_modelSummary.clear();
    LOG("[AI Depth] sidecar process launched pid=" << m_process.dwProcessId
        << " IPC=" << maxFrameWidth << "x" << maxFrameHeight << " -> 518x518; player process does not load TensorRT/CUDA.");
    return true;
}

void AIDepthWorker::CloseHandlesLocked() {
    if (m_view) { UnmapViewOfFile(m_view); m_view = nullptr; }
    if (m_mapping) { CloseHandle(m_mapping); m_mapping = nullptr; }
    if (m_frameEvent) { CloseHandle(m_frameEvent); m_frameEvent = nullptr; }
    if (m_resultEvent) { CloseHandle(m_resultEvent); m_resultEvent = nullptr; }
    if (m_stopEvent) { CloseHandle(m_stopEvent); m_stopEvent = nullptr; }
    if (m_ipcMutex) { CloseHandle(m_ipcMutex); m_ipcMutex = nullptr; }
    if (m_process.hThread) { CloseHandle(m_process.hThread); m_process.hThread = nullptr; }
    if (m_process.hProcess) { CloseHandle(m_process.hProcess); m_process.hProcess = nullptr; }
    m_started = false;
    m_ready = false;
}

void AIDepthWorker::Stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_started && !m_process.hProcess) { CloseHandlesLocked(); return; }
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_process.hProcess) {
        const DWORD r = WaitForSingleObject(m_process.hProcess, 2500);
        if (r == WAIT_TIMEOUT) {
            LOG("[AI Depth] sidecar did not exit in time; terminating helper process.");
            TerminateProcess(m_process.hProcess, 0xA1D0u);
            WaitForSingleObject(m_process.hProcess, 500);
        }
    }
    CloseHandlesLocked();
}

void AIDepthWorker::RefreshStateLocked() const {
    if (!m_view || !m_ipcMutex) return;
    uint32_t childState = Starting;
    {
        ScopedMutex ipc(m_ipcMutex, 0);
        if (!ipc.owned) return;
        const auto* h = static_cast<const SharedHeader*>(m_view);
        if (h->magic != Magic || h->version != Version) return;
        childState = h->childState;
        if (childState == Ready) {
            m_ready = true;
            m_failed = false;
            m_modelSummary = SafeText(h->summaryText, SummaryTextBytes);
            if (!m_loggedReady) {
                LOG("[AI Depth] sidecar ready: " << m_modelSummary);
                m_loggedReady = true;
            }
        } else if (childState == DmpAIDepthIpc::Failed) {
            m_ready = false;
            m_failed = true;
            m_lastError = SafeText(h->errorText, ErrorTextBytes);
        }
    }

    // Loader/dependency failures can terminate the helper before it can publish an
    // IPC error. Detect that case from the process handle so a black debug view is
    // accompanied by a useful player-log message rather than staying "Starting".
    if (!m_failed && m_process.hProcess && WaitForSingleObject(m_process.hProcess, 0) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        GetExitCodeProcess(m_process.hProcess, &exitCode);
        std::ostringstream s;
        s << "AI depth sidecar exited unexpectedly, code=" << exitCode;
        m_lastError = s.str();
        m_ready = false;
        m_failed = true;
    }
    if (m_failed && !m_loggedFailure) {
        LOG("[AI Depth] sidecar FAILED: " << m_lastError);
        m_loggedFailure = true;
    }
}

void AIDepthWorker::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_generation;
    m_lastCopiedSequence = 0;
    m_lastSubmitTimestamp100ns = -1;
    if (m_resultEvent) ResetEvent(m_resultEvent);
}

bool AIDepthWorker::Submit(const uint8_t* bgra, size_t bytes, uint32_t width, uint32_t height,
                           size_t strideBytes, int64_t timestamp100ns) {
    if (!bgra || !width || !height || strideBytes < size_t(width) * 4u) return false;
    const size_t needed = strideBytes * size_t(height);
    if (bytes < needed) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_started || !m_view || !m_ipcMutex) return false;
    RefreshStateLocked();
    if (m_failed) return false;

    // 30 Hz AI-depth cadence is sufficient for debug and preserves GPU headroom.
    if (m_lastSubmitTimestamp100ns >= 0 && timestamp100ns > m_lastSubmitTimestamp100ns &&
        timestamp100ns - m_lastSubmitTimestamp100ns < 300000) {
        return false;
    }

    ScopedMutex ipc(m_ipcMutex, 0);
    if (!ipc.owned) { ++m_droppedPending; return false; }
    auto* h = static_cast<SharedHeader*>(m_view);
    if (width > h->capacityWidth || height > h->capacityHeight || needed > h->inputCapacityBytes) {
        m_lastError = "AI depth frame exceeds shared-memory capacity.";
        m_failed = true;
        return false;
    }
    std::memcpy(InputBytes(m_view), bgra, needed);
    h->frameWidth = width;
    h->frameHeight = height;
    h->frameStrideBytes = strideBytes;
    h->frameBytes = needed;
    h->frameTimestamp100ns = timestamp100ns;
    h->frameGeneration = m_generation;
    h->frameSequence = ++m_submitSequence;
    m_lastSubmitTimestamp100ns = timestamp100ns;
    SetEvent(m_frameEvent);
    return true;
}

bool AIDepthWorker::GetLatest(uint64_t afterSequence, AIDepthFrame& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_started || !m_view || !m_ipcMutex) return false;
    RefreshStateLocked();
    if (m_failed) return false;

    ScopedMutex ipc(m_ipcMutex, 0);
    if (!ipc.owned) return false;
    const auto* h = static_cast<const SharedHeader*>(m_view);
    if (h->resultSequence == 0 || h->resultSequence <= afterSequence ||
        h->resultSequence <= m_lastCopiedSequence || h->resultGeneration != m_generation) return false;
    if (h->resultWidth != OutputWidth || h->resultHeight != OutputHeight ||
        h->outputCapacityFloats < uint64_t(OutputWidth) * OutputHeight) return false;

    AIDepthFrame completed;
    completed.sequence = h->resultSequence;
    completed.timestamp100ns = h->resultTimestamp100ns;
    completed.width = h->resultWidth;
    completed.height = h->resultHeight;
    completed.percentile02 = h->percentile02;
    completed.percentile98 = h->percentile98;
    completed.inferenceMs = h->inferenceMs;
    completed.totalMs = h->totalMs;
    const size_t count = size_t(OutputWidth) * OutputHeight;
    const float* src = OutputDepth(m_view, *h);
    completed.rawDepth.assign(src, src + count);
    completed.preview01.resize(count);
    float lo = completed.percentile02;
    float hi = completed.percentile98;
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo + 1e-8f) { lo = 0.0f; hi = 1.0f; }
    const float inv = 1.0f / std::max(hi - lo, 1e-8f);
    for (size_t i = 0; i < count; ++i) {
        float v = completed.rawDepth[i];
        if (!std::isfinite(v)) v = lo;
        completed.preview01[i] = std::clamp((v - lo) * inv, 0.0f, 1.0f);
    }
    m_lastCopiedSequence = completed.sequence;
    out = std::move(completed);
    return true;
}

bool AIDepthWorker::IsReady() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    RefreshStateLocked();
    return m_ready && !m_failed;
}

bool AIDepthWorker::Failed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    RefreshStateLocked();
    return m_failed;
}

std::string AIDepthWorker::LastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    RefreshStateLocked();
    return m_lastError;
}

std::string AIDepthWorker::ModelSummary() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    RefreshStateLocked();
    return m_modelSummary;
}

uint64_t AIDepthWorker::DroppedPendingFrames() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_droppedPending;
}
