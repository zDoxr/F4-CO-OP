#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <xaudio2.h>

#include "CoSyncVoice.h"

#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncProfiles.h"
#include "CoSyncFactions.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPrismaUI.h"

#include "opus.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr int    kRate         = 48000;
    constexpr int    kFrame        = 960;           // 20 ms at 48 kHz
    constexpr int    kMaxDecode    = 2880;          // 60 ms: the longest frame we accept
    constexpr size_t kMaxPayload   = 400;           // Opus at 24 kbit/s: ~60 bytes a frame
    constexpr uint64_t kTalkHoldMs = 350;

    // ── Settings ([Voice] in Data\F4SE\Plugins\CoSync.ini) ─────────────────
    struct Config
    {
        bool  enabled   = true;
        int   pttKey    = 0;       // 0 = voice activation
        int   modeKey   = 85;      // U
        int   muteKey   = 0;
        float threshold = 0.012f;  // voice activation level (RMS, full scale = 1)
        float micGain   = 1.0f;
        float volume    = 1.0f;    // playback
        float range     = 3000.f;  // host: proximity cut-off (game units)
        float fullRange = 600.f;   // host: full volume inside this distance
        int   bitrate   = 24000;
    };
    Config s_cfg;
    std::once_flag s_cfgOnce;

    std::string IniPath()
    {
        char exe[MAX_PATH] = {};
        if (!GetModuleFileNameA(nullptr, exe, MAX_PATH))
            return std::string();
        std::string p = exe;
        return p.substr(0, p.find_last_of("\\/") + 1) + "Data\\F4SE\\Plugins\\CoSync.ini";
    }

    float IniFloat(const char* key, float def, const std::string& ini)
    {
        char buf[64] = {};
        GetPrivateProfileStringA("Voice", key, "", buf, sizeof(buf), ini.c_str());
        if (!buf[0])
            return def;
        char* end = nullptr;
        const float v = std::strtof(buf, &end);
        return (end && end != buf && std::isfinite(v)) ? v : def;
    }

    int IniKey(const char* key, int def, const std::string& ini)
    {
        const int v = static_cast<int>(GetPrivateProfileIntA("Voice", key, def, ini.c_str()));
        return (v >= 0 && v <= 0xFE) ? v : def;
    }

    void LoadConfig()
    {
        std::call_once(s_cfgOnce, []
        {
            const std::string ini = IniPath();
            Config c;
            c.enabled   = GetPrivateProfileIntA("Voice", "bEnabled", 1, ini.c_str()) != 0;
            c.pttKey    = IniKey("iPushToTalkKey", 0, ini);
            c.modeKey   = IniKey("iModeKey", 85, ini);
            c.muteKey   = IniKey("iMuteKey", 0, ini);
            c.threshold = IniFloat("fThreshold", c.threshold, ini);
            c.micGain   = IniFloat("fMicGain", c.micGain, ini);
            c.volume    = IniFloat("fVolume", c.volume, ini);
            c.range     = IniFloat("fRange", c.range, ini);
            c.fullRange = IniFloat("fFullVolumeRange", c.fullRange, ini);
            c.bitrate   = static_cast<int>(GetPrivateProfileIntA("Voice", "iBitrate", c.bitrate, ini.c_str()));
            if (c.threshold < 0.f || c.threshold > 1.f) c.threshold = 0.012f;
            if (c.micGain < 0.f || c.micGain > 8.f)     c.micGain = 1.f;
            if (c.volume < 0.f || c.volume > 4.f)       c.volume = 1.f;
            if (c.range < 100.f)                        c.range = 3000.f;
            if (c.fullRange < 0.f || c.fullRange >= c.range) c.fullRange = c.range * 0.2f;
            if (c.bitrate < 8000 || c.bitrate > 64000)  c.bitrate = 24000;
            s_cfg = c;
            LOG_INFO("[Voice] Settings: enabled=%d %s key=%d mode key=%d mute key=%d threshold=%.3f range=%.0f/%.0f bitrate=%d",
                c.enabled ? 1 : 0, c.pttKey ? "push-to-talk" : "voice activation", c.pttKey, c.modeKey,
                c.muteKey, c.threshold, c.fullRange, c.range, c.bitrate);
        });
    }

    // ── Shared state ────────────────────────────────────────────────────────
    std::atomic<bool> s_session{ false };
    std::atomic<char> s_mode{ 'P' };
    std::atomic<bool> s_muted{ false };
    std::atomic<bool> s_tx{ false };
    std::atomic<bool> s_micOk{ false };
    std::atomic<bool> s_inFaction{ false };

    struct OutFrame { char mode; uint32_t seq; std::string data; };
    std::mutex             s_outMx;
    std::deque<OutFrame>   s_outbox;

    struct PlayPacket { uint32_t eid; uint32_t seq; uint8_t gain; std::string data; };
    std::mutex             s_playMx;
    std::deque<PlayPacket> s_playQ;
    HANDLE                 s_playEvent = nullptr;

    std::mutex                             s_talkMx;
    std::unordered_map<uint32_t, uint64_t> s_talkUntil;   // eid -> GetTickCount64 deadline

    // Heap-held and never destroyed: a joinable std::thread destroyed at
    // process exit (game quit mid-session, no Shutdown) calls std::terminate.
    std::thread*      s_captureThread = nullptr;
    std::thread*      s_playThread = nullptr;
    std::atomic<bool> s_stop{ false };

    // Host: per-connection packet budget (a 20 ms stream is 50/s).
    struct Budget { uint64_t windowStart = 0; int count = 0; };
    std::unordered_map<uint32_t, Budget> s_budget;

    void MarkTalking(uint32_t eid)
    {
        std::lock_guard<std::mutex> lk(s_talkMx);
        s_talkUntil[eid] = GetTickCount64() + kTalkHoldMs;
    }

    void EnqueuePlay(uint32_t eid, uint32_t seq, uint8_t gain, const char* data, size_t n)
    {
        MarkTalking(eid);
        {
            std::lock_guard<std::mutex> lk(s_playMx);
            if (s_playQ.size() >= 256)
                s_playQ.pop_front();
            s_playQ.push_back({ eid, seq, gain, std::string(data, n) });
        }
        if (s_playEvent)
            SetEvent(s_playEvent);
    }

    bool KeyHeld(int vk)
    {
        return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    bool GameHasFocus()
    {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg)
            GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    // ── Capture thread ──────────────────────────────────────────────────────
    void CaptureMain()
    {
        WAVEFORMATEX wf{};
        wf.wFormatTag      = WAVE_FORMAT_PCM;
        wf.nChannels       = 1;
        wf.nSamplesPerSec  = kRate;
        wf.wBitsPerSample  = 16;
        wf.nBlockAlign     = 2;
        wf.nAvgBytesPerSec = kRate * 2;

        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        HWAVEIN hin = nullptr;
        // WAVE_MAPPER converts from whatever the default microphone delivers.
        const MMRESULT r = waveInOpen(&hin, WAVE_MAPPER, &wf, reinterpret_cast<DWORD_PTR>(ev), 0, CALLBACK_EVENT);
        if (r != MMSYSERR_NOERROR)
        {
            LOG_WARN("[Voice] No microphone (waveInOpen failed, %u) -- you can still hear others", r);
            CloseHandle(ev);
            return;
        }

        int err = 0;
        OpusEncoder* enc = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
        if (!enc || err != OPUS_OK)
        {
            LOG_ERROR("[Voice] opus_encoder_create failed (%d)", err);
            waveInClose(hin);
            CloseHandle(ev);
            return;
        }
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(s_cfg.bitrate));
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(10));

        constexpr int kBufs = 6;
        std::vector<int16_t> mem(static_cast<size_t>(kBufs) * kFrame);
        WAVEHDR hdr[kBufs] = {};
        for (int i = 0; i < kBufs; ++i)
        {
            hdr[i].lpData = reinterpret_cast<LPSTR>(&mem[static_cast<size_t>(i) * kFrame]);
            hdr[i].dwBufferLength = kFrame * 2;
            waveInPrepareHeader(hin, &hdr[i], sizeof(WAVEHDR));
            waveInAddBuffer(hin, &hdr[i], sizeof(WAVEHDR));
        }
        waveInStart(hin);
        s_micOk.store(true);
        LOG_INFO("[Voice] Microphone open (48 kHz mono), Opus %s", opus_get_version_string());

        uint32_t seq = 0;
        uint64_t hangUntil = 0;
        bool     wasTx = false;
        std::string prev;          // the frame before, sent as pre-roll when talking starts
        int16_t  pcm[kFrame];
        unsigned char packet[kMaxPayload];
        int next = 0;

        while (!s_stop.load())
        {
            WaitForSingleObject(ev, 50);
            while (!s_stop.load() && (hdr[next].dwFlags & WHDR_DONE))
            {
                WAVEHDR& h = hdr[next];
                const int got = static_cast<int>(h.dwBytesRecorded / 2);
                const int16_t* src = reinterpret_cast<const int16_t*>(h.lpData);
                const float gain = s_cfg.micGain;
                double sum = 0.0;
                for (int i = 0; i < kFrame; ++i)
                {
                    float v = i < got ? src[i] * gain : 0.f;
                    if (v > 32767.f) v = 32767.f;
                    if (v < -32768.f) v = -32768.f;
                    pcm[i] = static_cast<int16_t>(v);
                    sum += static_cast<double>(v) * v;
                }
                h.dwBytesRecorded = 0;
                waveInAddBuffer(hin, &h, sizeof(WAVEHDR));
                next = (next + 1) % kBufs;

                const float rms = static_cast<float>(std::sqrt(sum / kFrame) / 32768.0);
                const uint64_t nowMs = GetTickCount64();
                bool want = false;
                if (s_session.load() && !s_muted.load())
                {
                    if (s_cfg.pttKey)
                        want = KeyHeld(s_cfg.pttKey) && GameHasFocus() && !CoSyncPrismaUI::IsChatOpen();
                    else
                    {
                        if (rms >= s_cfg.threshold)
                            hangUntil = nowMs + 400;
                        want = nowMs < hangUntil;
                    }
                }

                // Every frame is encoded (the encoder state stays continuous and
                // the previous frame is ready as pre-roll); only talking frames
                // are sent. seq counts every frame, so the receiver sees a gap
                // between bursts and resets its decoder instead of concealing.
                const int n = opus_encode(enc, pcm, kFrame, packet, static_cast<opus_int32>(sizeof(packet)));
                ++seq;
                if (n <= 0)
                {
                    prev.clear();
                    continue;
                }
                std::string cur(reinterpret_cast<const char*>(packet), static_cast<size_t>(n));
                if (want)
                {
                    std::lock_guard<std::mutex> lk(s_outMx);
                    if (!wasTx && !prev.empty())
                        s_outbox.push_back({ s_mode.load(), seq - 1, prev });
                    s_outbox.push_back({ s_mode.load(), seq, cur });
                    while (s_outbox.size() > 50)
                        s_outbox.pop_front();
                }
                if (want != wasTx)
                    LOG_DEBUG("[Voice] %s talking", want ? "Started" : "Stopped");
                wasTx = want;
                s_tx.store(want);
                prev.swap(cur);
            }
        }

        s_tx.store(false);
        s_micOk.store(false);
        waveInReset(hin);
        for (int i = 0; i < kBufs; ++i)
            waveInUnprepareHeader(hin, &hdr[i], sizeof(WAVEHDR));
        waveInClose(hin);
        opus_encoder_destroy(enc);
        CloseHandle(ev);
        LOG_INFO("[Voice] Microphone closed");
    }

    // ── Playback thread ─────────────────────────────────────────────────────
    typedef HRESULT (WINAPI* XAudio2CreateFn)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);

    constexpr int kRing = 16;       // buffers per speaker; at most 12 are ever queued

    struct Speaker
    {
        OpusDecoder*         dec = nullptr;
        IXAudio2SourceVoice* voice = nullptr;
        uint32_t             lastSeq = 0;
        bool                 hasSeq = false;
        uint32_t             submitted = 0;
        uint64_t             lastHeardMs = 0;
        float                volume = -1.f;
        std::vector<int16_t> ring = std::vector<int16_t>(static_cast<size_t>(kRing) * kMaxDecode);
    };

    void Submit(Speaker& sp, const int16_t* pcm, int samples)
    {
        XAUDIO2_VOICE_STATE st{};
        sp.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued >= 10)
            return;   // over 200 ms behind: drop rather than build up delay
        auto push = [&](const int16_t* src, int n)
        {
            int16_t* slot = &sp.ring[static_cast<size_t>(sp.submitted % kRing) * kMaxDecode];
            if (src) std::memcpy(slot, src, static_cast<size_t>(n) * 2);
            else     std::memset(slot, 0, static_cast<size_t>(n) * 2);
            ++sp.submitted;
            XAUDIO2_BUFFER b{};
            b.AudioBytes = static_cast<UINT32>(n) * 2;
            b.pAudioData = reinterpret_cast<const BYTE*>(slot);
            sp.voice->SubmitSourceBuffer(&b);
        };
        // Starved (a burst starts, or the network stalled): lead with 20 ms of
        // silence so the next packet's jitter does not immediately starve it.
        if (st.BuffersQueued == 0)
            push(nullptr, kFrame);
        push(pcm, samples);
    }

    void PlayMain()
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HMODULE dll = LoadLibraryExA("xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll)
            dll = LoadLibraryExA("xaudio2_8.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        XAudio2CreateFn create = dll ? reinterpret_cast<XAudio2CreateFn>(GetProcAddress(dll, "XAudio2Create")) : nullptr;
        IXAudio2* xa = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        if (!create || FAILED(create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !xa ||
            FAILED(xa->CreateMasteringVoice(&master)))
        {
            LOG_ERROR("[Voice] XAudio2 unavailable (dll=%p) -- voice playback off", dll);
            if (xa) xa->Release();
            if (dll) FreeLibrary(dll);
            if (SUCCEEDED(co)) CoUninitialize();
            return;
        }
        LOG_INFO("[Voice] Playback ready (XAudio2)");

        std::unordered_map<uint32_t, std::unique_ptr<Speaker>> speakers;
        std::vector<int16_t> pcm(kMaxDecode);
        uint64_t nextSweep = 0;

        WAVEFORMATEX wf{};
        wf.wFormatTag      = WAVE_FORMAT_PCM;
        wf.nChannels       = 1;
        wf.nSamplesPerSec  = kRate;
        wf.wBitsPerSample  = 16;
        wf.nBlockAlign     = 2;
        wf.nAvgBytesPerSec = kRate * 2;

        while (!s_stop.load())
        {
            WaitForSingleObject(s_playEvent, 20);
            std::deque<PlayPacket> batch;
            {
                std::lock_guard<std::mutex> lk(s_playMx);
                batch.swap(s_playQ);
            }
            const uint64_t nowMs = GetTickCount64();
            for (const PlayPacket& p : batch)
            {
                auto& slot = speakers[p.eid];
                if (!slot)
                {
                    auto sp = std::make_unique<Speaker>();
                    int err = 0;
                    sp->dec = opus_decoder_create(kRate, 1, &err);
                    if (!sp->dec || err != OPUS_OK || FAILED(xa->CreateSourceVoice(&sp->voice, &wf)) || !sp->voice)
                    {
                        if (sp->dec) opus_decoder_destroy(sp->dec);
                        speakers.erase(p.eid);
                        continue;
                    }
                    sp->voice->Start(0);
                    slot = std::move(sp);
                    LOG_INFO("[Voice] Hearing entity %u", p.eid);
                }
                Speaker& sp = *slot;
                sp.lastHeardMs = nowMs;

                const float vol = (p.gain / 100.f) * s_cfg.volume;
                if (std::fabs(vol - sp.volume) > 0.01f)
                {
                    sp.voice->SetVolume(vol);
                    sp.volume = vol;
                }

                const unsigned char* data = reinterpret_cast<const unsigned char*>(p.data.data());
                const opus_int32 len = static_cast<opus_int32>(p.data.size());
                if (sp.hasSeq)
                {
                    const int32_t diff = static_cast<int32_t>(p.seq - sp.lastSeq);
                    if (diff <= 0)
                        continue;   // late or duplicate
                    if (diff > 4)
                        opus_decoder_ctl(sp.dec, OPUS_RESET_STATE);   // a new burst
                    else if (diff > 1)
                    {
                        // Lost frames: conceal all but the last, rebuild the
                        // last from this packet's forward error correction.
                        for (int32_t i = 1; i < diff - 1; ++i)
                        {
                            const int n = opus_decode(sp.dec, nullptr, 0, pcm.data(), kFrame, 0);
                            if (n > 0) Submit(sp, pcm.data(), n);
                        }
                        const int n = opus_decode(sp.dec, data, len, pcm.data(), kFrame, 1);
                        if (n > 0) Submit(sp, pcm.data(), n);
                    }
                }
                sp.lastSeq = p.seq;
                sp.hasSeq = true;
                const int n = opus_decode(sp.dec, data, len, pcm.data(), kMaxDecode, 0);
                if (n > 0)
                    Submit(sp, pcm.data(), n);
            }

            if (nowMs >= nextSweep)
            {
                nextSweep = nowMs + 5000;
                for (auto it = speakers.begin(); it != speakers.end(); )
                {
                    if (nowMs - it->second->lastHeardMs > 30000)
                    {
                        it->second->voice->DestroyVoice();
                        opus_decoder_destroy(it->second->dec);
                        it = speakers.erase(it);
                    }
                    else
                        ++it;
                }
            }
        }

        for (auto& kv : speakers)
        {
            kv.second->voice->DestroyVoice();
            opus_decoder_destroy(kv.second->dec);
        }
        speakers.clear();
        master->DestroyVoice();
        xa->Release();
        FreeLibrary(dll);
        if (SUCCEEDED(co)) CoUninitialize();
        LOG_INFO("[Voice] Playback closed");
    }

    // ── Host routing (CoSync tick) ──────────────────────────────────────────
    bool SameLocation(uint32_t wsA, uint32_t cellA, uint32_t wsB, uint32_t cellB)
    {
        if (wsA == 0xFFFFFFFFu || wsB == 0xFFFFFFFFu || wsA != wsB)
            return false;
        return wsA != 0 || (cellA != 0 && cellA == cellB);   // interior: same cell
    }

    bool LocateEntity(uint32_t eid, uint32_t& ws, uint32_t& cell, float p[3])
    {
        return g_CoSyncPlayerManager.GetEntityLocation(eid, ws, cell) &&
               g_CoSyncPlayerManager.GetEntityPosition(eid, p[0], p[1], p[2]);
    }

    // conn 0 = the host's own microphone.
    void HostRoute(uint32_t senderEid, const std::string& senderKey, char mode, uint32_t seq,
                   const char* data, size_t n)
    {
        struct Rcpt { uint32_t conn; uint32_t eid; };
        std::vector<Rcpt> all;
        const uint32_t hostEid = CoSyncNet::GetMyEntityID();
        all.push_back({ 0, hostEid });
        for (const auto& p : CoSyncNet::HostGetPeers())
            if (p.entityID != 0)
                all.push_back({ static_cast<uint32_t>(p.conn), p.entityID });

        std::vector<std::pair<uint32_t, uint8_t>> out;   // (conn, gain)
        if (mode == 'F')
        {
            std::vector<uint32_t> conns;
            std::string faction;
            if (senderKey.empty() || !CoSyncFactions::GetFactionAudience(senderKey, conns, faction))
                return;
            const std::unordered_set<uint32_t> allowed(conns.begin(), conns.end());
            for (const Rcpt& r : all)
                if (r.eid != senderEid && allowed.count(r.conn))
                    out.push_back({ r.conn, 100 });
        }
        else
        {
            uint32_t sws, scell;
            float sp[3];
            if (!LocateEntity(senderEid, sws, scell, sp))
                return;
            const float range = s_cfg.range, full = s_cfg.fullRange;
            for (const Rcpt& r : all)
            {
                if (r.eid == senderEid)
                    continue;
                uint32_t ws, cell;
                float rp[3];
                if (!LocateEntity(r.eid, ws, cell, rp) || !SameLocation(sws, scell, ws, cell))
                    continue;
                const float dx = rp[0] - sp[0], dy = rp[1] - sp[1], dz = rp[2] - sp[2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d >= range)
                    continue;
                float g = 1.f;
                if (d > full)
                {
                    const float t = (range - d) / (range - full);
                    g = t * t;
                }
                const int gi = static_cast<int>(g * 100.f + 0.5f);
                if (gi >= 2)
                    out.push_back({ r.conn, static_cast<uint8_t>(gi) });
            }
        }
        if (out.empty())
            return;

        const std::string head = "VOICEM|" + std::to_string(senderEid) + "|" + mode + "|" + std::to_string(seq) + "|";
        for (const auto& o : out)
        {
            if (o.first == 0)
                EnqueuePlay(senderEid, seq, o.second, data, n);
            else
            {
                std::string wire = head + std::to_string(o.second) + "|";
                wire.append(data, n);
                CoSyncTransport::SendTo(o.first, wire, false);
            }
        }
    }

    // Splits the first `fields` '|'-separated fields; the rest is the payload.
    bool SplitHeader(const std::string& msg, size_t skip, int fields, std::vector<std::string>& out, size_t& payloadAt)
    {
        out.clear();
        size_t a = skip;
        for (int i = 0; i < fields; ++i)
        {
            const size_t b = msg.find('|', a);
            if (b == std::string::npos || b - a > 12)
                return false;
            out.push_back(msg.substr(a, b - a));
            a = b + 1;
        }
        payloadAt = a;
        return a < msg.size() && msg.size() - a <= kMaxPayload;
    }

    void StopThreads()
    {
        s_stop.store(true);
        if (s_playEvent)
            SetEvent(s_playEvent);
        for (std::thread** t : { &s_captureThread, &s_playThread })
        {
            if (*t && (*t)->joinable())
                (*t)->join();
            delete *t;
            *t = nullptr;
        }
        s_stop.store(false);
    }
}

namespace CoSyncVoice
{
    void OnSessionStart()
    {
        LOG_REQUIREMENT("CoSyncVoice::OnSessionStart", "CoSync tick. Starts the capture and playback threads when [Voice] bEnabled.");
        LoadConfig();
        StopThreads();
        {
            std::lock_guard<std::mutex> lk(s_outMx);
            s_outbox.clear();
        }
        {
            std::lock_guard<std::mutex> lk(s_playMx);
            s_playQ.clear();
        }
        {
            std::lock_guard<std::mutex> lk(s_talkMx);
            s_talkUntil.clear();
        }
        s_budget.clear();
        s_mode.store('P');
        if (!s_cfg.enabled)
        {
            LOG_INFO("[Voice] Disabled in CoSync.ini ([Voice] bEnabled=0)");
            return;
        }
        if (!s_playEvent)
            s_playEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        s_session.store(true);
        s_playThread = new std::thread(PlayMain);
        s_captureThread = new std::thread(CaptureMain);
    }

    void OnSessionEnd()
    {
        s_session.store(false);
        StopThreads();
        std::lock_guard<std::mutex> lk(s_talkMx);
        s_talkUntil.clear();
    }

    bool IsVoiceMessage(const std::string& msg)
    {
        return msg.rfind("VOICE|", 0) == 0 || msg.rfind("VOICEM|", 0) == 0;
    }

    void OnReceive(const std::string& msg, uint32_t conn, uint32_t senderEid, bool isHostRole)
    {
        if (!s_session.load())
            return;
        std::vector<std::string> f;
        size_t at = 0;

        if (msg.rfind("VOICE|", 0) == 0)
        {
            if (!isHostRole || conn == 0 || senderEid == 0)
                return;
            if (!SplitHeader(msg, 6, 2, f, at) || f[0].size() != 1 || (f[0][0] != 'P' && f[0][0] != 'F'))
                return;
            const uint64_t nowMs = GetTickCount64();
            Budget& b = s_budget[conn];
            if (nowMs - b.windowStart >= 1000) { b.windowStart = nowMs; b.count = 0; }
            if (++b.count > 60)
                return;
            const uint32_t seq = static_cast<uint32_t>(std::strtoul(f[1].c_str(), nullptr, 10));
            HostRoute(senderEid, CoSyncProfiles::HostGetConnKey(conn), f[0][0], seq, msg.data() + at, msg.size() - at);
            return;
        }

        // VOICEM: only from the host.
        if (isHostRole)
            return;
        if (!SplitHeader(msg, 7, 4, f, at))
            return;
        const uint32_t eid  = static_cast<uint32_t>(std::strtoul(f[0].c_str(), nullptr, 10));
        const uint32_t seq  = static_cast<uint32_t>(std::strtoul(f[2].c_str(), nullptr, 10));
        const unsigned long gain = std::strtoul(f[3].c_str(), nullptr, 10);
        if (eid == 0 || eid == CoSyncNet::GetMyEntityID() || gain > 100)
            return;
        EnqueuePlay(eid, seq, static_cast<uint8_t>(gain), msg.data() + at, msg.size() - at);
    }

    void Tick(double now)
    {
        (void)now;
        if (!s_session.load())
            return;

        // Faction mode needs a faction; fall back to proximity when you leave it.
        static uint64_t s_nextFactionCheck = 0;
        const uint64_t nowMs = GetTickCount64();
        if (nowMs >= s_nextFactionCheck)
        {
            s_nextFactionCheck = nowMs + 1000;
            CoSyncFactions::Badge badge;
            const bool in = CoSyncFactions::GetBadge(CoSyncNet::GetMyEntityID(), badge) && !badge.faction.empty();
            s_inFaction.store(in);
            if (!in && s_mode.load() == 'F')
            {
                s_mode.store('P');
                LOG_INFO("[Voice] Not in a faction any more -- back to proximity voice");
            }
        }

        std::deque<OutFrame> out;
        {
            std::lock_guard<std::mutex> lk(s_outMx);
            out.swap(s_outbox);
        }
        if (out.empty() || !CoSyncTransport::IsInitialized() || !CoSyncNet::IsConnected())
            return;
        const bool host = CoSyncTransport::IsHost();
        const uint32_t me = CoSyncNet::GetMyEntityID();
        const std::string myKey = host ? CoSyncProfiles::GetMyProfileKey() : std::string();
        for (const OutFrame& fr : out)
        {
            if (host)
                HostRoute(me, myKey, fr.mode, fr.seq, fr.data.data(), fr.data.size());
            else
            {
                std::string wire = std::string("VOICE|") + fr.mode + "|" + std::to_string(fr.seq) + "|";
                wire += fr.data;
                CoSyncTransport::Send(wire, false);
            }
        }
    }

    int ModeKey() { LoadConfig(); return s_cfg.modeKey; }
    int MuteKey() { LoadConfig(); return s_cfg.muteKey; }

    void CycleMode()
    {
        const char next = (s_mode.load() == 'P' && s_inFaction.load()) ? 'F' : 'P';
        s_mode.store(next);
        LOG_INFO("[Voice] Mode: %s", next == 'F' ? "faction" : "proximity");
    }

    void ToggleMute()
    {
        const bool m = !s_muted.load();
        s_muted.store(m);
        LOG_INFO("[Voice] Microphone %s", m ? "muted" : "unmuted");
    }

    Status GetStatus()
    {
        Status s;
        s.enabled = s_session.load();
        s.micOk = s_micOk.load();
        s.mode = s_mode.load();
        s.muted = s_muted.load();
        s.transmitting = s_tx.load();
        s.pushToTalk = s_cfg.pttKey != 0;
        s.inFaction = s_inFaction.load();
        return s;
    }

    bool IsTalking(uint32_t entityID)
    {
        if (entityID != 0 && entityID == CoSyncNet::GetMyEntityID())
            return s_tx.load();
        std::lock_guard<std::mutex> lk(s_talkMx);
        auto it = s_talkUntil.find(entityID);
        return it != s_talkUntil.end() && GetTickCount64() < it->second;
    }
}
