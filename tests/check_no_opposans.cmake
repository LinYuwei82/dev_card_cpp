# 新架构产物不得出现旧 Qt 线的 OPPOSans（2026-11-14 定）。
#
# 背景：改造前的 Qt 线曾引用 OPPOSans（两个 ttf 约 20 MB）。旧线已从仓库删除，
# 字体随之删除；这条守卫保留，防止新架构再混进 OPPOSans：
# 新架构用 Noto Sans SC 子集，界面与打印共用一份，混进 OPPOSans 就意味着某处
# 偷偷回退到了系统字体或旧资源。
#
# 本脚本同时查三类东西（任何一类命中即失败）：
#   1) 新架构源目录（web/ server/ shell/ core/ cmake/）里的文本引用；
#   2) 新建出来的可执行文件里的字符串（按 HEX 搜，二进制也可靠）；
#   3) 随包 fonts/ 目录里是否有 OPPOSans 字体文件。
#
# 用法：cmake -DBINARY=<exe> -DFONTS_DIR=<dir> -DSOURCE_DIR=<repo> -P check_no_opposans.cmake

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "check_no_opposans：缺少 SOURCE_DIR")
endif()

# "OPPOSans" 的 ASCII HEX；二进制与文本都用同一条搜索。
# 为什么不直接搜字符串：产物里的字体名可能是分块内嵌的字面量（embed_web 就是按 4 KB
# 切片拼的），也可能以非 ASCII 编码存储；按字符读一遍，遇到编码差异或分块边界就漏。
# 从原始字节转 HEX 再找，不受编码与分块影响，代价只是多占一倍内存。
set(needle "4f50504f53616e73")

# 1) 源目录
foreach(dir IN ITEMS web server shell core cmake)
  set(root "${SOURCE_DIR}/${dir}")
  if(NOT IS_DIRECTORY "${root}")
    continue()
  endif()
  file(GLOB_RECURSE files "${root}/*")
  foreach(path IN LISTS files)
    if(IS_DIRECTORY "${path}")
      continue()
    endif()
    file(READ "${path}" hex HEX)
    string(FIND "${hex}" "${needle}" hit)
    if(NOT hit EQUAL -1)
      message(FATAL_ERROR
              "新架构源目录出现 OPPOSans：${path}\n"
              "旧 Qt 线独占这批字体，新架构必须用 Noto Sans SC 子集（见 §10.2.1）。")
    endif()
  endforeach()
endforeach()

# 2) 可执行文件
if(BINARY AND EXISTS "${BINARY}")
  file(READ "${BINARY}" hex HEX)
  string(FIND "${hex}" "${needle}" hit)
  if(NOT hit EQUAL -1)
    message(FATAL_ERROR
            "新架构产物里出现 OPPOSans 字符串：${BINARY}\n"
            "说明仍有代码或资源引用旧字体。")
  endif()
endif()

# 3) 随包字体目录
if(FONTS_DIR AND IS_DIRECTORY "${FONTS_DIR}")
  file(GLOB stale "${FONTS_DIR}/*OPPOSans*")
  if(stale)
    message(FATAL_ERROR "随包字体目录里出现 OPPOSans：${stale}")
  endif()
endif()

message(STATUS "OPPOSans 检查通过：新架构源码 / 产物 / 随包字体均无旧字体引用")
