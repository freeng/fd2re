/* audio.cpp — SDL 音频设备 + libADLMIDI 音乐 + wav 混音。
 * 设备参数与 fd2re/src/audio.c dig_open 一致：44100Hz S16 双声道。 */
#include "audio.h"

#include <cstdio>
#include <cstring>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <adlmidi.h>

#include "util.h"

void Audio::callback(void* userdata, uint8_t* stream, int len)
{
    Audio* self = static_cast<Audio*>(userdata);
    int16_t* out = reinterpret_cast<int16_t*>(stream);
    int frames = len / 4;                       /* S16 双声道 */

    if (self->adl_)
        adl_generate(self->adl_, frames * 2, out);
    else
        std::memset(out, 0, size_t(len));

    for (Voice& v : self->voices_) {
        if (v.id < 0 || size_t(v.id) >= self->wavs_.size())
            continue;
        const std::vector<uint8_t>& pcm = self->wavs_[size_t(v.id)];
        for (int f = 0; f < frames; f++) {
            size_t i = size_t(v.pos);
            if (i < pcm.size()) {
                int s = (int(pcm[i]) - 128) << 7;   /* 8bit unsigned → S16 量级 */
                int l = out[f * 2] + s, r = out[f * 2 + 1] + s;
                out[f * 2] = int16_t(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
                out[f * 2 + 1] = int16_t(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
            }
            v.pos += v.step;
            if (v.pos >= double(pcm.size())) {
                if (v.loop_left > 0) {
                    v.loop_left--;
                    v.pos = 0;
                } else {
                    v.id = -1;
                    break;
                }
            }
        }
    }
}

bool Audio::init()
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "fd2play: SDL audio: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = &Audio::callback;
    want.userdata = this;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev_ == 0) {
        std::fprintf(stderr, "fd2play: SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return false;
    }
    rate_ = have.freq;
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void Audio::shutdown()
{
    if (dev_) {
        SDL_LockAudioDevice(dev_);
        if (adl_) {
            adl_shutdown(adl_);
            adl_ = nullptr;
        }
        voices_.clear();
        SDL_UnlockAudioDevice(dev_);
        SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }
}

void Audio::lock()   { if (dev_) SDL_LockAudioDevice(dev_); }
void Audio::unlock() { if (dev_) SDL_UnlockAudioDevice(dev_); }

bool Audio::music_open(const std::string& mid_path, const std::string& bank_path,
                       std::string* err)
{
    music_close();
    ADL_MIDIPlayer* adl = adl_init(rate_);
    if (!adl) {
        if (err) *err = "adl_init failed";
        return false;
    }
    adl_setNumChips(adl, 1);                       /* 原版单 OPL3 */
    if (adl_openBankFile(adl, bank_path.c_str()) < 0) {
        if (err) *err = adl_errorInfo(adl);
        adl_shutdown(adl);
        return false;
    }
    adl_setVolumeRangeModel(adl, ADLMIDI_VolumeModel_AIL);
    if (adl_openFile(adl, mid_path.c_str()) < 0) {
        if (err) *err = adl_errorInfo(adl);
        adl_shutdown(adl);
        return false;
    }
    lock();
    adl_ = adl;
    unlock();
    return true;
}

void Audio::music_close()
{
    if (!dev_ || !adl_)
        return;
    lock();
    adl_shutdown(adl_);
    adl_ = nullptr;
    unlock();
}

bool Audio::music_at_end()
{
    if (!adl_)
        return true;
    lock();
    bool e = adl_atEnd(adl_) != 0;
    unlock();
    return e;
}

int Audio::wav_add(const std::vector<uint8_t>& pcm_8u_mono, int rate)
{
    lock();
    wavs_.push_back(pcm_8u_mono);
    wav_steps_.push_back(double(rate) / double(rate_));
    int id = int(wavs_.size()) - 1;
    unlock();
    return id;
}

void Audio::sfx_play(int id, int loop)
{
    lock();
    /* 同 id 重触发：直接重置位置（原版 AIL stop→start 语义） */
    for (Voice& v : voices_) {
        if (v.id == id) {
            v.pos = 0;
            v.loop_left = loop;
            unlock();
            return;
        }
    }
    Voice v;
    v.id = id;
    v.step = size_t(id) < wav_steps_.size() ? wav_steps_[size_t(id)] : 0.25;
    v.loop_left = loop;
    voices_.push_back(v);
    unlock();
}

void Audio::sfx_stop_all()
{
    lock();
    voices_.clear();
    unlock();
}
