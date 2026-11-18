// inputstream.vivid — 插件内自诊断
//
// 目的：插件一被 Kodi 加载，就把「这台机器到底能不能解 Amlogic 硬解、
//       libamcodec 里到底有没有我们 dlsym 的那套符号」写成报告，
//       用户只需把报告发出来，不需要 SSH 跑脚本。
//
// 设计约束：
//   - 只依赖 POSIX + Kodi 的 kodi::Log，避免引入额外链接依赖
//   - 全部包在 try/catch 里，诊断失败绝不影响插件本身
//   - RunOnce() 幂等，一次 Kodi 会话只产出一次硬件环境报告
//   - 报告同时写文件 + 在 kodi.log 里打摘要，两条路都能拿到结论

#pragma once

namespace DIAG
{
// 执行一次环境诊断：探测 libamcodec、真实符号表、/proc/self/maps、sysfs HDR 能力，
// 写完报告后在 kodi.log 打一段摘要。重复调用只有第一次生效。
// stage 用于标记触发点（CreateInstance / Open），写进报告便于定位。
void RunOnce(const char* stage);

// 追加一条流水记录（例如 Open 的文件、大小）。会写进报告，不打 kodi.log。
void Note(const char* fmt, ...);

// 返回报告文件路径（可能是空串，表示没找到可写目录）。
const char* ReportPath();
} // namespace DIAG
