// Yamaha MODX Live Set Controller - CLAP port of MODX_LiveSet_Controller_Simple.jsfx
//
// A MIDI/note effect with four parameters:
//   Live Set Library  (Preset, User 1-8, Library 1-8)  -> SysEx  F0 43 10 7F 1C 07 09 00 00 XX F7
//   Live Set Page     (1-16)                           -> Bank LSB = page-1
//   Live Set Slot     (1-16, custom names from .reabank files)  -> Program Change = slot-1
//   MIDI Channel for Bank/Program (1-16)
//
// Whenever Library, Page or Slot changes, the plugin sends (in this order):
//   SysEx, CC0 = 62 (Bank MSB), CC32 = page-1 (Bank LSB), Program Change = slot-1
// Nothing is sent when a project loads. Incoming MIDI/note events pass through untouched.

#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef MODX_SEND_ON_PRESET_LOAD
#define MODX_SEND_ON_PRESET_LOAD 1
#endif
#ifndef MODX_SYSEX_WITH_FRAMING
#define MODX_SYSEX_WITH_FRAMING 1
#endif

namespace fs = std::filesystem;

namespace {

constexpr int kLibs = 17;   // 00 Preset, 01-08 User 1-8, 09-16 Library 1-8
constexpr int kPages = 16;
constexpr int kSlots = 16;
constexpr int kMaxSysexPerBlock = 32;

enum ParamIds : clap_id { P_LIB = 0, P_PAGE = 1, P_SLOT = 2, P_CH = 3, P_COUNT = 4 };

const char* const kLibNames[kLibs] = {
    "Preset",    "User 1",    "User 2",    "User 3",    "User 4",    "User 5",
    "User 6",    "User 7",    "User 8",    "Library 1", "Library 2", "Library 3",
    "Library 4", "Library 5", "Library 6", "Library 7", "Library 8"};

const char* const kParamNames[P_COUNT] = {"Live Set Library", "Live Set Page", "Live Set Slot",
                                          "MIDI Channel for Bank/Program"};
const int kParamMax[P_COUNT] = {kLibs - 1, kPages - 1, kSlots - 1, 15};

const char* const kPluginFeatures[] = {CLAP_PLUGIN_FEATURE_NOTE_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
                                       nullptr};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "com.harleyganiere.modx-liveset-controller",
    "MODX Live Set Controller",
    "Harley Ganiere",
    "",
    "",
    "",
    "1.1.1",
    "Selects Live Sets on a Yamaha MODX (SysEx + Bank/Program Change)",
    kPluginFeatures};

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool iequals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

struct Plugin {
  clap_plugin_t plugin{};
  const clap_host_t* host = nullptr;
  const clap_host_params_t* hostParams = nullptr;

  std::atomic<int> v[P_COUNT];
  std::atomic<bool> pendingSend{false};
  std::atomic<bool> textDirty{false};
  // True once process() has run since activation. A state load before that is a project
  // load (stay silent); a state load after that is a preset change (send the select).
  std::atomic<bool> hasProcessed{false};

  // Slot names from the .reabank files: names[library][page][slot]. Empty = use default.
  std::string names[kLibs][kPages][kSlots];
  // Library display names taken from the reabank file names. Empty = use the built-in label.
  std::string libLabels[kLibs];

  // SysEx buffers must stay valid until process() returns.
  uint8_t sysexBuf[kMaxSysexPerBlock][11];
  int sysexUsed = 0;

  Plugin() {
    for (auto& a : v) a.store(0);
  }

  // ---------------------------------------------------------------- reabank loading
  // Folder holding the .reabank files (independent of any host):
  //   $MODX_CONTROLLER_DIR if set, otherwise
  //   Windows: %APPDATA%\MODX_Controller
  //   macOS:   ~/Library/Application Support/MODX_Controller
  //   Linux:   $XDG_CONFIG_HOME/MODX_Controller  (default ~/.config/MODX_Controller)
  static fs::path dataDir() {
    if (const char* e = std::getenv("MODX_CONTROLLER_DIR"))
      if (*e) return fs::path(e);
#if defined(_WIN32)
    if (const char* a = std::getenv("APPDATA")) return fs::path(a) / "MODX_Controller";
#elif defined(__APPLE__)
    if (const char* h = std::getenv("HOME")) return fs::path(h) / "Library/Application Support/MODX_Controller";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"))
      if (*x) return fs::path(x) / "MODX_Controller";
    if (const char* h = std::getenv("HOME")) return fs::path(h) / ".config/MODX_Controller";
#endif
    return fs::path();
  }

  void parseReabank(const fs::path& file, int lib) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return;
    std::string line;
    int page = -1;
    bool first = true;
    while (std::getline(in, line)) {
      if (first) {  // strip UTF-8 BOM
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
          line.erase(0, 3);
        first = false;
      }
      line = trim(line);
      if (line.empty() || line[0] == ';' || line[0] == '/') continue;
      if (line.size() > 4 && iequals(line.substr(0, 4), "Bank") && std::isspace(static_cast<unsigned char>(line[4]))) {
        int msb = -1, lsb = -1;
        if (std::sscanf(line.c_str() + 4, "%d %d", &msb, &lsb) == 2 && msb == 62 && lsb >= 0 && lsb < kPages)
          page = lsb;
        else
          page = -1;
        continue;
      }
      int n = -1;
      if (page >= 0 && std::sscanf(line.c_str(), "%d", &n) == 1 && n >= 0 && n < kSlots) {
        size_t q1 = line.find('"');
        size_t q2 = line.rfind('"');
        if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1)
          names[lib][page][n] = line.substr(q1 + 1, q2 - q1 - 1);
      }
    }
  }

  // File name -> (library number, display name).
  //   "MODX_LiveSet_01_Worship_Set.reabank" -> 1, "Worship Set"
  //   "03_Gigs.reabank"                     -> 3, "Gigs"
  // The number XX picks which Live Set source the MODX is told to use; the rest is just the label.
  static bool parseFileName(const fs::path& path, int* lib, std::string* label) {
    std::string stem = path.stem().string();
    const std::string prefix = "MODX_LiveSet_";
    if (stem.size() >= prefix.size() && iequals(stem.substr(0, prefix.size()), prefix)) stem.erase(0, prefix.size());
    size_t i = 0;
    int num = 0;
    while (i < stem.size() && i < 2 && std::isdigit(static_cast<unsigned char>(stem[i]))) num = num * 10 + (stem[i++] - '0');
    if (i == 0 || num >= kLibs) return false;
    if (i < stem.size() && std::isdigit(static_cast<unsigned char>(stem[i]))) return false;  // 3+ digits
    while (i < stem.size() && (stem[i] == '_' || stem[i] == '-' || stem[i] == ' ')) ++i;
    std::string name = stem.substr(i);
    std::replace(name.begin(), name.end(), '_', ' ');
    *lib = num;
    *label = trim(name);
    return true;
  }

  void loadNames() {
    for (auto& l : names)
      for (auto& p : l)
        for (auto& sl : p) sl.clear();
    for (auto& l : libLabels) l.clear();

    const fs::path dir = dataDir();
    if (dir.empty()) return;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
      fs::create_directories(dir, ec);  // so the user has a place to put the files
      return;
    }
    std::vector<fs::path> files;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
      if (iequals(it->path().extension().string(), ".reabank")) files.push_back(it->path());
    std::sort(files.begin(), files.end());  // deterministic; if two files share a number the first wins
    bool seen[kLibs] = {};
    for (const auto& f : files) {
      int lib;
      std::string label;
      if (!parseFileName(f, &lib, &label) || seen[lib]) continue;
      seen[lib] = true;
      libLabels[lib] = label;
      parseReabank(f, lib);
    }
  }

  // ---------------------------------------------------------------- parameter text
  std::string valueText(clap_id id, int val) const {
    char b[64];
    switch (id) {
      case P_LIB: {
        const int l = std::clamp(val, 0, kLibs - 1);
        return libLabels[l].empty() ? std::string(kLibNames[l]) : libLabels[l];
      }
      case P_PAGE:
        std::snprintf(b, sizeof b, "Page %d", val + 1);
        return b;
      case P_SLOT: {
        const int lib = v[P_LIB].load(), page = v[P_PAGE].load();
        const std::string& n = names[std::clamp(lib, 0, kLibs - 1)][std::clamp(page, 0, kPages - 1)]
                                    [std::clamp(val, 0, kSlots - 1)];
        if (!n.empty()) return n;
        std::snprintf(b, sizeof b, "Slot %d", val + 1);
        return b;
      }
      default:
        std::snprintf(b, sizeof b, "%d", val + 1);
        return b;
    }
  }

  bool textToValue(clap_id id, const std::string& txt, double* out) const {
    const int maxv = kParamMax[id];
    if (id == P_LIB || id == P_SLOT) {
      const int n = (id == P_LIB) ? kLibs : kSlots;
      for (int i = 0; i < n; ++i)
        if (iequals(valueText(id, i), trim(txt))) {
          *out = i;
          return true;
        }
    }
    const char* p = txt.c_str();
    while (*p && !std::isdigit(static_cast<unsigned char>(*p))) ++p;
    if (!*p) return false;
    int n = std::atoi(p);
    if (id == P_LIB) {
      // a bare number means the library number 0..16
      *out = std::clamp(n, 0, maxv);
    } else {
      *out = std::clamp(n - 1, 0, maxv);
    }
    return true;
  }

  // ---------------------------------------------------------------- parameter events
  // Returns true if this event requires a Live Set select message to be sent.
  bool applyParam(const clap_event_param_value_t* e) {
    if (e->param_id >= P_COUNT) return false;
    const int nv = std::clamp(static_cast<int>(std::lround(e->value)), 0, kParamMax[e->param_id]);
    const int old = v[e->param_id].exchange(nv);
    if (nv == old) return false;
    if (e->param_id == P_LIB || e->param_id == P_PAGE) {
      textDirty.store(true);
      if (host && host->request_callback) host->request_callback(host);
    }
    return e->param_id != P_CH;  // channel change alone sends nothing
  }

  // ---------------------------------------------------------------- output
  static void pushMidi(const clap_output_events_t* out, uint32_t time, uint8_t b0, uint8_t b1, uint8_t b2) {
    clap_event_midi_t ev{};
    ev.header.size = sizeof(ev);
    ev.header.time = time;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_MIDI;
    ev.header.flags = 0;
    ev.port_index = 0;
    ev.data[0] = b0;
    ev.data[1] = b1;
    ev.data[2] = b2;
    out->try_push(out, &ev.header);
  }

  void emitSelect(const clap_output_events_t* out, uint32_t time) {
    const int lib = v[P_LIB].load();
    const int page = v[P_PAGE].load();
    const int slot = v[P_SLOT].load();
    const int ch = v[P_CH].load() & 15;

    // 1) SysEx first so the MODX is on the right Live Set source before the bank select.
    if (sysexUsed < kMaxSysexPerBlock) {
      uint8_t* b = sysexBuf[sysexUsed++];
      const uint8_t msg[11] = {0xF0, 0x43, 0x10, 0x7F, 0x1C, 0x07, 0x09, 0x00, 0x00,
                               static_cast<uint8_t>(lib), 0xF7};
      std::memcpy(b, msg, 11);
      clap_event_midi_sysex_t ev{};
      ev.header.size = sizeof(ev);
      ev.header.time = time;
      ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      ev.header.type = CLAP_EVENT_MIDI_SYSEX;
      ev.header.flags = 0;
      ev.port_index = 0;
#if MODX_SYSEX_WITH_FRAMING
      ev.buffer = b;
      ev.size = 11;
#else
      ev.buffer = b + 1;
      ev.size = 9;
#endif
      out->try_push(out, &ev.header);
    }
    // 2) Bank MSB 62, Bank LSB = page-1, Program Change = slot-1
    pushMidi(out, time, static_cast<uint8_t>(0xB0 | ch), 0, 62);
    pushMidi(out, time, static_cast<uint8_t>(0xB0 | ch), 32, static_cast<uint8_t>(page));
    pushMidi(out, time, static_cast<uint8_t>(0xC0 | ch), static_cast<uint8_t>(slot), 0);
  }
};

inline Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

// ------------------------------------------------------------------ note ports
uint32_t notePortsCount(const clap_plugin_t*, bool) { return 1; }

bool notePortsGet(const clap_plugin_t*, uint32_t index, bool is_input, clap_note_port_info_t* info) {
  if (index != 0) return false;
  info->id = 0;
  info->supported_dialects = CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP;
  info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
  std::snprintf(info->name, sizeof info->name, "%s", is_input ? "MIDI In" : "MIDI Out");
  return true;
}
const clap_plugin_note_ports_t kNotePorts = {notePortsCount, notePortsGet};

// ------------------------------------------------------------------ params
uint32_t paramsCount(const clap_plugin_t*) { return P_COUNT; }

bool paramsGetInfo(const clap_plugin_t*, uint32_t i, clap_param_info_t* info) {
  if (i >= P_COUNT) return false;
  std::memset(info, 0, sizeof *info);
  info->id = i;
  info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM;
  std::snprintf(info->name, sizeof info->name, "%s", kParamNames[i]);
  info->module[0] = '\0';
  info->min_value = 0;
  info->max_value = kParamMax[i];
  info->default_value = 0;
  return true;
}

bool paramsGetValue(const clap_plugin_t* p, clap_id id, double* value) {
  if (id >= P_COUNT) return false;
  *value = self(p)->v[id].load();
  return true;
}

bool paramsValueToText(const clap_plugin_t* p, clap_id id, double value, char* out, uint32_t size) {
  if (id >= P_COUNT) return false;
  const int iv = std::clamp(static_cast<int>(std::lround(value)), 0, kParamMax[id]);
  std::snprintf(out, size, "%s", self(p)->valueText(id, iv).c_str());
  return true;
}

bool paramsTextToValue(const clap_plugin_t* p, clap_id id, const char* text, double* out) {
  if (id >= P_COUNT) return false;
  return self(p)->textToValue(id, text, out);
}

void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) {
  Plugin* s = self(p);
  const uint32_t n = in->size(in);
  for (uint32_t i = 0; i < n; ++i) {
    const clap_event_header_t* h = in->get(in, i);
    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
      if (s->applyParam(reinterpret_cast<const clap_event_param_value_t*>(h))) s->pendingSend.store(true);
  }
}
const clap_plugin_params_t kParams = {paramsCount,       paramsGetInfo,    paramsGetValue,
                                      paramsValueToText, paramsTextToValue, paramsFlush};

// ------------------------------------------------------------------ state
struct StateBlob {
  uint32_t version;
  int32_t values[P_COUNT];
};

bool stateSave(const clap_plugin_t* p, const clap_ostream_t* stream) {
  Plugin* s = self(p);
  StateBlob b{1, {}};
  for (int i = 0; i < P_COUNT; ++i) b.values[i] = s->v[i].load();
  const char* ptr = reinterpret_cast<const char*>(&b);
  size_t left = sizeof b;
  while (left > 0) {
    const int64_t w = stream->write(stream, ptr, left);
    if (w <= 0) return false;
    ptr += w;
    left -= static_cast<size_t>(w);
  }
  return true;
}

bool stateLoad(const clap_plugin_t* p, const clap_istream_t* stream) {
  Plugin* s = self(p);
  StateBlob b{};
  char* ptr = reinterpret_cast<char*>(&b);
  size_t left = sizeof b;
  while (left > 0) {
    const int64_t r = stream->read(stream, ptr, left);
    if (r <= 0) return false;
    ptr += r;
    left -= static_cast<size_t>(r);
  }
  if (b.version != 1) return false;
  bool selectChanged = false;
  for (int i = 0; i < P_COUNT; ++i) {
    const int nv = std::clamp(static_cast<int>(b.values[i]), 0, kParamMax[i]);
    if (s->v[i].exchange(nv) != nv && i != P_CH) selectChanged = true;
  }
#if MODX_SEND_ON_PRESET_LOAD
  // Preset change while the plugin is running -> tell the MODX, like moving the sliders in the JSFX.
  // Loading a project (before processing has started) stays silent.
  if (selectChanged && s->hasProcessed.load()) s->pendingSend.store(true);
#else
  (void)selectChanged;
#endif
  s->textDirty.store(true);
  if (s->host && s->host->request_callback) s->host->request_callback(s->host);
  return true;
}
const clap_plugin_state_t kState = {stateSave, stateLoad};

// ------------------------------------------------------------------ plugin
bool pluginInit(const clap_plugin_t* p) {
  Plugin* s = self(p);
  s->hostParams = static_cast<const clap_host_params_t*>(s->host->get_extension(s->host, CLAP_EXT_PARAMS));
  s->loadNames();
  return true;
}

void pluginDestroy(const clap_plugin_t* p) { delete self(p); }

bool pluginActivate(const clap_plugin_t* p, double, uint32_t, uint32_t) {
  Plugin* s = self(p);
  s->hasProcessed.store(false);
  s->loadNames();  // pick up edited .reabank files each time the plugin is (re)activated
  if (s->hostParams) s->hostParams->rescan(s->host, CLAP_PARAM_RESCAN_TEXT);
  return true;
}

void pluginDeactivate(const clap_plugin_t*) {}
bool pluginStartProcessing(const clap_plugin_t*) { return true; }
void pluginStopProcessing(const clap_plugin_t*) {}
void pluginReset(const clap_plugin_t*) {}

bool isPassThrough(const clap_event_header_t* h) {
  if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return false;
  switch (h->type) {
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE:
    case CLAP_EVENT_NOTE_END:
    case CLAP_EVENT_NOTE_EXPRESSION:
    case CLAP_EVENT_MIDI:
    case CLAP_EVENT_MIDI_SYSEX:
    case CLAP_EVENT_MIDI2:
      return true;
    default:
      return false;
  }
}

clap_process_status pluginProcess(const clap_plugin_t* p, const clap_process_t* proc) {
  Plugin* s = self(p);
  s->hasProcessed.store(true);
  s->sysexUsed = 0;
  const clap_input_events_t* in = proc->in_events;
  const clap_output_events_t* out = proc->out_events;

  bool pending = s->pendingSend.exchange(false);  // from a param flush while idle
  uint32_t pendingTime = 0;

  const uint32_t n = in->size(in);
  for (uint32_t i = 0; i < n; ++i) {
    const clap_event_header_t* h = in->get(in, i);
    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
      if (s->applyParam(reinterpret_cast<const clap_event_param_value_t*>(h)) && !pending) {
        pending = true;
        pendingTime = h->time;
      }
    } else if (isPassThrough(h)) {
      if (pending) {  // keep output time-ordered: our select goes out before later events
        s->emitSelect(out, pendingTime);
        pending = false;
      }
      out->try_push(out, h);
    }
  }
  if (pending) s->emitSelect(out, pendingTime);
  return CLAP_PROCESS_CONTINUE;
}

const void* pluginGetExtension(const clap_plugin_t*, const char* id) {
  if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
  return nullptr;
}

void pluginOnMainThread(const clap_plugin_t* p) {
  Plugin* s = self(p);
  if (s->textDirty.exchange(false) && s->hostParams) s->hostParams->rescan(s->host, CLAP_PARAM_RESCAN_TEXT);
}

// ------------------------------------------------------------------ factory / entry
uint32_t factoryCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t i) {
  return i == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
  if (!clap_version_is_compatible(host->clap_version) || std::strcmp(id, kDescriptor.id) != 0) return nullptr;
  Plugin* s = new Plugin();
  s->host = host;
  s->plugin.desc = &kDescriptor;
  s->plugin.plugin_data = s;
  s->plugin.init = pluginInit;
  s->plugin.destroy = pluginDestroy;
  s->plugin.activate = pluginActivate;
  s->plugin.deactivate = pluginDeactivate;
  s->plugin.start_processing = pluginStartProcessing;
  s->plugin.stop_processing = pluginStopProcessing;
  s->plugin.reset = pluginReset;
  s->plugin.process = pluginProcess;
  s->plugin.get_extension = pluginGetExtension;
  s->plugin.on_main_thread = pluginOnMainThread;
  return &s->plugin;
}

const clap_plugin_factory_t kFactory = {factoryCount, factoryDescriptor, factoryCreate};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryGetFactory(const char* id) {
  return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit,
                                                                 entryGetFactory};
