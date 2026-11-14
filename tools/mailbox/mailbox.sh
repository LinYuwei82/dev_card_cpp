#!/usr/bin/env bash
# dev_card_cpp 双机会话信箱（macOS / POSIX 端）。
# 协议见信箱根目录的 PROTOCOL.md，本脚本不擅自扩展协议。
#
# 用法：
#   mailbox.sh send --subject <标题> --body <文件|-> [--re <对方文件名>]
#   mailbox.sh poll
#   mailbox.sh peek
#   mailbox.sh heartbeat
#   mailbox.sh status
#
# 环境变量：
#   DEVCARD_MAILBOX_DIR  信箱目录，默认 /Volumes/share/devcard-mailbox
#   DEVCARD_SIDE         本机身份 mac|win，默认按 uname 判断

set -u

MAILBOX_DIR="${DEVCARD_MAILBOX_DIR:-/Volumes/share/devcard-mailbox}"
INBOX="$MAILBOX_DIR/inbox"
ARCHIVE="$MAILBOX_DIR/archive"
PRESENCE="$MAILBOX_DIR/presence"
PEER_OFFLINE_SECONDS=$((30 * 60))
MAX_BODY_BYTES=8192

if [ -n "${DEVCARD_SIDE:-}" ]; then
  SIDE="$DEVCARD_SIDE"
else
  case "$(uname -s)" in
    Darwin) SIDE="mac" ;;
    *) SIDE="win" ;;
  esac
fi

case "$SIDE" in
  mac) PEER="win" ;;
  win) PEER="mac" ;;
  *) echo "DEVCARD_SIDE 只能是 mac 或 win（当前：${SIDE}）" >&2; exit 2 ;;
esac

# 心跳/时间统一用这个格式，Windows 侧 scripts 必须一致，否则解析不了。
now_utc() { date -u +%Y-%m-%dT%H:%M:%SZ; }
stamp_utc() { date -u +%Y%m%dT%H%M%SZ; }

fail() {
  echo "信箱不可用：$*" >&2
  exit 1
}

# 目录不在、或不可写，一律当「信箱不可用」——绝不退化成「没有新消息」。
require_mailbox() {
  [ -d "$MAILBOX_DIR" ] || fail "目录不存在 ${MAILBOX_DIR}（共享盘没挂上？）"
  [ -d "$INBOX" ] || fail "缺少 $INBOX"
  [ -d "$ARCHIVE" ] || fail "缺少 $ARCHIVE"
  [ -d "$PRESENCE" ] || fail "缺少 $PRESENCE"
  [ -w "$INBOX" ] || fail "$INBOX 不可写（挂载只读或权限不足）"
  [ -w "$ARCHIVE" ] || fail "$ARCHIVE 不可写（挂载只读或权限不足）"
}

to_epoch() {
  # 支持 macOS(BSD date) 与 GNU date 两种实现。
  if date -j -f "%Y-%m-%dT%H:%M:%SZ" "$1" +%s >/dev/null 2>&1; then
    date -j -f "%Y-%m-%dT%H:%M:%SZ" "$1" +%s
  else
    date -u -d "$1" +%s 2>/dev/null
  fi
}

write_heartbeat() {
  local tmp="$PRESENCE/.tmp-$SIDE.txt"
  if printf '%s\n' "$(now_utc)" >"$tmp" 2>/dev/null && mv -f "$tmp" "$PRESENCE/$SIDE.txt" 2>/dev/null; then
    return 0
  fi
  rm -f "$tmp" 2>/dev/null
  echo "警告：心跳写入失败（$PRESENCE/$SIDE.txt），不影响消息读写" >&2
  return 0
}

next_seq() {
  local max=0 n
  for f in "$INBOX" "$ARCHIVE"; do
    [ -d "$f" ] || continue
    for name in "$f"/*-"$SIDE"-*.md; do
      [ -e "$name" ] || continue
      n="$(basename "$name" | sed -n "s/.*-$SIDE-\([0-9][0-9][0-9]\)\.md$/\1/p")"
      [ -n "$n" ] || continue
      n=$((10#$n))
      [ "$n" -gt "$max" ] && max="$n"
    done
  done
  printf '%03d' $((max + 1))
}

# mv 失败重试一次；仍失败就报错并保留源文件，绝不丢内容。
move_file() {
  local src="$1" dst="$2"
  if mv "$src" "$dst" 2>/dev/null; then
    return 0
  fi
  sleep 1
  mv "$src" "$dst" 2>/dev/null && return 0
  echo "移动失败：${src} → ${dst}（源文件已保留）" >&2
  return 1
}

cmd_send() {
  local subject="" body="" re="-" seq name tmp
  while [ $# -gt 0 ]; do
    case "$1" in
      --subject) [ $# -ge 2 ] || { echo "--subject 缺少参数" >&2; exit 2; }; subject="$2"; shift 2 ;;
      --body) [ $# -ge 2 ] || { echo "--body 缺少参数" >&2; exit 2; }; body="$2"; shift 2 ;;
      --re) [ $# -ge 2 ] || { echo "--re 缺少参数" >&2; exit 2; }; re="$2"; shift 2 ;;
      *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
  done
  [ -n "$subject" ] || { echo "缺少 --subject" >&2; exit 2; }
  [ -n "$body" ] || { echo "缺少 --body（用 - 表示标准输入）" >&2; exit 2; }

  require_mailbox

  local body_file="$body"
  if [ "$body" = "-" ]; then
    body_file="$(mktemp)"
    cat >"$body_file"
  fi
  [ -f "$body_file" ] || { echo "正文文件不存在：$body_file" >&2; exit 2; }
  local size
  size="$(wc -c <"$body_file" | tr -d ' ')"
  if [ "$size" -gt "$MAX_BODY_BYTES" ]; then
    [ "$body" = "-" ] && rm -f "$body_file"
    echo "正文字节数 $size 超过上限 ${MAX_BODY_BYTES}（长内容给路径与 commit）" >&2
    exit 2
  fi

  seq="$(next_seq)"
  name="$(stamp_utc)-$SIDE-$seq.md"
  tmp="$INBOX/.tmp-$name"

  {
    printf 're: %s\n' "$re"
    printf 'from: %s\n' "$SIDE"
    printf 'subject: %s\n' "$subject"
    printf -- '---\n\n'
    cat "$body_file"
    # 正文末尾补一个换行，避免 cat 出来的内容粘住文件尾。
    printf '\n'
  } >"$tmp" || { [ "$body" = "-" ] && rm -f "$body_file"; fail "写入 $tmp 失败"; }
  [ "$body" = "-" ] && rm -f "$body_file"

  if ! move_file "$tmp" "$INBOX/$name"; then
    exit 1
  fi
  echo "已发送：$name"
}

cmd_poll() {
  require_mailbox
  write_heartbeat
  local count=0 name
  # 文件名排序即时间排序；.tmp- 是没写完的半截文件，跳过。
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    case "$name" in
      .tmp-*) continue ;;
      *-"$SIDE"-*) continue ;;
    esac
    count=$((count + 1))
    echo "===== $name ====="
    cat "$INBOX/$name"
    echo
    if ! move_file "$INBOX/$name" "$ARCHIVE/$name"; then
      echo "（归档失败，消息仍在 inbox，下次 poll 会再读到）" >&2
    fi
  done <<EOF
$(ls -1 "$INBOX" 2>/dev/null | sort)
EOF
  if [ "$count" -eq 0 ]; then
    echo "没有待读消息（inbox 为空，或只有自己发的）"
  else
    echo "共 $count 条，已归档到 archive/"
  fi
}

cmd_peek() {
  require_mailbox
  write_heartbeat
  local count=0 name
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    case "$name" in
      .tmp-*) continue ;;
      *-"$SIDE"-*) continue ;;
    esac
    count=$((count + 1))
    echo "$name"
    sed -n 's/^from: /  来自: /p; s/^subject: /  标题: /p' "$INBOX/$name" 2>/dev/null
  done <<EOF
$(ls -1 "$INBOX" 2>/dev/null | sort)
EOF
  echo "未读 $count 条（peek 不移动文件）"
}

cmd_status() {
  require_mailbox
  write_heartbeat
  local now self_ts peer_ts age_min unread name
  now="$(to_epoch "$(now_utc)")"

  echo "信箱：${MAILBOX_DIR}（可写）"
  echo "本机：$SIDE"

  if [ -f "$PRESENCE/$SIDE.txt" ]; then
    self_ts="$(tr -d '\r\n' <"$PRESENCE/$SIDE.txt")"
    echo "本机心跳：$self_ts"
  fi

  if [ -f "$PRESENCE/$PEER.txt" ]; then
    peer_ts="$(tr -d '\r\n' <"$PRESENCE/$PEER.txt")"
    local peer_epoch
    peer_epoch="$(to_epoch "$peer_ts")"
    if [ -n "$peer_epoch" ]; then
      age_min=$(((now - peer_epoch) / 60))
      if [ $((now - peer_epoch)) -gt "$PEER_OFFLINE_SECONDS" ]; then
        echo "对方（${PEER}）离线：最后一次心跳 ${peer_ts}，已 ${age_min} 分钟未更新"
      else
        echo "对方（${PEER}）在线：最后一次心跳 ${peer_ts}（${age_min} 分钟前）"
      fi
    else
      echo "对方（${PEER}）心跳无法解析：${peer_ts}"
    fi
  else
    echo "对方（${PEER}）离线：没有心跳文件 $PRESENCE/$PEER.txt"
  fi

  unread=0
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    case "$name" in
      .tmp-*|*-"$SIDE"-*) continue ;;
    esac
    unread=$((unread + 1))
  done <<EOF
$(ls -1 "$INBOX" 2>/dev/null)
EOF
  echo "未读消息：$unread 条"
}

usage() {
  cat <<'EOF'
用法：
  mailbox.sh send --subject <标题> --body <文件|-> [--re <对方文件名>]
  mailbox.sh poll      读取并归档未读消息
  mailbox.sh peek      只列出未读，不移动
  mailbox.sh heartbeat 刷新 presence/<side>.txt
  mailbox.sh status    信箱可用性、双方心跳、未读条数
EOF
}

cmd="${1:-}"
[ $# -gt 0 ] && shift
case "$cmd" in
  send) cmd_send "$@" ;;
  poll) cmd_poll ;;
  peek) cmd_peek ;;
  heartbeat) require_mailbox; write_heartbeat; echo "心跳已更新：$PRESENCE/$SIDE.txt" ;;
  status) cmd_status ;;
  ""|-h|--help|help) usage ;;
  *) echo "未知命令：$cmd" >&2; usage >&2; exit 2 ;;
esac
