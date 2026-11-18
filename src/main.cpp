// inputstream.vivid — Kodi 22 (Piers) InputStream 插件
//
// 说明：
//   本文件为「架构适配验证版」，目标是让插件在 CoreELEC 22.0-Piers
//   (Amlogic-no / aarch64 / Kodi 22) 上能被正确加载、Open/Close/Seek 不崩溃。
//
//   当前实现为「字节流透传」：把 MKV/ISO 的原始数据交给 Kodi，
//   由 Kodi 自身的 demuxer + amcodec 播放器完成解封装与硬件解码。
//   并未自行解码，也不涉及 aml_* 私有接口。
//
//   真正的 HDR Vivid 硬解需要在此基础上接入 CoreELEC/no 上真实可用的解码 API
//   （见 README 的「后续工作」），不属于本验证版范围。

#include <kodi/addon-instance/Inputstream.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>

#include "Utils.h"

namespace
{
constexpr const char* LOGTAG = "inputstream.vivid";

bool IsSupportedFile(const std::string& path)
{
  return UTILS::EndsWith(path, ".mkv") || UTILS::EndsWith(path, ".MKV") ||
         UTILS::EndsWith(path, ".iso") || UTILS::EndsWith(path, ".ISO");
}
} // namespace

class CInputStreamVivid : public kodi::addon::CInstanceInputStream
{
public:
  explicit CInputStreamVivid(const kodi::addon::IInstanceInfo& instance)
    : CInstanceInputStream(instance)
  {
  }

  ~CInputStreamVivid() override { Close(); }

  // Kodi 询问插件能力。这里声明支持 seek，但不声明 IDEMUX——
  // 由 Kodi 内置 demuxer 处理容器，插件只负责提供字节流。
  void GetCapabilities(kodi::addon::InputstreamCapabilities& capabilities) override
  {
    capabilities.SetMask(INPUTSTREAM_SUPPORTS_SEEK);
  }

  bool Open(const kodi::addon::InputstreamProperty& props) override
  {
    Close();

    std::string path = props.GetURL();
    if (UTILS::StartsWith(path, "file://"))
      path = path.substr(7);

    if (!IsSupportedFile(path))
    {
      kodi::Log(ADDON_LOG_INFO, "[%s] unsupported container: %s", LOGTAG, path.c_str());
      return false;
    }

    struct stat st{};
    if (stat(path.c_str(), &st) != 0)
    {
      kodi::Log(ADDON_LOG_ERROR, "[%s] stat failed: %s", LOGTAG, path.c_str());
      return false;
    }

    m_fd = open(path.c_str(), O_RDONLY);
    if (m_fd < 0)
    {
      kodi::Log(ADDON_LOG_ERROR, "[%s] open failed: %s", LOGTAG, path.c_str());
      return false;
    }

    m_length = static_cast<int64_t>(st.st_size);
    m_position = 0;

    kodi::Log(ADDON_LOG_INFO, "[%s] opened %s (%lld bytes)", LOGTAG, path.c_str(),
              static_cast<long long>(m_length));
    return true;
  }

  void Close() override
  {
    if (m_fd >= 0)
    {
      close(m_fd);
      m_fd = -1;
    }
    m_position = 0;
    m_length = 0;
  }

  int ReadStream(uint8_t* buffer, unsigned int bufferSize) override
  {
    if (m_fd < 0 || buffer == nullptr || bufferSize == 0)
      return -1;

    ssize_t n = pread(m_fd, buffer, bufferSize, m_position);
    if (n < 0)
      return -1;
    if (n == 0)
      return 0;

    m_position += n;
    return static_cast<int>(n);
  }

  int64_t SeekStream(int64_t position, int whence = SEEK_SET) override
  {
    if (m_fd < 0)
      return -1;

    int64_t base = 0;
    switch (whence)
    {
      case SEEK_SET:
        base = 0;
        break;
      case SEEK_CUR:
        base = m_position;
        break;
      case SEEK_END:
        base = m_length;
        break;
      default:
        return -1;
    }

    int64_t target = base + position;
    target = std::max<int64_t>(0, std::min<int64_t>(target, m_length));
    m_position = target;
    return m_position;
  }

  int64_t PositionStream() override { return m_position; }

  int64_t LengthStream() override { return m_length; }

  bool IsRealTimeStream() override { return false; }

private:
  int m_fd = -1;
  int64_t m_length = 0;
  int64_t m_position = 0;
};

class CAddonVivid : public kodi::addon::CAddonBase
{
public:
  CAddonVivid() = default;

  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override
  {
    // 本插件只有 inputstream 一种实例类型，无需再判断 instance 类型。
    kodi::Log(ADDON_LOG_INFO, "[%s] creating inputstream instance", LOGTAG);
    hdl = new CInputStreamVivid(instance);
    return ADDON_STATUS_OK;
  }
};

ADDONCREATOR(CAddonVivid)
