/**
 * @file src/platform/windows/gabs/mic_sink_win.cpp
 * @brief Gabs Remote: WASAPI sink for the tablet microphone (playback side of VB-CABLE, decision 0007).
 */
// standard includes
#include <algorithm>
#include <cstring>

// platform includes
#include <windows.h>
#include <Audioclient.h>
#include <mmdeviceapi.h>

// local includes
#include "src/config.h"
#include "src/gabs/mic_service.h"
#include "src/logging.h"
#include "src/platform/windows/utf_utils.h"

// Defined in platform/windows/audio.cpp.
extern const PROPERTYKEY PKEY_Device_FriendlyName;

namespace gabs::mic {
  using namespace std::literals;

  namespace {
    template<class T>
    void release(T *&p) {
      if (p) {
        p->Release();
        p = nullptr;
      }
    }

    std::string device_id(IMMDevice *d) {
      LPWSTR id = nullptr;
      if (FAILED(d->GetId(&id)) || !id) {
        return {};
      }
      auto s = utf_utils::to_utf8(id);
      CoTaskMemFree(id);
      return s;
    }

    std::string friendly_name(IMMDevice *d) {
      IPropertyStore *props = nullptr;
      if (FAILED(d->OpenPropertyStore(STGM_READ, &props))) {
        return {};
      }
      PROPVARIANT v;
      PropVariantInit(&v);
      std::string name;
      if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) {
        name = utf_utils::to_utf8(v.pwszVal);
      }
      PropVariantClear(&v);
      props->Release();
      return name;
    }

    class wasapi_sink_t: public sink_t {
    public:
      explicit wasapi_sink_t(std::string wanted):
          wanted_ {std::move(wanted)} {
      }

      ~wasapi_sink_t() override {
        close();
      }

      bool open() override {
        close();
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_ = SUCCEEDED(hr);
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **) &enumerator_))) {
          BOOST_LOG(error) << "Gabs Remote: microfone: sem acesso aos dispositivos de áudio"sv;
          close();
          return false;
        }

        // The endpoint whose sound goes to the tablet: the default output (the host swaps it to the
        // virtual sink during the session). Writing the microphone there would loop it back.
        std::string capture_id;
        IMMDevice *def = nullptr;
        if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
          capture_id = device_id(def);
          def->Release();
        }

        std::vector<endpoint_t> endpoints;
        IMMDeviceCollection *coll = nullptr;
        if (SUCCEEDED(enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) {
          UINT count = 0;
          coll->GetCount(&count);
          for (UINT i = 0; i < count; ++i) {
            IMMDevice *d = nullptr;
            if (SUCCEEDED(coll->Item(i, &d))) {
              endpoints.push_back({device_id(d), friendly_name(d)});
              d->Release();
            }
          }
          coll->Release();
        }

        auto id = choose_mic_sink(endpoints, wanted_, capture_id);
        if (!id) {
          if (choose_mic_sink(endpoints, wanted_, {})) {
            BOOST_LOG(warning) << "Gabs Remote: microfone virtual \""sv << wanted_ << "\" é a saída padrão do Windows (o som da sessão sai dele); recusado para o tablet não ouvir o próprio microfone"sv;
          } else {
            BOOST_LOG(warning) << "Gabs Remote: microfone virtual \""sv << wanted_ << "\" não encontrado; rode tools\\setup\\instalar-audio.ps1"sv;
          }
          close();
          return false;
        }

        IMMDevice *dev = nullptr;
        if (FAILED(enumerator_->GetDevice(utf_utils::from_utf8(*id).c_str(), &dev))) {
          close();
          return false;
        }
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **) &client_);
        dev->Release();
        if (FAILED(hr)) {
          BOOST_LOG(error) << "Gabs Remote: microfone: Activate falhou ("sv << hr << ')';
          close();
          return false;
        }

        WAVEFORMATEX wf {};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 1;
        wf.nSamplesPerSec = 48000;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2;
        wf.nAvgBytesPerSec = 96000;
        constexpr REFERENCE_TIME buffer_100ns = 1'000'000;  // 100 ms
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, buffer_100ns, 0, &wf, nullptr);
        if (FAILED(hr) || FAILED(client_->GetBufferSize(&buffer_frames_)) || FAILED(client_->GetService(__uuidof(IAudioRenderClient), (void **) &render_))) {
          BOOST_LOG(error) << "Gabs Remote: microfone: não foi possível abrir o microfone virtual ("sv << hr << ')';
          close();
          return false;
        }
        client_->Start();
        BOOST_LOG(info) << "Gabs Remote: microfone do tablet tocando em \""sv << wanted_ << '"';
        return true;
      }

      void write(std::span<const std::int16_t> pcm) override {
        if (!client_ || !render_) {
          return;
        }
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) {
          return;
        }
        // More than 80 ms waiting in the device: our clock runs ahead of it; drop to keep the delay short.
        if (padding > 48000 * 80 / 1000) {
          return;
        }
        auto frames = std::min<UINT32>(buffer_frames_ - padding, static_cast<UINT32>(pcm.size()));
        if (frames == 0) {
          return;
        }
        BYTE *data = nullptr;
        if (SUCCEEDED(render_->GetBuffer(frames, &data))) {
          std::memcpy(data, pcm.data(), frames * sizeof(std::int16_t));
          render_->ReleaseBuffer(frames, 0);
        }
      }

      void close() override {
        if (client_) {
          client_->Stop();
        }
        release(render_);
        release(client_);
        release(enumerator_);
        if (com_) {
          CoUninitialize();
          com_ = false;
        }
      }

    private:
      std::string wanted_;
      bool com_ {false};
      IMMDeviceEnumerator *enumerator_ {nullptr};
      IAudioClient *client_ {nullptr};
      IAudioRenderClient *render_ {nullptr};
      UINT32 buffer_frames_ {0};
    };
  }  // namespace

  std::unique_ptr<sink_t> make_platform_sink(const std::string &endpoint_name) {
    return std::make_unique<wasapi_sink_t>(endpoint_name);
  }

  service_t &instance() {
    static service_t svc {make_platform_sink(config::audio.gr_mic_sink)};
    return svc;
  }
}  // namespace gabs::mic
