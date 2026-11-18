#include "Diag.h"

#include <kodi/AddonBase.h>

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
// ------------------------------------------------------------------ 内部状态 --
std::mutex g_mutex;
bool g_done = false;
bool g_ready = false;
std::string g_path;

constexpr const char* kReportName = "inputstream.vivid-diag.txt";

// 报告可能落在这些位置，按顺序取第一个可写的。
// /storage 是 CoreELEC 的持久分区，重启不会被清掉，优先用它。
const char* kCandidates[] = {
    "/storage", "/storage/.kodi/temp", "/tmp", ".",
};

// 插件代码实际 dlsym 的那套符号（作者臆造，疑似不存在）
const char* kWantSyms[] = {
    "aml_vid_new", "aml_vid_open", "aml_vid_decode", "aml_vid_get", "aml_vid_close",
    "aml_aud_new", "aml_aud_open", "aml_aud_decode", "aml_aud_get", "aml_aud_close",
};

// Amlogic libamcodec 真实 API（codec_para_t * 那一套）
const char* kRealSyms[] = {
    "codec_init",        "codec_write",         "codec_read",     "codec_close",
    "codec_checkin_pts", "codec_set_cntl_mode", "codec_reset",    "codec_alloc",
    "codec_free",        "codec_set_src_fmt",
};

// 依次尝试打开的库名
const char* kLibs[] = {
    "libamcodec.so", "libamcodec.so.1", "libamcodec.so.0",
    "libamadec.so",  "libamavutils.so", "libamvpu.so",
};

constexpr int kWantCount = static_cast<int>(sizeof(kWantSyms) / sizeof(kWantSyms[0]));
constexpr int kRealCount = static_cast<int>(sizeof(kRealSyms) / sizeof(kRealSyms[0]));

// ---------------------------------------------------------------- 文本工具 --
bool PickPath()
{
  if (g_ready)
    return !g_path.empty();

  g_ready = true;
  for (const char* dir : kCandidates)
  {
    std::string p = std::string(dir) + "/" + kReportName;
    FILE* f = fopen(p.c_str(), "a");
    if (f)
    {
      fclose(f);
      g_path = p;
      return true;
    }
  }
  return false;
}

void WriteRaw(const std::string& line)
{
  if (g_path.empty())
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  FILE* f = fopen(g_path.c_str(), "a");
  if (!f)
    return;
  fwrite(line.data(), 1, line.size(), f);
  fputc('\n', f);
  fclose(f);
}

void Line(const char* fmt, ...)
{
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  WriteRaw(buf);
}

// 读整个文本文件，把 NUL 换成空格，截断过长的内容
std::string ReadFile(const char* path, size_t maxBytes = 4096)
{
  FILE* f = fopen(path, "rb");
  if (!f)
    return "";
  std::vector<char> buf(maxBytes + 1, 0);
  size_t n = fread(buf.data(), 1, maxBytes, f);
  fclose(f);
  std::string s(buf.data(), n);
  for (char& c : s)
  {
    if (c == '\0')
      c = ' ';
  }
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
    s.pop_back();
  return s;
}

// 只读 ELF 头前 20 字节，判断位宽与机器类型
std::string ElfInfo(const std::string& path)
{
  FILE* f = fopen(path.c_str(), "rb");
  if (!f)
    return "无法读取";
  unsigned char b[20] = {0};
  size_t n = fread(b, 1, 20, f);
  fclose(f);
  if (n < 20)
    return "文件过短";

  const char* cls = (b[4] == 2) ? "ELF64" : ((b[4] == 1) ? "ELF32" : "ELF?");
  unsigned int m = static_cast<unsigned int>(b[18]) | (static_cast<unsigned int>(b[19]) << 8);
  const char* mach = nullptr;
  switch (m)
  {
    case 183: mach = "aarch64";   break;
    case 40:  mach = "armv7(32)"; break;
    case 62:  mach = "x86_64";    break;
    case 3:   mach = "i386";      break;
    default:  break;
  }
  char buf[64];
  if (mach)
    snprintf(buf, sizeof(buf), "%s/%s", cls, mach);
  else
    snprintf(buf, sizeof(buf), "%s/machine%u", cls, m);
  return buf;
}

bool FileExists(const std::string& p)
{
  struct stat st{};
  return stat(p.c_str(), &st) == 0;
}

// 扫描 /proc/self/maps，返回命中的库路径（去重，最多 6 条）
std::vector<std::string> ScanMaps(const char* needle)
{
  std::vector<std::string> hits;
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f)
    return hits;

  char buf[1024];
  while (fgets(buf, sizeof(buf), f))
  {
    if (!strstr(buf, needle))
      continue;

    char* p = strrchr(buf, ' ');
    std::string path = p ? std::string(p + 1) : std::string(buf);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
      path.pop_back();
    if (path.empty())
      continue;

    bool dup = false;
    for (const auto& h : hits)
    {
      if (h == path)
      {
        dup = true;
        break;
      }
    }
    if (!dup)
      hits.push_back(path);
    if (hits.size() >= 6)
      break;
  }
  fclose(f);
  return hits;
}

void ListDevNodes()
{
  DIR* d = opendir("/dev");
  if (!d)
  {
    Line("  /dev 不可读");
    return;
  }
  std::vector<std::string> names;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr)
  {
    const char* n = e->d_name;
    if (strncmp(n, "am", 2) == 0 || strncmp(n, "video", 5) == 0 ||
        strncmp(n, "dvb", 3) == 0 || strncmp(n, "dma_heap", 8) == 0 ||
        strncmp(n, "media", 5) == 0)
      names.emplace_back(n);
    if (names.size() >= 40)
      break;
  }
  closedir(d);

  if (names.empty())
  {
    Line("  未发现 am* / video* / dvb* / dma_heap / media* 节点");
    return;
  }
  for (const auto& n : names)
    Line("  /dev/%s", n.c_str());
}

// -------------------------------------------------------------- 库探测主体 --
// 打开一个库，打印两套符号表，把命中数量回填给调用方
void ProbeLibrary(const char* name, int& wantFound, int& realFound)
{
  wantFound = 0;
  realFound = 0;

  Line("");
  Line("--- dlopen(\"%s\") ---", name);

  dlerror();
  void* h = dlopen(name, RTLD_LAZY);
  if (!h)
  {
    const char* err = dlerror();
    Line("  打开失败: %s", err ? err : "(无错误信息)");
    return;
  }
  Line("  打开成功, handle=%p", h);

  // 顺带报一下真实文件与架构
  const char* sysDirs[] = {"/usr/lib", "/usr/lib64", "/lib", "/lib64", "/usr/lib/kodi"};
  for (const char* d : sysDirs)
  {
    std::string p = std::string(d) + "/" + name;
    if (FileExists(p))
    {
      Line("  文件: %s", p.c_str());
      Line("  架构: %s", ElfInfo(p).c_str());
      break;
    }
  }

  Line("");
  Line("  [插件期望的符号（aml_vid_* / aml_aud_*）]");
  for (const char* s : kWantSyms)
  {
    void* p = dlsym(h, s);
    if (p)
      wantFound++;
    Line("    %-18s %s", s, p ? "存在" : "NULL");
  }

  Line("");
  Line("  [Amlogic 真实 amcodec API（codec_*）]");
  for (const char* s : kRealSyms)
  {
    void* p = dlsym(h, s);
    if (p)
      realFound++;
    Line("    %-20s %s", s, p ? "存在" : "NULL");
  }

  // RTLD_DEFAULT 只反映「Kodi 主程序是否已把它加载进全局符号空间」
  Line("");
  Line("  [全局可见（RTLD_DEFAULT，说明 Kodi 自己已加载）]");
  bool anyGlobal = false;
  for (const char* s : kRealSyms)
  {
    if (dlsym(RTLD_DEFAULT, s))
    {
      Line("    %-20s 全局可见", s);
      anyGlobal = true;
    }
  }
  if (!anyGlobal)
    Line("    (无) —— 可能只是本插件 dlopen 的，Kodi 未全局加载");
}

void RunDiagnosis(const char* stage)
{
  Line("");
  Line("==================================================================");
  Line("inputstream.vivid 自诊断   触发点: %s", stage);
  Line("==================================================================");

  // 1. 构建信息：证明这个 .so 用什么工具链、什么架构编出来的
  Line("");
  Line("[1] 构建信息");
#if defined(__aarch64__)
  Line("  目标架构: aarch64 (64 位 ARM)");
#elif defined(__arm__)
  Line("  目标架构: armv7 (32 位 ARM)");
#elif defined(__x86_64__)
  Line("  目标架构: x86_64 —— 装错包了");
#else
  Line("  目标架构: 未知");
#endif
  Line("  指针宽度: %d 字节 (%s)", static_cast<int>(sizeof(void*)),
       sizeof(void*) == 8 ? "64 位" : "32 位");
  Line("  编译器:   %s", __VERSION__);
  Line("  构建时间: %s %s", __DATE__, __TIME__);
  Line("  __LP64__: %s",
#ifdef __LP64__
       "是"
#else
       "否"
#endif
  );

  // 2. 运行环境
  Line("");
  Line("[2] 运行环境");
  struct utsname u{};
  if (uname(&u) == 0)
    Line("  内核: %s %s %s", u.sysname, u.release, u.machine);
  Line("  os-release: %s", ReadFile("/etc/os-release", 400).c_str());
  Line("  设备型号: %s", ReadFile("/proc/device-tree/model", 200).c_str());
  Line("  compatible: %s", ReadFile("/proc/device-tree/compatible", 200).c_str());
  Line("  LD_LIBRARY_PATH: %s",
       getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "(未设置)");

  // 3. 硬解库与符号
  Line("");
  Line("[3] Amlogic 解码库探测");
  int totalWant = 0;
  int totalReal = 0;
  int libsOpened = 0;
  for (const char* lib : kLibs)
  {
    int w = 0;
    int r = 0;
    ProbeLibrary(lib, w, r);
    if (w > 0 || r > 0)
      libsOpened++;
    if (w > totalWant)
      totalWant = w;
    if (r > totalReal)
      totalReal = r;
  }

  // 4. Kodi 自己加载了什么（这条最能说明实际解码路径）
  Line("");
  Line("[4] 当前进程已加载的多媒体库（/proc/self/maps）");
  const char* needles[] = {"amcodec", "amvdec", "libamav", "libamvpu", "libv4l2", "dma_buf"};
  bool mapsAmcodec = false;
  for (const char* n : needles)
  {
    auto hits = ScanMaps(n);
    if (hits.empty())
    {
      Line("  %-10s 未加载", n);
      continue;
    }
    if (strcmp(n, "amcodec") == 0)
      mapsAmcodec = true;
    for (const auto& h : hits)
      Line("  %-10s %s", n, h.c_str());
  }

  // 5. HDR / Vivid 能力
  Line("");
  Line("[5] HDR / Vivid 能力（sysfs）");
  const char* sysfs[] = {
      "/sys/class/amhdmitx/amhdmitx0/hdr_cap",
      "/sys/class/amhdmitx/amhdmitx0/dv_cap",
      "/sys/class/amhdmitx/amhdmitx0/disp_cap",
      "/sys/class/amhdmitx/amhdmitx0/hdr_cap_vivid",
      "/sys/class/amhdmitx/amhdmitx0/dc_cap",
  };
  bool anySysfs = false;
  bool sysfsVivid = false;
  for (const char* f : sysfs)
  {
    if (!FileExists(f))
      continue;
    anySysfs = true;
    std::string content = ReadFile(f, 800);
    if (content.find("VIVID") != std::string::npos || content.find("vivid") != std::string::npos)
      sysfsVivid = true;
    Line("  --- %s ---", f);
    Line("  %s", content.c_str());
  }
  if (!anySysfs)
    Line("  未找到 amhdmitx 能力节点（内核可能换了驱动）");

  // 6. 设备节点
  Line("");
  Line("[6] 设备节点");
  ListDevNodes();

  // 7. 结论
  Line("");
  Line("==================================================================");
  Line("[7] 结论");
  Line("==================================================================");
  Line("  可用解码库数量:            %d", libsOpened);
  Line("  插件期望 aml_vid_*:        %d / %d", totalWant, kWantCount);
  Line("  真实 codec_*:              %d / %d", totalReal, kRealCount);
  Line("  Kodi 进程内已映射 amcodec: %s", mapsAmcodec ? "是" : "否");
  Line("  HDMI/内核提到 Vivid:       %s", sysfsVivid ? "有" : "无（或未暴露）");
  Line("");
  if (totalWant == 0 && totalReal > 0)
  {
    Line("  → 本机库里有 codec_* 没有 aml_vid_*：VividDecoder 的解码路径不可能生效");
    Line("    （已判空，不会崩）。HDR Vivid 只能依赖 Kodi 自身 amcodec 通路。");
  }
  else if (totalWant > 0)
  {
    Line("  → 库里确实存在 aml_vid_*，与公开 amcodec API 不符，请把本报告发回重新评估。");
  }
  else if (totalReal == 0)
  {
    Line("  → 既没有 aml_vid_* 也没有 codec_*：NO 分支的解码接口必须重写。");
  }
  Line("");
  Line("报告文件: %s", g_path.c_str());
  Line("==================================================================");
}
} // namespace

// ================================================================ 对外接口 ==
void DIAG::RunOnce(const char* stage)
{
  try
  {
    if (g_done)
      return;
    if (!PickPath())
      return;
    g_done = true;

    RunDiagnosis(stage ? stage : "unknown");

    // kodi.log 里留摘要：用户只发 kodi.log 也能看到报告在哪
    kodi::Log(ADDON_LOG_INFO, "[%s] 自诊断完成，报告: %s", "inputstream.vivid",
              g_path.c_str());
  }
  catch (...)
  {
    // 诊断本身绝不能拖垮插件
  }
}

void DIAG::Note(const char* fmt, ...)
{
  try
  {
    if (g_path.empty())
      return;
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    WriteRaw(std::string("  [note] ") + buf);
  }
  catch (...)
  {
  }
}

const char* DIAG::ReportPath()
{
  return g_path.c_str();
}
