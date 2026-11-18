// inputstream.vivid — Kodi 22 (Piers) InputStream 插件
//
// 说明：
//   本文件为「架构适配验证版」，目标是让插件在 CoreELEC 22.0-Piers
//   (Amlogic-no / aarch64 / Kodi 22) 上能被正确加载、Open/Close/Seek 不崩溃。
//
//   当前实现为「字节流透传」：把 MKV/M2TS 的原始数据交给 Kodi，
//   由 Kodi 自身的 demuxer + amcodec 播放器完成解封装与硬件解码。
//   并未自行解码，也不涉及 aml_* 私有接口。
//
//   读写统一走 Kodi 的 VFS（kodi::vfs::CFile），因此本地 / SMB / NFS / HTTP
//   都能透传。这一点很重要：addon.xml 里用 extension="mkv|m2ts" 声明接管
//   这些容器后，Kodi 会把**所有**这类文件都交给本插件；若只支持本地路径，
//   网络播放会直接失败。
//
//   真正的 HDR Vivid 硬解需要在此基础上接入 CoreELEC/no 上真实可用的解码 API
//   （见 README 的「后续工作」），不属于本验证版范围。

#include <kodi/addon-instance/Inputstream.h>
#include <kodi/Filesystem.h>

#include <sys/types.h>

#include <cstdint>
#include <algorithm>
#include <string>

#include "Utils.h"
#include "Diag.h"

namespace
{
constexpr const char* LOGTAG = "inputstream.vivid";

// 与 addon.xml 的 extension 列表保持一致，仅用于日志提示。
bool IsDeclaredContainer(const std::string& path)
{
  return UTILS::EndsWith(path, ".mkv") || UTILS::EndsWith(path, ".MKV") ||
         UTILS::EndsWith(path, ".m2ts") || UTILS::EndsWith(path, ".M2TS");
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

    // 能走到这里说明 Kodi 真的把这个文件交给插件了，记进诊断报告
    DIAG::Note("Open: %s", path.c_str());

    if (!IsDeclaredContainer(path))
      kodi::Log(ADDON_LOG_INFO, "[%s] 非声明容器，仍按字节流尝试: %s", LOGTAG, path.c_str());

    // 走 Kodi VFS：本地路径、smb://、nfs://、http(s):// 都能读。
    // 不能用 POSIX open()：那样网络路径被接管后会直接播放失败。
    if (!m_file.OpenFile(path))
    {
      kodi::Log(ADDON_LOG_ERROR, "[%s] VFS OpenFile 失败: %s", LOGTAG, path.c_str());
      DIAG::Note("Open 失败：VFS 打不开 %s", path.c_str());
      return false;
    }

    m_length = m_file.GetLength();
    if (m_length <= 0)
    {
      kodi::Log(ADDON_LOG_WARNING, "[%s] 长度未知（可能是流式源）: %s", LOGTAG, path.c_str());
      DIAG::Note("Open：长度未知 %s", path.c_str());
    }

    kodi::Log(ADDON_LOG_INFO, "[%s] opened %s (%lld bytes)", LOGTAG, path.c_str(),
              static_cast<long long>(m_length));
    DIAG::Note("Open 成功：%lld 字节", static_cast<long long>(m_length));
    return true;
  }

  void Close() override
  {
    if (m_file.IsOpen())
      m_file.Close();
    m_length = 0;
  }

  int ReadStream(uint8_t* buffer, unsigned int bufferSize) override
  {
    if (!m_file.IsOpen() || buffer == nullptr || bufferSize == 0)
      return -1;

    ssize_t n = m_file.Read(buffer, bufferSize);
    if (n < 0)
      return -1;
    if (n == 0)
      return 0;
    return static_cast<int>(n);
  }

  int64_t SeekStream(int64_t position, int whence = SEEK_SET) override
  {
    if (!m_file.IsOpen())
      return -1;

    // CFile::Seek 直接支持 SEEK_SET / SEEK_CUR / SEEK_END；
    // 长度未知时不要自己算并钳位，交给底层处理。
    int64_t result = m_file.Seek(position, whence);
    if (result < 0)
      return -1;
    return result;
  }

  int64_t PositionStream() override
  {
    return m_file.IsOpen() ? m_file.GetPosition() : 0;
  }

  int64_t LengthStream() override { return m_length; }

  bool IsRealTimeStream() override { return false; }

private:
  kodi::vfs::CFile m_file;
  int64_t m_length = 0;
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

    // 一被加载就做一次环境自诊断，报告写到 /storage 下，用户不用 SSH。
    // 能走到这行，本身就已经证明：.so 架构正确、Kodi 22 愿意加载它。
    DIAG::RunOnce("CreateInstance");
    kodi::Log(ADDON_LOG_INFO, "[%s] 诊断报告: %s", LOGTAG, DIAG::ReportPath());

    hdl = new CInputStreamVivid(instance);
    return ADDON_STATUS_OK;
  }
};

ADDONCREATOR(CAddonVivid)
